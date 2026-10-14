#pragma once

/// @file LLMAdapter.hpp
/// @brief LLM 多后端适配层（基类 + vLLM/Ollama 实现）。
///
/// @details
/// 设计约束（关键）：
/// - LlmDelta 即统一中间表示 StreamEvent；各后端把自家原始流归一化为 LlmDelta，
/// 上层 AgentEngine 只消费 IR，完全不感知后端差异。
///
/// @note 所属命名空间：VMX::AE
/// @note 依赖：Core::Result、nlohmann::json、generator、AEType.hpp、StreamingLLMClient.hpp。

#include <generator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Core/Result.hpp"
#include "AEType.hpp"
#include "StreamingLLMClient.hpp"

namespace VMX::AE
{

    // ------------------------------------------- LLM 适配层（多后端归一化）
    // LlmDelta 即统一中间表示 StreamEvent；各后端把自家原始流归一化为 LlmDelta，
    // 上层 AgentEngine 只消费 IR，完全不感知后端差异。
    class LLMAdapter
    {
    public:
        virtual ~LLMAdapter() = default;
        // 把统一工具 schema 翻译成该后端请求体（含 tool_choice 策略）
        virtual auto normalize_request(const nlohmann::json &tools_schema,
                                       bool force_tool = false) -> nlohmann::json = 0;
        // 归一化整轮产出（带类型化错误），供底层实现用
        virtual auto stream_chat(const nlohmann::json &request_body)
            -> Core::Result<std::vector<LlmDelta>> = 0;
        // 便捷流式接口（默认基于 stream_chat 实现），供上层消费用
        virtual auto stream(const nlohmann::json &request_body) -> std::generator<LlmDelta>
        {
            auto r = stream_chat(request_body);
            if (r.isError())
                co_return;
            for (auto &d : r.value())
                co_yield std::move(d);
        }
    };

    // vLLM / OpenAI 兼容：直接用既有 StreamingLLMClient
    class VllmAdapter : public LLMAdapter
    {
        StreamingLLMClient &m_client;

    public:
        explicit VllmAdapter(StreamingLLMClient &c) : m_client(c) {}
        auto normalize_request(const nlohmann::json &tools_schema, bool force_tool)
            -> nlohmann::json override
        {
            nlohmann::json body = nlohmann::json::object();
            body["tools"] = tools_schema;                           // OpenAI/vLLM 兼容结构
            body["tool_choice"] = force_tool ? "required" : "auto"; // vLLM tool_choice 兼容坑在此归一化
            return body;
        }
        auto stream_chat(const nlohmann::json &request_body)
            -> Core::Result<std::vector<LlmDelta>> override
        {
            return m_client.stream_chat(request_body);
        }
        // 真·增量流式：逐 delta 实时上发（HTTP 在背景线程进行）。
        auto stream(const nlohmann::json &request_body) -> std::generator<LlmDelta> override
        {
            return m_client.stream_chat_gen(request_body);
        }
    };

    // Ollama：非流式工具调用时本地缓冲直至 done 再 emit（流式不返回结构化 tool_calls）
    class OllamaAdapter : public LLMAdapter
    {
        StreamingLLMClient &m_client;

    public:
        explicit OllamaAdapter(StreamingLLMClient &c) : m_client(c) {}
        auto normalize_request(const nlohmann::json &tools_schema, bool /*force_tool*/)
            -> nlohmann::json override
        {
            nlohmann::json body = nlohmann::json::object();
            body["tools"] = tools_schema;
            body["stream"] = false; // Ollama 在 non-stream 下才返回完整 tool_calls
            return body;
        }
        auto stream_chat(const nlohmann::json &request_body)
            -> Core::Result<std::vector<LlmDelta>> override
        {
            return m_client.stream_chat(request_body);
        }
        // Ollama 非流式请求经客户端返回后，也逐 delta 上发（保持上层消费一致）。
        auto stream(const nlohmann::json &request_body) -> std::generator<LlmDelta> override
        {
            return m_client.stream_chat_gen(request_body);
        }
    };

} // namespace VMX::AE
