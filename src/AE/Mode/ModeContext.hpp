#pragma once

/// @file Mode/ModeContext.hpp
/// @brief 模式执行上下文：承载注入依赖 + 单次 run 的请求态 / 运行态（Docs/01 §4.2.3）。
///
/// @details
/// 设计约束（关键）：
/// - **零拷贝、零虚调用**：以引用/裸指针引用已注入依赖；模式按 `ModeContext&` 读写，
///   无任何按次分配的间接层。
/// - 注入依赖由 AgentEngine 在构造期绑定；请求态/运行态由 AgentEngine 在每次 `run()`
///   起始经 `begin_run()` 复位，模式在 `execute()` 中消费。
/// - `messages` 保持既有 `nlohmann::json` 表示（不引入新 Message 类型），保证零回归。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>

#include "nlohmann-json.hpp"

#include "AEType.hpp"               // IToolProvider / IContextSource / IPermissionGate / IStateStore
#include "LLMAdapter.hpp"           // LLMAdapter
#include "Ports/ToolPort.hpp"       // ToolFailurePolicy
#include "Ports/HumanPort.hpp"      // IHumanGate
#include "Ports/MemoryPort.hpp"     // 记忆 Port

namespace AE::Engine
{

    class AETelemetry; // 遥测门面（前向声明；仅以 shared_ptr 持有）

    struct ModeContext
    {
        // ---------------- 注入依赖（构造期绑定，生命周期由宿主保证） ----------------
        IToolProvider &tools;
        IContextSource &ctx_source;
        LLMAdapter &llm;
        IPermissionGate *gate = nullptr;
        IStateStore *state = nullptr;
        std::shared_ptr<AETelemetry> telemetry;
        IHumanGate *human = nullptr;
        IWorkingMemory *working_memory = nullptr;
        ILongTermMemory *long_term_memory = nullptr;
        ISharedMemory *shared_memory = nullptr;
        ToolFailurePolicy *failures = nullptr;

        // ---------------- 请求态（每次 run 由 AgentEngine 复位） ----------------
        std::string session_id;
        nlohmann::json request_body = nlohmann::json::object();
        std::uint32_t max_iterations = 16;

        // ---------------- 运行态（模式读写） ----------------
        std::deque<nlohmann::json> messages;
        std::uint32_t iteration = 0;
        std::size_t tokens_used = 0;
        std::chrono::steady_clock::time_point deadline{};
        bool emits_contract = false;   // 是否已注入工具契约 system 消息
        std::uint32_t compression_count = 0;

        ModeContext(IToolProvider &t,
                    IContextSource &c,
                    LLMAdapter &l,
                    IPermissionGate *g = nullptr,
                    IStateStore *s = nullptr,
                    std::shared_ptr<AETelemetry> tele = nullptr) noexcept
            : tools(t), ctx_source(c), llm(l), gate(g), state(s), telemetry(std::move(tele)) {}

        // 复位单次 run 的请求态 / 运行态；保留注入依赖不变。
        void begin_run(std::string sid, const nlohmann::json &body, std::uint32_t max_iter)
        {
            session_id = std::move(sid);
            request_body = body;
            max_iterations = max_iter;
            messages.clear();
            iteration = 0;
            tokens_used = 0;
            emits_contract = false;
            compression_count = 0;
        }
    };

} // namespace AE::Engine
