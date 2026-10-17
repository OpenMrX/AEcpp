#pragma once

/// @file Mapping/VisualSpec.hpp
/// @brief 可序列化拓扑（Dify/Coze 低代码接入点，Docs/01 §6.7）。
///
/// @details `VisualSpec ↔ JSON` 双向编解码 + schema 校验 + 版本字段（便于迁移）。
///
/// @note 命名空间为扁平 `AE::Engine`（评审结论 #1）。

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nlohmann-json.hpp"

#include "Core/Result.hpp"
#include "Core/Error.hpp"

namespace AE::Engine
{

    struct VisualNode
    {
        std::string id;
        std::string agent;
        std::string role; // 可选（orchestrator / worker / moderator …）
        std::vector<std::string> capabilities;

        [[nodiscard]] auto to_json() const -> nlohmann::json
        {
            nlohmann::json j = nlohmann::json::object({{"id", id}, {"agent", agent}});
            if (!role.empty())
                j["role"] = role;
            if (!capabilities.empty())
                j["capabilities"] = capabilities;
            return j;
        }

        static auto from_json(const nlohmann::json &j) -> VisualNode
        {
            VisualNode n;
            n.id = j.value("id", std::string{});
            n.agent = j.value("agent", std::string{});
            n.role = j.value("role", std::string{});
            if (j.contains("capabilities") && j["capabilities"].is_array())
                for (const auto &c : j["capabilities"])
                    if (c.is_string())
                        n.capabilities.push_back(c.get<std::string>());
            return n;
        }
    };

    struct VisualEdge
    {
        std::string from;
        std::string to;

        [[nodiscard]] auto to_json() const -> nlohmann::json
        {
            return nlohmann::json::object({{"from", from}, {"to", to}});
        }

        static auto from_json(const nlohmann::json &j) -> VisualEdge
        {
            return VisualEdge{j.value("from", std::string{}), j.value("to", std::string{})};
        }
    };

    struct VisualSpec
    {
        static constexpr int kCurrentVersion = 1;

        int version{kCurrentVersion};
        std::string kind;
        std::vector<VisualNode> nodes;
        std::vector<VisualEdge> edges;
        nlohmann::json config = nlohmann::json::object();

        [[nodiscard]] auto to_json() const -> nlohmann::json
        {
            nlohmann::json jn = nlohmann::json::array();
            for (const auto &n : nodes)
                jn.push_back(n.to_json());
            nlohmann::json je = nlohmann::json::array();
            for (const auto &e : edges)
                je.push_back(e.to_json());
            return nlohmann::json::object({{"version", version},
                                           {"kind", kind},
                                           {"nodes", std::move(jn)},
                                           {"edges", std::move(je)},
                                           {"config", config}});
        }

        [[nodiscard]] auto to_string() const -> std::string { return to_json().dump(); }

        static auto from_json(const nlohmann::json &j) -> Core::Result<VisualSpec>
        {
            if (!j.is_object())
                return Core::Result<VisualSpec>::Error(
                    Core::Error(Core::ErrorCode::SerializationError, "visual spec: not a JSON object"));

            VisualSpec s;
            s.version = j.value("version", kCurrentVersion);
            s.kind = j.value("kind", std::string{});
            if (j.contains("nodes") && j["nodes"].is_array())
                for (const auto &n : j["nodes"])
                    s.nodes.push_back(VisualNode::from_json(n));
            if (j.contains("edges") && j["edges"].is_array())
                for (const auto &e : j["edges"])
                    s.edges.push_back(VisualEdge::from_json(e));
            if (j.contains("config") && j["config"].is_object())
                s.config = j["config"];

            auto ok = s.validate();
            if (ok.isError())
                return Core::Result<VisualSpec>::Error(ok.error());
            return Core::Result<VisualSpec>::Ok(std::move(s));
        }

        static auto from_string(std::string_view text) -> Core::Result<VisualSpec>
        {
            nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
            if (j.is_discarded())
                return Core::Result<VisualSpec>::Error(
                    Core::Error(Core::ErrorCode::SerializationError, "visual spec: invalid JSON"));
            return from_json(j);
        }

        [[nodiscard]] auto validate() const -> Core::Result<void>
        {
            if (version <= 0 || version > kCurrentVersion)
                return Core::Result<void>::Error(Core::Error(
                    Core::ErrorCode::SerializationError, "visual spec: unsupported version " + std::to_string(version)));
            if (kind.empty())
                return Core::Result<void>::Error(
                    Core::Error(Core::ErrorCode::SerializationError, "visual spec: empty kind"));
            if (nodes.empty())
                return Core::Result<void>::Error(
                    Core::Error(Core::ErrorCode::SerializationError, "visual spec: no nodes"));

            std::unordered_set<std::string> ids;
            for (const auto &n : nodes)
            {
                if (n.id.empty() || n.agent.empty())
                    return Core::Result<void>::Error(Core::Error(
                        Core::ErrorCode::SerializationError, "visual spec: node missing id/agent"));
                if (!ids.insert(n.id).second)
                    return Core::Result<void>::Error(
                        Core::Error(Core::ErrorCode::SerializationError, "visual spec: duplicate node id " + n.id));
            }
            for (const auto &e : edges)
                if (ids.find(e.from) == ids.end() || ids.find(e.to) == ids.end())
                    return Core::Result<void>::Error(Core::Error(
                        Core::ErrorCode::SerializationError, "visual spec: edge references unknown node"));
            return Core::Result<void>::Ok();
        }

        // 往返编解码等价校验（§6.8）。
        [[nodiscard]] auto roundtrip_equal() const -> bool
        {
            auto r = from_json(to_json());
            return r.isSuccess() && r.value().to_json() == to_json();
        }
    };

} // namespace AE::Engine
