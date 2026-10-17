#pragma once

/// @file Ports/McpHttpClient.hpp
/// @brief MCP 传输的参考实现：JSON-RPC 2.0 over HTTP（Docs/01 §7.1、§11.2#3 = 抽象 + Client 实现）。
///
/// @details
/// - 复用既装 libcurl（`Core::Net::curlPerform`），零新增第三方依赖。
/// - 惰性握手：首个 list_tools/call_tool 前自动 `initialize` + `notifications/initialized`。
/// - 兼容两种响应：`application/json` 直返；`text/event-stream` 时抽取 `data:` 行的 JSON-RPC 消息。
/// - 线程安全：内部互斥串行化请求（MCP 会话本身有序）。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Error.hpp"
#include "Core/Logger.hpp"
#include "Core/Net/HttpCurl.hpp"

#include "Ports/McpTransport.hpp"

namespace AE::Engine
{

    class McpHttpClient final : public IMcpTransport
    {
    public:
        struct Options
        {
            std::string endpoint;                                  // MCP HTTP 端点（必填）
            std::string protocol_version = "2024-11-05";
            std::string client_name = "vxae";
            std::string client_version = "0.1";
            std::map<std::string, std::string> headers;            // 额外头（如 Authorization）
            long timeout_ms = 30'000;
            bool verify_ssl = true;
        };

        explicit McpHttpClient(Options opts) : m_opts(std::move(opts)) {}

        auto initialize() -> Core::Result<void> override
        {
            std::scoped_lock lk{m_mu};
            return ensure_initialized_locked();
        }

        auto list_tools() -> Core::Result<std::vector<ToolSpec>> override
        {
            std::scoped_lock lk{m_mu};
            if (auto init = ensure_initialized_locked(); init.isError())
                return Core::Result<std::vector<ToolSpec>>::Error(init.error());

            nlohmann::json params = nlohmann::json::object();
            auto r = rpc_locked("tools/list", params, /*expect_result=*/true);
            if (r.isError())
                return Core::Result<std::vector<ToolSpec>>::Error(r.error());

            std::vector<ToolSpec> out;
            const auto &res = r.value();
            if (res.is_object() && res.contains("tools") && res["tools"].is_array())
            {
                for (const auto &t : res["tools"])
                {
                    if (!t.is_object() || !t.contains("name"))
                        continue;
                    ToolSpec spec;
                    spec.name = t.value("name", std::string{});
                    spec.description = t.value("description", std::string{});
                    if (t.contains("inputSchema") && t["inputSchema"].is_object())
                        spec.parameters = t["inputSchema"];
                    out.push_back(std::move(spec));
                }
            }
            return Core::Result<std::vector<ToolSpec>>::Ok(std::move(out));
        }

        auto call_tool(std::string_view name, const nlohmann::json &args)
            -> Core::Result<ToolResult> override
        {
            std::scoped_lock lk{m_mu};
            if (auto init = ensure_initialized_locked(); init.isError())
                return Core::Result<ToolResult>::Error(init.error());

            nlohmann::json params = nlohmann::json::object();
            params["name"] = std::string{name};
            params["arguments"] = args.is_null() ? nlohmann::json::object() : args;

            auto r = rpc_locked("tools/call", params, /*expect_result=*/true);
            if (r.isError())
                return Core::Result<ToolResult>::Error(r.error());

            const auto &res = r.value();
            const bool is_error = res.is_object() && res.value("isError", false);
            std::string content;
            if (res.is_object() && res.contains("content") && res["content"].is_array())
            {
                for (const auto &c : res["content"])
                {
                    if (!c.is_object())
                        continue;
                    if (c.value("type", std::string{}) == "text" && c.contains("text"))
                        content += c["text"].get<std::string>();
                    else
                        content += c.dump();
                }
            }
            else
            {
                content = res.dump();
            }

            if (is_error)
                return Core::Result<ToolResult>::Ok(
                    ToolResult::failure(Core::Error(Core::ErrorCode::ExecutionError, std::move(content))));
            return Core::Result<ToolResult>::Ok(ToolResult::success(std::move(content)));
        }

        auto notify(std::string_view method, const nlohmann::json &params)
            -> Core::Result<void> override
        {
            std::scoped_lock lk{m_mu};
            nlohmann::json req = nlohmann::json::object();
            req["jsonrpc"] = "2.0";
            req["method"] = std::string{method};
            req["params"] = params;
            return post_locked(req, /*notification=*/true);
        }

    private:
        auto ensure_initialized_locked() -> Core::Result<void>
        {
            if (m_initialized)
                return Core::Result<void>::Ok();
            if (m_opts.endpoint.empty())
                return Core::Result<void>::Error(
                    Core::Error(Core::ErrorCode::PortInvalidArg, "McpHttpClient: endpoint is required"));

            nlohmann::json params = nlohmann::json::object();
            params["protocolVersion"] = m_opts.protocol_version;
            params["capabilities"] = nlohmann::json::object();
            params["clientInfo"] = nlohmann::json::object(
                {{"name", m_opts.client_name}, {"version", m_opts.client_version}});

            auto r = rpc_locked("initialize", params, /*expect_result=*/true);
            if (r.isError())
                return Core::Result<void>::Error(r.error());

            // 通知服务端握手完成（无 id、期望 2xx/202）。
            nlohmann::json note = nlohmann::json::object();
            note["jsonrpc"] = "2.0";
            note["method"] = "notifications/initialized";
            note["params"] = nlohmann::json::object();
            MXDRO_RETURN_IF_ERROR(post_locked(note, /*notification=*/true));

            m_initialized = true;
            Core::Logger::getInstance().infoFmt(
                "[AE:MCP] initialized: endpoint={} protocol={}", m_opts.endpoint, m_opts.protocol_version);
            return Core::Result<void>::Ok();
        }

        // 发送 JSON-RPC 请求（带 id）并解析 result（expect_result=true 时 result 为空亦可）。
        auto rpc_locked(std::string_view method, const nlohmann::json &params, bool expect_result)
            -> Core::Result<nlohmann::json>
        {
            const std::uint64_t id = ++m_next_id;
            nlohmann::json req = nlohmann::json::object();
            req["jsonrpc"] = "2.0";
            req["id"] = id;
            req["method"] = std::string{method};
            req["params"] = params;

            auto post = post_locked(req, /*notification=*/false);
            if (post.isError())
                return Core::Result<nlohmann::json>::Error(post.error());

            auto parsed = extract_rpc_json(post.value(), id);
            if (parsed.isError())
                return Core::Result<nlohmann::json>::Error(parsed.error());

            const auto &msg = parsed.value();
            if (msg.contains("error") && msg["error"].is_object())
            {
                const auto &e = msg["error"];
                const std::string m = e.value("message", std::string{"MCP error"});
                return Core::Result<nlohmann::json>::Error(
                    Core::Error(Core::ErrorCode::PortInternal,
                                std::string{method} + ": " + m));
            }
            if (!expect_result)
                return Core::Result<nlohmann::json>::Ok(nlohmann::json::object());
            if (msg.contains("result"))
                return Core::Result<nlohmann::json>::Ok(msg["result"]);
            return Core::Result<nlohmann::json>::Ok(nlohmann::json::object());
        }

        // 执行一次 HTTP POST，返回响应体文本。
        auto post_locked(const nlohmann::json &req, bool notification) -> Core::Result<std::string>
        {
            Core::Net::CurlRequest cr;
            cr.method = "POST";
            cr.url = m_opts.endpoint;
            cr.timeoutMs = m_opts.timeout_ms;
            cr.verifySsl = m_opts.verify_ssl;
            cr.headers["Content-Type"] = "application/json";
            cr.headers["Accept"] = "application/json, text/event-stream";
            for (const auto &[k, v] : m_opts.headers)
                cr.headers[k] = v;
            cr.body = req.dump();

            auto res = Core::Net::curlPerform(cr);
            if (res.isError())
                return Core::Result<std::string>::Error(
                    Core::Error(Core::ErrorCode::PortUnavailable, res.error().message()));

            const long status = res.value().statusCode;
            // 通知：2xx/202，通常空体。
            if (notification)
            {
                if (status < 200 || status >= 300)
                    return Core::Result<std::string>::Error(
                        Core::Error(Core::ErrorCode::PortInternal, "MCP notify HTTP " + std::to_string(status)));
                return Core::Result<std::string>::Ok(std::move(res.value().body));
            }
            if (status < 200 || status >= 300)
                return Core::Result<std::string>::Error(
                    Core::Error(Core::ErrorCode::PortInternal, "MCP HTTP " + std::to_string(status)));
            return Core::Result<std::string>::Ok(std::move(res.value().body));
        }

        // 解析响应体为单个 JSON-RPC 消息：支持 application/json 与 text/event-stream(data: 行)。
        static auto extract_rpc_json(std::string_view body, std::uint64_t /*id*/)
            -> Core::Result<nlohmann::json>
        {
            if (body.empty())
                return Core::Result<nlohmann::json>::Ok(nlohmann::json::object());

            // 尝试整体 JSON。
            {
                auto j = nlohmann::json::parse(body, nullptr, false);
                if (!j.is_discarded())
                    return Core::Result<nlohmann::json>::Ok(std::move(j));
            }

            // SSE：逐行抽取 "data:" 载荷，取最后一个可解析的 JSON 对象。
            nlohmann::json last = nlohmann::json::object();
            bool got = false;
            std::size_t pos = 0;
            while (pos < body.size())
            {
                std::size_t nl = body.find('\n', pos);
                if (nl == std::string_view::npos)
                    nl = body.size();
                std::string_view line = body.substr(pos, nl - pos);
                pos = nl + 1;
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                constexpr std::string_view kPrefix{"data:"};
                if (line.substr(0, kPrefix.size()) != kPrefix)
                    continue;
                std::string_view payload = line.substr(kPrefix.size());
                while (!payload.empty() && (payload.front() == ' ' || payload.front() == '\t'))
                    payload.remove_prefix(1);
                if (payload == "[DONE]" || payload.empty())
                    continue;
                auto j = nlohmann::json::parse(payload, nullptr, false);
                if (!j.is_discarded())
                {
                    last = std::move(j);
                    got = true;
                }
            }
            if (got)
                return Core::Result<nlohmann::json>::Ok(std::move(last));
            return Core::Result<nlohmann::json>::Error(
                Core::Error(Core::ErrorCode::PortInternal, "MCP: response is not JSON nor SSE data"));
        }

        Options m_opts;
        std::mutex m_mu;
        bool m_initialized = false;
        std::uint64_t m_next_id = 0;
    };

} // namespace AE::Engine
