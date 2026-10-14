#include "MessageBubble.hpp"
#include "MarkdownRenderer.hpp"

#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <print>
#include <sstream>

namespace AE::TUI
{

    MessageBubble::MessageBubble(Message message, std::shared_ptr<ThemeManager> theme,
                                  std::shared_ptr<StreamingMarkdownSplitter> splitter,
                                  ftxui::Box *thinkingBoxOut,
                                  ftxui::Box *orchestrationBoxOut,
                                  ftxui::Box *toolResultBoxOut)
        : m_message(std::move(message)),
          m_theme(std::move(theme)),
          m_splitter(std::move(splitter)),
          m_thinkingBoxOut(thinkingBoxOut),
          m_orchestrationBoxOut(orchestrationBoxOut),
          m_toolResultBoxOut(toolResultBoxOut)
    {
    }

    auto MessageBubble::applyWidthConstraints(ftxui::Element element) const -> ftxui::Element
    {
        // 宽度算法（Phase D）：基于终端列宽 dimx 实时自适应，不使用硬编码像素。
        //   maxW = dimx * 90%  最大宽度（超宽内容自动换行）
        //   minW = dimx * 40%  最小宽度（保证窗口/终端列数变化时气泡随之缩放展开，
        //                       而不是缩成一窄条固定住；同时避免短内容被撑成满宽）
        // 组合成 [minW, maxW] 区间：短内容取 minW，长内容撑满到 maxW。
        const int dimx = ftxui::Terminal::Size().dimx;
        const int maxW = std::max(1, dimx * 9 / 10);
        const int minW = std::max(1, dimx * 4 / 10);
        return element |
               ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, maxW) |
               ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, minW);
    }

    auto MessageBubble::applyContentWidth(ftxui::Element element) const -> ftxui::Element
    {
        // 关键修复：把宽度约束直接施加在正文内容（paragraph / vbox）上，而不是只约束外层 window。
        // 只约束 window 会导致 paragraph 仍按原始未换行宽度计算需求，随后被 window 的 maxW 裁剪，
        // 表现为“超过消息框的内容不换行，只显示一半”。此处把内容最大宽度设为窗口可用内宽
        // （窗口边框占 2 列，再留 2 列内边距保险），使 paragraph 在布局阶段就按此宽度折行。
        const int dimx = ftxui::Terminal::Size().dimx;
        const int maxBubbleW = std::max(1, dimx * 9 / 10);
        const int maxContentW = std::max(1, maxBubbleW - 4);
        return element |
               ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, maxContentW);
    }

    auto MessageBubble::render() const -> ftxui::Element
    {
        // Phase G: isThinking 消息走独立思考渲染分支。
        if (m_message.isThinking)
        {
            return renderThinkingMessage();
        }
        switch (m_message.role)
        {
        case MessageRole::User:
            return renderUserMessage();
        case MessageRole::Assistant:
            return renderAssistantMessage();
        case MessageRole::System:
            return renderSystemMessage();
        case MessageRole::Tool:
            return renderToolMessage();
        default:
            return ftxui::text(m_message.content);
        }
    }

    auto MessageBubble::getMessage() const -> const Message &
    {
        return m_message;
    }

    auto MessageBubble::renderUserMessage() const -> ftxui::Element
    {
        // 用户输入 token：服务端真实值（相邻请求 prompt_tokens 差值）优先；未回填时回退字符近似。
        const int inputTokens = (m_message.inputTokens > 0)
                                    ? m_message.inputTokens
                                    : static_cast<int>(m_message.content.size());
        auto header = ftxui::hbox({
            ftxui::text(" You") | ftxui::bold,
            ftxui::text("  ") | ftxui::flex,
            ftxui::text(std::format("Token：{}", inputTokens)) |
                ftxui::dim | ftxui::color(m_theme->colors().muted),
            ftxui::text("   "),
            renderTimestamp(),
        });

        // 用户消息为完整内容，先关闭 Markdown 解析直传纯文本，便于排查
        // 是 Markdown 解析/渲染导致正文不显示，还是数据未到达。
        auto content = ftxui::vbox({
            applyContentWidth(
                ftxui::paragraph(breakLongWords(m_message.content)) |
                    ftxui::color(m_theme->colors().foreground) |
                    ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, 1)),
        });

        auto bubble = ftxui::window(ftxui::text(""), ftxui::vbox({header, ftxui::separator(), content}));

        // 右对齐（Phase C）：hbox({ filler, bubble })。
        // 注意：不再在消息前加 text("")，消息间间距由 ChatPanel 统一控制，
        // 以便实现“成对内小间距 + 成对间大间距”与连接线。
        return ftxui::hbox({
            ftxui::filler(),
            applyWidthConstraints(bubble),
        });
    }

    auto MessageBubble::renderAssistantMessage() const -> ftxui::Element
    {
        std::string roleName = " Assistant";

        // token 计数：服务端真实值（output/reasoning）优先；未回填时回退字符近似。
        // 真实值口径与底部 Ctx 上下文占比一致（均来自服务端 usage）。
        int thinkingTokens;
        int bodyTokens;
        if (m_message.outputTokens > 0 || m_message.reasoningTokens > 0)
        {
            thinkingTokens = m_message.reasoningTokens;
            bodyTokens = m_message.outputTokens;
        }
        else
        {
            thinkingTokens = static_cast<int>(m_message.thinkingContent.size());
            bodyTokens = static_cast<int>(m_message.content.size());
        }
        const int totalTokens = thinkingTokens + bodyTokens;
        auto header = ftxui::hbox({
            ftxui::text(roleName) | ftxui::bold | ftxui::color(m_theme->colors().primary),
            ftxui::text("  ") | ftxui::flex,
            ftxui::text(std::format("Token：{}(思考{}/正文{})", totalTokens, thinkingTokens, bodyTokens)) |
                ftxui::dim | ftxui::color(m_theme->colors().muted),
            ftxui::text("   "),
            renderTimestamp(),
        });

        ftxui::Element content;
        if (m_message.content.empty() && m_message.status == MessageStatus::Streaming)
        {
            // 紧凑的"思考中"指示器（避免空大框）
            content = ftxui::hbox({
                ftxui::text("thinking...") | ftxui::italic | ftxui::color(m_theme->colors().muted),
                ftxui::text(" ") | ftxui::color(m_theme->colors().primary) | ftxui::blink,
            });
        }
        else if (m_message.status == MessageStatus::Streaming)
        {
            // Phase E：流式消息用增量 Markdown 渲染（方案 B）+ 光标。
            if (m_splitter)
            {
                content = applyContentWidth(
                    ftxui::hbox({
                        m_splitter->render(m_theme),
                        ftxui::text("_") | ftxui::color(m_theme->colors().primary) | ftxui::blink,
                    }) | ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, 1));
            }
            else
            {
                // 无增量渲染器时回退到 Markdown 渲染（仍是逐字增量，仅不做分段缓冲）。
                content = applyContentWidth(
                    ftxui::hbox({
                        MarkdownRenderer::render(m_message.content, m_theme),
                        ftxui::text("_") | ftxui::color(m_theme->colors().primary) | ftxui::blink,
                    }) | ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, 1));
            }
        }
        else
        {
            // 已完成的助手消息用纯文本段落，避免 MarkdownRenderer 解析异常导致内容消失。
            // 后续 MarkdownRenderer 稳定后可重新启用；当前以稳定显示为优先。
            if (m_message.content.empty() && !m_message.thinkingContent.empty())
            {
                // 模型仅返回思考过程、未输出正文时，不能渲染成空段落（看起来像消息框被压扁）。
                // 用占位提示说明状态，并引导用户点击上方思考块查看详细过程。
                content = ftxui::vbox({
                    applyContentWidth(
                        ftxui::paragraph("（未输出正文，仅含思考过程，可点击上方展开查看）") |
                            ftxui::italic |
                            ftxui::color(m_theme->colors().muted) |
                            ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, 1)),
                });
            }
            else
            {
                content = ftxui::vbox({
                    applyContentWidth(
                        ftxui::paragraph(breakLongWords(m_message.content)) |
                            ftxui::color(m_theme->colors().foreground) |
                            ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, 1)),
                });
            }
        }

        if (m_message.toolCallName.has_value())
        {
            content = ftxui::vbox({
                content,
                ftxui::separator(),
                ftxui::hbox({
                    ftxui::text("* ") | ftxui::color(m_theme->colors().accent),
                    ftxui::text(m_message.toolCallName.value()) | ftxui::bold | ftxui::color(m_theme->colors().accent),
                }),
            });
        }

        // DEBUG: 若正文为空且没有思考内容，UI 上提示并暴露长度。
        // 仅当 truly empty（无正文、无思考）时才显示 debug 占位，避免覆盖已有思考块。
        if (m_message.content.empty() && m_message.thinkingContent.empty())
        {
            content = applyContentWidth(
                ftxui::vbox({
                    ftxui::text("[等待正文内容...]") | ftxui::color(m_theme->colors().accent),
                    ftxui::text(std::format("[debug] contentLen=0 thinkingLen={} status={}",
                                              m_message.thinkingContent.size(),
                                              static_cast<int>(m_message.status))) |
                        ftxui::color(m_theme->colors().muted),
                }) | ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, 2));
        }

        // 阶段4: 编排信息块（聚合意图/路由/迭代/工具步摘要），折叠呈现，位于思考块上方。
        if (m_message.orchestration.visible)
        {
            content = ftxui::vbox({
                renderOrchestrationBlock(),
                ftxui::separator(),
                content,
            });
        }

        // Phase I: 思考内容（reasoning_content）合并到正文消息，渲染为可折叠思考块，
        // 位于正文上方（用户反馈"思考显示在回复下方"属错误位置）。默认折叠，主要显示正文。
        if (!m_message.thinkingContent.empty())
        {
            content = ftxui::vbox({
                renderThinkingBlock(),
                ftxui::separator(),
                content,
            });
        }

        auto bubble = ftxui::window(ftxui::text(""), ftxui::vbox({header, ftxui::separator(), content}));

        static const bool noWindow = std::getenv("AE_TUI_NO_WINDOW") != nullptr;
        if (noWindow)
        {
            return ftxui::hbox({
                bubble,
                ftxui::filler(),
            });
        }

        // 注意：消息前不再加 text("")，消息间间距与连接线由 ChatPanel 统一控制。
        return ftxui::hbox({
            applyWidthConstraints(bubble),
            ftxui::filler(),
        });
    }

    auto MessageBubble::renderSystemMessage() const -> ftxui::Element
    {
        auto content = ftxui::paragraph(breakLongWords(m_message.content)) |
                       ftxui::color(m_theme->colors().muted) |
                       ftxui::italic;

        auto bubble = ftxui::window(ftxui::text(""), ftxui::vbox({content}));

        return ftxui::hbox({
            applyWidthConstraints(bubble),
            ftxui::filler(),
        });
    }

    // Phase G: 思考消息（reasoning_content）独立渲染分支。
    // 视觉上与正文区分：muted 色 + italic，流式时带闪烁光标。
    auto MessageBubble::renderThinkingMessage() const -> ftxui::Element
    {
        auto header = ftxui::hbox({
            ftxui::text(" Thinking") | ftxui::bold | ftxui::color(m_theme->colors().muted),
            ftxui::text("  ") | ftxui::flex,
            renderTimestamp(),
        });

        ftxui::Element content;
        if (m_message.content.empty() && m_message.status == MessageStatus::Streaming)
        {
            content = ftxui::hbox({
                ftxui::text("thinking...") | ftxui::italic | ftxui::color(m_theme->colors().muted),
                ftxui::text(" ") | ftxui::color(m_theme->colors().muted) | ftxui::blink,
            });
        }
        else if (m_message.status == MessageStatus::Streaming)
        {
            content = ftxui::hbox({
                ftxui::paragraph(m_message.content) | ftxui::italic | ftxui::color(m_theme->colors().muted),
                ftxui::text("_") | ftxui::color(m_theme->colors().muted) | ftxui::blink,
            });
        }
        else
        {
            content = ftxui::paragraph(breakLongWords(m_message.content)) | ftxui::italic | ftxui::color(m_theme->colors().muted);
        }

        // 修复：若消息同时携带正文（主语），在思考内容之后附加展示，避免只显示思考而缺主语。
        if (!m_message.content.empty())
        {
            ftxui::Element subject =
                ftxui::paragraph(breakLongWords(m_message.content)) | ftxui::color(m_theme->colors().foreground);
            content = ftxui::vbox({
                content,
                ftxui::separator(),
                ftxui::hbox({
                    ftxui::text(" 正文") | ftxui::bold | ftxui::color(m_theme->colors().primary),
                }),
                subject,
            });
        }

        auto bubble = ftxui::window(ftxui::text(""), ftxui::vbox({header, ftxui::separator(), content}));

        return ftxui::hbox({
            applyWidthConstraints(bubble) |
                ftxui::color(m_theme->colors().muted),
            ftxui::filler(),
        });
    }

    // Phase I: 正文消息附带的思考块（可折叠）。
    // 折叠态：单行摘要（"▸ 思考过程 (已折叠)"），不展示详细思考内容；
    // 展开态：完整思考内容（muted + italic），由全局 Ctrl+K 切换。
    auto MessageBubble::renderThinkingBlock() const -> ftxui::Element
    {
        // 思考块头部作为可点击区域（reflect 记录其屏幕命中框），鼠标点击单条展开/折叠。
        auto header = [this](const std::string &mark, const std::string &hint) -> ftxui::Element {
            auto elem = ftxui::hbox({
                ftxui::text(mark) | ftxui::bold | ftxui::color(m_theme->colors().muted),
                ftxui::text(" 思考过程") | ftxui::bold | ftxui::color(m_theme->colors().muted),
                ftxui::text(hint) | ftxui::dim | ftxui::color(m_theme->colors().muted),
            });
            // 记录头部命中框供点击命中；underlined 提示可点击。
            // 优先写入调用方持有的持久框（生命周期长于本 bubble），避免局部对象析构后悬垂。
            auto &target = m_thinkingBoxOut ? *m_thinkingBoxOut : m_thinkingBox;
            elem = elem | ftxui::reflect(target) | ftxui::underlined;
            m_hasThinkingBox = true;
            return elem;
        };

        if (m_message.thinkingCollapsed)
        {
            return header("▸", "  (已折叠 · 点击展开)");
        }
        return ftxui::vbox({
            header("▾", "  (点击折叠)"),
            ftxui::paragraph(breakLongWords(m_message.thinkingContent)) |
                ftxui::italic |
                ftxui::color(m_theme->colors().muted),
        });
    }

    // 阶段4：编排信息块（折叠/展开切换）。标题行聚合「意图→路由 · step x/y · 工具 N」，
    // 展开后逐步列出明细（工具步摘要等）。
    auto MessageBubble::renderOrchestrationBlock() const -> ftxui::Element
    {
        const auto &orch = m_message.orchestration;
        const int toolCount = static_cast<int>(std::count_if(
            orch.steps.begin(), orch.steps.end(),
            [](const OrchestrationStep &s) { return s.phase == "tool"; }));

        // 标题行（可点击区域）：聚合摘要 + 折叠提示。
        std::string titleParts;
        if (!orch.intent.empty())
        {
            titleParts += orch.intent;
        }
        if (!orch.route.empty())
        {
            titleParts += (titleParts.empty() ? "" : "→") + orch.route;
        }
        if (orch.stepTotal > 0)
        {
            titleParts += (titleParts.empty() ? "" : " ") +
                          std::format("step {}/{}", orch.stepIndex, orch.stepTotal);
        }
        if (toolCount > 0)
        {
            titleParts += (titleParts.empty() ? "" : " ") + std::format("工具 {}", toolCount);
        }
        if (titleParts.empty())
        {
            titleParts = "编 排";
        }

        auto header = [this, &titleParts](const std::string &mark, const std::string &hint) -> ftxui::Element {
            auto elem = ftxui::hbox({
                ftxui::text(mark) | ftxui::bold | ftxui::color(m_theme->colors().secondary),
                ftxui::text(" 编排 · " + titleParts) | ftxui::bold | ftxui::color(m_theme->colors().secondary),
                ftxui::text(hint) | ftxui::dim | ftxui::color(m_theme->colors().muted),
            });
            auto &target = m_orchestrationBoxOut ? *m_orchestrationBoxOut : m_orchestrationBox;
            elem = elem | ftxui::reflect(target) | ftxui::underlined;
            m_hasOrchestrationBox = true;
            return elem;
        };

        if (m_message.orchestrationCollapsed)
        {
            return header("▸", "  (已折叠 · 点击展开)");
        }

        std::vector<ftxui::Element> rows;
        rows.reserve(orch.steps.size() + 1);
        rows.push_back(header("▾", "  (点击折叠)"));
        for (const auto &step : orch.steps)
        {
            std::string badge = step.phase == "tool" ? "⚙ 工具"
                               : step.phase == "iterate" ? "↻ 迭代"
                               : step.phase == "route"   ? "◈ 路由"
                               : step.phase == "intent"  ? "◎ 意图"
                                                         : "· 步骤";
            rows.push_back(ftxui::hbox({
                ftxui::text(badge) | ftxui::bold | ftxui::color(m_theme->colors().muted),
                ftxui::text("  "),
                ftxui::paragraph(breakLongWords(step.summary)),
            }));
            if (!step.detail.empty())
            {
                rows.push_back(ftxui::paragraph(breakLongWords(step.detail)) |
                               ftxui::dim | ftxui::color(m_theme->colors().muted));
            }
        }
        if (rows.size() == 1)
        {
            rows.push_back(ftxui::text("（无编排明细）") | ftxui::dim | ftxui::color(m_theme->colors().muted));
        }
        return ftxui::vbox(std::move(rows));
    }

    namespace
    {
        /// 从 Tool Result 文本中提取单行摘要（工具名/状态/输出行数）。
        auto makeToolResultSummary(const std::string &text) -> std::string
        {
            const auto start = text.find('{');
            const auto end = text.rfind('}');
            if (start == std::string::npos || end == std::string::npos || end <= start)
            {
                return text.substr(0, 80) + (text.size() > 80 ? "..." : "");
            }
            try
            {
                const auto j = nlohmann::json::parse(text.substr(start, end - start + 1));
                std::string name = j.value("toolName", "tool");
                const int code = j.value("exitCode", -1);
                const std::string status = (code == 0)
                                               ? "success"
                                               : ("failed(" + std::to_string(code) + ")");
                int lines = 0;
                if (j.contains("stdout"))
                {
                    const auto &s = j["stdout"].get<std::string>();
                    lines = 1 + static_cast<int>(std::count(s.begin(), s.end(), '\n'));
                }
                return name + " \u00b7 " + status + " \u00b7 " + std::to_string(lines) + " lines";
            }
            catch (...)
            {
                return text.substr(0, 80) + (text.size() > 80 ? "..." : "");
            }
        }
    } // namespace

    auto MessageBubble::renderToolMessage() const -> ftxui::Element
    {
        auto makeHeader = [this](const std::string &mark, const std::string &hint) -> ftxui::Element {
            auto elem = ftxui::hbox({
                ftxui::text(mark) | ftxui::bold | ftxui::color(m_theme->colors().accent),
                ftxui::text(" * Tool Result") | ftxui::bold | ftxui::color(m_theme->colors().accent),
                ftxui::filler(),
                renderTimestamp(),
                ftxui::text(hint) | ftxui::dim | ftxui::color(m_theme->colors().muted),
            });
            auto &target = m_toolResultBoxOut ? *m_toolResultBoxOut : m_toolResultBox;
            elem = elem | ftxui::reflect(target) | ftxui::underlined;
            m_hasToolResultBox = true;
            return elem;
        };

        auto header = makeHeader(
            m_message.toolResultCollapsed ? "\u25b6" : "\u25bc",
            m_message.toolResultCollapsed ? "  (已折叠 \u00b7 点击展开)" : "  (点击折叠)");

        // ────────────────────────────────────────────────────────────────
        // 关键修复：空 Tool 消息显示紧凑状态指示，避免空大框
        // ────────────────────────────────────────────────────────────────
        ftxui::Element content;
        if (m_message.content.empty())
        {
            content = ftxui::hbox({
                ftxui::text("running...") | ftxui::italic | ftxui::color(m_theme->colors().muted),
                ftxui::text(" ") | ftxui::color(m_theme->colors().accent) | ftxui::blink,
            });
        }
        else if (m_message.toolResultCollapsed)
        {
            content = ftxui::paragraph(breakLongWords(makeToolResultSummary(m_message.content))) |
                      ftxui::dim | ftxui::color(m_theme->colors().muted);
        }
        else
        {
            content = ftxui::paragraph(breakLongWords(m_message.content));
        }

        auto bubble = ftxui::window(ftxui::text(""), ftxui::vbox({header, ftxui::separator(), content}));

        return ftxui::hbox({
            applyWidthConstraints(bubble) |
                ftxui::color(m_theme->colors().muted),
            ftxui::filler(),
        });
    }

    auto MessageBubble::renderTimestamp() const -> ftxui::Element
    {
        return ftxui::text(formatTime(m_message.timestamp)) |
               ftxui::color(m_theme->colors().muted) |
               ftxui::dim;
    }

    auto MessageBubble::formatTime(const std::chrono::system_clock::time_point &tp) const -> std::string
    {
        auto time = std::chrono::system_clock::to_time_t(tp);
        auto tmPtr = std::localtime(&time);
        // 防御：Windows localtime 对非法 time_t 返回 nullptr，解引用即访问违规崩溃。
        if (tmPtr == nullptr)
        {
            return "--:--";
        }
        auto tm = *tmPtr;
        std::ostringstream oss;
        oss << std::put_time(&tm, "%H:%M");
        return oss.str();
    }

} // namespace AE::TUI
