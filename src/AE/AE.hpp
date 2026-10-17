#pragma once

/// @file AE.hpp
/// @brief AE::Engine 聚合伞头（umbrella header）：一行 include 引入全部核心公共面。
///
/// @details
/// 结构性调整：本文件由「执行核心实现（AgentEngine）」改为「聚合入口」。各职责已拆分：
/// - 类型 / 接口层：AEType.hpp（IR + 依赖注入抽象 + AOP + 执行期基础设施）；
///   原独立接口头 IAE.hpp 已移除，其类型按序并入 AEType.hpp。
/// - 最小本地实现：AESimple.hpp（SimpleContextSource / SimpleToolProvider / SimpleStateStore）。
/// - LLM 客户端与多后端适配：StreamingLLMClient.hpp / LLMAdapter.hpp。
/// - 执行核心（ReAct 循环）：AgentEngine.hpp（具体类；抽象执行器 IAgentExecutor 已移除）。
/// - 无工具 provider：NullToolProvider.hpp（宿主未接工具时跑通链路）。
/// - 可观测性 / 录制回放 / 净化：AETelemetry.hpp / AERecordReplay.hpp / AESanitize.hpp。
/// - 工厂 / 管理器：AEManager.hpp（按后端装配适配器与引擎）。
///
/// 兼容性：既有仅 include "AE.hpp" 的调用方无需改动（本伞头已聚合上述头文件）。
///
/// @note 所属命名空间：AE::Engine

// 类型 / 接口层（IR + 抽象 + AOP + 基础设施）
#include "AEType.hpp"

// 外部扩展 Ports 层（工具/MCP/记忆/执行/人机/模型）
#include "Ports/Ports.hpp"

// 最小本地实现（Simple*）
#include "AESimple.hpp"

// LLM 客户端与适配层
#include "StreamingLLMClient.hpp"
#include "LLMAdapter.hpp"

// 无工具 provider（宿主未接工具时的兜底）
#include "NullToolProvider.hpp"

// 可观测性 / 录制回放 / 提示注入防护
#include "AETelemetry.hpp"
#include "AERecordReplay.hpp"
#include "AESanitize.hpp"

// 执行核心（ReAct 循环）
#include "AgentEngine.hpp"

// 工厂 / 管理器
#include "AEManager.hpp"
