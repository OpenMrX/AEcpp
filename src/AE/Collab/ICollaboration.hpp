#pragma once

/// @file Collab/ICollaboration.hpp
/// @brief 协作抽象 + 协作上下文（Docs/01 §5.0.3、§5.0.4）。
///
/// @details
/// - `CollabContext` 承载注册表/消息总线/遥测/人机门 + 任务/预算（引用注入，零拷贝）。
/// - `ICollaboration` 为 6 模式的运行期边界抽象（每模式一次调用，非热路径）。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <cstdint>
#include <generator>
#include <string>
#include <string_view>

#include "nlohmann-json.hpp"

#include "AEType.hpp"          // AgentEvent
#include "Ports/HumanPort.hpp" // IHumanGate
#include "Collab/CollaborationEvent.hpp"

namespace AE::Engine
{

    class AgentRegistry;
    class MessageBus;
    class AETelemetry;

    struct CollabContext
    {
        std::string session_id;
        nlohmann::json task;
        std::uint32_t max_rounds{8};
        std::uint64_t token_budget{0}; // 0 = 不限
        AgentRegistry &registry;
        MessageBus &bus;
        AETelemetry *telemetry{nullptr};
        IHumanGate *human{nullptr};
    };

    class ICollaboration
    {
    public:
        virtual ~ICollaboration() = default;
        [[nodiscard]] virtual auto name() const -> std::string_view = 0;
        virtual auto run(CollabContext &ctx) -> std::generator<CollaborationEvent> = 0;
    };

} // namespace AE::Engine
