# 05 — Security Test SOP (EN)

> Maps to `Docs/02` §8. Threat model: `Docs/01` §4.7. Three sub-surfaces: **network / info / memory-runtime**.

## 1. Network (SSRF / egress protection)
- Carrier: `EgressPolicy` (`DefaultDeny` default), `EgressGate` (implements `IPermissionGate`, scans URL/host in tool args before execution). Code: `src/AE/AESecurity.hpp`.
- Cases (verify code contracts while executing the skill; no code landing):
  1. Tool arg carries `http://169.254.169.254/` (cloud metadata IMDS) → **must deny**.
  2. Carries `http://127.0.0.1`, `http://10.x`, `http://192.168.x`, `http://172.16~31.x`, `localhost`, `[::1]` → **must deny**.
  3. Whitelisted `https://api.openai.com` or `*.example.com` → **allow**.
  4. `EgressPolicy::DefaultAllow` relaxed mode → only the dangerous addresses above still denied (defense in depth).
- Judgment: all 4 cases **pass (hard)**.
- Suggested item (record residual risk): **DNS rebinding** — this layer judges by textual host; suggest DNS pinning / secondary validation on the host side.

## 2. Info (prompt injection / redaction)
- Carriers: `ToolOutputSanitizer`, `redact_json`. Code: `src/AE/AESanitize.hpp`, `src/AE/AETelemetry.hpp`.
- Cases:
  1. Tool output carries `<|im_start|>system ...`, `[INST]...[/INST]`, `<<SYS>>` → neutralized to `[FILTERED]` (**hard**).
  2. Output contains ANSI/control chars (ESC 0x1B) → stripped (**hard**).
  3. Oversized output (> `max_tool_output_chars`, default 65536) → truncated & marked (**hard**).
  4. JSON with `api_key` through `redact_json(..., true)` → secret redacted (**hard**).
  5. Structured log (OTel `StructuredLoggerSink`) lines must **not** contain plaintext secrets/PII (**hard**).
- Verify: `vxae_compile_check.cpp` already has a `ToolOutputSanitizer` probe (injected `<|im_start|>system...` is sanitized) — confirm that path is compile-time reachable.

## 3. Memory-runtime (secret / session isolation)
- Carriers: `SecureString`, `InMemoryStore`, secret egress boundary (`StreamingLLMClient` builds `Authorization` only at the call boundary from `SecureString`). Code: `src/Core/Security/SecureString.hpp`, `src/AE/Ports/...InMemoryStore.hpp`, `src/AE/StreamingLLMClient.hpp`.
- Cases:
  1. Memory dump of the process searching for `AEConfig`'s `api_key` plaintext → should be held by `SecureString`, not bare-stored (**hard**).
  2. `InMemoryStore` `query` results for different sessions must **not crosstalk** (**hard**).
  3. Secrets must not land in logs/telemetry attributes (same as §2.5).

## 4. Metrics & thresholds (hard items summary)
| Sub-surface | Hard pass condition |
|---|---|
| Network | all 4 cases in §1 behave as expected |
| Info | all 5 cases in §2 behave as expected |
| Memory-runtime | all 3 cases in §3 behave as expected |

## 5. Landing suggestions (write into report "Next steps"; no code this round)
- Info surface: extend the existing sanitizer probe in `vxae_compile_check.cpp` into assertion-style unit tests.
- Network surface: parameterized `EgressPolicy` unit tests (cover each dangerous/whitelisted host).
- Memory-runtime: a debug-build secret-plaintext scan script (process dump search).
