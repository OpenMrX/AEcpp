#pragma once

/// @file AEShutdown.hpp
/// @brief 优雅退出信号助手（§11 可选、按需安装）。
///
/// @details
/// 设计纪律（关键）：
/// - AE::Engine 核心【不自动】安装全局信号处理器，避免与宿主（HTTP 服务 /
/// 进程主循环）的信号处理冲突，也避免把信号语义强加给库消费者。
/// - 仅暴露「取消入口」：AgentEngine::request_stop() / is_stop_requested()；
/// 是否监听 SIGINT/SIGTERM、如何把信号映射到取消，由宿主决定。
/// - 本助手以 RAII 形式提供「一键安装/卸载」SIGINT/SIGTERM 处理器，供需要
/// 7×24 独立运行的部署入口（CLI main / serve()）按需使用。
///
/// 行为：
/// - 首次信号：置 was_requested()=true，并（若提供回调）调用回调（典型为
/// engine.request_stop()）；随后重新安装自身（部分平台信号触发后复位默认处理器）。
/// - 第二次信号：恢复默认处理器并 std::raise，允许宿主被强制终止（防卡死）。
/// - 析构恢复先前处理器，撤销安装。
///
/// 信号安全说明：处理器内调用回调（engine.request_stop()）并非严格 async-signal-safe；
/// 对绝大多数部署可接受。若需严格安全，宿主应在主循环轮询 GracefulShutdown::
/// was_requested() 并自行调用 engine.request_stop()，而非依赖回调的异步调用。
///
/// @note 所属命名空间：AE::Engine

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <functional>

namespace AE::Engine
{
    class GracefulShutdown
    {
    public:
        using Callback = std::function<void()>;

        explicit GracefulShutdown(Callback cb = {}) : m_cb(std::move(cb))
        {
            install();
        }

        ~GracefulShutdown()
        {
            restore();
        }

        GracefulShutdown(const GracefulShutdown &) = delete;
        GracefulShutdown &operator=(const GracefulShutdown &) = delete;

        // 是否有信号已请求退出（跨线程可见）。宿主主循环可轮询此标志。
        static bool was_requested() noexcept
        {
            return g_requested.load(std::memory_order_acquire);
        }

    private:
        void install()
        {
            g_active = this;
            g_requested.store(false, std::memory_order_release);
            g_count.store(0, std::memory_order_release);
            m_prev.sigint = std::signal(SIGINT, ae_signal_handler);
            if (m_prev.sigint == SIG_ERR)
                m_prev.sigint = SIG_DFL;
            m_prev.sigterm = std::signal(SIGTERM, ae_signal_handler);
            if (m_prev.sigterm == SIG_ERR)
                m_prev.sigterm = SIG_DFL;
        }

        void restore()
        {
            if (g_active == this)
            {
                std::signal(SIGINT, m_prev.sigint);
                std::signal(SIGTERM, m_prev.sigterm);
                g_active = nullptr;
            }
        }

        struct PrevHandlers
        {
            void (*sigint)(int) = SIG_DFL;
            void (*sigterm)(int) = SIG_DFL;
        } m_prev{};

        Callback m_cb;

        static std::atomic<bool> g_requested;
        static std::atomic<int> g_count;
        static GracefulShutdown *g_active;

        static void ae_signal_handler(int signum)
        {
            const int n = ++g_count;
            if (n >= 2)
            {
                // 第二次信号：恢复默认行为并重新触发，允许强制终止（防卡死）。
                std::signal(SIGINT, SIG_DFL);
                std::signal(SIGTERM, SIG_DFL);
                std::raise(signum);
                return;
            }
            g_requested.store(true, std::memory_order_release);
            GracefulShutdown *g = g_active;
            if (g && g->m_cb)
            {
                try
                {
                    g->m_cb();
                }
                catch (...)
                {
                }
            }
            // 部分平台在信号处理后复位为默认处理器，需重新安装自身。
            std::signal(SIGINT, ae_signal_handler);
            std::signal(SIGTERM, ae_signal_handler);
        }
    };

    inline std::atomic<bool> GracefulShutdown::g_requested{false};
    inline std::atomic<int> GracefulShutdown::g_count{0};
    inline GracefulShutdown *GracefulShutdown::g_active{nullptr};

} // namespace AE::Engine
