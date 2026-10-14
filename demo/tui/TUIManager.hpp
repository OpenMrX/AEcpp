#pragma once

/// @file demo/tui/TUIManager.hpp
/// @brief TUI 生命周期管理器（单例）：initialize/shutdown/运行态（VXAE demo 移植版）。
/// @details 进程内单例（`instance()`）；管理 TUI 主实例的创建与销毁，
///          初始化失败返回 `Core::Result` 错误。

#include "ITUI.hpp"
#include "TUI.hpp"
#include "TUIType.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AE::TUI
{

    class TUIManager final
    {
    public:
        static auto instance() -> TUIManager &;

        auto initialize() -> Core::Result<void>;
        auto shutdown() -> Core::Result<void>;

        auto createTUI() -> Core::Result<std::shared_ptr<ITUI>>;
        auto getDefaultTUI() -> std::shared_ptr<ITUI>;
        auto getTUI(const std::string &name) -> std::shared_ptr<ITUI>;
        auto destroyTUI(const std::string &name) -> void;

        auto run() -> Core::Result<int>;
        auto stop() -> void;

        auto registerGlobalEventHandler(EventCallback callback) -> void;
        auto unregisterAllGlobalEventHandlers() -> void;

        auto setDefaultConfig(const TUIConfig &config) -> void;
        auto getDefaultConfig() const -> const TUIConfig &;

        auto isInitialized() const -> bool;

    private:
        TUIManager() = default;
        ~TUIManager();

        TUIManager(const TUIManager &) = delete;
        auto operator=(const TUIManager &) -> TUIManager & = delete;

        auto initializeLocked() -> Core::Result<void>;

        mutable std::mutex m_mutex;
        std::shared_ptr<TUI> m_defaultTUI;
        std::unordered_map<std::string, std::shared_ptr<ITUI>> m_tuiInstances;
        TUIConfig m_defaultConfig;
        bool m_initialized = false;
        std::vector<EventCallback> m_globalHandlers;
    };

} // namespace AE::TUI
