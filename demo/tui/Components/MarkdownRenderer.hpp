#pragma once

/// @file demo/tui/Components/MarkdownRenderer.hpp
/// @brief Markdown → FTXUI 元素渲染器：纯标准库实现的简易行级解析。
/// @details 不依赖任何外部 markdown 解析库：按行扫描识别标题（`#`~`######`）、
///          围栏代码块 ```、无序/有序列表、引用、分隔线与段落；行内解析
///          `` `code` `` / `**bold**` / `*italic*`。不追求 CommonMark 完备性，
///          解析失败一律回退纯文本段落，保证内容不丢失。
///          公开类名与方法签名与原版一致（MessageBubble / StreamingMarkdownSplitter 零改动可用）。

#include "../Core/Theme.hpp"

#include <ftxui/dom/elements.hpp>

#include <memory>
#include <string>
#include <vector>

namespace AE::TUI
{

    /**
     * @brief 对超长无空格 token（URL / base64 / 长文件路径 / 压缩代码单行等）按宽度断词，
     * 避免 ftxui::paragraph 无法在长 token 内换行而被横向裁切（溢出显示）。
     * 仅在 token 长度超过 kMaxWordWidth 时插入断点（换行符），正常文本保持不变。
     */
    auto breakLongWords(const std::string &text) -> std::string;

    /**
     * @brief Markdown 整段渲染器（非流式）。
     *
     * 流式增量渲染见 StreamingMarkdownSplitter，本类只做整段渲染。
     */
    class MarkdownRenderer
    {
    public:
        // 整段渲染 markdown 文本 → ftxui::Element。解析失败时回退为纯文本段落。
        static auto render(const std::string &markdown,
                           const std::shared_ptr<ThemeManager> &theme) -> ftxui::Element;

    private:
        // 行内片段：一段连续同装饰样式的文本。
        struct InlineSpan
        {
            std::string text;
            bool bold = false;
            bool italic = false;
            bool code = false;
        };

        // 解析单行 markdown 的行内片段序列（`code` / **bold** / *italic*）。
        static auto parseInline(const std::string &line) -> std::vector<InlineSpan>;
        // 行内渲染：把单行文本解析为带装饰且自动换行的 Element。
        static auto renderInline(const std::string &line,
                                 const std::shared_ptr<ThemeManager> &theme) -> ftxui::Element;
    };

} // namespace AE::TUI
