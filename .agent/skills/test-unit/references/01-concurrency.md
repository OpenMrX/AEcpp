# 01 — High-Concurrency Test SOP (EN)

> Maps to `Docs/02` §4. Goal: under concurrent scenarios, **no data race, no deadlock, no cross-session crosstalk**, and measure concurrent throughput & error rate.

## 1. Concurrency surfaces under test (code anchors)
- Multiple `AgentEngine` instances running `run()` in parallel: each has its own `ModeContext` (`src/AE/AgentEngine.hpp`).
- `MessageBus` concurrent `send` + multi-consumer `mailbox`/`log`: `src/AE/Collab/MessageBus.hpp`.
- `AETelemetry` multi-threaded `emit` fan-out (internal `shared_mutex`): `src/AE/AETelemetry.hpp`.
- `InMemoryStore` concurrent `upsert`/`query`/`expire`: `src/AE/Ports/...InMemoryStore.hpp`.
- `AgentRegistry` + `CapabilityAssigner` concurrent registration & assignment: `src/AE/Collab/*`.

## 2. Execution口径 (restricted this round: study + define口径, no code landing)
- Thread model: `std::jthread`/`std::async` spawn N workers, N = 2×CPU cores as a start, gradient 1/2/4/8×cores.
- Load: each worker runs M rounds of `run()` (M ≥ 200), each round with 1~3 tool calls.
- Static check: Debug + **ThreadSanitizer** (`clang-cl /fsanitize=thread`) runs the same concurrency harness to catch races/deadlocks.

## 3. Verification checklist (verify item-by-item while executing the skill)
- [ ] `AETelemetry::emit` always holds `shared_mutex` (read lock), `add_sink` holds write lock; no mixed read/write hazards?
- [ ] `MessageBus::send`/`mailbox`/`log` have concurrency protection; no iterator-invalidation risk?
- [ ] `InMemoryStore` backend (`upsert`/`query`/`expire`) preserves session isolation & consistency under concurrency?
- [ ] `AgentRegistry`/`CapabilityAssigner` reentrant under concurrent register/assign?
- [ ] Each `run()` strictly uses its own `ModeContext`; no cross-instance shared mutable state?

## 4. Metrics & thresholds (suggested; pre-baseline = suggested)
| Metric | 口径 | Threshold |
|---|---|---|
| Data races | TSan report | **0 (hard)** |
| Deadlocks | not finished within 30s timeout | **0 (hard)** |
| Cross-session crosstalk | each session's messages only seen in its own mailbox | **0 (hard)** |
| Concurrent throughput QPS | successful sessions / wall clock | record baseline; regression ≤ 15% |
| Error rate | failed run() / total | ≤ 0.1% |

## 5. Landing suggestions (write into report "Next steps"; no code this round)
- Add a concurrency harness under `tests/`, reusing the `DummyLLM` stub from `vxae_compile_check.cpp` to avoid real LLM dependency.
- Add a TSan build target to CI as a merge gate.
