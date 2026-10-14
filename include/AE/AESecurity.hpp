#pragma once

/// @file AESecurity.hpp
/// @brief SSRF / egress 出站防护（对应 AgentEngine.md §4.7「SSRF（工具/插件访问内网）」）。
///
/// @details
/// 设计约束（关键）：
/// - 纯 VMX::AE 模块，不依赖 Butler / DCEE / MemoryStick；经既有
/// IPermissionGate 注入点接线（AgentEngine 在每次工具执行前调用
/// can_execute_tool），不修改已构建文件的对外签名。
/// - 默认拒绝一切出网（DefaultDeny，安全优先）；仅放行显式白名单中的 host/域名。
/// - 硬性阻断链路本地 / 元数据地址（169.254.169.254 等 IMDS）、回环、私有网段；
/// 即便工具参数里夹带，也一律拒绝，杜绝「工具输出回灌 + 内网穿透」组合风险。
/// - 这是**纵深防御**的一环：DCEE 沙箱负责进程级隔离，本层负责「出网意图」拦截。
///
/// @note 所属命名空间：VMX::AE

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "Core/Result.hpp"
#include "Core/Logger.hpp"
#include "IAE.hpp" // IPermissionGate

namespace VMX::AE
{

    // ---------------------------------------------------- 出站策略
    class EgressPolicy
    {
    public:
        enum class Mode
        {
            DefaultDeny,  // 默认拒绝一切出网（§4.7 安全优先）
            DefaultAllow, // 仅阻断明确危险地址（宽松，不推荐生产）
        };

        explicit EgressPolicy(Mode mode = Mode::DefaultDeny) : m_mode(mode) {}

        // 精确 host 或 glob 后缀放行：支持 "api.openai.com" 精确，
        // 或 "*.example.com"（匹配 example.com 及其任意子域）。
        void allow_host(std::string host)
        {
            if (!host.empty())
                m_allowed_hosts.emplace_back(std::move(host));
        }
        // 域名后缀放行：".internal.example.com" 匹配该域及其子域。
        void allow_suffix(std::string suffix)
        {
            if (!suffix.empty())
                m_allowed_suffixes.emplace_back(std::move(suffix));
        }
        // 是否阻断元数据/回环/私有网段（默认开启）。
        void set_block_metadata(bool b) { m_block_metadata = b; }

        // 判定一个 URL / host 是否允许出网。
        [[nodiscard]] auto is_allowed(std::string_view target) const -> bool
        {
            const std::string host = extract_host(target);
            if (host.empty())
                return true; // 非网络目标，放行（无出网意图）
            if (host_allowed(host))
                return true; // 显式白名单优先（管理员意图）
            if (is_blocked_address(host))
                return false; // 元数据/回环/私有，硬性阻断
            return m_mode == Mode::DefaultAllow;
        }

    private:
        // 从 URL / host 文本抽取 host（去 scheme、路径、端口、IPv6 括号）。
        static auto extract_host(std::string_view target) -> std::string
        {
            std::string s{target};
            // 去 scheme
            auto pos = s.find("://");
            if (pos != std::string::npos)
                s = s.substr(pos + 3);
            // 去路径/查询/片段
            for (char c : {'/', '?', '#'})
            {
                auto p = s.find(c);
                if (p != std::string::npos)
                    s = s.substr(0, p);
            }
            // 去端口（非 IPv6 情形）
            if (s.empty() || s.front() != '[')
            {
                auto colon = s.rfind(':');
                if (colon != std::string::npos)
                    s = s.substr(0, colon);
            }
            else
            {
                // IPv6 [::1]:port → 取括号内
                auto close = s.find(']');
                if (close != std::string::npos)
                    s = s.substr(1, close - 1);
            }
            // 统一小写便于比对
            for (auto &ch : s)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        }

        [[nodiscard]] auto host_allowed(std::string_view host) const -> bool
        {
            for (const auto &a : m_allowed_hosts)
            {
                if (a == host)
                    return true;
                if (a.size() > 1 && a.front() == '*' && a[1] == '.')
                {
                    // "*.example.com" → host 以 ".example.com" 结尾
                    std::string_view suff = std::string_view{a}.substr(1);
                    if (host.size() >= suff.size() &&
                        host.compare(host.size() - suff.size(), suff.size(), suff) == 0)
                        return true;
                }
            }
            for (const auto &suff : m_allowed_suffixes)
            {
                std::string_view sv{suff};
                if (sv.empty())
                    continue;
                if (sv.front() == '.')
                {
                    if (host.size() >= sv.size() &&
                        host.compare(host.size() - sv.size(), sv.size(), sv) == 0)
                        return true;
                }
                else if (host == sv)
                {
                    return true;
                }
            }
            return false;
        }

        // 元数据/回环/私有网段：SSRF 高危地址一律阻断。
        [[nodiscard]] auto is_blocked_address(std::string_view host) const -> bool
        {
            if (!m_block_metadata)
                return false;
            if (host == "localhost" || host == "::1" || host == "[::1]" || host == "::")
                return true;
            if (host.starts_with("169.254.")) // 链路本地 + 云元数据 IMDS
                return true;
            if (host.starts_with("127.")) // 回环
                return true;
            if (host.starts_with("10.")) // RFC1918 A
                return true;
            if (host.starts_with("192.168.")) // RFC1918 C
                return true;
            if (host.starts_with("0.")) // 非法/保留
                return true;
            if (host.starts_with("172.")) // RFC1918 B 16-31
            {
                // 取第二段
                auto dot = host.find('.', 4);
                if (dot != std::string::npos)
                {
                    int octet = 0;
                    bool ok = true;
                    for (std::size_t i = 4; i < dot; ++i)
                    {
                        if (host[i] < '0' || host[i] > '9')
                        {
                            ok = false;
                            break;
                        }
                        octet = octet * 10 + (host[i] - '0');
                    }
                    if (ok && octet >= 16 && octet <= 31)
                        return true;
                }
            }
            return false;
        }

        std::vector<std::string> m_allowed_hosts;
        std::vector<std::string> m_allowed_suffixes;
        Mode m_mode = Mode::DefaultDeny;
        bool m_block_metadata = true;
    };

    // ---------------------------------------------------- 权限门：egress 校验
    // 实现 IPermissionGate，在工具执行前对参数中出现的 URL/host 做出站校验。
    // 已知网络键（url/endpoint/host/...）的值、以及任意含 "://" 的字符串均被扫描；
    // 命中拒绝即拦截该工具调用，返回类型化错误（绝不抛异常穿过边界）。
    class EgressGate : public IPermissionGate
    {
    public:
        explicit EgressGate(EgressPolicy policy) : m_policy(std::move(policy)) {}

        auto can_execute_tool(std::string_view name,
                              const nlohmann::json &args) -> Core::Result<void> override
        {
            std::vector<std::string> targets;
            collect_targets(args, targets, false);
            for (const auto &t : targets)
            {
                if (!m_policy.is_allowed(t))
                {
                    Core::Logger::getInstance().warningFmt(
                        "[AE:Security] egress denied: tool='{}' target='{}'", name, t);
                    return Core::Result<void>::Error(
                        Core::Error{"Egress denied: " + t});
                }
            }
            return Core::Result<void>::Ok();
        }

    private:
        static constexpr std::string_view k_net_keys[] = {
            "url", "uri", "endpoint", "host", "hostname", "address",
            "proxy", "base_url", "baseurl", "server", "api_url", "apiurl"};

        static auto is_net_key(std::string_view key) -> bool
        {
            for (auto k : k_net_keys)
                if (k == key)
                    return true;
            return false;
        }

        // 递归收集疑似网络目标字符串（保守：仅网络键的值 + 含 "://" 的串）。
        static void collect_targets(const nlohmann::json &v,
                                    std::vector<std::string> &out, bool key_is_net)
        {
            if (v.is_string())
            {
                const std::string s = v.get<std::string>();
                if (s.empty())
                    return;
                if (key_is_net || s.find("://") != std::string::npos)
                    out.push_back(s);
            }
            else if (v.is_object())
            {
                for (auto it = v.begin(); it != v.end(); ++it)
                {
                    const bool net = key_is_net || is_net_key(it.key());
                    collect_targets(it.value(), out, net);
                }
            }
            else if (v.is_array())
            {
                for (const auto &e : v)
                    collect_targets(e, out, key_is_net);
            }
        }

        EgressPolicy m_policy;
    };

} // namespace VMX::AE
