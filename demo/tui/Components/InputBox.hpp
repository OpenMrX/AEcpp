#pragma once

/// @file demo/tui/Components/InputBox.hpp
/// @brief 输入框组件：多行输入、发送回调、占位提示。
/// @details 基于 FTXUI Input/TextArea；提交经回调输出（不持业务状态），
///          内容清空由调用方（TUI 层）处理。

#include "../TUIType.hpp"
#include "../Core/Theme.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <functional>
#include <memory>
#include <string>

namespace AE::TUI
{

    class InputBox
    {
    public:
        using SubmitCallback = std::function<void(const std::string &text)>;
        using FocusCallback = std::function<void()>;

        explicit InputBox(std::shared_ptr<ThemeManager> theme);

        auto render() -> ftxui::Element;
        auto getComponent() -> ftxui::Component;

        auto onSubmit(SubmitCallback callback) -> void;
        auto onFocus(FocusCallback callback) -> void;

        auto clear() -> void;
        auto setPlaceholder(const std::string &placeholder) -> void;
        auto setEnabled(bool enabled) -> void;

        auto takeFocus() -> void;

    private:
        auto handleEvent(ftxui::Event event) -> bool;
        auto submit() -> void;

        std::shared_ptr<ThemeManager> m_theme;
        std::string m_content;
        std::string m_placeholder = "输入消息…（Enter 换行 · Shift+Enter 发送）";
        bool m_enabled = true;
        bool m_hasFocus = false;

        SubmitCallback m_submitCallback;
        FocusCallback m_focusCallback;

        ftxui::Component m_inputComponent;
    };

} // namespace AE::TUI
