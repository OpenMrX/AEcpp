#pragma once

/// @file Core/Error.hpp
/// @brief VXAE 自带统一错误类型 `Core::Error`：错误码 + 消息 + 可选原因链。
/// @details 与原项目接口一致，但为纯 header-only 内联实现（无 .cpp），
///          仅依赖标准库，无第三方依赖。

#include "CoreType.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace VMX::Core
{

    /// 统一错误：码 + 消息 + 可选 cause 链。值语义，可拷贝。
    class Error
    {
    public:
        Error() = default;
        explicit Error(std::string msg) : m_code(ErrorCode::Unknown), m_message(std::move(msg)) {}
        Error(ErrorCode code, std::string msg) : m_code(code), m_message(std::move(msg)) {}
        Error(ErrorCode code, std::string msg, std::shared_ptr<Error> cause)
            : m_code(code), m_message(std::move(msg)), m_cause(std::move(cause)) {}

        /// @return 错误码。
        [[nodiscard]] auto code() const -> ErrorCode { return m_code; }
        /// @return 错误消息（只读）。
        [[nodiscard]] auto message() const -> const std::string & { return m_message; }
        /// @return 错误消息（可写）。
        [[nodiscard]] auto message() -> std::string & { return m_message; }
        /// @return 原因链上一级错误（可能为空）。
        [[nodiscard]] auto cause() const -> const std::shared_ptr<Error> & { return m_cause; }
        /// @return 是否存在原因链。
        [[nodiscard]] auto hasCause() const -> bool { return m_cause != nullptr; }
        /// @return 本层错误格式化串（不含原因链）。
        [[nodiscard]] auto toString() const -> std::string
        {
            return std::string{errorCodeName(m_code)} + ": " + m_message;
        }
        /// @return 含完整原因链的格式化串（"code: msg -> cause: ..."）。
        [[nodiscard]] auto chainToString() const -> std::string
        {
            std::ostringstream oss;
            const Error *node = this;
            bool first = true;
            while (node)
            {
                if (!first)
                    oss << " -> ";
                oss << node->toString();
                first = false;
                node = node->m_cause.get();
            }
            return oss.str();
        }

    private:
        /// @return 错误码的可读名称（编译期常量表，无 magic_enum 依赖）。
        static constexpr auto errorCodeName(ErrorCode code) -> std::string_view
        {
            using enum ErrorCode;
            switch (code)
            {
            case Unknown: return "Unknown";
            case NotFound: return "NotFound";
            case InvalidArgument: return "InvalidArgument";
            case InternalError: return "InternalError";
            case NetworkError: return "NetworkError";
            case DatabaseError: return "DatabaseError";
            case PluginLoadError: return "PluginLoadError";
            case PluginInitError: return "PluginInitError";
            case PluginStartError: return "PluginStartError";
            case SerializationError: return "SerializationError";
            case PermissionDenied: return "PermissionDenied";
            case Timeout: return "Timeout";
            case Cancelled: return "Cancelled";
            case InvalidState: return "InvalidState";
            case ExecutionFailed: return "ExecutionFailed";
            case FeatureDisabled: return "FeatureDisabled";
            case NotConnected: return "NotConnected";
            case NotInitialized: return "NotInitialized";
            case ExecutionError: return "ExecutionError";
            case VersionMismatch: return "VersionMismatch";
            case InvalidOperation: return "InvalidOperation";
            case InitializationFailed: return "InitializationFailed";
            case Busy: return "Busy";
            }
            return "Unknown";
        }

        ErrorCode m_code = ErrorCode::Unknown; ///< 错误码
        std::string m_message;                 ///< 错误消息
        std::shared_ptr<Error> m_cause;        ///< 原因链（可选）
    };

} // namespace VMX::Core
