// =============================================================================
//  AE::Engine — AEBuiltinTools.cpp
//  内置工具集实现：FileTools、GitTools、ProcessRunner。
//  从 React 迁移，适配 AE::IToolProvider 接口。
// =============================================================================

#include "AEBuiltinTools.hpp"

#include <array>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace AE::Engine
{

    // ================================================================
    //  ProcessRunner
    // ================================================================

    auto ProcessRunner::run(std::string_view command, int timeout_ms,
        std::string_view /*cwd*/, const std::map<std::string, std::string>& /*env*/,
        std::size_t max_output_bytes) -> ProcessResult
    {
        ProcessResult result;
        if (command.empty())
        {
            result.exit_code = -1;
            result.error_output = "Empty command";
            return result;
        }

        std::string cmd = std::string{command} + " 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe)
        {
            result.exit_code = -1;
            result.error_output = "popen failed";
            return result;
        }

        std::array<char, 4096> buf{};
        std::size_t total = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{timeout_ms};

        while (std::chrono::steady_clock::now() < deadline && total < max_output_bytes)
        {
            std::size_t remaining = max_output_bytes - total;
            std::size_t to_read = std::min<std::size_t>(buf.size(), remaining);
            auto n = std::fread(buf.data(), 1, to_read, pipe);
            if (n == 0) break;
            result.output.append(buf.data(), n);
            total += n;
        }

        int rc = pclose(pipe);
        result.exit_code = rc;
        if (total >= max_output_bytes)
            result.output += "\n[output truncated]";
        return result;
    }

    auto ProcessRunner::run_or_throw(std::string_view command, int timeout_ms,
        std::string_view cwd) -> Core::Result<std::string>
    {
        auto r = run(command, timeout_ms, cwd);
        if (r.exit_code != 0)
            return Core::Result<std::string>::Error(
                Core::Error{Core::ErrorCode::ExecutionFailed, "Process failed: " + r.error_output});
        return Core::Result<std::string>::Ok(r.output);
    }

    // ================================================================
    //  FileTools
    // ================================================================

    namespace
    {
        static constexpr std::size_t k_max_read_bytes = 32 * 1024;
    }

    auto FileTools::fileRead(const nlohmann::json& args) -> Core::Result<nlohmann::json>
    {
        auto path = args.value("path", "");
        if (path.empty())
            return Core::Result<nlohmann::json>::Error(Core::Error{"file_read: path required"});

        std::ifstream f{path};
        if (!f.is_open())
            return Core::Result<nlohmann::json>::Error(Core::Error{"file_read: cannot open: " + path});

        std::string content;
        std::string line;
        int line_num = 0;
        while (std::getline(f, line) && content.size() < k_max_read_bytes)
        {
            ++line_num;
            content += line + "\n";
        }
        nlohmann::json result{{"content", content}, {"lines", line_num}, {"path", path}};
        return Core::Result<nlohmann::json>::Ok(std::move(result));
    }

    auto FileTools::fileWrite(const nlohmann::json& args) -> Core::Result<nlohmann::json>
    {
        auto path = args.value("path", "");
        if (path.empty())
            return Core::Result<nlohmann::json>::Error(Core::Error{"file_write: path required"});

        std::string content;
        if (args.contains("content") && args["content"].is_string())
            content = args["content"].get<std::string>();
        else
            return Core::Result<nlohmann::json>::Error(Core::Error{"file_write: content required"});

        std::filesystem::create_directories(std::filesystem::path{path}.parent_path());
        std::ofstream f{path, std::ios::binary};
        if (!f.is_open())
            return Core::Result<nlohmann::json>::Error(Core::Error{"file_write: cannot open: " + path});

        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        nlohmann::json result{{"success", true}, {"path", path}, {"bytes_written", content.size()}};
        return Core::Result<nlohmann::json>::Ok(std::move(result));
    }

    auto FileTools::fileEdit(const nlohmann::json& args) -> Core::Result<nlohmann::json>
    {
        auto path = args.value("path", "");
        auto old_str = args.value("old_string", "");
        auto new_str = args.value("new_string", "");
        if (path.empty())
            return Core::Result<nlohmann::json>::Error(Core::Error{"file_edit: path required"});

        std::ifstream fin{path, std::ios::binary};
        if (!fin.is_open())
            return Core::Result<nlohmann::json>::Error(Core::Error{"file_edit: cannot open: " + path});

        std::string content{std::istreambuf_iterator<char>{fin}, std::istreambuf_iterator<char>{}};
        fin.close();

        auto pos = content.find(old_str);
        if (pos == std::string::npos)
            return Core::Result<nlohmann::json>::Error(Core::Error{"file_edit: old_string not found"});

        content.replace(pos, old_str.size(), new_str);
        std::ofstream fout{path, std::ios::binary | std::ios::trunc};
        fout.write(content.data(), static_cast<std::streamsize>(content.size()));

        nlohmann::json result{{"success", true}, {"path", path}};
        return Core::Result<nlohmann::json>::Ok(std::move(result));
    }

    auto FileTools::fileSearch(const nlohmann::json& args) -> Core::Result<nlohmann::json>
    {
        auto pattern = args.value("pattern", "*");
        auto search_path = args.value("path", ".");
        auto max_results = args.value("max_results", 50);

        auto matches = nlohmann::json::array();
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator{search_path, ec})
        {
            if (static_cast<int>(matches.size()) >= max_results) break;
            if (entry.is_regular_file())
                matches.push_back(entry.path().string());
        }
        nlohmann::json result{{"matches", matches}, {"total", matches.size()}};
        return Core::Result<nlohmann::json>::Ok(std::move(result));
    }

    void FileTools::registerAll(IToolProvider& provider)
    {
        // FileTools 作为静态函数集合，由 CompositeToolProvider 委托调用。
        // registerAll 是便利入口：通过 AEBuiltinTools::registerAll 统一注册。
        (void)provider; // 实际注册在 AEBuiltinTools::registerAll 中完成
    }

    // ================================================================
    //  GitTools
    // ================================================================

    namespace
    {
        auto run_git(std::string_view args) -> std::string
        {
            std::string cmd = "git " + std::string{args} + " 2>&1";
            std::array<char, 4096> buf{};
            std::string out;
#ifdef _WIN32
            FILE* pipe = _popen(cmd.c_str(), "r");
#else
            FILE* pipe = popen(cmd.c_str(), "r");
#endif
            if (!pipe) return {};
            while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
                out += buf.data();
#ifdef _WIN32
            _pclose(pipe);
#else
            pclose(pipe);
#endif
            return out;
        }
    } // anonymous namespace

    auto GitTools::gitStatus(const nlohmann::json& /*args*/) -> Core::Result<nlohmann::json>
    {
        auto output = run_git("status --porcelain");
        nlohmann::json r;
        r["status"] = output;
        r["files"] = output.empty() ? 0 : 1;
        return Core::Result<nlohmann::json>::Ok(std::move(r));
    }

    auto GitTools::gitDiff(const nlohmann::json& args) -> Core::Result<nlohmann::json>
    {
        std::string cmd = "diff";
        if (args.value("staged", false)) cmd += " --staged";
        auto output = run_git(cmd);
        nlohmann::json r;
        r["diff"] = output;
        return Core::Result<nlohmann::json>::Ok(std::move(r));
    }

    auto GitTools::gitLog(const nlohmann::json& args) -> Core::Result<nlohmann::json>
    {
        auto count = args.value("count", 10);
        auto output = run_git("log --oneline -" + std::to_string(count));
        nlohmann::json r;
        r["log"] = output;
        return Core::Result<nlohmann::json>::Ok(std::move(r));
    }

    auto GitTools::gitShow(const nlohmann::json& args) -> Core::Result<nlohmann::json>
    {
        auto ref = args.value("ref", "HEAD");
        auto output = run_git("show " + ref);
        nlohmann::json r;
        r["output"] = output;
        return Core::Result<nlohmann::json>::Ok(std::move(r));
    }

    void GitTools::registerAll(IToolProvider& provider)
    {
        (void)provider; // 实际注册在 AEBuiltinTools::registerAll 中完成
    }

    // ================================================================
    //  CompositeToolProvider — 内置工具委托适配器
    //  将 FileTools/GitTools 的静态函数包装为 IToolProvider，
    //  可独立使用或与 DceeToolProvider 组合。
    // ================================================================

    namespace
    {
        using ToolCallback = std::function<Core::Result<nlohmann::json>(const nlohmann::json&)>;

        struct ToolEntry
        {
            std::string name;
            std::string description;
            nlohmann::json parameters; // OpenAI function parameters schema
            ToolCallback callback;
        };
    }

    class BuiltinToolProvider : public IToolProvider
    {
    public:
        void addTool(std::string name, std::string description,
                     nlohmann::json parameters, ToolCallback callback)
        {
            m_tools_.push_back(ToolEntry{
                std::move(name), std::move(description),
                std::move(parameters), std::move(callback)});
        }

        auto execute(std::string_view name, const nlohmann::json &args, int /*max_retries*/)
            -> Core::Result<nlohmann::json> override
        {
            for (const auto &t : m_tools_)
            {
                if (t.name == name)
                    return t.callback(args);
            }
            return Core::Result<nlohmann::json>::Error(
                Core::Error{std::string{"BuiltinToolProvider: unknown tool: "} + std::string{name}});
        }

        auto list_tools() const -> std::vector<std::string> override
        {
            std::vector<std::string> names;
            names.reserve(m_tools_.size());
            for (const auto &t : m_tools_)
                names.push_back(t.name);
            return names;
        }

        auto get_tools_schema() const -> nlohmann::json override
        {
            auto arr = nlohmann::json::array();
            for (const auto &t : m_tools_)
            {
                auto func = nlohmann::json::object();
                func["name"] = t.name;
                func["description"] = t.description;
                func["parameters"] = t.parameters;
                arr.push_back({{"type", "function"}, {"function", func}});
            }
            return arr;
        }

    private:
        std::vector<ToolEntry> m_tools_;
    };

    // ================================================================
    //  AEBuiltinTools
    // ================================================================

    void AEBuiltinTools::registerAll(IToolProvider& provider)
    {
        // 尝试 dynamic_cast 到 BuiltinToolProvider（如果 provider 就是 BuiltinToolProvider）
        auto* builtin = dynamic_cast<BuiltinToolProvider*>(&provider);
        if (!builtin)
        {
            // 非 BuiltinToolProvider（如 DceeToolProvider），内置工具注册到其 execute 委托。
            // 当前阶段：内置工具由调用方自行选择注册方式。
            // 提供独立工厂方法 createBuiltinProvider() 供外部使用。
            return;
        }

        // ---- FileTools ----
        builtin->addTool("file_read", "Read file content",
            nlohmann::json::object({
                {"type", "object"},
                {"properties", {{"path", {{"type", "string"}, {"description", "File path to read"}}}}},
                {"required", nlohmann::json::array({"path"})},
            }),
            FileTools::fileRead);

        builtin->addTool("file_write", "Write content to file",
            nlohmann::json::object({
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "File path"}}},
                    {"content", {{"type", "string"}, {"description", "Content to write"}}},
                }},
                {"required", nlohmann::json::array({"path", "content"})},
            }),
            FileTools::fileWrite);

        builtin->addTool("file_edit", "Edit file with string replacement",
            nlohmann::json::object({
                {"type", "object"},
                {"properties", {
                    {"path", {{"type", "string"}, {"description", "File path"}}},
                    {"old_string", {{"type", "string"}, {"description", "String to replace"}}},
                    {"new_string", {{"type", "string"}, {"description", "Replacement string"}}},
                }},
                {"required", nlohmann::json::array({"path", "old_string", "new_string"})},
            }),
            FileTools::fileEdit);

        builtin->addTool("file_search", "Search files by pattern",
            nlohmann::json::object({
                {"type", "object"},
                {"properties", {
                    {"pattern", {{"type", "string"}, {"description", "File pattern"}}},
                    {"path", {{"type", "string"}, {"description", "Search directory"}}},
                }},
                {"required", nlohmann::json::array({"pattern"})},
            }),
            FileTools::fileSearch);

        // ---- GitTools ----
        builtin->addTool("git_status", "Show working tree status",
            nlohmann::json::object({
                {"type", "object"},
                {"properties", nlohmann::json::object()},
            }),
            GitTools::gitStatus);

        builtin->addTool("git_diff", "Show changes",
            nlohmann::json::object({
                {"type", "object"},
                {"properties", {
                    {"staged", {{"type", "boolean"}, {"description", "Show staged changes only"}}},
                }},
            }),
            GitTools::gitDiff);

        builtin->addTool("git_log", "Show commit history",
            nlohmann::json::object({
                {"type", "object"},
                {"properties", {
                    {"count", {{"type", "integer"}, {"description", "Number of commits"}}},
                }},
            }),
            GitTools::gitLog);

        builtin->addTool("git_show", "Show object details",
            nlohmann::json::object({
                {"type", "object"},
                {"properties", {
                    {"ref", {{"type", "string"}, {"description", "Git reference (default: HEAD)"}}},
                }},
            }),
            GitTools::gitShow);
    }

    // 工厂方法：创建独立的 BuiltinToolProvider
    auto createBuiltinProvider() -> std::unique_ptr<IToolProvider>
    {
        auto provider = std::make_unique<BuiltinToolProvider>();
        AEBuiltinTools::registerAll(*provider);
        return provider;
    }

} // namespace AE::Engine
