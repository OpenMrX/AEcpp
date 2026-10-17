#pragma once

/// @file Collab/AgentRegistry.hpp
/// @brief Agent 注册/查找/生命周期（Docs/01 §5.0.3、§5.1）。
///
/// @details 不持有运行时所有权（生命周期由宿主管）。提供按 id 查找与按 capability 选择。
///
/// @note 所属命名空间：AE::Engine（扁平，Collab 目录）。

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Collab/AgentRuntime.hpp"

namespace AE::Engine
{

    class AgentRegistry
    {
    public:
        void add(IAgentRuntime *rt)
        {
            if (rt)
                m_agents.push_back(rt);
        }

        [[nodiscard]] auto get(std::string_view id) const -> IAgentRuntime *
        {
            for (auto *a : m_agents)
                if (a->spec().id == id)
                    return a;
            return nullptr;
        }

        [[nodiscard]] auto agents_by_capability(std::string_view cap) const
            -> std::vector<IAgentRuntime *>
        {
            std::vector<IAgentRuntime *> out;
            for (auto *a : m_agents)
                if (a->spec().has_capability(cap))
                    out.push_back(a);
            return out;
        }

        // 选取首个具备 cap 的 agent；cap 为空则返回首个 agent。找不到返回 nullptr。
        [[nodiscard]] auto pick(std::string_view cap) const -> IAgentRuntime *
        {
            if (cap.empty())
                return m_agents.empty() ? nullptr : m_agents.front();
            for (auto *a : m_agents)
                if (a->spec().has_capability(cap))
                    return a;
            return nullptr;
        }

        [[nodiscard]] auto all() const -> const std::vector<IAgentRuntime *> & { return m_agents; }
        [[nodiscard]] auto empty() const noexcept -> bool { return m_agents.empty(); }
        [[nodiscard]] auto size() const noexcept -> std::size_t { return m_agents.size(); }

    private:
        std::vector<IAgentRuntime *> m_agents;
    };

} // namespace AE::Engine
