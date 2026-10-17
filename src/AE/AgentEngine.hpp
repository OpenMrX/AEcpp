#pragma once

/// @file AgentEngine.hpp
/// @brief 执行核心薄壳：持有模式上下文 + 内置模式变体，按 Heap 分派委托（Docs/01 §3.4 / §4.2）。
///
/// @details
/// 设计约束（关键）：
/// - **零多态开销**：内置模式经 `std::variant` + `std::visit` 编译期分派，无虚调用、
///   无堆分配间接层；`IReasoningMode` 仅为外部插件保留（非核心路径，不参与内置分发）。
/// - AgentEngine 自身不含推理逻辑：`run()` 复位 `ModeContext` 后委托当前模式 `execute()`。
///   主循环实体在 `Mode/ReactMode.hpp`。
/// - 公共 API（构造 / run / resume / register_hook / set_sanitizer / request_stop /
///   set_compress_*）保持与重构前**完全一致**，保证零回归。
/// - 仅依赖注入抽象（IToolProvider / IContextSource / IPermissionGate / LLMAdapter）
///   + IStateStore（可选），不硬依赖任何宿主。
///
/// @note 所属命名空间：AE::Engine（扁平）。

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "nlohmann-json.hpp"

#include "AEType.hpp"
#include "LLMAdapter.hpp"
#include "AETelemetry.hpp"
#include "AERecordReplay.hpp"
#include "AESanitize.hpp"

#include "Mode/ModeContext.hpp"
#include "Mode/IReasoningMode.hpp"
#include "Mode/ReactMode.hpp"
#include "Mode/PlanExecuteMode.hpp"
#include "Mode/ReflectionMode.hpp"

namespace AE::Engine
{

    class AgentEngine
    {
    public:
        AgentEngine(IToolProvider &tools,
                    IContextSource &ctx,
                    LLMAdapter &llm,
                    IPermissionGate *gate = nullptr,
                    IStateStore *store = nullptr,
                    std::shared_ptr<AETelemetry> telemetry = nullptr,
                    AERunOptions run_opts = {})
            : m_context(tools, ctx, llm, gate, store,
                        telemetry ? std::move(telemetry) : run_opts.telemetry),
              m_mode(std::in_place_type<ReactMode>, std::move(run_opts))
        {
        }

        // ── 上下文软限压缩（会话压缩回调）──
        using CompressCallback = ReactMode::CompressCallback;

        /// 设置压缩触发阈值（0.0~1.0，默认 0.85）。
        auto set_compress_threshold(double threshold) noexcept -> void
        {
            std::visit([&](auto &m)
                       {
                           if constexpr (requires { m.set_compress_threshold(threshold); })
                               m.set_compress_threshold(threshold);
                       },
                       m_mode);
        }

        /// 注入会话压缩回调（由宿主提供 LLM 滚动摘要等能力）。
        auto set_compress_callback(CompressCallback cb) -> void
        {
            std::visit([&](auto &m)
                       {
                           if constexpr (requires { m.set_compress_callback(std::move(cb)); })
                               m.set_compress_callback(std::move(cb));
                       },
                       m_mode);
        }

        // 主入口：流式产出 token 粒度事件。复位上下文后委托当前模式执行。
        auto run(std::string_view session_id,
                 const nlohmann::json &request_body,
                 std::size_t max_iterations = 16)
            -> std::generator<AgentEvent>
        {
            m_context.begin_run(std::string{session_id}, request_body,
                                static_cast<std::uint32_t>(max_iterations));
            auto gen = std::visit([this](auto &m) { return m.execute(m_context); }, m_mode);
            for (auto &&e : gen)
                co_yield std::move(e);
        }

        // 从检查点恢复（主恢复逻辑在模式内统一处理，基于 IStateStore）
        auto resume(std::string_view session_id,
                    const nlohmann::json &request_body,
                    std::size_t /*from_iteration*/ = 0)
            -> std::generator<AgentEvent>
        {
            Core::Logger::getInstance().infoFmt("[AE] resume requested session={}", session_id);
            for (auto &&e : run(session_id, request_body))
                co_yield e;
        }

        // 注册 AOP 钩子，返回句柄
        auto register_hook(HookPoint point, Hook hook) -> std::size_t
        {
            return std::visit([&](auto &m) -> std::size_t
                              {
                                  if constexpr (requires { m.register_hook(point, std::move(hook)); })
                                      return m.register_hook(point, std::move(hook));
                                  else
                                  {
                                      (void)point;
                                      (void)hook;
                                      return 0;
                                  }
                              },
                              m_mode);
        }

        // §4.7 提示注入防护：配置工具输出净化器（限长/去控制字符/注入中和/物理隔离）。
        void set_sanitizer(ToolOutputSanitizer sanitizer)
        {
            std::visit([&](auto &m)
                       {
                           if constexpr (requires { m.set_sanitizer(std::move(sanitizer)); })
                               m.set_sanitizer(std::move(sanitizer));
                       },
                       m_mode);
        }

        // §11 优雅退出：取消进行中的 run()（在迭代边界检测并优雅退出 + 落盘）。
        void request_stop() noexcept
        {
            std::visit([](auto &m)
                       {
                           if constexpr (requires { m.request_stop(); })
                               m.request_stop();
                       },
                       m_mode);
        }

        [[nodiscard]] bool is_stop_requested() const
        {
            return std::visit([](const auto &m) -> bool
                              {
                                  if constexpr (requires { m.is_stop_requested(); })
                                      return m.is_stop_requested();
                                  else
                                      return false;
                              },
                              m_mode);
        }

        // ── 模式访问（诊断 / 测试） ──
        [[nodiscard]] auto mode_kind() const noexcept -> ModeKind
        {
            return std::visit([](const auto &m) noexcept { return m.kind(); }, m_mode);
        }
        [[nodiscard]] auto mode_name() const noexcept -> std::string_view
        {
            return std::visit([](const auto &m) noexcept { return m.name(); }, m_mode);
        }

        [[nodiscard]] auto context() noexcept -> ModeContext & { return m_context; }
        [[nodiscard]] auto context() const noexcept -> const ModeContext & { return m_context; }

        // 把 messages 中最早的若干轮压缩为一条 system 摘要消息（委托内置实现，保持 API 兼容）。
        static void compress_history(std::deque<nlohmann::json> &msgs, const std::string &summary)
        {
            ReactMode::compress_history(msgs, summary);
        }

        // 切换执行模式（内置模式为编译期闭合集合）。
        template <class Mode, class... Args>
        auto emplace_mode(Args &&...args) -> void
        {
            m_mode.template emplace<Mode>(std::forward<Args>(args)...);
        }

    private:
        // 内置模式变体（编译期闭合；新增模式只需扩展此 variant）。
        using ModeVariant = std::variant<ReactMode,
                                         PlanExecuteMode,
                                         ReflectionMode<ReactMode>,
                                         ReflectionMode<PlanExecuteMode>>;

        ModeContext m_context;
        ModeVariant m_mode;
    };

    static_assert(ReasoningMode<ReflectionMode<ReactMode>>,
                  "ReflectionMode<ReactMode> must satisfy the ReasoningMode contract");
    static_assert(ReasoningMode<ReflectionMode<PlanExecuteMode>>,
                  "ReflectionMode<PlanExecuteMode> must satisfy the ReasoningMode contract");

} // namespace AE::Engine
