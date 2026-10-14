#pragma once

/// @file AERecordReplay.hpp
/// @brief Record/Replay 录制回放层（对应 AgentEngine.md §4.6「Record/Replay（HAR 式）」）。
///
/// @details
/// 设计约束（关键）：
/// - 纯 AE::Engine 模块；仅依赖 Core::Result/Logger、AEType、AETelemetry，无额外第三方依赖。
/// - 录制：把每次 run() 的「请求 → 各 LLM 原始响应（SSE 分块归一后的 deltas）→
/// 工具调用与结果 → 最终产出」以 HAR 式 JSONL 落盘（每行一个事件）。密钥脱敏、
/// prompt 可选脱敏（默认开启）。
/// - 回放：ReplayController 读取录制文件，跳过真实 LLM/工具调用，直接回放事件流——
/// 用于回归测试、问题复现、对齐校验（Shadow Testing）、离线调试。
/// - 与 §4.8 Checkpointing 互补：录制是「会话全过程轨迹」，checkpoint 是「可恢复状态快照」。
/// - 注入方式：AERunOptions（ReplayMode + 路径 + 遥测引用）经 AgentEngine 构造函数传入，
/// run() 在 Replay 模式直接消费录制事件，不进入真实网络/工具执行。
///
/// @note 所属命名空间：AE::Engine

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Result.hpp"
#include "Core/Logger.hpp"
#include "AEType.hpp"
#include "AETelemetry.hpp"

namespace AE::Engine
{

    enum class ReplayMode
    {
        Off,    // 正常执行
        Record, // 执行并录制
        Replay  // 跳过真实调用，回放录制
    };

    // run() 级别的录制/回放/遥测注入选项（经 AgentEngine 构造函数传入）
    struct AERunOptions
    {
        ReplayMode replay = ReplayMode::Off;
        std::string record_path; // Record 模式：输出 JSONL 路径
        std::string replay_path; // Replay 模式：输入 JSONL 路径
        bool redact_prompt = true; // 脱敏 prompt 全文（只留长度），默认开启
        std::shared_ptr<AETelemetry> telemetry; // 可选：共享遥测门面
        std::chrono::seconds time_limit{0}; // 0 = 不限制（总时长预算）
    };

    // ---------------------------------------------------- 密钥脱敏
    // 递归扫描 JSON，将 secret 类键置为 <redacted>；可选脱敏 messages[].content 为长度标记。
    inline auto redact_json(const nlohmann::json &in, bool redact_prompt) -> nlohmann::json
    {
        static constexpr std::string_view kSecret[] = {
            "api_key", "apikey", "password", "passwd", "token", "secret",
            "authorization", "bearer", "cookie", "access_key", "private_key"};
        const auto is_secret = [](std::string_view k) -> bool
        {
            for (auto s : kSecret)
                if (k.size() == s.size() &&
                    std::equal(k.begin(), k.end(), s.begin(),
                               [](char a, char b) { return std::tolower(a) == std::tolower(b); }))
                    return true;
            return false;
        };

        if (in.is_object())
        {
            nlohmann::json out = nlohmann::json::object();
            for (auto it = in.begin(); it != in.end(); ++it)
            {
                const std::string key = it.key();
                if (is_secret(key))
                {
                    out[key] = "<redacted>";
                    continue;
                }
                if (redact_prompt && key == "messages" && it.value().is_array())
                {
                    nlohmann::json arr = nlohmann::json::array();
                    for (const auto &m : it.value())
                    {
                        nlohmann::json mm = m;
                        if (mm.is_object() && mm.contains("content") && mm["content"].is_string())
                        {
                            const std::string c = mm["content"].get<std::string>();
                            mm["content"] = nlohmann::json::object(
                                {{"__redacted__", true}, {"len", c.size()}});
                        }
                        arr.push_back(std::move(mm));
                    }
                    out[key] = std::move(arr);
                    continue;
                }
                out[key] = redact_json(it.value(), redact_prompt);
            }
            return out;
        }
        if (in.is_array())
        {
            nlohmann::json out = nlohmann::json::array();
            for (const auto &e : in)
                out.push_back(redact_json(e, redact_prompt));
            return out;
        }
        return in;
    }

    // ---------------------------------------------------- deltas <-> JSON（回放重建用）
    inline auto deltas_to_json(const std::vector<LlmDelta> &deltas) -> nlohmann::json
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &d : deltas)
        {
            switch (d.kind)
            {
            case LlmDelta::Kind::Text:
                arr.push_back(nlohmann::json::object(
                    {{"k", "text"}, {"t", d.text_content}}));
                break;
            case LlmDelta::Kind::Thinking:
                arr.push_back(nlohmann::json::object(
                    {{"k", "thinking"}, {"t", d.thinking_content}}));
                break;
            case LlmDelta::Kind::ToolCall:
                if (d.tool_call_info)
                    arr.push_back(nlohmann::json::object(
                        {{"k", "tool"},
                         {"id", d.tool_call_info->id},
                         {"name", d.tool_call_info->name},
                         {"args", d.tool_call_info->arguments}}));
                break;
            case LlmDelta::Kind::Usage:
                if (d.usage_info)
                    arr.push_back(nlohmann::json::object(
                        {{"k", "usage"},
                         {"p", d.usage_info->prompt_tokens},
                         {"c", d.usage_info->completion_tokens}}));
                break;
            case LlmDelta::Kind::Error:
                if (!d.error_message.empty())
                    arr.push_back(nlohmann::json::object(
                        {{"k", "error"}, {"m", d.error_message}}));
                break;
            case LlmDelta::Kind::Done:
                arr.push_back(nlohmann::json::object({{"k", "done"}}));
                break;
            }
        }
        return arr;
    }

    inline auto json_to_deltas(const nlohmann::json &arr) -> std::vector<LlmDelta>
    {
        std::vector<LlmDelta> out;
        if (!arr.is_array())
            return out;
        for (const auto &e : arr)
        {
            const std::string k = e.value("k", "");
            if (k == "text")
                out.push_back(LlmDelta::text(e.value("t", "")));
            else if (k == "thinking")
                out.push_back(LlmDelta::thinking(e.value("t", "")));
            else if (k == "tool")
            {
                nlohmann::json a = e.contains("args") && e["args"].is_object()
                                       ? e["args"]
                                       : nlohmann::json::object();
                out.push_back(LlmDelta::tool_call(e.value("id", ""), e.value("name", ""), std::move(a)));
            }
            else if (k == "usage")
                out.push_back(LlmDelta::usage(e.value("p", std::size_t{0}), e.value("c", std::size_t{0})));
            else if (k == "error")
                out.push_back(LlmDelta::error(e.value("m", "")));
            else if (k == "done")
                out.push_back(LlmDelta::done());
        }
        return out;
    }

    // ---------------------------------------------------- RecordingWriter（HAR 式 JSONL）
    class RecordingWriter
    {
    public:
        RecordingWriter(std::string path, bool redact_prompt)
            : m_path(std::move(path)), m_redact(redact_prompt)
        {
            std::error_code ec;
            const std::filesystem::path p{m_path};
            std::filesystem::create_directories(p.parent_path(), ec);
            m_out.open(m_path, std::ios::out | std::ios::app);
            if (!m_out)
                Core::Logger::getInstance().errorFmt(
                    "[AE:Replay] 录制文件无法打开: {}", m_path);
        }

        auto record_llm(std::string_view trace_id, const nlohmann::json &request,
                        const Core::Result<std::vector<LlmDelta>> &turn,
                        std::chrono::microseconds dur) -> void
        {
            nlohmann::json entry = nlohmann::json::object();
            entry["seq"] = m_seq++;
            entry["ts"] = now_iso();
            entry["kind"] = "llm";
            entry["trace_id"] = std::string{trace_id};
            entry["request"] = redact_json(request, m_redact);
            entry["response"] = turn.isSuccess() ? deltas_to_json(turn.value())
                                                  : nlohmann::json::array();
            entry["dur_us"] = dur.count();
            entry["ok"] = turn.isSuccess();
            entry["status"] = turn.isError() ? turn.error().message() : "ok";
            append_line(entry);
        }

        auto record_tool(std::string_view trace_id, std::string_view name,
                         const nlohmann::json &args,
                         const Core::Result<nlohmann::json> &res,
                         std::chrono::microseconds dur) -> void
        {
            nlohmann::json entry = nlohmann::json::object();
            entry["seq"] = m_seq++;
            entry["ts"] = now_iso();
            entry["kind"] = "tool";
            entry["trace_id"] = std::string{trace_id};
            entry["name"] = std::string{name};
            entry["args"] = redact_json(args, false);
            if (res.isSuccess())
                entry["result"] = res.value();
            else
                entry["result"] = nlohmann::json::object({{"error", res.error().message()}});
            entry["dur_us"] = dur.count();
            entry["ok"] = res.isSuccess();
            entry["status"] = res.isError() ? res.error().message() : "ok";
            append_line(entry);
        }

        auto record_event(std::string_view trace_id, std::string_view kind,
                          std::string_view name, std::string_view status) -> void
        {
            nlohmann::json entry = nlohmann::json::object();
            entry["seq"] = m_seq++;
            entry["ts"] = now_iso();
            entry["kind"] = std::string{kind};
            entry["trace_id"] = std::string{trace_id};
            entry["name"] = std::string{name};
            entry["status"] = std::string{status};
            append_line(entry);
        }

    private:
        auto now_iso() const -> std::string
        {
            const auto t = std::chrono::system_clock::now();
            const auto tt = std::chrono::system_clock::to_time_t(t);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&tt));
            return std::string{buf};
        }
        auto append_line(const nlohmann::json &entry) -> void
        {
            std::lock_guard<std::mutex> lk{m_mutex};
            if (!m_out)
                return;
            m_out << entry.dump() << "\n";
            m_out.flush();
        }

        std::string m_path;
        bool m_redact;
        std::ofstream m_out;
        std::mutex m_mutex;
        std::size_t m_seq = 0;
    };

    // ---------------------------------------------------- ReplayController（回放事件流）
    class ReplayController
    {
    public:
        explicit ReplayController(std::string path)
        {
            std::ifstream in{path, std::ios::binary};
            if (!in)
            {
                Core::Logger::getInstance().errorFmt("[AE:Replay] 回放文件无法打开: {}", path);
                return;
            }
            std::string line;
            while (std::getline(in, line))
            {
                if (line.empty())
                    continue;
                try
                {
                    m_events.push_back(nlohmann::json::parse(line));
                }
                catch (...)
                {
                    // 跳过损坏行
                }
            }
            if (m_events.empty())
                Core::Logger::getInstance().warningFmt("[AE:Replay] 回放文件为空或全损坏: {}", path);
        }

        [[nodiscard]] auto valid() const -> bool { return !m_events.empty(); }

        // 取下一个 LLM 录制事件并重建 deltas（顺序消费，和录制顺序一致）
        auto next_llm_turn() -> Core::Result<std::vector<LlmDelta>>
        {
            while (m_idx < m_events.size())
            {
                const nlohmann::json &ev = m_events[m_idx++];
                if (ev.value("kind", "") != "llm")
                    continue;
                return Core::Result<std::vector<LlmDelta>>::Ok(
                    json_to_deltas(ev.value("response", nlohmann::json::array())));
            }
            return Core::Result<std::vector<LlmDelta>>::Error(
                Core::Error{"Replay: no more llm turns"});
        }

        // 取下一个工具录制事件的结果（顺序消费）
        auto next_tool_result(std::string_view /*name*/, const nlohmann::json & /*args*/)
            -> Core::Result<nlohmann::json>
        {
            while (m_idx < m_events.size())
            {
                const nlohmann::json &ev = m_events[m_idx++];
                if (ev.value("kind", "") != "tool")
                    continue;
                const nlohmann::json &res = ev["result"];
                if (ev.value("ok", true) && res.is_object() && !res.contains("error"))
                    return Core::Result<nlohmann::json>::Ok(res);
                const std::string msg = res.contains("error") && res["error"].is_string()
                                            ? res["error"].get<std::string>()
                                            : "Replay tool error";
                return Core::Result<nlohmann::json>::Error(Core::Error{msg});
            }
            return Core::Result<nlohmann::json>::Error(
                Core::Error{"Replay: no more tool results"});
        }

    private:
        std::vector<nlohmann::json> m_events;
        std::size_t m_idx = 0;
    };

} // namespace AE::Engine
