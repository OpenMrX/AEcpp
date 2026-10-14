#pragma once

/// @file demo/tui/TUI.hpp
/// @brief TUI 主实现：FTXUI 组件树、面板编排、渲染主循环（VXAE demo 移植版）。
/// @details 组合 Header/ChatPanel/InputBox/StatusBar 等组件，运行于
///          FTXUI `ScreenInteractive`；会话与消息数据全部经 `ITUI` 契约由桥接层推入。
///          demo 层为纯视图：会话管理为纯内存（无后端持久化），配置持久化到
///          可执行文件同目录 `demo_settings.json`（nlohmann 直接读写）。

#include "ITUI.hpp"
#include "TUIType.hpp"
#include "Core/Theme.hpp"
#include "Components/Header.hpp"
#include "Components/ChatPanel.hpp"
#include "Components/InputBox.hpp"
#include "Components/StatusBar.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AE::TUI
{

    class TUI final : public ITUI, public std::enable_shared_from_this<TUI>
    {
    public:
        TUI();
        ~TUI() override;

        auto initialize() -> Core::Result<void> override;
        auto shutdown() -> Core::Result<void> override;

        auto run() -> Core::Result<int> override;
        auto stop() -> void override;

        auto setConfig(const TUIConfig &config) -> Core::Result<void> override;
        auto getConfig() const -> const TUIConfig & override;

        auto addMessage(const std::string &sessionId, const Message &message) -> void override;
        auto setMessages(const std::string &sessionId,
                         std::vector<Message> messages) -> void override;
        auto updateStreamingMessage(const std::string &sessionId,
                                    const std::string &messageId,
                                    std::string_view delta) -> void override;
        auto setMessageContent(const std::string &sessionId,
                               const std::string &messageId,
                               std::string_view content) -> bool override;
        auto finishStreamingMessage(const std::string &sessionId,
                                    const std::string &messageId) -> void override;
        auto setThinkingContent(const std::string &sessionId,
                                const std::string &messageId,
                                std::string_view thinking) -> void override;
        auto setOrchestrationInfo(const std::string &sessionId,
                                  const std::string &messageId,
                                  OrchestrationInfo info) -> void override;
        auto setMessageTokens(const std::string &sessionId, const std::string &messageId,
                              int32_t inputTokens, int32_t outputTokens,
                              int32_t reasoningTokens, int32_t totalTokens) -> void override;
        auto setLastUserMessageTokens(const std::string &sessionId, int32_t inputTokens) -> void override;

        auto addSession(const SessionInfo &session) -> void override;
        auto removeSession(const std::string &sessionId) -> void override;
        auto switchSession(const std::string &sessionId) -> void override;
        auto getCurrentSessionId() const -> std::string override;

        auto setStatusItems(const std::vector<StatusItem> &items) -> void override;

        auto setModelStatus(const std::string &model,
                            bool thinkingEnabled,
                            bool intentEnabled) -> void override;
        // 可用模型列表（供底部展开列表选择）。
        auto setModelList(const std::vector<std::string> &models) -> void override;
        /// 同步连接状态到 Header 连接灯 + 状态栏连接灯。
        auto setConnected(bool connected) -> void override;
        auto setTools(const ToolList &tools) -> void override;

        auto registerEventHandler(EventCallback callback) -> void override;
        auto unregisterAllEventHandlers() -> void override;

        auto isRunning() const -> bool override;

        // ── 配置持久化（demo_settings.json：主题/思考/意图/默认模型 4 项）──
        auto persistTUIConfig() -> void override;
        auto persistedTUIConfig(ThemeVariant &theme,
                                std::string &modelName,
                                bool &thinking,
                                bool &intent) -> void override;
        auto setDefaultModel(std::string modelName) -> void override;

    private:
        auto buildLayout() -> ftxui::Component;
        auto handleKeyEvent(const ftxui::Event &event) -> bool;
        auto emitEvent(const TUIEvent &event) -> void;
        auto onInputSubmit(const std::string &text) -> void;
        auto getOrCreateChatPanel(const std::string &sessionId) -> std::shared_ptr<ChatPanel>;
        auto getOrCreateChatPanelLocked(const std::string &sessionId) -> std::shared_ptr<ChatPanel>;
        // 仅置位重绘请求标志，不直接触碰 ftxui screen —— 由唯一 throttle 线程统一驱动重绘，
        // 避免业务/worker 线程与 throttle 线程并发调用 ftxui Post 引发 resource deadlock / 闪烁。
        auto postCustomEvent() -> void;
        // 合并 setStatusItems 提供的基础项与 setModelStatus 写入的模型项，统一写入状态栏。
        auto refreshStatusBar() -> void;
        // 底部模型展开列表：同步当前选中项、打开/选择逻辑。
        auto syncModelListSelection() -> void;
        auto openModelList() -> void;
        // Ctrl+T：主题样式轮换 + 同步持久化（demo_settings.json）。
        auto cycleTheme() -> void;
        // 从 demo_settings.json 恢复上次保存的主题（默认 Dark，供 initialize 早期应用）。
        auto loadPersistedTheme() -> ThemeVariant;
        // 纯内存会话：启动时无会话则创建一个默认会话。
        auto ensureDefaultSession() -> void;
        // 刷新 Header 会话列表与活动态（须在释放 m_stateMutex 后调用，避免非递归锁重入）。
        auto refreshSessionViews() -> void;

        std::shared_ptr<ThemeManager> m_themeManager;
        std::shared_ptr<HeaderComponent> m_header;
        std::shared_ptr<InputBox> m_inputBox;
        std::shared_ptr<StatusBarComponent> m_statusBar;
        // 当前会话的工具列表（demo 层仅保留数据，不渲染）。
        ToolList m_tools;

        std::unordered_map<std::string, std::shared_ptr<ChatPanel>> m_chatPanels;
        std::string m_currentSessionId;
        // 纯内存会话表（替代原 SQLite 后端）：addSession/removeSession 直接增删，
        // Header 侧栏由此渲染；消息历史由桥接层经 setMessages 推入。
        SessionList m_sessions;
        // 当前活动会话面板（供 throttle 线程查询 isStreaming 做自适应帧率）。
        // 仅作弱引用，不拥有所有权；ChatPanel 由 m_chatPanels 持有，生命周期稳定。
        std::atomic<ChatPanel *> m_activeChatPanel{nullptr};

        // 状态栏合并缓存：基础项（setStatusItems，含 setModelStatus 写入的模型项）。
        std::vector<StatusItem> m_statusBaseItems;
        // 帮助覆盖层可见状态（Ctrl+H 切换）。
        bool m_helpVisible{false};
        // 底部模型展开列表覆盖层状态（点击状态栏模型项打开，方向键导航，Enter 确认，Esc 取消）。
        bool m_modelListVisible{false};
        // 删除会话确认对话框：点击会话删除按钮后先弹确认，确认才真正删除（纯内存）。
        bool m_deleteConfirmVisible{false};
        std::string m_pendingDeleteSessionId;
        std::size_t m_modelListIndex{0};
        std::vector<std::string> m_modelList;
        // 当前已选模型（由 setModelStatus 缓存，用于列表高亮“当前”项）。
        std::string m_currentModel;

        TUIConfig m_config;
        std::atomic<bool> m_initialized{false};
        std::atomic<bool> m_running{false};
        // 思考模式开关的当前 UI 状态（由 setModelStatus 同步，供开关按钮初始化）。
        bool m_thinkingEnabled{true};
        // 意图分析开关的当前 UI 状态（由 setModelStatus 同步，供开关按钮初始化）。
        bool m_intentEnabled{true};

        std::vector<EventCallback> m_eventHandlers;
        mutable std::mutex m_eventMutex;
        mutable std::mutex m_stateMutex;

        std::atomic<ftxui::ScreenInteractive *> m_screenPtr{nullptr};
        ftxui::Component m_mainContainer;

        // 重绘请求标志：worker 线程更新数据后置位，单一 throttle 线程轮询并驱动一次 screen.Post。
        // 关键不变量：业务代码绝不从 worker 线程直接调用 ftxui 的 Post/PostEvent。
        std::atomic<bool> m_frameRequested{true};
    };

} // namespace AE::TUI
