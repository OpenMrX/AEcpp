// =============================================================================
//  VMX::AE — AESessionStore.cpp
//  会话 JSON 持久化实现。
//  从 React+Session.cpp 迁移，适配 AE::IStateStore 接口。
// =============================================================================

#include "AESessionStore.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "Core/Logger.hpp"

namespace VMX::AE
{

    AESessionStore::AESessionStore()
        : dir_{".vmx_sessions"}
    {
    }

    AESessionStore::AESessionStore(std::string_view dir)
        : dir_{std::string{dir}}
    {
    }

    auto AESessionStore::validateId(std::string_view id) -> Core::Result<void>
    {
        if (id.empty())
            return Core::Result<void>::Error(
                Core::Error{Core::ErrorCode::InvalidArgument, "AESessionStore: empty id"});
        return Core::Result<void>::Ok();
    }

    auto AESessionStore::pathFor(std::string_view id) const -> std::string
    {
        return dir_ + "/" + std::string{id} + ".json";
    }

    auto AESessionStore::save(std::string_view session_id,
                              std::string_view snapshot) -> Core::Result<void>
    {
        try
        {
            auto vR = validateId(session_id);
            if (vR.isError())
                return vR;
            std::scoped_lock lock(mutex_);
            std::filesystem::create_directories(dir_);

            // 解析 snapshot 为 JSON，注入时间戳
            auto j = nlohmann::json::parse(snapshot);
            j["saved_at"] = std::chrono::system_clock::now().time_since_epoch().count();

            auto tmp = pathFor(session_id) + ".tmp";
            {
                std::ofstream f{tmp, std::ios::trunc};
                if (!f.is_open())
                {
                    return Core::Result<void>::Error(
                        Core::Error{"AESessionStore: cannot open temp file for writing"});
                }
                f << j.dump(2);
                if (!f.good())
                {
                    return Core::Result<void>::Error(
                        Core::Error{"AESessionStore: write to temp file failed"});
                }
            }

            std::error_code ec;
            std::filesystem::rename(tmp, pathFor(session_id), ec);
            if (ec)
            {
                return Core::Result<void>::Error(
                    Core::Error{"AESessionStore: rename failed: " + ec.message()});
            }

            return Core::Result<void>::Ok();
        }
        catch (const std::exception& e)
        {
            return Core::Result<void>::Error(
                Core::Error{std::string{"AESessionStore: save failed: "} + e.what()});
        }
    }

    auto AESessionStore::load(std::string_view session_id) -> Core::Result<std::string>
    {
        try
        {
            auto vR = validateId(session_id);
            if (vR.isError())
                return vR;
            std::scoped_lock lock(mutex_);

            std::ifstream f{pathFor(session_id)};
            if (!f.is_open())
            {
                return Core::Result<std::string>::Error(
                    Core::Error{"AESessionStore: session not found: " + std::string{session_id}});
            }

            std::string content{std::istreambuf_iterator<char>{f},
                                std::istreambuf_iterator<char>{}};
            return Core::Result<std::string>::Ok(std::move(content));
        }
        catch (const std::exception& e)
        {
            return Core::Result<std::string>::Error(
                Core::Error{std::string{"AESessionStore: load failed: "} + e.what()});
        }
    }

    auto AESessionStore::exists(std::string_view session_id) const -> bool
    {
        return std::filesystem::exists(pathFor(session_id));
    }

    void AESessionStore::remove(std::string_view session_id)
    {
        std::error_code ec;
        std::filesystem::remove(pathFor(session_id), ec);
    }

    auto AESessionStore::list() const -> std::vector<SessionInfo>
    {
        std::vector<SessionInfo> out;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator{dir_, ec})
        {
            if (entry.is_regular_file() && entry.path().extension() == ".json")
            {
                auto stem = entry.path().stem().string();
                auto ftime = entry.last_write_time().time_since_epoch().count();
                out.push_back({stem, std::to_string(ftime), 0});
            }
        }
        return out;
    }

    auto AESessionStore::generateId() -> std::string
    {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << std::put_time(std::gmtime(&tt), "%Y%m%d-%H%M%S");
        return oss.str();
    }

} // namespace VMX::AE
