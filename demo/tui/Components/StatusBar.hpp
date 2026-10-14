#pragma once

/// @file demo/tui/Components/StatusBar.hpp
/// @brief 底部状态栏：模型/会话状态/连接灯等运行时信息展示。
/// @details 数据经 setter 注入（仅置脏），渲染延后到帧边界；不含业务逻辑。

#include "../TUIType.hpp"
#include "../Core/Theme.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AE::TUI
{

    class StatusBarComponent
    {
    public:
        explicit StatusBarComponent(std::shared_ptr<ThemeManager> theme);

        auto render() const -> ftxui::Element;
        auto getComponent() -> ftxui::Component;

        auto setItems(const std::vector<StatusItem> &items) -> void;
        auto setLeftText(const std::string &text) -> void;
        auto setRightText(const std::string &text) -> void;
        auto setModeText(const std::string &mode) -> void;

        // Phase I: 状态栏连接灯（● 已连接 / ○ 未连接）。
        auto setConnected(bool connected) -> void;

        auto setVisible(bool visible) -> void;
        auto isVisible() const -> bool;

        // 设置点击状态栏条目时的回调（主要用于点击模型名展开模型列表）。
        auto setOnItemClick(std::function<void(const StatusItem &)> cb) -> void;

    private:
        std::shared_ptr<ThemeManager> m_theme;
        std::vector<StatusItem> m_items;
        std::string m_leftText;
        std::string m_rightText;
        std::string m_modeText = "NORMAL";
        // Phase I: 状态栏连接灯。
        bool m_connected = true;
        bool m_visible = true;

        // 鼠标点击状态栏条目时的回调（如点击模型名展开模型列表）。
        std::function<void(const StatusItem &)> m_onItemClick;

        // 状态栏条目在屏幕上的命中框（每帧渲染时更新，供鼠标点击命中检测）。
        struct ItemHitBox
        {
            std::string displayLabel;
            int xStart = 0;
            int xEnd = 0;
        };
        mutable std::vector<ItemHitBox> m_itemHitBoxes;
    };

} // namespace AE::TUI
