#pragma once

/// @file Core/Net/SseParser.hpp
/// @brief 手写 SSE（Server-Sent Events）增量解析器（替代 cpr::ServerSentEventParser）。
/// @details
/// 以字节流方式喂入原始 HTTP 响应块，按 SSE 规范（空行分隔事件、'data:'/'event:'/'id:' 字段、
/// ':' 开头为注释）切分，逐事件回调。兼容被 chunk 边界切断的不完整行/事件。
/// 语义对齐 cpr::ServerSentEventParser：每个事件回调给出 (event, data)。
///
/// @note 头文件-only，无链接依赖。命名空间：AE::Core::Net。

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace AE::Core::Net
{

    /// SSE 增量解析器：feed() 多次调用，逐完整事件触发 cb(event, data)。
    /// cb 返回 false 表示中止（由上层传播为“停止流式传输”）。
    class SseParser
    {
    public:
        using EventCallback = std::function<bool(std::string_view event, std::string_view data)>;

        auto feed(std::string_view chunk, EventCallback cb) -> bool
        {
            m_buf.append(chunk.data(), chunk.size());

            std::size_t start = 0;
            while (true)
            {
                const auto nl = m_buf.find('\n', start);
                if (nl == std::string::npos)
                {
                    // 保留未完成的行（无换行符的尾部）
                    if (start > 0)
                        m_buf.erase(0, start);
                    break;
                }
                std::string_view line{m_buf.data() + start, nl - start};
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);

                if (!processLine(line, cb))
                    return false;

                start = nl + 1;
            }
            return true;
        }

    private:
        auto processLine(std::string_view line, EventCallback &cb) -> bool
        {
            // 空行 = 事件分隔符，触发派发
            if (line.empty())
                return dispatch(cb);

            // 注释行（以 ':' 开头）忽略
            if (line.front() == ':')
                return true;

            std::string_view field;
            std::string_view value;
            const auto colon = line.find(':');
            if (colon == std::string_view::npos)
            {
                field = line;
            }
            else
            {
                field = line.substr(0, colon);
                value = line.substr(colon + 1);
                if (!value.empty() && value.front() == ' ')
                    value.remove_prefix(1);
            }

            if (field == "data")
            {
                m_data += value;
                m_data.push_back('\n'); // 多 data 行以 \n 拼接（对齐 cpr 行为）
            }
            else if (field == "event")
            {
                m_event = value;
            }
            else if (field == "id")
            {
                m_id = value;
            }
            // 其余字段（retry 等）忽略
            return true;
        }

        auto dispatch(EventCallback &cb) -> bool
        {
            // 既无 data 也无 event 的空事件，跳过
            if (m_data.empty() && m_event.empty())
            {
                reset();
                return true;
            }
            if (!m_data.empty() && m_data.back() == '\n')
                m_data.pop_back();

            const bool cont = cb(m_event, m_data);
            reset();
            return cont;
        }

        auto reset() -> void
        {
            m_data.clear();
            m_event.clear();
            m_id.clear();
        }

        std::string m_buf;  // 跨 feed 调用的行缓冲
        std::string m_data; // 当前事件累积的 data
        std::string m_event;
        std::string m_id;
    };

} // namespace AE::Core::Net
