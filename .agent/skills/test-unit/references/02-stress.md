# 02 — Stress Test SOP (EN)

> Maps to `Docs/02` §5. Goal: under sustained high load, assess **stable throughput, latency tail, and resource-exhaustion/crash rate**.

## 1. Execution口径 (restricted this round: study + define口径, no code landing)
- Duration: ≥ 10 min steady state (suggest 30 min), gradient 10/30/60 min.
- Load intensity: concurrent workers held at high waterline (e.g., 4×cores from the concurrency SOP), each round with multiple tool calls and mode switches (React↔Plan↔Reflection via `AgentEngine::emplace_mode`).
- Fault injection: periodically trigger `EgressGate` denial (egress storm), `request_stop()` graceful exit, and oversized tool output (triggers `ToolOutputSanitizer` length cap).
- Observe: CPU, RSS, handle count, crash/OOM, error-event rate over time.

## 2. Verification checklist (verify item-by-item while executing the skill)
- [ ] `AgentEngine::request_stop()` exits cleanly at iteration boundary and persists (`AEShutdown`/`IStateStore`)?
- [ ] Mode switching (`emplace_mode`) under concurrency/stress leaks no resources (see `04-memory-growth.md`)?
- [ ] `EgressGate` under high-frequency denial only logs, never throws across the boundary (`Core::Result` error path)?
- [ ] `ToolOutputSanitizer` length-cap truncation stable under oversized-output storms (no OOB / unbounded growth)?
- [ ] Error paths emit via `AEStage::Error` and never crash the process?

## 3. Metrics & thresholds (suggested; pre-baseline = suggested)
| Metric | 口径 | Threshold |
|---|---|---|
| Stable throughput floor | avg QPS in last 1/3 of run | ≥ 70% of baseline |
| P99 kernel latency | see `06-kernel-latency.md` | regression ≤ 30% |
| Crash/OOM count | process exit code / events | **0 (hard)** |
| Error-event rate | `AEStage::Error` share | ≤ 0.5% |
| Handle leak | handle-count slope over time | ≈ 0 (no obvious growth) |

## 4. Landing suggestions (write into report "Next steps"; no code this round)
- Reuse the concurrency harness (from `01-concurrency.md` landing items) with added duration & fault injection; output time-series metrics.
- Suggest a simple orchestration script to drive gradient duration/intensity; funnel results into the delivery report.
