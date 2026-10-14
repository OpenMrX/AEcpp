#include "Header.hpp"
#include "ToggleButton.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace AE::TUI
{

    HeaderComponent::HeaderComponent(std::shared_ptr<ThemeManager> theme)
        : m_theme(std::move(theme))
    {
        // Phase I: 快捷键标签直接显示在开关旁（Ctrl+T / Ctrl+I），
        // 底部状态栏不再重复显示这两个快捷键。
        m_thinkToggle = std::make_shared<ToggleButtonComponent>(
            "思考ON", "思考OFF", m_thinking,
            [this](bool enabled)
            {
                m_thinking = enabled;
                if (m_thinkingCb)
                    m_thinkingCb(enabled);
            }, m_theme);
        m_intentToggle = std::make_shared<ToggleButtonComponent>(
            "意图ON", "意图OFF", m_intent,
            [this](bool enabled)
            {
                m_intent = enabled;
                if (m_intentCb)
                    m_intentCb(enabled);
            }, m_theme);
        rebuildContainer();
    }

    auto HeaderComponent::setTitle(const std::string &title) -> void
    {
        m_title = title;
        m_dirty = true;
    }

    auto HeaderComponent::setSubtitle(const std::string &subtitle) -> void
    {
        m_subtitle = subtitle;
        m_dirty = true;
    }

    auto HeaderComponent::setConnected(bool connected) -> void
    {
        m_connected = connected;
        m_dirty = true;
    }

    auto HeaderComponent::setThinking(bool enabled) -> void
    {
        m_thinking = enabled;
        if (m_thinkToggle)
        {
            m_thinkToggle->setState(enabled);
        }
    }

    auto HeaderComponent::setIntent(bool enabled) -> void
    {
        m_intent = enabled;
        if (m_intentToggle)
        {
            m_intentToggle->setState(enabled);
        }
    }

    auto HeaderComponent::setSessions(const SessionList &sessions) -> void
    {
        m_sessions = sessions;
        m_dirty = true;
    }

    auto HeaderComponent::setActiveSession(const std::string &sessionId) -> void
    {
        m_activeSessionId = sessionId;
        m_dirty = true;
    }

    auto HeaderComponent::onNewSession(NewSessionCallback callback) -> void
    {
        m_newSessionCb = std::move(callback);
    }

    auto HeaderComponent::onSessionSelect(SessionSelectCallback callback) -> void
    {
        m_sessionSelectCb = std::move(callback);
    }

    auto HeaderComponent::onDeleteSession(DeleteSessionCallback callback) -> void
    {
        m_deleteSessionCb = std::move(callback);
    }

    auto HeaderComponent::onThinkingToggle(ThinkingCallback callback) -> void
    {
        m_thinkingCb = std::move(callback);
    }

    auto HeaderComponent::onIntentToggle(IntentCallback callback) -> void
    {
        m_intentCb = std::move(callback);
    }

    auto HeaderComponent::syncComponent() -> void
    {
        if (!m_dirty)
        {
            return;
        }
        rebuildContainer();
        m_dirty = false;
    }

    auto HeaderComponent::activeSessionTitle() const -> std::string
    {
        for (const auto &s : m_sessions)
        {
            if (s.id == m_activeSessionId)
            {
                return s.title.empty() ? "(untitled)" : s.title;
            }
        }
        return m_activeSessionId.empty() ? "(选择会话)" : "(Session)";
    }

    auto HeaderComponent::toggleDropdown() -> void
    {
        m_dropdownOpen = !m_dropdownOpen;
        m_dirty = true;
    }

    auto HeaderComponent::selectSession(const std::string &sessionId) -> void
    {
        m_dropdownOpen = false;
        m_dirty = true;
        if (m_sessionSelectCb)
        {
            m_sessionSelectCb(sessionId);
        }
    }

    auto HeaderComponent::rebuildContainer() -> void
    {
        const auto &tc = m_theme ? m_theme->colors() : ThemeManager{}.colors();

        auto makeStyledButton = [this](const std::string &label, auto action) -> ftxui::Component {
            ftxui::ButtonOption opt;
            opt.transform = [this, label](const ftxui::EntryState &state) -> ftxui::Element {
                const auto &c = m_theme ? m_theme->colors() : ThemeManager{}.colors();
                auto elem = ftxui::text(label) | ftxui::color(state.focused ? c.accent : c.foreground);
                if (state.focused) elem = elem | ftxui::underlined;
                return elem;
            };
            return ftxui::Button(label, std::move(action), opt);
        };

        std::string sessionLabel = "▼ " + activeSessionTitle();
        auto sessionToggle = makeStyledButton(sessionLabel, [this]
                                                { toggleDropdown(); });
        auto newBtn = makeStyledButton("NEW Session", [this]
                                       {
            if (m_newSessionCb)
                m_newSessionCb(); });
        // Phase I: 思考/意图开关直接作为 Component 加入容器（事件可正常传递）。
        // 两者间用较窄的 spacer 留出紧凑间距（加宽在 ToggleButton 内部通过 label 前后空格实现）。
        auto intentGap = ftxui::Renderer([] { return ftxui::text("  "); });

        // 思考/意图整体右对齐：在 ＋新会话 与 思考 之间用 filler 把右侧簇推到最右（连接灯左侧）。
        // 注意：Container::Horizontal 只接受 ftxui::Component，filler() 返回的是 Element，
        // 必须包成 Renderer 组件并用 flex 吸收剩余宽度，否则 MSVC 报“非法参数”编译错误。
        auto rightPush = ftxui::Renderer([] { return ftxui::filler() | ftxui::flex; });

        // 会话切换与新建按钮之间加间距，避免紧贴。
        auto btnGap = ftxui::Renderer([] { return ftxui::text("  "); });

        auto bar = ftxui::Container::Horizontal({
            newBtn,
            btnGap,
            sessionToggle,
            rightPush,
            m_thinkToggle,
            intentGap,
            m_intentToggle,
        });

        if (!m_dropdownOpen)
        {
            m_container = bar;
            return;
        }

        std::vector<ftxui::Component> entries;
        entries.reserve(m_sessions.size() * 2 + 2);
        entries.push_back(bar);
        entries.push_back(ftxui::Renderer([] { return ftxui::separator(); }));

        // 下拉列表：会话标题（点击切换）+ 删除按钮。只展示前若干条，避免超高终端溢出。
        const std::size_t cap = 8;
        const std::size_t shown = (std::min)(m_sessions.size(), cap);
        for (std::size_t i = 0; i < shown; ++i)
        {
            const auto &s = m_sessions[i];
            auto titleBtn = makeStyledButton(s.title.empty() ? "(untitled)" : s.title,
                                             [this, id = s.id]
                                             { selectSession(id); });
            auto delBtn = makeStyledButton("[x]", [this, id = s.id]
                                           {
                if (m_deleteSessionCb)
                    m_deleteSessionCb(id); });
            entries.push_back(ftxui::Container::Horizontal({ titleBtn, delBtn }));
        }
        if (m_sessions.size() > cap)
        {
            entries.push_back(ftxui::Renderer([&]
                                              { return ftxui::text("  … 更多会话 ") | ftxui::dim; }));
        }

        m_container = ftxui::Container::Vertical(std::move(entries));
    }

    auto HeaderComponent::OnRender() -> ftxui::Element
    {
        const ThemeColors &tc = m_theme->colors();

        auto connected = m_connected
                             ? ftxui::text("● 已连接") | ftxui::color(tc.success)
                             : ftxui::text("● 未连接") | ftxui::color(tc.error);

        auto brand = ftxui::hbox({
            ftxui::text(m_title) | ftxui::bold | ftxui::color(tc.accent),
            m_subtitle.empty()
                ? ftxui::text("")
                : ftxui::hbox({ ftxui::separatorEmpty(), ftxui::text(m_subtitle) | ftxui::dim }),
        });

        auto interactive = m_container ? m_container->Render() : ftxui::text("");
        auto row = ftxui::hbox({
            brand,
            ftxui::text("     "),  // 增大标题与 会话/新会话 的间距
            interactive | ftxui::flex,
            ftxui::separatorEmpty(),
            connected,
        });

        auto result = ftxui::window(ftxui::text(""), row);
        if (!m_dropdownOpen)
        {
            result = result | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 3);
        }
        return result;
    }

    auto HeaderComponent::OnEvent(ftxui::Event event) -> bool
    {
        if (m_container && m_container->OnEvent(std::move(event)))
        {
            return true;
        }
        return false;
    }

    auto HeaderComponent::ActiveChild() -> ftxui::Component
    {
        return m_container;
    }

} // namespace AE::TUI