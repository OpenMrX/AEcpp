#pragma once

/// @file Mode/IReasoningMode.hpp
/// @brief 推理模式的编译期契约：`ReasoningMode` concept +（可选）插件动态接口（Docs/01 §4.2.2）。
///
/// @details
/// 设计约束（关键）：
/// - **优先编译期多态**：内置模式经 `std::variant` + `std::visit` 分发，零虚调用；
///   契约由 `ReasoningMode` concept 在编译期静态校验（不满足即编译错误）。
/// - `IReasoningMode` 仅为**外部插件**场景保留的动态接口，非核心路径，不参与内置模式分发。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <concepts>
#include <generator>
#include <string_view>

#include "AEType.hpp"        // AgentEvent
#include "Mode/ModeContext.hpp"
#include "Mode/ModeKind.hpp"

namespace AE::Engine
{

    // ---------------------------------------------------- 编译期契约
    // 满足者即具备：静态类型分类 + 名称 + 以 ModeContext& 驱动的事件生成器。
    template <class M>
    concept ReasoningMode = requires(M &m, ModeContext &ctx) {
        { M::kind() } -> std::convertible_to<ModeKind>;
        { m.name() } -> std::convertible_to<std::string_view>;
        { m.execute(ctx) } -> std::same_as<std::generator<AgentEvent>>;
    };

    // ---------------------------------------------------- 动态插件接口（可选）
    class IReasoningMode
    {
    public:
        virtual ~IReasoningMode() = default;
        [[nodiscard]] virtual auto kind() const noexcept -> ModeKind = 0;
        [[nodiscard]] virtual auto name() const noexcept -> std::string_view = 0;
        virtual auto execute(ModeContext &ctx) -> std::generator<AgentEvent> = 0;
    };

} // namespace AE::Engine
