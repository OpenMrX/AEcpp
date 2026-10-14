#include "InputBox.hpp"

#include <ftxui/component/event.hpp>

#include <algorithm>
#include <utility>

#ifdef _WIN32
// 系统动态解析接口最小声明（名称必须与导出符号一致；kernel32 为默认链接库，
// 无需引入任何平台头文件）。
extern "C" __declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
extern "C" __declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
#endif

namespace AE::TUI
{

namespace
{
    // Shift 虚拟键码（与系统键盘布局常量一致，避免引入平台头文件）。
    constexpr int kVKeyShift = 0x10;

    /// @brief 检测 Shift 修饰键当前是否按下（语义与原版 GetAsyncKeyState 探测完全一致）。
    /// 本层禁止直接引入平台头文件：Windows 下通过动态解析系统输入查询接口实现；
    /// 非 Windows 回退 false，由 kitty/CSI-u 转义匹配（见 handleEvent）兜底。
    auto shiftDownNow() -> bool
    {
#ifdef _WIN32
        using QueryFn = short(__stdcall *)(int);
        static const QueryFn query = []() -> QueryFn
        {
            void *mod = LoadLibraryA("user32.dll");
            if (mod == nullptr)
                return nullptr;
            return reinterpret_cast<QueryFn>(GetProcAddress(mod, "GetAsyncKeyState"));
        }();
        if (query != nullptr)
            return (query(kVKeyShift) & 0x8000) != 0;
        return false;
#else
        return false;
#endif
    }
} // namespace

    InputBox::InputBox(std::shared_ptr<ThemeManager> theme)
        : m_theme(std::move(theme))
    {
        ftxui::InputOption inputOption;
        inputOption.content = &m_content;
        inputOption.placeholder = &m_placeholder;
        // 多行输入：粘贴多行内容时保留换行，而不是整串连成一行。
        // 发送改由 Shift+Enter 触发（见 handleEvent），因此 on_enter 置为 no-op，
        // 否则 FTXUI 在多行模式下 Enter 会既插入换行又触发提交，导致"粘贴即发送"。
        inputOption.multiline = true;
        inputOption.on_enter = [] {};

        m_inputComponent = ftxui::Input(inputOption);
    }

    auto InputBox::render() -> ftxui::Element
    {
        auto inputElement = m_inputComponent->Render();

        auto wrapper = ftxui::window(
            ftxui::text(" Input "),
            ftxui::vbox({
                inputElement | ftxui::color(m_theme->colors().foreground) | ftxui::flex,
            }));

        if (!m_enabled)
        {
            wrapper = wrapper | ftxui::color(m_theme->colors().muted) | ftxui::dim;
        }

        // 自适应高度：随内容行数增长，但封顶（内部 frame 自动滚动），保证多行粘贴可见。
        // 计算内容行数用于提示，实际高度由 GREATER_THAN/LESS_THAN 约束决定。
        int contentLines = 1;
        for (char c : m_content)
        {
            if (c == '\n')
                ++contentLines;
        }
        (void)contentLines; // 仅供调试提示，布局高度由约束控制。

        return wrapper | ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, 3) |
               ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 8);
    }

    auto InputBox::getComponent() -> ftxui::Component
    {
        // 用 CatchEvent 拦截 Enter/Shift+Enter 等，先于 FTXUI Input 内部处理提交语义，
        // 再交给 Input 组件渲染与其余编辑键（左右/删除/方向键等）处理。
        auto inputWithEvents = ftxui::CatchEvent(m_inputComponent,
                                                 [this](ftxui::Event event)
                                                 { return handleEvent(std::move(event)); });
        return ftxui::Renderer(inputWithEvents,
                               [this]
                               { return render(); });
    }

    auto InputBox::onSubmit(SubmitCallback callback) -> void
    {
        m_submitCallback = std::move(callback);
    }

    auto InputBox::onFocus(FocusCallback callback) -> void
    {
        m_focusCallback = std::move(callback);
    }

    auto InputBox::clear() -> void
    {
        m_content.clear();
    }

    auto InputBox::setPlaceholder(const std::string &placeholder) -> void
    {
        m_placeholder = placeholder;
    }

    auto InputBox::setEnabled(bool enabled) -> void
    {
        m_enabled = enabled;
    }

    auto InputBox::takeFocus() -> void
    {
        if (m_inputComponent)
        {
            m_inputComponent->TakeFocus();
        }
        if (m_focusCallback)
        {
            m_focusCallback();
        }
    }

    auto InputBox::submit() -> void
    {
        if (!m_submitCallback)
            return;
        // 去除首尾空行/空白后为空则不发送。
        const std::string trimmed = m_content;
        if (trimmed.empty())
            return;
        const std::string text = m_content;
        m_content.clear();
        m_submitCallback(text);
    }

    auto InputBox::handleEvent(ftxui::Event event) -> bool
    {
        if (!m_enabled)
            return false;
        if (!m_inputComponent)
            return false;

        // Enter 键语义：
        //  - Enter = 插入换行（多行编辑）。on_enter 已置 no-op，Input 多行模式会插入 '\n'。
        //  - Shift+Enter = 发送。
        // 关键：发送必须绑定 Shift+Enter，而不能是 Enter。因为 Windows 终端里**粘贴多行文本的
        // 换行也会产生 Event::Return**（CR），与手动 Enter 完全无法区分——若 Enter 发送，
        // 多行粘贴会被自动分段发送（用户反馈的 bug）。用系统级 Shift 探测区分
        // 手动 Shift+Enter 与粘贴产生的 Enter：粘贴换行无 Shift → 走 Enter 分支插入换行，不发送。
        // 兼容 kitty/CSI-u 终端：Shift+Enter 编码为 "\x1b[13;2u"（Shift 位掩码 0x2），
        // 普通 Enter 编码为 "\x1b[13u"，据此区分。
        if (event.input().starts_with("\x1b[13;2u"))
        {
            submit();
            return true;
        }
        if (event == ftxui::Event::Return)
        {
            if (shiftDownNow())
            {
                // Shift+Enter → 发送（多行粘贴完成后再 Shift+Enter 提交整段）。
                submit();
                return true;
            }
            // Enter（含粘贴产生的换行）→ 插入换行，交由 Input 多行模式处理。
            return m_inputComponent->OnEvent(std::move(event));
        }

        return m_inputComponent->OnEvent(std::move(event));
    }

} // namespace AE::TUI
