# ============================================================
# VXAE demo —— FTXUI TUI（vxae_tui 静态库）+ 可执行入口（vxae_demo）
# 依赖：vxae（引擎/Core 基座）、ftxui::screen/dom/component（vcpkg manifest）。
# ============================================================

add_library(vxae_tui STATIC
    tui/TUI.cpp
    tui/TUIManager.cpp
    tui/Core/Theme.cpp
    tui/Components/ChatPanel.cpp
    tui/Components/Header.cpp
    tui/Components/InputBox.cpp
    tui/Components/MarkdownRenderer.cpp
    tui/Components/MessageBubble.cpp
    tui/Components/StatusBar.cpp
    tui/Components/StreamingMarkdownSplitter.cpp
    tui/Bridge/AETUIBridge.cpp
)

target_include_directories(vxae_tui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/tui
)

target_link_libraries(vxae_tui PUBLIC
    vxae
    ftxui::screen
    ftxui::dom
    ftxui::component
)

if(MSVC)
    # 与 vxae 一致：源码 UTF-8 无 BOM。
    target_compile_options(vxae_tui PRIVATE /utf-8)
    target_compile_options(vxae_tui PUBLIC /utf-8)
endif()

set_target_properties(vxae_tui PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

add_executable(vxae_demo main.cpp)

target_link_libraries(vxae_demo PRIVATE vxae_tui)

set_target_properties(vxae_demo PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
