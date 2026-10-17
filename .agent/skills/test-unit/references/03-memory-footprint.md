# 03 — Memory Footprint SOP (EN)

> Maps to `Docs/02` §6. Goal: measure **baseline resident memory** and **per-session working set**; establish capacity-planning baseline.

## 1. Execution口径 (restricted this round: study + define口径, no code landing)
- Build: **Release** (optimized + sanitizer overhead removed).
- Baseline: process started, `vxae`/`vxae_demo` loaded but **no `run()` issued** — RSS/VSS at that point.
- Per-session working set: after spawning 1 `AgentEngine` and completing 1 round of `run()` (with 1 tool call) — incremental RSS.
- Collection tools:
  - Windows: `GetProcessMemoryInfo` (PSAPI) / Process Explorer;
  - Linux: `/usr/bin/time -v`, `/proc/<pid>/status` (`VmRSS`/`VmPeak`).

## 2. Verification checklist (verify item-by-item while executing the skill)
- [ ] Engine holds dependencies only at injection points (`IToolProvider`/`IContextSource`/`LLMAdapter`/`IPermissionGate`/`IStateStore`); no implicit global singleton resident?
- [ ] `AETelemetry` sink vector reused across `run()` rather than growing unbounded?
- [ ] Existing `vxae_compile_check.cpp` `sizeof` probe covers key type sizes (compile-time locked, no regression)?
- [ ] Demo bridge layer (`demo/tui/Bridge/AETUIBridge.*`) holds state per session, no cross-session leak?

## 3. Metrics & thresholds (suggested; pre-baseline = suggested)
| Metric | 口径 | Threshold |
|---|---|---|
| Process baseline RSS | started, not run | trend reference only; record baseline |
| Per-session incremental RSS | after 1 run − baseline | regression ≤ 20% |
| Peak RSS (VmPeak) | stress peak | record; set capacity alert line |
| Type size (`sizeof` probe) | compile-time | no regression (locked) |

## 4. Landing suggestions (write into report "Next steps"; no code this round)
- Wrap a cross-platform memory-snapshot script sampling baseline / per-session / stress states.
- Add VmPeak to CI trend charts with a regression alert.
