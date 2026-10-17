# 07 — Delivery Report Template & Field Definitions (EN)

> **Mandatory**: every execution of the `test-unit` skill package (any single specialty, or multiple) **must** produce a delivery report and archive it.

## 1. Archive path & naming
- Directory: project root `Reports/test-unit/` (create before first execution).
- Single specialty: `Reports/test-unit/<YYYYMMDD-HHMM>-<specialty>.md`
  - Specialty names: `concurrency` / `stress` / `memory-footprint` / `memory-growth` / `security` / `kernel-latency`.
- Combined runs: `Reports/test-unit/<YYYYMMDD-HHMM>-multi.md`, with per-specialty sections in the body.
- Timestamp: local `YYYYMMDD-HHMM` (e.g., `20261017-1903`).

## 2. Report template (copy this structure and fill in)

```markdown
# Test Delivery Report — <specialty name / EN>

- Report ID: <YYYYMMDD-HHMM>-<specialty>
- Generated: <ISO8601 local time>
- Executor: WorkBuddy AI (test-unit skill package v1.0.0, EN)
- Related: Docs/02-标准化测试文档.md §<section> / Plan.md P9

## 1. Background
<user request / trigger reason / scope of this run (which specialties)>

## 2. SOP basis
<referenced references/*.md and Docs/02 sections>

## 3. Environment & version
- Build: <Release / Debug+sanitizer; preset name>
- Toolchain: <clang-cl + lld-link / clang, C++26>
- Tested version: <git commit or "working tree uncommitted">
- Baseline data: <yes/no; if yes, note source>

## 4. Actions taken (scope-boundary statement)
> This round only establishes skills & standardized docs; **no test code landed** (no new tests/*.cpp, no compile/run of real collection programs).
<actual actions: source/config paths studied, API contracts verified, metric口径 confirmed>

## 5. Key findings / metrics
| Metric | 口径 | Observed | Threshold | Judgment |
|---|---|---|---|---|
| ... | ... | ... | ... | pass/observe/fail |

<qualitative findings, code anchors (file:line), risk points>

## 6. Conclusion
<pass / observe / fail; overall judgment & rationale>

## 7. Residual risks
<uncovered items, follow-up landing items, external-dependency risks (e.g., DNS rebinding)>

## 8. Next steps
<suggested test code / CI integration items to land (describe only; no files written this round)>
```

## 3. Judgment-level definitions
- **Pass**: all hard items met, trend items within threshold.
- **Observe**: hard items met, but a trend item breaches or lacks baseline — needs ongoing observation.
- **Fail**: any hard item unmet — must locate root cause, fix, and re-test.

## 4. Delivery requirements
- After writing the report to disk, deliver it via `present_files` for user review.
- The report is a baseline asset for cross-version regression comparison; do not delete/modify historical reports (only append new ones).
- If the same specialty is executed multiple times, keep each report for trend comparison.
