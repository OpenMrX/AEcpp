/// @file demo/tui/Bridge/AETUIBridge.cpp
/// @brief AE ↔ TUI 接线桥实现（Chat / Agent 双模式，见 AETUIBridge.hpp 总述）。

#include "AETUIBridge.hpp"

#include "AE.hpp"
#include "AEType.hpp"
#include "IAE.hpp"
#include "LLMAdapter.hpp"
#include "StreamingLLMClient.hpp"
#include "Core/Security/SecureString.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <print>
#include <random>
#include <sstream>
#include <string_view>

// demo 层不包含任何 Windows SDK 头文件。exe 目录定位用 MSVC CRT 的 _get_pgmptr
// （curl 头会传递引入 Windows SDK 声明，不能再自行 extern 声明 GetModuleFileNameA）。
#ifdef _MSC_VER
#include <stdlib.h>
#endif

namespace AE::TUI
{

namespace
{
    // 消息 id：random_device 取 8 字节转 16 位十六进制（同 TUI.cpp 口径）。
    auto genId() -> std::string
    {
        std::random_device rd;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | (static_cast<std::uint64_t>(rd()) & 0xFFu);
        std::ostringstream oss;
        oss << std::hex << std::setw(16) << std::setfill('0') << v;
        return oss.str();
    }

    auto exeDir() -> std::filesystem::path
    {
#ifdef _MSC_VER
        char *pgm = nullptr;
        if (_get_pgmptr(&pgm) == 0 && pgm != nullptr)
            return std::filesystem::path(std::string{pgm}).parent_path();
#endif
        return std::filesystem::current_path();
    }

    auto envOrEmpty(const char *name) -> std::string
    {
        const char *v = std::getenv(name);
        return v ? std::string{v} : std::string{};
    }

    auto truthy(std::string_view s) -> bool
    {
        return s == "1" || s == "true" || s == "TRUE" || s == "on" || s == "yes";
    }

    // Agent 模式 demo 工具：返回当前时间。让工具循环链路真实可跑通
    // （SimpleToolProvider 无注册工具时 schema 为空，引擎单轮直出 Final）。
    auto makeDemoToolProvider() -> AE::Engine::SimpleToolProvider
    {
        AE::Engine::SimpleToolProvider tools;
        tools.register_tool("current_time", [](const nlohmann::json &) -> Core::Result<nlohmann::json>
                            {
            const auto now = std::chrono::system_clock::now();
            const auto t = std::chrono::system_clock::to_time_t(now);
            std::tm tmv{};
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
            return Core::Result<nlohmann::json>::Ok(nlohmann::json{{"time", buf}}); });
        tools.set_tools_schema(nlohmann::json::array(
            {{{"type", "function"},
              {"function",
               {{"name", "current_time"},
                {"description", "获取当前本地日期与时间（用户问现在几点/今天日期时使用）"},
                {"parameters", {{"type", "object"}, {"properties", nlohmann::json::object()}}}}}}}));
        return tools;
    }
} // namespace

    // ─────────────────────────────── 配置 ───────────────────────────────

    auto AETUIBridge::normalizeEndpoint(const std::string &endpoint) -> std::string
    {
        std::string url = endpoint;
        // StreamingLLMClient 把 endpoint 直接当完整请求 URL；只给到 /v1 时补全路径。
        if (url.ends_with("/v1"))
            url += "/chat/completions";
        else if (url.ends_with("/v1/"))
            url += "chat/completions";
        return url;
    }

    auto AETUIBridge::loadConfig() -> BridgeConfig
    {
        // 1) 内置占位默认。
        BridgeConfig cfg;
        cfg.endpoint = "https://api.openai.com/v1";
        cfg.apiKey = "";
        cfg.model = "gpt-4o-mini";
        cfg.agentMode = false;

        // 2) exe 同目录 demo_config.json（可选，尽力而为）。
        try
        {
            const auto path = exeDir() / "demo_config.json";
            if (std::filesystem::exists(path))
            {
                std::ifstream in(path);
                const auto j = nlohmann::json::parse(in, nullptr, false);
                if (!j.is_discarded() && j.is_object())
                {
                    if (j.contains("endpoint") && j["endpoint"].is_string())
                        cfg.endpoint = j["endpoint"].get<std::string>();
                    if (j.contains("apiKey") && j["apiKey"].is_string())
                        cfg.apiKey = j["apiKey"].get<std::string>();
                    if (j.contains("model") && j["model"].is_string())
                        cfg.model = j["model"].get<std::string>();
                    if (j.contains("agentMode") && j["agentMode"].is_boolean())
                        cfg.agentMode = j["agentMode"].get<bool>();
                    if (j.contains("modelList") && j["modelList"].is_array())
                        cfg.modelList = j["modelList"].get<std::vector<std::string>>();
                }
            }
        }
        catch (...)
        {
            // 配置读取失败回退默认，绝不让 demo 无法启动。
        }

        // 3) 环境变量（最高优先级）。
        if (auto v = envOrEmpty("VXAE_ENDPOINT"); !v.empty())
            cfg.endpoint = v;
        if (auto v = envOrEmpty("VXAE_API_KEY"); !v.empty())
            cfg.apiKey = v;
        if (auto v = envOrEmpty("VXAE_MODEL"); !v.empty())
            cfg.model = v;
        if (auto v = envOrEmpty("VXAE_AGENT_MODE"); !v.empty())
            cfg.agentMode = truthy(v);

        if (cfg.modelList.empty())
            cfg.modelList.push_back(cfg.model);
        return cfg;
    }

    auto AETUIBridge::saveConfig(const BridgeConfig &cfg) -> bool
    {
        // demo_config.json 与 loadConfig 同路径（exe 同目录）、同字段口径：
        // 模型配置页"保存并应用"时按需持久化，重启后经 loadConfig 恢复。
        try
        {
            nlohmann::json j;
            j["endpoint"] = cfg.endpoint;
            j["apiKey"] = cfg.apiKey;
            j["model"] = cfg.model;
            j["agentMode"] = cfg.agentMode;
            j["modelList"] = cfg.modelList;

            const auto path = exeDir() / "demo_config.json";
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                std::println(stderr, "[Bridge] persist config failed: cannot open {}",
                             path.string());
                return false;
            }
            out << j.dump(2) << '\n';
            return true;
        }
        catch (const std::exception &e)
        {
            std::println(stderr, "[Bridge] persist config failed: {}", e.what());
            return false;
        }
        catch (...)
        {
            std::println(stderr, "[Bridge] persist config failed: unknown exception");
            return false;
        }
    }

    auto AETUIBridge::hasConfiguredModelList() -> bool
    {
        // 1) 环境变量显式指定模型即视为已配置（优先级最高，启动不再弹配置页）。
        if (auto v = envOrEmpty("VXAE_MODEL"); !v.empty())
        {
            return true;
        }
        // 2) exe 同目录 demo_config.json：非空 modelList（模型配置页保存的多模型
        //    列表），或旧格式仅有的非空 model 字段。
        try
        {
            const auto path = exeDir() / "demo_config.json";
            if (!std::filesystem::exists(path))
            {
                return false;
            }
            std::ifstream in(path);
            const auto j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_discarded() || !j.is_object())
            {
                return false;
            }
            if (j.contains("modelList") && j["modelList"].is_array())
            {
                for (const auto &entry : j["modelList"])
                {
                    if (entry.is_string() && !entry.get<std::string>().empty())
                    {
                        return true;
                    }
                }
            }
            if (j.contains("model") && j["model"].is_string() &&
                !j["model"].get<std::string>().empty())
            {
                return true;
            }
            return false;
        }
        catch (...)
        {
            // 配置损坏/不可读：按"未配置"处理，引导用户进配置页重建。
            return false;
        }
    }

    // ─────────────────────────────── 生命周期 ───────────────────────────────

    AETUIBridge::AETUIBridge(std::shared_ptr<ITUI> tui) : m_tui(std::move(tui)) {}

    AETUIBridge::~AETUIBridge()
    {
        shutdown();
    }

    auto AETUIBridge::initialize() -> Core::Result<void>
    {
        return initialize(loadConfig());
    }

    auto AETUIBridge::initialize(BridgeConfig cfg) -> Core::Result<void>
    {
        if (!m_tui)
            return Core::Result<void>::Error(Core::Error{"AETUIBridge: null ITUI"});
        if (m_initialized)
            return Core::Result<void>::Ok();

        {
            std::scoped_lock lock(m_cfgMutex);
            m_config = cfg;
        }

        const bool connected = !cfg.apiKey.empty();
        m_tui->setModelList(cfg.modelList);
        m_tui->setModelStatus(cfg.model, m_thinkingEnabled.load(), m_intentEnabled.load());
        m_tui->setConnected(connected);

        std::vector<StatusItem> items;
        items.push_back(StatusItem{.label = "模式", .value = cfg.agentMode ? "agent" : "chat", .color = "cyan"});
        items.push_back(StatusItem{.label = "端点", .value = cfg.endpoint, .color = "gray"});
        m_tui->setStatusItems(items);

        if (!connected)
            postSystemMessage(m_tui->getCurrentSessionId(),
                              "⚠ 未配置 API Key（VXAE_API_KEY / demo_config.json.apiKey），"
                              "连接指示灯为断开状态；本地端点可忽略此提示。");

        m_initialized = true;
        return Core::Result<void>::Ok();
    }

    auto AETUIBridge::shutdown() -> Core::Result<void>
    {
        m_stopRequested.store(true);
        if (m_worker.joinable())
        {
            m_worker.request_stop();
            m_worker.join();
        }
        m_initialized = false;
        return Core::Result<void>::Ok();
    }

    // ─────────────────────────────── 请求入口 ───────────────────────────────

    auto AETUIBridge::handleUserInput(std::string input, std::string sessionId) -> void
    {
        if (!m_initialized || input.empty())
            return;
        if (sessionId.empty())
            sessionId = m_tui->getCurrentSessionId();

        if (m_processing.exchange(true))
        {
            postSystemMessage(sessionId, "上一条请求仍在处理中（Ctrl+P 可停止）。");
            m_processing.store(false);
            return;
        }
        m_stopRequested.store(false);

        BridgeConfig cfg;
        {
            std::scoped_lock lock(m_cfgMutex);
            cfg = m_config;
        }

        // 追加用户消息到 AE 历史 + UI 镜像（TUI 侧用户气泡已由 onInputSubmit 渲染）。
        nlohmann::json history;
        {
            std::scoped_lock lock(m_stateMutex);
            auto &state = m_sessions[sessionId];
            state.history.push_back(nlohmann::json{{"role", "user"}, {"content", input}});
            Message userMsg;
            userMsg.id = genId();
            userMsg.role = MessageRole::User;
            userMsg.content = input;
            userMsg.status = MessageStatus::Completed;
            state.uiMessages.push_back(userMsg);
            history = state.history;
        }

        // 助手流式占位气泡。
        const std::string assistantId = genId();
        {
            Message placeholder;
            placeholder.id = assistantId;
            placeholder.role = MessageRole::Assistant;
            placeholder.status = MessageStatus::Streaming;
            placeholder.modelName = cfg.model;
            m_tui->addMessage(sessionId, placeholder);
            mirrorAddMessage(sessionId, placeholder);
        }

        m_worker = std::jthread([this, cfg, sessionId, assistantId, history](std::stop_token)
                                { workerBody(cfg, sessionId, assistantId, history); });
    }

    auto AETUIBridge::requestStop() -> void
    {
        m_stopRequested.store(true);
    }

    auto AETUIBridge::clearHistory(const std::string &sessionId) -> void
    {
        std::scoped_lock lock(m_stateMutex);
        m_sessions.erase(sessionId);
    }

    auto AETUIBridge::messagesFor(const std::string &sessionId) const -> std::vector<Message>
    {
        std::scoped_lock lock(m_stateMutex);
        auto it = m_sessions.find(sessionId);
        return it == m_sessions.end() ? std::vector<Message>{} : it->second.uiMessages;
    }

    auto AETUIBridge::switchModel(const std::string &model) -> void
    {
        if (model.empty())
            return;
        {
            std::scoped_lock lock(m_cfgMutex);
            m_config.model = model;
            if (std::find(m_config.modelList.begin(), m_config.modelList.end(), model) ==
                m_config.modelList.end())
                m_config.modelList.push_back(model);
        }
        m_tui->setModelStatus(model, m_thinkingEnabled.load(), m_intentEnabled.load());
        m_tui->setModelList([this]
                            {
            std::scoped_lock lock(m_cfgMutex);
            return m_config.modelList; }());
    }

    auto AETUIBridge::applyConfig(BridgeConfig cfg) -> void
    {
        if (cfg.modelList.empty())
        {
            cfg.modelList.push_back(cfg.model);
        }
        // 当前模型必须出现在可选列表里（与 switchModel 同口径），否则底部列表
        // 选不中刚保存的模型。
        if (std::find(cfg.modelList.begin(), cfg.modelList.end(), cfg.model) ==
            cfg.modelList.end())
        {
            cfg.modelList.push_back(cfg.model);
        }
        {
            std::scoped_lock lock(m_cfgMutex);
            m_config = std::move(cfg);
        }

        if (!m_tui)
        {
            return;
        }

        // 与 initialize() 同口径联动 TUI：模型列表/模型状态/连接灯/状态栏（模式/端点）。
        const BridgeConfig snapshot = config();
        m_tui->setModelList(snapshot.modelList);
        m_tui->setModelStatus(snapshot.model, m_thinkingEnabled.load(), m_intentEnabled.load());
        m_tui->setConnected(!snapshot.apiKey.empty());

        std::vector<StatusItem> items;
        items.push_back(StatusItem{.label = "模式", .value = snapshot.agentMode ? "agent" : "chat", .color = "cyan"});
        items.push_back(StatusItem{.label = "端点", .value = snapshot.endpoint, .color = "gray"});
        m_tui->setStatusItems(items);

        if (snapshot.apiKey.empty())
        {
            postSystemMessage(m_tui->getCurrentSessionId(),
                              "⚠ 未配置 API Key（VXAE_API_KEY / demo_config.json.apiKey），"
                              "连接指示灯为断开状态；本地端点可忽略此提示。");
        }
    }

    auto AETUIBridge::syncToggles(bool thinkingEnabled, bool intentEnabled) -> void
    {
        m_thinkingEnabled.store(thinkingEnabled);
        m_intentEnabled.store(intentEnabled);
        std::scoped_lock lock(m_cfgMutex);
        m_tui->setModelStatus(m_config.model, thinkingEnabled, intentEnabled);
    }

    // ─────────────────────────────── worker 主体 ───────────────────────────────

    auto AETUIBridge::workerBody(BridgeConfig cfg, std::string sessionId,
                                 std::string assistantId, nlohmann::json history) -> void
    {
        std::string finalText;
        try
        {
            finalText = cfg.agentMode ? runAgentTurn(cfg, sessionId, assistantId, history)
                                      : runChatTurn(cfg, sessionId, assistantId, history);
        }
        catch (const std::exception &e)
        {
            postSystemMessage(sessionId, std::string{"请求异常："} + e.what());
            m_tui->finishStreamingMessage(sessionId, assistantId);
            m_processing.store(false);
            return;
        }
        catch (...)
        {
            postSystemMessage(sessionId, "请求发生未知异常。");
            m_tui->finishStreamingMessage(sessionId, assistantId);
            m_processing.store(false);
            return;
        }

        // 成功（或已停止的部分结果）：写回 AE 历史与 UI 镜像。
        if (!finalText.empty())
        {
            {
                std::scoped_lock lock(m_stateMutex);
                auto &state = m_sessions[sessionId];
                state.history.push_back(nlohmann::json{{"role", "assistant"}, {"content", finalText}});
                for (auto &m : state.uiMessages)
                    if (m.id == assistantId)
                        m.status = MessageStatus::Completed;
            }
        }
        m_processing.store(false);
    }

    // ─────────────────────────────── Chat 模式 ───────────────────────────────

    auto AETUIBridge::runChatTurn(const BridgeConfig &cfg, const std::string &sessionId,
                                  const std::string &assistantId, const nlohmann::json &history)
        -> std::string
    {
        AE::Engine::StreamingLLMClient client(normalizeEndpoint(cfg.endpoint),
                                      Core::Security::SecureString(cfg.apiKey));
        nlohmann::json body;
        body["model"] = cfg.model;
        body["messages"] = history;
        body["stream"] = true;

        std::string fullText;
        std::size_t promptTokens = 0;
        std::size_t completionTokens = 0;
        bool failed = false;
        std::string errorMsg;

        for (auto &&delta : client.stream_chat_gen(body))
        {
            if (stopRequested())
                break; // 生成器析构会汇合内部 HTTP 线程（在途响应收完即止）
            switch (delta.kind)
            {
            case AE::Engine::LlmDelta::Kind::Text:
                fullText += delta.text_content;
                m_tui->updateStreamingMessage(sessionId, assistantId, delta.text_content);
                mirrorAppendAssistant(sessionId, assistantId, delta.text_content);
                break;
            case AE::Engine::LlmDelta::Kind::Thinking:
                // ITUI::setThinkingContent 为追加语义，直传增量。
                m_tui->setThinkingContent(sessionId, assistantId, delta.thinking_content);
                mirrorAppendThinking(sessionId, assistantId, delta.thinking_content);
                break;
            case AE::Engine::LlmDelta::Kind::Usage:
                if (delta.usage_info)
                {
                    promptTokens = delta.usage_info->prompt_tokens;
                    completionTokens = delta.usage_info->completion_tokens;
                }
                break;
            case AE::Engine::LlmDelta::Kind::Error:
                failed = true;
                errorMsg = delta.error_message;
                break;
            case AE::Engine::LlmDelta::Kind::ToolCall:
            case AE::Engine::LlmDelta::Kind::Done:
            default:
                break;
            }
            if (failed)
                break;
        }

        m_tui->finishStreamingMessage(sessionId, assistantId);
        m_tui->setMessageTokens(sessionId, assistantId,
                                static_cast<int32_t>(promptTokens),
                                static_cast<int32_t>(completionTokens),
                                0,
                                static_cast<int32_t>(promptTokens + completionTokens));
        mirrorFinishAssistant(sessionId, assistantId,
                              static_cast<int32_t>(promptTokens),
                              static_cast<int32_t>(completionTokens), 0,
                              static_cast<int32_t>(promptTokens + completionTokens));

        if (failed)
        {
            postSystemMessage(sessionId, "请求失败：" + errorMsg);
            return {};
        }
        if (stopRequested())
            postSystemMessage(sessionId, "⏹ 已停止生成。");
        return fullText;
    }

    // ─────────────────────────────── Agent 模式 ───────────────────────────────

    auto AETUIBridge::runAgentTurn(const BridgeConfig &cfg, const std::string &sessionId,
                                   const std::string &assistantId, const nlohmann::json &history)
        -> std::string
    {
        AE::Engine::StreamingLLMClient client(normalizeEndpoint(cfg.endpoint),
                                      Core::Security::SecureString(cfg.apiKey));
        AE::Engine::VllmAdapter adapter(client);
        auto tools = makeDemoToolProvider();
        AE::Engine::SimpleContextSource ctx;
        AE::Engine::AgentEngine engine(tools, ctx, adapter);

        nlohmann::json body;
        body["model"] = cfg.model;
        body["messages"] = history;
        body["stream"] = true;

        const std::size_t tokensBefore = ctx.token_count();
        std::string fullText;      // 跨迭代累计（仅最后一轮正文计入历史）
        std::string iterText;      // 当前迭代流出的正文（工具轮为中间 JSON 文本，将被清空气泡）
        std::string thinkingText;
        bool failed = false;
        std::string errorMsg;

        // ToolCall→ToolResult 配对队列（引擎按 pending 顺序成对产出）。
        struct PendingTool
        {
            std::string id;
            std::string name;
        };
        std::vector<PendingTool> pendingTools;

        for (auto &&ev : engine.run(sessionId, body, 16))
        {
            if (stopRequested())
            {
                engine.request_stop();
                break;
            }
            switch (ev.kind)
            {
            case AE::Engine::EventKind::Thinking:
                thinkingText += ev.payload;
                m_tui->setThinkingContent(sessionId, assistantId, ev.payload);
                mirrorAppendThinking(sessionId, assistantId, ev.payload);
                break;
            case AE::Engine::EventKind::Token:
                iterText += ev.payload;
                m_tui->updateStreamingMessage(sessionId, assistantId, ev.payload);
                mirrorAppendAssistant(sessionId, assistantId, ev.payload);
                break;
            case AE::Engine::EventKind::ToolCall:
            {
                // 工具轮：本轮已流出的中间文本（工具调用 JSON）从气泡清除。
                if (!iterText.empty())
                {
                    m_tui->setMessageContent(sessionId, assistantId, std::string_view{});
                    mirrorSetAssistantContent(sessionId, assistantId, {});
                    iterText.clear();
                }
                Message toolMsg;
                toolMsg.id = genId();
                toolMsg.role = MessageRole::Tool;
                toolMsg.status = MessageStatus::Streaming;
                toolMsg.content = "🔧 调用工具：" + ev.payload;
                toolMsg.toolCallName = ev.payload;
                m_tui->addMessage(sessionId, toolMsg);
                mirrorAddMessage(sessionId, toolMsg);
                pendingTools.push_back({toolMsg.id, ev.payload});
                break;
            }
            case AE::Engine::EventKind::ToolResult:
            {
                if (pendingTools.empty())
                    break;
                const auto tool = pendingTools.front();
                pendingTools.erase(pendingTools.begin());
                const std::string content = "🔧 " + tool.name + " 结果：\n" + ev.payload;
                m_tui->setMessageContent(sessionId, tool.id, content);
                std::scoped_lock lock(m_stateMutex);
                auto &state = m_sessions[sessionId];
                for (auto &m : state.uiMessages)
                    if (m.id == tool.id)
                    {
                        m.content = content;
                        m.status = MessageStatus::Completed;
                    }
                break;
            }
            case AE::Engine::EventKind::Final:
            {
                if (!ev.payload.empty() && iterText.empty())
                {
                    // 未流式出正文的收尾轮（如 replay/abort 后带全文）：整段覆盖。
                    m_tui->setMessageContent(sessionId, assistantId, ev.payload);
                    mirrorSetAssistantContent(sessionId, assistantId, ev.payload);
                    iterText = ev.payload;
                }
                break;
            }
            case AE::Engine::EventKind::Error:
                failed = true;
                errorMsg = ev.error_info.message();
                break;
            default:
                break;
            }
            if (failed)
                break;
            // 工具轮结束进入下一迭代：本轮 Token 属于中间产物，不计入最终正文。
            if (ev.kind == AE::Engine::EventKind::ToolResult)
                iterText.clear();
            if (ev.kind == AE::Engine::EventKind::Final)
            {
                fullText = iterText;
                iterText.clear();
            }
        }

        m_tui->finishStreamingMessage(sessionId, assistantId);
        // 引擎内部吞并 Usage delta（只累计进 ctx）：以 ctx 增量作为本轮总 token。
        const auto tokensTotal = ctx.token_count() - tokensBefore;
        m_tui->setMessageTokens(sessionId, assistantId, 0, 0, 0,
                                static_cast<int32_t>(tokensTotal));
        mirrorFinishAssistant(sessionId, assistantId, 0, 0, 0, static_cast<int32_t>(tokensTotal));

        if (failed)
        {
            postSystemMessage(sessionId, "Agent 请求失败：" + errorMsg);
            return {};
        }
        if (stopRequested())
            postSystemMessage(sessionId, "⏹ 已停止生成。");
        return fullText;
    }

    // ─────────────────────────────── UI 镜像辅助 ───────────────────────────────

    auto AETUIBridge::postSystemMessage(const std::string &sessionId, const std::string &text) -> void
    {
        if (sessionId.empty() || !m_tui)
            return;
        Message sys;
        sys.id = genId();
        sys.role = MessageRole::System;
        sys.content = text;
        sys.status = MessageStatus::Completed;
        m_tui->addMessage(sessionId, sys);
        mirrorAddMessage(sessionId, sys);
    }

    auto AETUIBridge::mirrorAddMessage(const std::string &sessionId, const Message &message) -> void
    {
        std::scoped_lock lock(m_stateMutex);
        m_sessions[sessionId].uiMessages.push_back(message);
    }

    auto AETUIBridge::mirrorAppendAssistant(const std::string &sessionId, const std::string &messageId,
                                            const std::string &delta) -> void
    {
        std::scoped_lock lock(m_stateMutex);
        for (auto &m : m_sessions[sessionId].uiMessages)
            if (m.id == messageId)
                m.content += delta;
    }

    auto AETUIBridge::mirrorAppendThinking(const std::string &sessionId, const std::string &messageId,
                                           const std::string &delta) -> void
    {
        std::scoped_lock lock(m_stateMutex);
        for (auto &m : m_sessions[sessionId].uiMessages)
            if (m.id == messageId)
                m.thinkingContent += delta;
    }

    auto AETUIBridge::mirrorSetAssistantContent(const std::string &sessionId,
                                                const std::string &messageId,
                                                const std::string &content) -> void
    {
        std::scoped_lock lock(m_stateMutex);
        for (auto &m : m_sessions[sessionId].uiMessages)
            if (m.id == messageId)
                m.content = content;
    }

    auto AETUIBridge::mirrorFinishAssistant(const std::string &sessionId, const std::string &messageId,
                                            int32_t inputTokens, int32_t outputTokens,
                                            int32_t reasoningTokens, int32_t totalTokens) -> void
    {
        std::scoped_lock lock(m_stateMutex);
        for (auto &m : m_sessions[sessionId].uiMessages)
            if (m.id == messageId)
            {
                m.status = MessageStatus::Completed;
                m.inputTokens = inputTokens;
                m.outputTokens = outputTokens;
                m.reasoningTokens = reasoningTokens;
                m.totalTokens = totalTokens;
            }
    }

} // namespace AE::TUI
