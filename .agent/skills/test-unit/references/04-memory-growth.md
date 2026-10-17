# 04 — Memory Growth SOP (EN)

> Maps to `Docs/02` §7. Goal: detect **memory leak or unbounded growth** over long loops / many sessions; locate the steady-state plateau.

## 1. Execution口径 (restricted this round: study + define口径, no code landing)
- Long loop: a single `AgentEngine` runs K rounds of `run()` continuously (K ≥ 5000, suggest 10000), resetting context each round but **not rebuilding the process**.
- Many sessions: create and destroy S sessions sequentially (S ≥ 1000); verify `IStateStore`/session state is reclaimable.
- Leak detection: **ASan + LeakSanitizer** (Debug, `clang-cl /fsanitize=address`); under Release use massif (Linux) / Heob (Windows) for heap growth.
- Growth calculation: record RSS every 500 rounds; fit `ΔRSS / rounds` slope and steady-state plateau.

## 2. Verification checklist (verify item-by-item while executing the skill)
- [ ] `ModeContext` `begin_run` resets each round; no accumulated residue (message deque, tool results)?
- [ ] `AETelemetry` sink reused across `run()` rather than repeated `add_sink` causing unbounded growth?
- [ ] Session state (`IStateStore`/`AESessionStore`) released after destroy/restore?
- [ ] `InMemoryStore` reclaims after TTL expiry (`expire`); no permanent residence?
- [ ] Main-loop iteration cap (`max_iterations` default 16) and `request_stop` prevent unbounded growth?

## 3. Metrics & thresholds (suggested; pre-baseline = suggested)
| Metric | 口径 | Threshold |
|---|---|---|
| ASan leak report | LSan output | **0 bytes leaked (hard)** |
| Growth slope | ΔRSS / 1000 rounds | ≤ 1% of baseline (≈0) |
| Steady-state plateau | late RSS fluctuation band | stabilizes, no monotonic rise |
| Session reclaim | RSS fall after destroy | falls ≥ 90% of increment |

## 4. Landing suggestions (write into report "Next steps"; no code this round)
- Implement a long-loop harness on the `DummyLLM` stub; run ASan/LSan and massif/Heob.
- Many-session destroy cases to verify `IStateStore` release paths.
