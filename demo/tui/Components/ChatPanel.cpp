#include "ChatPanel.hpp"

#include <ftxui/component/component.hpp>  // for CatchEvent, Renderer, flex
#include <ftxui/component/event.hpp>      // for Event, Mouse
#include <ftxui/dom/elements.hpp>         // for focusPositionRelative, yframe
#include <ftxui/screen/terminal.hpp>      // for Terminal::Size() (窗口化视口计算)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <print>

namespace AE::TUI
{

    ChatPanel::ChatPanel(std::shared_ptr<ThemeManager> theme)
        : m_theme(std::move(theme))
    {
    }

    auto ChatPanel::render() -> ftxui::Element
    {
        try
        {

            std::vector<Message> visible;
            std::unordered_map<std::string, std::shared_ptr<StreamingMarkdownSplitter>> visRenderers;
            bool userScrolledUp = false;
            {
                std::scoped_lock lock(m_messagesMutex);
                const auto &msgs = m_messages;
                if (msgs.empty())
                {
                    return ftxui::vbox({
                               ftxui::filler(),
                               renderWelcome() | ftxui::center,
                               ftxui::filler(),
                           }) |
                           ftxui::flex;
                }

                userScrolledUp = m_userScrolledUp;

                // 拷贝全部消息（含流式渲染器快照），在锁外完整构建 DOM，避免渲染阻塞写线程。
                // 注：长对话下此拷贝/渲染会变重，后续如性能吃紧可改用渲染缓存或读锁；
                // 当前以"正确性优先"，首轮到 N 轮对话都能完整滚动。
                visible.reserve(msgs.size());
                for (const auto &m : msgs)
                    visible.push_back(m);
                for (auto &m : visible)
                {
                    if (m.status == MessageStatus::Streaming)
                    {
                        auto it = m_streamingRenderers.find(m.id);
                        if (it != m_streamingRenderers.end())
                            visRenderers[m.id] = it->second;
                    }
                }
            }

            // 完整构建所有消息与间隙，让 FTXUI 按真实高度测量并裁剪。
            m_thinkingBoxes.clear();
            m_orchestrationBoxes.clear();
            m_toolResultBoxes.clear();
            std::vector<ftxui::Element> slots;
            slots.reserve(visible.size() * 2);
            for (std::size_t i = 0; i < visible.size(); ++i)
            {
                auto msg = std::move(visible[i]);
                // Phase I: 逐条思考块展开标志——默认折叠（主显正文），仅被点击的消息展开。
                msg.thinkingCollapsed = !isThinkingExpanded(msg.id);
                // 阶段4: 逐条编排块展开标志——默认折叠，仅被点击的消息展开。
                msg.orchestrationCollapsed = !isOrchestrationExpanded(msg.id);
                msg.toolResultCollapsed = !isToolResultExpanded(msg.id);
                // Phase E：流式消息传递增量渲染器，完成消息不传（nullptr → 整段渲染）。
                std::shared_ptr<StreamingMarkdownSplitter> splitter;
                if (msg.status == MessageStatus::Streaming)
                {
                    auto it = visRenderers.find(msg.id);
                    if (it != visRenderers.end())
                        splitter = it->second;
                }
                // 思考块命中框写入 ChatPanel 持有的持久 Box（m_thinkingBoxes），生命周期覆盖整个
                // 布局期与下一帧事件处理，reflect 不会因局部 bubble 析构而悬垂。
                MessageBubble bubble(msg, m_theme, std::move(splitter),
                                     &m_thinkingBoxes[msg.id], &m_orchestrationBoxes[msg.id],
                                     &m_toolResultBoxes[msg.id]);
                ftxui::Element bubbleElem = bubble.render();
                if (!bubble.hasThinkingBox())
                {
                    // 该消息无思考块，移除此持久条目，避免误命中默认 Box(0,0,0,0)。
                    m_thinkingBoxes.erase(msg.id);
                }
                if (!bubble.hasOrchestrationBox())
                {
                    // 该消息无编排块，同上。
                    m_orchestrationBoxes.erase(msg.id);
                }

                if (!bubble.hasToolResultBox())
                {
                    // 该消息无工具结果卡片，移除此持久条目。
                    m_toolResultBoxes.erase(msg.id);
                }
                slots.push_back(std::move(bubbleElem));
                if (i + 1 >= visible.size())
                    continue;
                const auto &nx = visible[i + 1];
                if (nx.role == MessageRole::User)
                {
                    // 新一轮：用户发送前加一条居中灰色横线（非全宽贯穿，占 80%），区分轮次；
                    // 用重复的 "─" 画一条固定宽度的水平线，避免 ftxui::separator() 在窄分配时被误渲为竖线。
                    const int dimx = ftxui::Terminal::Size().dimx;
                    const int sepW = std::max(10, dimx * 9 / 10);
                    std::string dashLine;
                    dashLine.reserve(static_cast<std::size_t>(sepW) * 3);
                    for (int k = 0; k < sepW; ++k)
                        dashLine += "─";
                    slots.push_back(ftxui::vbox({
                        ftxui::separatorEmpty(),
                        ftxui::hbox({
                            ftxui::filler(),
                            ftxui::text(dashLine) | ftxui::color(m_theme->colors().muted),
                            ftxui::filler(),
                        }),
                        ftxui::separatorEmpty(),
                    }));
                }
                else
                {
                    // 其它相邻消息（含用户↔助手配对）：仅留小间距，不再画配对连接竖线。
                    slots.push_back(ftxui::separatorEmpty());
                }
            }

            auto document = ftxui::vbox(std::move(slots));

            // 用 FTXUI 真实测量内容高度（含 markdown/代码块/中文/边框），替代不准确的行数估算，
            // 供滚动步长换算（每次渲染刷新；跨线程读取用 atomic 保护）。
            document->ComputeRequirement();
            const int contentLines = document->requirement().min_y;
            m_lastContentLines.store(std::max(1, contentLines));

            // 自动跟随底部 → focusY=1.0；手动滚动 → 用记录的比例 m_scrollRatio。
            const float focusY = userScrolledUp ? m_scrollRatio : 1.0f;
            // vscroll_indicator 依据 document 真实测量高度与视口的差值自动绘制/定位右侧滚动条，
            // 仅当内容真正溢出时才出现，彻底解决"估算偏低导致无滚条、无法滚动"。
            auto inner = document |
                         ftxui::focusPositionRelative(0.0f, focusY) |
                         ftxui::yframe |
                         ftxui::vscroll_indicator |
                         ftxui::flex;
            return inner;
        }
        catch (const std::exception &e)
        {
            return ftxui::vbox({ftxui::text(std::format("Render error: {}", e.what()))}) | ftxui::flex;
        }
        catch (...)
        {
            return ftxui::vbox({ftxui::text("Render error: unknown")}) | ftxui::flex;
        }
    }

    auto ChatPanel::renderWelcome() const -> ftxui::Element
    {
        return ftxui::vbox({
            ftxui::text("  AE AI Assistant  ") | ftxui::bold | ftxui::center | ftxui::color(m_theme->colors().primary),
            ftxui::separatorEmpty(),
            ftxui::text("Welcome to the terminal AI interface") | ftxui::center | ftxui::color(m_theme->colors().muted),
            ftxui::separatorEmpty(),
            ftxui::text("Type your message below and press Enter to start") | ftxui::center | ftxui::color(m_theme->colors().muted),
            ftxui::separatorEmpty(),
            ftxui::separatorEmpty(),
            ftxui::hbox({
                ftxui::filler(),
                ftxui::text("Try:") | ftxui::color(m_theme->colors().muted),
                ftxui::filler(),
            }),
            ftxui::separator(),
            ftxui::hbox({
                ftxui::filler(),
                ftxui::text("  • Explain a concept") | ftxui::color(m_theme->colors().secondary),
                ftxui::filler(),
            }),
            ftxui::hbox({
                ftxui::filler(),
                ftxui::text("  • Write some code") | ftxui::color(m_theme->colors().secondary),
                ftxui::filler(),
            }),
            ftxui::hbox({
                ftxui::filler(),
                ftxui::text("  • Run a tool") | ftxui::color(m_theme->colors().secondary),
                ftxui::filler(),
            }),
        });
    }

    auto ChatPanel::addMessage(const Message &message) -> void
    {
        std::scoped_lock lock(m_messagesMutex);
        m_messages.push_back(message);
        // 新消息到达时：若用户在底部则保持自动跟随（m_scrollRatio=1.0 由 render 解析为贴底）。
        if (!m_userScrolledUp)
        {
            m_scrollRatio = 1.0f;
        }
    }

    auto ChatPanel::updateStreamingMessage(const std::string &messageId, std::string_view delta) -> void
    {
        std::scoped_lock lock(m_messagesMutex);
        for (auto &msg : m_messages)
        {
            if (msg.id == messageId)
            {
                msg.content += delta;
                msg.status = MessageStatus::Streaming;
                break;
            }
        }
        // Phase E：增量渲染器同步追加 delta（懒创建）。
        // m_streamingRenderers 与 m_messages 同锁保护（本函数已持 m_messagesMutex，
        // render() 读该 map 时也持同一锁），直接访问即可，无需独立 mutex。
        auto &splitter = m_streamingRenderers[messageId];
        if (!splitter)
        {
            splitter = std::make_shared<StreamingMarkdownSplitter>();
        }
        splitter->append(delta);
        if (!m_userScrolledUp)
        {
            m_scrollRatio = 1.0f;
        }
    }

    auto ChatPanel::isStreaming() const -> bool
    {
        // 与 updateStreamingMessage/finishStreamingMessage 同锁保护：m_streamingRenderers
        // 非空即表示存在在途流式消息（update 时插入、finish/清空时移除）。
        std::scoped_lock lock(m_messagesMutex);
        return !m_streamingRenderers.empty();
    }

    auto ChatPanel::setMessageContent(const std::string &messageId, std::string_view content) -> bool
    {
        // 关键：直接覆盖消息内容（替换而非追加），用于工具结果回填、最终答案写入等场景
        std::scoped_lock lock(m_messagesMutex);
        bool found = false;
        for (auto &msg : m_messages)
        {
            if (msg.id == messageId)
            {
                msg.content = std::string{content};
                found = true;
                break;
            }
        }
        // 内容被整体覆盖 → 增量渲染器状态失效，重置（后续按整段渲染）。
        if (found)
        {
            m_streamingRenderers.erase(messageId);
        }
        return found;
    }

    auto ChatPanel::finishStreamingMessage(const std::string &messageId) -> void
    {
        std::scoped_lock lock(m_messagesMutex);
        for (auto &msg : m_messages)
        {
            if (msg.id == messageId)
            {
                msg.status = MessageStatus::Completed;
                break;
            }
        }
        // Phase E：结束增量渲染器并释放（消息转为 Completed 后走整段渲染）。
        auto it = m_streamingRenderers.find(messageId);
        if (it != m_streamingRenderers.end())
        {
            it->second->finish();
            m_streamingRenderers.erase(it);
        }
    }

    auto ChatPanel::setThinkingContent(const std::string &messageId, std::string_view thinking) -> void
    {
        std::scoped_lock lock(m_messagesMutex);
        for (auto &msg : m_messages)
        {
            if (msg.id == messageId)
            {
                msg.thinkingContent += thinking;
                break;
            }
        }
        if (!m_userScrolledUp)
        {
            m_scrollRatio = 1.0f;
        }
    }

    auto ChatPanel::isThinkingExpanded(const std::string &messageId) const -> bool
    {
        auto it = m_thinkingExpanded.find(messageId);
        return it != m_thinkingExpanded.end() && it->second;
    }

    auto ChatPanel::setOrchestrationInfo(const std::string &messageId, OrchestrationInfo info) -> void
    {
        std::scoped_lock lock(m_messagesMutex);
        for (auto &msg : m_messages)
        {
            if (msg.id == messageId)
            {
                msg.orchestration = std::move(info);
                msg.orchestrationCollapsed = true; // 默认折叠
                break;
            }
        }
        if (!m_userScrolledUp)
        {
            m_scrollRatio = 1.0f;
        }
    }

    auto ChatPanel::isOrchestrationExpanded(const std::string &messageId) const -> bool
    {
        auto it = m_orchestrationExpanded.find(messageId);
        return it != m_orchestrationExpanded.end() && it->second;
    }

    auto ChatPanel::handleOrchestrationClick(int x, int y) -> bool
    {
        // 与 handleThinkingClick 同语义：命中某条消息编排块头部则翻转其展开/折叠。
        for (const auto &[id, box] : m_orchestrationBoxes)
        {
            if (x >= box.x_min && x <= box.x_max && y >= box.y_min && y <= box.y_max)
            {
                auto it = m_orchestrationExpanded.find(id);
                const bool nowExpanded = (it == m_orchestrationExpanded.end()) ? true : !it->second;
                m_orchestrationExpanded[id] = nowExpanded;
                return true;
            }
        }
        return false;
    }

    auto ChatPanel::handleThinkingClick(int x, int y) -> bool
    {
        // 仅 TUI 线程调用（渲染与事件处理同线程）。点击命中某条消息的思考块头部
        // 时，翻转该条展开/折叠状态。返回 true 表示已消费该点击事件。
        for (const auto &[id, box] : m_thinkingBoxes)
        {
            if (x >= box.x_min && x <= box.x_max && y >= box.y_min && y <= box.y_max)
            {
                auto it = m_thinkingExpanded.find(id);
                const bool nowExpanded = (it == m_thinkingExpanded.end()) ? true : !it->second;
                m_thinkingExpanded[id] = nowExpanded;
                return true;
            }
        }
        return false;
    }

    auto ChatPanel::isToolResultExpanded(const std::string &messageId) const -> bool
    {
        auto it = m_toolResultExpanded.find(messageId);
        return it != m_toolResultExpanded.end() && it->second;
    }

    auto ChatPanel::handleToolResultClick(int x, int y) -> bool
    {
        for (const auto &[id, box] : m_toolResultBoxes)
        {
            if (x >= box.x_min && x <= box.x_max && y >= box.y_min && y <= box.y_max)
            {
                auto it = m_toolResultExpanded.find(id);
                const bool nowExpanded = (it == m_toolResultExpanded.end()) ? true : !it->second;
                m_toolResultExpanded[id] = nowExpanded;
                return true;
            }
        }
        return false;
    }

    auto ChatPanel::clearMessages() -> void
    {
        std::scoped_lock lock(m_messagesMutex);
        m_messages.clear();
        m_streamingRenderers.clear();
        m_thinkingExpanded.clear();
        m_thinkingBoxes.clear();
        m_orchestrationExpanded.clear();
        m_orchestrationBoxes.clear();
        m_toolResultExpanded.clear();
        m_toolResultBoxes.clear();
        m_scrollRatio = 1.0f;
        m_userScrolledUp = false;
    }

    auto ChatPanel::setMessages(std::vector<Message> messages) -> void
    {
        std::scoped_lock lock(m_messagesMutex);
        m_messages = std::move(messages);
        m_streamingRenderers.clear(); // 历史回读均为已完成消息，无在途流式渲染器
        m_thinkingExpanded.clear();   // 切换会话后重置逐条展开状态
        m_thinkingBoxes.clear();
        m_orchestrationExpanded.clear(); // 切换会话后重置编排块逐条展开状态
        m_orchestrationBoxes.clear();
        m_toolResultExpanded.clear();
        m_toolResultBoxes.clear();
        m_scrollRatio = 1.0f;      // 切换会话后自动跟随底部
        m_userScrolledUp = false;
    }

    auto ChatPanel::setMessageTokens(const std::string &messageId, int32_t inputTokens,
                                     int32_t outputTokens, int32_t reasoningTokens,
                                     int32_t totalTokens) -> void
    {
        std::scoped_lock lock(m_messagesMutex);
        for (auto &msg : m_messages)
        {
            if (msg.id == messageId)
            {
                msg.inputTokens = inputTokens;
                msg.outputTokens = outputTokens;
                msg.reasoningTokens = reasoningTokens;
                msg.totalTokens = totalTokens;
                break;
            }
        }
    }

    auto ChatPanel::setLastUserMessageTokens(int32_t inputTokens) -> void
    {
        if (inputTokens <= 0)
        {
            return;
        }
        std::scoped_lock lock(m_messagesMutex);
        // 倒序找最近一条 User 消息回填输入 token（相邻请求 prompt_tokens 差值）。
        for (auto it = m_messages.rbegin(); it != m_messages.rend(); ++it)
        {
            if (it->role == MessageRole::User)
            {
                it->inputTokens = inputTokens;
                it->totalTokens = inputTokens;
                break;
            }
        }
    }

    auto ChatPanel::getMessages() const -> MessageList
    {
        std::scoped_lock lock(m_messagesMutex);
        return m_messages;
    }

    auto ChatPanel::setSessionTitle(const std::string &title) -> void
    {
        m_sessionTitle = title;
    }

    // ─── 滚动控制 ───────────────────────────────────────────────────────────
    //
    // 滚动语义（根治"估算行数不准 → 无滚条/无法滚动/内容截断"）：
    // - 渲染用 FTXUI 原生 `vscroll_indicator + yframe`，依据元素真实测量高度自动显示/定位滚动条，
    //   仅当内容真正溢出才出现，彻底解决"估算偏低 → 无滚条"。
    // - 滚动状态以比例 m_scrollRatio ∈ [0,1] 表达：1.0=贴底(自动跟随最新消息)，0.0=顶部。
    // - m_userScrolledUp = true → 手动滚动（按 m_scrollRatio 渲染 focusY）；false → 自动跟随底部。
    // - 步长由"真实内容高度 m_lastContentLines（render 每帧测量）"换算，长对话每格更精细，短对话不跳变。

    auto ChatPanel::scrollStepLines() const -> int
    {
        // 名义步长：视口 1/6（约 3~6 行），作为"每次滚轮应滚动的内容行数"（再折算为比例）。
        const int dimy = ftxui::Terminal::Size().dimy;
        const int viewport = std::max(1, dimy - 4);
        return std::max(3, viewport / 6);
    }

    // 由"应滚动的内容行数"折算为比例步长（相对真实内容高度），杜绝估算误差放大。
    auto ChatPanel::scrollRatioStep(int lines) const -> float
    {
        const int total = m_lastContentLines.load();
        if (total <= 0)
            return 0.05f;
        // 至少 1% 一跳（极长内容也能逐步推进）；至多一次滚一整屏。
        return std::clamp(static_cast<float>(lines) / static_cast<float>(total), 0.01f, 1.0f);
    }

    auto ChatPanel::scrollUp(int lines) -> void
    {
        // 用户上滚（鼠标滚轮向上 / PgUp）→ 看更早消息 → 比例减小（趋近顶部 0）。
        std::scoped_lock lock(m_messagesMutex);
        m_userScrolledUp = true;
        const float step = scrollRatioStep(lines);
        m_scrollRatio = std::max(0.0f, m_scrollRatio - step);
    }

    auto ChatPanel::scrollDown(int lines) -> void
    {
        // 用户下滚（鼠标滚轮向下 / PgDn）→ 看更新消息 → 比例增大（趋近底部 1）。
        std::scoped_lock lock(m_messagesMutex);
        // 已在自动跟随底部，下滚无意义。
        if (!m_userScrolledUp)
            return;
        const float step = scrollRatioStep(lines);
        m_scrollRatio += step;
        // 到达底部即恢复自动跟随，避免比例略小于 1 时残留死区。
        if (m_scrollRatio >= 0.999f)
        {
            m_scrollRatio = 1.0f;
            m_userScrolledUp = false;
        }
    }

    auto ChatPanel::scrollToTop() -> void
    {
        std::scoped_lock lock(m_messagesMutex);
        m_userScrolledUp = true;
        m_scrollRatio = 0.0f;
    }

    auto ChatPanel::scrollToBottom() -> void
    {
        std::scoped_lock lock(m_messagesMutex);
        m_scrollRatio = 1.0f;
        m_userScrolledUp = false;
    }

    auto ChatPanel::getComponent() -> ftxui::Component
    {
        // 关键：返回一个 CatchEvent 包裹的 Renderer 组件。
        // - CatchEvent 在组件层级捕获鼠标滚轮事件：鼠标事件不受"焦点"限制，
        //   由顶层向下逐层分发到每个 CatchEvent，因此滚轮滚动必定生效
        //   （前提是终端启用 mouse tracking，ftxui 默认开启）。
        // - 键盘滚动（PgUp/PgDn/Home/End）由 TUI 顶层 eventHandler 兜底处理，
        //   因为纯 Renderer 不可聚焦，不会在 Container::Vertical 中抢占焦点，
        //   键盘事件会冒泡上去被顶层拦截。两层合起来保证滚轮 + 键盘都能滚动。
        auto inner = ftxui::Renderer([this]
                                     { return render(); });

        auto scrollable = ftxui::CatchEvent(
            inner,
            [this](ftxui::Event event) -> bool
            {
                if (!event.is_mouse())
                    return false;
                const auto &mouse = event.mouse();
                if (mouse.button == ftxui::Mouse::WheelUp)
                {
                    scrollUp(scrollStepLines());
                    return true;
                }
                if (mouse.button == ftxui::Mouse::WheelDown)
                {
                    scrollDown(scrollStepLines());
                    return true;
                }
                // 左键点击：命中某条消息的思考块/编排块头部则翻转该条展开/折叠（单条独立）。
                if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed)
                {
                    if (handleThinkingClick(mouse.x, mouse.y))
                        return true;
                    if (handleOrchestrationClick(mouse.x, mouse.y))
                        return true;
                    if (handleToolResultClick(mouse.x, mouse.y))
                        return true;
                }
                return false;
            });

        return scrollable | ftxui::flex;
    }

} // namespace AE::TUI
