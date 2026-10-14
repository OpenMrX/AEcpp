#pragma once

/// @file AEOtel.hpp
/// @brief OpenTelemetry C++ SDK 桥接 Sink（对应 AgentEngine.md §4.6「OpenTelemetry 接入」）。
///
/// @details
/// ⚠️ 本文件受 VMX_AE_ENABLE_OTEL 宏保护：仅当 CMake 开关 VMX_AE_OTEL=ON
/// （开发阶段默认 ON）时编译并链接 opentelemetry-cpp。目的：
/// - opentelemetry-cpp 已通过 vcpkg 安装可用；开发阶段默认开启，
/// 确保文档描述的可观测性能力在开发环境立即可用；
/// - 部署时可关闭（-DVMX_AE_OTEL=OFF）以减小编译体积与依赖链，
/// 符合「导出管线（OTLP / Collector）按部署形态接入」。
/// 部署需要端到端 Trace 时：cmake -DVMX_AE_OTEL=ON … 并自行选定 exporter 宏。
///
/// 职责：把 AE 语义事件 AERecord 转换为 OTel Span，串成 Trace，覆盖
/// 「一次 run() → 多轮 LLM+工具」的端到端链路；定位长尾延迟与失败根因。
///
/// @note 所属命名空间：VMX::AE

#ifdef VMX_AE_ENABLE_OTEL

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "AETelemetry.hpp"

#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>

// 可选：OTLP / OStream 导出管线。部署形态自行开启其一（并链接对应 opentelemetry-cpp exporter 目标）：
//   -DVMX_AE_OTEL_STDOUT  使用 OStream  exporter（调试，打印到 stdout）
//   -DVMX_AE_OTEL_OTLP    使用 OTLP HTTP exporter（对接 Collector）
#if defined(VMX_AE_OTEL_STDOUT)
#include <opentelemetry/exporters/ostream/span_exporter.h>
#include <opentelemetry/sdk/trace/simple_processor.h>
#elif defined(VMX_AE_OTEL_OTLP)
#include <opentelemetry/exporters/otlp/otlp_http_exporter.h>
#include <opentelemetry/sdk/trace/simple_processor.h>
#endif

namespace VMX::AE
{

    namespace detail
    {
        // 惰性、进程级唯一的 TracerProvider（默认无 processor → Span 合法但不导出；
        // 开启 VMX_AE_OTEL_STDOUT / VMX_AE_OTEL_OTLP 时挂对应 exporter）。
        inline auto get_otel_provider()
            -> std::shared_ptr<opentelemetry::trace::TracerProvider>
        {
            static std::shared_ptr<opentelemetry::trace::TracerProvider> provider;
            static std::once_flag flag;
            std::call_once(flag, []
                           {
                std::unique_ptr<opentelemetry::sdk::trace::SpanProcessor> processor;
#if defined(VMX_AE_OTEL_STDOUT)
                processor = std::make_unique<opentelemetry::sdk::trace::SimpleSpanProcessor>(
                    std::make_unique<opentelemetry::exporter::trace::OStreamSpanExporter>());
#elif defined(VMX_AE_OTEL_OTLP)
                processor = std::make_unique<opentelemetry::sdk::trace::SimpleSpanProcessor>(
                    std::make_unique<opentelemetry::exporter::otlp::OtlpHttpExporter>());
#endif
                provider = std::make_shared<opentelemetry::sdk::trace::TracerProvider>(
                    std::move(processor));
                opentelemetry::trace::Provider::SetTracerProvider(provider);
            });
            return provider;
        }
    } // namespace detail

    // OTel SDK Sink：把 AERecord 转为 Span（name=事件名，属性含 stage/session/dur/status）。
    class AEOtelSink : public IAEEventSink
    {
    public:
        auto emit(const AERecord &r) -> void override
        {
            auto provider = detail::get_otel_provider();
            if (!provider)
                return;
            auto tracer = provider->GetTracer("vmx-ae", "1.0.0");
            if (!tracer)
                return;

            opentelemetry::trace::StartSpanOptions opts;
            opts.kind = opentelemetry::trace::SpanKind::kInternal;
            auto span = tracer->StartSpan(r.name, opts);
            if (!span)
                return;

            // 通过 SetAttribute 注入语义属性（避免直接构造 KeyValueIterable 的接口约束）。
            span->SetAttribute("ae.stage", stage_name(r.stage));
            span->SetAttribute("ae.session_id", r.session_id);
            span->SetAttribute("ae.dur_us", static_cast<int64_t>(r.dur.count()));
            span->SetAttribute("ae.ok", r.ok);
            if (!r.status.empty())
                span->SetAttribute("ae.status", r.status);
            for (const auto &kv : r.attrs)
                span->SetAttribute(kv.first, kv.second);

            span->SetStatus(r.ok ? opentelemetry::trace::StatusCode::kOk
                                 : opentelemetry::trace::StatusCode::kError,
                            r.status);
            span->End();
        }
    };

} // namespace VMX::AE

#endif // VMX_AE_ENABLE_OTEL
