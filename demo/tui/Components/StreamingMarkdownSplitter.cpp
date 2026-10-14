#include "StreamingMarkdownSplitter.hpp"
#include "MarkdownRenderer.hpp"

#include <utility>

namespace AE::TUI
{

    auto StreamingMarkdownSplitter::append(std::string_view delta) -> void
    {
        std::scoped_lock lock(m_mutex);
        if (m_finished)
            return;
        m_fullText += delta;
        m_pending += delta;

        // 逐行处理完整行（含行尾 \n）。
        std::size_t pos = 0;
        while (true)
        {
            auto nl = m_pending.find('\n', pos);
            if (nl == std::string::npos)
                break;
            processLine({m_pending.data() + pos, nl - pos + 1});
            pos = nl + 1;
        }
        if (pos > 0)
            m_pending.erase(0, pos);
    }

    auto StreamingMarkdownSplitter::finish() -> void
    {
        std::scoped_lock lock(m_mutex);
        if (m_finished)
            return;
        m_finished = true;

        // 提交未闭合的尾行（补一个换行使其成为完整行）。
        if (!m_pending.empty())
        {
            if (m_pending.back() != '\n')
                m_pending.push_back('\n');
            processLine(m_pending);
            m_pending.clear();
        }
        if (m_pendingKind != BlockKind::None)
            commitPending();
    }

    auto StreamingMarkdownSplitter::isFinished() const -> bool
    {
        std::scoped_lock lock(m_mutex);
        return m_finished;
    }

    auto StreamingMarkdownSplitter::render(const std::shared_ptr<ThemeManager> &theme) const -> ftxui::Element
    {
        // ── 锁内取快照 ────────────────────────────────────────────────────
        struct Pending
        {
            std::size_t index;
            std::string text;
        };
        std::vector<Pending> toParse;
        std::string pendingText;
        BlockKind pendingKind = BlockKind::None;
        bool finished = false;
        std::string fullText;
        {
            std::scoped_lock lock(m_mutex);
            finished = m_finished;
            if (finished)
            {
                fullText = m_fullText;
            }
            else
            {
                for (std::size_t i = 0; i < m_committed.size(); ++i)
                {
                    if (!m_committed[i].rendered)
                        toParse.push_back({i, m_committed[i].text});
                }
                // 尾部渲染 = 已处理完整行 + 当前未闭合行（m_pending 只含未换行的尾行，
                // 与 m_pendingText 无重叠；不拼上它会导致流式时当前行在换行前不可见）。
                pendingText = m_pendingText;
                if (!m_pending.empty())
                    pendingText += m_pending;
                pendingKind = m_pendingKind;
            }
        }

        // ── 已完成：整段解析一次做最终干净渲染，替换增量结果 ────────────
        if (finished)
        {
            std::scoped_lock lock(m_mutex);
            if (!m_finalElement.has_value())
            {
                m_finalElement = MarkdownRenderer::render(fullText, theme);
            }
            return *m_finalElement;
        }

        // ── 锁外解析未渲染块（避免阻塞后台 append）──────────────────────
        std::vector<std::pair<std::size_t, ftxui::Element>> parsed;
        parsed.reserve(toParse.size());
        for (const auto &p : toParse)
        {
            parsed.emplace_back(p.index, MarkdownRenderer::render(p.text, theme));
        }

        // ── 锁内写回缓存并组合 ───────────────────────────────────────────
        std::scoped_lock lock(m_mutex);
        for (const auto &[index, el] : parsed)
        {
            m_committed[index].cached = el;
            m_committed[index].rendered = true;
        }

        ftxui::Elements elements;
        elements.reserve(m_committed.size() + 1);
        for (const auto &block : m_committed)
            elements.push_back(block.cached);

        if (!pendingText.empty())
            elements.push_back(renderPendingText(pendingText, pendingKind, theme));

        if (elements.empty())
            return ftxui::text("");
        return ftxui::vbox(std::move(elements));
    }

    // ─── 私有实现 ───────────────────────────────────────────────────────────

    auto StreamingMarkdownSplitter::processLine(std::string_view line) -> void
    {
        // 去除行尾换行符进行分析。
        std::string_view trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\n')
            trimmed.remove_suffix(1);
        if (!trimmed.empty() && trimmed.back() == '\r')
            trimmed.remove_suffix(1);

        const bool isBlank = trimmed.empty();
        const bool isCodeFence = trimmed.starts_with("```");

        // 代码围栏：打开 / 闭合。
        if (isCodeFence)
        {
            if (m_pendingKind == BlockKind::CodeFence)
            {
                m_pendingText += std::string(line);
                commitPending();
                return;
            }
            if (m_pendingKind != BlockKind::None)
                commitPending();
            m_pendingKind = BlockKind::CodeFence;
            m_pendingText += std::string(line);
            return;
        }

        // 围栏内：直接累积。
        if (m_pendingKind == BlockKind::CodeFence)
        {
            m_pendingText += std::string(line);
            return;
        }

        // 空行：提交当前块。
        if (isBlank)
        {
            if (m_pendingKind != BlockKind::None)
                commitPending();
            return;
        }

        // 从行首推断块类型。
        BlockKind lineKind = BlockKind::Paragraph;
        if (trimmed.starts_with('#'))
            lineKind = BlockKind::Heading;
        else if (trimmed.starts_with('>'))
            lineKind = BlockKind::BlockQuote;
        else if (trimmed.starts_with("- ") || trimmed.starts_with("* ") || trimmed.starts_with("+ "))
            lineKind = BlockKind::List;
        else if (trimmed[0] >= '0' && trimmed[0] <= '9')
        {
            std::size_t d = 0;
            while (d < trimmed.size() && trimmed[d] >= '0' && trimmed[d] <= '9')
                ++d;
            if (d < trimmed.size() && trimmed[d] == '.' && d + 1 < trimmed.size() && trimmed[d + 1] == ' ')
                lineKind = BlockKind::List;
        }
        else if (trimmed.find('|') != std::string_view::npos)
        {
            lineKind = BlockKind::Table;
        }

        // 类型切换 → 提交前一块。
        if (lineKind != m_pendingKind)
        {
            if (m_pendingKind != BlockKind::None)
                commitPending();
            m_pendingKind = lineKind;
        }

        m_pendingText += std::string(line);
    }

    auto StreamingMarkdownSplitter::commitPending() -> void
    {
        if (m_pendingText.empty())
            return;
        CommittedBlock block;
        block.kind = m_pendingKind;
        block.text = m_pendingText;
        m_committed.push_back(std::move(block));
        m_pendingText.clear();
        m_pendingKind = BlockKind::None;
    }

    auto StreamingMarkdownSplitter::renderPendingText(const std::string &text, BlockKind kind,
                                                       const std::shared_ptr<ThemeManager> &theme) const -> ftxui::Element
    {
        // 尾块按纯文本渲染（流式结束前可能不完整，不送 Markdown 解析器解析）。
        (void)kind;
        return ftxui::paragraph(AE::TUI::breakLongWords(text)) | ftxui::color(theme->colors().foreground);
    }

} // namespace AE::TUI