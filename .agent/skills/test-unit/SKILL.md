---
name: test-unit
description: "VXAE test-unit standard operating procedures: high-concurrency / stress / memory footprint / memory growth / security (network, info, memory-runtime) / average kernel response time (excluding vLLM latency) SOPs plus a delivery report. Trigger when the user asks to test, stress-test, concurrency-test, memory-test, security-test, performance-accept, measure kernel latency, or mentions test-unit."
description_zh: "VXAE 测试单元标准作业包（英文版）"
description_en: "VXAE test-unit standard operating procedures + delivery report (EN)"
version: 1.0.0
display_name: "test-unit (EN)"
display_name_en: "test-unit"
visibility: "project"
---

# Test-Unit SOP (EN)

The **English** counterpart of VXAE's (AE::Engine) test-unit standard skill package. It codifies the 6 specialized SOPs defined in `Docs/02-标准化测试文档.md` into an executable workflow, and **mandates a delivery report after every execution**.

This package is a standalone English edition placed under `.agent/skills/test-unit/`. The Chinese edition lives at `.workbuddy-ai/skills/test-unit/`; both share the same SOPs and report convention.

## When to trigger

Activate this skill when the user expresses any of the following intents:
- Wants to **test / stress-test / concurrency-test / memory-test / security-test / performance-accept / measure kernel response time** of the engine;
- Explicitly names the `test-unit` skill or one of its specialties (high-concurrency, stress, memory footprint, memory growth, security, kernel latency);
- Asks to generate or archive a **test delivery report**.

## Scope constraint (hard)

> **Only skills and standardized documentation are established; no test code is implemented.**
> While executing this skill, do **not** add `tests/*.cpp`, and do **not** compile/run real stress or collection programs.
> Permitted actions are limited to: studying source code and build config, verifying component APIs and contracts, defining metric口径 (metric definitions) and acceptance thresholds, and producing a delivery report.
> Actually landing test programs is a follow-up task (see `Plan.md` P9, items 3/4).

## The 6 specialties (each maps to a reference)

| Specialty | reference | One-line goal |
|---|---|---|
| High concurrency | `references/01-concurrency.md` | Thread safety & throughput of concurrent sessions / message bus / telemetry fan-out |
| Stress | `references/02-stress.md` | Stable throughput, latency tail, crash rate under sustained load |
| Memory footprint | `references/03-memory-footprint.md` | Baseline resident memory & per-session working set |
| Memory growth | `references/04-memory-growth.md` | Growth slope & leak detection over long loops / many sessions |
| Security | `references/05-security.md` | Network (SSRF) / Info (injection·redaction) / Memory-runtime (secret·isolation) |
| Average kernel response time | `references/06-kernel-latency.md` | Engine's own cost, excluding vLLM inference/transport latency |

Overview and execution steps: `references/00-index.md`.

## Execution flow (always follow)

1. Confirm the **specialty** (one or more) the user wants to run.
2. Read the corresponding `references/*.md` (and the relevant section of `Docs/02`) as the job basis.
3. Perform the **allowed actions** per SOP: study `src/AE/**`, `CMakeLists.txt`, `CMakePresets.json`, `tests/`, and the demo bridge layer; verify API contracts and metric口径.
4. **Do not land test code**; if a landing suggestion is warranted, describe it only in the report's "Next steps" — do not write files this round.
5. **Mandatorily produce a delivery report**: per `references/07-delivery-report.md`, write
   `Reports/test-unit/<YYYYMMDD-HHMM>-<specialty>.md` (use `multi` or the primary specialty for combined runs), and deliver it via `present_files`.

## Delivery report (mandatory)

Regardless of the conclusion (pass / observe / fail), **every skill execution must generate a delivery report** archived under `Reports/test-unit/`.
The report must cover: background, SOP basis, environment & version, actions taken (note "no test code landed"), key findings/metrics, conclusion, residual risks, and next steps.
Template and field definitions: `references/07-delivery-report.md`.
