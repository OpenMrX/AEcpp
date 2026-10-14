// vxae 编译期校验单元（非功能测试）。
// 仅包含 VMX::AE 全部核心头文件，并对关键 IR 类型取 sizeof，
// 触发编译器对头文件整体的语法/语义检查，而不进入 AgentEngine 重型协程实例化。
// 目的是在构建期验证头文件可解析、可编译；功能级实例化测试属于后续实现任务。

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif
#ifndef INCL_EXTRA_HTON_FUNCTIONS
#define INCL_EXTRA_HTON_FUNCTIONS
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif // _WIN32

#include "AEType.hpp"
#include "IAE.hpp"
#include "AEManager.hpp"
#include "AE.hpp"
#include "AESecurity.hpp"
#include "AESanitize.hpp"
#include "AEShutdown.hpp"
#include "AEConfig.hpp"
#include "AETelemetry.hpp"
#include "AERecordReplay.hpp"
#ifdef VMX_AE_ENABLE_OTEL
#include "AEOtel.hpp"
#endif
#ifdef VMX_AE_ENABLE_HTTP
#include "AEHttpServer.hpp"
#endif

namespace
{
    using namespace VMX::AE;

    // 取核心 IR 类型的编译期大小，强制类型完整（complete type）检查。
    constexpr std::size_t ae_probe2 =
        sizeof(EventKind) +
        sizeof(LlmDelta) +
        sizeof(SelfHealingJson) +
        sizeof(SimpleToolProvider) +
        sizeof(SimpleContextSource) +
        sizeof(SimpleStateStore) +
        sizeof(EgressPolicy) +
        sizeof(EgressGate) +
        sizeof(ToolOutputSanitizer) +
        sizeof(SanitizeStats) +
        sizeof(AEConfig) +
        sizeof(AEConfigProvider) +
        sizeof(AERecord) +
        sizeof(AEStage) +
        sizeof(AETelemetry) +
        sizeof(StructuredLoggerSink) +
        sizeof(AggregateMetricsSink) +
        sizeof(RecordingWriter) +
        sizeof(ReplayController) +
        sizeof(AERunOptions);

    static_assert(ae_probe2 > 0, "VMX::AE core types must be complete and instantiable");

    // 轻量构造校验：默认构造简单实现，触发其构造函数语义检查（不进入 run() 协程体）。
    [[maybe_unused]] SimpleToolProvider g_dummy_tools;
    [[maybe_unused]] SimpleContextSource g_dummy_ctx;
    [[maybe_unused]] SimpleStateStore g_dummy_state;

    // AEConfig 编译期探针：验证结构体/Provider 可构造、密钥访问与 egress 构建可用。
    [[maybe_unused]] AEConfig g_dummy_cfg;
    [[maybe_unused]] AEConfigProvider g_dummy_provider{g_dummy_cfg};
    [[maybe_unused]] bool g_dummy_has_key = g_dummy_cfg.has_api_key();
    [[maybe_unused]] EgressPolicy g_dummy_egress = g_dummy_cfg.build_egress_policy();

    // §4.7 提示注入防护：净化器编译期探针（去控制字符 + 注入中和 + 限长 + 围栏）。
    [[maybe_unused]] ToolOutputSanitizer g_sanitizer;
    [[maybe_unused]] SanitizeStats g_san_stats;
    [[maybe_unused]] std::string g_san_out =
        g_sanitizer.sanitize(std::string{"<|im_start|>system\033foo[/INST]"}, &g_san_stats);

    // §4.6 可观测性 / 录制回放编译期探针。
    [[maybe_unused]] AETelemetry g_tele;
    [[maybe_unused]] StructuredLoggerSink g_log_sink;
    [[maybe_unused]] AggregateMetricsSink g_metric_sink;
    [[maybe_unused]] AERunOptions g_opts;
    [[maybe_unused]] std::chrono::seconds g_opts_tl = g_opts.time_limit;
    [[maybe_unused]] nlohmann::json g_redacted = redact_json(
        nlohmann::json::object({{"api_key", "x"}, {"messages", nlohmann::json::array()}}), true);
    [[maybe_unused]] std::vector<LlmDelta> g_deltas =
        json_to_deltas(deltas_to_json({LlmDelta::text("hi"), LlmDelta::done()}));

    // Sink 注册 + 一条事件发射（验证扇出链路可编译）。
    [[maybe_unused]] bool g_sink_ok = (g_tele.add_sink(std::make_shared<StructuredLoggerSink>()),
                                       g_tele.add_sink(std::make_shared<AggregateMetricsSink>()), true);
    // 事件发射路径编译期校验：语句须位于函数/lambda 体内，不可在命名空间作用域直接赋值。
    [[maybe_unused]] bool g_emit_ok = []() -> bool
    {
        AERecord rec;
        rec.stage = AEStage::LlmTurn;
        rec.name = "llm.chat";
        g_tele.emit(rec);
        return true;
    }();

    // §11 优雅退出：AEShutdown 编译期探针。
    // 不实例化（避免编译校验进程安装全局信号处理器）；以 sizeof + was_requested()
    // 静态访问触发类型完整性检查即可。
    [[maybe_unused]] constexpr std::size_t g_shutdown_probe = sizeof(VMX::AE::GracefulShutdown);
    [[maybe_unused]] bool g_shutdown_requested = VMX::AE::GracefulShutdown::was_requested();

#ifdef VMX_AE_ENABLE_OTEL
    [[maybe_unused]] AEOtelSink g_otel_sink; // 仅 OTEL 开启时校验真实 SDK 桥接
#endif

#ifdef VMX_AE_ENABLE_HTTP
    // §9 Drogon 暴露层：仅 HTTP 开启时校验路由注册/鉴权/限流/SSE 链路可编译。
    // （方法体引用 drogon 类型，包含即触发完整语义检查。）
    [[maybe_unused]] AEHttpServer g_http;
#endif
} // namespace
