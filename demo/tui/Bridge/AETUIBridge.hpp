#pragma once

/// @file demo/tui/Bridge/AETUIBridge.hpp
/// @brief AE ↔ TUI 接线桥：把 AE::Engine 引擎/客户端的流式产出映射到 ITUI 视图契约。
/// @details 双模式（启动时由配置决定）：
///          - Chat 模式：StreamingLLMClient::stream_chat_gen 单轮对话，SSE delta 直推
///            updateStreamingMessage / setThinkingContent / setMessageTokens。
///          - Agent 模式：AgentEngine::run 完整循环（多轮工具调用），AgentEvent 的
///            ToolCall/ToolResult 映射为可折叠 tool 角色消息，Token/Final 走流式气泡。
///
///          线程不变量（与 ITUI 契约一致）：worker 线程只调用 ITUI 数据方法
///          （内部仅置原子重绘标志），绝不触碰 ftxui screen。每请求一个 std::jthread，
///          m_processing 原子守卫防并发；析构自动 request_stop + join，防 use-after-free。
///
/// @note 命名空间：AE::TUI（视图侧），依赖 AE::Engine 引擎侧类型。

#include "ITUI.hpp"
#include "TUIType.hpp"

#include "Core/Result.hpp"
#include "nlohmann-json.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace AE::TUI
{

    /// 桥接层运行配置（来源优先级：环境变量 > demo_config.json > 内置默认）。
    struct BridgeConfig
    {
        std::string endpoint;                  ///< OpenAI 兼容 /chat/completions 端点
        std::string apiKey;                    ///< Bearer Key（空 → setConnected(false) + 提示）
        std::string model;                     ///< 当前模型名
        bool agentMode = false;                ///< true=Agent 循环，false=单轮 Chat
        std::vector<std::string> modelList;    ///< TUI 底部可选模型列表
    };

    class AETUIBridge final
    {
    public:
        explicit AETUIBridge(std::shared_ptr<ITUI> tui);
        ~AETUIBridge();

        AETUIBridge(const AETUIBridge &) = delete;
        auto operator=(const AETUIBridge &) -> AETUIBridge & = delete;

        /// 读取配置：环境变量 VXAE_ENDPOINT/VXAE_API_KEY/VXAE_MODEL/VXAE_AGENT_MODE
        /// > exe 同目录 demo_config.json（可选）> 内置占位默认。
        static auto loadConfig() -> BridgeConfig;
        /// 把运行配置写回 exe 同目录 demo_config.json（模型配置页"保存并应用"时
        /// 按需持久化）。尽力而为：写盘失败仅返回 false，不影响调用方热应用。
        static auto saveConfig(const BridgeConfig &cfg) -> bool;
        /// 是否已配置模型列表：环境变量 VXAE_MODEL 非空，或 exe 同目录
        /// demo_config.json 含非空 modelList / model 字段。demo 启动时据此决定
        /// 是否自动弹出模型配置页（无配置时引导用户先配模型，见 main.cpp）。
        static auto hasConfiguredModelList() -> bool;
        /// 端点归一化：以 /v1 或 /v1/ 结尾时补全 /chat/completions（StreamingLLMClient
        /// 直接把 endpoint 当完整请求 URL 用）。
        static auto normalizeEndpoint(const std::string &endpoint) -> std::string;

        /// 初始化（用 loadConfig() 解析配置）：推送模型列表/模型状态/连接灯。
        auto initialize() -> Core::Result<void>;
        /// 初始化（显式注入配置）。
        auto initialize(BridgeConfig cfg) -> Core::Result<void>;
        /// 关停：请求停止当前 worker 并 join。
        auto shutdown() -> Core::Result<void>;

        /// 处理一条用户输入：追加历史 → 起 worker 线程跑一轮请求。
        /// 上一条仍在处理时以系统消息提示并忽略。
        auto handleUserInput(std::string input, std::string sessionId) -> void;

        /// 请求停止当前生成（TUIEvent::StopRequested → 此处）。
        auto requestStop() -> void;
        /// 当前是否有请求在跑。
        auto isProcessing() const -> bool { return m_processing.load(); }

        /// 会话删除：清桥接层历史与 UI 镜像。
        auto clearHistory(const std::string &sessionId) -> void;
        /// 某会话的 UI 消息镜像（SwitchSession 时经 setMessages 回灌渲染）。
        auto messagesFor(const std::string &sessionId) const -> std::vector<Message>;

        /// 当前生效配置快照（欢迎打印/状态展示用）。
        auto config() const -> BridgeConfig
        {
            std::scoped_lock lock(m_cfgMutex);
            return m_config;
        }
        /// 模型切换（ModelSwitch 事件）：更新配置 + setModelStatus 联动。
        auto switchModel(const std::string &model) -> void;
        /// 热应用新配置（模型配置页保存后）：更新内部配置并联动模型列表/模型状态/
        /// 连接灯/状态栏（模式与端点）。m_tui 为空时仅更新内部配置。
        auto applyConfig(BridgeConfig cfg) -> void;
        /// 同步思考/意图开关状态（ToggleThinking/ToggleIntent 与启动恢复）。
        auto syncToggles(bool thinkingEnabled, bool intentEnabled) -> void;

    private:
        /// 单会话桥接状态：AE 侧对话历史（OpenAI messages 形态）+ TUI 消息镜像。
        struct SessionState
        {
            nlohmann::json history = nlohmann::json::array();
            std::vector<Message> uiMessages;
        };

        auto workerBody(BridgeConfig cfg, std::string sessionId,
                        std::string assistantId, nlohmann::json history) -> void;
        auto runChatTurn(const BridgeConfig &cfg, const std::string &sessionId,
                         const std::string &assistantId, const nlohmann::json &history)
            -> std::string;
        auto runAgentTurn(const BridgeConfig &cfg, const std::string &sessionId,
                          const std::string &assistantId, const nlohmann::json &history)
            -> std::string;

        auto stopRequested() const -> bool { return m_stopRequested.load(); }
        /// 系统消息气泡（worker 内调用，仅触碰 ITUI 数据方法）。
        auto postSystemMessage(const std::string &sessionId, const std::string &text) -> void;
        /// 更新 UI 镜像中指定消息（与 ITUI 推送同步，供 SwitchSession 回灌）。
        auto mirrorAppendAssistant(const std::string &sessionId, const std::string &messageId,
                                   const std::string &delta) -> void;
        auto mirrorAppendThinking(const std::string &sessionId, const std::string &messageId,
                                  const std::string &delta) -> void;
        auto mirrorSetAssistantContent(const std::string &sessionId, const std::string &messageId,
                                       const std::string &content) -> void;
        auto mirrorFinishAssistant(const std::string &sessionId, const std::string &messageId,
                                   int32_t inputTokens, int32_t outputTokens,
                                   int32_t reasoningTokens, int32_t totalTokens) -> void;
        auto mirrorAddMessage(const std::string &sessionId, const Message &message) -> void;

        std::shared_ptr<ITUI> m_tui;

        mutable std::mutex m_cfgMutex;
        BridgeConfig m_config;

        mutable std::mutex m_stateMutex;
        std::unordered_map<std::string, SessionState> m_sessions;

        std::atomic<bool> m_processing{false};
        std::atomic<bool> m_stopRequested{false};
        std::atomic<bool> m_thinkingEnabled{true};
        std::atomic<bool> m_intentEnabled{true};
        bool m_initialized{false};

        // 每请求一个 worker 线程；析构/shutdown 时 request_stop + join（jthread 语义）。
        std::jthread m_worker;
    };

} // namespace AE::TUI
