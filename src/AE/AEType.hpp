#pragma once

/// @file AEType.hpp
/// @brief 统一类型层：IR（token 流式事件/类型化错误/自修复 JSON）+ 依赖注入抽象
///        （IToolProvider/IContextSource/IPermissionGate/IStateStore）+ AOP 插入点 +
///        执行期基础设施（LoopDetector/Checkpointer）。
///
/// @details
/// 设计约束（关键）：
/// - 本层【不 include】任何宿主实现，保持 AE::Engine 完全解耦，可独立编译并自跑
/// 单循环单 agent。
/// - 结构性调整：原独立接口头 IAE.hpp 已移除，其「所需类型」按序并入本层；抽象执行器
/// IAgentExecutor 已移除（AgentEngine 为具体类）。仅有具体实现身份的 Simple* 位于
/// AESimple.hpp，聚合入口见 AE.hpp。
/// - token 粒度的 LlmDelta 是 AgentEngine 自身的 IR；与外部事件模型是两套模型，
/// 由薄适配器桥接，禁止在 AE::Engine 内引入外部宿主类型。
///
/// @note 所属命名空间：AE::Engine
/// @note 依赖：Core::Result / Core::Logger、nlohmann::json。

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Logger.hpp"

namespace AE::Engine
{

    // ------------------------------------------------------------ 类型化错误
    enum class AgentError
    {
        None,
        MaxIterations,
        LlmTransport,
        LlmParse,
        ToolNotFound,
        ToolExec,
        ContextFull,
        BudgetExceeded,
        ConfigMissing,
        Cancelled // §11 优雅退出（取消信号 / 消费方提前终止）
    };

    // ------------------------------------------------------ LLM 增量事件（IR）
    // LlmDelta 即统一中间表示（StreamEvent）：各后端把自家原始流归一化为 LlmDelta，
    // 上层 AgentEngine 只消费 IR，完全不感知后端差异。token 粒度。
    struct LlmDelta
    {
        enum class Kind
        {
            Text,
            Thinking, // Phase G: reasoning_content 思考内容
            ToolCall,
            Usage,
            Done,
            Error // 流式传输错误（替代协程内 throw，H15 合规）
        } kind{Kind::Text};

        std::string text_content;
        std::string thinking_content; // Phase G: reasoning_content 增量
        std::string error_message;    // Kind::Error 时有效：传输错误信息

        struct ToolCallInfo
        {
            std::string id;
            std::string name;
            nlohmann::json arguments;
        };
        std::optional<ToolCallInfo> tool_call_info;

        struct UsageInfo
        {
            std::size_t prompt_tokens = 0;
            std::size_t completion_tokens = 0;
        };
        std::optional<UsageInfo> usage_info;

        static auto text(std::string_view t) -> LlmDelta
        {
            LlmDelta d;
            d.kind = Kind::Text;
            d.text_content = std::string{t};
            return d;
        }
        static auto thinking(std::string_view t) -> LlmDelta
        {
            LlmDelta d;
            d.kind = Kind::Thinking;
            d.thinking_content = std::string{t};
            return d;
        }
        static auto tool_call(std::string id, std::string name, nlohmann::json a) -> LlmDelta
        {
            LlmDelta d;
            d.kind = Kind::ToolCall;
            d.tool_call_info = ToolCallInfo{std::move(id), std::move(name), std::move(a)};
            return d;
        }
        static auto usage(std::size_t p, std::size_t c) -> LlmDelta
        {
            LlmDelta d;
            d.kind = Kind::Usage;
            d.usage_info = UsageInfo{p, c};
            return d;
        }
        static auto done() -> LlmDelta
        {
            LlmDelta d;
            d.kind = Kind::Done;
            return d;
        }
        static auto error(std::string msg) -> LlmDelta
        {
            LlmDelta d;
            d.kind = Kind::Error;
            d.error_message = std::move(msg);
            return d;
        }
    };

    // -------------------------------------------------- JSON 回退级别
    // 记录 SelfHealingJson 使用了哪一级回退策略，用于日志/遥测/调试。
    enum class JsonFallbackLevel
    {
        None = 0,       // 解析失败，未命中任何级别
        Direct = 1,     // L1 直接 JSON::parse 成功
        MarkdownFence,  // L2 从 ```json ... ``` 代码块提取
        BracketMatch,   // L3 括号匹配找完整 {} + 尾随逗号修复
        RegexExtract,   // L4 正则提取 "answer": "..."（React 独有能力，已合并）
        RawText         // L5 全部失败，将原始文本作为 answer 返回
    };

    // ------------------------------------------- 自修复 JSON 解析
    // LLM 输出的 tool_call arguments 常残缺/不合法；本类按成本递增修复，命中即停。
    //
    // repair()         — 原始 3 级（L1–L3）：仅尝试结构化 JSON，失败返回 nullopt。
    //                     用于 tool_call arguments 修复（StreamingLLMClient）。
    // repairWithFallback() — 增强 5 级（L1–L5）：融入 React 的 L4 正则提取 + L5 裸文本兜底，
    //                         用于 AgentEngine 顶层 LLM 响应解析（无 tool 的纯文本场景）。
    class SelfHealingJson
    {
    public:
        // ---- 原始接口（向后兼容，用于 tool_call args 修复） ----
        static auto repair(std::string_view raw) -> std::optional<nlohmann::json>
        {
            auto [json, level] = repairWithFallback(raw);
            (void)level;
            return json;
        }

        // ---- 增强接口：5 级回退 + 级别追踪 ----
        // 返回 (解析结果, 命中级别)。L5 裸文本兜底时结果包含 {"answer": raw_text}。
        static auto repairWithFallback(std::string_view raw)
            -> std::pair<std::optional<nlohmann::json>, JsonFallbackLevel>
        {
            std::string s = strip_markdown(raw);    // 1) 去 ```json 围栏与解释文字
            s = sanitize(s);                        // 2) 尾随逗号/括号配平
            for (auto &cand : truncate_to_valid(s)) // 3) 截断到首个可解析 JSON 边界
            {
                try
                {
                    auto j = nlohmann::json::parse(cand);
                    return {j, JsonFallbackLevel::BracketMatch};
                }
                catch (...)
                {
                }
            }

            // ---- React L4：正则提取 "answer": "..." ----
            auto regex_answer = extract_answer_regex(raw);
            if (regex_answer.has_value())
            {
                return {nlohmann::json::object({{"answer", *regex_answer}}),
                        JsonFallbackLevel::RegexExtract};
            }

            // ---- React L5：全部失败，原始文本作为 answer 兜底 ----
            std::string trimmed = trim_copy(raw);
            if (!trimmed.empty())
            {
                return {nlohmann::json::object({{"answer", trimmed}}),
                        JsonFallbackLevel::RawText};
            }

            return {std::nullopt, JsonFallbackLevel::None};
        }

    private:
        static auto trim_copy(std::string_view sv) -> std::string
        {
            auto start = sv.find_first_not_of(" \t\n\r");
            if (start == std::string_view::npos)
                return {};
            auto end = sv.find_last_not_of(" \t\n\r");
            return std::string{sv.substr(start, end - start + 1)};
        }

        static auto strip_markdown(std::string_view in) -> std::string
        {
            std::string s(in);
            auto a = s.find("```");
            if (a != std::string::npos)
            {
                auto start = a + 3;
                auto nl = s.find('\n', start);
                if (nl != std::string::npos && nl - start < 20)
                    start = nl + 1;
                auto b = s.find("```", start);
                if (b != std::string::npos)
                {
                    s = s.substr(start, b - start);
                    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                        s.pop_back();
                    return s;
                }
            }
            return s;
        }

        static auto sanitize(std::string s) -> std::string
        {
            size_t pos = 0;
            while ((pos = s.find(",}")) != std::string::npos)
                s.replace(pos, 2, "}");
            while ((pos = s.find(",]")) != std::string::npos)
                s.replace(pos, 2, "]");
            int depth = 0;
            for (char c : s)
            {
                if (c == '{' || c == '[')
                    ++depth;
                else if (c == '}' || c == ']')
                    --depth;
            }
            while (depth-- > 0)
                s += '}';
            return s;
        }

        static auto truncate_to_valid(std::string_view in) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            std::string cur;
            int depth = 0;
            for (char c : in)
            {
                cur.push_back(c);
                if (c == '{' || c == '[')
                    ++depth;
                else if (c == '}' || c == ']')
                {
                    --depth;
                    if (depth == 0)
                        out.push_back(cur);
                }
            }
            std::reverse(out.begin(), out.end());
            return out;
        }

        // React L4：提取 "answer": "..."（支持转义引号）。用字符串查找替代 std::regex，
        // 规避 MSVC Debug 下 std::regex 对真实模型文本的堆损坏/栈溢出风险。
        static auto extract_answer_regex(std::string_view raw) -> std::optional<std::string>
        {
            constexpr std::string_view kKey{"\"answer\""};
            const auto key_pos = raw.find(kKey);
            if (key_pos == std::string_view::npos)
                return std::nullopt;

            // 跳过键与空白，定位冒号
            auto i = key_pos + kKey.size();
            while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\n' || raw[i] == '\r'))
                ++i;
            if (i >= raw.size() || raw[i] != ':')
                return std::nullopt;
            ++i;
            while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\n' || raw[i] == '\r'))
                ++i;
            if (i >= raw.size() || raw[i] != '"')
                return std::nullopt;
            ++i;

            // 逐字符解析字符串值，支持 \x 转义
            std::string out;
            while (i < raw.size())
            {
                const char c = raw[i];
                if (c == '"')
                    return out;
                if (c == '\\' && i + 1 < raw.size())
                {
                    ++i;
                    out += raw[i];
                }
                else
                {
                    out += c;
                }
                ++i;
            }
            return std::nullopt; // 未闭合引号
        }
    };

    // ---------------------------------------------------- 对外事件（编排门面产出）
    enum class EventKind
    {
        Thinking,
        Token,
        ToolCall,
        ToolResult,
        Final,
        Error,
        // ---- 执行模式（Plan-and-Execute）事件（Docs/01 §4.3.5）----
        PlanCreated,      // payload: 计划 JSON（含 revision）
        StepStarted,      // payload: 步骤 JSON（id/description）
        StepFinished,     // payload: 步骤 JSON（id/status/result）
        ReplanTriggered,  // payload: 偏差 JSON（kind/step_id/detail）
        Aggregating,      // 进入聚合阶段
        // ---- 反思纠错（Reflection）事件（Docs/01 §4.4）----
        ReflectionEvaluated, // payload: 评审 JSON（passed/score/fingerprint）
        ReflectionExhausted  // payload: fingerprint；反馈原地打转或无进展，已熔断
    };

    struct AgentEvent
    {
        EventKind kind{EventKind::Thinking};
        std::string payload;
        Core::Error error_info; // 仅 Error 事件使用

        static auto token(std::string_view t) -> AgentEvent
        {
            AgentEvent e;
            e.kind = EventKind::Token;
            e.payload = std::string{t};
            return e;
        }
        static auto tool_call(std::string_view name) -> AgentEvent
        {
            AgentEvent e;
            e.kind = EventKind::ToolCall;
            e.payload = std::string{name};
            return e;
        }
        static auto tool_result(std::string_view r) -> AgentEvent
        {
            AgentEvent e;
            e.kind = EventKind::ToolResult;
            e.payload = std::string{r};
            return e;
        }
        static auto error(Core::Error err) -> AgentEvent
        {
            AgentEvent e;
            e.kind = EventKind::Error;
            e.error_info = std::move(err);
            return e;
        }
        static auto final_event(std::string_view t = "") -> AgentEvent
        {
            AgentEvent e;
            e.kind = EventKind::Final;
            e.payload = std::string{t};
            return e;
        }
    };

    // ================================================================
    //  依赖注入抽象 + AOP 插入点 + 执行期基础设施
    //
    //  说明（结构性调整）：本节类型原位于已移除的 IAE.hpp。为消除独立接口头，
    //  将「所需类型」按序合并至本 IR 层；仅有具体实现身份的 Simple* 迁至 AESimple.hpp。
    //  抽象执行器 IAgentExecutor 已移除——AgentEngine 收敛为具体类（零多态开销）。
    // ================================================================

    // ---------------------------------------------------- 工具执行提供方
    // AgentEngine 经此调用工具，不直接持有 DCEE / 沙箱实现。
    class IToolProvider
    {
    public:
        virtual ~IToolProvider() = default;
        // 执行工具；max_retries 由实现内部处理或透传。返回结构化结果或类型化错误。
        virtual auto execute(std::string_view name,
                             const nlohmann::json &args,
                             int max_retries = 3) -> Core::Result<nlohmann::json> = 0;
        // 暴露工具名清单（供 LLM tool schema 组装 / 权限门控白名单）
        virtual auto list_tools() const -> std::vector<std::string> = 0;

        // 工具 schema（OpenAI 兼容 function 列表），供 LLM tool_choice 与请求体组装。
        // 默认空数组：无工具时 LLM 仅做文本生成；本地注册表 / 外部实现可覆盖。
        virtual auto get_tools_schema() const -> nlohmann::json { return nlohmann::json::array(); }
    };

    // ---------------------------------------------------- 上下文来源
    // AgentEngine 用于 token 计数与硬限熔断；不规定存储介质。
    class IContextSource
    {
    public:
        virtual ~IContextSource() = default;
        virtual auto token_count() const -> std::size_t = 0;
        virtual auto add_tokens(std::size_t n) -> void = 0;
        // 若再加 additional 个 token 会越过硬限则返回 true（熔断判定）
        virtual auto would_exceed_hard_limit(std::size_t additional) const -> bool = 0;
        virtual auto hard_limit() const -> std::size_t = 0;
        // 当前上下文使用率（0.0 ~ 1.0）；hard_limit==0 时返回 0.0。
        virtual auto usage_ratio() const -> double { return 0.0; }
    };

    // ---------------------------------------------------- 权限门控
    // AgentEngine 在每次工具执行前询问；返回 Ok 放行，Error 拦截（含原因）。
    class IPermissionGate
    {
    public:
        virtual ~IPermissionGate() = default;
        virtual auto can_execute_tool(std::string_view name,
                                      const nlohmann::json &args)
            -> Core::Result<void> = 0;
    };

    // ============================================================ AOP 插入点
    enum class HookPoint
    {
        BeforeLLM,   // LLM 回合前（可改 request_body）
        AfterTool,   // 工具执行后（可审查/改写结果）
        OnIteration, // 每次迭代边界
        OnError      // 错误事件产生前（可拦截/转换）
    };

    enum class HookControl
    {
        Continue, // 正常继续
        Abort,    // 终止循环，立即以 Final 结束
        Retry     // 重试当前 LLM 回合（谨慎使用，避免活锁）
    };

    struct HookContext
    {
        HookPoint point{};
        std::string session_id;
        std::size_t iteration = 0;

        nlohmann::json *request_body = nullptr;              // BeforeLLM 有效
        const LlmDelta *llm_delta = nullptr;                 // 预留：AfterLLM 片段
        const std::string *tool_name = nullptr;              // AfterTool 有效
        Core::Result<nlohmann::json> *tool_result = nullptr; // AfterTool 有效（可改写）
        const Core::Error *error = nullptr;                  // OnError 有效
    };

    using Hook = std::function<HookControl(HookContext &)>;

    // ============================================================ 循环检测
    // 连续 window 次同 key 且参数相似度≥sim → 判定循环并熔断。
    class LoopDetector
    {
    public:
        explicit LoopDetector(std::size_t window = 3, double sim = 0.95)
            : m_window(window), m_sim(sim) {}
        auto observe(std::string_view name, const nlohmann::json &args) -> bool
        {
            const double s = similarity(args, m_last_args);
            const bool same = (name == m_last_name) && (s >= m_sim);
            m_streak = same ? m_streak + 1 : 1;
            m_last_name = std::string{name};
            m_last_args = args;
            return m_streak >= m_window;
        }
        void reset()
        {
            m_streak = 0;
            m_last_name.clear();
            m_last_args = nlohmann::json::object();
        }

    private:
        static auto similarity(const nlohmann::json &a, const nlohmann::json &b) -> double
        {
            // 结构相似度近似：归一化 JSON 字符串的字符重合比例（生产可换 Jaccard/余弦）
            const std::string sa = a.dump();
            const std::string sb = b.dump();
            if (sa.empty() && sb.empty())
                return 1.0;
            if (sa.empty() || sb.empty())
                return 0.0;
            std::unordered_set<char> setb(sb.begin(), sb.end());
            size_t hit = 0;
            for (char c : sa)
                if (setb.contains(c))
                    ++hit;
            // 重叠比例：hit / (sa + sb - hit)
            return static_cast<double>(hit) / static_cast<double>(sa.size() + sb.size() - hit);
        }
        std::size_t m_window;
        double m_sim;
        std::size_t m_streak = 0;
        std::string m_last_name;
        nlohmann::json m_last_args = nlohmann::json::object();
    };

    // ============================================================ Checkpointing
    // 状态存储抽象（落地时映射到既有 ISessionStorage / sqlite3，或外部记忆实现）
    class IStateStore
    {
    public:
        virtual ~IStateStore() = default;
        virtual auto save(std::string_view session_id, std::string_view snapshot) -> Core::Result<void> = 0;
        virtual auto load(std::string_view session_id) -> Core::Result<std::string> = 0; // 空=无快照
    };

    class Checkpointer
    {
        IStateStore &m_store;

    public:
        explicit Checkpointer(IStateStore &s) : m_store(s) {}
        auto save(std::string_view sid, const nlohmann::json &state) -> Core::Result<void>
        {
            return m_store.save(sid, state.dump()); // 原子写由存储实现保证（临时文件+rename）
        }
        auto resume(std::string_view sid) -> Core::Result<std::optional<nlohmann::json>>
        {
            auto r = m_store.load(sid);
            if (r.isError())
                return Core::Result<std::optional<nlohmann::json>>::Error(r.error());
            if (r.value().empty())
                return Core::Result<std::optional<nlohmann::json>>::Ok(std::nullopt);
            try
            {
                return Core::Result<std::optional<nlohmann::json>>::Ok(nlohmann::json::parse(r.value()));
            }
            catch (...)
            {
                return Core::Result<std::optional<nlohmann::json>>::Ok(std::nullopt);
            }
        }
    };

} // namespace AE::Engine
