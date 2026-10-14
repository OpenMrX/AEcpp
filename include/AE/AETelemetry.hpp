#pragma once

/// @file AETelemetry.hpp
/// @brief 可观测性门面（对应 AgentEngine.md §4.6「可观测性」核心部分）。
///
/// @details
/// 设计约束（关键）：
/// - 纯 VMX::AE 模块；仅依赖 Core::Result/Logger、AEType 与标准库，保持核心解耦。
/// - 采用「语义事件 + 可插拔 Sink」抽象：AE 业务代码只吐 AERecord，
/// 具体落地（JSON 日志 / 内存聚合 / OTel SDK）由 Sink 实现，互不影响。
/// → 这是 §4.6 要求的「OpenTelemetry 接入」的安全扩展点：OTel 走 AEOtel.hpp
/// 可选 Sink，默认不链 opentelemetry-cpp，避免把重型 SDK 拖入 AE 默认构建。
/// - 每次 run() 生成随机 trace_id，贯穿 LLM 调用与工具执行（§4.6）。
/// - 脱敏责任在调用方 / RecordingWriter：本门面不主动扫描 secrets，仅在
/// StructuredLoggerSink 输出结构化 JSON 行时原样转义属性值。
///
/// @note 所属命名空间：VMX::AE

#include <chrono>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "AEType.hpp"
#include "Core/Logger.hpp"
#include "Core/Result.hpp"

namespace VMX::AE
{

    // 可观测阶段（语义化；OTel 侧映射为 span name）
    enum class AEStage
    {
        Run,         // 一次 run() 全程
        LlmTurn,     // 单次 LLM 回合（含 SSE 全部分块）
        ToolExec,    // 单次工具执行
        Checkpoint,  // Checkpointing 落盘
        SelfHeal,    // 自愈/自动修复提示
        EgressDeny,  // egress 被权限门拦截
        Error        // 错误事件
    };

    [[nodiscard]] inline auto stage_name(AEStage s) -> std::string
    {
        switch (s)
        {
            case AEStage::Run:
                return "run";
            case AEStage::LlmTurn:
                return "llm";
            case AEStage::ToolExec:
                return "tool";
            case AEStage::Checkpoint:
                return "checkpoint";
            case AEStage::SelfHeal:
                return "self_heal";
            case AEStage::EgressDeny:
                return "egress_deny";
            case AEStage::Error:
                return "error";
        }
        return "unknown";
    }

    // 一条语义遥测记录（结构化、可序列化为 JSON / OTel span）
    struct AERecord
    {
        std::string trace_id;
        std::string session_id;
        AEStage stage = AEStage::Run;
        std::string name;  // 事件/span 名，如 "llm.chat"、工具名
        std::chrono::system_clock::time_point ts = std::chrono::system_clock::now();
        std::chrono::microseconds dur{0};
        bool ok = true;
        std::string status;  // 如 "ok"、"error:LoopDetected"
        std::unordered_map<std::string, std::string> attrs;
    };

    // 遥测下沉抽象：AE 业务代码只调 AETelemetry::emit，具体落地由实现决定。
    class IAEEventSink
    {
    public:
        virtual ~IAEEventSink() = default;
        virtual auto emit(const AERecord &) -> void = 0;
    };

    // ---------------------------------------------------- 默认 Sink：结构化 JSON 日志
    // 经 Core::Logger 输出单行 JSON（含 trace_id/session_id/stage/event/dur_us/ok/attrs），
    // 便于 ELK/Loki 采集与告警。属性值原样转义，不含密钥（调用方已脱敏）。
    class StructuredLoggerSink : public IAEEventSink
    {
    public:
        auto emit(const AERecord &r) -> void override
        {
            std::string line = "{";
            line += "\"trace_id\":\"" + escape(r.trace_id) + "\"";
            line += ",\"session_id\":\"" + escape(r.session_id) + "\"";
            line += ",\"stage\":\"" + stage_name(r.stage) + "\"";
            line += ",\"event\":\"" + escape(r.name) + "\"";
            line += ",\"dur_us\":" + std::to_string(r.dur.count());
            line += std::string(",\"ok\":") + (r.ok ? "true" : "false");
            if (!r.status.empty())
                line += ",\"status\":\"" + escape(r.status) + "\"";
            if (!r.attrs.empty())
            {
                line += ",\"attrs\":{";
                bool first = true;
                for (const auto &kv : r.attrs)
                {
                    if (!first)
                        line += ",";
                    line += "\"" + escape(kv.first) + "\":\"" + escape(kv.second) + "\"";
                    first = false;
                }
                line += "}";
            }
            line += "}";
            if (r.ok)
                Core::Logger::getInstance().infoFmt("[AE:OTel] {}", line);
            else
                Core::Logger::getInstance().errorFmt("[AE:OTel] {}", line);
        }

    private:
        // 仅转义 JSON 字符串里的引号与反斜杠/控制字符，避免破坏日志行结构。
        static auto escape(std::string_view s) -> std::string
        {
            std::string out;
            out.reserve(s.size() + 2);
            for (char c : s)
            {
                if (c == '"' || c == '\\')
                    out += '\\';
                if (c < 0x20)  // 控制字符直接丢弃，防止日志注入/换行破坏
                    continue;
                out += c;
            }
            return out;
        }
    };

    // ---------------------------------------------------- 默认 Sink：内存聚合指标
    // 实时聚合 LLM/工具调用次数、失败数、累计耗时，供成本计量 / 限流 / 健康度面板。
    struct AEMetricsSnapshot
    {
        std::size_t llm_calls = 0;
        std::size_t tool_calls = 0;
        std::size_t tool_failures = 0;
        std::size_t errors = 0;
        std::chrono::microseconds llm_dur{0};
        std::chrono::microseconds tool_dur{0};
    };

    class AggregateMetricsSink : public IAEEventSink
    {
    public:
        auto emit(const AERecord &r) -> void override
        {
            std::lock_guard<std::mutex> lk{m_mutex};
            switch (r.stage)
            {
                case AEStage::LlmTurn:
                    m_.llm_calls++;
                    m_.llm_dur += r.dur;
                    break;
                case AEStage::ToolExec:
                    m_.tool_calls++;
                    m_.tool_dur += r.dur;
                    if (!r.ok)
                        m_.tool_failures++;
                    break;
                case AEStage::Error:
                    m_.errors++;
                    break;
                default:
                    break;
            }
        }
        [[nodiscard]] auto snapshot() const -> AEMetricsSnapshot
        {
            std::lock_guard<std::mutex> lk{m_mutex};
            return m_;
        }

    private:
        mutable std::mutex m_mutex;
        AEMetricsSnapshot m_;
    };

    // ---------------------------------------------------- 遥测门面（扇出 + RAII Span）
    class AETelemetry
    {
    public:
        auto add_sink(std::shared_ptr<IAEEventSink> s) -> void
        {
            if (!s)
                return;
            std::unique_lock lock{m_mutex};
            m_sinks.push_back(std::move(s));
        }
        auto emit(const AERecord &r) const -> void
        {
            std::shared_lock lock{m_mutex};
            for (const auto &s : m_sinks)
                s->emit(r);
        }

        // RAII Span：构造即记起点，析构时计算耗时并 emit（跨 suspend 点安全，仅计时）。
        class Span
        {
        public:
            Span(const AETelemetry *t, std::string trace_id, std::string sid, AEStage stage, std::string name)
                : m_t(t),
                  m_trace(std::move(trace_id)),
                  m_sid(std::move(sid)),
                  m_stage(stage),
                  m_name(std::move(name)),
                  m_start(std::chrono::steady_clock::now())
            {
            }
            ~Span()
            {
                if (!m_t)
                    return;
                AERecord r;
                r.trace_id = m_trace;
                r.session_id = m_sid;
                r.stage = m_stage;
                r.name = m_name;
                r.ts = std::chrono::system_clock::now();
                r.dur =
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_start);
                r.ok = m_ok;
                r.status = m_status;
                r.attrs = m_attrs;
                m_t->emit(r);
            }
            auto set_ok(bool ok, std::string status = "") -> void
            {
                m_ok = ok;
                m_status = std::move(status);
            }
            auto attr(std::string k, std::string v) -> void
            {
                m_attrs.emplace(std::move(k), std::move(v));
            }
            Span(const Span &) = delete;
            auto operator=(const Span &) -> Span & = delete;
            Span(Span &&) = default;
            auto operator=(Span &&) -> Span & = default;

        private:
            const AETelemetry *m_t;
            std::string m_trace, m_sid, m_name, m_status;
            AEStage m_stage;
            std::chrono::steady_clock::time_point m_start;
            std::unordered_map<std::string, std::string> m_attrs;
            bool m_ok = true;
        };

        // 便捷构造（返回值须在本作用域保持存活，析构时自动上报）
        [[nodiscard]] auto span(std::string_view trace_id,
                                std::string_view sid,
                                AEStage stage,
                                std::string_view name) const -> Span
        {
            return Span{this, std::string{trace_id}, std::string{sid}, stage, std::string{name}};
        }

    private:
        mutable std::shared_mutex m_mutex;
        std::vector<std::shared_ptr<IAEEventSink>> m_sinks;
    };

    // 生成一次 run() 的 trace_id（VXAE 独立版：标准库随机 128-bit hex，去 stduuid/Ole32 依赖，
    // 仍满足 §4.6「贯穿 LLM/工具执行」的唯一追踪需求）。
    [[nodiscard]] inline auto make_trace_id() -> std::string
    {
        thread_local std::mt19937_64 gen{std::random_device{}()};
        const auto hi = gen();
        const auto lo = gen();
        return std::format("{:016x}{:016x}", hi, lo);
    }

}  // namespace VMX::AE
