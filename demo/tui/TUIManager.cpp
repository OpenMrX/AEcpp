/// @file demo/tui/TUIManager.cpp
/// @brief TUI 生命周期管理器实现（VXAE demo 移植版，逻辑与原版一致）。

#include "TUIManager.hpp"

namespace AE::TUI
{

    auto TUIManager::instance() -> TUIManager &
    {
        static TUIManager s_instance;
        return s_instance;
    }

    TUIManager::~TUIManager()
    {
        shutdown();
    }

    auto TUIManager::initialize() -> Core::Result<void>
    {
        std::scoped_lock lock(m_mutex);
        return initializeLocked();
    }

    auto TUIManager::initializeLocked() -> Core::Result<void>
    {
        if (m_initialized)
        {
            return Core::Result<void>::Ok();
        }

        m_initialized = true;
        return Core::Result<void>::Ok();
    }

    auto TUIManager::shutdown() -> Core::Result<void>
    {
        std::scoped_lock lock(m_mutex);

        if (!m_initialized)
        {
            return Core::Result<void>::Ok();
        }

        for (auto &[name, tui] : m_tuiInstances)
        {
            if (tui)
            {
                tui->shutdown();
            }
        }
        m_tuiInstances.clear();
        m_defaultTUI.reset();
        m_globalHandlers.clear();

        m_initialized = false;
        return Core::Result<void>::Ok();
    }

    auto TUIManager::createTUI() -> Core::Result<std::shared_ptr<ITUI>>
    {
        std::scoped_lock lock(m_mutex);

        if (!m_initialized)
        {
            auto initResult = initializeLocked();
            if (initResult.isError())
            {
                return Core::Result<std::shared_ptr<ITUI>>::Error(initResult.error());
            }
        }

        auto tui = std::make_shared<TUI>();
        auto initResult = tui->initialize();
        if (initResult.isError())
        {
            return Core::Result<std::shared_ptr<ITUI>>::Error(initResult.error());
        }

        tui->setConfig(m_defaultConfig);

        for (const auto &handler : m_globalHandlers)
        {
            tui->registerEventHandler(handler);
        }

        if (!m_defaultTUI)
        {
            m_defaultTUI = tui;
        }

        m_tuiInstances["default"] = tui;

        return Core::Result<std::shared_ptr<ITUI>>::Ok(tui);
    }

    auto TUIManager::getDefaultTUI() -> std::shared_ptr<ITUI>
    {
        std::scoped_lock lock(m_mutex);
        if (m_defaultTUI)
        {
            return m_defaultTUI;
        }

        auto it = m_tuiInstances.find("default");
        if (it != m_tuiInstances.end())
        {
            return it->second;
        }

        return nullptr;
    }

    auto TUIManager::getTUI(const std::string &name) -> std::shared_ptr<ITUI>
    {
        std::scoped_lock lock(m_mutex);
        auto it = m_tuiInstances.find(name);
        if (it != m_tuiInstances.end())
        {
            return it->second;
        }
        return nullptr;
    }

    auto TUIManager::destroyTUI(const std::string &name) -> void
    {
        std::scoped_lock lock(m_mutex);
        auto it = m_tuiInstances.find(name);
        if (it != m_tuiInstances.end())
        {
            if (it->second)
            {
                it->second->shutdown();
            }
            if (m_defaultTUI == it->second)
            {
                m_defaultTUI.reset();
            }
            m_tuiInstances.erase(it);
        }
    }

    auto TUIManager::run() -> Core::Result<int>
    {
        std::shared_ptr<ITUI> tui;
        {
            std::scoped_lock lock(m_mutex);
            tui = m_defaultTUI;
        }

        if (!tui)
        {
            auto result = createTUI();
            if (result.isError())
            {
                return Core::Result<int>::Error(result.error());
            }
            tui = result.value();
        }

        return tui->run();
    }

    auto TUIManager::stop() -> void
    {
        std::scoped_lock lock(m_mutex);
        for (auto &[name, tui] : m_tuiInstances)
        {
            if (tui)
            {
                tui->stop();
            }
        }
    }

    auto TUIManager::registerGlobalEventHandler(EventCallback callback) -> void
    {
        std::scoped_lock lock(m_mutex);
        m_globalHandlers.push_back(std::move(callback));

        for (auto &[name, tui] : m_tuiInstances)
        {
            if (tui)
            {
                tui->registerEventHandler(m_globalHandlers.back());
            }
        }
    }

    auto TUIManager::unregisterAllGlobalEventHandlers() -> void
    {
        std::scoped_lock lock(m_mutex);
        m_globalHandlers.clear();

        for (auto &[name, tui] : m_tuiInstances)
        {
            if (tui)
            {
                tui->unregisterAllEventHandlers();
            }
        }
    }

    auto TUIManager::setDefaultConfig(const TUIConfig &config) -> void
    {
        std::scoped_lock lock(m_mutex);
        m_defaultConfig = config;

        for (auto &[name, tui] : m_tuiInstances)
        {
            if (tui)
            {
                tui->setConfig(config);
            }
        }
    }

    auto TUIManager::getDefaultConfig() const -> const TUIConfig &
    {
        return m_defaultConfig;
    }

    auto TUIManager::isInitialized() const -> bool
    {
        return m_initialized;
    }

} // namespace AE::TUI
