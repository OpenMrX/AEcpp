#include "StatusBar.hpp"

#include <ftxui/component/event.hpp>

namespace AE::TUI
{

namespace
{
    constexpr std::size_t kModelDisplayMax = 18;

    // 估算字符串在终端中的显示宽度：ASCII 占 1 列，非 ASCII（CJK/Emoji 等）占 2 列。
    auto displayWidth(std::string_view s) -> int
    {
        int w = 0;
        std::size_t i = 0;
        while (i < s.size())
        {
            unsigned char c = static_cast<unsigned char>(s[i]);
            int len = 1;
            if ((c & 0x80) == 0)
                len = 1;
            else if ((c & 0xE0) == 0xC0)
                len = 2;
            else if ((c & 0xF0) == 0xE0)
                len = 3;
            else if ((c & 0xF8) == 0xF0)
                len = 4;
            else
            {
                ++i;
                continue;
            }

            w += (len == 1) ? 1 : 2;
            i += len;
        }
        return w;
    }

    // 生成状态栏条目的显示文本。模型名在栏中固定截取 20 字符，点击选择时仍使用全称。
    auto makeDisplayLabel(const StatusItem &item) -> std::string
    {
        if (item.label.rfind("📦", 0) == 0)
        {
            const std::string &name = item.value.empty() ? item.label : item.value;
            if (name.size() <= kModelDisplayMax)
                return "Model: " + name;
            return "Model: " + name.substr(0, kModelDisplayMax);
        }
        return item.value.empty() ? item.label : item.label + ": " + item.value;
    }

    // 把 StatusItem.color 名称映射到 ftxui::Color（仅解析本项目使用的有限集合，
    // 未知名称回退到主题状态前景色）。
    auto resolveStatusColor(const std::string &name, ftxui::Color fallback) -> ftxui::Color
    {
        if (name == "green")
            return ftxui::Color::Green3;
        if (name == "yellow")
            return ftxui::Color::Yellow3;
        if (name == "orange")
            return ftxui::Color::Orange3;
        if (name == "red")
            return ftxui::Color::Red3;
        if (name == "cyan")
            return ftxui::Color::Cyan3;
        if (name == "blue")
            return ftxui::Color::Blue3;
        if (name == "magenta")
            return ftxui::Color::Magenta3;
        if (name == "gray" || name == "grey")
            return ftxui::Color::Grey66;
        return fallback;
    }
}

StatusBarComponent::StatusBarComponent(std::shared_ptr<ThemeManager> theme)
    : m_theme(std::move(theme))
{
}

auto StatusBarComponent::render() const -> ftxui::Element
{
    if (!m_visible)
    {
        return ftxui::emptyElement();
    }

    std::vector<ftxui::Element> leftItems;
    int x = 0;

    // 左侧：运行模式徽章
    const std::string modeLabel = " " + m_modeText + " ";
    leftItems.push_back(ftxui::text(modeLabel) | ftxui::bold |
                         ftxui::bgcolor(m_theme->colors().primary));
    x += displayWidth(modeLabel);

    leftItems.push_back(ftxui::separatorEmpty());
    x += 1;

    // Phase I: 连接状态灯
    leftItems.push_back(
        ftxui::text(m_connected ? "●" : "○") |
        ftxui::color(m_connected ? m_theme->colors().success : m_theme->colors().error));
    x += 1;

    if (!m_leftText.empty())
    {
        leftItems.push_back(ftxui::separatorEmpty());
        x += 1;
        leftItems.push_back(ftxui::text(m_leftText));
        x += displayWidth(m_leftText);
    }

    m_itemHitBoxes.clear();
    for (const auto &item : m_items)
    {
        leftItems.push_back(ftxui::separatorEmpty());
        x += 1;

        const std::string displayLabel = makeDisplayLabel(item);
        auto itemElem = ftxui::text(displayLabel);
        if (item.value.empty())
        {
            itemElem |= ftxui::color(m_theme->colors().muted);
        }
        else if (!item.color.empty())
        {
            // 有显式颜色（如 Ctx 占比阈值色）→ 以该颜色渲染整个条目文本。
            itemElem |= ftxui::color(resolveStatusColor(item.color, m_theme->colors().statusFg));
        }
        else
        {
            itemElem |= ftxui::color(m_theme->colors().statusFg);
        }
        leftItems.push_back(itemElem);

        const int width = displayWidth(displayLabel);
        m_itemHitBoxes.push_back(ItemHitBox{displayLabel, x, x + width});
        x += width;
    }

    auto left = ftxui::hbox(std::move(leftItems));

    auto right = ftxui::hbox({
        ftxui::text(m_rightText) | ftxui::color(m_theme->colors().muted),
        ftxui::text("  "),
    });

    auto bar = ftxui::hbox({
        left | ftxui::flex,
        right,
    });

    bar = bar | ftxui::bgcolor(m_theme->colors().statusBg) |
          ftxui::color(m_theme->colors().statusFg) |
          ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1);

    return bar;
}

auto StatusBarComponent::getComponent() -> ftxui::Component
{
    auto renderer = ftxui::Renderer([this] { return render(); });

    if (!m_onItemClick)
    {
        return renderer;
    }

    // 用纯文本渲染状态栏，同时捕获鼠标点击事件，命中模型项时触发回调。
    return ftxui::CatchEvent(renderer, [this](ftxui::Event event) -> bool {
        if (!event.is_mouse())
            return false;

        const auto &mouse = event.mouse();
        if (mouse.button != ftxui::Mouse::Left || mouse.motion != ftxui::Mouse::Pressed)
            return false;

        // 状态栏固定在屏幕最底行。
        const int screenH = ftxui::Terminal::Size().dimy;
        if (mouse.y != screenH - 1)
            return false;

        for (std::size_t i = 0; i < m_itemHitBoxes.size(); ++i)
        {
            if (i >= m_items.size())
                break;
            if (mouse.x >= m_itemHitBoxes[i].xStart && mouse.x < m_itemHitBoxes[i].xEnd)
            {
                if (m_items[i].label.rfind("📦", 0) == 0)
                {
                    m_onItemClick(m_items[i]);
                    return true;
                }
            }
        }
        return false;
    });
}

auto StatusBarComponent::setItems(const std::vector<StatusItem> &items) -> void
{
    m_items = items;
}

auto StatusBarComponent::setLeftText(const std::string &text) -> void
{
    m_leftText = text;
}

auto StatusBarComponent::setRightText(const std::string &text) -> void
{
    m_rightText = text;
}

auto StatusBarComponent::setModeText(const std::string &mode) -> void
{
    m_modeText = mode;
}

auto StatusBarComponent::setOnItemClick(std::function<void(const StatusItem &)> cb) -> void
{
    m_onItemClick = std::move(cb);
}

auto StatusBarComponent::setConnected(bool connected) -> void
{
    m_connected = connected;
}

auto StatusBarComponent::setVisible(bool visible) -> void
{
    m_visible = visible;
}

auto StatusBarComponent::isVisible() const -> bool
{
    return m_visible;
}

} // namespace AE::TUI
