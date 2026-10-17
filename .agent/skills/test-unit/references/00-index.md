# test-unit Overview & Execution General Rules (EN)

This directory holds the operational details of VXAE's test-unit skill package (English edition). Companion authoritative spec: `Docs/02-标准化测试文档.md`.

## Contents

- `00-index.md` — this file (overview + common steps + environment self-check)
- `01-concurrency.md` — High-concurrency SOP
- `02-stress.md` — Stress test SOP
- `03-memory-footprint.md` — Memory footprint SOP
- `04-memory-growth.md` — Memory growth (leak) SOP
- `05-security.md` — Security test SOP (network / info / memory-runtime)
- `06-kernel-latency.md` — Average kernel response time SOP (excluding vLLM)
- `07-delivery-report.md` — Delivery report template & field definitions

## Common execution steps (shared by all specialties)

1. **Prepare**: confirm build state (Release for perf/memory; Debug + sanitizer for concurrency/security static checks), version commit, baseline data.
2. **Execute (restricted this round)**: **only study code, verify API contracts, and define metric口径**; do not compile/run new test programs (see SKILL.md scope constraint).
3. **Collect**: gather per the specialty's metric table; keep raw evidence (OTel lines, memory snapshots, TSan/ASan reports, source line numbers).
4. **Judge**: against the `Docs/02` §10 threshold summary, give pass / observe / fail; locate the root cause (source location) of any breach.
5. **Report**: produce and archive per `07-delivery-report.md`.

## Environment self-check (before every execution)

```bash
# 1) Core library buildable (L0 gate)
cmake --preset llvm
cmake --build --preset llvm-release --target vxae

# 2) Compile-time probe passes (existing tests/vxae_compile_check.cpp)
cmake --build --preset llvm-debug   --target vxae_check
```

- Performance / memory / latency specialties: **must use Release** (`llvm-release`).
- Concurrency / security static checks: use Debug + `clang-cl /fsanitize=thread` (TSan) or `/fsanitize=address` (ASan/LSan).

## Key component quick-reference (SOP anchors)

| Concern | Code anchor |
|---|---|
| Main loop / mode dispatch | `src/AE/AgentEngine.hpp`, `src/AE/Mode/*` |
| vLLM latency boundary | `src/AE/StreamingLLMClient.hpp`, `src/AE/LLMAdapter.hpp` |
| Network protection (SSRF) | `src/AE/AESecurity.hpp` (`EgressPolicy`/`EgressGate`) |
| Info protection (injection/redaction) | `src/AE/AESanitize.hpp` (`ToolOutputSanitizer`), `AETelemetry.hpp` (`redact_json`) |
| Memory-runtime (secret) | `src/Core/Security/SecureString.hpp`, `src/AE/Ports/...InMemoryStore.hpp` |
| Telemetry / kernel latency | `src/AE/AETelemetry.hpp` (`AEStage`/`Span`) |
| Concurrency surface | `AETelemetry` (shared_mutex), `Collab/MessageBus.hpp`, `Ports/...InMemoryStore.hpp` |
| Existing probe | `tests/vxae_compile_check.cpp` (`vxae_check`) |
