#pragma once

/// @file demo/tui/Core/Theme.hpp
/// @brief TUI 主题定义：FTXUI 颜色集合与主题表（暗色/亮色切换）。
/// @details 提供 `ThemeColors` 与主题名→颜色映射，供组件统一取色，禁止硬编码 RGB。

#include "../TUIType.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <string>
#include <unordered_map>

namespace AE::TUI
{

    struct ThemeColors
    {
        ftxui::Color background;
        ftxui::Color foreground;
        ftxui::Color primary;
        ftxui::Color secondary;
        ftxui::Color accent;
        ftxui::Color muted;
        ftxui::Color border;
        ftxui::Color userBubble;
        ftxui::Color assistantBubble;
        ftxui::Color systemBubble;
        ftxui::Color sidebarBg;
        ftxui::Color sidebarFg;
        ftxui::Color headerBg;
        ftxui::Color headerFg;
        ftxui::Color statusBg;
        ftxui::Color statusFg;
        ftxui::Color inputBg;
        ftxui::Color inputFg;
        ftxui::Color error;
        ftxui::Color warning;
        ftxui::Color success;
        ftxui::Color info;
    };

    class ThemeManager
    {
    public:
        ThemeManager();

        auto setTheme(ThemeVariant variant) -> void;
        auto getCurrentTheme() const -> ThemeVariant;
        auto colors() const -> const ThemeColors &;

        auto applyHeaderStyle(ftxui::Element element) const -> ftxui::Element;
        auto applySidebarStyle(ftxui::Element element) const -> ftxui::Element;
        auto applyUserBubbleStyle(ftxui::Element element) const -> ftxui::Element;
        auto applyAssistantBubbleStyle(ftxui::Element element) const -> ftxui::Element;
        auto applySystemBubbleStyle(ftxui::Element element) const -> ftxui::Element;
        auto applyInputStyle(ftxui::Element element) const -> ftxui::Element;
        auto applyStatusBarStyle(ftxui::Element element) const -> ftxui::Element;
        auto applyBorderStyle(ftxui::Element element) const -> ftxui::Element;
        auto applyFocusedStyle(ftxui::Element element) const -> ftxui::Element;

    private:
        auto loadDarkTheme() -> ThemeColors;
        auto loadLightTheme() -> ThemeColors;
        auto loadDraculaTheme() -> ThemeColors;
        auto loadMonokaiTheme() -> ThemeColors;
        auto loadSolarizedTheme() -> ThemeColors;

        ThemeVariant m_currentTheme = ThemeVariant::Dark;
        ThemeColors m_colors;
    };

} // namespace AE::TUI
