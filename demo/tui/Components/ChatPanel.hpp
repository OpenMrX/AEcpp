#pragma once

/// @file demo/tui/Components/ChatPanel.hpp
/// @brief 聊天面板：消息流渲染 + 流式 Markdown 拆分 + 滚动管理。
/// @details 组合 `MessageBubble`/`StreamingMarkdownSplitter` 呈现会话消息；
///          流式增量经拆分器逐步渲染，避免整段重绘。

#include "../TUIType.hpp"
#include "../Core/Theme.hpp"
#include "MessageBubble.hpp"
#include "StreamingMarkdownSplitter.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AE::TUI
{

    /**
     * @brief 聊天面板组件（Phase B：原生滚动 + 完整渲染）
     *
     * 关键设计：
     * - 完整渲染全部消息（不再跳过最早消息，修复历史被缩减问题）。
     * - 用 FTXUI 原生滚动容器承载：`vbox(allBubbles) | focusPositionRelative(0, focusY) | yframe`。
     *   frame 的滚动由 focusPositionRelative 装饰器驱动，无需组件聚焦，纯 dom 层即可实现。
     * - 自动跟随底部：focusY=1.0（内容贴底，流式输出钉在最新消息）。
     * - 手动滚动：focusY 由比例 m_scrollRatio 驱动，用户上滚暂停跟随，滚回底部恢复。
     * - 滚动条用 FTXUI 原生 `vscroll_indicator`（依据元素真实测量高度自动显示/定位），
     *   彻底根治"估算偏低 → 无滚条/无法滚动/内容截断"。鼠标滚轮 / PgUp / PgDn 浏览历史。
     */
    class ChatPanel
    {
    public:
        explicit ChatPanel(std::shared_ptr<ThemeManager> theme);

        auto render() -> ftxui::Element;
        auto getComponent() -> ftxui::Component;

        auto addMessage(const Message &message) -> void;
        // §8.7：整体替换消息列表（切换会话时从后端回读历史）。
        auto setMessages(std::vector<Message> messages) -> void;
        auto updateStreamingMessage(const std::string &messageId, std::string_view delta) -> void;
        // 是否有进行中的流式消息（供 TUI 自适应帧率：流式时高帧率、安静时保留模式）。
        auto isStreaming() const -> bool;
        // 关键接口：直接覆盖指定消息的内容（替换而非追加），用于工具结果回填、最终答案写入等场景
        auto setMessageContent(const std::string &messageId, std::string_view content) -> bool;
        auto finishStreamingMessage(const std::string &messageId) -> void;
        // Phase I: 设置正文消息的思考内容（to thinkingContent field）。
        auto setThinkingContent(const std::string &messageId, std::string_view thinking) -> void;
        // 阶段4: 设置正文消息的编排信息块（聚合意图/路由/迭代/工具步摘要，默认折叠）。
        auto setOrchestrationInfo(const std::string &messageId, OrchestrationInfo info) -> void;
        auto clearMessages() -> void;
        // 鼠标点击某条消息的思考块头部，切换该消息思考内容的展开/折叠（单条独立）。
        // 返回 true 表示点击命中了某条思考块（已处理，消费该点击事件）。
        auto handleThinkingClick(int x, int y) -> bool;
        // 阶段4: 鼠标点击某条消息的编排块头部，切换该消息编排块的展开/折叠（单条独立）。
        auto handleOrchestrationClick(int x, int y) -> bool;
        // 鼠标点击某条 Tool Result 卡片头部，切换该卡片展开/折叠（单条独立）。
        auto handleToolResultClick(int x, int y) -> bool;
        // 回填服务端真实 token（助手消息：input/output/reasoning/total；用户消息仅 input）。
        auto setMessageTokens(const std::string &messageId, int32_t inputTokens,
                              int32_t outputTokens, int32_t reasoningTokens, int32_t totalTokens) -> void;
        // 回填最近一条用户消息的输入 token（相邻请求 prompt_tokens 差值）。
        auto setLastUserMessageTokens(int32_t inputTokens) -> void;
        // 加锁返回消息快照（值拷贝），避免调用方在后台写线程并发时无锁读导致堆损坏。
        auto getMessages() const -> MessageList;

        auto setSessionTitle(const std::string &title) -> void;

        // 滚动控制接口（由 TUI::buildLayout 的 CatchEvent 调用）
        auto scrollUp(int lines) -> void;
        auto scrollDown(int lines) -> void;
        auto scrollToTop() -> void;
        auto scrollToBottom() -> void;
        // 滚轮单次步长：按视口半屏计算，长对话也能快速到达历史（替代固定 3 行）。
        auto scrollStepLines() const -> int;

    private:
        auto renderWelcome() const -> ftxui::Element;
        // 由"应滚动的内容行数"折算滚动比例步长（相对真实内容高度，见 m_lastContentLines）。
        auto scrollRatioStep(int lines) const -> float;
        // 判断某消息思考块是否处于展开态（默认折叠）。
        auto isThinkingExpanded(const std::string &messageId) const -> bool;
        // 阶段4: 判断某消息编排块是否处于展开态（默认折叠）。
        auto isOrchestrationExpanded(const std::string &messageId) const -> bool;
        // 判断某条 Tool Result 卡片是否处于展开态（默认折叠）。
        auto isToolResultExpanded(const std::string &messageId) const -> bool;

        std::shared_ptr<ThemeManager> m_theme;
        MessageList m_messages;
        std::string m_sessionTitle;
        mutable std::mutex m_messagesMutex;

        // Phase I：思考块逐条展开状态 + 头部屏幕命中框（render 期间经 reflect 填充）。
        std::unordered_map<std::string, bool> m_thinkingExpanded;
        std::unordered_map<std::string, ftxui::Box> m_thinkingBoxes;
        // 阶段4：编排块逐条展开状态 + 头部屏幕命中框（同上）。
        std::unordered_map<std::string, bool> m_orchestrationExpanded;
        std::unordered_map<std::string, ftxui::Box> m_orchestrationBoxes;
        // 工具结果卡片逐条展开状态 + 头部屏幕命中框（render 期间经 reflect 填充）。
        std::unordered_map<std::string, bool> m_toolResultExpanded;
        std::unordered_map<std::string, ftxui::Box> m_toolResultBoxes;

        // Phase E：流式消息 → 增量 Markdown 渲染器（方案 B）。key = messageId。
        // 与 m_messages 同锁保护；渲染时在锁内拷贝 shared_ptr 快照，锁外使用。
        std::unordered_map<std::string, std::shared_ptr<StreamingMarkdownSplitter>> m_streamingRenderers;

        // 滚动状态：内容滚动比例，1.0=贴底(自动跟随最新消息)，0.0=顶部。
        // 用比例而非估算行数，避免估算偏低导致"无滚条/无法滚动"。
        float m_scrollRatio{1.0f};
        // 用户是否主动上滚过（true 时暂停自动跟随）。
        bool m_userScrolledUp{false};
        // 每帧渲染时由 FTXUI 真实测量的内容高度（行），供滚动步长换算；
        // 跨线程（渲染线程写、事件线程读）用 atomic 保护，避免数据竞争。
        std::atomic<int> m_lastContentLines{1};
    };

} // namespace AE::TUI
