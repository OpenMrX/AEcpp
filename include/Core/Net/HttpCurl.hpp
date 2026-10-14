#pragma once

/// @file Core/Net/HttpCurl.hpp
/// @brief 基于 libcurl 的同步 HTTP 客户端工具（替代 cpr）。
/// @details
/// 提供阻塞式请求 curlPerform 与流式请求 curlStream（逐原始块回调 onData）。
/// 头文件-only，内联实现；使用方链接目标需依赖 CURL::libcurl。
/// libcurl 为 VXAE 当前唯一 vcpkg 依赖（去留待定，替代候选见 plan.md §8.2），
/// 由本文件统一承载出向 HTTP 客户端；替换传输层时仅重写本文件、接口签名不变。
///
/// @note 命名空间：AE::Core::Net。

#include <curl/curl.h>

#include <string>
#include <string_view>
#include <map>
#include <functional>
#include <mutex>
#include <cstddef>

#include "Core/Result.hpp"
#include "Core/Error.hpp"

namespace AE::Core::Net
{

    /// 出向 HTTP 请求描述（方法 / URL / 头 / 体 / 超时 / TLS 校验）。
    struct CurlRequest
    {
        std::string method = "GET";
        std::string url;
        std::map<std::string, std::string> headers;
        std::string body;
        long timeoutMs = 30000;
        long connectTimeoutMs = 10000;
        bool verifySsl = true;
    };

    /// 完整响应（状态码 / 响应体 / 响应头）。
    struct CurlResponse
    {
        long statusCode = 0;
        std::string body;
        std::map<std::string, std::string> headers;
    };

    namespace detail
    {

        /// 线程安全的 libcurl 全局初始化（单次）。
        inline void curlEnsureGlobalInit()
        {
            static std::once_flag flag;
            std::call_once(flag, [] { ::curl_global_init(CURL_GLOBAL_ALL); });
        }

        /// RAII 封装 CURL* 与 curl_slist*，析构统一清理。
        struct CurlSession
        {
            CURL *easy = ::curl_easy_init();
            curl_slist *list = nullptr;

            CurlSession() = default;
            CurlSession(const CurlSession &) = delete;
            auto operator=(const CurlSession &) -> CurlSession & = delete;
            CurlSession(CurlSession &&) = delete;
            auto operator=(CurlSession &&) -> CurlSession & = delete;

            ~CurlSession()
            {
                if (list != nullptr)
                    ::curl_slist_free_all(list);
                if (easy != nullptr)
                    ::curl_easy_cleanup(easy);
            }
        };

        /// 累积写回调：把响应体追加到 std::string。
        inline auto accumulateWrite(char *ptr, std::size_t sz, std::size_t nmemb, void *ud) -> std::size_t
        {
            auto *buf = static_cast<std::string *>(ud);
            const std::size_t total = sz * nmemb;
            buf->append(ptr, total);
            return total;
        }

        /// 响应头写回调：跳过状态行，按原始大小写写入 map。
        inline auto headerWrite(char *ptr, std::size_t sz, std::size_t nmemb, void *ud) -> std::size_t
        {
            auto *out = static_cast<std::map<std::string, std::string> *>(ud);
            const std::size_t total = sz * nmemb;
            std::string_view line(ptr, total);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.remove_suffix(1);
            if (line.empty())
                return total;
            // 跳过状态行（"HTTP/1.1 200 OK"）
            if (line.size() >= 5 && line.substr(0, 5) == "HTTP/")
                return total;
            const auto colon = line.find(':');
            if (colon == std::string_view::npos)
                return total;
            std::string_view key = line.substr(0, colon);
            std::string_view val = line.substr(colon + 1);
            if (!val.empty() && val.front() == ' ')
                val.remove_prefix(1);
            (*out)[std::string(key)] = std::string(val);
            return total;
        }

        /// 应用通用选项（URL / 方法 / 体 / 头 / 超时 / TLS）。
        inline void applyCommon(CurlSession &s, const CurlRequest &req)
        {
            ::curl_easy_setopt(s.easy, CURLOPT_URL, req.url.c_str());
            ::curl_easy_setopt(s.easy, CURLOPT_CUSTOMREQUEST, req.method.c_str());
            if (!req.body.empty())
            {
                ::curl_easy_setopt(s.easy, CURLOPT_POSTFIELDS, req.body.data());
                ::curl_easy_setopt(s.easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
            }
            for (const auto &[k, v] : req.headers)
                s.list = ::curl_slist_append(s.list, (k + ": " + v).c_str());
            if (s.list != nullptr)
                ::curl_easy_setopt(s.easy, CURLOPT_HTTPHEADER, s.list);

            ::curl_easy_setopt(s.easy, CURLOPT_TIMEOUT_MS, req.timeoutMs);
            ::curl_easy_setopt(s.easy, CURLOPT_CONNECTTIMEOUT_MS, req.connectTimeoutMs);
            ::curl_easy_setopt(s.easy, CURLOPT_SSL_VERIFYPEER, req.verifySsl ? 1L : 0L);
            ::curl_easy_setopt(s.easy, CURLOPT_SSL_VERIFYHOST, req.verifySsl ? 2L : 0L);
        }

    } // namespace detail

    /// 阻塞式执行请求，返回完整响应体。
    inline auto curlPerform(const CurlRequest &req) -> Core::Result<CurlResponse>
    {
        detail::curlEnsureGlobalInit();
        detail::CurlSession s;
        if (s.easy == nullptr)
            return Core::Result<CurlResponse>::Error(
                Core::Error(Core::ErrorCode::NetworkError, "curl_easy_init failed"));

        CurlResponse out;
        detail::applyCommon(s, req);
        ::curl_easy_setopt(s.easy, CURLOPT_WRITEFUNCTION, detail::accumulateWrite);
        ::curl_easy_setopt(s.easy, CURLOPT_WRITEDATA, &out.body);
        ::curl_easy_setopt(s.easy, CURLOPT_HEADERFUNCTION, detail::headerWrite);
        ::curl_easy_setopt(s.easy, CURLOPT_HEADERDATA, &out.headers);

        const CURLcode code = ::curl_easy_perform(s.easy);
        if (code != CURLE_OK)
        {
            return Core::Result<CurlResponse>::Error(
                Core::Error(Core::ErrorCode::NetworkError,
                            std::string("curl_easy_perform failed: ") + ::curl_easy_strerror(code)));
        }
        long status = 0;
        ::curl_easy_getinfo(s.easy, CURLINFO_RESPONSE_CODE, &status);
        out.statusCode = status;
        return Core::Result<CurlResponse>::Ok(out);
    }

    /// 流式执行：逐原始块回调 onData；onData 返回 false 即中止传输（视为正常终止）。
    inline auto curlStream(const CurlRequest &req,
                           const std::function<bool(std::string_view)> &onData)
        -> Core::Result<CurlResponse>
    {
        detail::curlEnsureGlobalInit();
        detail::CurlSession s;
        if (s.easy == nullptr)
            return Core::Result<CurlResponse>::Error(
                Core::Error(Core::ErrorCode::NetworkError, "curl_easy_init failed"));

        CurlResponse out;
        struct Ctx
        {
            const std::function<bool(std::string_view)> *cb = nullptr;
            bool aborted = false;
        } ctx;
        ctx.cb = &onData;

        auto streamWrite = [](char *ptr, std::size_t sz, std::size_t nmemb, void *ud) -> std::size_t
        {
            auto *c = static_cast<Ctx *>(ud);
            const std::size_t total = sz * nmemb;
            if (c->aborted)
                return 0;
            if (c->cb != nullptr && !(*c->cb)(std::string_view(ptr, total)))
            {
                c->aborted = true;
                return 0;
            }
            return total;
        };

        detail::applyCommon(s, req);
        ::curl_easy_setopt(s.easy, CURLOPT_WRITEFUNCTION, streamWrite);
        ::curl_easy_setopt(s.easy, CURLOPT_WRITEDATA, &ctx);
        ::curl_easy_setopt(s.easy, CURLOPT_HEADERFUNCTION, detail::headerWrite);
        ::curl_easy_setopt(s.easy, CURLOPT_HEADERDATA, &out.headers);

        const CURLcode code = ::curl_easy_perform(s.easy);
        long status = 0;
        ::curl_easy_getinfo(s.easy, CURLINFO_RESPONSE_CODE, &status);
        out.statusCode = status;

        // 消费方主动中止（write 回调返回 false → libcurl 报 CURLE_WRITE_ERROR）视为正常终止
        if (code != CURLE_OK && code != CURLE_WRITE_ERROR)
        {
            return Core::Result<CurlResponse>::Error(
                Core::Error(Core::ErrorCode::NetworkError,
                            std::string("curl_easy_perform failed: ") + ::curl_easy_strerror(code)));
        }
        return Core::Result<CurlResponse>::Ok(out);
    }

    /// 百分号编码（用于 URL 查询参数）。封装 curl_easy_escape。
    inline auto curlUrlEncode(std::string_view in) -> std::string
    {
        detail::curlEnsureGlobalInit();
        detail::CurlSession s;
        if (s.easy == nullptr)
            return std::string(in);
        char *enc = ::curl_easy_escape(s.easy, in.data(), static_cast<int>(in.size()));
        if (enc == nullptr)
            return std::string(in);
        std::string out(enc);
        ::curl_free(enc);
        return out;
    }

} // namespace AE::Core::Net
