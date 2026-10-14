#include "MarkdownRenderer.hpp"

#include <ftxui/dom/flexbox_config.hpp>

#include <algorithm>
#include <string_view>

namespace AE::TUI
{

    namespace
    {
        // 超长无空格词（如中文长段）按该宽度插入换行断点，保证渲染能换行。
        constexpr int kMaxWordWidth = 40;

        // 去除首尾空白（含 \r）。
        auto trimView(std::string_view s) -> std::string_view
        {
            const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
            while (!s.empty() && isSpace(s.front()))
                s.remove_prefix(1);
            while (!s.empty() && isSpace(s.back()))
                s.remove_suffix(1);
            return s;
        }

        // 把文本切成"非空白词"与"空白串"交替的 token 序列（供 flexbox 逐词换行）。
        auto tokenizeWords(const std::string &text) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            std::size_t i = 0;
            while (i < text.size())
            {
                const bool space = text[i] == ' ' || text[i] == '\t';
                std::size_t j = i;
                while (j < text.size() && ((text[j] == ' ' || text[j] == '\t') == space))
                    ++j;
                out.emplace_back(text.substr(i, j - i));
                i = j;
            }
            return out;
        }

        // 行首是否为无序列表项（"- " / "* " / "+ "）。
        auto isUnorderedItem(std::string_view trimmed) -> bool
        {
            return trimmed.size() >= 2 && (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') &&
                   trimmed[1] == ' ';
        }

        // 行首有序列表项：返回 "N. " 前缀长度（0 表示不是）。
        auto orderedItemPrefix(std::string_view trimmed) -> std::size_t
        {
            std::size_t d = 0;
            while (d < trimmed.size() && trimmed[d] >= '0' && trimmed[d] <= '9')
                ++d;
            if (d == 0 || d + 1 >= trimmed.size())
                return 0;
            if (trimmed[d] == '.' && trimmed[d + 1] == ' ')
                return d + 2;
            return 0;
        }
    } // namespace

    auto breakLongWords(const std::string &text) -> std::string
    {
        // 保留原文所有空白/换行不变，仅对"超过 kMaxWordWidth 的连续非空白段"在段内插入真实换行
        // '\n' 作为可断行点，供 FTXUI 在窄宽度下换行。
        // 为什么用 '\n' 而不是其它断点：
        //   1) 空格会在字符间显示可见空格 → 历史 bug"字符间随机多余空格"；
        //   2) 零宽空格 U+200B 在 Windows 控制台会被渲染成 '??' → 用户反馈的"随机双重问号"；
        //   3) '\n' 是 FTXUI paragraph 认可的真实换行，不产生任何可见伪字符。
        std::string out;
        out.reserve(text.size());
        int run = 0;
        for (char ch : text)
        {
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
            {
                out += ch;
                run = 0;
            }
            else
            {
                out += ch;
                ++run;
                if (run >= kMaxWordWidth)
                {
                    out += '\n';
                    run = 0;
                }
            }
        }
        return out;
    }

    auto MarkdownRenderer::parseInline(const std::string &line) -> std::vector<InlineSpan>
    {
        // 简易行内状态机：`code` / **bold** / *italic*；未闭合的标记按字面量保留。
        std::vector<InlineSpan> spans;
        std::string plain;
        auto flush = [&]()
        {
            if (!plain.empty())
            {
                spans.push_back(InlineSpan{plain, false, false, false});
                plain.clear();
            }
        };

        const std::size_t n = line.size();
        std::size_t i = 0;
        while (i < n)
        {
            const char c = line[i];
            if (c == '`')
            {
                const auto close = line.find('`', i + 1);
                if (close != std::string::npos && close > i + 1)
                {
                    flush();
                    spans.push_back(InlineSpan{line.substr(i + 1, close - i - 1), false, false, true});
                    i = close + 1;
                    continue;
                }
                plain += c;
                ++i;
                continue;
            }
            if (c == '*' && i + 1 < n && line[i + 1] == '*')
            {
                const auto close = line.find("**", i + 2);
                if (close != std::string::npos && close > i + 2)
                {
                    flush();
                    spans.push_back(InlineSpan{line.substr(i + 2, close - i - 2), true, false, false});
                    i = close + 2;
                    continue;
                }
                plain += c;
                ++i;
                continue;
            }
            if (c == '*')
            {
                const auto close = line.find('*', i + 1);
                if (close != std::string::npos && close > i + 1)
                {
                    flush();
                    spans.push_back(InlineSpan{line.substr(i + 1, close - i - 1), false, true, false});
                    i = close + 1;
                    continue;
                }
                plain += c;
                ++i;
                continue;
            }
            plain += c;
            ++i;
        }
        flush();
        return spans;
    }

    auto MarkdownRenderer::renderInline(const std::string &line,
                                        const std::shared_ptr<ThemeManager> &theme) -> ftxui::Element
    {
        const auto &tc = theme->colors();
        const auto spans = parseInline(line);

        // 纯文本快路径：与原版一致走 paragraph，保证中文/长文本折行行为稳定。
        if (spans.empty())
        {
            return ftxui::paragraph(breakLongWords(line)) | ftxui::color(tc.foreground);
        }
        if (spans.size() == 1 && !spans.front().bold && !spans.front().italic && !spans.front().code)
        {
            return ftxui::paragraph(breakLongWords(spans.front().text)) | ftxui::color(tc.foreground);
        }

        // 混合样式：flexbox 逐词排布（与 paragraph 同机制，自动换行），逐词应用装饰。
        ftxui::Elements items;
        for (const auto &span : spans)
        {
            for (const auto &token : tokenizeWords(span.text))
            {
                if (token.empty() || (token.front() != ' ' && token.front() != '\t'))
                {
                    auto elem = ftxui::text(breakLongWords(token));
                    if (span.bold)
                        elem = elem | ftxui::bold;
                    if (span.italic)
                        elem = elem | ftxui::italic;
                    if (span.code)
                        elem = elem | ftxui::color(tc.primary);
                    items.push_back(std::move(elem));
                }
                else
                {
                    // 空白 token 作为可见间隔项，保持词间空格。
                    items.push_back(ftxui::text(token));
                }
            }
        }
        return ftxui::flexbox(std::move(items)) | ftxui::color(tc.foreground);
    }

    auto MarkdownRenderer::render(const std::string &markdown,
                                  const std::shared_ptr<ThemeManager> &theme) -> ftxui::Element
    {
        const auto &tc = theme->colors();

        // 按行扫描：围栏代码块 / 标题 / 分隔线 / 引用 / 列表 / 段落。不追求完备语法覆盖，
        // 任何无法识别的行一律按段落渲染，保证内容不丢失。
        std::vector<std::string_view> lines;
        {
            std::string_view rest{markdown};
            while (true)
            {
                const auto nl = rest.find('\n');
                if (nl == std::string_view::npos)
                {
                    lines.push_back(rest);
                    break;
                }
                lines.push_back(rest.substr(0, nl));
                rest = rest.substr(nl + 1);
            }
        }

        ftxui::Elements blocks;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            const std::string_view raw = lines[i];
            const std::string_view trimmed = trimView(raw);
            if (trimmed.empty())
                continue; // 空行 = 块边界（demo 不做跨行段落合并）

            // 围栏代码块 ``` … ```：等宽感 + 底色边框。
            if (trimmed.starts_with("```"))
            {
                std::string lang(trimmed.substr(3));
                lang = std::string{trimView(lang)};
                std::string body;
                std::size_t j = i + 1;
                for (; j < lines.size(); ++j)
                {
                    const std::string_view t = trimView(lines[j]);
                    if (t.starts_with("```"))
                        break;
                    if (!body.empty())
                        body += '\n';
                    body += lines[j]; // 代码保留原始缩进
                }
                ftxui::Element codeElem = ftxui::paragraph(breakLongWords(body)) |
                                          ftxui::color(tc.foreground) |
                                          ftxui::bgcolor(tc.assistantBubble);
                blocks.push_back(ftxui::vbox({
                    ftxui::text(" " + (lang.empty() ? std::string{"code"} : std::move(lang))) |
                        ftxui::bold | ftxui::color(tc.muted),
                    codeElem | ftxui::border,
                }));
                i = j; // 跳过闭合围栏（j==size 时循环自然结束）
                continue;
            }

            // 标题 # ~ ######：加粗 + 主色；h1/h2 前后留空行。
            if (trimmed.front() == '#')
            {
                std::size_t level = 0;
                while (level < trimmed.size() && trimmed[level] == '#')
                    ++level;
                const bool isHeading = level <= 6 &&
                                       (level == trimmed.size() || trimmed[level] == ' ');
                if (isHeading)
                {
                    auto heading = ftxui::paragraph(breakLongWords(std::string{trimView(trimmed.substr(level))})) |
                                   ftxui::bold | ftxui::color(tc.primary);
                    if (level <= 2)
                        blocks.push_back(ftxui::vbox({ftxui::text(""), std::move(heading), ftxui::text("")}));
                    else
                        blocks.push_back(ftxui::vbox({std::move(heading), ftxui::text("")}));
                    continue;
                }
            }

            // 分隔线 --- / *** / ___
            if (trimmed == "---" || trimmed == "***" || trimmed == "___")
            {
                blocks.push_back(ftxui::separator());
                continue;
            }

            // 引用 > ：muted 色窗口缩进。
            if (trimmed.starts_with(">") && (trimmed.size() == 1 || trimmed[1] == ' '))
            {
                auto content = renderInline(std::string{trimView(trimmed.substr(1))}, theme);
                blocks.push_back(ftxui::window(ftxui::text(""), std::move(content)) |
                                 ftxui::color(tc.muted));
                continue;
            }

            // 列表项（无序 / 有序）：连续项归组为一个 vbox。
            const bool unordered = isUnorderedItem(trimmed);
            const std::size_t orderedLen = unordered ? 0 : orderedItemPrefix(trimmed);
            if (unordered || orderedLen > 0)
            {
                ftxui::Elements items;
                std::size_t j = i;
                for (; j < lines.size(); ++j)
                {
                    const std::string_view t = trimView(lines[j]);
                    const bool u = isUnorderedItem(t);
                    const std::size_t o = u ? 0 : orderedItemPrefix(t);
                    if (!u && o == 0)
                        break;
                    const std::string prefix = u ? "• " : std::string{t.substr(0, o)};
                    const std::string content{trimView(t.substr(u ? 2 : o))};
                    items.push_back(ftxui::hbox({
                        ftxui::text(prefix) | ftxui::color(tc.muted),
                        renderInline(content, theme) | ftxui::flex,
                    }));
                }
                blocks.push_back(ftxui::vbox(std::move(items)));
                i = j - 1;
                continue;
            }

            // 普通段落（含未识别行）：行内样式渲染。
            blocks.push_back(renderInline(std::string{trimmed}, theme));
        }

        if (blocks.empty())
        {
            return ftxui::paragraph(breakLongWords(markdown)) | ftxui::color(tc.foreground);
        }
        return ftxui::vbox(std::move(blocks));
    }

} // namespace AE::TUI
