#include "Theme.hpp"

namespace AE::TUI
{

    ThemeManager::ThemeManager()
    {
        m_colors = loadDarkTheme();
    }

    auto ThemeManager::setTheme(ThemeVariant variant) -> void
    {
        m_currentTheme = variant;
        switch (variant)
        {
        case ThemeVariant::Dark:
            m_colors = loadDarkTheme();
            break;
        case ThemeVariant::Light:
            m_colors = loadLightTheme();
            break;
        case ThemeVariant::Dracula:
            m_colors = loadDraculaTheme();
            break;
        case ThemeVariant::Monokai:
            m_colors = loadMonokaiTheme();
            break;
        case ThemeVariant::Solarized:
            m_colors = loadSolarizedTheme();
            break;
        default:
            m_colors = loadDarkTheme();
            break;
        }
    }

    auto ThemeManager::getCurrentTheme() const -> ThemeVariant
    {
        return m_currentTheme;
    }

    auto ThemeManager::colors() const -> const ThemeColors &
    {
        return m_colors;
    }

    auto ThemeManager::applyHeaderStyle(ftxui::Element element) const -> ftxui::Element
    {
        return element | ftxui::bgcolor(m_colors.headerBg) | ftxui::color(m_colors.headerFg) | ftxui::bold;
    }

    auto ThemeManager::applySidebarStyle(ftxui::Element element) const -> ftxui::Element
    {
        return element | ftxui::bgcolor(m_colors.sidebarBg) | ftxui::color(m_colors.sidebarFg);
    }

    auto ThemeManager::applyUserBubbleStyle(ftxui::Element element) const -> ftxui::Element
    {
        // 仅边框 + 文字，不再填充背景色。
        return ftxui::borderRounded(element | ftxui::color(m_colors.foreground)) |
               ftxui::color(m_colors.border);
    }

    auto ThemeManager::applyAssistantBubbleStyle(ftxui::Element element) const -> ftxui::Element
    {
        // 仅边框 + 文字，不再填充背景色。
        return ftxui::borderRounded(element | ftxui::color(m_colors.foreground)) |
               ftxui::color(m_colors.border);
    }

    auto ThemeManager::applySystemBubbleStyle(ftxui::Element element) const -> ftxui::Element
    {
        // 仅边框 + 文字，不再填充背景色。
        return ftxui::borderDashed(element | ftxui::color(m_colors.muted)) |
               ftxui::color(m_colors.border);
    }

    auto ThemeManager::applyInputStyle(ftxui::Element element) const -> ftxui::Element
    {
        return ftxui::borderRounded(
                   element | ftxui::bgcolor(m_colors.inputBg) | ftxui::color(m_colors.inputFg)) |
               ftxui::color(m_colors.primary);
    }

    auto ThemeManager::applyStatusBarStyle(ftxui::Element element) const -> ftxui::Element
    {
        return element | ftxui::bgcolor(m_colors.statusBg) | ftxui::color(m_colors.statusFg);
    }

    auto ThemeManager::applyBorderStyle(ftxui::Element element) const -> ftxui::Element
    {
        return ftxui::border(element) | ftxui::color(m_colors.border);
    }

    auto ThemeManager::applyFocusedStyle(ftxui::Element element) const -> ftxui::Element
    {
        return ftxui::focus(element) | ftxui::color(m_colors.primary);
    }

    auto ThemeManager::loadDarkTheme() -> ThemeColors
    {
        ThemeColors c;
        c.background = ftxui::Color::Default;
        c.foreground = ftxui::Color::GrayLight;
        c.primary = ftxui::Color::DeepSkyBlue1;
        c.secondary = ftxui::Color::Grey62;
        c.accent = ftxui::Color::Magenta;
        c.muted = ftxui::Color::Grey50;
        c.border = ftxui::Color::Grey30;
        c.userBubble = ftxui::Color::DeepSkyBlue4;
        c.assistantBubble = ftxui::Color::Grey23;
        c.systemBubble = ftxui::Color::Grey15;
        c.sidebarBg = ftxui::Color::Grey11;
        c.sidebarFg = ftxui::Color::GrayLight;
        c.headerBg = ftxui::Color::Grey23;
        c.headerFg = ftxui::Color::White;
        c.statusBg = ftxui::Color::Grey15;
        c.statusFg = ftxui::Color::GrayLight;
        c.inputBg = ftxui::Color::Grey11;
        c.inputFg = ftxui::Color::White;
        c.error = ftxui::Color::Red;
        c.warning = ftxui::Color::Yellow;
        c.success = ftxui::Color::Green;
        c.info = ftxui::Color::Cyan;
        return c;
    }

    auto ThemeManager::loadLightTheme() -> ThemeColors
    {
        ThemeColors c;
        c.background = ftxui::Color::White;
        c.foreground = ftxui::Color::Grey30;
        c.primary = ftxui::Color::DeepSkyBlue3;
        c.secondary = ftxui::Color::Grey62;
        c.accent = ftxui::Color::Magenta;
        c.muted = ftxui::Color::Grey50;
        c.border = ftxui::Color::Grey70;
        c.userBubble = ftxui::Color::DeepSkyBlue2;
        c.assistantBubble = ftxui::Color::Grey93;
        c.systemBubble = ftxui::Color::Grey100;
        c.sidebarBg = ftxui::Color::Grey100;
        c.sidebarFg = ftxui::Color::Grey30;
        c.headerBg = ftxui::Color::Grey89;
        c.headerFg = ftxui::Color::Black;
        c.statusBg = ftxui::Color::Grey93;
        c.statusFg = ftxui::Color::Grey42;
        c.inputBg = ftxui::Color::White;
        c.inputFg = ftxui::Color::Black;
        c.error = ftxui::Color::Red;
        c.warning = ftxui::Color::Yellow;
        c.success = ftxui::Color::Green;
        c.info = ftxui::Color::Cyan;
        return c;
    }

    auto ThemeManager::loadDraculaTheme() -> ThemeColors
    {
        ThemeColors c;
        c.background = ftxui::Color::Default;
        c.foreground = ftxui::Color::GrayLight;
        c.primary = ftxui::Color::Cyan;
        c.secondary = ftxui::Color::Grey62;
        c.accent = ftxui::Color::HotPink;
        c.muted = ftxui::Color::Grey50;
        c.border = ftxui::Color::Grey30;
        c.userBubble = ftxui::Color::Purple4;
        c.assistantBubble = ftxui::Color::Grey23;
        c.systemBubble = ftxui::Color::Grey15;
        c.sidebarBg = ftxui::Color::Grey11;
        c.sidebarFg = ftxui::Color::GrayLight;
        c.headerBg = ftxui::Color::Grey23;
        c.headerFg = ftxui::Color::Cyan;
        c.statusBg = ftxui::Color::Grey15;
        c.statusFg = ftxui::Color::Green;
        c.inputBg = ftxui::Color::Grey11;
        c.inputFg = ftxui::Color::White;
        c.error = ftxui::Color::Red;
        c.warning = ftxui::Color::Yellow;
        c.success = ftxui::Color::Green;
        c.info = ftxui::Color::Cyan;
        return c;
    }

    auto ThemeManager::loadMonokaiTheme() -> ThemeColors
    {
        ThemeColors c;
        c.background = ftxui::Color::Default;
        c.foreground = ftxui::Color::GrayLight;
        c.primary = ftxui::Color::Green;
        c.secondary = ftxui::Color::Grey62;
        c.accent = ftxui::Color::Magenta;
        c.muted = ftxui::Color::Grey50;
        c.border = ftxui::Color::Grey30;
        c.userBubble = ftxui::Color::Green;
        c.assistantBubble = ftxui::Color::Grey23;
        c.systemBubble = ftxui::Color::Grey15;
        c.sidebarBg = ftxui::Color::Grey11;
        c.sidebarFg = ftxui::Color::GrayLight;
        c.headerBg = ftxui::Color::Grey23;
        c.headerFg = ftxui::Color::Yellow;
        c.statusBg = ftxui::Color::Grey15;
        c.statusFg = ftxui::Color::DarkOrange;
        c.inputBg = ftxui::Color::Grey11;
        c.inputFg = ftxui::Color::White;
        c.error = ftxui::Color::Red;
        c.warning = ftxui::Color::Yellow;
        c.success = ftxui::Color::Green;
        c.info = ftxui::Color::Cyan;
        return c;
    }

    auto ThemeManager::loadSolarizedTheme() -> ThemeColors
    {
        ThemeColors c;
        c.background = ftxui::Color::Default;
        c.foreground = ftxui::Color::Grey62;
        c.primary = ftxui::Color::Blue;
        c.secondary = ftxui::Color::Grey50;
        c.accent = ftxui::Color::DarkOrange;
        c.muted = ftxui::Color::Grey42;
        c.border = ftxui::Color::Grey70;
        c.userBubble = ftxui::Color::Blue;
        c.assistantBubble = ftxui::Color::Grey93;
        c.systemBubble = ftxui::Color::Grey100;
        c.sidebarBg = ftxui::Color::Grey100;
        c.sidebarFg = ftxui::Color::Grey62;
        c.headerBg = ftxui::Color::Grey93;
        c.headerFg = ftxui::Color::Blue;
        c.statusBg = ftxui::Color::Grey93;
        c.statusFg = ftxui::Color::Cyan;
        c.inputBg = ftxui::Color::Grey100;
        c.inputFg = ftxui::Color::Black;
        c.error = ftxui::Color::Red;
        c.warning = ftxui::Color::Yellow;
        c.success = ftxui::Color::Green;
        c.info = ftxui::Color::Cyan;
        return c;
    }

} // namespace AE::TUI
