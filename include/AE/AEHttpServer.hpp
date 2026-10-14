#pragma once

/// @file AEHttpServer.hpp
/// @brief §9 可选远程暴露层：用 Drogon 把 AgentEngine::run() 的结果以 SSE 回传给上层。
///
/// @details
/// 设计约束（关键）：
/// - 本文件整体受宏 VMX_AE_ENABLE_HTTP 保护。仅当 CMake 开关 VMX_AE_HTTP=ON
/// （开发阶段默认 ON）时编译并链接 Drogon；部署时可关闭以保持
/// VMX::AE 核心库（vxae）的轻量解耦（与 §4.6 OTel 开关同范式）。
/// - 仅注册路由到全局单例 drogon::app()，不自行 run()，以便与既有
/// Infrastructure::SSEPipeline / StreamingServer 共用同一 Drogon 实例。
/// 独立部署时可调用 serve()，由本类独占 drogon::app().run()。
/// - 鉴权（Bearer token）+ 进程内令牌桶限流在 handler 内联完成，
/// 不引入额外 filter 类，降低编译风险。
/// - 完全经 IAgentExecutor 抽象运行，不感知 AgentEngine 具体依赖（工具/上下文/LLM）。
///
/// @note 所属命名空间：VMX::AE

#ifdef VMX_AE_ENABLE_HTTP

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef INCL_EXTRA_HTON_FUNCTIONS
#define INCL_EXTRA_HTON_FUNCTIONS
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif // _WIN32

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "Core/Logger.hpp"
#include "IAE.hpp"         // IAgentExecutor
#include "AEType.hpp"      // AgentEvent / EventKind
#include "AETelemetry.hpp" // make_trace_id()

namespace VMX::AE
{

    /**
     * 进程内令牌桶限流（按客户端 IP 固定窗口）。
     * 极简实现，仅用于 §9「限流中间件」的可用落地；
     * 生产可换为 Redis/集中式限流，接口不变。
     */
    class HttpRateLimiter
    {
    public:
        explicit HttpRateLimiter(uint32_t max_per_window = 60,
                                 std::chrono::seconds window = std::chrono::seconds{60})
            : m_max(max_per_window), m_window(window) {}

        // 返回 true 表示放行；false 表示触发限流（应返回 429）。
        auto acquire(std::string_view client_ip) -> bool
        {
            if (m_max == 0)
                return true; // 0 = 不限流
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(m_mtx);
            auto &b = m_buckets[std::string{client_ip}];
            if (now >= b.reset_at || b.remaining == 0)
            {
                b.remaining = m_max;
                b.reset_at = now + m_window;
            }
            if (b.remaining == 0)
                return false;
            --b.remaining;
            return true;
        }

    private:
        struct Bucket
        {
            uint32_t remaining = 0;
            std::chrono::steady_clock::time_point reset_at;
        };
        std::mutex m_mtx;
        std::unordered_map<std::string, Bucket> m_buckets;
        uint32_t m_max;
        std::chrono::seconds m_window;
    };

    /**
     * AE 的 Drogon HTTP 暴露层（可选）。
     *
     * 使用流程：
     *   1) auto srv = std::make_shared<AEHttpServer>();
     *   2) srv->set_executor(engine);          // engine 实现 IAgentExecutor
     *   3) srv->set_auth_token(env_token);      // 可选；空=关闭鉴权
     *   4) srv->configure_from_env();           // 可选；读 VMX_AE_* 环境变量
     *   5) 集成部署：srv->register_routes();      // 仅注册，由中心服务 run()
     *      独立部署：srv->serve(host, port);      // 本类独占 drogon::app().run()
     *
     * 路由：
     *   POST /api/ae/v1/run   (SSE)  请求体 OpenAI 风格 {messages:[...], session_id?, max_iterations?}
     *   GET  /api/ae/v1/run   (SSE)  ?prompt=... 或 ?session_id=&messages=[...]
     *   GET  /api/ae/v1/health       存活探测（不受鉴权/限流约束）
     */
    class AEHttpServer
    {
    public:
        // 绑定执行内核（AgentEngine 实例）。必填。
        auto set_executor(std::shared_ptr<IAgentExecutor> executor) -> void
        {
            m_executor = std::move(executor);
        }

        // 设置 Bearer token；传空串表示关闭鉴权（任何请求放行）。
        auto set_auth_token(std::string token) -> void
        {
            m_auth_token = std::move(token);
        }

        // 设置限流：每窗口最多 max_per_window 次请求。
        auto set_rate_limit(uint32_t max_per_window,
                            std::chrono::seconds window = std::chrono::seconds{60}) -> void
        {
            m_rl = std::make_shared<HttpRateLimiter>(max_per_window, window);
        }

        // 独立部署时设置 Drogon 工作线程数。
        auto set_thread_num(size_t n) -> void { m_threads = n; }

        // 从环境变量读取配置（不覆盖已显式 set_* 的值）。
        //   VMX_AE_API_TOKEN        鉴权 token（空则不启用）
        //   VMX_AE_HTTP_RATE_LIMIT  每 60s 窗口最大请求数（0=不限）
        //   VMX_AE_HTTP_THREADS     Drogon 工作线程数
        auto configure_from_env() -> void
        {
            const char *tok = std::getenv("VMX_AE_API_TOKEN");
            if (tok && m_auth_token.empty())
                m_auth_token = tok;
            const char *rl = std::getenv("VMX_AE_HTTP_RATE_LIMIT");
            if (rl)
            {
                try
                {
                    const uint32_t n = static_cast<uint32_t>(std::stoul(rl));
                    if (n > 0)
                        m_rl = std::make_shared<HttpRateLimiter>(n, std::chrono::seconds{60});
                }
                catch (...)
                {
                }
            }
            const char *th = std::getenv("VMX_AE_HTTP_THREADS");
            if (th)
            {
                try
                {
                    const size_t n = std::stoul(th);
                    if (n > 0)
                        m_threads = n;
                }
                catch (...)
                {
                }
            }
        }

        // 把路由注册到全局 drogon::app()（须在 drogon::app().run() 之前调用）。
        auto register_routes() -> void
        {
            auto &app = drogon::app();

            // ---- 存活探测 ----
            app.registerHandler(
                "/api/ae/v1/health",
                [](const drogon::HttpRequestPtr &,
                   std::function<void(const drogon::HttpResponsePtr &)> &&callback)
                {
                    nlohmann::json ok = nlohmann::json::object();
                    ok["status"] = "ok";
                    ok["service"] = "vmx-ae";
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k200OK);
                    resp->addHeader("Content-Type", "application/json; charset=utf-8");
                    resp->setBody(ok.dump());
                    callback(resp);
                },
                {drogon::Get, drogon::Options});

            // ---- 运行时（SSE） ----
            // 用 weak_ptr 捕获，避免 server 析构后回调仍持有 executor。
            std::weak_ptr<IAgentExecutor> weak_exec = m_executor;
            const std::string auth = m_auth_token;
            auto rl = m_rl; // 共享指针，handler 内共享同一限流状态

            auto run_handler =
                [weak_exec, auth, rl](const drogon::HttpRequestPtr &req,
                                      std::function<void(const drogon::HttpResponsePtr &)> &&callback) mutable
            {
                // 限流
                if (!rl->acquire(req->getPeerAddr().toIp()))
                {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k429TooManyRequests);
                    resp->setBody("{\"error\":\"rate_limited\"}");
                    callback(resp);
                    return;
                }
                // 鉴权
                if (!auth.empty())
                {
                    const std::string h = req->getHeader("authorization");
                    if (h.size() < 7 || h.compare(0, 7, "Bearer ") != 0 ||
                        h.compare(7, std::string::npos, auth) != 0)
                    {
                        auto resp = drogon::HttpResponse::newHttpResponse();
                        resp->setStatusCode(drogon::k401Unauthorized);
                        resp->addHeader("WWW-Authenticate", "Bearer");
                        resp->setBody("{\"error\":\"unauthorized\"}");
                        callback(resp);
                        return;
                    }
                }

                auto exec = weak_exec.lock();
                if (!exec)
                {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k503ServiceUnavailable);
                    resp->setBody("{\"error\":\"executor_unavailable\"}");
                    callback(resp);
                    return;
                }

                // 解析请求体
                nlohmann::json body;
                std::string session_id;
                std::size_t max_iter = 16;
                auto parse_err = parse_request(req, body, session_id, max_iter);
                if (!parse_err.empty())
                {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k400BadRequest);
                    resp->setBody("{\"error\":\"" + parse_err + "\"}");
                    callback(resp);
                    return;
                }

                // 构造 SSE 异步响应
                auto resp = drogon::HttpResponse::newAsyncStreamResponse(
                    [exec, body, session_id, max_iter](drogon::ResponseStreamPtr stream)
                    {
                        // 握手：SSE 重连间隔
                        stream->send("retry: 3000\n\n");
                        try
                        {
                            for (auto ev : exec->run(session_id, body, max_iter))
                            {
                                const std::string sse = to_sse(ev);
                                if (!stream->send(sse)) // 客户端断开
                                    break;
                            }
                            stream->send("data: {\"type\":\"done\"}\n\n");
                        }
                        catch (const std::exception &e)
                        {
                            nlohmann::json err = nlohmann::json::object();
                            err["type"] = "error";
                            err["error"] = e.what();
                            stream->send("data: " + err.dump() + "\n\n");
                        }
                        // stream 析构自动 close()
                    });
                resp->setStatusCode(drogon::k200OK);
                resp->addHeader("Content-Type", "text/event-stream; charset=utf-8");
                resp->addHeader("Cache-Control", "no-cache");
                resp->addHeader("Connection", "keep-alive");
                resp->addHeader("X-Accel-Buffering", "no");
                resp->addHeader("Access-Control-Allow-Origin", "*");
                callback(resp);
            };

            app.registerHandler("/api/ae/v1/run", run_handler,
                                {drogon::Post, drogon::Get, drogon::Options});
        }

        // 独立部署：注册路由后在独立线程启动 Drogon。
        // ⚠️ 仅当本进程没有其他 Drogon 子系统（SSEPipeline/StreamingServer）时调用；
        //    集成部署请只调 register_routes()，让中心服务统一 run()。
        auto serve(const std::string &host = "0.0.0.0", uint16_t port = 8088) -> Core::Result<void>
        {
            if (!m_executor)
                return Core::Result<void>::Error(
                    Core::Error{Core::ErrorCode::InvalidArgument, "AEHttpServer: executor not set"});
            try
            {
                register_routes();
                drogon::app().setThreadNum(static_cast<int>(m_threads));
                drogon::app().addListener(host, port);
                m_running = true;
                m_thread = std::thread([this, host, port]
                                       {
                    try
                    {
                        Core::Logger::getInstance().infoFmt("[AEHttpServer] Drogon listening on {}:{}", host, port);
                        drogon::app().run();
                    }
                    catch (const std::exception &e)
                    {
                        Core::Logger::getInstance().errorFmt("[AEHttpServer] run exception: {}", e.what());
                    }
                    m_running = false; });
                return Core::Result<void>::Ok();
            }
            catch (const std::exception &e)
            {
                m_running = false;
                return Core::Result<void>::Error(
                    Core::Error{Core::ErrorCode::InternalError,
                                std::string{"AEHttpServer serve failed: "} + e.what()});
            }
        }

        auto stop() -> Core::Result<void>
        {
            if (!m_running.load())
                return Core::Result<void>::Ok();
            try
            {
                drogon::app().quit();
                if (m_thread.joinable())
                    m_thread.detach();
                m_running = false;
                return Core::Result<void>::Ok();
            }
            catch (const std::exception &e)
            {
                m_running = false;
                return Core::Result<void>::Error(
                    Core::Error{Core::ErrorCode::InternalError,
                                std::string{"AEHttpServer stop failed: "} + e.what()});
            }
        }

        auto is_running() const -> bool { return m_running.load(); }

    private:
        // 把 AgentEvent 序列化为 SSE data 行。
        static auto to_sse(const AgentEvent &ev) -> std::string
        {
            nlohmann::json e = nlohmann::json::object();
            switch (ev.kind)
            {
            case EventKind::Token:
                e["type"] = "token";
                e["data"] = ev.payload;
                break;
            case EventKind::Thinking:
                e["type"] = "thinking";
                e["data"] = ev.payload;
                break;
            case EventKind::ToolCall:
                e["type"] = "tool_call";
                e["name"] = ev.payload;
                break;
            case EventKind::ToolResult:
                e["type"] = "tool_result";
                e["data"] = ev.payload;
                break;
            case EventKind::Final:
                e["type"] = "final";
                e["data"] = ev.payload;
                break;
            case EventKind::Error:
                e["type"] = "error";
                e["error"] = ev.error_info.message();
                break;
            default:
                e["type"] = "unknown";
                break;
            }
            return "data: " + e.dump() + "\n\n";
        }

        // 解析请求体（POST JSON 或 GET 查询参数）为引擎请求体。
        static auto parse_request(const drogon::HttpRequestPtr &req,
                                  nlohmann::json &out_body,
                                  std::string &out_sid,
                                  std::size_t &out_max_iter) -> std::string
        {
            out_max_iter = 16;
            const std::string method = req->getMethodString(); // "GET"/"POST"/...
            try
            {
                if (method == "POST" || method == "PUT")
                {
                    const std::string raw{req->getBody()};
                    if (raw.empty())
                        return "empty_body";
                    out_body = nlohmann::json::parse(raw);
                }
                else // GET
                {
                    const std::string prompt = req->getParameter("prompt");
                    const std::string messages = req->getParameter("messages");
                    if (!messages.empty())
                        out_body = nlohmann::json::parse(messages);
                    else if (!prompt.empty())
                    {
                        nlohmann::json msg = nlohmann::json::object();
                        msg["role"] = "user";
                        msg["content"] = prompt;
                        out_body = nlohmann::json::object();
                        out_body["messages"] = nlohmann::json::array({msg});
                    }
                    else
                        return "missing_prompt_or_messages";
                }
            }
            catch (const std::exception &e)
            {
                return std::string{"bad_request: "} + e.what();
            }
            if (out_body.is_object())
            {
                if (out_body.contains("session_id") && out_body["session_id"].is_string())
                    out_sid = out_body["session_id"].get<std::string>();
                if (out_body.contains("max_iterations") && out_body["max_iterations"].is_number())
                    out_max_iter = out_body["max_iterations"].get<std::size_t>();
            }
            if (out_sid.empty())
                out_sid = make_trace_id();
            return "";
        }

        std::shared_ptr<IAgentExecutor> m_executor;
        std::string m_auth_token;
        std::shared_ptr<HttpRateLimiter> m_rl =
            std::make_shared<HttpRateLimiter>(60, std::chrono::seconds{60});
        size_t m_threads = 4;
        std::atomic<bool> m_running{false};
        std::thread m_thread;
    };

} // namespace VMX::AE

#endif // VMX_AE_ENABLE_HTTP
