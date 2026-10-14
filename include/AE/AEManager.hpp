#pragma once

/// @file AEManager.hpp
/// @brief LLM 管理器聚合头文件：包含流式客户端与多后端适配层。
///
/// @details
/// 设计约束（关键）：
/// - 不 include Butler / MemoryStick；LlmDelta（见 AEType.hpp）为统一 IR。
/// - WASM 运行时选定 WasmEdge（由 DCEE 沙箱统一托管），本层不涉及沙箱。
/// - TLS 校验默认开启，API Key 用 AE::Core::Security::SecureString（不在内存裸存）。
///
/// @note 所属命名空间：AE::Engine
/// @note 依赖：libcurl, nlohmann, Core::Result/Logger。

#include "StreamingLLMClient.hpp"
#include "LLMAdapter.hpp"

namespace AE::Engine
{

} // namespace AE::Engine
