#pragma once

/// @file StreamingLLMClient.hpp
/// @brief 流式 LLM 客户端（libcurl SSE + nlohmann 解析）。
///
/// @details
/// 设计约束（关键）：
/// - 不 include Butler / MemoryStick；LlmDelta（见 AEType.hpp）为统一 IR。
/// - TLS 校验默认开启，API Key 用 VMX::Core::Security::SecureString（不在内存裸存）。
///
/// @note 所属命名空间：VMX::AE
/// @note 依赖：libcurl, nlohmann, Core::Result/Logger。

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <generator>
#include <mutex>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "Core/Security/SecureString.hpp"
#include "Core/Net/SseParser.hpp"
#include "Core/Net/HttpCurl.hpp"

#include "Core/Result.hpp"
#include "Core/Logger.hpp"
#include "AEType.hpp"

namespace VMX::AE
{

    // ---------------------------------------------------- 流式 LLM 客户端
    // 设计：libcurl 驱动 SSE（WriteCallback -> VMX::Core::Net::SseParser 标准解析），
    //       每个 SSE 事件的 data 用 nlohmann::json::parse 解析；
    //       tool_call 的 arguments 跨 chunk 残缺，按 index 累加，流结束后整体组装。
    class StreamingLLMClient
    {
    public:
        StreamingLLMClient(std::string endpoint,
                           VMX::Core::Security::SecureString api_key,
                           int timeout_ms = 30000,
                           int max_retries = 3)
            : m_endpoint(std::move(endpoint)),
              m_api_key(std::move(api_key)),
              m_timeout_ms(timeout_ms),
              m_max_retries(max_retries)
        {
        }

        // 阻塞收齐整轮（保留给需要完整结果的调用方 / 兜底），返回有序增量。
        auto stream_chat(const nlohmann::json &request_body)
            -> Core::Result<std::vector<LlmDelta>>
        {
            std::vector<LlmDelta> collected;
            auto r = collect_all(request_body, [&](LlmDelta d) { collected.push_back(std::move(d)); });
            if (r.isError())
                return Core::Result<std::vector<LlmDelta>>::Error(r.error());
            return Core::Result<std::vector<LlmDelta>>::Ok(std::move(collected));
        }

        // ─── 真·增量流式 ───
        // HTTP 在背景线程进行，逐 delta 经线程安全队列实时上发；生成器在消费线程逐步 co_yield，
        // 实现"边生成边显示"，彻底解决"等后端生成完才整段弹出"。传输错误在队列结束后抛出，
        // 由上层（AE::run / ButlerEngine）收敛为错误事件。
        auto stream_chat_gen(const nlohmann::json &request_body) -> std::generator<LlmDelta>
        {
            Core::Logger::getInstance().info(
                "[AE:LLM] stream_chat_gen: incremental generator path active");
            struct Queue
            {
                std::mutex mtx;
                std::condition_variable cv;
                std::deque<LlmDelta> q;
                bool finished = false;
                std::string err;
                auto push(LlmDelta d) -> void
                {
                    {
                        std::scoped_lock lock(mtx);
                        q.push_back(std::move(d));
                    }
                    cv.notify_one();
                }
                auto mark_done(std::string e = {}) -> void
                {
                    {
                        std::scoped_lock lock(mtx);
                        finished = true;
                        err = std::move(e);
                    }
                    cv.notify_all();
                }
                auto pop() -> std::optional<LlmDelta>
                {
                    std::unique_lock lock(mtx);
                    cv.wait(lock, [this] { return finished || !q.empty(); });
                    if (!q.empty())
                    {
                        auto d = std::move(q.front());
                        q.pop_front();
                        return d;
                    }
                    return std::nullopt;
                }
            };
            auto queue = std::make_shared<Queue>();
            std::jthread worker([this, request_body, queue]
                                {
                                    try
                                    {
                                        auto r = collect_all(
                                            request_body,
                                            [queue](LlmDelta d) { queue->push(std::move(d)); });
                                        if (r.isError())
                                            queue->mark_done(r.error().message());
                                        else
                                            queue->mark_done();
                                    }
                                    catch (const std::exception &e)
                                    {
                                        queue->mark_done(std::string{e.what()});
                                    }
                                    catch (...)
                                    {
                                        queue->mark_done("unknown streaming error");
                                    }
                                });
            while (auto d = queue->pop())
            {
                if (d->kind == LlmDelta::Kind::Done)
                    co_return;
                co_yield std::move(*d);
            }
            // 队列已结束且无 Done（传输错误）：以 Error delta 上发，替代 throw（H15 合规）。
            std::string err;
            {
                std::scoped_lock lock(queue->mtx);
                err = queue->err;
            }
            if (!err.empty())
                co_yield LlmDelta::error(std::move(err));
        }

        // ─── 配置访问器 ───
        auto endpoint() const -> std::string_view { return m_endpoint; }

        // 从 endpoint 推导 base URL（去掉 /v1/chat/completions 等路径）
        // 返回格式: http://host:port  （不含 /v1）
        auto base_url() const -> std::string
        {
            std::string url{m_endpoint};
            // 去掉 /chat/completions
            auto pos = url.find("/chat/completions");
            if (pos != std::string::npos)
                url = url.substr(0, pos);
            // 去掉末尾的 /v1
            if (url.ends_with("/v1"))
                url = url.substr(0, url.size() - 3);
            else if (url.ends_with("/v1/"))
                url = url.substr(0, url.size() - 4);
            return url;
        }

        auto api_key() const -> std::string
        {
            return std::string{m_api_key};
        }

        auto timeout_ms() const -> int { return m_timeout_ms; }
        auto max_retries() const -> int { return m_max_retries; }

        auto set_timeout_ms(int ms) -> void { m_timeout_ms = ms; }
        auto set_max_retries(int n) -> void { m_max_retries = n; }

    private:
        struct TurnOutcome
        {
            std::size_t produced = 0; // 非 Done delta 计数（用于空响应兜底判断）
            long status_code = 0;
            bool network_error = false;
            std::string error;
        };

        // 单次流式请求：边解析边经 onDelta 推送（不再内部累积 vector）。
        // 注意：collect_all 的定义见下方类体内（成员函数在类内任意位置可见，可被 stream_chat/stream_chat_gen 调用）。
        auto do_request_cb(const nlohmann::json &body, std::function<void(LlmDelta)> onDelta) -> TurnOutcome
        {
            TurnOutcome out;
            VMX::Core::Net::SseParser sse;

            // 按 tool_call index 累加残缺 arguments / id / name
            std::unordered_map<std::size_t, std::string> arg_buffers;
            std::unordered_map<std::size_t, std::string> tool_ids;
            std::unordered_map<std::size_t, std::string> tool_names;

            // 增量推送：边解析边经 onDelta 上发；同时本地累加文本、思考内容与工具调用标志，
            // 供下方文本工具调用兜底使用（不再依赖 out.deltas 扫描）。
            // 某些本地模型（如 qwen3.8-9b-heretic）在 enable_thinking=false 时仍把工具调用 JSON
            // 放在 reasoning_content 中，因此 reasoning 也需参与兜底解析。
            std::string acc_text;
            std::string acc_reasoning;
            bool has_toolcall = false;
            auto push_delta = [&](LlmDelta d)
            {
                if (d.kind == LlmDelta::Kind::Text && !d.text_content.empty())
                    acc_text += d.text_content;
                else if (d.kind == LlmDelta::Kind::Thinking && !d.thinking_content.empty())
                    acc_reasoning += d.thinking_content;
                else if (d.kind == LlmDelta::Kind::ToolCall)
                    has_toolcall = true;
                if (d.kind != LlmDelta::Kind::Done)
                    ++out.produced;
                onDelta(std::move(d));
            };

            auto parse_chunk = [&](std::string_view data) -> void
            {
                Core::Logger::getInstance().debugFmt(
                    "[AE:LLM] parse_chunk: raw_data={}",
                    data);

                auto j = nlohmann::json::parse(data, nullptr, false);
                if (j.is_discarded() || !j.is_object())
                {
                    return; // 非 JSON（如 ": ping"）直接跳过
                }

                if (!j.contains("choices") || !j["choices"].is_array())
                {
                    Core::Logger::getInstance().debug(
                        "[AE:LLM] parse_chunk: no 'choices' field");
                    return;
                }

                int choice_idx = 0;
                for (const auto &c : j["choices"])
                {
                    if (!c.is_object())
                        continue;

                    // 尝试 delta（流式标准），失败则尝试 message（非流式格式）
                    const nlohmann::json *delta = nullptr;
                    bool has_delta = false;
                    if (c.contains("delta") && c["delta"].is_object())
                    {
                        delta = &c["delta"];
                        has_delta = true;
                    }
                    else if (c.contains("message") && c["message"].is_object())
                    {
                        delta = &c["message"];
                        has_delta = true;
                    }
                    if (!has_delta)
                    {
                        Core::Logger::getInstance().debugFmt(
                            "[AE:LLM] parse_chunk: choice[{}] has neither 'delta' nor 'message'",
                            choice_idx);
                        continue;
                    }

                    bool has_content = delta->contains("content") && delta->at("content").is_string();
                    std::string content = has_content ? delta->at("content").get<std::string>() : "";

                    // Phase G: 检查 reasoning_content（思考内容）
                    bool has_reasoning = delta->contains("reasoning_content") && delta->at("reasoning_content").is_string();
                    std::string reasoning = has_reasoning ? delta->at("reasoning_content").get<std::string>() : "";

                    // 检查 tool_calls
                    bool has_tool_calls = delta->contains("tool_calls") && delta->at("tool_calls").is_array();

                    // nlohmann 为完整 DOM，随机访问无 simdjson on-demand 的前向消费顺序限制，
                    // 可先遍历 tool_calls 再读取 finish_reason（彻底消除原 on-demand 的踩坑点）。
                    const std::size_t content_len = (has_content ? content.size() : 0);
                    const std::size_t reasoning_len = (has_reasoning ? reasoning.size() : 0);

                    if (has_content && !content.empty())
                        push_delta(LlmDelta::text(content));

                    if (has_reasoning && !reasoning.empty())
                        push_delta(LlmDelta::thinking(reasoning));

                    if (has_tool_calls)
                    {
                        for (const auto &t : delta->at("tool_calls"))
                        {
                            if (!t.is_object())
                                continue;
                            std::size_t index = 0;
                            if (t.contains("index") && t["index"].is_number())
                                index = static_cast<std::size_t>(t["index"].get<std::int64_t>());

                            if (t.contains("id") && t["id"].is_string())
                            {
                                std::string id = t["id"].get<std::string>();
                                if (!id.empty())
                                    tool_ids[index] = std::move(id);
                            }

                            if (t.contains("function") && t["function"].is_object())
                            {
                                const auto &func = t["function"];
                                std::string name;
                                bool has_name = false;
                                if (func.contains("name") && func["name"].is_string())
                                {
                                    name = func["name"].get<std::string>();
                                    has_name = !name.empty();
                                }
                                std::string frag;
                                const bool has_args = func.contains("arguments") && func["arguments"].is_string();
                                if (has_args)
                                {
                                    frag = func["arguments"].get<std::string>();
                                    arg_buffers[index] += frag;
                                }
                                const auto buf_len_it = arg_buffers.find(index);
                                const std::size_t buf_len =
                                    (buf_len_it != arg_buffers.end()) ? buf_len_it->second.size() : 0;
                                Core::Logger::getInstance().debugFmt(
                                    "[AE:LLM] tool fragment idx={} has_id={} has_name={} has_args={} frag_len={} buf_len={}",
                                    index, tool_ids.contains(index), has_name, has_args,
                                    has_args ? frag.size() : 0, buf_len);
                            }
                        }
                    }

                    std::string finish_reason;
                    if (c.contains("finish_reason") && c["finish_reason"].is_string())
                        finish_reason = c["finish_reason"].get<std::string>();
                    Core::Logger::getInstance().debugFmt(
                        "[AE:LLM] parse_chunk: choice[{}] has_delta={} has_content={} content_len={} reasoning_len={} has_tool_calls={} finish_reason={}",
                        choice_idx, has_delta, has_content, content_len, reasoning_len, has_tool_calls,
                        finish_reason);

                    choice_idx++;
                }

                // usage 计数（优先用于精确 token 统计）
                if (j.contains("usage") && j["usage"].is_object())
                {
                    const auto &usage = j["usage"];
                    if (usage.contains("prompt_tokens") && usage["prompt_tokens"].is_number() &&
                        usage.contains("completion_tokens") && usage["completion_tokens"].is_number())
                    {
                        push_delta(LlmDelta::usage(
                            static_cast<std::size_t>(usage["prompt_tokens"].get<std::int64_t>()),
                            static_cast<std::size_t>(usage["completion_tokens"].get<std::int64_t>())));
                    }
                }
            };

            std::string streamBody;
            VMX::Core::Net::CurlRequest sseReq;
            sseReq.method = "POST";
            sseReq.url = m_endpoint;
            sseReq.body = body.dump();
            sseReq.timeoutMs = m_timeout_ms;
            sseReq.headers["Accept"] = "text/event-stream";
            sseReq.headers["Content-Type"] = "application/json";
            sseReq.headers["Authorization"] = "Bearer " + std::string{m_api_key}; // 仅在调用边界由 SecureString 构造

            Core::Logger::getInstance().debugFmt(
                "[AE:LLM] POST {} body={}",
                m_endpoint, sseReq.body.substr(0, 500));
            // TLS 校验默认开启（verifySsl=true）；如需固定 CA 见 Core/Net/HttpCurl.hpp。

            auto sseResult = VMX::Core::Net::curlStream(
                sseReq,
                [&sse, &streamBody, &parse_chunk, this](std::string_view data) -> bool
                {
                    streamBody.append(data);
                    Core::Logger::getInstance().debugFmt(
                        "[AE:LLM] SSE raw chunk: len={} preview={}",
                        data.size(),
                        data.substr(0, 300));
                    return sse.feed(data, [&](std::string_view event, std::string_view data) -> bool
                                    {
                        Core::Logger::getInstance().debugFmt(
                            "[AE:LLM] SSE parsed event: event={} data_len={} data_preview={}",
                            event, data.size(),
                            data.substr(0, 200));
                        if (data == "[DONE]")
                            return true;
                        parse_chunk(data);
                        return true; });
                });

            if (sseResult.isError())
            {
                if (out.produced > 0)
                {
                    Core::Logger::getInstance().warningFmt(
                        "[AE:LLM] stream delivered content but transport tail error; treating as complete");
                    out.status_code = 200;
                    out.network_error = false;
                    out.error.clear();
                }
                else
                {
                    Core::Logger::getInstance().errorFmt(
                        "[AE:LLM] transport failed: url={} msg={}",
                        m_endpoint, sseResult.error().message());
                    out.error = "LLM transport error";
                    return out;
                }
            }
            else
            {
                const long status = static_cast<long>(sseResult.value().statusCode);
                out.status_code = status;
                out.network_error = false;
                if (status < 200 || status >= 300)
                {
                    if (out.produced > 0)
                    {
                        Core::Logger::getInstance().warningFmt(
                            "[AE:LLM] stream delivered content but transport tail error "
                            "(status={}); treating as complete",
                            status);
                        out.status_code = 200;
                        out.network_error = false;
                        out.error.clear();
                    }
                    else
                    {
                        Core::Logger::getInstance().errorFmt(
                            "[AE:LLM] transport failed: url={} status={}",
                            m_endpoint, status);
                        out.error = "LLM transport error";
                        return out;
                    }
                }
            }

            Core::Logger::getInstance().debugFmt(
                "[AE:LLM] stream completed, produced={} status={} resp_text_len={}",
                out.produced, out.status_code, streamBody.size());

            // 兜底：如果 SSE 解析为空但 streamBody 有数据（服务器返回了非流式 JSON）
            if (out.produced == 0 && !streamBody.empty())
            {
                Core::Logger::getInstance().debug(
                    "[AE:LLM] SSE deltas empty but resp.text has data, trying non-stream parse");

                auto j = nlohmann::json::parse(streamBody, nullptr, false);
                if (!j.is_discarded() && j.is_object() &&
                    j.contains("choices") && j["choices"].is_array())
                {
                    for (const auto &c : j["choices"])
                    {
                        if (!c.is_object())
                            continue;

                        // 非流式格式用 message 字段
                        const nlohmann::json *msg = nullptr;
                        if (c.contains("message") && c["message"].is_object())
                            msg = &c["message"];
                        else if (c.contains("delta") && c["delta"].is_object())
                            msg = &c["delta"];
                        if (!msg)
                            continue;

                        if (msg->contains("content") && msg->at("content").is_string())
                        {
                            std::string content = msg->at("content").get<std::string>();
                            if (!content.empty())
                            {
                                push_delta(LlmDelta::text(content));
                                Core::Logger::getInstance().debugFmt(
                                    "[AE:LLM] fallback parse: text content len={}",
                                    content.size());
                            }
                        }

                        // Phase G: 兜底路径同样解析 reasoning_content
                        if (msg->contains("reasoning_content") && msg->at("reasoning_content").is_string())
                        {
                            std::string reasoning = msg->at("reasoning_content").get<std::string>();
                            if (!reasoning.empty())
                                push_delta(LlmDelta::thinking(reasoning));
                        }

                        // tool_calls
                        if (msg->contains("tool_calls") && msg->at("tool_calls").is_array())
                        {
                            for (const auto &t : msg->at("tool_calls"))
                            {
                                if (!t.is_object())
                                    continue;
                                std::string id;
                                std::string name;
                                std::string args;
                                if (t.contains("id") && t["id"].is_string())
                                    id = t["id"].get<std::string>();
                                if (t.contains("function") && t["function"].is_object())
                                {
                                    const auto &func = t["function"];
                                    if (func.contains("name") && func["name"].is_string())
                                        name = func["name"].get<std::string>();
                                    if (func.contains("arguments") && func["arguments"].is_string())
                                        args = func["arguments"].get<std::string>();
                                }
                                nlohmann::json args_json = nlohmann::json::object();
                                if (!args.empty())
                                {
                                    auto repaired = SelfHealingJson::repair(args);
                                    if (repaired.has_value())
                                        args_json = std::move(*repaired);
                                }
                                push_delta(
                                    LlmDelta::tool_call(std::move(id), std::move(name), std::move(args_json)));
                            }
                        }
                    }

                    // usage
                    if (j.contains("usage") && j["usage"].is_object())
                    {
                        const auto &usage = j["usage"];
                        if (usage.contains("prompt_tokens") && usage["prompt_tokens"].is_number() &&
                            usage.contains("completion_tokens") && usage["completion_tokens"].is_number())
                        {
                            push_delta(LlmDelta::usage(
                                static_cast<std::size_t>(usage["prompt_tokens"].get<std::int64_t>()),
                                static_cast<std::size_t>(usage["completion_tokens"].get<std::int64_t>())));
                        }
                    }
                }

                // 关键修复：SSE 兜底路径也尝试 text-based tool call fallback。
                // 同时扫描 reasoning_content，处理模型把工具调用 JSON 放在 thinking 中的情况。
                if (!has_toolcall && (!acc_text.empty() || !acc_reasoning.empty()))
                {
                    auto tc = try_parse_text_tool_call(acc_text);
                    if (!tc.has_value() && !acc_reasoning.empty())
                        tc = try_parse_text_tool_call(acc_reasoning);
                    if (tc.has_value())
                    {
                        Core::Logger::getInstance().infoFmt(
                            "[AE:LLM] SSE fallback: text-based tool call detected: name='{}'",
                            tc->name);
                        push_delta(
                            LlmDelta::tool_call(
                                std::string{}, tc->name, std::move(tc->arguments)));
                    }
                }
            }

            // 流结束后，把累计的 tool_call 碎片组装为完整调用（带 Self-Healing 修复）
            Core::Logger::getInstance().debugFmt(
                "[AE:LLM] tool flush start: arg_buffers.size={}", arg_buffers.size());
            for (auto &[idx, args] : arg_buffers)
            {
                nlohmann::json args_json = nlohmann::json::object();
                const bool args_non_empty = !args.empty();
                if (args_non_empty)
                {
                    auto repaired = SelfHealingJson::repair(args);
                    if (repaired.has_value())
                        args_json = std::move(*repaired);
                    else
                        Core::Logger::getInstance().warningFmt(
                            "[AE:LLM] tool_call[{}] args 无法自修复，已置空并触发自动修复提示", idx);
                }
                Core::Logger::getInstance().debugFmt(
                    "[AE:LLM] tool flush emit idx={} name='{}' id='{}' args_non_empty={} args_len={}",
                    idx, tool_names[idx], tool_ids[idx], args_non_empty, args.size());
                push_delta(
                    LlmDelta::tool_call(tool_ids[idx], tool_names[idx], std::move(args_json)));
            }

            // ────────────────────────────────────────────────────────────────
            // 关键修复：text-based tool call fallback
            //
            // 某些 LLM（如 qwythos-9b-claude-mythos-5-1m、base 微调的 chat 模型）
            // 不支持 OpenAI 原生 function calling 协议，但能遵循 prompt 指令输出
            //   {"thought": "...", "tool": "...", "tool_args": {...}}
            // 结构的 JSON 来表达工具调用。
            //
            // 检测流程：
            //   1) 如果 SSE 阶段已经检测到 tool_calls 字段（arg_buffers 非空），跳过
            //   2) 否则把所有 Text delta 拼接为完整文本
            //   3) 尝试多种 JSON 解析策略（直接解析、code block 提取、{} 匹配）
            //   4) 解析出 "tool" 字段则构造 LlmDelta::tool_call 并追加
            //   5) 同时也把 thought/goal 字段合并为一个 "thought" delta（便于 UI 显示）
            //
            // 这是对未原生支持 function calling 模型的关键兜底，确保 AE 模块
            // 能与 React 模块一样处理文本形式的工具调用指令。
            // ────────────────────────────────────────────────────────────────
            if (arg_buffers.empty() && (!acc_text.empty() || !acc_reasoning.empty()))
            {
                auto text_tool_call = try_parse_text_tool_call(acc_text);
                if (!text_tool_call.has_value() && !acc_reasoning.empty())
                    text_tool_call = try_parse_text_tool_call(acc_reasoning);
                if (text_tool_call.has_value())
                {
                    Core::Logger::getInstance().infoFmt(
                        "[AE:LLM] text-based tool call detected: name='{}' args_size={}",
                        text_tool_call->name,
                        text_tool_call->arguments.dump().size());

                    // 追加 tool_call delta（id 由 AgentEngine 生成）
                    push_delta(
                        LlmDelta::tool_call(
                            std::string{},
                            text_tool_call->name,
                            std::move(text_tool_call->arguments)));
                }
            }

            push_delta(LlmDelta::done());
            return out;
        }

        // 含重试 + 非流式兜底的整轮收集：逐 delta 经 onDelta 推送；终态失败返回 Error。
        // 流式中途已产出部分 delta 的失败不可安全重试（会重复文本），直接报错。
        auto collect_all(const nlohmann::json &body, std::function<void(LlmDelta)> onDelta)
            -> Core::Result<void>
        {
            bool any_produced = false;
            auto sink = [&](LlmDelta d)
            {
                if (d.kind != LlmDelta::Kind::Done)
                    any_produced = true;
                onDelta(std::move(d));
            };

            int attempt = 0;
            for (;;)
            {
                TurnOutcome o = do_request_cb(body, sink);
                if (o.network_error || o.status_code >= 500)
                {
                    // 已产出部分 delta（流中途失败）无法安全重试，交给下方错误处理。
                    if (any_produced || ++attempt > m_max_retries)
                        break;
                    // 指数退避 + 抖动，避免惊群
                    const int raw_base = attempt * 200;
                    const auto base = raw_base < 2000 ? raw_base : 2000;
                    const auto jitter = std::uniform_int_distribution<int>(0, 200)(m_rng);
                    std::this_thread::sleep_for(std::chrono::milliseconds(base + jitter));
                    continue;
                }
                if (!o.error.empty())
                    break; // 4xx 等业务错误，不可重试
                // 流式成功但没有任何 delta（可能 SSE 解析问题），fallback 到非流式
                if (o.produced == 0)
                {
                    Core::Logger::getInstance().warning(
                        "[AE:LLM] stream returned empty deltas, falling back to non-stream");
                    TurnOutcome ns = do_request_nonstream_cb(body, sink);
                    if (!ns.error.empty() || ns.produced == 0)
                        break; // non-stream 也失败
                }
                return Core::Result<void>::Ok();
            }

            // 流式彻底失败：若中途已上发部分 delta，直接报错，避免非流式兜底重复文本。
            if (any_produced)
                return Core::Result<void>::Error(Core::Error{"LLM stream interrupted"});

            Core::Logger::getInstance().warning(
                "[AE:LLM] stream failed, falling back to non-stream mode");
            TurnOutcome ns = do_request_nonstream_cb(body, sink);
            if (ns.error.empty() && ns.produced > 0)
                return Core::Result<void>::Ok();
            return Core::Result<void>::Error(
                ns.error.empty() ? Core::Error{"LLM transport error: empty response"}
                                 : Core::Error{ns.error});
        }

        // ────────────────────────────────────────────────────────────────
        //  try_parse_text_tool_call — 文本中检测 JSON 工具调用指令
        //
        //  支持的格式（按优先级）：
        //    0) <tool_call>{...}</tool_call>  (Hugging Face / qwen-style 工具调用标签)
        //    1) {"thought": "...", "goal": "...", "tool": "...", "tool_args": {...}}
        //    2) {"tool": "...", "tool_args": {...}}
        //    3) ```json ... ```  代码块包裹的 JSON
        //    4) 文本中嵌套的 {...} 子对象（按括号深度匹配）
        // ────────────────────────────────────────────────────────────────
        struct ParsedTextToolCall
        {
            std::string name;
            nlohmann::json arguments;
        };

        static auto try_parse_text_tool_call(std::string_view text)
            -> std::optional<ParsedTextToolCall>
        {
            // 去除首尾空白
            auto start = text.find_first_not_of(" \t\n\r");
            auto end = text.find_last_not_of(" \t\n\r");
            if (start == std::string::npos)
                return std::nullopt;
            std::string trimmed{text.substr(start, end - start + 1)};

            auto extract_tool_from_json = [](const nlohmann::json &j) -> std::optional<ParsedTextToolCall>
            {
                if (j.is_object() && j.contains("tool") && j["tool"].is_string())
                {
                    ParsedTextToolCall r;
                    r.name = j["tool"].get<std::string>();
                    r.arguments = j.value("tool_args", nlohmann::json::object());
                    return r;
                }
                return std::nullopt;
            };

            // 策略 0：<tool_call>...</tool_call> (Hugging Face / qwen-style 工具调用标签)
            //
            // 支持两种子格式：
            //   a) <tool_call>{"tool": "...", "tool_args": {...}}</tool_call>  (纯 JSON)
            //   b) <tool_call><function>name</function><arguments>{...}</arguments></tool_call>  (XML-like)
            {
                constexpr std::string_view kOpenTag{"<tool_call>"};
                constexpr std::string_view kCloseTag{"</tool_call>"};
                const auto open_pos = trimmed.find(kOpenTag);
                if (open_pos != std::string::npos)
                {
                    const auto content_begin = open_pos + kOpenTag.size();
                    const auto close_pos = trimmed.find(kCloseTag, content_begin);
                    if (close_pos != std::string::npos)
                    {
                        std::string inner = trimmed.substr(content_begin, close_pos - content_begin);

                        // 子格式 a：内部是 JSON
                        try
                        {
                            auto j = nlohmann::json::parse(inner);
                            if (auto r = extract_tool_from_json(j); r.has_value())
                                return r;
                        }
                        catch (...)
                        {
                        }

                        // 子格式 b：内部是 <function>name</function><arguments>{...}</arguments>
                        constexpr std::string_view kFnOpen{"<function>"};
                        constexpr std::string_view kFnClose{"</function>"};
                        constexpr std::string_view kArgsOpen{"<arguments>"};
                        constexpr std::string_view kArgsClose{"</arguments>"};
                        const auto fn_b = inner.find(kFnOpen);
                        const auto fn_e = inner.find(kFnClose);
                        if (fn_b != std::string::npos && fn_e != std::string::npos && fn_e > fn_b)
                        {
                            ParsedTextToolCall r;
                            r.name = inner.substr(fn_b + kFnOpen.size(),
                                                  fn_e - fn_b - kFnOpen.size());
                            // trim 工具名
                            auto fns = r.name.find_first_not_of(" \t\n\r");
                            auto fne = r.name.find_last_not_of(" \t\n\r");
                            if (fns != std::string::npos && fne != std::string::npos)
                                r.name = r.name.substr(fns, fne - fns + 1);

                            // 尝试解析 <arguments> 块
                            const auto a_b = inner.find(kArgsOpen, fn_e);
                            const auto a_e = inner.find(kArgsClose, a_b);
                            if (a_b != std::string::npos && a_e != std::string::npos && a_e > a_b)
                            {
                                try
                                {
                                    r.arguments = nlohmann::json::parse(
                                        inner.substr(a_b + kArgsOpen.size(),
                                                     a_e - a_b - kArgsOpen.size()));
                                }
                                catch (...)
                                {
                                    r.arguments = nlohmann::json::object();
                                }
                            }
                            else
                            {
                                r.arguments = nlohmann::json::object();
                            }

                            if (!r.name.empty())
                                return r;
                        }
                    }
                }
            }

            // 策略 1：直接 JSON 解析
            // 快速预判：普通文本回复首字符不会是 '{' 或 '['，跳过 JSON parse 避免崩溃
            {
                auto ps = trimmed.find_first_not_of(" \t\n\r");
                if (ps != std::string::npos && trimmed[ps] != '{' && trimmed[ps] != '[')
                {
                    goto strategy2;
                }
            }
            // 用 allow_exceptions=false，避免非法输入触发 std::terminate
            {
                auto j = nlohmann::json::parse(trimmed, nullptr, false);
                if (j.is_null())
                {
                }
                else
                {
                    if (auto r = extract_tool_from_json(j); r.has_value())
                        return r;
                }
            }

            // 策略 2：```json ... ``` 代码块
            strategy2:
            {
                constexpr std::string_view kFence{"```"};
                const auto fence_b = trimmed.find(kFence);
                if (fence_b != std::string::npos)
                {
                    const auto content_begin = fence_b + kFence.size();
                    const auto fence_e = trimmed.find(kFence, content_begin);
                    if (fence_e != std::string::npos && fence_e > content_begin)
                    {
                        std::string inner = trimmed.substr(content_begin,
                                                           fence_e - content_begin);
                        auto j = nlohmann::json::parse(inner, nullptr, false);
                        if (!j.is_null() && extract_tool_from_json(j).has_value())
                            return extract_tool_from_json(j);
                    }
                }
            }

            // 策略 3：按括号深度匹配第一个完整 {...}
            {
                int depth = 0;
                std::size_t obj_start = trimmed.find('{');
                if (obj_start != std::string::npos)
                {
                    for (std::size_t i = obj_start; i < trimmed.size(); ++i)
                    {
                        if (trimmed[i] == '{')
                            ++depth;
                        else if (trimmed[i] == '}')
                        {
                            --depth;
                            if (depth == 0)
                            {
                                auto sub = trimmed.substr(obj_start, i - obj_start + 1);
                                // 修复尾部多余逗号
                                for (std::size_t pos = 0;
                                     (pos = sub.find(",}")) != std::string::npos;)
                                    sub.replace(pos, 2, "}");
                                for (std::size_t pos = 0;
                                     (pos = sub.find(",]")) != std::string::npos;)
                                    sub.replace(pos, 2, "]");

                                auto j = nlohmann::json::parse(sub, nullptr, false);
                                if (!j.is_null() && extract_tool_from_json(j).has_value())
                                    return extract_tool_from_json(j);
                                break;
                            }
                        }
                    }
                }
            }

            return std::nullopt;
        }

        // 非流式请求（fallback）：把完整响应当作一个个 delta 经 onDelta 推送
        auto do_request_nonstream_cb(const nlohmann::json &body, std::function<void(LlmDelta)> onDelta) -> TurnOutcome
        {
            TurnOutcome out;

            // 本地累加器（文本 + 思考内容 + 工具调用标志），供下方文本工具调用兜底使用。
            // reasoning_content 同样参与兜底解析，兼容把工具调用 JSON 放在 thinking 中的模型。
            std::string acc_text;
            std::string acc_reasoning;
            bool has_toolcall = false;
            auto push_delta = [&](LlmDelta d)
            {
                if (d.kind == LlmDelta::Kind::Text && !d.text_content.empty())
                    acc_text += d.text_content;
                else if (d.kind == LlmDelta::Kind::Thinking && !d.thinking_content.empty())
                    acc_reasoning += d.thinking_content;
                else if (d.kind == LlmDelta::Kind::ToolCall)
                    has_toolcall = true;
                if (d.kind != LlmDelta::Kind::Done)
                    ++out.produced;
                onDelta(std::move(d));
            };

            VMX::Core::Net::CurlRequest nsReq;
            nsReq.method = "POST";
            nsReq.url = m_endpoint;
            nsReq.body = body.dump();
            nsReq.timeoutMs = m_timeout_ms;
            nsReq.headers["Accept"] = "application/json";
            nsReq.headers["Content-Type"] = "application/json";
            nsReq.headers["Authorization"] = "Bearer " + std::string{m_api_key};

            Core::Logger::getInstance().debugFmt(
                "[AE:LLM] non-stream POST {} body={}",
                m_endpoint, nsReq.body.substr(0, 500));

            auto nsResult = VMX::Core::Net::curlPerform(nsReq);
            if (nsResult.isError())
            {
                out.network_error = true;
                Core::Logger::getInstance().errorFmt(
                    "[AE:LLM] non-stream transport failed: url={} msg={}",
                    m_endpoint, nsResult.error().message());
                out.error = "LLM transport error";
                return out;
            }
            const VMX::Core::Net::CurlResponse &resp = nsResult.value();
            out.status_code = static_cast<long>(resp.statusCode);
            out.network_error = false;

            if (resp.statusCode < 200 || resp.statusCode >= 300)
            {
                std::string body_preview = resp.body.substr(0, 500);
                Core::Logger::getInstance().errorFmt(
                    "[AE:LLM] non-stream transport failed: url={} status={} body={}",
                    m_endpoint, resp.statusCode, body_preview);
                out.error = "LLM transport error";
                return out;
            }

            // 解析非流式 JSON 响应（nlohmann DOM，无异常解析）
            auto doc = nlohmann::json::parse(resp.body, nullptr, false);
            if (doc.is_discarded() || !doc.is_object())
            {
                Core::Logger::getInstance().errorFmt(
                    "[AE:LLM] non-stream response parse failed, body={}",
                    resp.body.substr(0, 500));
                out.error = "invalid JSON response";
                return out;
            }

            if (!doc.contains("choices") || !doc["choices"].is_array())
            {
                Core::Logger::getInstance().errorFmt(
                    "[AE:LLM] non-stream response missing 'choices', body={}",
                    resp.body.substr(0, 500));
                out.error = "invalid response: no choices";
                return out;
            }

            for (const auto &c : doc["choices"])
            {
                if (!c.is_object())
                    continue;
                if (!c.contains("message") || !c["message"].is_object())
                    continue;

                const nlohmann::json &msg = c["message"];

                if (msg.contains("content") && msg["content"].is_string())
                {
                    std::string content = msg["content"].get<std::string>();
                    if (!content.empty())
                        push_delta(LlmDelta::text(content));
                }

                // Phase G: 非流式路径同样解析 reasoning_content
                if (msg.contains("reasoning_content") && msg["reasoning_content"].is_string())
                {
                    std::string reasoning = msg["reasoning_content"].get<std::string>();
                    if (!reasoning.empty())
                        push_delta(LlmDelta::thinking(reasoning));
                }

                // tool_calls
                if (msg.contains("tool_calls") && msg["tool_calls"].is_array())
                {
                    for (const auto &t : msg["tool_calls"])
                    {
                        if (!t.is_object())
                            continue;
                        std::string id;
                        std::string name;
                        std::string args;
                        if (t.contains("id") && t["id"].is_string())
                            id = t["id"].get<std::string>();
                        if (t.contains("function") && t["function"].is_object())
                        {
                            const auto &func = t["function"];
                            if (func.contains("name") && func["name"].is_string())
                                name = func["name"].get<std::string>();
                            if (func.contains("arguments") && func["arguments"].is_string())
                                args = func["arguments"].get<std::string>();
                        }
                        nlohmann::json args_json = nlohmann::json::object();
                        if (!args.empty())
                        {
                            auto repaired = SelfHealingJson::repair(args);
                            if (repaired.has_value())
                                args_json = std::move(*repaired);
                        }
                        push_delta(
                            LlmDelta::tool_call(std::move(id), std::move(name), std::move(args_json)));
                    }
                }
            }

            // usage
            if (doc.contains("usage") && doc["usage"].is_object())
            {
                const auto &usage = doc["usage"];
                if (usage.contains("prompt_tokens") && usage["prompt_tokens"].is_number() &&
                    usage.contains("completion_tokens") && usage["completion_tokens"].is_number())
                {
                    push_delta(LlmDelta::usage(
                        static_cast<std::size_t>(usage["prompt_tokens"].get<std::int64_t>()),
                        static_cast<std::size_t>(usage["completion_tokens"].get<std::int64_t>())));
                }
            }

            // ────────────────────────────────────────────────────────────────
            // 关键修复：text-based tool call fallback（非流式路径）
            //
            // 与流式路径相同：检查是否有原生 tool_calls，没有则尝试从文本中解析
            // {"tool": "...", "tool_args": {...}} 格式的工具调用指令。
            // ────────────────────────────────────────────────────────────────
            if (!has_toolcall && (!acc_text.empty() || !acc_reasoning.empty()))
            {
                auto text_tool_call = try_parse_text_tool_call(acc_text);
                if (!text_tool_call.has_value() && !acc_reasoning.empty())
                    text_tool_call = try_parse_text_tool_call(acc_reasoning);
                if (text_tool_call.has_value())
                {
                    Core::Logger::getInstance().infoFmt(
                        "[AE:LLM] non-stream: text-based tool call detected: name='{}' args_size={}",
                        text_tool_call->name,
                        text_tool_call->arguments.dump().size());
                    push_delta(
                        LlmDelta::tool_call(
                            std::string{},
                            text_tool_call->name,
                            std::move(text_tool_call->arguments)));
                }
            }

            push_delta(LlmDelta::done());
            Core::Logger::getInstance().debugFmt(
                "[AE:LLM] non-stream completed, produced={}", out.produced);
            return out;
        }

        std::string m_endpoint;
        VMX::Core::Security::SecureString m_api_key;
        int m_timeout_ms;
        int m_max_retries;
        std::mt19937 m_rng{std::random_device{}()};
    };

} // namespace VMX::AE
