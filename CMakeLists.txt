# ============================================================
# VXAE — 独立 Agent Engine 库（AE::Engine）
# 零依赖原项目：自带 Core 基座（std::expected / std::println 原生实现）。
#
# 依赖面（vcpkg manifest）：
#  - 必需：curl（LLM HTTP/SSE 传输）—— 待定，替代候选见 plan.md §8
#  - 内置：nlohmann-json 单头文件 vendor/nlohmann-json.hpp（v3.12.0，不经 vcpkg）
#  - 已移除：opentelemetry-cpp / Drogon（可选层 AEOtel.hpp / AEHttpServer.hpp 随之删除）
#            spdlog / fmt / yaml-cpp / stduuid / Ole32（配置改 JSON、trace_id 改标准库随机 hex）
#
# 目标：
#  - vxae        STATIC，含 4 个功能 .cpp，PUBLIC 传播 include 与依赖
#  - vxae_check  STATIC，构建期头文件可编译性探针（不参与常规链接）
#
# 语言标准：C++26（由 preset 指定 clang-cl 前端 / lld-link 后端，ABI 仍为 MSVC，
# 因此 vcpkg x64-windows 预编译依赖可直接复用）
# ============================================================

cmake_minimum_required(VERSION 3.25)
project(vxagentengine LANGUAGES CXX VERSION 0.1.0)

# 全局默认：所有目标一律 C++26，禁用 GNU 扩展（-std=c++26 / -std:c++26）
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL CONFIG REQUIRED)

# ------------------------------------------------------------
# vxae —— 核心 STATIC 库
# ------------------------------------------------------------
add_library(vxae STATIC
    src/AE/AESecurityGuard.cpp
    src/AE/AEBuiltinTools.cpp
    src/AE/AEToolPolicy.cpp
    src/AE/AESessionStore.cpp
)

target_include_directories(vxae PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor
)

target_link_libraries(vxae PUBLIC
    CURL::libcurl
)

# clang-cl 前端 + MSVC ABI：这里同样命中（MSVC 或 clang 的 MSVC 前端变体）
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # 源码 UTF-8 无 BOM；CXX_STANDARD 26 由 CMake 映射为 -std:c++26 / -std=c++latest
    target_compile_options(vxae PUBLIC /utf-8)
endif()

set_target_properties(vxae PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# ------------------------------------------------------------
# vxae_check —— 编译期头文件可编译性校验（独立 STATIC 探针目标）
# ------------------------------------------------------------
add_library(vxae_check STATIC
    tests/vxae_compile_check.cpp
)

target_link_libraries(vxae_check PRIVATE vxae)

set_target_properties(vxae_check PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# ------------------------------------------------------------
# demo —— FTXUI TUI 演示（vxae_tui + vxae_demo）
# ------------------------------------------------------------
option(AE_BUILD_DEMO "Build FTXUI demo" ON)
if(AE_BUILD_DEMO)
    find_package(ftxui CONFIG REQUIRED)
    add_subdirectory(demo)
endif()
