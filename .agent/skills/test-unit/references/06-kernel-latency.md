# 06 — Average Kernel Response Time SOP (EN, excluding vLLM latency)

> Maps to `Docs/02` §9. Goal: measure the **engine's own processing cost**, strictly excluding LLM/vLLM inference & transport latency.

## 1. vLLM latency boundary (explicit split)
- **Belongs to vLLM latency (must exclude)**: inside `LLMAdapter::stream_chat`/`normalize_request`, `StreamingLLMClient`'s SSE intake, network round-trip, model inference, token generation. In telemetry this equals all `dur` of `AEStage::LlmTurn`.
- **Belongs to kernel (must include)**: `AgentEngine::run()` scheduling, `ModeContext` iteration, `ReactMode` main loop, `ToolExec` (tool execution itself, excluding LLM), `EgressDeny` (egress block), `Checkpoint` (persist), `SelfHeal`, `ToolOutputSanitizer` sanitization, JSON parse & `SelfHealingJson` repair, mode-switch overhead.

## 2. Calculation口径 (two equivalent ways)
- **Method A (recommended, subtraction)**: `kernel cost = Σ AEStage::Run.dur − Σ AEStage::LlmTurn.dur` (aggregate by `trace_id`).
- **Method B (addition)**: `kernel cost = Σ(ToolExec + EgressDeny + Checkpoint + SelfHeal + scheduling/parse/sanitize overhead)`.
- **Isolation trick**: inject a zero-latency stub `LLMAdapter` (e.g., `DummyLLM` in `vxae_compile_check.cpp`) so `LlmTurn` contains only minimal stub transport cost — lets you calibrate the kernel baseline offline without a real LLM.

## 3. Verification checklist (verify item-by-item while executing the skill)
- [ ] `AETelemetry::Span` covers Run / LlmTurn / ToolExec / Checkpoint / SelfHeal / EgressDeny / Error; no kernel stage left un-instrumented?
- [ ] `StreamingLLMClient`'s LLM cost is fully attributed to `LlmTurn` (so subtraction won't mis-count LLM into kernel)?
- [ ] `ToolOutputSanitizer` cost is timed within ToolExec/Span (counts as kernel, not mixed into LlmTurn)?
- [ ] `AggregateMetricsSink` already accumulates `llm_dur` and `tool_dur` separately — directly diffable?

## 4. Metrics & thresholds (suggested; pre-baseline = suggested)
| Metric | 口径 | Threshold |
|---|---|---|
| Average kernel response time | mean kernel cost per `run()` | regression ≤ 20% |
| P50 / P95 / P99 kernel latency | distribution quantiles | P99 regression ≤ 30% |
| Tool-exec share | `ToolExec.dur / kernel cost` | profiling reference only |
| Sanitizer cost | single `ToolOutputSanitizer` call | < a few ms even on large output (reference) |

## 5. Landing suggestions (write into report "Next steps"; no code this round)
- Reuse `AETelemetry` + `AggregateMetricsSink`'s `snapshot()` to take the `llm_dur`/`tool_dur` difference as kernel cost — no separate instrumentation needed.
- Use the `DummyLLM` stub for offline kernel baseline calibration, then compare against real-LLM scenarios to separate costs.
