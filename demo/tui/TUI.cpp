#include "TUI.hpp"

#include "nlohmann-json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <print>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

// demo 层不包含任何 Windows SDK 头文件：仅按真实导出名 dllimport 声明 kernel32 接口
// （kernel32 默认链接），运行期动态解析 user32 的 GetAsyncKeyState，
// 用于区分 Windows 控制台上与 Backspace 字节相同的 Ctrl+H。
#ifdef _WIN32
extern "C" __declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
extern "C" __declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);
#endif

namespace AE::TUI
{

namespace
{
    // 消息/会话 id：random_device 取 8 字节随机数转 16 位十六进制
    // （替代原版第三方 UUID 库，demo 层仅需唯一性，不要求 RFC 4122 格式）。
    auto makeMessageId() -> std::string
    {
        std::random_device rd;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
        {
            v = (v << 8) | (static_cast<std::uint64_t>(rd()) & 0xFFu);
        }
        std::ostringstream oss;
        oss << std::hex << std::setw(16) << std::setfill('0') << v;
        return oss.str();
    }

    // 取可执行文件所在目录，用于定位 demo_settings.json（与 GUI 侧口径一致）。
    auto executableDir() -> std::filesystem::path
    {
#ifdef _WIN32
        char buf[260];
        const unsigned long n = GetModuleFileNameA(nullptr, buf, static_cast<unsigned long>(sizeof(buf)));
        if (n != 0 && n < sizeof(buf))
        {
            return std::filesystem::path(std::string(buf, n)).parent_path();
        }
#endif
        return std::filesystem::current_path();
    }

    // demo 配置文件路径：可执行文件同目录 demo_settings.json（扁平 4 字段）。
    auto demoSettingsPath() -> const std::filesystem::path &
    {
        static const std::filesystem::path s_path = executableDir() / "demo_settings.json";
        return s_path;
    }

    // 读 demo_settings.json：文件缺失/解析失败一律回退空对象（尽力而为，绝不抛）。
    auto loadDemoSettingsJson() -> nlohmann::json
    {
        try
        {
            std::ifstream in(demoSettingsPath(), std::ios::binary);
            if (!in.is_open())
            {
                return nlohmann::json::object();
            }
            auto j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_discarded() || !j.is_object())
            {
                return nlohmann::json::object();
            }
            return j;
        }
        catch (const std::exception &e)
        {
            std::println(stderr, "[TUI] load demo settings failed: {}", e.what());
            return nlohmann::json::object();
        }
        catch (...)
        {
            std::println(stderr, "[TUI] load demo settings failed: unknown exception");
            return nlohmann::json::object();
        }
    }

    // 写 demo_settings.json：失败仅告警，不影响运行。
    auto saveDemoSettingsJson(const nlohmann::json &j) -> void
    {
        try
        {
            std::ofstream out(demoSettingsPath(), std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                std::println(stderr, "[TUI] persist config failed: cannot open {}",
                             demoSettingsPath().string());
                return;
            }
            out << j.dump(2) << '\n';
        }
        catch (const std::exception &e)
        {
            std::println(stderr, "[TUI] persist config failed: {}", e.what());
        }
        catch (...)
        {
            std::println(stderr, "[TUI] persist config failed: unknown exception");
        }
    }

    // 去首尾空白（配置页字段校验/入库前统一裁剪）。
    auto trimCopy(const std::string &text) -> std::string
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        const auto last = text.find_last_not_of(" \t\r\n");
        return text.substr(first, last - first + 1);
    }

    // 逗号分隔字符串 → 去空白/去空项列表（模型配置页"模型列表"字段解析，兼容中文全角逗号）。
    auto splitCsv(const std::string &text) -> std::vector<std::string>
    {
        // 中文全角逗号 U+FF0C（UTF-8: EF BC 8C）统一替换为英文逗号后再切分。
        std::string normalized = text;
        for (std::size_t i = normalized.find("\xEF\xBC\x8C"); i != std::string::npos;
             i = normalized.find("\xEF\xBC\x8C", i))
        {
            normalized.replace(i, 3, ",");
        }

        std::vector<std::string> out;
        std::string item;
        auto flush = [&]
        {
            const std::string trimmed = trimCopy(item);
            if (!trimmed.empty())
            {
                out.push_back(trimmed);
            }
            item.clear();
        };
        for (const char c : normalized)
        {
            if (c == ',')
            {
                flush();
                continue;
            }
            item += c;
        }
        flush();
        return out;
    }

    // Windows 控制台 Ctrl 修饰键探测（同 InputBox 的 Shift 手法）：
    // Ctrl+H 字节 0x08 与 Backspace 相同，FTXUI 无法区分，需运行期查 GetAsyncKeyState。
    // 经 dllimport 的 LoadLibraryA/GetProcAddress 动态解析 user32，避免包含 Windows SDK 头文件。
    auto ctrlDownNow() -> bool
    {
#ifdef _WIN32
        using GetAsyncKeyStateFn = short(__stdcall *)(int);
        static const GetAsyncKeyStateFn s_fn = []() -> GetAsyncKeyStateFn
        {
            void *hModule = LoadLibraryA("user32.dll");
            if (hModule == nullptr)
            {
                return nullptr;
            }
            return reinterpret_cast<GetAsyncKeyStateFn>(GetProcAddress(hModule, "GetAsyncKeyState"));
        }();
        constexpr int kVKeyControl = 0x11;
        return s_fn != nullptr && (s_fn(kVKeyControl) & 0x8000) != 0;
#else
        return false;
#endif
    }
} // namespace

    TUI::TUI() = default;

    TUI::~TUI()
    {
        if (m_running.load())
        {
            stop();
        }
    }

    auto TUI::initialize() -> Core::Result<void>
    {
        if (m_initialized.load())
        {
            return Core::Result<void>::Ok();
        }

        try
        {
            m_themeManager = std::make_shared<ThemeManager>();
            // 优先恢复持久化的主题（demo_settings.json），否则用配置默认。
            m_config.theme = loadPersistedTheme();
            m_themeManager->setTheme(m_config.theme);

            m_header = std::make_shared<HeaderComponent>(m_themeManager);
            m_inputBox = std::make_shared<InputBox>(m_themeManager);
            m_statusBar = std::make_shared<StatusBarComponent>(m_themeManager);

            m_inputBox->onSubmit([this](const std::string &text)
                                 { onInputSubmit(text); });

            m_statusBar->setVisible(m_config.showStatusBar);

            // Header 承载会话切换/新建/删除 + 思考/意图开关，经事件总线转发给桥接层。
            m_header->setTitle("AE TUI");
            m_header->onNewSession([this]
                                   { emitEvent(TUIEvent{.type = TUIEvent::Type::NewSession}); });
            // 会话池点击 → 走 TUI::switchSession（切当前会话 + emitEvent(SwitchSession)；
            // 历史消息由桥接层监听事件后经 setMessages 推入，demo 层不回读后端）。
            m_header->onSessionSelect([this](const std::string &sessionId)
                                      { switchSession(sessionId); });
            m_header->onDeleteSession([this](const std::string &sessionId)
                                      {
                                          // 先弹确认对话框，确认后才真正删除。
                                          m_pendingDeleteSessionId = sessionId;
                                          m_deleteConfirmVisible = true;
                                          postCustomEvent();
                                      });
            m_header->onThinkingToggle([this](bool enabled)
                                       { emitEvent(TUIEvent{.type = TUIEvent::Type::ToggleThinking, .sessionId = m_currentSessionId, .payload = enabled ? "on" : "off"}); });
            m_header->onIntentToggle([this](bool enabled)
                                     { emitEvent(TUIEvent{.type = TUIEvent::Type::ToggleIntent, .sessionId = m_currentSessionId, .payload = enabled ? "on" : "off"}); });

            // demo 层会话为纯内存：启动时无会话则创建一个默认会话
            // （原版由业务层经 SQLite 后端创建，此处 TUI 自持会话表）。
            ensureDefaultSession();

            // 底部右侧仅保留最常用两个快捷键：Ctrl+H 帮助、Ctrl+Q 退出。
            m_statusBar->setRightText("Ctrl+H: 帮助  Ctrl+Q: 退出");

            // 鼠标点击状态栏模型项 → 展开底部模型列表（唯一的展开入口）。
            m_statusBar->setOnItemClick([this](const StatusItem &item)
                                        {
                                            if (item.label.rfind("📦", 0) == 0)
                                            {
                                                openModelList();
                                                postCustomEvent();
                                            }
                                        });

            // 模型配置页组件树（-m/--model 或 Ctrl+G 打开；buildLayout 以 Modal 挂载）。
            buildModelConfigPage();

            m_initialized.store(true);
            return Core::Result<void>::Ok();
        }
        catch (const std::exception &e)
        {
            return Core::Result<void>::Error(
                Core::Error(Core::ErrorCode::Unknown, std::format("TUI initialization failed: {}", e.what())));
        }
    }

    auto TUI::shutdown() -> Core::Result<void>
    {
        if (!m_initialized.load())
        {
            return Core::Result<void>::Ok();
        }

        stop();

        {
            std::scoped_lock lock(m_eventMutex);
            m_eventHandlers.clear();
        }

        {
            std::scoped_lock lock(m_stateMutex);
            m_chatPanels.clear();
            m_sessions.clear();
            m_currentSessionId.clear();
        }
        m_initialized.store(false);

        return Core::Result<void>::Ok();
    }

    auto TUI::run() -> Core::Result<int>
    {
        if (!m_initialized.load())
        {
            auto initResult = initialize();
            if (initResult.isError())
            {
                return Core::Result<int>::Error(initResult.error());
            }
        }

        if (m_running.exchange(true))
        {
            return Core::Result<int>::Error(
                Core::Error(Core::ErrorCode::Unknown, "TUI is already running"));
        }

        // 终端恢复守卫：无论以何种路径退出（正常 / 异常），都确保恢复光标可见并
        // 退出 FTXUI 进入的 alternate 屏幕缓冲，避免进程异常退出后控制台残留
        // "无光标 / 残屏 / 乱码"，导致下次启动无法正常工作。
        bool terminalRestored = false;
        std::thread throttle;
        auto restoreTerminal = [&]() {
            if (terminalRestored)
                return;
            terminalRestored = true;
            // 异常路径下若节流线程尚未 join，先回收，杜绝 std::thread 析构时
            // 仍 joinable 触发 std::terminate（这正是"崩溃后终端损坏/重启乱码"的根因）。
            if (throttle.joinable())
                throttle.join();
            // 退出 alternate 屏幕，恢复主屏幕；恢复光标可见。幂等，重复执行无害。
            std::print("\033[?1049l");
            std::print("\033[?25h");
            std::fflush(stdout);
        };
        // 以 shared_ptr 作 scope guard：run() 任意 return / 异常路径均触发恢复。
        auto terminalGuard = std::shared_ptr<void>(nullptr, [&](void *) { restoreTerminal(); });

        try
        {
            // 隐藏终端光标，消除左上角闪烁；显式进入 alternate 屏幕（与异常退出时的
            // restoreTerminal 对称退出，确保即使 screen.Loop 异常中断也能恢复）。
            std::print("\033[?25l");
            std::print("\033[?1049h");
            std::fflush(stdout);
            m_running.store(true);

            auto screen = ftxui::ScreenInteractive::Fullscreen();
            // Ctrl+C 处理策略：
            // ForceHandleCtrlC(false) —— force=false 表示“不强求 FTXUI 自行处理
            // Ctrl+C 信号”，从而把 Ctrl+C 作为普通 UI 键盘事件（Event::CtrlC）下传
            // 给顶层 CatchEvent；配合下方 handleKeyEvent 显式忽略该事件，实现“不中断程序”。
            // （注意：传 true 会让 FTXUI 直接调用 Exit() 终止 Loop，Ctrl+C 立即退出程序。）
            screen.ForceHandleCtrlC(false);
            m_screenPtr.store(&screen);

            m_mainContainer = buildLayout();

            // 节流帧率（自适应见 throttle 线程内）：默认 50ms → 流式/交互 16ms、安静 200ms。
            // 重绘请求统一由 m_frameRequested 标志驱动；业务层只置位该标志，
            // 这里（唯一 throttle 线程）是全局唯一调用 ftxui screen.Post 的地方。
            throttle = std::thread([&] {
                // 自适应帧率：
                // - 活跃（流式输出中 / 有重绘请求，含滚轮等交互）：高帧率刷新，
                //   让逐 token 流式连续流畅、滚动平滑（默认 50ms → 16ms≈60fps）。
                // - 安静（无流式、无交互）：低频保留模式（200ms≈5fps），降低 CPU/耗电。
                // 任一活跃信号（流式 token 经 postCustomEvent 置位 / 滚轮置位 / ChatPanel 在途流式）
                // 都会把本帧判为 busy → 进入高频档，安静后自动回落低频。
                const int activeMs = (m_config.refreshRateMs > 0) ? m_config.refreshRateMs : 16;
                const int idleMs = 200;
                while (m_running.load()) {
                    const bool busy = m_frameRequested.exchange(false, std::memory_order_acquire);
                    if (auto *panel = m_activeChatPanel.load(std::memory_order_acquire); panel && panel->isStreaming())
                    {
                        // 仍在流式输出：持续高帧率，避免"一段段卡出来"。
                        screen.Post([] {});
                        std::this_thread::sleep_for(std::chrono::milliseconds(activeMs));
                        continue;
                    }
                    screen.Post([] {});
                    std::this_thread::sleep_for(std::chrono::milliseconds(busy ? activeMs : idleMs));
                }
            });

            screen.Loop(m_mainContainer);

            if (throttle.joinable())
                throttle.join();

            m_screenPtr.store(nullptr);
            m_running.store(false);
            return Core::Result<int>::Ok(0);
        }
        catch (const std::exception &e)
        {
            m_screenPtr.store(nullptr);
            m_running.store(false);
            return Core::Result<int>::Error(
                Core::Error(Core::ErrorCode::Unknown, std::format("TUI runtime error: {}", e.what())));
        }
        catch (...)
        {
            m_screenPtr.store(nullptr);
            m_running.store(false);
            return Core::Result<int>::Error(
                Core::Error(Core::ErrorCode::Unknown, "TUI runtime error: unknown exception"));
        }
    }

    auto TUI::stop() -> void
    {
        // 关键修复：退出 FTXUI 主循环只取决于 screen 是否存在，不可再依赖
        // m_running 标志——否则调用方（如 Ctrl+Q 处理）若先置 m_running=false 再调
        // stop()，会导致 screen->Exit() 被跳过、主循环永不终止，表现为"按 Ctrl+Q 无反应、
        // 进程不退出"。screen 存在即说明 Loop 正在运行，直接 Exit 即可。
        auto *screen = m_screenPtr.load();
        if (screen != nullptr)
        {
            screen->Exit();
        }
    }

    auto TUI::buildLayout() -> ftxui::Component
    {
        // 关键修复：聊天面板必须随当前会话（m_currentSessionId）动态切换。
        // 若在 buildLayout 时把"启动会话"的 ChatPanel 组件钉死进 mainVertical，
        // 之后切换/新建会话只改 m_currentSessionId 并替换消息，但渲染树仍指向旧面板，
        // 于是新会话界面仍显示上一会话的历史消息。
        // 这里改为"代理组件"：每帧渲染/事件都按 m_currentSessionId 取当前面板，
        // 这样切换会话即时生效，且每个会话各自的 ChatPanel 实例（含消息）独立保留。
        auto chatProxy = ftxui::Renderer([this]
                                         { return getOrCreateChatPanel(m_currentSessionId)->render(); });
        auto chatPanelComponent = chatProxy | ftxui::flex;

        // 布局：Header / 聊天(flex 撑满) / 输入框 / 状态栏，全终端高度。
        auto mainVertical = ftxui::Container::Vertical({
            m_header->getComponent(),
            chatPanelComponent,
            m_inputBox->getComponent(),
            m_statusBar->getComponent(),
        });

        // 模型配置页（-m/--model 或 Ctrl+G 打开）：以 Modal 覆盖层挂在主界面之上。
        // visible 时 Modal 内部 Tab 容器把事件/焦点全量切到配置页——handleKeyEvent
        // 开头分支只截断 Esc/Ctrl+S/Ctrl+Q，其余按键一律返回 false 下沉给配置页
        // （文本编辑/字段导航/按钮），主界面（输入框/会话快捷键）不会被误触。
        auto mainWithModal =
            mainVertical | ftxui::Modal(m_modelConfigPage, &m_modelConfigVisible);

        // 顶层事件处理：键盘快捷键 + 鼠标下沉到当前会话 ChatPanel。
        // 关键：顶层 CatchEvent 是所有子组件的祖先，每个事件（含鼠标）必先经过此处，
        // 故滚轮滚动与思考块点击展开统一在此处理。若挂在各会话 ChatPanel 的
        // CatchEvent 上，因非焦点链祖先（焦点组件是输入框），鼠标事件永远到不了，
        // 会导致思考块点击无反应、滚轮也失效。
        auto eventHandler = ftxui::CatchEvent(mainWithModal, [this](ftxui::Event event)
                                              {
            // 模型配置页（-m/--model / Ctrl+G 打开）事件优先级最高：
            // 文本编辑/字段导航/按钮一律下沉给配置页（返回 false 经 Modal 分发），
            // 仅 Esc 取消、Ctrl+S 保存、Ctrl+Q 退出由本层截断处理。
            if (m_modelConfigVisible)
            {
                if (event == ftxui::Event::Escape)
                {
                    closeModelConfigPage();
                    return true;
                }
                if (event == ftxui::Event::CtrlS)
                {
                    applyModelConfigPage();
                    return true;
                }
                if (event == ftxui::Event::CtrlQ)
                {
                    // 与下方 Ctrl+Q 同口径：先 Exit 主循环再广播退出事件。
                    stop();
                    m_running.store(false);
                    emitEvent(TUIEvent{
                        .type = TUIEvent::Type::QuitRequested,
                        .sessionId = m_currentSessionId,
                    });
                    return true;
                }
                return false; // 其余按键（含字符/方向/Tab）下沉 Modal 配置页
            }

            // 鼠标事件下沉到当前会话的 ChatPanel：滚轮滚动 + 思考块点击展开（单条独立）。
            if (event.is_mouse())
            {
                const auto &mouse = event.mouse();
                auto *panel = getOrCreateChatPanel(m_currentSessionId).get();
                if (mouse.button == ftxui::Mouse::WheelUp)
                {
                    panel->scrollUp(panel->scrollStepLines());
                    // 置位重绘请求：滚轮交互视为"活跃"，throttle 进入高帧率档，滚动更平滑。
                    postCustomEvent();
                    return true;
                }
                if (mouse.button == ftxui::Mouse::WheelDown)
                {
                    panel->scrollDown(panel->scrollStepLines());
                    postCustomEvent();
                    return true;
                }
                // 左键按下：命中某条消息的思考块/编排块头部则翻转该条展开/折叠（单条独立）。
                if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed)
                {
                    if (panel->handleThinkingClick(mouse.x, mouse.y))
                    {
                        postCustomEvent();
                        return true;
                    }
                    if (panel->handleOrchestrationClick(mouse.x, mouse.y))
                    {
                        postCustomEvent();
                        return true;
                    }
                    if (panel->handleToolResultClick(mouse.x, mouse.y))
                    {
                        postCustomEvent();
                        return true;
                    }
                }
                return false;
            }
            if (handleKeyEvent(event))
                return true;
            return false; });

        // 每帧渲染前的安全时机同步 Header（会话/模型/连接变化时重建组件树），
        // 远离 OnEvent 对组件树的遍历，避免事件回调中销毁被遍历的子组件导致野指针崩溃。
        auto root = ftxui::Renderer(eventHandler, [this, eventHandler]
                                    {
            // 终极防御：整帧渲染（含 eventHandler->Render() 的布局/绘制以及覆盖层
            // dbox）若抛异常（含底层 emoji/越界等触发、经 /EHa 转译的非 std 异常），
            // 绝不能逃逸出渲染回调——否则会击穿 screen.Loop，导致整个 TUI 崩溃退出
            // 且终端损坏（光标隐藏 / 残屏 / 重启乱码）。这里兜底返回安全占位帧，
            // 下一帧由 worker 继续刷新，保证流式会话期间 UI 始终存活。
            try
            {
                try
                {
                    if (m_header)
                        m_header->syncComponent();
                }
                catch (...)
                {
                }

                ftxui::Element base = eventHandler->Render();
                const auto &tc = m_themeManager ? m_themeManager->colors() : ThemeManager{}.colors();

                // 底部模型展开列表：dbox 叠加在主界面之上，列表面板贴底显示。
                if (m_modelListVisible)
                {
                    std::vector<ftxui::Element> rows;
                    rows.push_back(ftxui::text("选择模型 (↑/↓ 移动 · Enter 确认 · Esc 取消)") |
                                   ftxui::bold | ftxui::color(tc.accent));
                    rows.push_back(ftxui::separator());
                    for (std::size_t i = 0; i < m_modelList.size(); ++i)
                    {
                        const bool selected = (i == m_modelListIndex);
                        const bool current = (m_modelList[i] == m_currentModel);
                        std::string label = (selected ? "▸ " : "  ") + m_modelList[i];
                        if (current)
                            label += "  ✓";
                        ftxui::Element row = ftxui::text(label);
                        if (selected)
                            row = row | ftxui::bold | ftxui::color(tc.accent) | ftxui::inverted;
                        else if (current)
                            row = row | ftxui::color(tc.success);
                        rows.push_back(row);
                    }
                    auto listPanel = ftxui::window(
                                         ftxui::text(" 模型 "),
                                         ftxui::vbox(std::move(rows)) | ftxui::flex);
                    // 列表贴底：上方 filler 撑开，限制高度避免覆盖整屏。
                    auto bottom = ftxui::vbox({
                        ftxui::filler(),
                        listPanel | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 16),
                    });
                    return ftxui::dbox({base, bottom});
                }

                // Ctrl+H 帮助覆盖层：dbox 叠加在主界面之上，居中显示快捷键说明。
                // 关键：给窗口加不透明背景（bgcolor），否则 dbox 会透视出后方聊天框。
                if (m_helpVisible)
                {
                    auto helpContent = ftxui::vbox({
                        ftxui::text("快捷键") | ftxui::bold | ftxui::color(tc.accent),
                        ftxui::separator(),
                        ftxui::text("Ctrl+H  显示/隐藏本帮助") | ftxui::color(tc.foreground),
                        ftxui::text("Ctrl+T  轮换主题样式") | ftxui::color(tc.foreground),
                        ftxui::text("Ctrl+G  模型配置页（保存写入 demo_config.json）") |
                            ftxui::color(tc.foreground),
                        ftxui::text("Ctrl+N  新建会话") | ftxui::color(tc.foreground),
                        ftxui::text("Ctrl+I  切换意图分析") | ftxui::color(tc.foreground),
                        ftxui::text("Ctrl+P  停止当前生成") | ftxui::color(tc.foreground),
                        ftxui::text("Ctrl+L  清空当前会话") | ftxui::color(tc.foreground),
                        ftxui::text("Ctrl+Q  退出") | ftxui::color(tc.foreground),
                        ftxui::text("输入框：Enter 换行 · Shift+Enter 发送") |
                            ftxui::color(tc.foreground),
                        ftxui::text("PgUp/Dn 滚动历史  Home/End 跳首尾") |
                            ftxui::color(tc.foreground),
                        ftxui::separator(),
                        ftxui::text("按 Esc / Enter / ? 关闭") | ftxui::dim |
                            ftxui::color(tc.muted),
                    });
                    auto helpBox = ftxui::window(ftxui::text(" 帮助 ") | ftxui::bold |
                                                     ftxui::color(tc.foreground),
                                                 helpContent) |
                                   // 不透明背景，隔开后方的聊天框。
                                   ftxui::bgcolor(tc.background) |
                                   ftxui::border |
                                   ftxui::center;
                    return ftxui::dbox({base, helpBox});
                }

                // 删除会话确认对话框：dbox 叠加在主界面之上，居中显示。
                // 交互经 handleKeyEvent 处理（Enter/y 确认，Esc/n 取消）。
                if (m_deleteConfirmVisible)
                {
                    auto dialog = ftxui::vbox({
                        ftxui::text("确认删除该会话？") | ftxui::bold | ftxui::color(tc.accent),
                        ftxui::separator(),
                        ftxui::text("将同时删除该会话的全部聊天记录，") | ftxui::color(tc.foreground),
                        ftxui::text("此操作不可恢复。") | ftxui::color(tc.foreground),
                        ftxui::separator(),
                        ftxui::text("Enter / y  确认删除      Esc / n  取消") | ftxui::dim |
                            ftxui::color(tc.muted),
                    });
                    const std::string title = m_pendingDeleteSessionId.empty()
                                                  ? " 删除会话 "
                                                  : (" 删除会话 " + m_pendingDeleteSessionId + " ");
                    auto dialogBox = ftxui::window(ftxui::text(title) | ftxui::bold |
                                                       ftxui::color(tc.foreground),
                                                   dialog) |
                                     // 不透明背景，隔开后方的聊天框。
                                     ftxui::bgcolor(tc.background) | ftxui::border | ftxui::center;
                    return ftxui::dbox({base, dialogBox});
                }

                return base;
            }
            catch (...)
            {
                // 单帧渲染失败（极罕见，如渲染内容触发底层异常）：返回安全占位帧，
                // 下一帧由 worker 继续刷新，避免整个 TUI 崩溃退出。
                return ftxui::vbox({
                           ftxui::text("[渲染异常：本帧跳过，等待下一帧恢复]") |
                               ftxui::color(ThemeManager{}.colors().error),
                       }) |
                       ftxui::flex;
            }
        });

        return root;
    }

    auto TUI::handleKeyEvent(const ftxui::Event &event) -> bool
    {
        // 键盘滚动兜底：纯 Renderer 的 ChatPanel 不可聚焦，键盘事件会冒泡到此处。
        // 滚轮滚动已由顶层 CatchEvent 处理，这里只兜底键盘。
        {
            auto chatPanel = getOrCreateChatPanel(m_currentSessionId);
            if (event == ftxui::Event::PageUp)
            {
                chatPanel->scrollUp(10);
                return true;
            }
            if (event == ftxui::Event::PageDown)
            {
                chatPanel->scrollDown(10);
                return true;
            }
            if (event == ftxui::Event::Home)
            {
                chatPanel->scrollToTop();
                return true;
            }
            if (event == ftxui::Event::End)
            {
                chatPanel->scrollToBottom();
                return true;
            }
        }

        if (event == ftxui::Event::CtrlQ)
        {
            // 先退出 UI 主循环（screen->Exit()），再通知业务层做退出清理。
            // 注意：必须在 store(false) 之前调用 stop()，否则 stop() 会因
            // m_running 为假而跳过 screen->Exit()，导致主循环不终止、Ctrl+Q 无反应。
            stop();
            m_running.store(false);
            emitEvent(TUIEvent{
                .type = TUIEvent::Type::QuitRequested,
                .sessionId = m_currentSessionId,
            });
            return true;
        }

        if (event == ftxui::Event::CtrlN)
        {
            emitEvent(TUIEvent{
                .type = TUIEvent::Type::NewSession,
                .sessionId = m_currentSessionId,
            });
            return true;
        }

        if (event == ftxui::Event::CtrlL)
        {
            auto chatPanel = getOrCreateChatPanel(m_currentSessionId);
            chatPanel->clearMessages();
            return true;
        }

        // 删除会话确认对话框：Enter/y 确认，Esc/n 取消；打开时吞掉其余按键，避免误触底层组件。
        if (m_deleteConfirmVisible)
        {
            if (event == ftxui::Event::Return ||
                event == ftxui::Event::Character('y') || event == ftxui::Event::Character('Y'))
            {
                const std::string id = m_pendingDeleteSessionId;
                m_deleteConfirmVisible = false;
                m_pendingDeleteSessionId.clear();
                postCustomEvent();
                // demo 层自持会话表：本地删除 + 通知桥接层清理其侧消息存储。
                removeSession(id);
                emitEvent(TUIEvent{.type = TUIEvent::Type::DeleteSession, .sessionId = id});
                return true;
            }
            if (event == ftxui::Event::Escape ||
                event == ftxui::Event::Character('n') || event == ftxui::Event::Character('N'))
            {
                m_deleteConfirmVisible = false;
                m_pendingDeleteSessionId.clear();
                postCustomEvent();
                return true;
            }
            return true; // 对话框打开时吞掉其余按键
        }

        // 底部模型展开列表打开时：方向键移动选择，Enter 确认，Esc 取消。
        if (m_modelListVisible)
        {
            if (m_modelList.empty())
            {
                // 列表尚未就绪（等待桥接层推送），任意键关闭。
                m_modelListVisible = false;
                postCustomEvent();
                return true;
            }
            if (event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k'))
            {
                if (m_modelListIndex > 0)
                    --m_modelListIndex;
                postCustomEvent();
                return true;
            }
            if (event == ftxui::Event::ArrowDown || event == ftxui::Event::Character('j'))
            {
                if (m_modelListIndex + 1 < m_modelList.size())
                    ++m_modelListIndex;
                postCustomEvent();
                return true;
            }
            if (event == ftxui::Event::Return || event == ftxui::Event::Character(' '))
            {
                const std::string chosen = m_modelList[m_modelListIndex];
                m_modelListVisible = false;
                postCustomEvent();
                emitEvent(TUIEvent{.type = TUIEvent::Type::ModelSwitch,
                                   .sessionId = m_currentSessionId,
                                   .payload = chosen});
                return true;
            }
            if (event == ftxui::Event::Escape)
            {
                m_modelListVisible = false;
                postCustomEvent();
                return true;
            }
            return true; // 列表打开时吞掉其余按键，避免误触底层组件
        }

        // Ctrl+H：切换帮助覆盖层。
        // 注意：FTXUI Windows 控制台读取丢弃 Ctrl 修饰键，Ctrl+H 字节 0x08 与 Backspace 相同，
        // 会被解析成 Event::Backspace，Event::CtrlH 收不到真实按键；用 GetAsyncKeyState 探测 Ctrl。
        if (event == ftxui::Event::CtrlH)
        {
            m_helpVisible = !m_helpVisible;
            postCustomEvent();
            return true;
        }
        if (event == ftxui::Event::Backspace && ctrlDownNow())
        {
            m_helpVisible = !m_helpVisible;
            postCustomEvent();
            return true;
        }

        // 帮助覆盖层打开时，任意其他键（Esc/Enter/?）关闭。
        if (m_helpVisible && (event == ftxui::Event::Escape ||
                              event == ftxui::Event::Return ||
                              event == ftxui::Event::Character('?')))
        {
            m_helpVisible = false;
            postCustomEvent();
            return true;
        }

        // Ctrl+G：打开模型配置页（命令行 -m/--model 的运行时等价入口）。
        // 字段预填由 main 响应 ModelConfigOpen 事件完成（注入当前生效配置）。
        if (event == ftxui::Event::CtrlG)
        {
            emitEvent(TUIEvent{
                .type = TUIEvent::Type::ModelConfigOpen,
                .sessionId = m_currentSessionId,
            });
            return true;
        }

        // Ctrl+T：轮换 TUI 主题样式并同步持久化。
        // （原版主题轮换在 Ctrl+D，MCP 开关占用 Ctrl+P；demo 移除 MCP 后，
        //   原 Ctrl+T 思考切换改由 Header 开关按钮承担，Ctrl+T 让位给主题轮换。）
        if (event == ftxui::Event::CtrlT)
        {
            cycleTheme();
            return true;
        }

        // Ctrl+P：请求停止当前生成（demo 层仅广播事件，由桥接层消费执行停止）。
        if (event == ftxui::Event::CtrlP)
        {
            emitEvent(TUIEvent{
                .type = TUIEvent::Type::StopRequested,
                .sessionId = m_currentSessionId,
            });
            return true;
        }

        if (event == ftxui::Event::CtrlI)
        {
            emitEvent(TUIEvent{
                .type = TUIEvent::Type::ToggleIntent,
                .sessionId = m_currentSessionId,
            });
            return true;
        }

        // Ctrl+C：不中断程序，静默忽略（demo 层无 Win32 控制台 API，不做选区复制）。
        if (event == ftxui::Event::CtrlC)
        {
            return true;
        }

        if (event == ftxui::Event::Character('/') || event == ftxui::Event::Escape)
        {
            m_inputBox->takeFocus();
            return true;
        }

        return false;
    }

    auto TUI::setConfig(const TUIConfig &config) -> Core::Result<void>
    {
        // 合并语义：调用方显式提供非空默认模型名才覆盖，避免空配置顶掉持久化值。
        m_config.theme = config.theme;
        m_config.showSidebar = config.showSidebar;
        m_config.showStatusBar = config.showStatusBar;
        m_config.sidebarWidth = config.sidebarWidth;
        m_config.enableMouse = config.enableMouse;
        m_config.enableMarkdown = config.enableMarkdown;
        if (!config.defaultModel.empty())
            m_config.defaultModel = config.defaultModel;
        m_config.refreshRateMs = config.refreshRateMs;
        // 上下文策略字段为纯数据透传（demo 层不驱动面板切换）。
        m_config.contextMode = config.contextMode;
        m_config.contextWindow = config.contextWindow;

        if (m_themeManager)
        {
            m_themeManager->setTheme(config.theme);
        }

        if (m_statusBar)
        {
            m_statusBar->setVisible(config.showStatusBar);
        }

        return Core::Result<void>::Ok();
    }

    auto TUI::getConfig() const -> const TUIConfig &
    {
        return m_config;
    }

    auto TUI::cycleTheme() -> void
    {
        const ThemeVariant current =
            m_themeManager ? m_themeManager->getCurrentTheme() : m_config.theme;
        ThemeVariant next = ThemeVariant::Dark;
        switch (current)
        {
        case ThemeVariant::Dark:
            next = ThemeVariant::Light;
            break;
        case ThemeVariant::Light:
            next = ThemeVariant::Dracula;
            break;
        case ThemeVariant::Dracula:
            next = ThemeVariant::Monokai;
            break;
        case ThemeVariant::Monokai:
            next = ThemeVariant::Solarized;
            break;
        case ThemeVariant::Solarized:
            next = ThemeVariant::Dark;
            break;
        }
        m_config.theme = next;
        if (m_themeManager)
        {
            m_themeManager->setTheme(next);
        }
        persistTUIConfig();
        // 状态栏保持"Ctrl+H  Ctrl+Q"，主题变化不覆盖它（快捷键说明只在帮助覆盖层里）。
        postCustomEvent();
    }

    auto TUI::loadPersistedTheme() -> ThemeVariant
    {
        // 配置读取属于"尽力而为"：任何异常都必须兜底回退默认主题，
        // 绝不允许配置读取失败让 TUI 无法启动。
        const auto j = loadDemoSettingsJson();
        if (j.contains("theme") && j["theme"].is_string())
        {
            return themeVariantFromString(j["theme"].get<std::string>());
        }
        return ThemeVariant::Dark;
    }

    auto TUI::persistTUIConfig() -> void
    {
        // demo_settings.json 扁平结构：{theme, thinking, intent, defaultModel}。
        auto j = loadDemoSettingsJson();
        j["theme"] = themeVariantToString(m_config.theme);
        j["thinking"] = m_thinkingEnabled;
        j["intent"] = m_intentEnabled;
        if (!m_config.defaultModel.empty())
            j["defaultModel"] = m_config.defaultModel;
        saveDemoSettingsJson(j);
    }

    auto TUI::persistedTUIConfig(ThemeVariant &theme, std::string &modelName,
                                 bool &thinking, bool &intent) -> void
    {
        const auto j = loadDemoSettingsJson();
        if (j.contains("theme") && j["theme"].is_string())
            theme = themeVariantFromString(j["theme"].get<std::string>());
        if (j.contains("defaultModel") && j["defaultModel"].is_string())
            modelName = j["defaultModel"].get<std::string>();
        if (j.contains("thinking") && j["thinking"].is_boolean())
            thinking = j["thinking"].get<bool>();
        if (j.contains("intent") && j["intent"].is_boolean())
            intent = j["intent"].get<bool>();
    }

    auto TUI::setDefaultModel(std::string modelName) -> void
    {
        m_config.defaultModel = std::move(modelName);
        persistTUIConfig();
    }

    // ─────────────────────────── 模型配置页 ───────────────────────────

    auto TUI::buildModelConfigPage() -> ftxui::Component
    {
        auto mkInput = [](std::string *content, const char *placeholder, bool password)
        {
            ftxui::InputOption option;
            option.content = content;
            option.placeholder = placeholder;
            option.password = password;
            // 单行字段：Enter 不插换行（on_enter 置空操作；字段间导航走 Tab）。
            option.multiline = false;
            option.on_enter = [] {};
            return ftxui::Input(option);
        };
        m_inputCfgModel = mkInput(&m_cfgModel, "gpt-4o-mini", false);
        m_inputCfgEndpoint = mkInput(&m_cfgEndpoint, "https://api.openai.com/v1", false);
        m_inputCfgApiKey = mkInput(&m_cfgApiKey, "sk-...", true);
        m_inputCfgModelList = mkInput(&m_cfgModelListText, "逗号分隔：gpt-4o-mini, gpt-4o", false);

        // 模式开关（手写两态 < chat > / < agent >，按键全控）：
        // ←/→/h/l 直接切换，Enter/Space 反向切换；Tab/Shift+Tab 返回 false 放行给
        // 外层 Vertical 容器做字段间导航（FTXUI Menu 系组件会自行吞掉 Tab 循环条目，
        // 因此这里不用 ftxui::Toggle）。
        auto modeRow = ftxui::Renderer([this]
                                       {
            const std::string text = (m_cfgModeIndex == 0) ? "< chat >" : "< agent >";
            if (m_themeManager)
            {
                return ftxui::text(text) | ftxui::bold |
                       ftxui::color(m_themeManager->colors().accent);
            }
            return ftxui::text(text) | ftxui::bold; });
        m_toggleCfgMode = ftxui::CatchEvent(modeRow, [this](ftxui::Event event)
                                            {
            if (event == ftxui::Event::ArrowLeft || event == ftxui::Event::Character('h'))
            {
                m_cfgModeIndex = 0;
                postCustomEvent();
                return true;
            }
            if (event == ftxui::Event::ArrowRight || event == ftxui::Event::Character('l'))
            {
                m_cfgModeIndex = 1;
                postCustomEvent();
                return true;
            }
            if (event == ftxui::Event::Return || event == ftxui::Event::Character(' '))
            {
                m_cfgModeIndex = 1 - m_cfgModeIndex;
                postCustomEvent();
                return true;
            }
            return false; });

        m_btnCfgSave = ftxui::Button("保存并应用", [this] { applyModelConfigPage(); });
        m_btnCfgCancel = ftxui::Button("取消", [this] { closeModelConfigPage(); });

        // 扁平 Vertical：输入框×4 + 模式开关 + 按钮行；非聚焦子项（无）不参与 Tab 环。
        m_modelConfigContainer = ftxui::Container::Vertical({
            m_inputCfgModel,
            m_inputCfgEndpoint,
            m_inputCfgApiKey,
            m_inputCfgModelList,
            m_toggleCfgMode,
            ftxui::Container::Horizontal({m_btnCfgSave, m_btnCfgCancel}),
        });
        m_modelConfigPage = ftxui::Renderer(m_modelConfigContainer,
                                            [this] { return renderModelConfigPage(); });
        return m_modelConfigPage;
    }

    auto TUI::renderModelConfigPage() -> ftxui::Element
    {
        const auto &tc = m_themeManager->colors();

        // 字段行：定宽标签 + 自适应输入框（输入框组件自行渲染，光标/聚焦态由 FTXUI 管理）。
        auto label = [&tc](const std::string &text)
        {
            return ftxui::text(text) | ftxui::bold | ftxui::color(tc.accent) |
                   ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 10);
        };
        auto field = [](ftxui::Element element)
        {
            return element | ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, 20) |
                   ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 44);
        };

        const ftxui::Element content = ftxui::vbox({
            ftxui::hbox({label("模型"), field(m_inputCfgModel->Render())}),
            ftxui::hbox({label("端点"), field(m_inputCfgEndpoint->Render())}),
            ftxui::hbox({label("API Key"), field(m_inputCfgApiKey->Render())}),
            ftxui::hbox({label("模型列表"), field(m_inputCfgModelList->Render())}),
            ftxui::hbox({label("模式"), focusedCfgModeElement()}),
            ftxui::separator(),
            ftxui::hbox({m_btnCfgSave->Render(), ftxui::text("   "), m_btnCfgCancel->Render()}),
            ftxui::separator(),
            ftxui::text("Tab/Shift+Tab 切换字段 · ←/→ 切换模式 · Enter 激活按钮") |
                ftxui::dim | ftxui::color(tc.muted),
            ftxui::text("Ctrl+S 保存并应用 · Esc 取消（保存写入 exe 同目录 demo_config.json）") |
                ftxui::dim | ftxui::color(tc.muted),
        });

        return ftxui::window(ftxui::text(" 模型配置 ") | ftxui::bold | ftxui::color(tc.accent),
                             content) |
               ftxui::bgcolor(tc.background) | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 72);
    }

    auto TUI::focusedCfgModeElement() -> ftxui::Element
    {
        // 模式行聚焦高亮：输入框/按钮的聚焦态由各自组件渲染，模式行为手写组件，
        // 这里按 Focused() 链补一层 inverted，保证字段环游时有明确视觉焦点。
        ftxui::Element element = m_toggleCfgMode ? m_toggleCfgMode->Render() : ftxui::text("");
        if (m_toggleCfgMode && m_toggleCfgMode->Focused())
        {
            element = element | ftxui::inverted;
        }
        return element;
    }

    auto TUI::openModelConfigPage() -> void
    {
        if (m_modelConfigVisible)
        {
            return;
        }
        m_modelConfigVisible = true;
        // 焦点进入"模型"输入框：TakeFocus 沿父链激活所在行；Modal 的 Tab 容器也会在
        // 下一个事件/帧把选择切到配置页（OnEvent/OnRender 均会同步 selector_）。
        if (m_inputCfgModel)
        {
            m_inputCfgModel->TakeFocus();
        }
        postCustomEvent();
    }

    auto TUI::closeModelConfigPage() -> void
    {
        if (!m_modelConfigVisible)
        {
            return;
        }
        m_modelConfigVisible = false;
        postCustomEvent();
        // 焦点还给聊天输入框（与 '/' / Esc 的聚焦口径一致）。
        if (m_inputBox)
        {
            m_inputBox->takeFocus();
        }
    }

    auto TUI::isModelConfigPageVisible() const -> bool
    {
        return m_modelConfigVisible;
    }

    auto TUI::setModelConfigDefaults(std::string endpoint, std::string apiKey,
                                     std::string model, bool agentMode,
                                     std::vector<std::string> modelList) -> void
    {
        m_cfgEndpoint = std::move(endpoint);
        m_cfgApiKey = std::move(apiKey);
        m_cfgModel = std::move(model);
        m_cfgModeIndex = agentMode ? 1 : 0;
        // 模型列表编辑态：逗号分隔；为空回退当前模型（保证列表至少一项）。
        if (modelList.empty())
        {
            modelList.push_back(m_cfgModel);
        }
        std::string joined;
        for (std::size_t i = 0; i < modelList.size(); ++i)
        {
            if (i != 0)
            {
                joined += ", ";
            }
            joined += modelList[i];
        }
        m_cfgModelListText = std::move(joined);
        postCustomEvent();
    }

    auto TUI::applyModelConfigPage() -> void
    {
        const std::string model = trimCopy(m_cfgModel);
        const std::string endpoint = trimCopy(m_cfgEndpoint);
        if (model.empty() || endpoint.empty())
        {
            // 必填校验失败：保持页面打开，系统消息提示（不写盘、不应用）。
            Message tip;
            tip.id = makeMessageId();
            tip.role = MessageRole::System;
            tip.status = MessageStatus::Completed;
            tip.content = model.empty() ? "⚠ 模型名不能为空，已取消保存（配置页保持打开）。"
                                        : "⚠ 端点不能为空，已取消保存（配置页保持打开）。";
            addMessage(m_currentSessionId, tip);
            return;
        }

        // 模型列表：逗号分隔解析（兼容中文逗号）；为空回退为 [当前模型]。
        std::vector<std::string> modelList = splitCsv(m_cfgModelListText);
        if (modelList.empty())
        {
            modelList.push_back(model);
        }
        // 去重（保留首次出现顺序）：多模型统一入 demo_config.json，面板列表不出现重复项。
        std::vector<std::string> deduped;
        for (const auto &entry : modelList)
        {
            if (std::find(deduped.begin(), deduped.end(), entry) == deduped.end())
            {
                deduped.push_back(entry);
            }
        }
        modelList = std::move(deduped);

        nlohmann::json data;
        data["endpoint"] = endpoint;
        data["apiKey"] = trimCopy(m_cfgApiKey);
        data["model"] = model;
        data["agentMode"] = (m_cfgModeIndex == 1);
        data["modelList"] = modelList;

        closeModelConfigPage();
        // 完整配置经事件回传 main：按需落盘 demo_config.json + 热应用桥接层。
        emitEvent(TUIEvent{
            .type = TUIEvent::Type::ModelConfigApply,
            .sessionId = m_currentSessionId,
            .data = std::move(data),
        });
    }

    auto TUI::addMessage(const std::string &sessionId, const Message &message) -> void
    {
        auto chatPanel = getOrCreateChatPanel(sessionId);
        chatPanel->addMessage(message);
        postCustomEvent();
    }

    auto TUI::setMessages(const std::string &sessionId, std::vector<Message> messages) -> void
    {
        auto chatPanel = getOrCreateChatPanel(sessionId);
        chatPanel->setMessages(std::move(messages));
        postCustomEvent();
    }

    auto TUI::updateStreamingMessage(const std::string &sessionId,
                                     const std::string &messageId,
                                     std::string_view delta) -> void
    {
        auto chatPanel = getOrCreateChatPanel(sessionId);
        chatPanel->updateStreamingMessage(messageId, delta);
        postCustomEvent();
    }

    auto TUI::setMessageContent(const std::string &sessionId,
                                const std::string &messageId,
                                std::string_view content) -> bool
    {
        auto chatPanel = getOrCreateChatPanel(sessionId);
        auto result = chatPanel->setMessageContent(messageId, content);
        postCustomEvent();
        return result;
    }

    auto TUI::finishStreamingMessage(const std::string &sessionId,
                                     const std::string &messageId) -> void
    {
        auto chatPanel = getOrCreateChatPanel(sessionId);
        chatPanel->finishStreamingMessage(messageId);
        postCustomEvent();
    }

    auto TUI::setThinkingContent(const std::string &sessionId,
                                 const std::string &messageId,
                                 std::string_view thinking) -> void
    {
        auto chatPanel = getOrCreateChatPanel(sessionId);
        chatPanel->setThinkingContent(messageId, thinking);
        postCustomEvent();
    }

    auto TUI::setOrchestrationInfo(const std::string &sessionId,
                                   const std::string &messageId,
                                   OrchestrationInfo info) -> void
    {
        auto chatPanel = getOrCreateChatPanel(sessionId);
        chatPanel->setOrchestrationInfo(messageId, std::move(info));
        postCustomEvent();
    }

    auto TUI::setMessageTokens(const std::string &sessionId, const std::string &messageId,
                               int32_t inputTokens, int32_t outputTokens,
                               int32_t reasoningTokens, int32_t totalTokens) -> void
    {
        auto chatPanel = getOrCreateChatPanel(sessionId);
        chatPanel->setMessageTokens(messageId, inputTokens, outputTokens, reasoningTokens, totalTokens);
        postCustomEvent();
    }

    auto TUI::setLastUserMessageTokens(const std::string &sessionId, int32_t inputTokens) -> void
    {
        auto chatPanel = getOrCreateChatPanel(sessionId);
        chatPanel->setLastUserMessageTokens(inputTokens);
        postCustomEvent();
    }

    auto TUI::addSession(const SessionInfo &session) -> void
    {
        {
            std::scoped_lock lock(m_stateMutex);
            auto it = std::find_if(m_sessions.begin(), m_sessions.end(),
                                   [&](const SessionInfo &s) { return s.id == session.id; });
            if (it == m_sessions.end())
            {
                m_sessions.push_back(session);
            }
            else
            {
                *it = session;
            }
            if (m_currentSessionId.empty())
            {
                m_currentSessionId = session.id;
            }
        }
        refreshSessionViews();
        postCustomEvent();
    }

    auto TUI::removeSession(const std::string &sessionId) -> void
    {
        {
            std::scoped_lock lock(m_stateMutex);
            m_chatPanels.erase(sessionId);
            std::erase_if(m_sessions, [&](const SessionInfo &s) { return s.id == sessionId; });

            // 若删除的是当前会话：切到剩余第一个。
            if (m_currentSessionId == sessionId)
            {
                m_currentSessionId = m_sessions.empty() ? std::string{} : m_sessions.front().id;
            }
        }

        // 关键修复：先释放 m_stateMutex（上面 scoped_lock 作用域已结束），再调用
        // refreshSessionViews()——它内部会再次锁定 m_stateMutex。若持锁调用会触发
        // 同一线程对非递归互斥量的递归加锁，抛出 "resource deadlock would occur"。
        refreshSessionViews();
        postCustomEvent();
    }

    auto TUI::switchSession(const std::string &sessionId) -> void
    {
        {
            std::scoped_lock lock(m_stateMutex);
            m_currentSessionId = sessionId;
            for (auto &s : m_sessions)
            {
                s.isActive = (s.id == sessionId);
            }
        }

        if (m_header)
        {
            m_header->setActiveSession(sessionId);
        }

        // demo 层为纯视图：切换会话不回读历史（原版经 SQLite loadSessionMessages），
        // 消息列表由桥接层监听 SwitchSession 事件后经 setMessages 推入。
        emitEvent(TUIEvent{
            .type = TUIEvent::Type::SwitchSession,
            .sessionId = sessionId,
        });

        postCustomEvent();
    }

    auto TUI::getCurrentSessionId() const -> std::string
    {
        return m_currentSessionId;
    }

    auto TUI::ensureDefaultSession() -> void
    {
        {
            std::scoped_lock lock(m_stateMutex);
            if (!m_sessions.empty())
            {
                return;
            }
        }
        SessionInfo info;
        info.id = makeMessageId();
        info.title = "会话 1";
        info.createdAt = std::chrono::system_clock::now();
        info.updatedAt = info.createdAt;
        info.isActive = true;
        addSession(info);
    }

    auto TUI::refreshSessionViews() -> void
    {
        SessionList snapshot;
        std::string current;
        {
            std::scoped_lock lock(m_stateMutex);
            snapshot = m_sessions;
            current = m_currentSessionId;
            for (auto &s : snapshot)
            {
                s.isActive = (s.id == current);
            }
        }

        if (m_header)
        {
            m_header->setSessions(snapshot);
            if (!current.empty())
            {
                m_header->setActiveSession(current);
            }
        }

        // 注意：不可在持有 m_stateMutex 的情况下调用 getOrCreateChatPanel()
        // （其内部会再次锁定同一非递归互斥量，导致 "resource deadlock would occur"）。
        // 此处锁已在上方块结束，故安全。
        if (!current.empty())
        {
            getOrCreateChatPanel(current);
        }
    }

    auto TUI::setStatusItems(const std::vector<StatusItem> &items) -> void
    {
        m_statusBaseItems = items;
        refreshStatusBar();
    }

    auto TUI::setModelStatus(const std::string &model,
                             bool thinkingEnabled,
                             bool intentEnabled) -> void
    {
        // 同步 UI 状态：思考/意图开关在 Header 中。
        // 当前模型缓存用于底部状态栏显示与模型列表高亮。
        m_thinkingEnabled = thinkingEnabled;
        m_intentEnabled = intentEnabled;
        m_currentModel = model;
        if (m_header)
        {
            m_header->setThinking(thinkingEnabled);
            m_header->setIntent(intentEnabled);
        }

        // 底部状态栏显示当前模型名（点击可展开模型列表选择）。
        // 直接持久化到 m_statusBaseItems，避免 refreshStatusBar 重建时丢失。
        {
            bool replaced = false;
            for (auto &it : m_statusBaseItems)
            {
                if (it.label.rfind("📦", 0) == 0)
                {
                    it.value = model;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
            {
                m_statusBaseItems.insert(m_statusBaseItems.begin(),
                                         StatusItem{.label = "📦", .value = model});
            }
            if (m_statusBar)
            {
                m_statusBar->setItems(m_statusBaseItems);
            }
        }

        // 若模型列表已展开，刷新高亮。
        if (m_modelListVisible)
        {
            syncModelListSelection();
        }
    }

    auto TUI::setModelList(const std::vector<std::string> &models) -> void
    {
        m_modelList = models;
        syncModelListSelection();
    }

    auto TUI::syncModelListSelection() -> void
    {
        if (m_modelList.empty())
        {
            m_modelListIndex = 0;
            return;
        }
        // 默认选中当前模型；找不到则停在 0。
        auto it = std::find(m_modelList.begin(), m_modelList.end(), m_currentModel);
        m_modelListIndex = (it != m_modelList.end())
                               ? static_cast<std::size_t>(it - m_modelList.begin())
                               : 0;
    }

    auto TUI::openModelList() -> void
    {
        // 模型列表为空（桥接层尚未推送）→ 先触发一次刷新事件，下一帧再打开。
        if (m_modelList.empty())
        {
            emitEvent(TUIEvent{.type = TUIEvent::Type::ModelSwitch,
                               .sessionId = m_currentSessionId});
        }
        syncModelListSelection();
        m_modelListVisible = true;
        postCustomEvent();
    }

    auto TUI::setConnected(bool connected) -> void
    {
        if (m_header)
        {
            m_header->setConnected(connected);
        }
        if (m_statusBar)
        {
            m_statusBar->setConnected(connected);
        }
    }

    auto TUI::refreshStatusBar() -> void
    {
        if (!m_statusBar)
        {
            return;
        }
        // 基础项已含 setModelStatus 写入的模型项，直接整体下发。
        m_statusBar->setItems(m_statusBaseItems);
    }

    auto TUI::setTools(const ToolList &tools) -> void
    {
        // demo 层暂不渲染工具列表；保留数据供后续阶段在 Header/下拉展示。
        m_tools = tools;
    }

    auto TUI::registerEventHandler(EventCallback callback) -> void
    {
        std::scoped_lock lock(m_eventMutex);
        m_eventHandlers.push_back(std::move(callback));
    }

    auto TUI::unregisterAllEventHandlers() -> void
    {
        std::scoped_lock lock(m_eventMutex);
        m_eventHandlers.clear();
    }

    auto TUI::isRunning() const -> bool
    {
        return m_running.load();
    }

    auto TUI::emitEvent(const TUIEvent &event) -> void
    {
        // 关键修复：先持锁拷贝 handler 快照，再释放锁后调用 handler。
        // 若持 m_eventMutex 期间直接调用 handler，而业务 handler
        // （如桥接层处理 NewSession）会同步回调用 emitEvent
        // （经 switchSession → emitEvent(SwitchSession)），导致同一线程
        // 对非递归互斥量 m_eventMutex 递归加锁，抛出 "resource deadlock would occur"。
        // 释放锁后再调用 handler 既消除重入死锁，也避免 handler 长时间运行阻塞
        // 事件注册/注销。
        std::vector<EventCallback> handlers;
        {
            std::scoped_lock lock(m_eventMutex);
            handlers = m_eventHandlers;
        }
        for (const auto &handler : handlers)
        {
            if (handler)
            {
                handler(event);
            }
        }
    }

    auto TUI::onInputSubmit(const std::string &text) -> void
    {
        if (text.empty())
        {
            return;
        }

        Message userMsg;
        userMsg.id = makeMessageId();
        userMsg.role = MessageRole::User;
        userMsg.content = text;
        userMsg.status = MessageStatus::Completed;

        addMessage(m_currentSessionId, userMsg);

        emitEvent(TUIEvent{
            .type = TUIEvent::Type::UserInput,
            .sessionId = m_currentSessionId,
            .payload = text,
        });
    }

    auto TUI::postCustomEvent() -> void
    {
        // 仅置位重绘请求；真正的 ftxui screen.Post 由唯一 throttle 线程执行，
        // 避免 worker 线程并发触碰 ftxui 内部 screen 状态导致 resource deadlock / 闪烁。
        m_frameRequested.store(true, std::memory_order_release);
    }

    auto TUI::getOrCreateChatPanel(const std::string &sessionId) -> std::shared_ptr<ChatPanel>
    {
        std::scoped_lock lock(m_stateMutex);
        auto panel = getOrCreateChatPanelLocked(sessionId);
        // 仅当请求的是当前活动会话时，刷新 throttle 线程的自适应帧率信号。
        if (panel && sessionId == m_currentSessionId)
        {
            m_activeChatPanel.store(panel.get(), std::memory_order_release);
        }
        return panel;
    }

    auto TUI::getOrCreateChatPanelLocked(const std::string &sessionId) -> std::shared_ptr<ChatPanel>
    {
        auto it = m_chatPanels.find(sessionId);
        if (it != m_chatPanels.end())
        {
            return it->second;
        }

        auto panel = std::make_shared<ChatPanel>(m_themeManager);
        m_chatPanels[sessionId] = panel;
        return panel;
    }

} // namespace AE::TUI
