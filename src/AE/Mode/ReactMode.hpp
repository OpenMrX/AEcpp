#pragma once

/// @file Mode/ReactMode.hpp
/// @brief 默认单循环 ReAct 模式：主循环由 AgentEngine 下沉至此，经 ModeContext 驱动（Docs/01 §4.2）。
///
/// @details
/// 结构性调整：本文件承接原 AgentEngine 的 ReAct 主循环（行为逐行等价）；AgentEngine 收敛为
/// 薄壳（ModeVariant + std::visit）。除上下文/依赖经 `ModeContext` 注入外，逻辑与原实现一致。
///
/// 覆盖缺口：R4 工具失败分类（仅当注入 `ToolFailurePolicy` 时生效）、R5 多次压缩（compression_count）、
/// R7 失败动作编译期表（`failure_action`）。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <latch>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Logger.hpp"
#include "AEType.hpp"
#include "LLMAdapter.hpp"
#include "AETelemetry.hpp"
#include "AERecordReplay.hpp"
#include "AESanitize.hpp"

#include "Mode/ModeContext.hpp"
#include "Mode/ModeKind.hpp"
#include "Mode/IReasoningMode.hpp"

namespace AE::Engine
{

    // ---------------------------------------------------- R7：失败动作编译期表
    // AgentError → 处置动作，编译期常量映射（无运行期分支表分配）。
    enum class FailureAction : std::uint8_t
    {
        Retry,   // 可重试
        Degrade, // 降级继续（回灌错误文本）
        Abort,   // 中止本轮
        Stop     // 优雅停止
    };

    [[nodiscard]] constexpr auto failure_action(AgentError e) noexcept -> FailureAction
    {
        switch (e)
        {
        case AgentError::LlmTransport:
            return FailureAction::Retry;
        case AgentError::ContextFull:
        case AgentError::BudgetExceeded:
            return FailureAction::Stop;
        case AgentError::MaxIterations:
        case AgentError::Cancelled:
            return FailureAction::Abort;
        case AgentError::LlmParse:
        case AgentError::ToolNotFound:
        case AgentError::ToolExec:
            return FailureAction::Degrade;
        case AgentError::ConfigMissing:
        case AgentError::None:
        default:
            return FailureAction::Abort;
        }
    }

    class ReactMode final
    {
    public:
        using CompressCallback =
            std::function<Core::Result<std::optional<std::string>>(std::string_view session_id)>;

        explicit ReactMode(AERunOptions opts = {}) : m_opts(std::move(opts))
        {
            if (m_opts.replay == ReplayMode::Record && !m_opts.record_path.empty())
                m_rec = std::make_shared<RecordingWriter>(m_opts.record_path, m_opts.redact_prompt);
            if (m_opts.replay == ReplayMode::Replay && !m_opts.replay_path.empty())
                m_replay = std::make_shared<ReplayController>(m_opts.replay_path);
        }

        // ─────────────────────────────────────────── 编译期契约（ReasoningMode）
        [[nodiscard]] static constexpr auto kind() noexcept -> ModeKind { return ModeKind::React; }
        [[nodiscard]] static constexpr auto name() noexcept -> std::string_view { return "react"; }

        // ─────────────────────────────────────────── 配置（薄壳转发）
        auto set_compress_threshold(double threshold) noexcept -> void { m_compress_threshold = threshold; }
        auto set_compress_callback(CompressCallback cb) -> void { m_compress_callback = std::move(cb); }
        auto register_hook(HookPoint point, Hook hook) -> std::size_t
        {
            m_hooks[static_cast<std::size_t>(point)].push_back(std::move(hook));
            return ++m_hook_seq;
        }
        void set_sanitizer(ToolOutputSanitizer sanitizer) { m_sanitizer = std::move(sanitizer); }
        void request_stop() noexcept { m_stop.request_stop(); }
        [[nodiscard]] bool is_stop_requested() const { return m_stop.stop_requested(); }

        // 把 messages 中最早的若干轮压缩为一条 system 摘要消息。
        // 保留：全部 system（含工具契约、历史摘要）+ 尾部最近 kKeepTail 条消息（约 3 轮）。
        static void compress_history(std::deque<nlohmann::json> &msgs, const std::string &summary)
        {
            constexpr std::size_t kKeepTail = 6;
            std::size_t start = 0;
            while (start < msgs.size() && msgs[start].value("role", std::string{}) == "system")
                ++start;
            if (msgs.size() - start <= kKeepTail)
                return; // 历史太短，无需压缩
            const std::size_t end = msgs.size() - kKeepTail;
            if (end <= start)
                return;
            nlohmann::json block = nlohmann::json::object();
            block["role"] = "system";
            block["content"] = "[会话压缩摘要] " + summary;
            msgs.erase(msgs.begin() + static_cast<std::ptrdiff_t>(start),
                       msgs.begin() + static_cast<std::ptrdiff_t>(end));
            msgs.insert(msgs.begin() + static_cast<std::ptrdiff_t>(start), std::move(block));
        }

        // ─────────────────────────────────────────── 主入口
        auto execute(ModeContext &ctx) -> std::generator<AgentEvent>
        {
            std::size_t iter = 0;
            m_stop = std::stop_source{}; // 每次 run() 取一份全新可取消令牌
            std::stop_token st = m_stop.get_token();
            struct RunGuard
            {
                ReactMode *self;
                ModeContext *ctxp;
                std::size_t iter = 0;
                bool cancelled = false;
                ~RunGuard()
                {
                    if (ctxp->state)
                    {
                        if (cancelled)
                            Core::Logger::getInstance().infoFmt(
                                "[AE] graceful shutdown: flushing checkpoint for session={}", ctxp->session_id);
                        self->save_checkpoint(*ctxp, iter);
                    }
                }
            };
            RunGuard fin{this, &ctx, 0};
            const auto run_start = std::chrono::steady_clock::now();
            const std::string trace_id = make_trace_id();
            auto run_span = ctx.telemetry ? std::optional<AETelemetry::Span>(
                                                ctx.telemetry->span(trace_id, ctx.session_id, AEStage::Run, "agent.run"))
                                          : std::nullopt;
            if (ctx.state)
            {
                auto snap = Checkpointer{*ctx.state}.resume(ctx.session_id);
                if (snap.isSuccess() && snap.value().has_value())
                {
                    const auto &st2 = *snap.value();
                    if (st2.contains("tokens_used"))
                        ctx.ctx_source.add_tokens(st2["tokens_used"].get<std::size_t>());
                    Core::Logger::getInstance().infoFmt(
                        "[AE] resumed session={} iter_reset={}", ctx.session_id, iter);
                }
            }

            // 初始化业务层消息历史（System 最前、历史按时间、当前最后）。
            ctx.messages = init_history(ctx.request_body);

            nlohmann::json static_cfg = ctx.request_body.is_object() ? ctx.request_body : nlohmann::json::object();
            if (static_cfg.contains("messages"))
                static_cfg.erase("messages");

            // 双轨 P0(text)：真实工具面非空时注入「可用工具 + 文本调用协议」契约 system 消息。
            {
                auto toolSchema = ctx.tools.get_tools_schema();
                if (toolSchema.is_array() && !toolSchema.empty())
                {
                    std::string contract;
                    contract.reserve(512);
                    contract.append("你是具备工具调用能力的助手。当前可用工具：\n");
                    for (const auto &fn : toolSchema)
                    {
                        if (!fn.is_object() || !fn.contains("function"))
                            continue;
                        const auto &fun = fn["function"];
                        if (!fun.is_object() || !fun.contains("name"))
                            continue;
                        const std::string nm = fun.value("name", std::string{});
                        const std::string ds = fun.value("description", std::string{});
                        contract.append("- ").append(nm);
                        if (!ds.empty())
                            contract.append(": ").append(ds);
                        contract.append("\n");
                    }
                    contract.append(
                        "调用规则：若需使用工具完成当前任务，请在主回复正文（content）中单独输出 "
                        "{\"tool\":\"<工具名>\",\"tool_args\":{...}} 形式的 JSON"
                        "（也可用 ```json ... ``` 代码块包裹）。"
                        "严禁把工具调用 JSON 放进 reasoning/thinking 内容中，否则系统无法识别。"
                        "工具执行结果会返回给你；收到结果后必须给出最终正文回答用户。");
                    ctx.messages.push_front(
                        nlohmann::json{{"role", "system"}, {"content", std::move(contract)}});
                    ctx.emits_contract = true;
                    Core::Logger::getInstance().infoFmt(
                        "[AE] tool-contract injected: {} tools advertised, body tools+tool_choice=auto",
                        toolSchema.size());
                }
            }

            constexpr std::uint32_t kMaxCompressions = 8; // R5：单次 run 内压缩次数上限（防抖）

            for (;;)
            {
                if (st.stop_requested())
                {
                    if (run_span)
                        run_span->set_ok(false, "Cancelled");
                    fin.cancelled = true;
                    fin.iter = iter;
                    co_yield fire_error(ctx, iter, Core::Error{"Cancelled"});
                    co_return;
                }
                if (iter++ >= ctx.max_iterations)
                {
                    if (run_span)
                        run_span->set_ok(false, "MaxIterations");
                    co_yield fire_error(ctx, iter, Core::Error{"MaxIterations"});
                    co_return;
                }
                ctx.iteration = static_cast<std::uint32_t>(iter);
                if (m_opts.time_limit.count() > 0)
                {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - run_start);
                    if (elapsed >= m_opts.time_limit)
                    {
                        if (run_span)
                            run_span->set_ok(false, "TimeLimitExceeded");
                        co_yield fire_error(ctx, iter, Core::Error{"TimeLimitExceeded"});
                        co_return;
                    }
                }
                co_yield AgentEvent{EventKind::Thinking};

                // ---- 组装本轮请求体 ----
                nlohmann::json body = static_cfg;
                body["messages"] = nlohmann::json::array();
                for (const auto &m : ctx.messages)
                    body["messages"].push_back(m);

                // BeforeLLM hook（可改写 body，支持 Retry）
                {
                    const std::size_t max_retry = 3;
                    std::size_t retry_count = 0;
                    bool retry = false;
                    do
                    {
                        retry = false;
                        HookContext hc;
                        hc.point = HookPoint::BeforeLLM;
                        hc.session_id = ctx.session_id;
                        hc.iteration = iter;
                        hc.request_body = &body;
                        for (auto &hk : m_hooks[static_cast<size_t>(HookPoint::BeforeLLM)])
                        {
                            HookControl ctrl = hk(hc);
                            if (ctrl == HookControl::Abort)
                            {
                                co_yield AgentEvent{EventKind::Final};
                                co_return;
                            }
                            if (ctrl == HookControl::Retry)
                            {
                                if (retry_count < max_retry)
                                {
                                    ++retry_count;
                                    retry = true;
                                    Core::Logger::getInstance().warningFmt(
                                        "[AE] BeforeLLM hook requested retry ({} / {})",
                                        retry_count, max_retry);
                                    break;
                                }
                                else
                                {
                                    Core::Logger::getInstance().errorFmt(
                                        "[AE] BeforeLLM hook retry limit exceeded ({}), aborting",
                                        max_retry);
                                    co_yield AgentEvent{EventKind::Final};
                                    co_return;
                                }
                            }
                        }
                    } while (retry);
                }

                // 注入工具 schema：调用方已在 body["tools"] 提供则不覆盖。
                auto schema = ctx.tools.get_tools_schema();
                // [DIAG] 临时诊断：确认工具 schema 是否注入（确诊后删除）
                Core::Logger::getInstance().infoFmt(
                    "[AE-DIAG] tools schema size={} model={}", schema.size(), body.value("model", ""));
                if (!schema.empty() && !body.contains("tools"))
                {
                    body["tools"] = schema;
                    if (!body.contains("tool_choice"))
                        body["tool_choice"] = "auto";
                }

                // ---- LLM 回合 ----
                Core::Result<std::vector<LlmDelta>> turn =
                    Core::Result<std::vector<LlmDelta>>::Ok(std::vector<LlmDelta>{});
                const std::size_t max_on_error_retry = 2;
                std::size_t on_error_retry_count = 0;
                bool on_error_retry = false;

                std::string text_acc;
                nlohmann::json tool_calls_json = nlohmann::json::array();
                std::vector<LlmDelta::ToolCallInfo> pending;
                bool has_tool = false;

                auto process_delta = [&](const LlmDelta &d) -> std::optional<AgentEvent>
                {
                    switch (d.kind)
                    {
                    case LlmDelta::Kind::Text:
                        if (!d.text_content.empty())
                        {
                            text_acc += d.text_content;
                            return AgentEvent{EventKind::Token, d.text_content};
                        }
                        break;
                    case LlmDelta::Kind::Thinking:
                        if (!d.thinking_content.empty())
                            return AgentEvent{EventKind::Thinking, d.thinking_content};
                        break;
                    case LlmDelta::Kind::Usage:
                        if (d.usage_info)
                        {
                            ctx.ctx_source.add_tokens(d.usage_info->prompt_tokens +
                                                      d.usage_info->completion_tokens);
                            Core::Logger::getInstance().debugFmt(
                                "[AE] prompt_tokens={} completion_tokens={}",
                                d.usage_info->prompt_tokens, d.usage_info->completion_tokens);
                        }
                        break;
                    case LlmDelta::Kind::ToolCall:
                        if (d.tool_call_info)
                        {
                            has_tool = true;
                            std::string tid = d.tool_call_info->id;
                            if (tid.empty())
                                tid = "call_" + std::to_string(iter) + "_" + std::to_string(pending.size());
                            LlmDelta::ToolCallInfo info;
                            info.id = tid;
                            info.name = d.tool_call_info->name;
                            info.arguments = d.tool_call_info->arguments;

                            // 归一化：某些本地模型会把真实参数 JSON 字符串化后嵌套进 answer 字段。
                            if (info.arguments.is_object() && info.arguments.size() == 1 &&
                                info.arguments.contains("answer") && info.arguments["answer"].is_string())
                            {
                                const auto inner = info.arguments["answer"].get<std::string>();
                                auto parsed = nlohmann::json::parse(inner, nullptr, false);
                                if (!parsed.is_null())
                                {
                                    Core::Logger::getInstance().infoFmt(
                                        "[AE] unwrapped answer-wrapped args for tool='{}'", info.name);
                                    info.arguments = std::move(parsed);
                                }
                            }

                            pending.push_back(info);
                            nlohmann::json tc = nlohmann::json::object();
                            tc["id"] = tid;
                            tc["type"] = "function";
                            tc["function"] = nlohmann::json::object({{"name", info.name},
                                                                     {"arguments", info.arguments.dump()}});
                            tool_calls_json.push_back(std::move(tc));
                        }
                        break;
                    case LlmDelta::Kind::Error:
                        if (!d.error_message.empty())
                            return AgentEvent{EventKind::Error, {}, Core::Error{d.error_message}};
                        break;
                    case LlmDelta::Kind::Done:
                    default:
                        break;
                    }
                    return std::nullopt;
                };

                bool streamed = false;
                do
                {
                    on_error_retry = false;
                    {
                        auto span = ctx.telemetry ? std::optional<AETelemetry::Span>(
                                                        ctx.telemetry->span(trace_id, ctx.session_id,
                                                                            AEStage::LlmTurn, "llm.chat"))
                                                  : std::nullopt;
                        const auto t0 = std::chrono::steady_clock::now();
                        if (m_replay)
                        {
                            turn = m_replay->next_llm_turn();
                        }
                        else
                        {
                            auto gen = ctx.llm.stream(body);
                            std::vector<LlmDelta> recorded;
                            std::string streamError;
                            try
                            {
                                for (auto &&d : gen)
                                {
                                    recorded.push_back(d);
                                    if (auto ev = process_delta(d); ev)
                                        co_yield *std::move(ev);
                                }
                                const auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - t0);
                                if (m_rec)
                                    m_rec->record_llm(trace_id, body,
                                                      Core::Result<std::vector<LlmDelta>>::Ok(std::move(recorded)),
                                                      dur);
                                Core::Logger::getInstance().infoFmt(
                                    "[AE] LLM turn streamed ok, tokens(deltas)={}", recorded.size());
                                streamed = true;
                            }
                            catch (const std::exception &e)
                            {
                                const auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - t0);
                                if (m_rec)
                                    m_rec->record_llm(trace_id, body,
                                                      Core::Result<std::vector<LlmDelta>>::Error(
                                                          Core::Error{std::string{e.what()}}),
                                                      dur);
                                streamError = std::string{e.what()};
                            }
                            if (!streamError.empty())
                            {
                                if (run_span)
                                    run_span->set_ok(false, streamError);
                                co_yield AgentEvent{EventKind::Error, {}, Core::Error{streamError}};
                                co_return;
                            }
                        }
                        if (span)
                        {
                            span->set_ok(streamed ? true : turn.isSuccess(),
                                         streamed ? "ok" : (turn.isError() ? turn.error().message() : "ok"));
                            span->attr("iter", std::to_string(iter));
                        }
                    }
                    if (!streamed && turn.isError())
                    {
                        HookControl ctrl = run_on_error_hooks(ctx, iter, turn.error());
                        if (ctrl == HookControl::Abort)
                        {
                            if (run_span)
                                run_span->set_ok(false, turn.error().message());
                            co_yield AgentEvent{EventKind::Final};
                            co_return;
                        }
                        if (ctrl == HookControl::Retry && on_error_retry_count < max_on_error_retry)
                        {
                            ++on_error_retry_count;
                            on_error_retry = true;
                            Core::Logger::getInstance().warningFmt(
                                "[AE] OnError hook requested retry for LLM turn ({} / {})",
                                on_error_retry_count, max_on_error_retry);
                            continue;
                        }
                        if (run_span)
                            run_span->set_ok(false, turn.error().message());
                        co_yield AgentEvent{EventKind::Error, {}, turn.error()};
                        co_return;
                    }
                } while (on_error_retry);

                if (!streamed)
                {
                    for (const auto &d : turn.value())
                    {
                        if (auto ev = process_delta(d); ev)
                            co_yield *std::move(ev);
                    }
                }

                nlohmann::json assistant = nlohmann::json::object();
                assistant["role"] = "assistant";
                assistant["content"] = text_acc;
                if (has_tool)
                    assistant["tool_calls"] = tool_calls_json;
                ctx.messages.push_back(std::move(assistant));

                if (ctx.ctx_source.would_exceed_hard_limit(0))
                {
                    Core::Logger::getInstance().errorFmt(
                        "[AE] token hard-limit breached: {}/{}",
                        ctx.ctx_source.token_count(), ctx.ctx_source.hard_limit());
                    if (run_span)
                        run_span->set_ok(false, "BudgetExceeded");
                    co_yield fire_error(ctx, iter, Core::Error{"BudgetExceeded"});
                    co_return;
                }
                save_checkpoint(ctx, iter);

                // [DIAG] 临时诊断：确认模型回合是否解析到工具调用（确诊后删除）
                Core::Logger::getInstance().infoFmt(
                    "[AE-DIAG] has_tool={} pending={} text_len={} tool_json={}",
                    has_tool, pending.size(), text_acc.size(), tool_calls_json.dump());
                if (!has_tool)
                {
                    if (m_rec)
                        m_rec->record_event(trace_id, "final", "agent.done", "ok");
                    Core::Logger::getInstance().infoFmt(
                        "[AE] final answer emitted iter={} text_len={}", iter, text_acc.size());
                    co_yield AgentEvent{EventKind::Final, text_acc};
                    co_return;
                }

                // ---- 并发执行工具（std::jthread + latch），结果回写历史 ----
                for (const auto &info : pending)
                    co_yield AgentEvent{EventKind::ToolCall, info.name};

                std::vector<Core::Result<nlohmann::json>> results;
                results.reserve(pending.size());
                for (std::size_t i = 0; i < pending.size(); ++i)
                    results.emplace_back(Core::Result<nlohmann::json>::Error(Core::Error{"pending"}));
                std::latch done{static_cast<ptrdiff_t>(pending.size())};
                std::vector<std::jthread> workers;
                workers.reserve(pending.size());
                for (std::size_t i = 0; i < pending.size(); ++i)
                {
                    workers.emplace_back([&, i, st]
                                         {
                                             if (st.stop_requested())
                                             {
                                                 results[i] = Core::Result<nlohmann::json>::Error(
                                                     Core::Error{"Cancelled"});
                                                 done.count_down();
                                                 return;
                                             }
                                             auto span = ctx.telemetry ? std::optional<AETelemetry::Span>(
                                                 ctx.telemetry->span(trace_id, ctx.session_id, AEStage::ToolExec, pending[i].name))
                                                 : std::nullopt;
                                             const auto t0 = std::chrono::steady_clock::now();

                                             if (m_replay)
                                             {
                                                 results[i] = m_replay->next_tool_result(
                                                     pending[i].name, pending[i].arguments);
                                             }
                                             else
                                             {
                                                 if (ctx.gate)
                                                 {
                                                     auto g = ctx.gate->can_execute_tool(
                                                         pending[i].name, pending[i].arguments);
                                                     if (g.isError())
                                                     {
                                                         results[i] = Core::Result<nlohmann::json>::Error(g.error());
                                                         if (span) span->set_ok(false, g.error().message());
                                                         done.count_down();
                                                         return;
                                                     }
                                                 }
                                                 // [DIAG] 执行前：工具名 + 参数（确诊后删除）
                                                 Core::Logger::getInstance().infoFmt(
                                                     "[AE-DIAG] executing tool: name={} args={}",
                                                     pending[i].name, pending[i].arguments.dump());
                                                 results[i] = ctx.tools.execute(
                                                     pending[i].name, pending[i].arguments, /*maxRetries=*/3);
                                                 // [DIAG] 执行后：结果内容（截断，确诊后删除）
                                                 if (results[i].isSuccess())
                                                 {
                                                     auto dumped = results[i].value().dump();
                                                     if (dumped.size() > 300)
                                                         dumped = dumped.substr(0, 300) + "...(truncated)";
                                                     Core::Logger::getInstance().infoFmt(
                                                         "[AE-DIAG] tool result OK: name={} result={}",
                                                         pending[i].name, dumped);
                                                 }
                                                 else
                                                 {
                                                     Core::Logger::getInstance().infoFmt(
                                                         "[AE-DIAG] tool result ERROR: name={} err={}",
                                                         pending[i].name, results[i].error().message());
                                                 }
                                                 if (m_rec)
                                                 {
                                                     const auto d = std::chrono::duration_cast<std::chrono::microseconds>(
                                                         std::chrono::steady_clock::now() - t0);
                                                     m_rec->record_tool(trace_id, pending[i].name,
                                                                       pending[i].arguments, results[i], d);
                                                 }
                                             }
                                             if (span)
                                                 span->set_ok(results[i].isSuccess(),
                                                              results[i].isError() ? results[i].error().message() : "ok");
                                             done.count_down(); });
                }
                done.wait();

                for (std::size_t i = 0; i < pending.size(); ++i)
                {
                    Core::Logger::getInstance().infoFmt(
                        "[AE] tool executed name='{}' success={} out_bytes={}",
                        pending[i].name, results[i].isSuccess(),
                        results[i].isSuccess() ? results[i].value().dump().size()
                                               : results[i].error().message().size());
                }

                // ---- R4：工具失败分类（仅当注入策略时生效；默认不改变既有行为） ----
                if (ctx.failures)
                {
                    for (std::size_t i = 0; i < pending.size(); ++i)
                    {
                        if (ctx.failures->classify(pending[i].name, results[i]) == ToolFailureClass::Fatal)
                        {
                            const Core::Error err = results[i].isError()
                                                        ? results[i].error()
                                                        : Core::Error{"ToolFatal"};
                            Core::Logger::getInstance().errorFmt(
                                "[AE] tool '{}' fatal failure: {}", pending[i].name, err.message());
                            if (run_span)
                                run_span->set_ok(false, err.message());
                            co_yield fire_error(ctx, iter, err);
                            co_return;
                        }
                    }
                }

                // 循环检测：连续 3 次同工具且参数相似度>95% → 熔断
                for (const auto &t : pending)
                {
                    if (m_loop.observe(t.name, t.arguments))
                    {
                        Core::Logger::getInstance().warningFmt("[AE] loop detected on tool={}", t.name);
                        if (run_span)
                            run_span->set_ok(false, "LoopDetected");
                        co_yield fire_error(ctx, iter, Core::Error{"LoopDetected"});
                        co_return;
                    }
                }

                // ---- 写回 tool 消息 + AfterTool hooks（支持 Retry） ----
                for (std::size_t i = 0; i < pending.size(); ++i)
                {
                    nlohmann::json tool_msg = nlohmann::json::object();
                    tool_msg["role"] = "tool";
                    tool_msg["tool_call_id"] = pending[i].id;

                    const std::size_t max_retry = 3;
                    std::size_t retry_count = 0;
                    bool retry = false;
                    do
                    {
                        retry = false;
                        HookContext hc;
                        hc.point = HookPoint::AfterTool;
                        hc.session_id = ctx.session_id;
                        hc.iteration = iter;
                        hc.tool_name = &pending[i].name;
                        hc.tool_result = &results[i];
                        for (auto &hk : m_hooks[static_cast<size_t>(HookPoint::AfterTool)])
                        {
                            HookControl ctrl = hk(hc);
                            if (ctrl == HookControl::Abort)
                            {
                                co_yield AgentEvent{EventKind::Final};
                                co_return;
                            }
                            if (ctrl == HookControl::Retry)
                            {
                                if (retry_count < max_retry)
                                {
                                    ++retry_count;
                                    retry = true;
                                    Core::Logger::getInstance().warningFmt(
                                        "[AE] AfterTool hook requested retry for tool='{}' ({} / {})",
                                        pending[i].name, retry_count, max_retry);

                                    co_yield AgentEvent{EventKind::ToolCall, pending[i].name};
                                    if (m_replay)
                                    {
                                        results[i] = m_replay->next_tool_result(
                                            pending[i].name, pending[i].arguments);
                                    }
                                    else
                                    {
                                        if (ctx.gate)
                                        {
                                            auto g = ctx.gate->can_execute_tool(
                                                pending[i].name, pending[i].arguments);
                                            if (g.isError())
                                            {
                                                results[i] = Core::Result<nlohmann::json>::Error(g.error());
                                                break;
                                            }
                                        }
                                        results[i] = ctx.tools.execute(
                                            pending[i].name, pending[i].arguments, /*maxRetries=*/3);
                                        if (m_rec)
                                        {
                                            const auto d = std::chrono::microseconds{0};
                                            m_rec->record_tool(trace_id, pending[i].name,
                                                               pending[i].arguments, results[i], d);
                                        }
                                    }
                                    break;
                                }
                                else
                                {
                                    Core::Logger::getInstance().warningFmt(
                                        "[AE] AfterTool hook retry limit exceeded for tool='{}' ({}), continuing",
                                        pending[i].name, max_retry);
                                }
                            }
                        }
                    } while (retry);

                    const std::string raw = results[i].isSuccess()
                                                ? results[i].value().dump()
                                                : std::string{"Tool execution failed: "} + results[i].error().message();

                    SanitizeStats san_st;
                    std::string clean = m_sanitizer.sanitize(raw, &san_st);
                    if (san_st.truncated || san_st.control_stripped > 0 || san_st.injection_neutralized > 0)
                        Core::Logger::getInstance().warningFmt(
                            "[AE:Security] tool='{}' output sanitized: trunc={} ctrl={} inj={} ({}->{} bytes)",
                            pending[i].name, san_st.truncated, san_st.control_stripped,
                            san_st.injection_neutralized, san_st.original_len, san_st.final_len);

                    tool_msg["content"] = clean;
                    co_yield AgentEvent{EventKind::ToolResult, clean};
                    ctx.messages.push_back(std::move(tool_msg));
                }

                // ---- OnIteration hook ----
                {
                    HookContext hc;
                    hc.point = HookPoint::OnIteration;
                    hc.session_id = ctx.session_id;
                    hc.iteration = iter;
                    for (auto &hk : m_hooks[static_cast<size_t>(HookPoint::OnIteration)])
                        if (hk(hc) == HookControl::Abort)
                        {
                            co_yield AgentEvent{EventKind::Final};
                            co_return;
                        }
                }

                save_checkpoint(ctx, iter);
                fin.iter = iter;

                // ---- 上下文软限压缩（R5：compression_count 上限内可多次压缩） ----
                if (ctx.compression_count < kMaxCompressions && m_compress_callback &&
                    ctx.ctx_source.usage_ratio() >= m_compress_threshold)
                {
                    Core::Logger::getInstance().infoFmt(
                        "[AE] context usage {:.0f}% >= threshold {:.0f}%, triggering session compression (session={})",
                        ctx.ctx_source.usage_ratio() * 100.0, m_compress_threshold * 100.0, ctx.session_id);
                    auto cr = m_compress_callback(ctx.session_id);
                    if (cr.isSuccess() && cr.value().has_value() && !cr.value()->empty())
                    {
                        compress_history(ctx.messages, *cr.value());
                        ++ctx.compression_count;
                        Core::Logger::getInstance().infoFmt(
                            "[AE] session compressed: summary {} chars, messages now {}",
                            cr.value()->size(), ctx.messages.size());
                    }
                    else
                    {
                        Core::Logger::getInstance().warningFmt(
                            "[AE] session compression skipped: {}",
                            cr.isError() ? cr.error().message() : "empty summary");
                    }
                }
            }
        }

    private:
        static auto init_history(const nlohmann::json &req) -> std::deque<nlohmann::json>
        {
            std::deque<nlohmann::json> msgs;
            if (req.is_object() && req.contains("messages") && req["messages"].is_array())
                for (const auto &m : req["messages"])
                    msgs.push_back(m);
            else if (req.is_string())
                msgs.push_back(nlohmann::json::object({{"role", "user"}, {"content", req.get<std::string>()}}));
            else
                msgs.push_back(nlohmann::json::object({{"role", "user"}, {"content", req.dump()}}));
            return msgs;
        }

        auto run_on_error_hooks(ModeContext &ctx, std::size_t iter, const Core::Error &err) -> HookControl
        {
            HookControl result = HookControl::Continue;
            HookContext hc;
            hc.point = HookPoint::OnError;
            hc.session_id = ctx.session_id;
            hc.iteration = iter;
            hc.error = &err;
            for (auto &hk : m_hooks[static_cast<size_t>(HookPoint::OnError)])
            {
                HookControl ctrl = hk(hc);
                if (ctrl == HookControl::Abort)
                    return HookControl::Abort;
                if (ctrl == HookControl::Retry)
                    result = HookControl::Retry;
            }
            return result;
        }

        auto fire_error(ModeContext &ctx, std::size_t iter, Core::Error err) -> AgentEvent
        {
            auto ctrl = run_on_error_hooks(ctx, iter, err);
            if (ctrl == HookControl::Abort)
                return AgentEvent{EventKind::Final};
            return AgentEvent{EventKind::Error, {}, std::move(err)};
        }

        void save_checkpoint(ModeContext &ctx, std::size_t /*iter*/)
        {
            if (!ctx.state)
                return;
            nlohmann::json state = nlohmann::json::object();
            state["tokens_used"] = ctx.ctx_source.token_count();
            auto r = Checkpointer{*ctx.state}.save(ctx.session_id, state);
            if (r.isError())
                Core::Logger::getInstance().errorFmt("[AE] checkpoint save failed: {}", r.error().message());
        }

        AERunOptions m_opts;
        std::shared_ptr<RecordingWriter> m_rec;
        std::shared_ptr<ReplayController> m_replay;
        LoopDetector m_loop;
        std::stop_source m_stop;
        ToolOutputSanitizer m_sanitizer;
        std::vector<Hook> m_hooks[4];
        std::size_t m_hook_seq = 0;
        double m_compress_threshold = 0.85;
        CompressCallback m_compress_callback;
    };

    static_assert(ReasoningMode<ReactMode>, "ReactMode must satisfy the ReasoningMode contract");

} // namespace AE::Engine
