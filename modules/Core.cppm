module;
#include <chrono>
#include <string>
#include <memory>
#include <sstream>
#include <utility>
#include <expected>
#include <type_traits>
#include <stdexcept>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <print>
#include <string_view>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <curl/curl.h>
#include <map>
export module Core;

/// @file Core/CoreType.hpp
/// @brief VXAE 自带基础类型（独立基座，不依赖原项目 vxcore）。
/// @details 仅保留 AE 实际使用的 ErrorCode / LogLevel，
///          去除 nlohmann json 依赖与事件信封等无关类型。


namespace AE::Core
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

} // namespace AE::Core

/// @file Core/Error.hpp
/// @brief VXAE 自带统一错误类型 `Core::Error`：错误码 + 消息 + 可选原因链。
/// @details 与原项目接口一致，但为纯 header-only 内联实现（无 .cpp），
///          仅依赖标准库，无第三方依赖。



namespace AE::Core
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

} // namespace AE::Core

/// @file Core/Result.hpp
/// @brief 项目统一错误传播载体 `Core::Result<T>`。
/// @details 生产代码禁止主动抛异常（H15），错误一律走 `Result` 返回路径。
///          `Result<T>` **继承** `std::expected<T, Core::Error>`，从而：
///          - 直接复用标准 monadic 能力（`transform`/`and_then`/`transform_error`/
///            `or_else`/`value_or`/`operator*`/`operator->`/`operator bool`/`==`/`swap`…）；
///          - `value()`/`error()` 在状态不匹配时抛 `std::bad_expected_access<Core::Error>`
///            （**异常内携带 `Error` 值**，可 `e.error()` 取回，等价 `std::expected` 契约），
///            由 AGENTS.md §3.3 明确接受；
///          - 同时满足 H7「不要重新发明 expected」——此处是**复用 + 扩展**，而非重造。
///          项目在基类之上扩展：强制 `Ok`/`Error` 工厂构造（屏蔽裸构造）、`isSuccess`/
///          `isError` 别名、跨类型错误透传构造、以及 camelCase monadic 别名
///          `map`(=transform) / `flatMap`(=and_then) / `mapError`(=transform_error) /
///          `orElse`(=or_else) / `valueOr`(=value_or)，外加 `MXDRO_RETURN_*_IF_ERROR` 早返回宏。
/// @note 命名风格：基类 `std::expected` 的标准方法（transform / and_then / transform_error /
///          or_else）全仓以 camelCase 别名（`map` / `flatMap` / `mapError` / `orElse`）暴露，
///          与既有 `map` / `flatMap` 调用点保持一致；两者语义等价，新代码优先用 camelCase 别名。
/// @note 移动操作显式 `noexcept`，保证错误传播热路径不发生隐式退化拷贝（§6.2）。
/// @note `std::expected` 无虚析构，但 `Result` 派生类**不持有任何需析构的拥有型成员**
///       （仅静态工厂 + 内联别名），故无虚析构在此实际安全。



namespace AE::Core
{
    // 类型别名：类 `Result` 内有一个同名的静态方法 `Error`，会遮蔽类型 `Error`。
    // 此处为类型引用提供无冲突别名，避免 `Error` 被方法名遮蔽导致编译失败。
    using ErrorType = Error;

    /// 类型安全的操作结果：要么成功持有值 `T`，要么失败持有 `Error`。
    /// 继承自 `std::expected<T, ErrorType>`，获得完整标准 monadic API。
    /// @tparam T 成功载荷类型；`Result<void>` 专用于无载荷的成功/失败语义（见特化）。
    template <typename T>
    class Result : public std::expected<T, ErrorType>
    {
        using Base = std::expected<T, ErrorType>;

    public:
        /// 跨类型转换构造：保持 `return otherResult;` 的早返回写法可用。
        /// 错误态直接透传 `Error`（不要求值类型可转换）；成功态要求 `U` 可转换为 `T`，
        /// 否则保持原 `Result` 的不变量（成功态不允许静默转为不同类型，抛异常）。
        template <typename U>
            requires (!std::is_same_v<U, T>)
        Result(const Result<U> &other)
        {
            if (other.isError())
            {
                *this = Result<T>::Error(ErrorType{other.error()});
                return;
            }
            if constexpr (std::is_convertible_v<const U &, T>)
            {
                *this = Result<T>::Ok(other.value());
            }
            else
            {
                throw std::runtime_error(
                    "Result: cannot convert success Result<U> to different value type Result<T>");
            }
        }

        /// 跨类型移动构造（语义同 const 版本）。
        template <typename U>
            requires (!std::is_same_v<U, T>)
        Result(Result<U> &&other)
        {
            if (other.isError())
            {
                *this = Result<T>::Error(ErrorType{std::move(other.error())});
                return;
            }
            if constexpr (std::is_convertible_v<U, T>)
            {
                *this = Result<T>::Ok(std::move(other.value()));
            }
            else
            {
                throw std::runtime_error(
                    "Result: cannot convert success Result<U> to different value type Result<T>");
            }
        }

        /// 复制 / 移动（保持 noexcept 契约，错误传播热路径零退化）。
        Result(Result &&) noexcept = default;
        Result &operator=(Result &&) noexcept = default;
        Result(const Result &) = default;
        Result &operator=(const Result &) = default;

        /// 暴露基类标准访问接口（异常内携带 `Error`，见类注释）。
        using Base::value;
        using Base::error;
        using Base::value_or;
        using Base::has_value;

        /// 成功态？等价于 `has_value()`。
        [[nodiscard]] auto isSuccess() const -> bool { return this->has_value(); }

        /// 错误态？等价于 `!has_value()`。
        [[nodiscard]] auto isError() const -> bool { return !this->has_value(); }

        /// 等价基类 `transform`：成功态变换载荷，错误态透传。
        template <typename Func>
        auto map(Func &&func) const -> Result<std::invoke_result_t<Func, T>>
        {
            using R = std::invoke_result_t<Func, T>;
            auto e = Base::transform(std::forward<Func>(func));
            if (e.has_value())
            {
                return Result<R>::Ok(*e);
            }
            return Result<R>::Error(ErrorType{e.error()});
        }

        /// 等价基类 `and_then`：展开嵌套 `Result`（避免 `Result<Result<R>>`）。
        template <typename Func>
        auto flatMap(Func &&func) const -> std::invoke_result_t<Func, T>
        {
            using R = std::invoke_result_t<Func, T>; // R == Result<X>
            auto e = Base::and_then(std::forward<Func>(func));
            if (e.has_value())
            {
                return R::Ok(*e);
            }
            return R::Error(ErrorType{e.error()});
        }

        /// 等价基类 `transform_error`：错误态重映射 `Error -> Error`，成功态透传。
        /// 因 `Result` 错误类型锁定为 `Core::Error`，`func` 必须返回 `Error`（可作重标注/包装）。
        /// 命名 `mapError` 沿用项目 camelCase 别名风格（对应标准 `transform_error`）。
        template <typename Func>
            requires (std::is_invocable_v<Func, const ErrorType &> &&
                      std::is_convertible_v<std::invoke_result_t<Func, const ErrorType &>, ErrorType>)
        auto mapError(Func &&func) const -> Result<T>
        {
            auto e = Base::transform_error(std::forward<Func>(func));
            if (e.has_value())
            {
                return Result<T>::Ok(*e);
            }
            return Result<T>::Error(ErrorType{e.error()});
        }

        /// 等价基类 `transform_error`（右值重载，可移动 `Error`）。
        template <typename Func>
            requires (std::is_invocable_v<Func, ErrorType &&> &&
                      std::is_convertible_v<std::invoke_result_t<Func, ErrorType &&>, ErrorType>)
        auto mapError(Func &&func) && -> Result<T>
        {
            auto e = std::move(*this).transform_error(std::forward<Func>(func));
            if (e.has_value())
            {
                return Result<T>::Ok(std::move(*e));
            }
            return Result<T>::Error(ErrorType{std::move(e.error())});
        }

        /// 等价基类 `or_else`：错误态恢复 / 兜底。`func` 返回 `Result<T>`（恢复为成功）
        /// 或 `Error`（替换为新错误）；成功态直接透传。值类型固定为 `T`（恢复不得改变值类型）。
        /// 命名 `orElse` 沿用项目 camelCase 别名风格（对应标准 `or_else`）。
        template <typename Func>
            requires (std::is_invocable_v<Func, const ErrorType &> &&
                      (std::is_convertible_v<std::invoke_result_t<Func, const ErrorType &>, Base> ||
                       std::is_convertible_v<std::invoke_result_t<Func, const ErrorType &>, ErrorType>))
        auto orElse(Func &&func) const -> Result<T>
        {
            auto e = Base::or_else(std::forward<Func>(func));
            if (e.has_value())
            {
                return Result<T>::Ok(*e);
            }
            return Result<T>::Error(ErrorType{e.error()});
        }

        /// 等价基类 `or_else`（右值重载，可移动 `Error`）。
        template <typename Func>
            requires (std::is_invocable_v<Func, ErrorType &&> &&
                      (std::is_convertible_v<std::invoke_result_t<Func, ErrorType &&>, Base> ||
                       std::is_convertible_v<std::invoke_result_t<Func, ErrorType &&>, ErrorType>))
        auto orElse(Func &&func) && -> Result<T>
        {
            auto e = std::move(*this).or_else(std::forward<Func>(func));
            if (e.has_value())
            {
                return Result<T>::Ok(std::move(*e));
            }
            return Result<T>::Error(ErrorType{std::move(e.error())});
        }

        /// 别名 `value_or`（默认兜底，不抛异常）。
        template <typename U>
        [[nodiscard]] auto valueOr(const U &defaultValue) const & -> T
        {
            return this->value_or(defaultValue);
        }

        /// 别名 `value_or`（右值重载）。
        template <typename U>
        [[nodiscard]] auto valueOr(U &&defaultValue) && -> T
        {
            return std::move(*this).value_or(std::forward<U>(defaultValue));
        }

        /// 构造成功结果。
        /// @param value 业务载荷，按值移入。
        static auto Ok(T value) -> Result<T> { return Result<T>{std::move(value)}; }

        /// 构造错误结果。
        /// @param error 错误详情，按值移入。
        static auto Error(ErrorType error) -> Result<T>
        {
            return Result<T>{std::unexpected{std::move(error)}};
        }

    private:
        /// 基类构造（含值构造 / unexpected 构造 / 标准转换构造）降为私有：
        /// 外部只能通过 `Ok` / `Error` 工厂构造，保持构造纪律。
        using Base::Base;
    };

    /// `Result` 针对无载荷场景的特化：仅表达成功/失败，不携带业务值。
    /// 继承标准 `std::expected<void, ErrorType>`，无需手搓 `bool + optional<Error>`。
    template <>
    class Result<void> : public std::expected<void, ErrorType>
    {
        using Base = std::expected<void, ErrorType>;

    public:
        /// 跨类型转换构造：任何 `Result<U>` 的错误态透传为本 `Result<void>` 错误；
        /// 成功态丢弃值转为 void 成功（void 无业务值可丢失）。
        template <typename U>
            requires (!std::is_same_v<U, void>)
        Result(const Result<U> &other)
        {
            if (other.isError())
            {
                *this = Result<void>::Error(ErrorType{other.error()});
                return;
            }
            *this = Result<void>::Ok();
        }

        /// 跨类型移动构造（语义同 const 版本）。
        template <typename U>
            requires (!std::is_same_v<U, void>)
        Result(Result<U> &&other)
        {
            if (other.isError())
            {
                *this = Result<void>::Error(ErrorType{std::move(other.error())});
                return;
            }
            *this = Result<void>::Ok();
        }

        /// 复制 / 移动（保持 noexcept 契约）。
        Result(Result &&) noexcept = default;
        Result &operator=(Result &&) noexcept = default;
        Result(const Result &) = default;
        Result &operator=(const Result &) = default;

        /// 暴露基类标准访问接口（void 无 `value()`，故仅暴露 `error`）。
        using Base::error;
        using Base::has_value;

        /// 成功态？
        [[nodiscard]] auto isSuccess() const -> bool { return this->has_value(); }

        /// 错误态？
        [[nodiscard]] auto isError() const -> bool { return !this->has_value(); }

        /// 等价基类 `transform_error`：错误态重映射 `Error -> Error`，成功态透传（void 无载荷）。
        /// `func` 必须返回 `Error`。命名 `mapError` 对应标准 `transform_error`。
        template <typename Func>
            requires (std::is_invocable_v<Func, const ErrorType &> &&
                      std::is_convertible_v<std::invoke_result_t<Func, const ErrorType &>, ErrorType>)
        auto mapError(Func &&func) const -> Result<void>
        {
            auto e = Base::transform_error(std::forward<Func>(func));
            if (e.has_value())
            {
                return Result<void>::Ok();
            }
            return Result<void>::Error(ErrorType{e.error()});
        }

        /// 等价基类 `transform_error`（右值重载）。
        template <typename Func>
            requires (std::is_invocable_v<Func, ErrorType &&> &&
                      std::is_convertible_v<std::invoke_result_t<Func, ErrorType &&>, ErrorType>)
        auto mapError(Func &&func) && -> Result<void>
        {
            auto e = std::move(*this).transform_error(std::forward<Func>(func));
            if (e.has_value())
            {
                return Result<void>::Ok();
            }
            return Result<void>::Error(ErrorType{std::move(e.error())});
        }

        /// 等价基类 `or_else`：错误态恢复 / 兜底（void 无载荷）。`func` 返回 `Result<void>`
        /// （恢复为成功）或 `Error`（替换为新错误）；成功态直接透传。
        /// 命名 `orElse` 对应标准 `or_else`。
        template <typename Func>
            requires (std::is_invocable_v<Func, const ErrorType &> &&
                      (std::is_convertible_v<std::invoke_result_t<Func, const ErrorType &>, Base> ||
                       std::is_convertible_v<std::invoke_result_t<Func, const ErrorType &>, ErrorType>))
        auto orElse(Func &&func) const -> Result<void>
        {
            auto e = Base::or_else(std::forward<Func>(func));
            if (e.has_value())
            {
                return Result<void>::Ok();
            }
            return Result<void>::Error(ErrorType{e.error()});
        }

        /// 等价基类 `or_else`（右值重载）。
        template <typename Func>
            requires (std::is_invocable_v<Func, ErrorType &&> &&
                      (std::is_convertible_v<std::invoke_result_t<Func, ErrorType &&>, Base> ||
                       std::is_convertible_v<std::invoke_result_t<Func, ErrorType &&>, ErrorType>))
        auto orElse(Func &&func) && -> Result<void>
        {
            auto e = std::move(*this).or_else(std::forward<Func>(func));
            if (e.has_value())
            {
                return Result<void>::Ok();
            }
            return Result<void>::Error(ErrorType{std::move(e.error())});
        }

        /// 构造成功结果（无载荷）。
        static auto Ok() -> Result<void> { return Result<void>{}; }

        /// 构造错误结果。
        /// @param error 错误详情，按值移入。
        static auto Error(ErrorType error) -> Result<void>
        {
            return Result<void>{std::unexpected{std::move(error)}};
        }

    private:
        /// 基类构造降为私有，外部只能通过 `Ok` / `Error` 构造。
        using Base::Base;
    };

} // namespace AE::Core

#define MXDRO_RETURN_IF_ERROR(result)                                     \
    do                                                                    \
    {                                                                     \
        auto _mxdro_result = (result);                                    \
        if (_mxdro_result.isError())                                      \
        {                                                                 \
            return AE::Core::Result<void>::Error(_mxdro_result.error()); \
        }                                                                 \
    } while (0)

#define MXDRO_RETURN_VALUE_IF_ERROR(result)                                                                        \
    do                                                                                                             \
    {                                                                                                              \
        auto _mxdro_result = (result);                                                                             \
        if (_mxdro_result.isError())                                                                               \
        {                                                                                                          \
            return AE::Core::Result<std::decay_t<decltype(_mxdro_result.value())>>::Error(_mxdro_result.error()); \
        }                                                                                                          \
    } while (0)

/// @file Core/Logger.hpp
/// @brief VXAE 自带日志门面 `Core::Logger`：纯 C++23 标准库实现（std::println /
///        std::format），无 spdlog/fmt 依赖。
/// @details 保持与原 vxcore Logger 相同的调用面（getInstance + 6 档 *Fmt + isEnabled），
///          AE 各文件调用点零改动。输出：控制台（可关）+ 可选日志文件。
/// @note 与 spdlog 版差异：
///  - 格式化使用 std::vformat（`{}` 位置参数语义与 fmt 一致）；
///  - 无 addCustomSink（spdlog 专有概念，去除）；
///  - initialize 可选打开文件 sink，未初始化时默认惰性输出到控制台（与原 ensureLoggingReady 语义一致）。



namespace AE::Core
{

    /// 全局日志门面（线程安全，标准库后端）。
    class Logger
    {
    public:
        /// @return 进程内单例引用。
        static auto getInstance() -> Logger &
        {
            static Logger s_instance;
            return s_instance;
        }

        /// @brief 初始化日志系统（控制台 + 可选文件 sink）。
        /// @param logFilePath 日志文件路径（空串=不写文件）
        /// @param level 日志级别
        /// @param enableConsole 是否启用控制台输出；TUI 等占用终端的程序应传 false
        auto initialize(const std::string &logFilePath = "VXAE.log",
                        LogLevel level = LogLevel::Info,
                        bool enableConsole = true) -> Result<void>
        {
            std::lock_guard lock{m_mutex};
            m_level = level;
            m_enableConsole = enableConsole;
            m_filePath = logFilePath;
            if (!logFilePath.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(logFilePath).parent_path(), ec);
                m_file.open(logFilePath, std::ios::app);
                if (!m_file.is_open())
                {
                    return Result<void>::Error(Error{ErrorCode::InternalError, "cannot open log file: " + logFilePath});
                }
            }
            m_initialized = true;
            return Result<void>::Ok();
        }

        auto shutdown() -> void
        {
            std::lock_guard lock{m_mutex};
            if (m_file.is_open())
            {
                m_file.flush();
                m_file.close();
            }
            m_initialized = false;
        }

        auto log(LogLevel level, std::string_view message) -> void
        {
            emit(level, message);
        }
        auto trace(std::string_view message) -> void
        {
            emit(LogLevel::Trace, message);
        }
        auto debug(std::string_view message) -> void
        {
            emit(LogLevel::Debug, message);
        }
        auto info(std::string_view message) -> void
        {
            emit(LogLevel::Info, message);
        }
        auto warning(std::string_view message) -> void
        {
            emit(LogLevel::Warning, message);
        }
        auto error(std::string_view message) -> void
        {
            emit(LogLevel::Error, message);
        }
        auto critical(std::string_view message) -> void
        {
            emit(LogLevel::Critical, message);
        }

        auto setLogLevel(LogLevel level) -> void
        {
            std::lock_guard lock{m_mutex};
            m_level = level;
        }

        auto setModuleTag(std::string_view tag) -> void
        {
            std::lock_guard lock{m_mutex};
            m_moduleTag = tag;
        }

        /// @brief 查询指定级别当前是否启用（供调用点门控昂贵实参的求值）。
        [[nodiscard]] auto isEnabled(LogLevel level) const noexcept -> bool
        {
            return static_cast<int>(level) >= static_cast<int>(m_level);
        }

        template <typename... Args>
        auto infoFmt(std::string_view fmt, Args &&...args) -> void
        {
            emit(LogLevel::Info, vformatOrRaw(fmt, std::forward<Args>(args)...));
        }

        template <typename... Args>
        auto errorFmt(std::string_view fmt, Args &&...args) -> void
        {
            emit(LogLevel::Error, vformatOrRaw(fmt, std::forward<Args>(args)...));
        }

        template <typename... Args>
        auto debugFmt(std::string_view fmt, Args &&...args) -> void
        {
            emit(LogLevel::Debug, vformatOrRaw(fmt, std::forward<Args>(args)...));
        }

        template <typename... Args>
        auto warningFmt(std::string_view fmt, Args &&...args) -> void
        {
            emit(LogLevel::Warning, vformatOrRaw(fmt, std::forward<Args>(args)...));
        }

        template <typename... Args>
        auto traceFmt(std::string_view fmt, Args &&...args) -> void
        {
            emit(LogLevel::Trace, vformatOrRaw(fmt, std::forward<Args>(args)...));
        }

        template <typename... Args>
        auto criticalFmt(std::string_view fmt, Args &&...args) -> void
        {
            emit(LogLevel::Critical, vformatOrRaw(fmt, std::forward<Args>(args)...));
        }

    private:
        Logger() = default;
        ~Logger() = default;

        Logger(const Logger &) = delete;
        auto operator=(const Logger &) -> Logger & = delete;

        LogLevel m_level = LogLevel::Info;
        std::string m_moduleTag;
        std::string m_filePath;
        std::ofstream m_file;
        bool m_enableConsole = true;
        bool m_initialized = false;
        mutable std::mutex m_mutex;

        /// @brief 格式化实参；格式串非法（如运行期拼接的花括号不配对）时回退原串，绝不抛出。
        /// @note 实参一律以具名左值传入 make_format_args（其签名只接受左值引用；
        ///       右值实参已在形参 Args&& 处具化，无需也不可再 forward）。
        template <typename... Args>
        static auto vformatOrRaw(std::string_view fmt, Args &&...args) -> std::string
        {
            try
            {
                return std::vformat(fmt, std::make_format_args(args...));
            }
            catch (const std::format_error &)
            {
                return std::string{fmt};
            }
        }

        static constexpr auto levelName(LogLevel level) -> std::string_view
        {
            switch (level)
            {
                case LogLevel::Trace:
                    return "TRACE";
                case LogLevel::Debug:
                    return "DEBUG";
                case LogLevel::Info:
                    return "INFO";
                case LogLevel::Warning:
                    return "WARN";
                case LogLevel::Error:
                    return "ERROR";
                case LogLevel::Critical:
                    return "CRIT";
            }
            return "INFO";
        }

        /// @brief 输出一条日志：级别短路 + 时间戳/级别/模块标签前缀，控制台与文件双 sink。
        auto emit(LogLevel level, std::string_view message) -> void
        {
            if (static_cast<int>(level) < static_cast<int>(m_level))
            {
                return;
            }
            const auto stamp = std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::system_clock::now());
            std::string line;
            line.reserve(stamp.size() + message.size() + m_moduleTag.size() + 16);
            line += '[';
            line += stamp;
            line += "] [";
            line += levelName(level);
            line += ']';
            if (!m_moduleTag.empty())
            {
                line += " [";
                line += m_moduleTag;
                line += ']';
            }
            line += ' ';
            line += message;

            std::lock_guard lock{m_mutex};
            if (m_enableConsole)
            {
                if (level >= LogLevel::Error)
                {
                    std::print(stderr, "{}\n", line);
                }
                else
                {
                    std::print(stdout, "{}\n", line);
                }
            }
            if (m_file.is_open())
            {
                m_file << line << '\n';
                m_file.flush();
            }
        }
    };

}  // namespace AE::Core

/// @file Core/Security/SecureString.hpp
/// @brief 密钥安全字符串（替代 cpr::util::SecureString）。
/// @details
/// 构造 / 拷贝 / 移动 / 析构时清零底层缓冲，避免明文密钥长期驻留内存。
/// 仅通过显式转换暴露明文，调用边界（如注入 Bearer / 构造请求体）才取出。
///
/// @note 头文件-only，无链接依赖。命名空间：AE::Core::Security。


namespace AE::Core::Security
{

    class SecureString
    {
    public:
        SecureString() = default;

        explicit SecureString(std::string data) : m_data(std::move(data)) {}

        explicit SecureString(std::string_view sv) : m_data(sv) {}

        ~SecureString() { clear(); }

        SecureString(const SecureString &other) : m_data(other.m_data) {}

        SecureString(SecureString &&other) noexcept : m_data(std::move(other.m_data))
        {
            other.clear();
        }

        auto operator=(const SecureString &other) -> SecureString &
        {
            if (this != &other)
            {
                clear();
                m_data = other.m_data;
            }
            return *this;
        }

        auto operator=(SecureString &&other) noexcept -> SecureString &
        {
            if (this != &other)
            {
                clear();
                m_data = std::move(other.m_data);
                other.clear();
            }
            return *this;
        }

        auto operator=(const std::string &s) -> SecureString &
        {
            clear();
            m_data = s;
            return *this;
        }

        auto operator=(std::string &&s) noexcept -> SecureString &
        {
            clear();
            m_data = std::move(s);
            return *this;
        }

        [[nodiscard]] auto empty() const noexcept -> bool { return m_data.empty(); }

        [[nodiscard]] auto size() const noexcept -> std::size_t { return m_data.size(); }

        /// 仅在调用边界由 SecureString 构造 std::string（密钥短暂离开保护范围）。
        explicit operator std::string() const { return m_data; }

        [[nodiscard]] auto str() const -> std::string { return m_data; }

        [[nodiscard]] auto view() const noexcept -> std::string_view { return m_data; }

    private:
        auto clear() -> void
        {
            if (!m_data.empty())
                std::fill(m_data.begin(), m_data.end(), static_cast<char>(0));
            m_data.clear();
        }

        std::string m_data;
    };

} // namespace AE::Core::Security

/// @file Core/Net/SseParser.hpp
/// @brief 手写 SSE（Server-Sent Events）增量解析器（替代 cpr::ServerSentEventParser）。
/// @details
/// 以字节流方式喂入原始 HTTP 响应块，按 SSE 规范（空行分隔事件、'data:'/'event:'/'id:' 字段、
/// ':' 开头为注释）切分，逐事件回调。兼容被 chunk 边界切断的不完整行/事件。
/// 语义对齐 cpr::ServerSentEventParser：每个事件回调给出 (event, data)。
///
/// @note 头文件-only，无链接依赖。命名空间：AE::Core::Net。


namespace AE::Core::Net
{

    /// SSE 增量解析器：feed() 多次调用，逐完整事件触发 cb(event, data)。
    /// cb 返回 false 表示中止（由上层传播为“停止流式传输”）。
    class SseParser
    {
    public:
        using EventCallback = std::function<bool(std::string_view event, std::string_view data)>;

        auto feed(std::string_view chunk, EventCallback cb) -> bool
        {
            m_buf.append(chunk.data(), chunk.size());

            std::size_t start = 0;
            while (true)
            {
                const auto nl = m_buf.find('\n', start);
                if (nl == std::string::npos)
                {
                    // 保留未完成的行（无换行符的尾部）
                    if (start > 0)
                        m_buf.erase(0, start);
                    break;
                }
                std::string_view line{m_buf.data() + start, nl - start};
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);

                if (!processLine(line, cb))
                    return false;

                start = nl + 1;
            }
            return true;
        }

    private:
        auto processLine(std::string_view line, EventCallback &cb) -> bool
        {
            // 空行 = 事件分隔符，触发派发
            if (line.empty())
                return dispatch(cb);

            // 注释行（以 ':' 开头）忽略
            if (line.front() == ':')
                return true;

            std::string_view field;
            std::string_view value;
            const auto colon = line.find(':');
            if (colon == std::string_view::npos)
            {
                field = line;
            }
            else
            {
                field = line.substr(0, colon);
                value = line.substr(colon + 1);
                if (!value.empty() && value.front() == ' ')
                    value.remove_prefix(1);
            }

            if (field == "data")
            {
                m_data += value;
                m_data.push_back('\n'); // 多 data 行以 \n 拼接（对齐 cpr 行为）
            }
            else if (field == "event")
            {
                m_event = value;
            }
            else if (field == "id")
            {
                m_id = value;
            }
            // 其余字段（retry 等）忽略
            return true;
        }

        auto dispatch(EventCallback &cb) -> bool
        {
            // 既无 data 也无 event 的空事件，跳过
            if (m_data.empty() && m_event.empty())
            {
                reset();
                return true;
            }
            if (!m_data.empty() && m_data.back() == '\n')
                m_data.pop_back();

            const bool cont = cb(m_event, m_data);
            reset();
            return cont;
        }

        auto reset() -> void
        {
            m_data.clear();
            m_event.clear();
            m_id.clear();
        }

        std::string m_buf;  // 跨 feed 调用的行缓冲
        std::string m_data; // 当前事件累积的 data
        std::string m_event;
        std::string m_id;
    };

} // namespace AE::Core::Net

/// @file Core/Net/HttpCurl.hpp
/// @brief 基于 libcurl 的同步 HTTP 客户端工具（替代 cpr）。
/// @details
/// 提供阻塞式请求 curlPerform 与流式请求 curlStream（逐原始块回调 onData）。
/// 头文件-only，内联实现；使用方链接目标需依赖 CURL::libcurl。
/// libcurl 为 VXAE 当前唯一 vcpkg 依赖（去留待定，替代候选见 plan.md §8.2），
/// 由本文件统一承载出向 HTTP 客户端；替换传输层时仅重写本文件、接口签名不变。
///
/// @note 命名空间：AE::Core::Net。




namespace AE::Core::Net
{

    /// 出向 HTTP 请求描述（方法 / URL / 头 / 体 / 超时 / TLS 校验）。
    struct CurlRequest
    {
        std::string method = "GET";
        std::string url;
        std::map<std::string, std::string> headers;
        std::string body;
        long timeoutMs = 30000;
        long connectTimeoutMs = 10000;
        bool verifySsl = true;
    };

    /// 完整响应（状态码 / 响应体 / 响应头）。
    struct CurlResponse
    {
        long statusCode = 0;
        std::string body;
        std::map<std::string, std::string> headers;
    };

    namespace detail
    {

        /// 线程安全的 libcurl 全局初始化（单次）。
        inline void curlEnsureGlobalInit()
        {
            static std::once_flag flag;
            std::call_once(flag, [] { ::curl_global_init(CURL_GLOBAL_ALL); });
        }

        /// RAII 封装 CURL* 与 curl_slist*，析构统一清理。
        struct CurlSession
        {
            CURL *easy = ::curl_easy_init();
            curl_slist *list = nullptr;

            CurlSession() = default;
            CurlSession(const CurlSession &) = delete;
            auto operator=(const CurlSession &) -> CurlSession & = delete;
            CurlSession(CurlSession &&) = delete;
            auto operator=(CurlSession &&) -> CurlSession & = delete;

            ~CurlSession()
            {
                if (list != nullptr)
                    ::curl_slist_free_all(list);
                if (easy != nullptr)
                    ::curl_easy_cleanup(easy);
            }
        };

        /// 累积写回调：把响应体追加到 std::string。
        inline auto accumulateWrite(char *ptr, std::size_t sz, std::size_t nmemb, void *ud) -> std::size_t
        {
            auto *buf = static_cast<std::string *>(ud);
            const std::size_t total = sz * nmemb;
            buf->append(ptr, total);
            return total;
        }

        /// 响应头写回调：跳过状态行，按原始大小写写入 map。
        inline auto headerWrite(char *ptr, std::size_t sz, std::size_t nmemb, void *ud) -> std::size_t
        {
            auto *out = static_cast<std::map<std::string, std::string> *>(ud);
            const std::size_t total = sz * nmemb;
            std::string_view line(ptr, total);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.remove_suffix(1);
            if (line.empty())
                return total;
            // 跳过状态行（"HTTP/1.1 200 OK"）
            if (line.size() >= 5 && line.substr(0, 5) == "HTTP/")
                return total;
            const auto colon = line.find(':');
            if (colon == std::string_view::npos)
                return total;
            std::string_view key = line.substr(0, colon);
            std::string_view val = line.substr(colon + 1);
            if (!val.empty() && val.front() == ' ')
                val.remove_prefix(1);
            (*out)[std::string(key)] = std::string(val);
            return total;
        }

        /// 应用通用选项（URL / 方法 / 体 / 头 / 超时 / TLS）。
        inline void applyCommon(CurlSession &s, const CurlRequest &req)
        {
            ::curl_easy_setopt(s.easy, CURLOPT_URL, req.url.c_str());
            ::curl_easy_setopt(s.easy, CURLOPT_CUSTOMREQUEST, req.method.c_str());
            if (!req.body.empty())
            {
                ::curl_easy_setopt(s.easy, CURLOPT_POSTFIELDS, req.body.data());
                ::curl_easy_setopt(s.easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
            }
            for (const auto &[k, v] : req.headers)
                s.list = ::curl_slist_append(s.list, (k + ": " + v).c_str());
            if (s.list != nullptr)
                ::curl_easy_setopt(s.easy, CURLOPT_HTTPHEADER, s.list);

            ::curl_easy_setopt(s.easy, CURLOPT_TIMEOUT_MS, req.timeoutMs);
            ::curl_easy_setopt(s.easy, CURLOPT_CONNECTTIMEOUT_MS, req.connectTimeoutMs);
            ::curl_easy_setopt(s.easy, CURLOPT_SSL_VERIFYPEER, req.verifySsl ? 1L : 0L);
            ::curl_easy_setopt(s.easy, CURLOPT_SSL_VERIFYHOST, req.verifySsl ? 2L : 0L);
        }

    } // namespace detail

    /// 阻塞式执行请求，返回完整响应体。
    inline auto curlPerform(const CurlRequest &req) -> Core::Result<CurlResponse>
    {
        detail::curlEnsureGlobalInit();
        detail::CurlSession s;
        if (s.easy == nullptr)
            return Core::Result<CurlResponse>::Error(
                Core::Error(Core::ErrorCode::NetworkError, "curl_easy_init failed"));

        CurlResponse out;
        detail::applyCommon(s, req);
        ::curl_easy_setopt(s.easy, CURLOPT_WRITEFUNCTION, detail::accumulateWrite);
        ::curl_easy_setopt(s.easy, CURLOPT_WRITEDATA, &out.body);
        ::curl_easy_setopt(s.easy, CURLOPT_HEADERFUNCTION, detail::headerWrite);
        ::curl_easy_setopt(s.easy, CURLOPT_HEADERDATA, &out.headers);

        const CURLcode code = ::curl_easy_perform(s.easy);
        if (code != CURLE_OK)
        {
            return Core::Result<CurlResponse>::Error(
                Core::Error(Core::ErrorCode::NetworkError,
                            std::string("curl_easy_perform failed: ") + ::curl_easy_strerror(code)));
        }
        long status = 0;
        ::curl_easy_getinfo(s.easy, CURLINFO_RESPONSE_CODE, &status);
        out.statusCode = status;
        return Core::Result<CurlResponse>::Ok(out);
    }

    /// 流式执行：逐原始块回调 onData；onData 返回 false 即中止传输（视为正常终止）。
    inline auto curlStream(const CurlRequest &req,
                           const std::function<bool(std::string_view)> &onData)
        -> Core::Result<CurlResponse>
    {
        detail::curlEnsureGlobalInit();
        detail::CurlSession s;
        if (s.easy == nullptr)
            return Core::Result<CurlResponse>::Error(
                Core::Error(Core::ErrorCode::NetworkError, "curl_easy_init failed"));

        CurlResponse out;
        struct Ctx
        {
            const std::function<bool(std::string_view)> *cb = nullptr;
            bool aborted = false;
        } ctx;
        ctx.cb = &onData;

        auto streamWrite = [](char *ptr, std::size_t sz, std::size_t nmemb, void *ud) -> std::size_t
        {
            auto *c = static_cast<Ctx *>(ud);
            const std::size_t total = sz * nmemb;
            if (c->aborted)
                return 0;
            if (c->cb != nullptr && !(*c->cb)(std::string_view(ptr, total)))
            {
                c->aborted = true;
                return 0;
            }
            return total;
        };

        detail::applyCommon(s, req);
        ::curl_easy_setopt(s.easy, CURLOPT_WRITEFUNCTION, streamWrite);
        ::curl_easy_setopt(s.easy, CURLOPT_WRITEDATA, &ctx);
        ::curl_easy_setopt(s.easy, CURLOPT_HEADERFUNCTION, detail::headerWrite);
        ::curl_easy_setopt(s.easy, CURLOPT_HEADERDATA, &out.headers);

        const CURLcode code = ::curl_easy_perform(s.easy);
        long status = 0;
        ::curl_easy_getinfo(s.easy, CURLINFO_RESPONSE_CODE, &status);
        out.statusCode = status;

        // 消费方主动中止（write 回调返回 false → libcurl 报 CURLE_WRITE_ERROR）视为正常终止
        if (code != CURLE_OK && code != CURLE_WRITE_ERROR)
        {
            return Core::Result<CurlResponse>::Error(
                Core::Error(Core::ErrorCode::NetworkError,
                            std::string("curl_easy_perform failed: ") + ::curl_easy_strerror(code)));
        }
        return Core::Result<CurlResponse>::Ok(out);
    }

    /// 百分号编码（用于 URL 查询参数）。封装 curl_easy_escape。
    inline auto curlUrlEncode(std::string_view in) -> std::string
    {
        detail::curlEnsureGlobalInit();
        detail::CurlSession s;
        if (s.easy == nullptr)
            return std::string(in);
        char *enc = ::curl_easy_escape(s.easy, in.data(), static_cast<int>(in.size()));
        if (enc == nullptr)
            return std::string(in);
        std::string out(enc);
        ::curl_free(enc);
        return out;
    }

} // namespace AE::Core::Net
