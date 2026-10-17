#pragma once

/// @file Ports/ProcessExecutor.hpp
/// @brief 执行 Port 参考实现：进程级执行器 + 白名单沙箱（Docs/01 §7.3、§8.1 P7）。
///
/// @details
/// - `AllowListSandbox`：在执行前按语言白名单放行/拒绝，并可注入环境变量与工作目录。
/// - `LocalProcessExecutor`：以子进程执行并捕获标准输出（参考级；stderr 合并策略与
///   硬超时由宿主在后续实现加强，此处保持最小可用）。
/// - 平台：Windows 用 `_popen`/`_pclose`，POSIX 用 `popen`/`pclose`。
///
/// @warning 参考实现：命令构造与 shell 引用为基础处理，生产环境应结合宿主隔离策略
///          （容器/命名空间）与更严格的转义使用。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "Core/Result.hpp"
#include "Core/Error.hpp"

#include "Ports/ExecutorPort.hpp"

namespace AE::Engine
{

    // ---------------------------------------------------- 语言白名单沙箱
    class AllowListSandbox final : public ISandbox
    {
    public:
        std::vector<std::string> allowed_languages; // 空 = 不限制
        std::vector<std::string> inject_env;        // "KEY=VALUE"
        std::string force_cwd;

        auto wrap(ExecRequest req) -> Core::Result<ExecRequest> override
        {
            if (!allowed_languages.empty() &&
                std::find(allowed_languages.begin(), allowed_languages.end(), req.language) ==
                    allowed_languages.end())
                return Core::Result<ExecRequest>::Error(
                    Core::Error(Core::ErrorCode::PortDenied, "language not allowed: " + req.language));

            for (auto &e : inject_env)
                req.env.push_back(e);
            if (!force_cwd.empty())
                req.cwd = force_cwd;
            return Core::Result<ExecRequest>::Ok(std::move(req));
        }
    };

    // ---------------------------------------------------- 进程级执行器
    class LocalProcessExecutor final : public IExecutor
    {
    public:
        using CommandBuilder = std::function<std::vector<std::string>(const ExecRequest &)>;

        struct Options
        {
            std::vector<std::string> allowed_languages; // 空 = 不限制
        };

        explicit LocalProcessExecutor(Options opts = {}) : m_opts(std::move(opts))
        {
            m_builders["shell"] = [](const ExecRequest &r)
            { return std::vector<std::string>{r.code}; };
#ifdef _WIN32
            m_builders["cmd"] = [](const ExecRequest &r)
            { return std::vector<std::string>{r.code}; };
            m_builders["powershell"] = [](const ExecRequest &r)
            { return std::vector<std::string>{"powershell", "-NoProfile", "-Command", r.code}; };
            m_builders["python"] = [](const ExecRequest &r)
            { return std::vector<std::string>{"python", "-c", r.code}; };
#else
            m_builders["sh"] = [](const ExecRequest &r)
            { return std::vector<std::string>{"sh", "-c", r.code}; };
            m_builders["python"] = [](const ExecRequest &r)
            { return std::vector<std::string>{"python3", "-c", r.code}; };
#endif
            m_builders["node"] = [](const ExecRequest &r)
            { return std::vector<std::string>{"node", "-e", r.code}; };
            m_builders["js"] = m_builders["node"];
        }

        void set_builder(std::string language, CommandBuilder b)
        {
            m_builders[std::move(language)] = std::move(b);
        }

        auto execute(const ExecRequest &req) -> Core::Result<ExecResult> override
        {
            if (!m_opts.allowed_languages.empty() &&
                std::find(m_opts.allowed_languages.begin(), m_opts.allowed_languages.end(), req.language) ==
                    m_opts.allowed_languages.end())
                return Core::Result<ExecResult>::Error(
                    Core::Error(Core::ErrorCode::PortDenied, "language not allowed: " + req.language));

            std::vector<std::string> argv;
            if (auto it = m_builders.find(req.language); it != m_builders.end())
                argv = it->second(req);
            if (argv.empty())
                return Core::Result<ExecResult>::Error(
                    Core::Error(Core::ErrorCode::PortInvalidArg, "no command builder for language: " + req.language));
            for (const auto &a : req.args)
                argv.push_back(a);

            const std::string cmd = join_quoted(argv, req.env, req.cwd);

#ifdef _WIN32
            std::FILE *pipe = ::_popen(cmd.c_str(), "r");
#else
            std::FILE *pipe = ::popen(cmd.c_str(), "r");
#endif
            if (!pipe)
                return Core::Result<ExecResult>::Error(
                    Core::Error(Core::ErrorCode::PortInternal, "failed to spawn process"));

            ExecResult res;
            char buf[4096];
            while (std::fgets(buf, sizeof(buf), pipe) != nullptr)
                res.out += buf;

#ifdef _WIN32
            const int rc = ::_pclose(pipe);
#else
            const int rc = ::pclose(pipe);
#endif
            res.exit_code = normalize_exit(rc);
            res.timed_out = false; // 参考实现：不强制超时（由沙箱/宿主加强）
            return Core::Result<ExecResult>::Ok(std::move(res));
        }

    private:
        static auto normalize_exit(int rc) -> int
        {
#ifdef _WIN32
            return rc;
#else
            if (rc == -1)
                return -1;
            if (WIFEXITED(rc))
                return WEXITSTATUS(rc);
            return rc;
#endif
        }

        static void append_quoted(std::string &out, const std::string &s)
        {
            if (!out.empty())
                out.push_back(' ');
#ifdef _WIN32
            out.push_back('"');
            for (char c : s)
            {
                if (c == '"')
                    out += "\\\"";
                else
                    out.push_back(c);
            }
            out.push_back('"');
#else
            out.push_back('\'');
            for (char c : s)
            {
                if (c == '\'')
                    out += "'\\''";
                else
                    out.push_back(c);
            }
            out.push_back('\'');
#endif
        }

        static auto join_quoted(const std::vector<std::string> &argv,
                                const std::vector<std::string> &env,
                                const std::string &cwd) -> std::string
        {
            std::string out;
            if (!cwd.empty())
            {
#ifdef _WIN32
                out += "cd /d ";
#else
                out += "cd ";
#endif
                append_quoted(out, cwd);
                out += " && ";
            }
#ifndef _WIN32
            // POSIX：环境变量赋值前缀（K=V cmd ...）。
            for (const auto &e : env)
            {
                out += e;
                out.push_back(' ');
            }
#else
            // Windows：set "K=V" && cmd ...
            for (const auto &e : env)
            {
                out += "set ";
                append_quoted(out, e);
                out += " && ";
            }
#endif
            for (const auto &a : argv)
                append_quoted(out, a);
            return out;
        }

        Options m_opts;
        std::unordered_map<std::string, CommandBuilder> m_builders;
    };

} // namespace AE::Engine
