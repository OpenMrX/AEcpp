#pragma once

/// @file AEManager.hpp
/// @brief AE 工厂 / 管理器：集中构建 LLM 适配层与执行引擎（工厂模式）。
///
/// @details
/// 设计约束（关键）：
/// - 不 include Butler / MemoryStick；LlmDelta（见 AEType.hpp）为统一 IR。
/// - 以「工厂」职责集中装配：按后端创建 LLMAdapter（Vllm/Ollama），并把已注入的
///   端口（IToolProvider / IContextSource / IPermissionGate / IStateStore）装配为
///   AgentEngine；未提供端口时使用内建最小本地实现（Simple*）。
/// - 仅持有对外部传入资源的引用 / 独占适配器；不拥有注入端口（生命周期由调用方保证）。
///
/// 结构性调整：本文件原为「LLM 聚合头」，现改为 AE 工厂/管理器类（AEManager）。
/// LLM 客户端与适配仍分别位于 StreamingLLMClient.hpp / LLMAdapter.hpp。
///
/// @note 所属命名空间：AE::Engine
/// @note 依赖：libcurl, nlohmann, Core::Result/Logger。

#include <memory>
#include <utility>

#include "AEType.hpp"
#include "AESimple.hpp"
#include "StreamingLLMClient.hpp"
#include "LLMAdapter.hpp"
#include "AgentEngine.hpp"

namespace AE::Engine
{

    // 后端选择：决定工厂产出的 LLMAdapter 具体类型。
    enum class LlmBackend
    {
        Vllm,  // vLLM / OpenAI 兼容（默认）
        Ollama // Ollama（非流式工具调用缓冲）
    };

    // ---------------------------------------------------- AE 工厂 / 管理器
    // 集中装配 AE 运行时对象：LLM 适配器（多后端）、执行引擎。
    // 轻量、无状态化到「客户端引用 + 后端选择」；适配器由本对象独占持有。
    class AEManager
    {
    public:
        explicit AEManager(StreamingLLMClient &client,
                           LlmBackend backend = LlmBackend::Vllm)
            : m_client(client),
              m_backend(backend),
              m_adapter(make_adapter(client, backend))
        {
        }

        // ── 工厂：LLM 适配器 ──
        // 按后端把 StreamingLLMClient 归一化为统一 IR（LlmDelta）的适配器。
        [[nodiscard]] static auto make_adapter(StreamingLLMClient &client, LlmBackend backend)
            -> std::unique_ptr<LLMAdapter>
        {
            switch (backend)
            {
            case LlmBackend::Ollama:
                return std::make_unique<OllamaAdapter>(client);
            case LlmBackend::Vllm:
            default:
                return std::make_unique<VllmAdapter>(client);
            }
        }

        // ── 工厂：执行引擎（显式注入端口）──
        // 由调用方提供端口；返回的 AgentEngine 引用这些端口与本公司适配器，
        // 故 AEManager 与端口须比引擎长寿。
        [[nodiscard]] auto make_engine(IToolProvider &tools,
                                       IContextSource &ctx,
                                       IPermissionGate *gate = nullptr,
                                       IStateStore *store = nullptr,
                                       std::shared_ptr<AETelemetry> telemetry = nullptr,
                                       AERunOptions run_opts = {})
            -> std::unique_ptr<AgentEngine>
        {
            return std::make_unique<AgentEngine>(tools, ctx, *m_adapter, gate, store,
                                                 std::move(telemetry), std::move(run_opts));
        }

        // ── 工厂：执行引擎（内建最小本地端口自跑）──
        // 使用本对象持有的 Simple* 端口；本对象须比返回的引擎长寿。
        [[nodiscard]] auto make_engine(AERunOptions run_opts = {})
            -> std::unique_ptr<AgentEngine>
        {
            return make_engine(m_default_tools, m_default_ctx,
                               /*gate=*/nullptr, &m_default_state,
                               /*telemetry=*/nullptr, std::move(run_opts));
        }

        // ── 访问器 ──
        auto client() noexcept -> StreamingLLMClient & { return m_client; }
        auto adapter() noexcept -> LLMAdapter & { return *m_adapter; }
        auto backend() const noexcept -> LlmBackend { return m_backend; }

        // 内建端口（make_engine() 无参重载使用；亦可直接访问）
        auto default_tools() noexcept -> SimpleToolProvider & { return m_default_tools; }
        auto default_context() noexcept -> SimpleContextSource & { return m_default_ctx; }
        auto default_state() noexcept -> SimpleStateStore & { return m_default_state; }

    private:
        StreamingLLMClient &m_client;           // 外部持有（不拥有）
        LlmBackend m_backend;                   // 后端选择
        std::unique_ptr<LLMAdapter> m_adapter;  // 工厂产物（独占）
        SimpleToolProvider m_default_tools;     // 自跑端口
        SimpleContextSource m_default_ctx;      // 自跑端口
        SimpleStateStore m_default_state;       // 自跑端口
    };

} // namespace AE::Engine
