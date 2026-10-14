#pragma once

/// @file demo/tui/Components/StreamingMarkdownSplitter.hpp
/// @brief 流式 Markdown 拆分器：增量渲染窗口，避免每 token 全量重解析。
/// @details 维护已渲染前缀与待处理缓冲，按块边界逐步消费流式输入；
///          线程安全（内部 `mutex` 保护增量状态）。

#include "../Core/Theme.hpp"

#include <ftxui/dom/elements.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace AE::TUI
{

    /**
     * @brief 流式 Markdown 增量渲染器（Phase E，规划 §7 方案 B）
     *
     * 增量块状态机 + 已提交缓存：
     * - 维护块打开状态（代码围栏 / 表格 / 引用 / 列表 / 标题 / 段落）。
     * - 每 delta 追加原始 buffer；块一旦闭合即提交：parseContent(该块) 一次，
     *   缓存 ftxui::Element。
     * - 仅当前打开的尾块按纯文本重渲染（廉价）。
     * - finish() 后整段 parseContent 一次，做最终干净渲染替换增量结果。
     *
     * 线程安全：
     * - append() / finish() 由后台流式线程调用，内部加锁。
     * - render() 由 UI 渲染线程调用：锁内取未渲染块快照，锁外解析渲染
     *   （避免长耗时 markdown 解析阻塞后台写入），再加锁写回缓存。
     */
    class StreamingMarkdownSplitter
    {
    public:
        StreamingMarkdownSplitter() = default;

        // 追加流式增量（任意长度，内部按行切分推进状态机）。
        auto append(std::string_view delta) -> void;
        // 流式结束：提交尾块并标记完成，后续 render() 返回整段最终渲染。
        auto finish() -> void;
        auto isFinished() const -> bool;
        // 渲染当前增量结果（已提交块缓存 + 尾块纯文本）。
        auto render(const std::shared_ptr<ThemeManager> &theme) const -> ftxui::Element;

    private:
        enum class BlockKind
        {
            None,
            Paragraph,
            Heading,
            List,
            BlockQuote,
            CodeFence,
            Table
        };

        struct CommittedBlock
        {
            BlockKind kind = BlockKind::None;
            std::string text;
            mutable ftxui::Element cached;
            mutable bool rendered = false;
        };

        // 处理一行完整文本（含行尾 '\n'），推进块状态机。
        auto processLine(std::string_view line) -> void;
        // 把当前打开的块移入已提交列表。
        auto commitPending() -> void;
        // 渲染当前打开的尾块（纯文本，廉价）。
        auto renderPendingText(const std::string &text, BlockKind kind,
                               const std::shared_ptr<ThemeManager> &theme) const -> ftxui::Element;

        std::string m_fullText;   // 全文（finish 后整段渲染用）
        std::string m_pending;    // 未处理尾部（含未闭合行）
        std::vector<CommittedBlock> m_committed;
        std::string m_pendingText;
        BlockKind m_pendingKind = BlockKind::None;
        bool m_finished = false;
        mutable std::optional<ftxui::Element> m_finalElement; // finish 后整段渲染缓存
        mutable std::mutex m_mutex;
    };

} // namespace AE::TUI
