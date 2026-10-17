#pragma once

/// @file Mode/IReflector.hpp
/// @brief 评审器抽象 + 双实现（LLM 评审 / 规则评审）（Docs/01 §4.4.2、§4.4.4）。
///
/// @details
/// - `Critique` / `IReflector` 为反思模式提供「评估—反馈」边界抽象（每次评审一次调用）。
/// - `LlmReflector`（默认）：用 `ctx.llm` 评审，JSON 经 `SelfHealingJson` 自愈解析。
/// - `RuleReflector`：确定性规则（长度/必备词/禁用词/正则），无 LLM 依赖。
/// - `critique_fingerprint`：无效循环检测用（issues 排序 + feedback 的哈希）。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <algorithm>
#include <cstddef>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Error.hpp"

#include "AEType.hpp"     // SelfHealingJson / LlmDelta
#include "LLMAdapter.hpp" // LLMAdapter
#include "Mode/ModeContext.hpp"

namespace AE::Engine
{

    struct Critique
    {
        bool passed{false};
        double score{0.0};                     // [0,1]
        std::string feedback;                  // 注入下一轮生成
        std::vector<std::string> issues;       // 具体问题清单
        std::string fingerprint;               // 无效循环检测用（§4.4.4）

        [[nodiscard]] auto to_json() const -> nlohmann::json
        {
            return nlohmann::json::object({{"passed", passed},
                                           {"score", score},
                                           {"feedback", feedback},
                                           {"issues", issues},
                                           {"fingerprint", fingerprint}});
        }
    };

    // 无效循环检测：fingerprint = hash(sorted(issues) + feedback)。
    [[nodiscard]] inline auto critique_fingerprint(const std::vector<std::string> &issues,
                                                   std::string_view feedback) -> std::string
    {
        std::vector<std::string> sorted = issues;
        std::sort(sorted.begin(), sorted.end());
        std::string s;
        for (const auto &i : sorted)
        {
            s += i;
            s += '\x1f';
        }
        s.append(feedback);
        return std::to_string(std::hash<std::string>{}(s));
    }

    class IReflector
    {
    public:
        virtual ~IReflector() = default;
        virtual auto evaluate(std::string_view output,
                              std::string_view criteria,
                              ModeContext &ctx) -> Core::Result<Critique> = 0;
    };

    // ---------------------------------------------------- LLM 评审器（默认）
    // 评审质量随模型提升；可经构造参数指定更强模型（model 覆盖）。
    class LlmReflector final : public IReflector
    {
    public:
        explicit LlmReflector(std::string model_override = {}) : m_model(std::move(model_override)) {}

        auto evaluate(std::string_view output, std::string_view criteria, ModeContext &ctx)
            -> Core::Result<Critique> override
        {
            const char *kSys =
                "你是严格的质量评审器。依据「评审标准」评估「候选产出」，仅输出 JSON："
                "{\"passed\":true|false,\"score\":0..1,\"feedback\":\"...\",\"issues\":[\"...\"]}。"
                "score 为 0~1 的达标度；feedback 给出可执行的改进建议；issues 为具体问题清单。";
            nlohmann::json body = nlohmann::json::object();
            const auto &src = ctx.request_body;
            if (src.is_object())
                for (const char *k : {"model", "temperature", "top_p", "max_tokens"})
                    if (src.contains(k))
                        body[k] = src[k];
            if (!m_model.empty())
                body["model"] = m_model;
            body["stream"] = false;
            std::string user = "评审标准：\n";
            user += criteria.empty() ? "(未提供明确标准，按通用正确性/完整性/可读性评审)" : std::string{criteria};
            user += "\n\n候选产出：\n";
            user += std::string{output};
            body["messages"] = nlohmann::json::array(
                {nlohmann::json::object({{"role", "system"}, {"content", std::string{kSys}}}),
                 nlohmann::json::object({{"role", "user"}, {"content", std::move(user)}})});

            auto text = collect_text(ctx.llm, body);
            if (text.isError())
                return Core::Result<Critique>::Error(text.error());

            auto [json, level] = SelfHealingJson::repairWithFallback(text.value());
            (void)level;
            if (!json.has_value() || !json->is_object())
                return Core::Result<Critique>::Error(
                    Core::Error(Core::ErrorCode::SerializationError, "reflector: response is not a JSON object"));

            Critique c;
            c.passed = json->value("passed", false);
            c.score = json->value("score", 0.0);
            c.feedback = json->value("feedback", std::string{});
            if (json->contains("issues") && (*json)["issues"].is_array())
                for (const auto &i : (*json)["issues"])
                    if (i.is_string())
                        c.issues.push_back(i.get<std::string>());
            if (c.feedback.empty() && !c.issues.empty())
                c.feedback = c.issues.front();
            c.fingerprint = critique_fingerprint(c.issues, c.feedback);
            return Core::Result<Critique>::Ok(std::move(c));
        }

    private:
        static auto collect_text(LLMAdapter &llm, const nlohmann::json &body) -> Core::Result<std::string>
        {
            std::string out;
            try
            {
                for (auto &&d : llm.stream(body))
                    if (d.kind == LlmDelta::Kind::Text)
                        out += d.text_content;
                    else if (d.kind == LlmDelta::Kind::Error)
                        return Core::Result<std::string>::Error(
                            Core::Error(Core::ErrorCode::NetworkError, d.error_message));
            }
            catch (const std::exception &e)
            {
                return Core::Result<std::string>::Error(
                    Core::Error(Core::ErrorCode::NetworkError, std::string{e.what()}));
            }
            if (out.empty())
                return Core::Result<std::string>::Error(
                    Core::Error(Core::ErrorCode::SerializationError, "reflector: empty LLM response"));
            return Core::Result<std::string>::Ok(std::move(out));
        }

        std::string m_model;
    };

    // ---------------------------------------------------- 规则评审器（确定性）
    // 格式 / 类型 / 长度 / 正则（Docs/01 §4.4.2）。
    struct ReflectionRules
    {
        std::size_t min_len = 0; // 0 = 不限
        std::size_t max_len = 0; // 0 = 不限
        std::vector<std::string> must_contain;
        std::vector<std::string> must_not_contain;
        std::string regex; // 空 = 不限
    };

    class RuleReflector final : public IReflector
    {
    public:
        explicit RuleReflector(ReflectionRules rules = {}) : m_rules(std::move(rules)) {}

        auto evaluate(std::string_view output, std::string_view /*criteria*/, ModeContext & /*ctx*/)
            -> Core::Result<Critique> override
        {
            Critique c;
            std::size_t checks = 0;
            auto check = [&](bool ok, std::string issue)
            {
                ++checks;
                if (!ok)
                    c.issues.push_back(std::move(issue));
            };

            if (m_rules.min_len > 0)
                check(output.size() >= m_rules.min_len,
                      "length below min (" + std::to_string(output.size()) + " < " +
                          std::to_string(m_rules.min_len) + ")");
            if (m_rules.max_len > 0)
                check(output.size() <= m_rules.max_len,
                      "length above max (" + std::to_string(output.size()) + " > " +
                          std::to_string(m_rules.max_len) + ")");
            for (const auto &s : m_rules.must_contain)
                check(output.find(s) != std::string_view::npos, "missing required: " + s);
            for (const auto &s : m_rules.must_not_contain)
                check(output.find(s) == std::string_view::npos, "contains forbidden: " + s);
            if (!m_rules.regex.empty())
            {
                std::regex re;
                bool ok = false;
                try
                {
                    re = std::regex{m_rules.regex};
                    ok = std::regex_search(std::string{output}, re);
                }
                catch (const std::regex_error &)
                {
                    ok = false;
                }
                check(ok, "regex not matched: " + m_rules.regex);
            }

            c.passed = c.issues.empty();
            c.score = checks == 0 ? (c.passed ? 1.0 : 0.0)
                                  : 1.0 - static_cast<double>(c.issues.size()) / static_cast<double>(checks);
            if (!c.issues.empty())
                c.feedback = c.issues.front();
            c.fingerprint = critique_fingerprint(c.issues, c.feedback);
            return Core::Result<Critique>::Ok(std::move(c));
        }

    private:
        ReflectionRules m_rules;
    };

} // namespace AE::Engine
