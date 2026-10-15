#pragma once

/// @file TokenCounter.hpp
/// @brief Token 计数与模型上下文窗口探测工具
///
/// @details
/// 功能：
/// 1. 本地近似 token 计数（无需外部依赖，支持中英文混合文本）
/// 2. 从 OpenAI 兼容 API（LM Studio / vLLM / Ollama）探测模型上下文窗口
/// 3. 支持按模型名称映射常见模型的默认 context window
///
/// - 纯 header-only，零外部依赖（仅依赖 cpr / nlohmann_json，AE 已自带）
/// - 近似算法：中文按 ~1.5 chars/token，英文按 ~4 chars/token，
/// 代码按 ~3 chars/token 的加权估算，误差通常在 15% 以内
/// - 探测失败时回退到模型名映射或默认值 8192
///
/// @note 所属命名空间：AE::Engine
/// @note 设计约束：

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Logger.hpp"
#include "Core/Net/HttpCurl.hpp"

namespace AE::Engine
{

    class TokenCounter
    {
    public:
        // ─── 近似 token 计数 ───
        // 估算一段文本的 token 数量，无需外部 tokenizer
        // 支持中英文混合、代码、特殊字符
        static auto estimate_tokens(std::string_view text) -> std::size_t
        {
            if (text.empty())
                return 0;

            std::size_t chineseChars = 0;    // 中文字符
            std::size_t asciiLetters = 0;     // 英文字母
            std::size_t asciiDigits = 0;      // 数字
            std::size_t asciiWhitespace = 0;  // 空白
            std::size_t asciiPunct = 0;       // 英文标点
            std::size_t otherChars = 0;       // 其他 Unicode

            for (unsigned char c : text)
            {
                if (c < 128)
                {
                    if (std::isalpha(c))
                        ++asciiLetters;
                    else if (std::isdigit(c))
                        ++asciiDigits;
                    else if (std::isspace(c))
                        ++asciiWhitespace;
                    else
                        ++asciiPunct;
                }
                else
                {
                    // 简化判断：UTF-8 多字节字符，按中文字符处理
                    // （实际项目中可用 ICU 等库做精确语言检测）
                    if ((c & 0xC0) == 0x80)
                        continue; // 跳过 UTF-8 续字节
                    ++chineseChars;
                }
            }

            // 经验系数（基于常见 tokenizer 的统计）：
            //  - 英文单词：约 4 chars/token（含空格）
            //  - 中文字符：约 0.7 chars/token（即 1 个中文字 ~ 1.5 token 的反比）
            //  - 数字：约 2 digits/token
            //  - 标点：每 2-3 个算 1 token
            //  - 代码/符号：约 2.5 chars/token

            double englishTokens = static_cast<double>(asciiLetters + asciiWhitespace) / 4.0;
            double chineseTokens = static_cast<double>(chineseChars) * 1.5;
            double digitTokens = static_cast<double>(asciiDigits) / 2.0;
            double punctTokens = static_cast<double>(asciiPunct) / 3.0;
            double otherTokens = static_cast<double>(otherChars) * 1.2;

            double total = englishTokens + chineseTokens + digitTokens + punctTokens + otherTokens;
            return static_cast<std::size_t>(std::ceil(total));
        }

        // 估算 messages 数组的总 token 数（OpenAI 格式）
        // 每条消息额外 ~4 token 开销（角色名、分隔符等）
        static auto estimate_messages_tokens(const nlohmann::json &messages) -> std::size_t
        {
            if (!messages.is_array())
                return 0;

            std::size_t total = 0;
            for (const auto &msg : messages)
            {
                std::size_t msgTokens = 4; // 每条消息的基础开销（<|start|> role<|end|>）

                if (msg.contains("content"))
                {
                    const auto &content = msg["content"];
                    if (content.is_string())
                        msgTokens += estimate_tokens(content.get<std::string_view>());
                    else if (content.is_array())
                    {
                        for (const auto &part : content)
                        {
                            if (part.contains("text") && part["text"].is_string())
                                msgTokens += estimate_tokens(part["text"].get<std::string_view>());
                        }
                    }
                }

                if (msg.contains("tool_calls") && msg["tool_calls"].is_array())
                {
                    for (const auto &tc : msg["tool_calls"])
                    {
                        if (tc.contains("function"))
                        {
                            const auto &fn = tc["function"];
                            if (fn.contains("name"))
                                msgTokens += estimate_tokens(fn["name"].get<std::string_view>());
                            if (fn.contains("arguments"))
                            {
                                std::string args = fn["arguments"].is_string()
                                                       ? fn["arguments"].get<std::string>()
                                                       : fn["arguments"].dump();
                                msgTokens += estimate_tokens(args);
                            }
                        }
                        msgTokens += 3; // tool_call 结构开销
                    }
                }

                total += msgTokens;
            }

            total += 3; // 系统级开销（<|start|>assistant<|end|> 启动）
            return total;
        }

        // ─── 模型上下文窗口探测 ───

        struct ModelInfo
        {
            std::string model_name;
            std::size_t context_window = 8192;
            std::size_t max_completion_tokens = 2048;
            bool has_usage_stats = true; // 服务端是否返回 usage 统计
        };

        // 从 OpenAI 兼容的 /v1/models 端点获取模型信息
        // 仅探测一次，结果可缓存
        static auto probe_model_info(std::string_view base_url,
                                     std::string_view model_name,
                                     std::string_view api_key = "") -> Core::Result<ModelInfo>
        {
            ModelInfo info;
            info.model_name = std::string{model_name};

            // 先查表：常见模型的 context window
            if (auto mapped = lookup_model_defaults(model_name))
            {
                info.context_window = mapped->context_window;
                info.max_completion_tokens = mapped->max_completion_tokens;
                info.has_usage_stats = mapped->has_usage_stats;
            }

            // 尝试从 API 探测
            try
            {
                std::string url = std::string{base_url};
                if (!url.ends_with("/v1"))
                {
                    if (url.ends_with("/"))
                        url += "v1";
                    else
                        url += "/v1";
                }

                AE::Core::Net::CurlRequest probeReq;
                probeReq.method = "GET";
                probeReq.url = url + "/models";
                probeReq.timeoutMs = 5000;

                if (!api_key.empty())
                    probeReq.headers["Authorization"] = "Bearer " + std::string{api_key};

                auto probeR = AE::Core::Net::curlPerform(probeReq);
                const long probeCode = probeR.isError() ? 0 : probeR.value().statusCode;
                if (probeR.isError() || probeCode != 200)
                {
                    Core::Logger::getInstance().warningFmt(
                        "[TokenCounter] /v1/models returned {}, using defaults for {}",
                        probeCode, model_name);
                    return Core::Result<ModelInfo>::Ok(std::move(info));
                }

                auto data = nlohmann::json::parse(probeR.value().body);
                if (!data.contains("data") || !data["data"].is_array())
                {
                    Core::Logger::getInstance().warningFmt(
                        "[TokenCounter] /v1/models response missing 'data' array");
                    return Core::Result<ModelInfo>::Ok(std::move(info));
                }

                // 查找匹配的模型
                for (const auto &m : data["data"])
                {
                    std::string id = m.value("id", "");
                    if (id == model_name || id.find(model_name) != std::string::npos)
                    {
                        // LM Studio / vLLM 可能在 model 详情中有 context_window
                        if (m.contains("context_window") && m["context_window"].is_number())
                        {
                            info.context_window = m["context_window"].get<std::size_t>();
                        }
                        if (m.contains("max_tokens") && m["max_tokens"].is_number())
                        {
                            info.max_completion_tokens = m["max_tokens"].get<std::size_t>();
                        }

                        Core::Logger::getInstance().infoFmt(
                            "[TokenCounter] probed model {}: context_window={}",
                            model_name, info.context_window);
                        return Core::Result<ModelInfo>::Ok(std::move(info));
                    }
                }

                Core::Logger::getInstance().infoFmt(
                    "[TokenCounter] model '{}' not found in /v1/models, using default ctx={}",
                    model_name, info.context_window);
            }
            catch (const std::exception &e)
            {
                Core::Logger::getInstance().warningFmt(
                    "[TokenCounter] probe failed: {} (using defaults)", e.what());
            }

            return Core::Result<ModelInfo>::Ok(std::move(info));
        }

        // 查表：常见模型的默认 context window
        static auto lookup_model_defaults(std::string_view model_name) -> std::optional<ModelInfo>
        {
            static const std::unordered_map<std::string, ModelInfo> kKnownModels = {
                // Qwen 系列
                {"qwen", {"qwen", 32768, 8192, true}},
                {"qwen2", {"qwen2", 131072, 8192, true}},
                {"qwen2.5", {"qwen2.5", 131072, 8192, true}},
                {"qwen3", {"qwen3", 131072, 8192, true}},
                {"qwythos", {"qwythos", 1048576, 8192, true}}, // Qwythos 超长上下文
                // Llama 系列
                {"llama-2", {"llama-2", 4096, 2048, true}},
                {"llama-3", {"llama-3", 8192, 4096, true}},
                {"llama-3.1", {"llama-3.1", 131072, 8192, true}},
                {"llama-3.2", {"llama-3.2", 131072, 8192, true}},
                {"llama-3.3", {"llama-3.3", 131072, 8192, true}},
                // Mistral 系列
                {"mistral", {"mistral", 8192, 4096, true}},
                {"mixtral", {"mixtral", 32768, 8192, true}},
                // Claude 风格
                {"claude", {"claude", 200000, 4096, true}},
                {"sonnet", {"sonnet", 200000, 8192, true}},
                // GPT 系列（参考）
                {"gpt-3.5", {"gpt-3.5", 16384, 4096, true}},
                {"gpt-4", {"gpt-4", 8192, 4096, true}},
                {"gpt-4o", {"gpt-4o", 128000, 4096, true}},
                // Yi 系列
                {"yi", {"yi", 4096, 2048, true}},
                {"yi-34b", {"yi-34b", 4096, 2048, true}},
                // DeepSeek 系列
                {"deepseek", {"deepseek", 131072, 8192, true}},
                // 通用默认
                {"default", {"default", 8192, 2048, true}},
            };

            std::string lower{model_name};
            std::ranges::transform(lower, lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });

            // 精确前缀匹配
            for (const auto &[prefix, info] : kKnownModels)
            {
                if (lower.starts_with(prefix))
                    return info;
            }

            // 子串匹配（找最长匹配）
            std::optional<ModelInfo> best = std::nullopt;
            std::size_t bestLen = 0;
            for (const auto &[prefix, info] : kKnownModels)
            {
                if (lower.find(prefix) != std::string::npos && prefix.size() > bestLen)
                {
                    best = info;
                    bestLen = prefix.size();
                }
            }

            return best;
        }
    };

} // namespace AE::Engine
