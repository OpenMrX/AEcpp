#pragma once

/// @file demo/tui/ITUI.hpp
/// @brief TUI 抽象接口：渲染/输入/会话绑定/面板管理的最小契约（VXAE demo 移植版）。
/// @details 实现见 `TUI.hpp`；demo 层为纯视图契约，不注入任何后端/引擎门面类型，
///          会话与消息数据全部经本接口由桥接层推入。

#include "TUIType.hpp"

#include <memory>
#include <string>
#include <vector>

namespace AE::TUI
{

    class ITUI
    {
    public:
        virtual ~ITUI() = default;

        virtual auto initialize() -> Core::Result<void> = 0;
        virtual auto shutdown() -> Core::Result<void> = 0;

        virtual auto run() -> Core::Result<int> = 0;
        virtual auto stop() -> void = 0;

        virtual auto setConfig(const TUIConfig &config) -> Core::Result<void> = 0;
        virtual auto getConfig() const -> const TUIConfig & = 0;

        virtual auto addMessage(const std::string &sessionId, const Message &message) -> void = 0;
        // §8.7：整体替换指定会话的消息列表（切换会话时从业务层回读历史后渲染）。
        virtual auto setMessages(const std::string &sessionId,
                                 std::vector<Message> messages) -> void = 0;
        virtual auto updateStreamingMessage(const std::string &sessionId,
                                            const std::string &messageId,
                                            std::string_view delta) -> void = 0;
        // 关键接口：直接覆盖指定消息的内容（替换而非追加），用于工具结果回填、最终答案写入等场景
        virtual auto setMessageContent(const std::string &sessionId,
                                       const std::string &messageId,
                                       std::string_view content) -> bool = 0;
        virtual auto finishStreamingMessage(const std::string &sessionId,
                                            const std::string &messageId) -> void = 0;
        // Phase I: 设置正文消息附带的思考内容（reasoning_content 合并到正文消息的
        // thinkingContent 字段，渲染为可折叠思考块；不创建独立思考气泡）。
        virtual auto setThinkingContent(const std::string &sessionId,
                                        const std::string &messageId,
                                        std::string_view thinking) -> void = 0;
        // 阶段4: 回填正文消息的编排信息块（业务层广播聚合后折叠呈现）。
        virtual auto setOrchestrationInfo(const std::string &sessionId,
                                          const std::string &messageId,
                                          OrchestrationInfo info) -> void = 0;
        // 回填服务端真实 token（助手消息：input/output/reasoning/total；用户消息仅 input）。
        virtual auto setMessageTokens(const std::string &sessionId, const std::string &messageId,
                                      int32_t inputTokens, int32_t outputTokens,
                                      int32_t reasoningTokens, int32_t totalTokens) -> void = 0;
        // 回填最近一条用户消息的输入 token（相邻请求 prompt_tokens 差值）。
        virtual auto setLastUserMessageTokens(const std::string &sessionId, int32_t inputTokens) -> void = 0;

        virtual auto addSession(const SessionInfo &session) -> void = 0;
        virtual auto removeSession(const std::string &sessionId) -> void = 0;
        virtual auto switchSession(const std::string &sessionId) -> void = 0;
        virtual auto getCurrentSessionId() const -> std::string = 0;

        virtual auto setStatusItems(const std::vector<StatusItem> &items) -> void = 0;
        // 输入框底部模型状态栏：同步 Header 思考/意图开关与状态栏模型徽章。
        virtual auto setModelStatus(const std::string &model,
                                    bool thinkingEnabled,
                                    bool intentEnabled) -> void = 0;
        // 可用模型列表（供 TUI 底部展开列表选择；顶栏徽章/轮换已移除）。
        virtual auto setModelList(const std::vector<std::string> &models) -> void = 0;
        /// 同步连接状态到 Header 连接灯 + 状态栏连接灯。
        virtual auto setConnected(bool connected) -> void = 0;
        virtual auto setTools(const ToolList &tools) -> void = 0;

        virtual auto registerEventHandler(EventCallback callback) -> void = 0;
        virtual auto unregisterAllEventHandlers() -> void = 0;

        virtual auto isRunning() const -> bool = 0;

        // ── 配置持久化（可执行文件同目录 demo_settings.json，nlohmann 直接读写）──
        // 保存当前 TUI 配置（主题/思考模式/意图模式/默认模型名）。
        virtual auto persistTUIConfig() -> void = 0;
        // 读取持久化的 TUI 配置（供桥接层启动时恢复主题/思考/意图/默认模型）。
        // 输出参数为各持久化值的当前值；无持久化时保持入参不变。
        virtual auto persistedTUIConfig(ThemeVariant &theme,
                                        std::string &modelName,
                                        bool &thinking,
                                        bool &intent) -> void = 0;
        // 更新 TUI 侧默认模型名（模型切换时调用，并同步持久化）。
        virtual auto setDefaultModel(std::string modelName) -> void = 0;
    };

} // namespace AE::TUI
