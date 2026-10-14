/// @file demo/main.cpp
/// @brief VXAE demo 入口：FTXUI TUI + AETUIBridge 接线（跨平台，不含任何 Windows SDK 头）。
/// @details 流程：恢复 demo_settings.json 持久化配置（主题/思考/意图/默认模型）→
///          构造 TUI（TUIManager 单例）→ 构造 AETUIBridge（配置优先级见 loadConfig）→
///          注册事件路由（UserInput/ModelSwitch/StopRequested/DeleteSession/…）→
///          tui->run() 阻塞主循环 → shutdown。全局异常经 std::set_terminate 落 ae_crash.log。

#include "Bridge/AETUIBridge.hpp"
#include "TUIManager.hpp"
#include "TUIType.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <print>
#include <random>
#include <sstream>
#include <string>
#include <iomanip>

namespace
{
    using namespace AE::TUI;

    // 会话 id：与 TUI 内部 makeMessageId 同口径（16 位十六进制随机数）。
    auto genSessionId() -> std::string
    {
        std::random_device rd;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | (static_cast<std::uint64_t>(rd()) & 0xFFu);
        std::ostringstream oss;
        oss << std::hex << std::setw(16) << std::setfill('0') << v;
        return oss.str();
    }

    // 全局 terminate 兜底：崩溃现场落 exe 工作目录 ae_crash.log（纯 std，跨平台）。
    auto installCrashLog() -> void
    {
        std::set_terminate([]
                           {
            std::string what = "unknown termination";
            if (auto p = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(p);
                }
                catch (const std::exception &e)
                {
                    what = e.what();
                }
                catch (...)
                {
                    what = "non-exception thrown";
                }
            }
            std::error_code ec;
            const auto path = std::filesystem::current_path(ec) / "ae_crash.log";
            if (ec)
                what += " (crash log path unavailable)";
            else
            {
                std::ofstream ofs(path, std::ios::app);
                ofs << "[terminate] " << what << "\n";
            }
            std::abort(); });
    }
} // namespace

auto main() -> int
{
    installCrashLog();

    auto &manager = TUIManager::instance();
    if (auto r = manager.initialize(); r.isError())
    {
        std::println(stderr, "TUIManager 初始化失败: {}", r.error().message());
        return 1;
    }

    auto tuiResult = manager.createTUI();
    if (tuiResult.isError())
    {
        std::println(stderr, "TUI 创建失败: {}", tuiResult.error().message());
        return 1;
    }
    auto tui = tuiResult.value();

    // ── 恢复持久化配置（demo_settings.json：主题/默认模型/思考/意图）──
    ThemeVariant theme = ThemeVariant::Dark;
    std::string persistedModel;
    bool thinking = true;
    bool intent = true;
    tui->persistedTUIConfig(theme, persistedModel, thinking, intent);

    TUIConfig uiConfig = tui->getConfig();
    uiConfig.theme = theme;
    if (!persistedModel.empty())
        uiConfig.defaultModel = persistedModel;
    if (auto r = tui->setConfig(uiConfig); r.isError())
        std::println(stderr, "TUI 配置应用失败: {}", r.error().message());

    // ── 桥接层：配置注入（env > demo_config.json > 内置默认；持久化默认模型次优先）──
    BridgeConfig bridgeCfg = AETUIBridge::loadConfig();
    const bool envModelSet = std::getenv("VXAE_MODEL") != nullptr;
    if (!envModelSet && !persistedModel.empty())
    {
        bridgeCfg.model = persistedModel;
        if (bridgeCfg.modelList.empty() || bridgeCfg.modelList.front().empty())
            bridgeCfg.modelList = {persistedModel};
    }

    AETUIBridge bridge(tui);
    if (auto r = bridge.initialize(std::move(bridgeCfg)); r.isError())
    {
        std::println(stderr, "AETUIBridge 初始化失败: {}", r.error().message());
        return 1;
    }
    bridge.syncToggles(thinking, intent);

    // ── 事件路由（handler 运行在 ftxui 事件线程；bridge 调用均非阻塞）──
    std::size_t sessionCounter = 1;
    tui->registerEventHandler([&bridge, tui, &thinking, &intent, &sessionCounter](const TUIEvent &ev)
                              {
        switch (ev.type)
        {
        case TUIEvent::Type::UserInput:
            bridge.handleUserInput(ev.payload, ev.sessionId);
            break;
        case TUIEvent::Type::StopRequested:
            bridge.requestStop();
            break;
        case TUIEvent::Type::ModelSwitch:
            bridge.switchModel(ev.payload);
            tui->setDefaultModel(ev.payload); // 同步持久化 defaultModel
            break;
        case TUIEvent::Type::DeleteSession:
            bridge.clearHistory(ev.sessionId);
            break;
        case TUIEvent::Type::SwitchSession:
            tui->setMessages(ev.sessionId, bridge.messagesFor(ev.sessionId));
            break;
        case TUIEvent::Type::NewSession:
        {
            SessionInfo info;
            info.id = genSessionId();
            info.title = "会话 " + std::to_string(++sessionCounter);
            info.createdAt = std::chrono::system_clock::now();
            info.updatedAt = info.createdAt;
            info.isActive = true;
            tui->addSession(info);
            tui->switchSession(info.id); // 触发 SwitchSession → 空历史回灌
            break;
        }
        case TUIEvent::Type::ToggleThinking:
            thinking = (ev.payload == "on");
            bridge.syncToggles(thinking, intent);
            tui->persistTUIConfig();
            break;
        case TUIEvent::Type::ToggleIntent:
            intent = (ev.payload == "on");
            bridge.syncToggles(thinking, intent);
            tui->persistTUIConfig();
            break;
        case TUIEvent::Type::QuitRequested:
            bridge.requestStop();
            break;
        default:
            break;
        } });

    // ── 启动欢迎打印（进入 alternate screen 前）──
    {
        const auto cfg = bridge.config();
        std::println("VXAE demo 启动");
        std::println("  模式    : {}", cfg.agentMode ? "agent" : "chat");
        std::println("  模型    : {}", cfg.model);
        std::println("  端点    : {}", AETUIBridge::normalizeEndpoint(cfg.endpoint));
        std::println("  快捷键  : Ctrl+P 停止生成 | Ctrl+N 新会话 | Ctrl+Q 退出 | Ctrl+H 帮助");
    }

    const int exitCode = [&tui, &bridge, &manager]
    {
        auto runResult = tui->run();
        bridge.shutdown();
        manager.shutdown();
        if (runResult.isError())
        {
            std::println(stderr, "TUI 运行失败: {}", runResult.error().message());
            return 1;
        }
        return runResult.value();
    }();

    std::println("VXAE demo 已退出 (code={})", exitCode);
    return exitCode;
}
