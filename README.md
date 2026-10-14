# AE::Engine (VXAE)

独立、跨平台、C++23 的 **Agent Engine** 库：LLM 流式对话 + 多轮工具调用主循环 + 安全/策略/会话/遥测基元，附带基于 [FTXUI](https://github.com/ArthurSonzogni/FTXUI) 的终端聊天 Demo。

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
| Agent 主循环 | `AE.hpp`（`AgentEngine`） | 多轮工具调用、迭代上限、优雅停止（stop token）、hooks |
| 流式客户端 | `StreamingLLMClient.hpp` | libcurl SSE、重试/退避、`LlmDelta` IR（Text/Thinking/Usage/Error） |
| 后端适配 | `LLMAdapter.hpp` | OpenAI 兼容 / vLLM / Ollama 统一 IR，上层无感知 |
| 配置层 | `AEConfig.hpp` | JSON 配置（nlohmann），密钥经 `SecureString` |
| 会话存储 | `AESessionStore.*` | `IStateStore` 实现，落盘/恢复对话状态 |
| 工具面 | `IAE.hpp` / `AEBuiltinTools.*` / `NullToolProvider.hpp` / `SimpleToolProvider` | `IToolProvider` 抽象 + 内建工具 |
| 安全 | `AESecurity.hpp`、`AESecurityGuard.*`、`AESanitize.hpp` | SSRF/出站防护、路径校验、Shell 注入防护、提示注入净化 |
| 策略 | `AEToolPolicy.*` | 工具执行权限门控（`IPermissionGate`） |
| 遥测 | `AETelemetry.hpp` | 语义事件 + 可插拔 `IAEEventSink`，trace id 标准库随机 hex |
| 上下文/计量 | `TokenCounter.hpp` | token 近似计数、上下文窗口探测 |
| 录制回放 | `AERecordReplay.hpp` | HAR 式 Record/Replay 注入 |
| 自带基座 | `include/Core/` | `Result`（`std::expected`）、`Error`、`Logger`（`std::print` 原生）、`SecureString`、`SseParser`、`HttpCurl` |

## 依赖与致谢

| 库 | 用途 | 许可 | 链接 |
|---|---|---|---|
| [libcurl](https://curl.se/) | HTTP/SSE 传输（唯一运行时依赖） | curl License（MIT 兼容） | [curl/curl](https://github.com/curl/curl) |
| [nlohmann/json](https://json.nlohmann.me/) | 类型/协议层，**v3.12.0 单头内置**于 `vendor/nlohmann-json.hpp`，不经包管理 | MIT | [nlohmann/json](https://github.com/nlohmann/json) |
| [FTXUI](https://ftxui.cpp.dev/) | demo 终端 UI（仅 `vxae_demo` 消费，核心库不受影响） | Apache-2.0 | [ArthurSonzogni/FTXUI](https://github.com/ArthurSonzogni/FTXUI) |
| [vcpkg](https://vcpkg.io/) | 依赖管理（manifest 模式，见 `vcpkg.json`） | MIT | [microsoft/vcpkg](https://github.com/microsoft/vcpkg) |

同时致谢 C++23 标准库（`std::expected` / `std::generator` / `std::print` / `std::jthread`）让本库得以去掉 spdlog/fmt/yaml-cpp/stduuid 等传统依赖。
demo 的 TUI 布局移植自本团队前作 `puppeteer-vmrx`，接线层已完全重写为本库 API。

## 构建

前置：CMake ≥ 3.25、Ninja、MSVC 2026(18)/GCC 14+/Clang 18+，仓库内 `vcpkg/` checkout（或自备 vcpkg 并改 `CMakePresets.json` 中工具链路径）。

### 使用 CMake Presets（推荐，兼容 IDE CMake Tools 插件）

```bash
cmake --preset default          # 配置：核心库 + demo（Ninja Multi-Config）
cmake --build --preset debug    # 构建 vxae / vxae_check / vxae_demo（Debug）
cmake --build --preset release  # Release
cmake --preset lib-only && cmake --build --preset lib-debug   # 仅核心库，不拉 ftxui
```

产物位于 `build/default/{Debug,Release}/`。

### 手动命令（不经 preset）

```bash
cmake -S . -B build -G "Ninja Multi-Config" \
  -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake -DAE_BUILD_DEMO=ON
cmake --build build --config Debug --target vxae_demo
```

首次 configure 时 vcpkg manifest 会自动解析并构建 `curl`、`ftxui`（Linux/macOS 从源码构建，耗时视机器而定）。

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

3. 内置默认（endpoint `https://api.openai.com/v1`、model `gpt-4o-mini`、无 key —— 连接灯红色并提示）。

### 运行

```bash
build/default/Debug/vxae_demo        # Linux/macOS
build\default\Debug\vxae_demo.exe    # Windows
```

- **chat 模式（默认）**：单轮流式对话（`StreamingLLMClient`），SSE 增量/思考块/真实 token 统计；
- **agent 模式**：`AgentEngine` 完整循环，多轮工具调用（内置演示工具 `current_time`），工具消息可折叠回填；
- 快捷键：`Shift+Enter` 发送、`Enter` 换行、`Ctrl+P` 停止当前请求、`Ctrl+H` 帮助、`Ctrl+T` 主题轮换、`Ctrl+Q` 退出；
- 主题/模型选择持久化在 exe 同目录 `demo_settings.json`。

## 在你的工程中使用

```cmake
add_subdirectory(external/vxae)            # 或 git submodule / FetchContent
target_link_libraries(your_target PRIVATE vxae)
target_compile_features(your_target PRIVATE cxx_std_23)
```

```cpp
#include "AE/AE.hpp"                 // AgentEngine
#include "AE/StreamingLLMClient.hpp" // 或直接流式客户端
#include "AE/AEConfig.hpp"           // JSON 配置
```

要点：C++23 必需（MSVC 映射 `/std:c++latest`）；include 根为 `include/`、`include/AE/`、`vendor/`；`UUID_SYSTEM_GENERATOR`/`Ole32` 等历史包袱已不存在。

## 许可

本项目以 [Apache-2.0](LICENSE) 授权。第三方库遵循其各自许可：libcurl（curl License）、nlohmann/json（MIT，随仓库内置单头文件）、FTXUI（Apache-2.0）、vcpkg（MIT）。
