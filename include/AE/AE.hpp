#pragma once

/// @file AE.hpp
/// @brief 执行核心：单循环单 agent 的 ReAct 循环。实现 IAgentExecutor。
///
/// @details
/// 设计约束（关键）：
/// - 仅依赖注入抽象（IToolProvider / IContextSource / IPermissionGate / LLMAdapter）
/// + IStateStore（可选），不硬依赖 DCEE / ContextManager / Butler。
/// - 主循环单线程驱动，天然无锁；工具并发用 std::jthread + std::latch 汇合后
/// 由主循环串行回写上下文。
/// - 在 BeforeLLM / AfterTool / OnIteration / OnError 处调用已注册 hooks（AOP 下沉），
/// 还「循环单体无扩展点」架构债。
/// - 不干预物理 KV Cache：C++ 核心只管业务层 Message History（见 §4.3.1），
/// KV Cache 由推理引擎管理。
///
/// @note 所属命名空间：VMX::AE

#include <chrono>
#include <deque>
#include <functional>
#include <latch>
#include <optional>
#include <thread>
#include <stop_token>
#include <vector>
#include <string>
#include <string_view>

#include "Core/Result.hpp"
#include "Core/Logger.hpp"
#include "AEType.hpp"
#include "AEManager.hpp"
#include "IAE.hpp"
#include "AETelemetry.hpp"
#include "AERecordReplay.hpp"
#include "AESanitize.hpp"
#include "NullToolProvider.hpp"  // 无工具 provider，供 DCEE 未连时跑通 AgentEngine 链路

namespace VMX::AE
{

    class AgentEngine : public IAgentExecutor
    {
    public:
        AgentEngine(IToolProvider &tools,
                    IContextSource &ctx,
                    LLMAdapter &llm,
                    IPermissionGate *gate = nullptr,
                    IStateStore *store = nullptr,
                    std::shared_ptr<AETelemetry> telemetry = nullptr,
                    AERunOptions run_opts = {})
            : m_tools(tools), m_ctx(ctx), m_llm(llm),
              m_gate(gate), m_store(store),
              m_tele(telemetry ? std::move(telemetry) : run_opts.telemetry),
              m_opts(std::move(run_opts))
        {
            if (m_opts.replay == ReplayMode::Record && !m_opts.record_path.empty())
                m_rec = std::make_shared<RecordingWriter>(m_opts.record_path, m_opts.redact_prompt);
            if (m_opts.replay == ReplayMode::Replay && !m_opts.replay_path.empty())
                m_replay = std::make_shared<ReplayController>(m_opts.replay_path);
        }

        // ── 上下文软限压缩（会话压缩回调）──
        // 当 m_ctx.usage_ratio() ≥ 阈值时，run() 触发 compress_callback(session_id)，
        // 回调返回可选摘要文本（例如 LLM 滚动摘要）；有摘要则将 messages 最早若干轮
        // 替换为一条 system 摘要消息，为后续回合腾出上下文预算。
        using CompressCallback =
            std::function<Core::Result<std::optional<std::string>>(std::string_view session_id)>;

        /// 设置压缩触发阈值（0.0~1.0，默认 0.85）。
        auto set_compress_threshold(double threshold) noexcept -> void
        {
            m_compress_threshold = threshold;
        }

        /// 注入会话压缩回调（由宿主提供 LLM 滚动摘要等能力）。
        auto set_compress_callback(CompressCallback cb) -> void
        {
            m_compress_callback = std::move(cb);
        }

        // 主入口：流式产出 token 粒度事件。单循环单 agent。
        auto run(std::string_view session_id,
                 const nlohmann::json &request_body,
                 std::size_t max_iterations = 16)
            -> std::generator<AgentEvent> override
        {
            std::size_t iter = 0;
            bool compressed_this_run = false; // 单次 run 内最多压缩一次，防抖
            // §11 优雅退出：为本轮 run() 取一份全新可取消令牌；宿主（或消费方）经
            //   request_stop() 请求取消，循环在迭代边界检测后优雅退出并落盘。
            m_stop = std::stop_source{};
            std::stop_token st = m_stop.get_token();
            // 作用域出口守卫：无论正常完成 / 错误 / 取消 / 消费方提前终止（generator 被析构），
            // 都在此汇合在途工具 worker（已由主循环 latch 保证）并落盘会话快照。
            struct RunGuard
            {
                AgentEngine *self;
                std::string sid;
                std::size_t iter = 0;
                bool cancelled = false;
                ~RunGuard()
                {
                    if (self->m_store)
                    {
                        if (cancelled)
                            Core::Logger::getInstance().infoFmt(
                                "[AE] graceful shutdown: flushing checkpoint for session={}", sid);
                        self->save_checkpoint(sid, iter);
                    }
                }
            };
            RunGuard fin{this, std::string{session_id}, 0};
            const auto run_start = std::chrono::steady_clock::now();
            // §4.6：每次 run() 生成 trace_id，贯穿 LLM 调用与工具执行日志/遥测。
            const std::string trace_id = make_trace_id();
            auto run_span = m_tele ? std::optional<AETelemetry::Span>(
                                         m_tele->span(trace_id, session_id, AEStage::Run, "agent.run"))
                                   : std::nullopt;
            if (m_store)
            {
                auto snap = Checkpointer{*m_store}.resume(session_id);
                if (snap.isSuccess() && snap.value().has_value())
                {
                    const auto &st = *snap.value();
                    if (st.contains("tokens_used"))
                        m_ctx.add_tokens(st["tokens_used"].get<std::size_t>());
                    // 注意：iter 是单次 run 内的迭代计数器，必须每次从 0 开始，
                    // 否则多轮对话会累积迭代次数并很快触发 MaxIterations。
                    // if (st.contains("iter"))
                    //     iter = st["iter"].get<std::size_t>();
                    Core::Logger::getInstance().infoFmt("[AE] resumed session={} iter_reset={}", session_id, iter);
                }
            }

            // 初始化业务层消息历史（见 AgentEngine.md §4.3.1）：System 最前、历史按时间、
            // 当前追加最后，固定顺序以命中 vLLM Prefix Caching；工具结果按不可信处理，
            // 与系统指令物理隔离。
            auto init_history = [](const nlohmann::json &req) -> std::deque<nlohmann::json>
            {
                std::deque<nlohmann::json> msgs;
                if (req.is_object() && req.contains("messages") && req["messages"].is_array())
                    for (const auto &m : req["messages"])
                        msgs.push_back(m);
                else if (req.is_string())
                    msgs.push_back(nlohmann::json::object(
                        {{"role", "user"}, {"content", req.get<std::string>()}}));
                else
                    msgs.push_back(nlohmann::json::object(
                        {{"role", "user"}, {"content", req.dump()}}));
                return msgs;
            };
            std::deque<nlohmann::json> messages = init_history(request_body);

            // 静态请求配置（model / temperature / stream 等），每轮与 messages 合并发出
            nlohmann::json static_cfg = request_body.is_object() ? request_body : nlohmann::json::object();
            if (static_cfg.contains("messages"))
                static_cfg.erase("messages");

            // 双轨 P0(text)：真实工具面非空时，向 messages 头部注入"可用工具 + 文本调用协议"
            // 契约 system 消息。与原生 body["tools"]（+tool_choice=auto）双保险：模型即便不走
            // 原生 tool_calls 字段，也可按契约输出 {"tool":...,"tool_args":{...}} 的文本调用，
            // 由流尾 try_parse_text_tool_call 解析兜底 → DCEE 执行 → 结果回灌 → 第二轮出正文。
            {
                auto toolSchema = m_tools.get_tools_schema();
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
                    messages.push_front(
                        nlohmann::json{{"role", "system"}, {"content", std::move(contract)}});
                    Core::Logger::getInstance().infoFmt(
                        "[AE] tool-contract injected: {} tools advertised, body tools+tool_choice=auto",
                        toolSchema.size());
                }
            }

            for (;;)
            {
                // §11 取消检测（迭代边界）：一旦请求取消，产出 Cancelled 事件并优雅退出；
                // 在途工具 worker 已在本轮 latch 汇合后才会到达此处，故退出前已完成汇合。
                if (st.stop_requested())
                {
                    if (run_span)
                        run_span->set_ok(false, "Cancelled");
                    fin.cancelled = true;
                    fin.iter = iter;
                    co_yield fire_error(session_id, iter, Core::Error{"Cancelled"});
                    co_return;
                }
                if (iter++ >= max_iterations)
                {
                    if (run_span)
                        run_span->set_ok(false, "MaxIterations");
                    co_yield fire_error(session_id, iter, Core::Error{"MaxIterations"});
                    co_return;
                }
                if (m_opts.time_limit.count() > 0)
                {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - run_start);
                    if (elapsed >= m_opts.time_limit)
                    {
                        if (run_span)
                            run_span->set_ok(false, "TimeLimitExceeded");
                        co_yield fire_error(session_id, iter, Core::Error{"TimeLimitExceeded"});
                        co_return;
                    }
                }
                co_yield AgentEvent{EventKind::Thinking};

                // ---- 组装本轮请求体（固定 messages 顺序 + 工具 schema）----
                nlohmann::json body = static_cfg;
                body["messages"] = nlohmann::json::array();
                for (const auto &m : messages)
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
                        hc.session_id = std::string{session_id};
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

                // 注入工具 schema：调用方已在 body["tools"] 提供则不覆盖（§4.1.1 归一化）。
                // P0 双轨(native)：显式 tool_choice=auto，避免部分兼容实现默认不回调工具，
                // 导致模型只能"用文字说明"而无法触发原生函数调用。
                auto schema = m_tools.get_tools_schema();
                // [DIAG] 临时诊断：确认工具 schema 是否注入（确诊后删除）
                Core::Logger::getInstance().infoFmt(
                    "[AE-DIAG] tools schema size={} model={}", schema.size(), body.value("model", ""));
                if (!schema.empty() && !body.contains("tools"))
                {
                    body["tools"] = schema;
                    if (!body.contains("tool_choice"))
                        body["tool_choice"] = "auto";
                }

                // ---- LLM 回合（遥测 Span + 录制；Replay 模式直接回放；支持 OnError Retry）----
                Core::Result<std::vector<LlmDelta>> turn =
                    Core::Result<std::vector<LlmDelta>>::Ok(std::vector<LlmDelta>{});
                const std::size_t max_on_error_retry = 2;
                std::size_t on_error_retry_count = 0;
                bool on_error_retry = false;

                // 本轮 assistant 文本与 tool_calls 累加器（流式与 replay 路径共用）。
                std::string text_acc;
                nlohmann::json tool_calls_json = nlohmann::json::array();
                std::vector<LlmDelta::ToolCallInfo> pending;
                bool has_tool = false;

                // 统一处理单个 delta：累加文本/工具调用，返回需上发的事件（无则返回 nullopt）。
                // 流式路径边收边处理；replay 路径收齐后处理——两条路径复用，避免重复 switch。
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
                            m_ctx.add_tokens(d.usage_info->prompt_tokens + d.usage_info->completion_tokens);
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
                            // 例如 args={"answer":"{\"path\":\"...\",\"edits\":[...]}"}。
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
                            return AgentEvent{EventKind::Error, {},
                                              Core::Error{d.error_message}};
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
                        auto span = m_tele ? std::optional<AETelemetry::Span>(
                                                 m_tele->span(trace_id, session_id, AEStage::LlmTurn, "llm.chat"))
                                           : std::nullopt;
                        const auto t0 = std::chrono::steady_clock::now();
                        if (m_replay)
                        {
                            turn = m_replay->next_llm_turn();
                        }
                        else
                        {
                            // 真·增量流式：HTTP 在背景线程进行，逐 delta 经生成器实时上发，
                            // run() 边收边 co_yield Token，实现"边生成边显示"。
                            auto gen = m_llm.stream(body);
                            std::vector<LlmDelta> recorded;
                            std::string streamError; // 流式传输异常消息（在 catch 外上发，避免 co_yield 出现在 catch 块）
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
                                                      Core::Result<std::vector<LlmDelta>>::Ok(
                                                          std::move(recorded)),
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
                            // 流式传输异常在 catch 外上发（co_yield 不得出现在 catch 块内）。
                            if (!streamError.empty())
                            {
                                if (run_span)
                                    run_span->set_ok(false, streamError);
                                co_yield AgentEvent{EventKind::Error, {},
                                                    Core::Error{streamError}};
                                co_return;
                            }
                        }
                        if (span)
                        {
                            span->set_ok(streamed ? true : turn.isSuccess(),
                                         streamed ? "ok"
                                                  : (turn.isError() ? turn.error().message() : "ok"));
                            span->attr("iter", std::to_string(iter));
                        }
                    }
                    if (!streamed && turn.isError())
                    {
                        HookControl ctrl = run_on_error_hooks(session_id, iter, turn.error());
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

                // replay 路径：流式已边收边处理，此处仅对 replay 的 turn 补处理。
                if (!streamed)
                {
                    for (const auto &d : turn.value())
                    {
                        if (auto ev = process_delta(d); ev)
                            co_yield *std::move(ev);
                    }
                }

                // 写回 assistant 消息（文本 + 工具调用，OpenAI 兼容）；无工具时仅文本
                nlohmann::json assistant = nlohmann::json::object();
                assistant["role"] = "assistant";
                assistant["content"] = text_acc;
                if (has_tool)
                    assistant["tool_calls"] = tool_calls_json;
                messages.push_back(std::move(assistant));

                // token 硬限熔断：逼近 context_window 即熔断，避免无意义长尾消耗
                if (m_ctx.would_exceed_hard_limit(0))
                {
                    Core::Logger::getInstance().errorFmt(
                        "[AE] token hard-limit breached: {}/{}", m_ctx.token_count(), m_ctx.hard_limit());
                    if (run_span)
                        run_span->set_ok(false, "BudgetExceeded");
                    co_yield fire_error(session_id, iter, Core::Error{"BudgetExceeded"});
                    co_return;
                }
                save_checkpoint(session_id, iter); // 每次 LLM 响应后落盘

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
                                            // §11 协作式取消：若本回合执行前已被请求取消，
                                            // 跳过真实工具调用，直接以 Cancelled 结果汇合（best-effort）。
                                            if (st.stop_requested())
                                            {
                                                results[i] = Core::Result<nlohmann::json>::Error(
                                                    Core::Error{"Cancelled"});
                                                done.count_down();
                                                return;
                                            }
                                            auto span = m_tele ? std::optional<AETelemetry::Span>(
                                                m_tele->span(trace_id, session_id, AEStage::ToolExec, pending[i].name))
                                                : std::nullopt;
                                            const auto t0 = std::chrono::steady_clock::now();

                                            if (m_replay)
                                            {
                                                // Replay 模式：跳过真实权限门/工具，直接回放录制结果。
                                                results[i] = m_replay->next_tool_result(
                                                    pending[i].name, pending[i].arguments);
                                            }
                                            else
                                            {
                                                if (m_gate)
                                                {
                                                    auto g = m_gate->can_execute_tool(
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
                                                results[i] = m_tools.execute(
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
                done.wait(); // 汇合后由主循环线程统一处理

                // 执行结果追踪：确认 DCEE 是否成功执行、回灌前结果体积（供"执行后是否收到"核查）。
                for (std::size_t i = 0; i < pending.size(); ++i)
                {
                    Core::Logger::getInstance().infoFmt(
                        "[AE] tool executed name='{}' success={} out_bytes={}",
                        pending[i].name, results[i].isSuccess(),
                        results[i].isSuccess() ? results[i].value().dump().size()
                                                : results[i].error().message().size());
                }

                // 循环检测：连续 3 次同工具且参数相似度>95% → 熔断
                for (const auto &t : pending)
                {
                    if (m_loop.observe(t.name, t.arguments))
                    {
                        Core::Logger::getInstance().warningFmt("[AE] loop detected on tool={}", t.name);
                        if (run_span)
                            run_span->set_ok(false, "LoopDetected");
                        co_yield fire_error(session_id, iter, Core::Error{"LoopDetected"});
                        co_return;
                    }
                }

                // ---- 写回 tool 消息 + AfterTool hooks（支持 Retry） ----
                // §4.7 提示注入防护：工具输出/错误文本按**不可信**处理，回灌 LLM 前统一经
                //   ToolOutputSanitizer 净化（去控制字符 / 中和 ChatML 等模板注入标记 /
                //   限长截断 / 不可信物理隔离围栏）。这是对 DCEE 侧净化的 AE 层兜底。
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
                        hc.session_id = std::string{session_id};
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
                                        if (m_gate)
                                        {
                                            auto g = m_gate->can_execute_tool(
                                                pending[i].name, pending[i].arguments);
                                            if (g.isError())
                                            {
                                                results[i] = Core::Result<nlohmann::json>::Error(g.error());
                                                break;
                                            }
                                        }
                                        results[i] = m_tools.execute(
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

                    // 原始（不可信）文本：成功取 JSON dump，失败取错误文案。
                    const std::string raw = results[i].isSuccess()
                                                ? results[i].value().dump()
                                                : std::string{"Tool execution failed: "} + results[i].error().message();

                    SanitizeStats st;
                    std::string clean = m_sanitizer.sanitize(raw, &st);
                    if (st.truncated || st.control_stripped > 0 || st.injection_neutralized > 0)
                        Core::Logger::getInstance().warningFmt(
                            "[AE:Security] tool='{}' output sanitized: trunc={} ctrl={} inj={} ({}->{} bytes)",
                            pending[i].name, st.truncated, st.control_stripped,
                            st.injection_neutralized, st.original_len, st.final_len);

                    tool_msg["content"] = clean;
                    co_yield AgentEvent{EventKind::ToolResult, clean};
                    messages.push_back(std::move(tool_msg));
                }

                // ---- OnIteration hook ----
                {
                    HookContext hc;
                    hc.point = HookPoint::OnIteration;
                    hc.session_id = std::string{session_id};
                    hc.iteration = iter;
                    for (auto &hk : m_hooks[static_cast<size_t>(HookPoint::OnIteration)])
                        if (hk(hc) == HookControl::Abort)
                        {
                            co_yield AgentEvent{EventKind::Final};
                            co_return;
                        }
                }

                save_checkpoint(session_id, iter); // 工具执行后落盘
                fin.iter = iter;                   // §11：记录最新已完成迭代，供出口守卫落盘

                // ---- 上下文软限压缩：使用率 ≥ 阈值 → 回调生成滚动摘要并压缩历史 ----
                // 触发一次后本 run 不再重复（防抖），避免每轮反复做 LLM 摘要。
                if (!compressed_this_run && m_compress_callback &&
                    m_ctx.usage_ratio() >= m_compress_threshold)
                {
                    Core::Logger::getInstance().infoFmt(
                        "[AE] context usage {:.0f}% >= threshold {:.0f}%, triggering session compression (session={})",
                        m_ctx.usage_ratio() * 100.0, m_compress_threshold * 100.0, session_id);
                    auto cr = m_compress_callback(session_id);
                    if (cr.isSuccess() && cr.value().has_value() && !cr.value()->empty())
                    {
                        compress_history(messages, *cr.value());
                        compressed_this_run = true;
                        Core::Logger::getInstance().infoFmt(
                            "[AE] session compressed: summary {} chars, messages now {}",
                            cr.value()->size(), messages.size());
                    }
                    else
                    {
                        Core::Logger::getInstance().warningFmt(
                            "[AE] session compression skipped: {}", cr.isError() ? cr.error().message() : "empty summary");
                    }
                }
            }
        }


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
        // 从检查点恢复（主恢复逻辑在 run 内统一处理，基于 IStateStore）
        auto resume(std::string_view session_id,
                    const nlohmann::json &request_body,
                    std::size_t /*from_iteration*/ = 0)
            -> std::generator<AgentEvent> override
        {
            Core::Logger::getInstance().infoFmt("[AE] resume requested session={}", session_id);
            for (auto &&e : run(session_id, request_body))
                co_yield e;
        }

        // 注册 AOP 钩子，返回句柄
        auto register_hook(HookPoint point, Hook hook) -> std::size_t override
        {
            m_hooks[static_cast<size_t>(point)].push_back(std::move(hook));
            return ++m_hook_seq;
        }

        // §4.7 提示注入防护：配置工具输出净化器（限长/去控制字符/注入中和/物理隔离）。
        // 不改构造签名；调用方（如由 AEConfig::Budget::max_tool_output_chars）按需注入。
        void set_sanitizer(ToolOutputSanitizer sanitizer) { m_sanitizer = std::move(sanitizer); }

        // §11 优雅退出：取消进行中的 run()（在迭代边界检测并优雅退出 + 落盘）。
        // 由宿主信号处理器 / 消费方 / AOP hook 驱动；不影响已完成回合的 latch 汇合。
        void request_stop() noexcept override { m_stop.request_stop(); }
        bool is_stop_requested() const override { return m_stop.stop_requested(); }

    private:
        // 调用 OnError hooks，返回 HookControl（Continue / Abort / Retry）
        auto run_on_error_hooks(std::string_view session_id, std::size_t iter, const Core::Error &err)
            -> HookControl
        {
            HookControl result = HookControl::Continue;
            HookContext hc;
            hc.point = HookPoint::OnError;
            hc.session_id = std::string{session_id};
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

        // OnError 插入点统一出口：先触发 OnError hook，再产出 Error 事件
        // 注意：此函数不处理 Retry，调用方需自行处理 Retry 逻辑
        auto fire_error(std::string_view session_id, std::size_t iter, Core::Error err)
            -> AgentEvent
        {
            auto ctrl = run_on_error_hooks(session_id, iter, err);
            if (ctrl == HookControl::Abort)
                return AgentEvent{EventKind::Final};
            return AgentEvent{EventKind::Error, {}, std::move(err)};
        }

        void save_checkpoint(std::string_view sid, std::size_t /*iter*/)
        {
            if (!m_store)
                return;
            nlohmann::json state = nlohmann::json::object();
            // iter 不再持久化：它是单次 run 内的计数器，跨请求累积会导致 MaxIterations。
            // state["iter"] = iter;
            state["tokens_used"] = m_ctx.token_count();
            auto r = Checkpointer{*m_store}.save(sid, state);
            if (r.isError())
                Core::Logger::getInstance().errorFmt("[AE] checkpoint save failed: {}", r.error().message());
        }

        IToolProvider &m_tools;
        IContextSource &m_ctx;
        LLMAdapter &m_llm;
        IPermissionGate *m_gate;
        IStateStore *m_store;
        std::shared_ptr<AETelemetry> m_tele;        // §4.6 遥测门面（可选）
        AERunOptions m_opts;                        // §4.6 录制/回放/遥测选项
        std::shared_ptr<RecordingWriter> m_rec;     // §4.6 Record 模式落盘
        std::shared_ptr<ReplayController> m_replay; // §4.6 Replay 模式回放
        LoopDetector m_loop;
        std::stop_source m_stop;         // §11 取消令牌（每次 run() 重置为全新可停止源）
        ToolOutputSanitizer m_sanitizer; // §4.7 工具输出净化（不可信内容回灌前兜底）
        std::vector<Hook> m_hooks[4];    // 索引 = HookPoint 枚举序
        std::size_t m_hook_seq = 0;
        double m_compress_threshold = 0.85;      ///< 上下文软限压缩阈值（默认 85%）
        CompressCallback m_compress_callback;    ///< 会话压缩回调（主机注入，滚动摘要）
    };

} // namespace VMX::AE
