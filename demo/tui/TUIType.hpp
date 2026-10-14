#pragma once

/// @file demo/tui/TUIType.hpp
/// @brief TUI 层基础类型：面板类型、UI 事件、会话/模型信息载体（VXAE demo 移植版）。
/// @details 被 `ITUI`/`TUI`/`TUIManager` 及全部 Components 共用；错误语义走 `Core::Result`。
///          纯数据载体，不依赖任何引擎/后端类型，桥接层可直接复用。

#include "Core/Result.hpp"
#include "nlohmann-json.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace AE::TUI
{

    enum class PanelType
    {
        None,
        Chat,
        Sessions,
        Tools,
        Settings,
        Help,
        Memory
    };

    enum class MessageRole
    {
        User,
        Assistant,
        System,
        Tool
    };

    enum class MessageStatus
    {
        Pending,
        Streaming,
        Completed,
        Failed
    };

    enum class ThemeVariant
    {
        Dark,
        Light,
        Dracula,
        Monokai,
        Solarized
    };

    enum class KeyBinding
    {
        ToggleSidebar,
        FocusInput,
        NewSession,
        CycleTab,
        Quit,
        Help,
        ClearChat,
        SubmitInput,
        ScrollUp,
        ScrollDown,
        Search
    };

    /// 上下文拼接作用范围（纯数据枚举，供桥接层按需使用；demo 层不驱动策略切换）。
    /// SlidingWindow：仅回读最近 N 轮完整轮次 + 会话摘要，token 预算可控。
    /// Full：全量回读该会话全部历史轮次，不丢信息但受模型上下文窗口限制。
    enum class ContextConcatMode
    {
        SlidingWindow,
        Full
    };

    /// 阶段4：编排轨迹单步摘要（意图 / 路由 / 迭代 / 工具步均可作为一步）。
    struct OrchestrationStep
    {
        std::string phase;    ///< 阶段标记：intent / route / iterate / tool
        std::string summary;  ///< 单行摘要（折叠标题下可见）
        std::string detail;   ///< 展开后可读的明细（工具名/入参要点等，可为空）
    };

    /// 阶段4：一次助手回复的编排信息块（业务层广播 → TUI 折叠呈现）。
    /// 默认折叠：标题栏聚合「编排 · 意图→路由 · step x/y · 工具 N」，展开后逐步明细。
    struct OrchestrationInfo
    {
        bool visible = false;                    ///< 是否含编排信息（无则编排块不渲染）
        std::string intent;                      ///< 聚合意图
        std::string route;                       ///< 路由目标
        int stepIndex = 0;                       ///< 当前执行步（x，1 起）
        int stepTotal = 0;                       ///< 总步数（y）
        std::vector<OrchestrationStep> steps;    ///< 各步明细（含工具调用摘要）
    };

    struct Message
    {
        std::string id;
        MessageRole role = MessageRole::User;
        std::string content;
        MessageStatus status = MessageStatus::Completed;
        std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now();
        std::string modelName;
        // Phase G：思考消息标记（reasoning_content 独立渲染为思考气泡）。
        bool isThinking = false;
        // Phase I：正文消息附带的思考内容（reasoning_content 合并到正文，渲染为可折叠思考块）。
        std::string thinkingContent;
        // Phase I：思考块折叠状态（默认折叠，主要显示正文）。
        bool thinkingCollapsed = true;
        // 阶段4：编排信息块（聚合意图/路由/迭代/工具步摘要），折叠呈现。
        OrchestrationInfo orchestration;
        // 阶段4：编排块折叠状态（默认折叠，标题栏显示「编排 · step x/y」）。
        bool orchestrationCollapsed = true;
        std::optional<std::string> toolCallName;
        std::optional<nlohmann::json> toolCallArgs;
        std::optional<nlohmann::json> toolResult;
        // 工具结果卡片折叠状态（默认折叠，长输出避免刷屏）。
        bool toolResultCollapsed = true;

        // 服务端真实 token（由业务层 usage 回填，0 表示未知/未回填，渲染回退字符近似）。
        int32_t inputTokens = 0;      // 用户输入 token（= 相邻请求 prompt_tokens 差值）
        int32_t outputTokens = 0;     // 助手输出 token（正文）
        int32_t reasoningTokens = 0;  // 助手思考 token
        int32_t totalTokens = 0;      // 合计（input+output 或 completion+reasoning，依角色）
    };

    struct SessionInfo
    {
        std::string id;
        std::string title;
        std::chrono::system_clock::time_point createdAt;
        std::chrono::system_clock::time_point updatedAt;
        std::size_t messageCount = 0;
        bool isActive = false;
    };

    struct ToolInfo
    {
        std::string name;
        std::string description;
        std::string category;
        bool isEnabled = true;
    };

    struct StatusItem
    {
        std::string label;
        std::string value;
        std::string color;
    };

    struct TUIConfig
    {
        ThemeVariant theme = ThemeVariant::Dark;
        bool showSidebar = true;
        bool showStatusBar = true;
        std::size_t sidebarWidth = 30;
        bool enableMouse = true;
        bool enableMarkdown = true;
        std::string defaultModel = "local";
        int refreshRateMs = 50;
        // 上下文拼接策略（纯数据字段，demo 层仅透传，不驱动面板切换）。
        // contextMode 决定作用范围，contextWindow 为滑动窗口轮数 N（默认 5）。
        ContextConcatMode contextMode = ContextConcatMode::SlidingWindow;
        int contextWindow = 5;
    };

    struct TUIEvent
    {
        enum class Type
        {
            UserInput,
            NewSession,
            SwitchSession,
            DeleteSession,
            TogglePanel,
            ThemeChanged,
            ConfigChanged,
            QuitRequested,
            ToolExecuted,
            MessageReceived,
            StreamingUpdate,
            ToggleThinking,
            ToggleIntent,
            ToggleMcp,
            ModelSwitch,
            RestartRequested,
            StopRequested, // Ctrl+P：请求停止当前生成（桥接层消费）
            // 占位：帮助信息事件（当前未绑定按键，保留供后续实现）。
            HelpRequested
        };

        Type type = Type::UserInput;
        std::string sessionId;
        std::string payload;
        nlohmann::json data = nlohmann::json::object();
    };

    using EventCallback = std::function<void(const TUIEvent &event)>;
    using MessageList = std::vector<Message>;
    using SessionList = std::vector<SessionInfo>;
    using ToolList = std::vector<ToolInfo>;

    inline auto messageRoleToString(MessageRole role) -> std::string
    {
        switch (role)
        {
        case MessageRole::User:
            return "user";
        case MessageRole::Assistant:
            return "assistant";
        case MessageRole::System:
            return "system";
        case MessageRole::Tool:
            return "tool";
        default:
            return "unknown";
        }
    }

    inline auto messageRoleFromString(const std::string &str) -> MessageRole
    {
        if (str == "user")
            return MessageRole::User;
        if (str == "assistant")
            return MessageRole::Assistant;
        if (str == "system")
            return MessageRole::System;
        if (str == "tool")
            return MessageRole::Tool;
        return MessageRole::User;
    }

    inline auto themeVariantToString(ThemeVariant theme) -> std::string
    {
        switch (theme)
        {
        case ThemeVariant::Dark:
            return "dark";
        case ThemeVariant::Light:
            return "light";
        case ThemeVariant::Dracula:
            return "dracula";
        case ThemeVariant::Monokai:
            return "monokai";
        case ThemeVariant::Solarized:
            return "solarized";
        default:
            return "dark";
        }
    }

    inline auto themeVariantFromString(const std::string &str) -> ThemeVariant
    {
        if (str == "light")
            return ThemeVariant::Light;
        if (str == "dracula")
            return ThemeVariant::Dracula;
        if (str == "monokai")
            return ThemeVariant::Monokai;
        if (str == "solarized")
            return ThemeVariant::Solarized;
        return ThemeVariant::Dark;
    }

} // namespace AE::TUI
