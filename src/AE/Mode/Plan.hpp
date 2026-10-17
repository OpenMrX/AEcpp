#pragma once

/// @file Mode/Plan.hpp
/// @brief 计划数据结构 + DAG 校验/状态（Docs/01 §4.3.1）。
///
/// @details
/// 设计约束（关键）：
/// - 纯数据 + 编译期/静态工具；只依赖 `nlohmann::json` / 标准库 / `AEType.hpp`（AgentError）。
/// - `validate()` 检测空计划 / 重复 id / 未知依赖 / 依赖环（规划质量兜底）。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nlohmann-json.hpp"

#include "AEType.hpp" // AgentError

namespace AE::Engine
{

    enum class PlanStatus : std::uint8_t
    {
        Pending,
        Running,
        Done,
        Failed,
        Skipped
    };

    [[nodiscard]] inline auto plan_status_name(PlanStatus s) noexcept -> std::string_view
    {
        switch (s)
        {
        case PlanStatus::Pending:
            return "Pending";
        case PlanStatus::Running:
            return "Running";
        case PlanStatus::Done:
            return "Done";
        case PlanStatus::Failed:
            return "Failed";
        case PlanStatus::Skipped:
            return "Skipped";
        }
        return "Pending";
    }

    struct PlanStep
    {
        std::uint32_t id{};
        std::string description;
        std::vector<std::uint32_t> depends_on; // DAG 依赖（空=可立即执行）
        std::string capability;                // 所需能力（§5.1/§5.8 任务分配用，可空）
        PlanStatus status{PlanStatus::Pending};
        std::string result;                    // 步骤产出摘要（供聚合/反思）
        std::optional<AgentError> error;

        [[nodiscard]] auto to_json() const -> nlohmann::json
        {
            nlohmann::json j = nlohmann::json::object();
            j["id"] = id;
            j["description"] = description;
            j["depends_on"] = depends_on;
            if (!capability.empty())
                j["capability"] = capability;
            j["status"] = plan_status_name(status);
            if (!result.empty())
                j["result"] = result;
            return j;
        }

        static auto from_json(const nlohmann::json &j) -> PlanStep
        {
            PlanStep s;
            s.id = j.value("id", 0u);
            s.description = j.value("description", std::string{});
            s.capability = j.value("capability", std::string{});
            if (j.contains("depends_on") && j["depends_on"].is_array())
                for (const auto &d : j["depends_on"])
                    if (d.is_number_unsigned())
                        s.depends_on.push_back(d.get<std::uint32_t>());
            return s;
        }
    };

    struct Plan
    {
        std::string goal;
        std::vector<PlanStep> steps;
        std::uint32_t revision{0}; // 每次 Replan +1

        [[nodiscard]] bool valid() const noexcept { return !steps.empty(); }

        [[nodiscard]] auto find(std::uint32_t id) -> PlanStep *
        {
            for (auto &s : steps)
                if (s.id == id)
                    return &s;
            return nullptr;
        }
        [[nodiscard]] auto find(std::uint32_t id) const -> const PlanStep *
        {
            for (const auto &s : steps)
                if (s.id == id)
                    return &s;
            return nullptr;
        }
        [[nodiscard]] bool all_done() const noexcept
        {
            for (const auto &s : steps)
                if (s.status != PlanStatus::Done && s.status != PlanStatus::Skipped)
                    return false;
            return !steps.empty();
        }

        // 首个就绪步骤（Pending 且依赖均 Done/Skipped）；无则 nullptr。
        [[nodiscard]] auto pick_ready() -> PlanStep *
        {
            for (auto &s : steps)
            {
                if (s.status != PlanStatus::Pending)
                    continue;
                bool ok = true;
                for (auto d : s.depends_on)
                {
                    const auto *dep = find(d);
                    if (!dep || (dep->status != PlanStatus::Done && dep->status != PlanStatus::Skipped))
                    {
                        ok = false;
                        break;
                    }
                }
                if (ok)
                    return &s;
            }
            return nullptr;
        }

        // 规划质量校验：空计划 / 重复 id / 未知依赖 / 依赖环。
        [[nodiscard]] auto validate(std::string *why = nullptr) const -> bool
        {
            if (steps.empty())
            {
                if (why)
                    *why = "empty plan";
                return false;
            }
            std::unordered_map<std::uint32_t, const PlanStep *> by_id;
            for (const auto &s : steps)
            {
                if (!by_id.emplace(s.id, &s).second)
                {
                    if (why)
                        *why = "duplicate step id " + std::to_string(s.id);
                    return false;
                }
            }
            for (const auto &s : steps)
                for (auto d : s.depends_on)
                    if (!by_id.contains(d))
                    {
                        if (why)
                            *why = "step " + std::to_string(s.id) + " depends on unknown " + std::to_string(d);
                        return false;
                    }

            // 依赖环检测（DFS 三色标记）。
            enum class Color : std::uint8_t
            {
                White,
                Gray,
                Black
            };
            std::unordered_map<std::uint32_t, Color> color;
            for (const auto &s : steps)
                color[s.id] = Color::White;
            bool cycle = false;
            auto dfs = [&](auto &&self, std::uint32_t id) -> void
            {
                color[id] = Color::Gray;
                const auto *st = by_id.at(id);
                for (auto d : st->depends_on)
                {
                    const auto c = color[d];
                    if (c == Color::Gray)
                    {
                        cycle = true;
                        return;
                    }
                    if (c == Color::White)
                    {
                        self(self, d);
                        if (cycle)
                            return;
                    }
                }
                color[id] = Color::Black;
            };
            for (const auto &s : steps)
            {
                if (color[s.id] == Color::White)
                {
                    dfs(dfs, s.id);
                    if (cycle)
                    {
                        if (why)
                            *why = "dependency cycle detected";
                        return false;
                    }
                }
            }
            return true;
        }

        [[nodiscard]] auto to_json() const -> nlohmann::json
        {
            nlohmann::json j = nlohmann::json::object();
            j["goal"] = goal;
            j["revision"] = revision;
            j["steps"] = nlohmann::json::array();
            for (const auto &s : steps)
                j["steps"].push_back(s.to_json());
            return j;
        }

        static auto from_json(const nlohmann::json &j) -> Plan
        {
            Plan p;
            // 兼容 {plan:{...}} 包裹。
            const nlohmann::json &src =
                (j.is_object() && j.contains("plan") && j["plan"].is_object()) ? j["plan"] : j;
            p.goal = src.value("goal", std::string{});
            p.revision = src.value("revision", 0u);
            if (src.contains("steps") && src["steps"].is_array())
            {
                std::uint32_t auto_id = 1;
                for (const auto &s : src["steps"])
                {
                    if (!s.is_object())
                        continue;
                    auto step = PlanStep::from_json(s);
                    if (step.id == 0)
                        step.id = auto_id;
                    ++auto_id;
                    p.steps.push_back(std::move(step));
                }
            }
            return p;
        }
    };

    // AgentMode.md §一.2「偏差触发 Replan」
    struct Deviation
    {
        enum class Kind : std::uint8_t
        {
            None,
            FailedStep,
            OffTrack,
            BudgetExceeded,
            PlanInvalid
        };
        Kind kind{Kind::None};
        std::uint32_t step_id{};
        std::string detail;

        [[nodiscard]] auto to_json() const -> nlohmann::json
        {
            const std::string_view k = kind == Kind::FailedStep     ? "FailedStep"
                                       : kind == Kind::OffTrack     ? "OffTrack"
                                       : kind == Kind::BudgetExceeded ? "BudgetExceeded"
                                       : kind == Kind::PlanInvalid  ? "PlanInvalid"
                                                                    : "None";
            return nlohmann::json::object(
                {{"kind", k}, {"step_id", step_id}, {"detail", detail}});
        }
    };

} // namespace AE::Engine
