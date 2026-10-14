#pragma once

/// @file AEChatHelper.hpp
/// @brief 便捷聊天辅助类：简化"单轮/多轮纯聊天"场景的使用，不用手动组装 messages 数组。
///
/// @details
/// - 内部自动管理 VllmAdapter、SimpleContextSource（可选）、AgentEngine 的生命周期
/// - 所有依赖用智能指针或引用管理，无裸 new/delete
/// - 遵循 VMX::AE 命名空间与 Core::Result 错误处理规范
/// - 支持自动从 LM Studio / vLLM 等 OpenAI 兼容 API 探测 context window
/// - 支持本地近似 token 计数（TokenCounter），在 LLM 不返回 usage 时使用
///
/// @note 所属命名空间：VMX::AE
/// @note 设计约束：

#include <deque>
#include <generator>
#include <memory>
#include <string>
#include <string_view>
#include <optional>

#include <nlohmann/json.hpp>

#include "Core/Result.hpp"
#include "AEType.hpp"
#include "IAE.hpp"
#include "LLMAdapter.hpp"
#include "AE.hpp"
#include "StreamingLLMClient.hpp"
#include "TokenCounter.hpp"

namespace VMX::AE
{

    class AEChatHelper
    {
    public:
        explicit AEChatHelper(StreamingLLMClient &client,
                              IToolProvider *tools = nullptr,
                              IContextSource *ctx = nullptr,
                              IPermissionGate *gate = nullptr)
            : m_client(client),
              m_vllmAdapter(std::make_unique<VllmAdapter>(client)),
              m_ownedTools(tools ? nullptr : std::make_unique<SimpleToolProvider>()),
              m_ownedCtx(ctx ? nullptr : std::make_unique<SimpleContextSource>()),
              m_tools(tools ? tools : m_ownedTools.get()),
              m_ctx(ctx ? ctx : m_ownedCtx.get()),
              m_gate(gate),
              m_engine(std::make_unique<AgentEngine>(*m_tools, *m_ctx, *m_vllmAdapter, m_gate))
        {
            m_staticCfg = nlohmann::json::object();
            m_staticCfg["stream"] = true;
        }

        // ─── 消息管理 ───

        auto add_user_message(std::string_view content) -> void
        {
            nlohmann::json msg = nlohmann::json::object();
            msg["role"] = "user";
            msg["content"] = std::string{content};
            m_messages.push_back(std::move(msg));
        }

        auto add_assistant_message(std::string_view content) -> void
        {
            nlohmann::json msg = nlohmann::json::object();
            msg["role"] = "assistant";
            msg["content"] = std::string{content};
            m_messages.push_back(std::move(msg));
        }

        auto add_system_message(std::string_view content) -> void
        {
            nlohmann::json msg = nlohmann::json::object();
            msg["role"] = "system";
            msg["content"] = std::string{content};
            m_messages.push_front(std::move(msg));
        }

        auto clear_history() -> void
        {
            m_messages.clear();
            if (m_ownedCtx)
                m_ownedCtx->reset();
        }

        auto get_history() const -> const std::deque<nlohmann::json> &
        {
            return m_messages;
        }

        // ─── 生成回复 ───

        auto generate_response() -> Core::Result<std::string>
        {
            nlohmann::json body = build_request_body();
            std::string final_text;
            bool has_error = false;
            Core::Error last_error;
            bool got_usage = false;

            for (auto &&event : m_engine->run("aec", body, m_maxIterations))
            {
                switch (event.kind)
                {
                case EventKind::Final:
                    final_text = event.payload;
                    break;
                case EventKind::Error:
                    has_error = true;
                    last_error = event.error_info;
                    break;
                default:
                    break;
                }
            }

            if (has_error)
                return Core::Result<std::string>::Error(std::move(last_error));

            // 如果 LLM 没返回 usage（token 计数没变），用本地估算补充
            if (!got_usage && m_ownedCtx && m_ownedCtx->token_count() == 0)
            {
                std::size_t estimated = TokenCounter::estimate_messages_tokens(m_messages);
                if (m_ownedCtx)
                    m_ownedCtx->add_tokens(estimated);
            }

            nlohmann::json msg = nlohmann::json::object();
            msg["role"] = "assistant";
            msg["content"] = final_text;
            m_messages.push_back(std::move(msg));

            return Core::Result<std::string>::Ok(std::move(final_text));
        }

        auto generate_stream() -> std::generator<std::string>
        {
            nlohmann::json body = build_request_body();
            std::string final_text;
            bool has_error = false;
            std::size_t tokens_before = m_ctx ? m_ctx->token_count() : 0;

            for (auto &&event : m_engine->run("aec", body, m_maxIterations))
            {
                switch (event.kind)
                {
                case EventKind::Token:
                    final_text += event.payload;
                    co_yield event.payload;
                    break;
                case EventKind::Error:
                    has_error = true;
                    break;
                case EventKind::Final:
                    break;
                default:
                    break;
                }
            }

            if (!has_error)
            {
                nlohmann::json msg = nlohmann::json::object();
                msg["role"] = "assistant";
                msg["content"] = final_text;
                m_messages.push_back(std::move(msg));

                // 如果 LLM 没返回 usage，用本地估算补充
                if (m_ctx && m_ctx->token_count() == tokens_before)
                {
                    std::size_t estimated = TokenCounter::estimate_tokens(final_text);
                    if (m_ownedCtx)
                        m_ownedCtx->add_tokens(estimated);
                }
            }
        }

        // ─── 配置 ───

        auto set_model(std::string_view model) -> void
        {
            m_staticCfg["model"] = std::string{model};
            m_modelName = std::string{model};
            m_modelProbed = false;
        }

        auto set_temperature(float t) -> void
        {
            m_staticCfg["temperature"] = t;
        }

        auto set_max_tokens(int n) -> void
        {
            m_staticCfg["max_tokens"] = n;
            m_manualMaxTokens = true;
        }

        auto set_max_iterations(std::size_t n) -> void
        {
            m_maxIterations = n;
        }

        // ─── Context Window 自动探测 ───

        // 从 LLM 服务探测模型的 context window 大小
        // 成功后自动设置到 ContextSource，并据此调整 max_tokens
        auto probe_context_window() -> Core::Result<std::size_t>
        {
            if (m_modelName.empty())
                return Core::Result<std::size_t>::Error(
                    Core::Error{"probe_context_window: model name not set"});

            auto info = TokenCounter::probe_model_info(
                m_client.base_url(), m_modelName, m_client.api_key());

            if (info.isError())
                return Core::Result<std::size_t>::Error(info.error());

            const auto &model = info.value();
            m_modelInfo = model;

            // 设置到 ContextSource
            if (m_ownedCtx)
                m_ownedCtx->set_hard_limit(model.context_window);

            // 自动调整 max_tokens（如果用户没手动设置）
            if (!m_manualMaxTokens)
            {
                m_staticCfg["max_tokens"] = static_cast<int>(model.max_completion_tokens);
            }

            m_modelProbed = true;
            return Core::Result<std::size_t>::Ok(model.context_window);
        }

        auto is_model_probed() const -> bool { return m_modelProbed; }
        auto model_info() const -> const std::optional<TokenCounter::ModelInfo> & { return m_modelInfo; }

        // ─── Token 统计 ───

        // 当前已使用的 token 数（来自 LLM usage 或本地估算）
        auto tokens_used() const -> std::size_t
        {
            return m_ctx ? m_ctx->token_count() : 0;
        }

        // context window 大小（硬限）
        auto context_window() const -> std::size_t
        {
            return m_ctx ? m_ctx->hard_limit() : 0;
        }

        // 剩余可用 token 数
        auto tokens_remaining() const -> std::size_t
        {
            if (!m_ownedCtx)
                return 0;
            return m_ownedCtx->remaining();
        }

        // 使用率 0.0 ~ 1.0
        auto token_usage_ratio() const -> double
        {
            if (!m_ownedCtx)
                return 0.0;
            return m_ownedCtx->usage_ratio();
        }

        // 本地估算当前消息历史的 token 数
        auto estimate_current_tokens() const -> std::size_t
        {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto &m : m_messages)
                arr.push_back(m);
            return TokenCounter::estimate_messages_tokens(arr);
        }

        // ─── 底层访问 ───

        auto get_engine() -> AgentEngine & { return *m_engine; }
        auto context_source() -> IContextSource * { return m_ctx; }
        auto simple_context_source() -> SimpleContextSource * { return m_ownedCtx.get(); }

    private:
        auto build_request_body() const -> nlohmann::json
        {
            nlohmann::json body = m_staticCfg;
            body["messages"] = nlohmann::json::array();
            for (const auto &msg : m_messages)
                body["messages"].push_back(msg);
            return body;
        }

        StreamingLLMClient &m_client;
        std::unique_ptr<VllmAdapter> m_vllmAdapter;
        std::unique_ptr<SimpleToolProvider> m_ownedTools;
        std::unique_ptr<SimpleContextSource> m_ownedCtx;
        IToolProvider *m_tools;
        IContextSource *m_ctx;
        IPermissionGate *m_gate;
        std::unique_ptr<AgentEngine> m_engine;

        std::deque<nlohmann::json> m_messages;
        nlohmann::json m_staticCfg;
        std::size_t m_maxIterations = 16;

        std::string m_modelName;
        bool m_manualMaxTokens = false;
        bool m_modelProbed = false;
        std::optional<TokenCounter::ModelInfo> m_modelInfo;
    };

} // namespace VMX::AE
