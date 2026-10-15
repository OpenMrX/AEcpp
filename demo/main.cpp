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
#include <algorithm>
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

    // ── 命令行选项（-m/--model 模型配置页；-h/--help 用法）──
    struct CliOptions
    {
        bool help = false;
        bool modelConfig = false;  ///< -m/--model：启动即进入模型配置页
        std::string model;         ///< -m/--model 的可选值：预填"模型"字段
        bool invalid = false;      ///< 未知参数：打印用法并以非零码退出
    };

    auto printUsage() -> void
    {
        std::println("VXAE demo — 终端聊天演示（FTXUI）");
        std::println();
        std::println("用法:");
        std::println("  vxae_demo [选项]");
        std::println();
        std::println("选项:");
        std::println("  -m, --model [模型名]  启动即进入模型配置页（模型/端点/API Key/模式/模型列表），");
        std::println("                        \"保存并应用\"按需写入 exe 同目录 demo_config.json；");
        std::println("                        给定模型名时以它预填\"模型\"字段");
        std::println("  -h, --help            显示本帮助并退出");
        std::println();
        std::println("运行时快捷键: Ctrl+G 模型配置页 | Ctrl+H 帮助 | Ctrl+P 停止 | Ctrl+Q 退出");
        std::println("配置优先级:   环境变量 (VXAE_ENDPOINT/VXAE_API_KEY/VXAE_MODEL/VXAE_AGENT_MODE)");
        std::println("              > demo_config.json > 内置默认");
    }

    // 仅支持 -m/--model（值可选，亦支持 --model=值）与 -h/--help；其余参数判 invalid。
    auto parseCli(int argc, char **argv) -> CliOptions
    {
        CliOptions opt;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "-h" || arg == "--help")
            {
                opt.help = true;
                return opt;
            }
            if (arg == "-m" || arg == "--model")
            {
                opt.modelConfig = true;
                // 值可选：下一个参数非选项（不以 '-' 开头）时吞为模型名。
                if (i + 1 < argc && argv[i + 1][0] != '-')
                {
                    opt.model = argv[++i];
                }
                continue;
            }
            if (arg.rfind("--model=", 0) == 0)
            {
                opt.modelConfig = true;
                opt.model = arg.substr(std::string("--model=").size());
                continue;
            }
            opt.invalid = true;
        }
        return opt;
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

auto main(int argc, char **argv) -> int
{
    installCrashLog();

    const CliOptions cli = parseCli(argc, argv);
    if (cli.help)
    {
        printUsage();
        return 0;
    }
    if (cli.invalid)
    {
        printUsage();
        return 2;
    }

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
        {
            bridgeCfg.modelList = {persistedModel};
        }
        else if (std::find(bridgeCfg.modelList.begin(), bridgeCfg.modelList.end(),
                           persistedModel) == bridgeCfg.modelList.end())
        {
            // 持久化的默认模型必须出现在面板可选列表里（否则底部列表选不中它）。
            bridgeCfg.modelList.push_back(persistedModel);
        }
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
        case TUIEvent::Type::ModelConfigOpen:
        {
            // Ctrl+G：用当前生效配置预填后打开模型配置页（运行时等价于 -m/--model）。
            auto cfg = bridge.config();
            tui->setModelConfigDefaults(cfg.endpoint, cfg.apiKey, cfg.model,
                                        cfg.agentMode, cfg.modelList);
            tui->openModelConfigPage();
            break;
        }
        case TUIEvent::Type::ModelConfigApply:
        {
            // 模型配置页"保存并应用"：按需落盘 demo_config.json + 热应用桥接层。
            BridgeConfig cfg = bridge.config();
            if (ev.data.contains("endpoint") && ev.data["endpoint"].is_string())
                cfg.endpoint = ev.data["endpoint"].get<std::string>();
            if (ev.data.contains("apiKey") && ev.data["apiKey"].is_string())
                cfg.apiKey = ev.data["apiKey"].get<std::string>();
            if (ev.data.contains("model") && ev.data["model"].is_string())
                cfg.model = ev.data["model"].get<std::string>();
            if (ev.data.contains("agentMode") && ev.data["agentMode"].is_boolean())
                cfg.agentMode = ev.data["agentMode"].get<bool>();
            if (ev.data.contains("modelList") && ev.data["modelList"].is_array())
                cfg.modelList = ev.data["modelList"].get<std::vector<std::string>>();
            if (cfg.modelList.empty())
                cfg.modelList.push_back(cfg.model);

            const bool saved = AETUIBridge::saveConfig(cfg);
            bridge.applyConfig(cfg);
            tui->setDefaultModel(cfg.model); // demo_settings.json 默认模型同步

            Message tip;
            tip.id = genSessionId(); // 消息 id（与 TUI makeMessageId 同口径随机 hex）
            tip.role = MessageRole::System;
            tip.status = MessageStatus::Completed;
            tip.content = saved
                ? "✓ 模型配置已保存到 demo_config.json 并应用（模型 " + cfg.model +
                      " · 模式 " + (cfg.agentMode ? "agent" : "chat") + "）。"
                : "⚠ demo_config.json 写入失败：配置已在本会话应用，重启后不保留。";
            tui->addMessage(tui->getCurrentSessionId(), tip);
            break;
        }
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

    // ── 模型配置页弹出时机 ──
    // 1) 命令行 -m/--model 显式进入；2) 未配置任何模型列表（env VXAE_MODEL 未设、
    //    demo_config.json 无 modelList/model、也无持久化默认模型）时自动进入，
    //    引导用户先配置模型再聊天。已配置则直接进聊天主界面。
    const bool modelConfigured =
        AETUIBridge::hasConfiguredModelList() || !persistedModel.empty();
    if (cli.modelConfig || !modelConfigured)
    {
        auto cfg = bridge.config();
        if (!cli.model.empty())
        {
            cfg.model = cli.model; // CLI 显式模型名优先预填
        }
        tui->setModelConfigDefaults(cfg.endpoint, cfg.apiKey, cfg.model,
                                    cfg.agentMode, cfg.modelList);
        tui->openModelConfigPage();
    }

    // ── 启动欢迎打印（进入 alternate screen 前）──
    {
        const auto cfg = bridge.config();
        std::println("VXAE demo 启动");
        std::println("  模式    : {}", cfg.agentMode ? "agent" : "chat");
        std::println("  模型    : {}", cfg.model);
        std::println("  端点    : {}", AETUIBridge::normalizeEndpoint(cfg.endpoint));
        std::println("  配置页  : 未配置模型列表时自动弹出 · vxae_demo -m/--model · 运行时 Ctrl+G · 保存写入 demo_config.json");
        std::println("  快捷键  : Ctrl+P 停止生成 | Ctrl+N 新会话 | Ctrl+G 模型配置 | Ctrl+Q 退出 | Ctrl+H 帮助");
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
