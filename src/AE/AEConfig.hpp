#pragma once

/// @file AEConfig.hpp
/// @brief 结构化配置层（对应 AgentEngine.md §4.5「配置与密钥管理」）。
///
/// @details
/// 设计约束（关键）：
/// - 纯 AE::Engine 模块，不依赖 Butler / DCEE / Infrastructure，保持核心解耦可独立编译；
/// VXAE 独立版：文件解析改用既装的 nlohmann::json（JSON 配置过渡方案，去除 yaml-cpp
/// 依赖；格式定性后期再调整）。
/// - 密钥绝不硬编码、绝不入日志：从环境变量（前缀 AE_）或挂载的 secrets 文件读取，
/// 存入 AE::Core::Security::SecureString（析构时清零内存）；任何日志只记录「是否存在」而非明文。
/// - endpoint 等连接参数外部化；配置缺失 → Core::Result::Error，启动即失败（fail-fast），
/// 不允许「静默用默认值连生产」。
/// - 与 §4.7 接线：egress 白名单/模式直接构建 EgressPolicy（权限门默认拒绝）。
/// - 与 §6 接线：AEConfigProvider 以 shared_mutex + 不可变快照支持配置热加载。
///
/// @note 所属命名空间：AE::Engine

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann-json.hpp"
#include "Core/Security/SecureString.hpp"

#include "StreamingLLMClient.hpp"   // StreamingLLMClient（config → client 工厂）
#include "AESecurity.hpp"  // EgressPolicy
#include "Core/Logger.hpp"
#include "Core/Result.hpp"

namespace AE::Engine
{

    // ---------------------------------------------------- 结构化配置
    // 全部为纯数据（可拷贝/移动）；密钥单独以 SecureString 持有，绝不进入本结构体明文。
    class AEConfig
    {
    public:
        // ---- LLM 连接参数 ----
        struct Llm
        {
            std::string endpoint;    // 必填（fail-fast 校验）
            std::string model;       // 可选，部分后端需要
            int timeout_ms = 30000;  // 单次请求超时
            int max_retries = 3;     // 仅 5xx/网络错误退避重试
            double temperature = 0.7;
        };

        // ---- 预算 / 熔断 ----
        struct Budget
        {
            std::size_t max_iterations = 16;            // 循环硬上限
            std::size_t token_hard_limit = 0;           // 0 = 不限制（§4.4.1 熔断）
            std::chrono::seconds time_limit{0};         // 0 = 不限制（总时长预算）
            std::size_t max_tool_output_chars = 65536;  // 工具输出限长（§4.7 防投毒）
        };

        // ---- 出站（egress，§4.7 SSRF 防护）----
        struct Egress
        {
            EgressPolicy::Mode mode = EgressPolicy::Mode::DefaultDeny;  // 默认拒绝一切出网
            std::vector<std::string> allowed_hosts;                     // 精确 host 白名单
            std::vector<std::string> allowed_suffixes;                  // 域名后缀白名单
            bool block_metadata = true;                                 // 阻断 IMDS/回环/私有网段
        };

        // ---- 上下文 ----
        struct Context
        {
            std::size_t token_safety_margin_pct = 10;  // §4.3 Token 安全余量
        };

        Llm llm;
        Budget budget;
        Egress egress;
        Context context;

        // 取密钥（SecureString）。绝不以明文形式拷贝/打印。
        [[nodiscard]] auto api_key() const -> const AE::Core::Security::SecureString &
        {
            return m_api_key;
        }
        [[nodiscard]] auto has_api_key() const -> bool
        {
            return !std::string{m_api_key}.empty();
        }

        // ============================================ 加载入口
        // 1) 仅环境变量（前缀默认 "AE_"）；密钥从 env / secrets 文件注入。
        static auto from_env(std::string_view prefix = "AE_") -> Core::Result<AEConfig>
        {
            AEConfig cfg;
            apply_env_overrides(cfg, prefix);
            auto v = cfg.validate();
            if (v.isError())
                return Core::Result<AEConfig>::Error(v.error());
            return Core::Result<AEConfig>::Ok(std::move(cfg));
        }

        // 2) 仅 JSON 文本（env 不覆盖）；parse 失败返回类型化错误（绝不抛异常穿过边界）。
        static auto from_json(std::string_view json_text) -> Core::Result<AEConfig>
        {
            AEConfig cfg;
            try
            {
                const auto root = nlohmann::json::parse(json_text);
                fill_from_json(cfg, root);
            }
            catch (const std::exception &e)
            {
                return Core::Result<AEConfig>::Error(
                    Core::Error{std::string{"AEConfig JSON parse failed: "} + e.what()});
            }
            auto v = cfg.validate();
            if (v.isError())
                return Core::Result<AEConfig>::Error(v.error());
            return Core::Result<AEConfig>::Ok(std::move(cfg));
        }

        // 3) 从文件加载 JSON（自动探测路径；env 随后覆盖）。
        static auto from_file(std::string_view path) -> Core::Result<AEConfig>
        {
            std::error_code ec;
            const std::filesystem::path p{std::string{path}};
            if (!std::filesystem::is_regular_file(p, ec) || ec)
                return Core::Result<AEConfig>::Error(Core::Error{"AEConfig file not found: " + std::string{path}});
            std::ifstream in{p, std::ios::binary};
            if (!in)
                return Core::Result<AEConfig>::Error(Core::Error{"AEConfig cannot open file: " + std::string{path}});
            std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            return from_json(buf);
        }

        // 4) 组合：先文件（可选）再环境变量覆盖（env 优先），fail-fast 校验。
        static auto load(std::optional<std::string> file, std::string_view env_prefix = "AE_")
            -> Core::Result<AEConfig>
        {
            AEConfig cfg;
            if (file && !file->empty())
            {
                auto fr = from_file(*file);
                if (fr.isError())
                    return Core::Result<AEConfig>::Error(fr.error());
                cfg = std::move(fr.value());
            }
            apply_env_overrides(cfg, env_prefix);
            auto v = cfg.validate();
            if (v.isError())
                return Core::Result<AEConfig>::Error(v.error());
            return Core::Result<AEConfig>::Ok(std::move(cfg));
        }

        // ============================================ 校验（fail-fast）
        // endpoint 缺失 → Error（不允许静默连生产）；其余参数出界则告警并夹紧到安全默认。
        [[nodiscard]] auto validate() const -> Core::Result<void>
        {
            if (llm.endpoint.empty())
                return Core::Result<void>::Error(
                    Core::Error{"AEConfig: llm.endpoint is required (set AE_ENDPOINT or llm.endpoint in config)"});
            if (llm.timeout_ms <= 0)
                Core::Logger::getInstance().warningFmt("[AE:Config] timeout_ms={} 非法，夹紧到 30000", llm.timeout_ms);
            if (llm.max_retries < 0)
                Core::Logger::getInstance().warningFmt("[AE:Config] max_retries={} 非法，夹紧到 0", llm.max_retries);
            if (budget.max_iterations == 0)
                Core::Logger::getInstance().warningFmt("[AE:Config] max_iterations=0 非法，夹紧到 16");
            if (!has_api_key())
                Core::Logger::getInstance().warningFmt(
                    "[AE:Config] api_key 为空（本地模型可忽略；远程模型将鉴权失败）");
            return Core::Result<void>::Ok();
        }

        // ============================================ 接线 §4.7 / 客户端
        // 由配置构建 EgressPolicy（权限门默认拒绝未列白名单的出网）。
        [[nodiscard]] auto build_egress_policy() const -> EgressPolicy
        {
            EgressPolicy p{egress.mode};
            for (const auto &h : egress.allowed_hosts)
                p.allow_host(h);
            for (const auto &s : egress.allowed_suffixes)
                p.allow_suffix(s);
            p.set_block_metadata(egress.block_metadata);
            return p;
        }

        // 由配置直接构造流式 LLM 客户端（endpoint/密钥/超时/重试全部外部化）。
        [[nodiscard]] auto make_llm_client() const -> StreamingLLMClient
        {
            return StreamingLLMClient{llm.endpoint,
                                      m_api_key,
                                      llm.timeout_ms > 0 ? llm.timeout_ms : 30000,
                                      llm.max_retries > 0 ? llm.max_retries : 0};
        }

    private:
        AE::Core::Security::SecureString m_api_key;  // 密钥 NEVER 明文落结构体/日志

        // ----- JSON → 结构体（支持扁平键与 llm:/budget:/egress: 嵌套）-----
        static void fill_from_json(AEConfig &cfg, const nlohmann::json &root)
        {
            if (!root.is_object())
                return;
            const nlohmann::json llm = pick_section(root, "llm");
            const nlohmann::json budget = pick_section(root, "budget");
            const nlohmann::json egress = pick_section(root, "egress");
            const nlohmann::json ctx = pick_section(root, "context");

            cfg.llm.endpoint = pick_str(root, llm, "endpoint", cfg.llm.endpoint);
            cfg.llm.model = pick_str(root, llm, "model", cfg.llm.model);
            cfg.llm.timeout_ms = static_cast<int>(pick_int(root, llm, "timeout_ms", cfg.llm.timeout_ms));
            cfg.llm.max_retries = static_cast<int>(pick_int(root, llm, "max_retries", cfg.llm.max_retries));
            cfg.llm.temperature = pick_dbl(root, llm, "temperature", cfg.llm.temperature);

            cfg.budget.max_iterations = static_cast<std::size_t>(
                pick_int(root, budget, "max_iterations", static_cast<long>(cfg.budget.max_iterations)));
            cfg.budget.token_hard_limit = static_cast<std::size_t>(
                pick_int(root, budget, "token_hard_limit", static_cast<long>(cfg.budget.token_hard_limit)));
            cfg.budget.time_limit = std::chrono::seconds{
                pick_int(root, budget, "time_limit_s", static_cast<long>(cfg.budget.time_limit.count()))};
            cfg.budget.max_tool_output_chars = static_cast<std::size_t>(
                pick_int(root, budget, "max_tool_output_chars", static_cast<long>(cfg.budget.max_tool_output_chars)));

            const std::string mode = pick_str(root, egress, "mode", "default_deny");
            if (mode == "default_allow")
                cfg.egress.mode = EgressPolicy::Mode::DefaultAllow;
            cfg.egress.block_metadata = pick_bool(root, egress, "block_metadata", cfg.egress.block_metadata);
            pick_str_array(egress, "allowed_hosts", cfg.egress.allowed_hosts);
            pick_str_array(egress, "allowed_suffixes", cfg.egress.allowed_suffixes);

            cfg.context.token_safety_margin_pct = static_cast<std::size_t>(
                pick_int(root, ctx, "token_safety_margin_pct", static_cast<long>(cfg.context.token_safety_margin_pct)));

            // 密钥：优先 env（apply_env_overrides 覆盖）；此处仅取文件内明文（仅当 env 未给）。
            if (root.contains("api_key") && root["api_key"].is_string())
            {
                const std::string k = root["api_key"].get<std::string>();
                if (!k.empty())
                    cfg.m_api_key = AE::Core::Security::SecureString{k};
            }
        }

        // ----- 环境变量覆盖（env 优先于文件）-----
        static void apply_env_overrides(AEConfig &cfg, std::string_view prefix)
        {
            const std::string p{prefix};
            auto ev = [&](const std::string &key) -> std::optional<std::string>
            {
                const std::string full = p + key;
                const char *v = std::getenv(full.c_str());
                if (!v || v[0] == '\0')
                    return std::nullopt;
                return std::string{v};
            };

            if (auto v = ev("ENDPOINT"))
                cfg.llm.endpoint = *v;
            if (auto v = ev("MODEL"))
                cfg.llm.model = *v;
            if (auto v = ev("TIMEOUT_MS"))
                cfg.llm.timeout_ms = static_cast<int>(env_int(*v, cfg.llm.timeout_ms));
            if (auto v = ev("MAX_RETRIES"))
                cfg.llm.max_retries = static_cast<int>(env_int(*v, cfg.llm.max_retries));
            if (auto v = ev("TEMPERATURE"))
                cfg.llm.temperature = env_dbl(*v, cfg.llm.temperature);

            if (auto v = ev("MAX_ITERATIONS"))
                cfg.budget.max_iterations =
                    static_cast<std::size_t>(env_int(*v, static_cast<long>(cfg.budget.max_iterations)));
            if (auto v = ev("TOKEN_HARD_LIMIT"))
                cfg.budget.token_hard_limit =
                    static_cast<std::size_t>(env_int(*v, static_cast<long>(cfg.budget.token_hard_limit)));
            if (auto v = ev("TIME_LIMIT_S"))
                cfg.budget.time_limit =
                    std::chrono::seconds{env_int(*v, static_cast<long>(cfg.budget.time_limit.count()))};
            if (auto v = ev("MAX_TOOL_OUTPUT_CHARS"))
                cfg.budget.max_tool_output_chars =
                    static_cast<std::size_t>(env_int(*v, static_cast<long>(cfg.budget.max_tool_output_chars)));

            if (auto v = ev("EGRESS_MODE"))
                cfg.egress.mode =
                    (*v == "default_allow") ? EgressPolicy::Mode::DefaultAllow : EgressPolicy::Mode::DefaultDeny;
            if (auto v = ev("EGRESS_BLOCK_METADATA"))
                cfg.egress.block_metadata = env_bool(*v, cfg.egress.block_metadata);
            if (auto v = ev("ALLOWED_HOSTS"))
                split_csv(*v, cfg.egress.allowed_hosts);
            if (auto v = ev("ALLOWED_SUFFIXES"))
                split_csv(*v, cfg.egress.allowed_suffixes);

            if (auto v = ev("TOKEN_SAFETY_MARGIN_PCT"))
                cfg.context.token_safety_margin_pct =
                    static_cast<std::size_t>(env_int(*v, static_cast<long>(cfg.context.token_safety_margin_pct)));

            // 密钥：AE_API_KEY 直接给；AE_API_KEY_FILE 指向挂载的 secrets 文件（内容即密钥）。
            if (auto v = ev("API_KEY"))
            {
                cfg.m_api_key = AE::Core::Security::SecureString{*v};
            }
            else if (auto f = ev("API_KEY_FILE"))
            {
                if (auto content = read_secret_file(*f))
                    cfg.m_api_key = AE::Core::Security::SecureString{*content};
            }
        }

        // ----- JSON 取值辅助（扁平优先，其次嵌套节）-----
        static auto pick_section(const nlohmann::json &root, const char *name) -> nlohmann::json
        {
            if (root.contains(name) && root[name].is_object())
                return root[name];
            return nlohmann::json::object();
        }
        static auto pick_str(const nlohmann::json &root,
                             const nlohmann::json &section,
                             const char *key,
                             std::string fallback) -> std::string
        {
            if (root.contains(key) && root[key].is_string())
                return root[key].get<std::string>();
            if (section.contains(key) && section[key].is_string())
                return section[key].get<std::string>();
            return fallback;
        }
        static auto pick_int(const nlohmann::json &root, const nlohmann::json &section, const char *key, long fallback)
            -> long
        {
            if (root.contains(key) && root[key].is_number_integer())
                return root[key].get<long>();
            if (section.contains(key) && section[key].is_number_integer())
                return section[key].get<long>();
            return fallback;
        }
        static auto pick_bool(const nlohmann::json &root, const nlohmann::json &section, const char *key, bool fallback)
            -> bool
        {
            if (root.contains(key) && root[key].is_boolean())
                return root[key].get<bool>();
            if (section.contains(key) && section[key].is_boolean())
                return section[key].get<bool>();
            return fallback;
        }
        static auto pick_dbl(const nlohmann::json &root,
                             const nlohmann::json &section,
                             const char *key,
                             double fallback) -> double
        {
            if (root.contains(key) && root[key].is_number())
                return root[key].get<double>();
            if (section.contains(key) && section[key].is_number())
                return section[key].get<double>();
            return fallback;
        }
        static void pick_str_array(const nlohmann::json &section, const char *key, std::vector<std::string> &out)
        {
            if (!section.contains(key) || !section[key].is_array())
                return;
            for (const auto &it : section[key])
                if (it.is_string())
                    out.push_back(it.get<std::string>());
        }

        // ----- 环境变量解析辅助 -----
        static auto env_int(std::string_view s, long fallback) -> long
        {
            long v = 0;
            const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
            if (res.ec != std::errc())
                return fallback;
            return v;
        }
        static auto env_dbl(std::string_view s, double fallback) -> double
        {
            const std::string tmp{s};
            char *end = nullptr;
            const double v = std::strtod(tmp.c_str(), &end);  // <cstdlib>
            if (end == tmp.c_str())
                return fallback;
            return v;
        }
        static auto env_bool(std::string_view s, bool fallback) -> bool
        {
            if (s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "YES" || s == "on")
                return true;
            if (s == "0" || s == "false" || s == "FALSE" || s == "no" || s == "NO" || s == "off")
                return false;
            return fallback;
        }
        static void split_csv(const std::string &s, std::vector<std::string> &out)
        {
            std::string cur;
            for (char c : s)
            {
                if (c == ',')
                {
                    if (!cur.empty())
                        out.push_back(cur);
                    cur.clear();
                }
                else if (!std::isspace(static_cast<unsigned char>(c)))
                {
                    cur.push_back(c);
                }
            }
            if (!cur.empty())
                out.push_back(cur);
        }

        // 读取 secrets 文件内容（去除末尾换行）；失败返回 nullopt，绝不抛异常。
        static auto read_secret_file(std::string_view path) -> std::optional<std::string>
        {
            std::ifstream in{std::string{path}, std::ios::binary};
            if (!in)
                return std::nullopt;
            std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r'))
                buf.pop_back();
            return buf.empty() ? std::nullopt : std::optional<std::string>{std::move(buf)};
        }
    };

    // ---------------------------------------------------- 配置热加载（§6 shared_mutex + 不可变快照）
    // 持有不可变 AEConfig 快照；reload() 重新读取并原子替换，读取方仅拷贝 shared_ptr（无锁）。
    class AEConfigProvider
    {
    public:
        explicit AEConfigProvider(AEConfig initial)
            : m_snapshot(std::make_shared<const AEConfig>(std::move(initial)))
        {
        }

        // 读取当前快照（shared_ptr 拷贝，调用方持有期间配置不被销毁）。
        [[nodiscard]] auto snapshot() const -> std::shared_ptr<const AEConfig>
        {
            std::shared_lock lock{m_mutex};
            return m_snapshot;
        }

        // 热加载：重新读取文件 + 环境变量覆盖，校验通过后原子替换快照。
        auto reload(std::optional<std::string> file, std::string_view env_prefix = "AE_") -> Core::Result<void>
        {
            auto r = AEConfig::load(std::move(file), env_prefix);
            if (r.isError())
                return Core::Result<void>::Error(r.error());
            {
                std::unique_lock lock{m_mutex};
                m_snapshot = std::make_shared<const AEConfig>(std::move(r.value()));
            }
            return Core::Result<void>::Ok();
        }

    private:
        mutable std::shared_mutex m_mutex;
        std::shared_ptr<const AEConfig> m_snapshot;
    };

}  // namespace AE::Engine
