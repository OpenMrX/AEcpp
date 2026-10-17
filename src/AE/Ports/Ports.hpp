#pragma once

/// @file Ports/Ports.hpp
/// @brief Ports 层聚合伞头：外部扩展统一抽象接口（Docs/01 §7）。
///
/// @details 聚合工具 / MCP / 记忆 / 执行 / 人机 / 模型 六类 Port。
///          仅依赖 `Core::Result` / `nlohmann::json` / 标准库，不耦合任何宿主实现。
///          另含 P7 参考实现（`McpHttpClient` / `InMemoryStore` / `LocalProcessExecutor` + `AllowListSandbox` / `ScriptedHumanGate`）。
/// @note 所属命名空间：AE::Engine（扁平）。

#include "Ports/ToolPort.hpp"
#include "Ports/McpTransport.hpp"
#include "Ports/McpHttpClient.hpp"
#include "Ports/MemoryPort.hpp"
#include "Ports/InMemoryStore.hpp"
#include "Ports/ExecutorPort.hpp"
#include "Ports/ProcessExecutor.hpp"
#include "Ports/HumanPort.hpp"
#include "Ports/ScriptedHumanGate.hpp"
#include "Ports/ModelPort.hpp"
