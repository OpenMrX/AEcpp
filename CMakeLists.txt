# ============================================================
# VXAE — 独立 Agent Engine 库（VMX::AE）
# 零依赖原项目：自带 Core 基座（std::expected / std::println 原生实现）。
#
# 依赖面（vcpkg manifest）：
#  - 必需：curl（LLM HTTP/SSE 传输）、nlohmann-json（协议/配置）
#  - 可选 feature：otel（opentelemetry-cpp）、http（Drogon），默认均关闭
# 已移除：spdlog / fmt / yaml-cpp / stduuid / Ole32（配置改 JSON、trace_id 改标准库随机 hex）
#
# 目标：
#  - vxae        STATIC，含 4 个功能 .cpp，PUBLIC 传播 include 与依赖
#  - vxae_check  STATIC，构建期头文件可编译性探针（不参与常规链接）
# ============================================================

cmake_minimum_required(VERSION 3.25)
project(vxagentengine LANGUAGES CXX VERSION 0.1.0)

option(VMX_AE_OTEL "Enable OpenTelemetry C++ SDK sink for AE (requires opentelemetry-cpp)" OFF)
option(VMX_AE_HTTP "Enable Drogon HTTP exposure layer for AE (requires Drogon)" OFF)

find_package(CURL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# ------------------------------------------------------------
# vxae —— 核心 STATIC 库
# ------------------------------------------------------------
add_library(vxae STATIC
    src/AESecurityGuard.cpp
    src/AEBuiltinTools.cpp
    src/AEToolPolicy.cpp
    src/AESessionStore.cpp
)

target_include_directories(vxae PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/AE
)

target_link_libraries(vxae PUBLIC
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

# 可选 OTel 暴露层：宏 + 依赖经 INTERFACE 透传给消费方（AEOtel.hpp 受宏保护）。
if(VMX_AE_OTEL)
    find_package(opentelemetry-cpp CONFIG REQUIRED)
    target_compile_definitions(vxae INTERFACE VMX_AE_ENABLE_OTEL=1)
    target_link_libraries(vxae PUBLIC
        opentelemetry-cpp::api opentelemetry-cpp::sdk opentelemetry-cpp::trace)
    if(TARGET opentelemetry-cpp::exporter-ostream)
        target_link_libraries(vxae PUBLIC opentelemetry-cpp::exporter-ostream)
    endif()
    if(TARGET opentelemetry-cpp::otlp-http-exporter)
        target_link_libraries(vxae PUBLIC opentelemetry-cpp::otlp-http-exporter)
    endif()
endif()

# 可选 Drogon HTTP 暴露层（AEHttpServer.hpp 受宏保护）。
if(VMX_AE_HTTP)
    find_package(Drogon CONFIG REQUIRED)
    target_compile_definitions(vxae INTERFACE VMX_AE_ENABLE_HTTP=1)
    target_link_libraries(vxae PUBLIC Drogon::Drogon)
endif()

if(MSVC)
    # 源码 UTF-8 无 BOM；/std 由 CXX_STANDARD 23 映射为 c++latest。
    target_compile_options(vxae PRIVATE /utf-8)
    target_compile_options(vxae PUBLIC /utf-8)
endif()

set_target_properties(vxae PROPERTIES
    CXX_STANDARD 23
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
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
