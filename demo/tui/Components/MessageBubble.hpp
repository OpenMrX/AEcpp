#pragma once

/// @file demo/tui/Components/MessageBubble.hpp
/// @brief 单条消息气泡：角色着色、文本/流式内容呈现、对齐布局。
/// @details 用户/助手消息区分对齐与配色；流式内容委托 `StreamingMarkdownSplitter`。

#include "../TUIType.hpp"
#include "../Core/Theme.hpp"
#include "StreamingMarkdownSplitter.hpp"

#include <ftxui/dom/elements.hpp>

#include <memory>
#include <string>

namespace AE::TUI
{

    class MessageBubble
    {
    public:
        // thinkingBoxOut / orchestrationBoxOut：可选。指向由调用方（ChatPanel）持有的持久命中框，
        // 思考块/编排块头部经 reflect 写入它。必须由调用方持有稳定地址——不能指向本对象自身成员，
        // 否则局部 bubble 析构后 reflect 会写入悬垂内存，导致点击命中框失效。
        MessageBubble(Message message, std::shared_ptr<ThemeManager> theme,
                      std::shared_ptr<StreamingMarkdownSplitter> splitter = nullptr,
                      ftxui::Box *thinkingBoxOut = nullptr,
                      ftxui::Box *orchestrationBoxOut = nullptr,
                      ftxui::Box *toolResultBoxOut = nullptr);

        auto render() const -> ftxui::Element;
        auto getMessage() const -> const Message &;

        // 若该消息渲染了可点击的思考块头部，返回 true 且 thinkingBox() 给出其屏幕命中框。
        auto hasThinkingBox() const -> bool { return m_hasThinkingBox; }
        auto thinkingBox() const -> ftxui::Box { return m_thinkingBoxOut ? *m_thinkingBoxOut : m_thinkingBox; }
        // 阶段4：编排块头部命中框（同上，供 ChatPanel 点击展开/折叠）。
        auto hasOrchestrationBox() const -> bool { return m_hasOrchestrationBox; }
        auto orchestrationBox() const -> ftxui::Box
        {
            return m_orchestrationBoxOut ? *m_orchestrationBoxOut : m_orchestrationBox;
        }
        // 工具结果卡片头部命中框（同上，支持点击展开/折叠）。
        auto hasToolResultBox() const -> bool { return m_hasToolResultBox; }
        auto toolResultBox() const -> ftxui::Box
        {
            return m_toolResultBoxOut ? *m_toolResultBoxOut : m_toolResultBox;
        }

    private:
        auto renderUserMessage() const -> ftxui::Element;
        auto renderAssistantMessage() const -> ftxui::Element;
        auto renderSystemMessage() const -> ftxui::Element;
        auto renderToolMessage() const -> ftxui::Element;
        // Phase G: 思考消息（isThinking）独立渲染分支。
        auto renderThinkingMessage() const -> ftxui::Element;
        // Phase I: 正文消息附带的思考块（可折叠，位于正文上方）。
        auto renderThinkingBlock() const -> ftxui::Element;
        // 阶段4：编排信息块（可折叠，聚合意图/路由/迭代/工具步摘要，位于思考块上方）。
        auto renderOrchestrationBlock() const -> ftxui::Element;
        auto renderTimestamp() const -> ftxui::Element;
        auto formatTime(const std::chrono::system_clock::time_point &tp) const -> std::string;
        // 消息框宽度约束：最小 20% 终端宽、最大 90% 终端宽，超宽自动换行。
        auto applyWidthConstraints(ftxui::Element element) const -> ftxui::Element;
        // 正文内容宽度约束：在窗口边框/内边距内按终端宽度自适应，确保 paragraph 真正换行而非被截断。
        auto applyContentWidth(ftxui::Element element) const -> ftxui::Element;

        Message m_message;
        std::shared_ptr<ThemeManager> m_theme;
        // Phase E：流式消息的增量 Markdown 渲染器（方案 B）。为空时回退纯文本。
        std::shared_ptr<StreamingMarkdownSplitter> m_splitter;
        // 思考块头部命中框（经 reflect 记录其屏幕区域），供鼠标点击单条展开。
        mutable bool m_hasThinkingBox{false};
        mutable ftxui::Box m_thinkingBox{};
        // 指向调用方持有的持久命中框（非空时优先写入它，避免本对象析构后悬垂）。
        ftxui::Box *m_thinkingBoxOut{nullptr};
        // 编排块头部命中框（同上）。
        mutable bool m_hasOrchestrationBox{false};
        mutable ftxui::Box m_orchestrationBox{};
        ftxui::Box *m_orchestrationBoxOut{nullptr};
        // 工具结果卡片头部命中框（同上）。
        mutable bool m_hasToolResultBox{false};
        mutable ftxui::Box m_toolResultBox{};
        ftxui::Box *m_toolResultBoxOut{nullptr};
    };

} // namespace AE::TUI
