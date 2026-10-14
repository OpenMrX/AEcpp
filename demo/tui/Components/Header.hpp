/// @file demo/tui/Components/Header.hpp
/// @brief TUI 顶栏组件：应用品牌 + 会话切换下拉 + 新建会话 + 思考/意图开关 + 连接灯。
/// @details 模型切换已移至底部展开列表（由 TUI 层负责），不在顶栏显示模型徽章。
///          继承 ftxui::ComponentBase：OnRender 合成静态部分与交互容器，OnEvent 委托容器，
///          ChildAt/ActiveChild 把交互容器暴露给 FTXUI 焦点与鼠标命中测试。
/// @note 数据 setter 仅置脏标记，实体重建延后到帧边界 syncComponent()（同 Sidebar 模式），
///       避免在事件回调遍历组件树途中销毁子组件导致野指针崩溃。requires: C++23。
#pragma once

#include "../TUIType.hpp"
#include "../Core/Theme.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <functional>
#include <memory>
#include <string>

namespace AE::TUI
{

    class HeaderComponent final : public ftxui::ComponentBase,
                                    public std::enable_shared_from_this<HeaderComponent>
    {
    public:
        using NewSessionCallback = std::function<void()>;
        using SessionSelectCallback = std::function<void(const std::string &sessionId)>;
        using DeleteSessionCallback = std::function<void(const std::string &sessionId)>;
        using ThinkingCallback = std::function<void(bool /*enabled*/)>;
        using IntentCallback = std::function<void(bool /*enabled*/)>;

        explicit HeaderComponent(std::shared_ptr<ThemeManager> theme);

        auto getComponent() -> ftxui::Component { return shared_from_this(); }

        auto setTitle(const std::string &title) -> void;
        auto setSubtitle(const std::string &subtitle) -> void;
        auto setConnected(bool connected) -> void;
        auto setThinking(bool enabled) -> void;
        auto setIntent(bool enabled) -> void;
        auto setSessions(const SessionList &sessions) -> void;
        auto setActiveSession(const std::string &sessionId) -> void;

        auto onNewSession(NewSessionCallback callback) -> void;
        auto onSessionSelect(SessionSelectCallback callback) -> void;
        auto onDeleteSession(DeleteSessionCallback callback) -> void;
        auto onThinkingToggle(ThinkingCallback callback) -> void;
        auto onIntentToggle(IntentCallback callback) -> void;

        auto syncComponent() -> void;

    private:
        auto OnRender() -> ftxui::Element override;
        auto OnEvent(ftxui::Event event) -> bool override;
        auto ActiveChild() -> ftxui::Component override;

        auto rebuildContainer() -> void;
        auto activeSessionTitle() const -> std::string;
        auto toggleDropdown() -> void;
        auto selectSession(const std::string &sessionId) -> void;

        std::shared_ptr<ThemeManager> m_theme;
        std::string m_title = "AE TUI";
        std::string m_subtitle;
        bool m_connected = true;
        bool m_thinking = true;
        bool m_intent = false;
        SessionList m_sessions;
        std::string m_activeSessionId;
        bool m_dropdownOpen = false;
        bool m_dirty = true;

        // 顶栏交互容器：关闭态为 Horizontal(会话切换/新建/模型/思考/意图)，打开态为
        // Vertical(上述顶栏 + 会话列表 + 删除)，由 rebuildContainer 整体替换。
        ftxui::Component m_container;
        // 思考开关持久组件：setThinking 直接更新其状态，不参与 rebuild（避免失焦）。
        std::shared_ptr<class ToggleButtonComponent> m_thinkToggle;
        // 意图分析开关持久组件：setIntent 直接更新其状态，不参与 rebuild（避免失焦）。
        std::shared_ptr<class ToggleButtonComponent> m_intentToggle;

        NewSessionCallback m_newSessionCb;
        SessionSelectCallback m_sessionSelectCb;
        DeleteSessionCallback m_deleteSessionCb;
        ThinkingCallback m_thinkingCb;
        IntentCallback m_intentCb;
    };

} // namespace AE::TUI