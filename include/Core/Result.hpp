#pragma once

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

#include "Error.hpp"

#include <expected>
#include <type_traits>
#include <utility>
#include <stdexcept>

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
