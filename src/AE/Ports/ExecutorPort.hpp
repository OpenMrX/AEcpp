#pragma once

/// @file Ports/ExecutorPort.hpp
/// @brief 执行 Port：外部执行 / 沙箱隔离抽象（Docs/01 §7.3）。
///
/// @details
/// 设计约束（关键）：
/// - 只依赖 `Core::Result` / 标准库。不 include 任何宿主实现（现仓 `ProcessRunner`
///   可作为进程级参考实现）。
/// - `ISandbox::wrap` 在真正执行前注入隔离参数（进程/容器/无），策略与执行分离。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <chrono>
#include <string>
#include <vector>

#include "Core/Result.hpp"

namespace AE::Engine
{

    // ---------------------------------------------------- 执行请求
    struct ExecRequest
    {
        std::string language;                 // "python" / "shell" / "js" ...
        std::string code;
        std::vector<std::string> args;
        std::chrono::milliseconds timeout{30'000};
        std::vector<std::string> env;         // "KEY=VALUE"
        std::string cwd;                      // 工作目录（可空）
    };

    // ---------------------------------------------------- 执行结果
    struct ExecResult
    {
        int exit_code = 0;
        std::string out;
        std::string err;
        bool timed_out = false;

        [[nodiscard]] auto ok() const noexcept -> bool { return exit_code == 0 && !timed_out; }
    };

    // ---------------------------------------------------- 执行器
    class IExecutor
    {
    public:
        virtual ~IExecutor() = default;
        virtual auto execute(const ExecRequest &req) -> Core::Result<ExecResult> = 0;
    };

    // ---------------------------------------------------- 沙箱隔离策略
    class ISandbox
    {
    public:
        virtual ~ISandbox() = default;
        // 注入隔离参数（如容器/命名空间/cgroup 包装），保持 ExecRequest 语义不变。
        virtual auto wrap(ExecRequest req) -> Core::Result<ExecRequest> = 0;
    };

    // 直通沙箱（无隔离）：满足「默认可用」与测试。
    class NullSandbox final : public ISandbox
    {
    public:
        auto wrap(ExecRequest req) -> Core::Result<ExecRequest> override
        {
            return Core::Result<ExecRequest>::Ok(std::move(req));
        }
    };

} // namespace AE::Engine
