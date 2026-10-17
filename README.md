# AE::Engine (VXAE)

独立、跨平台、C++26 的 **Agent Engine** 库：LLM 流式对话 + 多轮工具调用主循环 + 安全/策略/会话/遥测基元，附带基于 [FTXUI](https://github.com/ArthurSonzogni/FTXUI) 的终端聊天 Demo。

- 命名空间：`AE::Engine`（引擎）、`AE::Core`（自带最小基座）、`AE::TUI`（demo）
- 零项目耦合：不依赖任何宿主工程；运行时第三方依赖**只有 libcurl**
- 跨平台：Windows / Linux / macOS（平台差异全部条件编译守卫 + 标准库回退）
- 头文件为主（header-first）+ 4 个功能 `.cpp`，以 STATIC 库 `vxae` 整体消费

# 未来实现目标（暂未实现）
 - 支持统一输出接口格式，衔接Markdown输出接口
 - 支持更多工具（函数调用、文件操作、系统调用等），提供模块化工具池
 - 提供记忆引擎接口，支持上下文记忆、会话记忆、全局记忆，模块化设计
 - 敬请期待


## 能力一览

| 模块 | 头/源文件 | 说明 |
|---|---|---|
| Agent 主循环 | `AgentEngine.hpp`（薄壳）/ `Mode/ReactMode.hpp` / `AE.hpp`（聚合伞头） | 多轮工具调用、迭代上限、优雅停止（stop token）、hooks；主循环在 ReAct 模式内 |
| 推理模式 | `Mode/`（`ModeContext.hpp` / `IReasoningMode.hpp` / `ReactMode.hpp` / `Plan*.hpp` / `IReflector.hpp` / `ReflectionMode.hpp`） | `ReasoningMode` 编译期契约 + `ModeContext`；ReAct / Plan-and-Execute（`IPlanner`+`IStepExecutor`+`IAggregator`）/ Reflection（零开销装饰器 + 无效循环熔断） |
| 多 Agent 协作 | `Collab/`（`AgentRuntime.hpp` / `AgentRegistry.hpp` / `MessageBus.hpp` / `CollaborationEvent.hpp` / `TaskAssigner.hpp` / `Critic.hpp` / `CollabSelector.hpp`） | 6 协作模式（Orchestrator-Worker / Supervisor / GroupChat / Debate / Peer-to-Peer / Sequential Workflow）+ 独立 `CollaborationEvent` IR + 能力分配/策略选择 |
| 框架→模式映射 | `Mapping/`（`Primitives/Graph.hpp` / `RoleTask.hpp` / `Conversation.hpp` / `Flow.hpp` / `Router.hpp` / `VisualSpec.hpp` / `PatternBuilder.hpp`） | 可组合原语（图/角色/对话/数据流/Handoff）+ `Patterns` 6 模式构建器 + `VisualSpec` 可序列化拓扑（低代码接入） |
| 外部扩展 Ports | `Ports/`（`ToolPort.hpp` / `McpTransport.hpp` / `McpHttpClient.hpp` / `MemoryPort.hpp` / `ExecutorPort.hpp` / `HumanPort.hpp` / `ModelPort.hpp`） | 纯接口 + 默认实现；MCP JSON-RPC over HTTP 参考实现 |
| 外部实现参考 | `Ports/`（`InMemoryStore.hpp` / `ProcessExecutor.hpp` / `ScriptedHumanGate.hpp`） | P7 参考实现：KV/TTL/打分召回记忆后端（+长期/共享/工作三层适配）、`LocalProcessExecutor`+`AllowListSandbox`、`ScriptedHumanGate` 决策队列 |
| 流式客户端 | `StreamingLLMClient.hpp` | libcurl SSE、重试/退避、`LlmDelta` IR（Text/Thinking/Usage/Error） |
| 后端适配 | `LLMAdapter.hpp` / `AEManager.hpp` | OpenAI 兼容 / vLLM / Ollama 统一 IR，上层无感知；`AEManager` 为 AE 工厂/管理器 |
| 配置层 | `AEConfig.hpp` | JSON 配置（nlohmann），密钥经 `SecureString` |
| 会话存储 | `AESessionStore.*` | `IStateStore` 实现，落盘/恢复对话状态 |
| 工具面 | `AEType.hpp` / `AESimple.hpp` / `AEBuiltinTools.*` / `NullToolProvider.hpp` | `IToolProvider` 抽象 + `SimpleToolProvider` + 内建工具 |
| 安全 | `AESecurity.hpp`、`AESecurityGuard.*`、`AESanitize.hpp` | SSRF/出站防护、路径校验、Shell 注入防护、提示注入净化 |
| 策略 | `AEToolPolicy.*` | 工具执行权限门控（`IPermissionGate`） |
| 遥测 | `AETelemetry.hpp` | 语义事件 + 可插拔 `IAEEventSink`，trace id 标准库随机 hex |
| 上下文/计量 | `TokenCounter.hpp` | token 近似计数、上下文窗口探测 |
| 录制回放 | `AERecordReplay.hpp` | HAR 式 Record/Replay 注入 |
| 自带基座 | `src/Core/` | `Result`（`std::expected`）、`Error`、`Logger`（`std::print` 原生）、`SecureString`、`SseParser`、`HttpCurl` |

## 依赖与致谢

| 库 | 用途 | 许可 | 链接 |
|---|---|---|---|
| [libcurl](https://curl.se/) | HTTP/SSE 传输（唯一运行时依赖） | curl License（MIT 兼容） | [curl/curl](https://github.com/curl/curl) |
| [nlohmann/json](https://json.nlohmann.me/) | 类型/协议层，**v3.12.0 单头内置**于 `vendor/nlohmann-json.hpp`，不经包管理 | MIT | [nlohmann/json](https://github.com/nlohmann/json) |
| [FTXUI](https://ftxui.cpp.dev/) | demo 终端 UI（仅 `vxae_demo` 消费，核心库不受影响） | Apache-2.0 | [ArthurSonzogni/FTXUI](https://github.com/ArthurSonzogni/FTXUI) |
| [vcpkg](https://vcpkg.io/) | 依赖管理（manifest 模式，见 `vcpkg.json`） | MIT | [microsoft/vcpkg](https://github.com/microsoft/vcpkg) |

同时致谢 C++26 标准库（`std::expected` / `std::generator` / `std::print` / `std::jthread`）让本库得以去掉 spdlog/fmt/yaml-cpp/stduuid 等传统依赖。
demo 的 TUI 布局移植自本团队前作 `puppeteer-vmrx`，接线层已完全重写为本库 API。

## 构建

前置：CMake ≥ 3.25、Ninja、**LLVM 22+（clang-cl 作为前端 / lld-link 作为后端）**，仓库内 `vcpkg/` checkout（或自备 vcpkg 并改 `CMakePresets.json` 中工具链路径）。
标准固定在 **C++26**（各目标 `CXX_STANDARD 26`；clang-cl 下 CMake 映射为 `-std:c++latest`，实测 `__cplusplus == 202400`）。

> 工具链走 MSVC ABI（`x86_64-pc-windows-msvc`），因此 vcpkg 的 `x64-windows` 预编译依赖可原样复用，无需重建。
> MSVC 自带工具链不再参与：归档用 `llvm-lib`，链接用 `lld-link`，资源/清单用 `llvm-rc` / `llvm-mt`。

### 使用 CMake Presets（推荐，兼容 IDE CMake Tools 插件）

```bash
cmake --preset llvm          # 配置：clang-cl + lld-link + C++26，核心库 + demo
cmake --build --preset llvm-debug     # Debug：vxae / vxae_check / vxae_demo
cmake --build --preset llvm-release   # Release
cmake --preset llvm-lib-only && cmake --build --preset llvm-lib-debug  # 仅核心库，不拉 ftxui
```

产物位于 `build/llvm/{Debug,Release}/`（demo：`build/llvm/demo/Debug/vxae_demo.exe`）。

`default` / `lib-only` 两组 preset 仍保持“环境默认编译器（MSVC cl）”的行为，便于对照。

### 手动命令（不经 preset）

```bash
cmake -S . -B build/llvm -G "Ninja Multi-Config" `
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake -DAE_BUILD_DEMO=ON `
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" `
  -DCMAKE_LINKER="C:/Program Files/LLVM/bin/lld-link.exe" `
  -DCMAKE_AR="C:/Program Files/LLVM/bin/llvm-lib.exe" `
  -DCMAKE_RC_COMPILER="C:/Program Files/LLVM/bin/llvm-rc.exe" `
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/llvm --config Debug --target vxae_demo
```

首次 configure 时 vcpkg manifest 会自动解析并构建 `curl`、`ftxui`（Linux/macOS 从源码构建，耗时视机器而定）。

### clangd

preset 已开启 `CMAKE_EXPORT_COMPILE_COMMANDS`，编译数据库落在 `build/llvm/compile_commands.json`；仓库根目录 `.clangd` 已通过 `CompilationDatabase` 指向它，新版 clangd（本仓验证版本 22.1.8）开箱可用，无需 `--compile-commands-dir`。

### 目标一览

| 目标 | 形态 | 说明 |
|---|---|---|
| `vxae` | STATIC | 核心引擎库（4 个功能 `.cpp`，PUBLIC 传播 include 与 curl 依赖） |
| `vxae_check` | STATIC | 全头文件编译探针（构建期校验，不参与常规链接） |
| `vxae_tui` | STATIC | demo UI 层（FTXUI） |
| `vxae_demo` | EXE | 终端聊天演示 |

## Demo 使用

### 配置 API Key（三选一，优先级从高到低）

1. 环境变量：`VXAE_ENDPOINT` / `VXAE_API_KEY` / `VXAE_MODEL` / `VXAE_AGENT_MODE`（`1`/`true` 开启 Agent 全循环）
2. 可执行文件同目录 `demo_config.json`：

```json
{
  "endpoint": "https://api.openai.com/v1",
  "apiKey": "sk-...",
  "model": "gpt-4o-mini",
  "agentMode": false,
  "modelList": ["gpt-4o-mini", "gpt-4o"]
}
```

该文件也可由模型配置页（`-m`/`--model` 或运行时 `Ctrl+G` 的"保存并应用"）自动生成/更新。

3. 内置默认（endpoint `https://api.openai.com/v1`、model `gpt-4o-mini`、无 key —— 连接灯红色并提示）。

### 运行

```bash
build/llvm/Debug/vxae_demo                 # Linux/macOS
build\llvm\Debug\vxae_demo.exe             # Windows
build\llvm\Debug\vxae_demo.exe -m          # 启动即进入模型配置页
build\llvm\Debug\vxae_demo.exe -m gpt-4o   # 以 gpt-4o 预填"模型"字段
build\llvm\Debug\vxae_demo.exe --help      # 查看全部命令行选项
```

- **chat 模式（默认）**：单轮流式对话（`StreamingLLMClient`），SSE 增量/思考块/真实 token 统计；
- **agent 模式**：`AgentEngine` 完整循环，多轮工具调用（内置演示工具 `current_time`），工具消息可折叠回填；
- **推理 / 协作模式（P8）**：agent 模式下 `Ctrl+R` 循环单 Agent 推理模式（`react`→`plan`→`reflection`）、`Ctrl+B` 循环多 Agent 协作模式（`off`→`orchestrator`→`supervisor`→`group_chat`→`debate`→`peer_to_peer`→`sequential`）；助手消息内回填编排信息块（推理模式 + 协作拓扑 + 失败降级说明），失败路径可见；也可用环境变量 `VXAE_REASONING_MODE`/`VXAE_COLLAB_MODE` 或 `demo_config.json` 预设；
- **模型配置页**：未配置任何模型列表时启动**自动弹出**（引导先配模型；已配置则直接进聊天）；也可用启动参数 `-m`/`--model` 或运行时 `Ctrl+G` 打开。编辑模型/端点/API Key/模式/模型列表，"保存并应用"按需写入 exe 同目录 `demo_config.json` 并立即生效（写盘失败时仅本会话生效并给出提示）；
- **多模型**：配置页"模型列表"字段逗号分隔（如 `gpt-4o-mini, gpt-4o, qwen-plus`），统一持久化到 `demo_config.json` 的 `modelList`；底部状态栏点击模型名（`Model: xxx`）展开面板列表，`↑/↓` 选择、`Enter` 切换，当前模型以 ✓ 高亮，切换后同步状态栏、默认模型与后续请求；
- 快捷键：`Shift+Enter` 发送、`Enter` 换行、`Ctrl+P` 停止当前请求、`Ctrl+R` 循环推理模式、`Ctrl+B` 循环协作模式、`Ctrl+G` 模型配置页、`Ctrl+H` 帮助、`Ctrl+T` 主题轮换、`Ctrl+Q` 退出；
- 主题/模型选择持久化在 exe 同目录 `demo_settings.json`；模型配置页保存的是 `demo_config.json`（环境变量优先级仍高于它）。

## 在你的工程中使用

```cmake
add_subdirectory(external/vxae)            # 或 git submodule / FetchContent
target_link_libraries(your_target PRIVATE vxae)
target_compile_features(your_target PRIVATE cxx_std_26)
```

```cpp
#include "AE/AE.hpp"                 // AgentEngine
#include "AE/StreamingLLMClient.hpp" // 或直接流式客户端
#include "AE/AEConfig.hpp"           // JSON 配置
```

要点：C++26 必需（clang-cl 映射 `-std:c++latest`）；include 根为 `src/`、`src/AE/`、`src/vendor/`；`UUID_SYSTEM_GENERATOR`/`Ole32` 等历史包袱已不存在。

## 许可

本项目以 [Apache-2.0](LICENSE) 授权。第三方库遵循其各自许可：libcurl（curl License）、nlohmann/json（MIT，随仓库内置单头文件）、FTXUI（Apache-2.0）、vcpkg（MIT）。
