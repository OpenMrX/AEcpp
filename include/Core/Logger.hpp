#pragma once

/// @file Core/Logger.hpp
/// @brief VXAE 自带日志门面 `Core::Logger`：纯 C++23 标准库实现（std::println /
///        std::format），无 spdlog/fmt 依赖。
/// @details 保持与原 vxcore Logger 相同的调用面（getInstance + 6 档 *Fmt + isEnabled），
///          AE 各文件调用点零改动。输出：控制台（可关）+ 可选日志文件。
/// @note 与 spdlog 版差异：
///  - 格式化使用 std::vformat（`{}` 位置参数语义与 fmt 一致）；
///  - 无 addCustomSink（spdlog 专有概念，去除）；
///  - initialize 可选打开文件 sink，未初始化时默认惰性输出到控制台（与原 ensureLoggingReady 语义一致）。

#include "CoreType.hpp"
#include "Result.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <print>
#include <string>
#include <string_view>
#include <utility>

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
