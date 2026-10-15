#pragma once

/// @file Core/Security/SecureString.hpp
/// @brief 密钥安全字符串（替代 cpr::util::SecureString）。
/// @details
/// 构造 / 拷贝 / 移动 / 析构时清零底层缓冲，避免明文密钥长期驻留内存。
/// 仅通过显式转换暴露明文，调用边界（如注入 Bearer / 构造请求体）才取出。
///
/// @note 头文件-only，无链接依赖。命名空间：AE::Core::Security。

#include <algorithm>
#include <string>
#include <string_view>

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
