#pragma once

/// @file IAE.hpp
/// @brief 执行器契约 + 依赖注入抽象 + AOP 插入点（hooks）+ 执行期基础设施。
///
/// @details
/// 设计约束（关键）：
/// - 本层只定义【抽象接口】与最小本地实现（Simple*），不硬依赖 DCEE /
/// vLLmProvide::ContextManager / Butler。AgentEngine 完全经这些抽象运行，
/// 可独立编译并自跑单循环单 agent。
/// - DCEE / vLLmProvide::ContextManager 的适配器（把项目基建接到 IToolProvider /
/// IContextSource）落在独立的 AEInfra.hpp，避免本文件引入外部模块头，
/// 保证 VMX::AE 核心可独立编译。
/// - Butler 完全体经 IAgentExecutor 单向消费；Butler 的 LLMRouter / ToolRegistry
/// 可桥接到这些抽象，绝不允许反向依赖 VMX::AE 的实现文件。
///
/// @note 所属命名空间：VMX::AE

#include <deque>
#include <exception>
#include <functional>
#include <generator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "Core/Result.hpp"
#include "Core/Logger.hpp"
#include "AEType.hpp"

namespace VMX::AE
{

    // ============================================================ 依赖注入抽象
    // 工具执行提供方：AgentEngine 经此调用工具，不直接持有 DCEE / 沙箱实现。
    // 签名与 Butler::ToolExecuteFn（Core::Result<nlohmann::json>(name,args)）兼容，
    // 便于 Butler 侧薄适配器桥接。
    class IToolProvider
    {
    public:
        virtual ~IToolProvider() = default;
        // 执行工具；maxRetries 由实现内部处理或透传。返回结构化结果或类型化错误。
        virtual auto execute(std::string_view name,
                             const nlohmann::json &args,
                             int max_retries = 3) -> Core::Result<nlohmann::json> = 0;
        // 暴露工具名清单（供 LLM tool schema 组装 / 权限门控白名单）
        virtual auto list_tools() const -> std::vector<std::string> = 0;

        // 工具 schema（OpenAI 兼容 function 列表），供 LLM tool_choice 与请求体组装。
        // 默认空数组：无工具时 LLM 仅做文本生成；DCEE / 本地注册表可覆盖。
        // 注意：返回的是「声明式 schema」，不含运行态；跨会话持久记忆由 Butler / MemoryStick 经 IStateStore 提供。
        virtual auto get_tools_schema() const -> nlohmann::json { return nlohmann::json::array(); }
    };

    // 上下文来源：AgentEngine 用于 token 计数与硬限熔断；不规定存储介质。
    // （跨会话持久记忆由 Butler / MemoryStick 经 IStateStore 提供，见下。）
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
        // 供 AgentEngine 软限压缩判定（≥阈值触发会话压缩回调）。
        virtual auto usage_ratio() const -> double { return 0.0; }
    };

    // 权限门控：AgentEngine 在每次工具执行前询问；返回 Ok 放行，Error 拦截（含原因）。
    // 本地运行（无 Butler）可传 nullptr 表示全部放行；Butler 完全体注入三级权限实现。
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

    // ============================================================ 执行器接口
    // Butler 完全体单向依赖此接口驱动自主 agent 执行（不重造执行层）。
    class IAgentExecutor
    {
    public:
        virtual ~IAgentExecutor() = default;

        // 主入口：流式产出 AgentEvent（token 粒度）。单循环单 agent。
        virtual auto run(std::string_view session_id,
                         const nlohmann::json &request_body,
                         std::size_t max_iterations = 16)
            -> std::generator<AgentEvent> = 0;

        // 从指定迭代恢复（配合 IStateStore checkpoint）
        virtual auto resume(std::string_view session_id,
                            const nlohmann::json &request_body,
                            std::size_t from_iteration = 0)
            -> std::generator<AgentEvent> = 0;

        // 注册 AOP 钩子，返回句柄（用于后续注销扩展）
        virtual auto register_hook(HookPoint point, Hook hook) -> std::size_t = 0;

        // §11 优雅退出：取消当前 run() / 查询取消状态。
        // 由宿主信号处理器（GracefulShutdown）、消费方或 AOP hook 驱动；
        // AgentEngine 在迭代边界检测并优雅退出（汇合在途工具 worker + 落盘）。
        virtual void request_stop() noexcept = 0;
        virtual bool is_stop_requested() const = 0;
    };

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
            // 相同字符串 → 1.0，完全不同 → 0.0
            return static_cast<double>(hit) / static_cast<double>(sa.size() + sb.size() - hit);
        }
        std::size_t m_window;
        double m_sim;
        std::size_t m_streak = 0;
        std::string m_last_name;
        nlohmann::json m_last_args = nlohmann::json::object();
    };

    // ============================================================ Checkpointing
    // 状态存储抽象（落地时映射到既有 ISessionStorage / sqlite3，或 MemoryStick）
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

    // ============================================================ 最小本地实现
    // 让 VMX::AE 可独立编译自跑（无 DCEE / 无 Butler）。生产接入项目基建时，
    // 用 DCEE / ContextManager 适配器（AEInfra.hpp）替换即可，AgentEngine 代码无需改动。

    class SimpleContextSource : public IContextSource
    {
    public:
        explicit SimpleContextSource(std::size_t hard_limit = 0)
            : m_hard_limit(hard_limit), m_used(0) {}

        auto token_count() const -> std::size_t override { return m_used; }
        auto add_tokens(std::size_t n) -> void override { m_used += n; }
        auto would_exceed_hard_limit(std::size_t additional) const -> bool override
        {
            return m_hard_limit > 0 && (m_used + additional) > m_hard_limit;
        }
        auto hard_limit() const -> std::size_t override { return m_hard_limit; }

        // ─── 扩展接口 ───

        // 设置硬限（context window 大小）
        auto set_hard_limit(std::size_t limit) -> void { m_hard_limit = limit; }

        // 重置计数
        auto reset() -> void { m_used = 0; }

        // 估算剩余可用 token 数
        auto remaining() const -> std::size_t
        {
            if (m_hard_limit == 0 || m_used >= m_hard_limit)
                return 0;
            return m_hard_limit - m_used;
        }

        // 使用率（0.0 ~ 1.0）
        auto usage_ratio() const -> double override
        {
            if (m_hard_limit == 0)
                return 0.0;
            return static_cast<double>(m_used) / static_cast<double>(m_hard_limit);
        }

        // 安全预留比例（返回安全的 max_completion_tokens，预留缓冲）
        // buffer_ratio: 预留比例，默认 0.1（10% 缓冲）
        auto safe_completion_tokens(double buffer_ratio = 0.1) const -> std::size_t
        {
            if (m_hard_limit == 0)
                return 2048; // 默认值
            if (m_used >= m_hard_limit)
                return 0;
            std::size_t avail = m_hard_limit - m_used;
            auto buffered = static_cast<std::size_t>(static_cast<double>(avail) * (1.0 - buffer_ratio));
            // 用 (std::max) 避免 Windows.h 的 max 宏干扰
            return (std::max)(buffered, std::size_t{64});
        }

    private:
        std::size_t m_hard_limit;
        std::size_t m_used = 0;
    };

    // 本地工具注册表（lambda 式）；生产环境由 DCEE 适配器（AEInfra.hpp）替换。
    class SimpleToolProvider : public IToolProvider
    {
    public:
        using ToolFn = std::function<Core::Result<nlohmann::json>(const nlohmann::json &args)>;
        auto register_tool(std::string name, ToolFn fn) -> void
        {
            m_tools[std::move(name)] = std::move(fn);
        }
        auto execute(std::string_view name,
                     const nlohmann::json &args,
                     int /*max_retries*/ = 3) -> Core::Result<nlohmann::json> override
        {
            auto it = m_tools.find(std::string{name});
            if (it == m_tools.end())
                return Core::Result<nlohmann::json>::Error(
                    Core::Error{"ToolNotFound: " + std::string{name}});
            try
            {
                return it->second(args);
            }
            catch (const std::exception &e)
            {
                return Core::Result<nlohmann::json>::Error(
                    Core::Error{std::string{"ToolExec: "} + e.what()});
            }
        }
        auto list_tools() const -> std::vector<std::string> override
        {
            std::vector<std::string> out;
            for (auto &kv : m_tools)
                out.push_back(kv.first);
            return out;
        }
        // 装载本地工具的 OpenAI 兼容 schema（供 LLM tool_choice 与请求体组装）。
        void set_tools_schema(nlohmann::json schema) { m_schema = std::move(schema); }
        auto get_tools_schema() const -> nlohmann::json override { return m_schema; }

    private:
        std::unordered_map<std::string, ToolFn> m_tools;
        nlohmann::json m_schema = nlohmann::json::array();
    };

    class SimpleStateStore : public IStateStore
    {
    public:
        auto save(std::string_view session_id, std::string_view snapshot) -> Core::Result<void> override
        {
            std::scoped_lock lock{m_mu};
            m_data[std::string{session_id}] = std::string{snapshot};
            return Core::Result<void>::Ok();
        }

        auto load(std::string_view session_id) -> Core::Result<std::string> override
        {
            std::scoped_lock lock{m_mu};
            auto it = m_data.find(std::string{session_id});
            if (it == m_data.end())
                return Core::Result<std::string>::Ok(std::string{});
            return Core::Result<std::string>::Ok(it->second);
        }

        auto contains(std::string_view session_id) const -> bool
        {
            std::scoped_lock lock{m_mu};
            return m_data.contains(std::string{session_id});
        }

        auto clear() -> void
        {
            std::scoped_lock lock{m_mu};
            m_data.clear();
        }

        auto remove(std::string_view session_id) -> void
        {
            std::scoped_lock lock{m_mu};
            m_data.erase(std::string{session_id});
        }

    private:
        mutable std::mutex m_mu;
        std::unordered_map<std::string, std::string> m_data;
    };

} // namespace VMX::AE
