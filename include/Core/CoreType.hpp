#pragma once

/// @file Core/CoreType.hpp
/// @brief VXAE 自带基础类型（独立基座，不依赖原项目 vxcore）。
/// @details 仅保留 AE 实际使用的 ErrorCode / LogLevel，
///          去除 nlohmann json 依赖与事件信封等无关类型。

#include <chrono>
#include <string>

namespace VMX::Core
{

    /// 统一错误码：贯穿 `Core::Result`/`Core::Error` 的错误分类。
    enum class ErrorCode
    {
        Unknown,
        NotFound,
        InvalidArgument,
        InternalError,
        NetworkError,
        DatabaseError,
        PluginLoadError,
        PluginInitError,
        PluginStartError,
        SerializationError,
        PermissionDenied,
        Timeout,
        Cancelled,
        InvalidState,
        ExecutionFailed,
        FeatureDisabled,
        NotConnected,
        NotInitialized,
        ExecutionError,
        VersionMismatch,
        InvalidOperation,
        InitializationFailed,
        Busy
    };

    /// 日志级别。
    enum class LogLevel
    {
        Trace,
        Debug,
        Info,
        Warning,
        Error,
        Critical
    };

} // namespace VMX::Core
