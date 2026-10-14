/// @file demo/tui/Components/ToggleButton.hpp
/// @brief 底部状态栏开关按钮（思考模式切换）。
/// @details 内部委托 ftxui::Button 实现：Button 渲染时记录自身 box 区域，
///          鼠标事件仅落在按钮区域内才触发 on_click，避免点击聊天区/状态栏
///          其他位置时误切换思考模式。requires: C++23。
#pragma once

#include <functional>
#include <string>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>

#include "../Core/Theme.hpp"

namespace AE::TUI {

class ToggleButtonComponent final : public ftxui::ComponentBase {
public:
    using Callback = std::function<void(bool /*newState*/)>;

    ToggleButtonComponent(std::string on_label, std::string off_label,
                          bool initial, Callback on_change,
                          std::shared_ptr<ThemeManager> theme = nullptr)
        : m_onLabel(std::move(on_label)),
          m_offLabel(std::move(off_label)),
          m_state(initial),
          m_onChange(std::move(on_change)),
          m_theme(std::move(theme))
    {
        updateLabel();
        ftxui::ButtonOption opt;
        // 绑定到持久成员的指针，运行时修改 m_label 即可刷新显示。
        opt.label = &m_label;
        opt.on_click = [this] { toggle(); };
        opt.transform = [this](const ftxui::EntryState& state) -> ftxui::Element {
            // 仅文字，不填充背景，避免与顶栏背景形成异色块/异常阴影。
            const ThemeColors& tc = m_theme ? m_theme->colors() : loadDarkThemeFallback();
            // 开启：绿色（success）；关闭：灰色（muted）。
            ftxui::Color fg = m_state ? tc.success : tc.muted;
            // 鼠标悬停：加粗 + 提亮，提供明确的交互反馈（开启更亮绿、关闭更亮灰）。
            if (m_hovered)
            {
                fg = m_state ? ftxui::Color::Green3 : ftxui::Color::Grey66;
            }
            // 紧凑显示：保留最小内边距，减少顶栏右侧占用。
            ftxui::Element elem = ftxui::hbox({
                ftxui::text(" ") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 1),
                ftxui::text(state.label) | ftxui::color(fg),
                ftxui::text(" ") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 1),
            });
            // 仅在悬停时加粗以体现"悬停变化效果"，不添加下划线。
            if (m_hovered)
            {
                elem = elem | ftxui::bold;
            }
            return elem;
        };
        m_button = ftxui::Button(opt);
    }

    // 无主题时的安全回退（仅取颜色，不依赖 ThemeManager 单例）。
    static auto loadDarkThemeFallback() -> ThemeColors
    {
        ThemeColors c;
        c.primary = ftxui::Color::DeepSkyBlue1;
        c.muted = ftxui::Color::Grey50;
        return c;
    }

    auto setState(bool v) -> void
    {
        m_state = v;
        updateLabel();
    }

    [[nodiscard]] auto state() const -> bool { return m_state; }

    auto OnEvent(ftxui::Event event) -> bool override
    {
        if (event == ftxui::Event::Return || event == ftxui::Event::Character(' '))
        {
            toggle();
            TakeFocus();
            return true;
        }
        // 鼠标移动：根据按钮渲染区域（box）判断是否悬停，用于悬停高亮反馈。
        if (event.is_mouse())
        {
            const auto& mouse = event.mouse();
            if (mouse.motion == ftxui::Mouse::Moved)
            {
                const bool nowHovered = m_box.Contain(mouse.x, mouse.y);
                if (nowHovered != m_hovered)
                {
                    m_hovered = nowHovered;
                }
                // 无论是否悬停都交还 Button 处理（它也会消费移动事件）。
            }
        }
        // 鼠标点击交给 ftxui::Button，其内部分区域处理，不会误触。
        return m_button->OnEvent(std::move(event));
    }

    auto OnRender() -> ftxui::Element override
    {
        // reflect 把按钮的渲染区域记录到 m_box，供 OnEvent 做悬停检测。
        return m_button->Render() | ftxui::reflect(m_box);
    }

    auto ActiveChild() -> ftxui::Component override
    {
        return m_button;
    }

private:
    auto toggle() -> void
    {
        m_state = !m_state;
        updateLabel();
        if (m_onChange)
            m_onChange(m_state);
    }

    auto updateLabel() -> void
    {
        m_label = m_state ? m_onLabel : m_offLabel;
    }

    std::string m_onLabel;
    std::string m_offLabel;
    std::string m_label;
    bool m_state = false;
    bool m_hovered = false;
    ftxui::Box m_box;
    Callback m_onChange;
    std::shared_ptr<ThemeManager> m_theme;
    ftxui::Component m_button;
};

} // namespace AE::TUI
