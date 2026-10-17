#pragma once

/// @file AEBuiltinTools.hpp
/// @brief 内置工具集：FileTools、GitTools、ProcessRunner。
///
/// @details
/// 从 React 迁移（FileTools/GitTools/ProcessRunner），适配 AE::IToolProvider 接口。
///
/// 使用方式：调用 AEBuiltinTools::registerAll(toolProvider) 一键注册全部内置工具。
/// 也可单独注册各工具类。
///
/// @note 所属命名空间：AE::Engine
/// @note 依赖：AEType.hpp（IToolProvider）、nlohmann::json、Core::Result。

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#include "Core/Result.hpp"
#include "AEType.hpp"

namespace AE::Engine
{

    // ================================================================
    //  ProcessResult — 子进程执行结果
    // ================================================================

    struct ProcessResult
    {
        std::string output;
        std::string error_output;
        int exit_code{-1};
        bool timed_out{false};
    };

    // ================================================================
    //  ProcessRunner — 跨平台子进程执行
    // ================================================================

    class ProcessRunner
    {
    public:
        [[nodiscard]] static auto run(std::string_view command,
            int timeout_ms = 30000,
            std::string_view cwd = "",
            const std::map<std::string, std::string>& env = {},
            std::size_t max_output_bytes = 1024 * 1024) -> ProcessResult;

        [[nodiscard]] static auto run_or_throw(std::string_view command,
            int timeout_ms = 30000, std::string_view cwd = "") -> Core::Result<std::string>;
    };

    // ================================================================
    //  FileTools — 文件读写编辑搜索（注册到 IToolProvider）
    // ================================================================

    class FileTools
    {
    public:
        // 将全部文件工具注册到指定 IToolProvider
        static void registerAll(IToolProvider& provider);

        [[nodiscard]] static auto fileRead(const nlohmann::json& args) -> Core::Result<nlohmann::json>;
        [[nodiscard]] static auto fileWrite(const nlohmann::json& args) -> Core::Result<nlohmann::json>;
        [[nodiscard]] static auto fileEdit(const nlohmann::json& args) -> Core::Result<nlohmann::json>;
        [[nodiscard]] static auto fileSearch(const nlohmann::json& args) -> Core::Result<nlohmann::json>;
    };

    // ================================================================
    //  GitTools — 只读 Git 工具集（注册到 IToolProvider）
    // ================================================================

    class GitTools
    {
    public:
        // 将全部 Git 工具注册到指定 IToolProvider
        static void registerAll(IToolProvider& provider);

        [[nodiscard]] static auto gitStatus(const nlohmann::json& args) -> Core::Result<nlohmann::json>;
        [[nodiscard]] static auto gitDiff(const nlohmann::json& args) -> Core::Result<nlohmann::json>;
        [[nodiscard]] static auto gitLog(const nlohmann::json& args) -> Core::Result<nlohmann::json>;
        [[nodiscard]] static auto gitShow(const nlohmann::json& args) -> Core::Result<nlohmann::json>;
    };

    // ================================================================
    //  AEBuiltinTools — 一键注册所有内置工具
    // ================================================================

    class AEBuiltinTools
    {
    public:
        // 将 FileTools + GitTools 全部注册到指定 IToolProvider
        static void registerAll(IToolProvider& provider);
    };

    // 工厂方法：创建独立的内置工具 IToolProvider（FileTools + GitTools）。
    // 可与 DceeToolProvider 组合使用，或独立用于轻量场景。
    [[nodiscard]] auto createBuiltinProvider() -> std::unique_ptr<IToolProvider>;

} // namespace AE::Engine
