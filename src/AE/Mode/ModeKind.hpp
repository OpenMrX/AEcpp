#pragma once

/// @file Mode/ModeKind.hpp
/// @brief 推理模式分类（Docs/01 §4.2.2）。
/// @note 所属命名空间：AE::Engine（扁平）。

#include <cstdint>

namespace AE::Engine
{

    enum class ModeKind : std::uint8_t
    {
        React,       // 单循环 ReAct（默认）
        PlanExecute, // 先规划后执行（§4.3）
        Reflection   // 反思纠错装饰（§4.4）
    };

} // namespace AE::Engine
