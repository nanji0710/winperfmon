cmake_minimum_required(VERSION 3.25)
project(winperfmon VERSION 0.1.0 LANGUAGES CXX)

# ── 全局编译选项 ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 发布配置：体积优先（主方案 §5）
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# WPM_NO_NETWORK=1 编译期彻底剔除网络代码（release-github §9 隐私声明）
option(WPM_NO_NETWORK "Compile out every network code path" ON)
option(WPM_BUILD_TESTS "Build unit tests" ON)

add_library(wpm_options INTERFACE)
target_compile_definitions(wpm_options INTERFACE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  $<$<BOOL:${WPM_NO_NETWORK}>:WPM_NO_NETWORK=1>
)
if(MSVC)
  target_compile_options(wpm_options INTERFACE /W4 /permissive- /utf-8 /EHsc /GS)
else()
  target_compile_options(wpm_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
    -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections)
  # 注意：GCC 下 -mwindows 会强制查找 WinMain；M0 的控制台探针程序用 wmain，
  # 因此这里只保留通用链接选项，GUI 子系统（M3）届时单独加 -mwindows。
  target_link_options(wpm_options INTERFACE -Wl,--gc-sections -static -s)
endif()

# ── 核心库（可被单测链接）────────────────────────────────────────────────────
add_library(wpm_core STATIC
  src/core/json.cpp
  src/core/text.cpp
  src/config/catalog.cpp
  src/config/config.cpp
  src/config/store.cpp
  src/sensors/sensor.cpp
  src/sensors/memory.cpp
  src/sensors/cpu_pdh.cpp
)
target_include_directories(wpm_core PUBLIC include src)
target_link_libraries(wpm_core PUBLIC wpm_options pdh)

# ── 主程序 ──────────────────────────────────────────────────────────────────
add_executable(winperfmon src/app/main.cpp)
target_link_libraries(winperfmon PRIVATE wpm_core)
if(NOT MSVC)
  # GCC 默认入口是 main/WinMain；程序用 wmain（宽字符命令行），必须开 -municode。
  target_compile_options(winperfmon PRIVATE -municode)
  target_link_options(winperfmon PRIVATE -municode)
endif()

# ── 单测 ────────────────────────────────────────────────────────────────────
if(WPM_BUILD_TESTS)
  enable_testing()
  add_executable(wpm_tests tests/test_main.cpp tests/test_json.cpp tests/test_config.cpp)
  target_link_libraries(wpm_tests PRIVATE wpm_core)
  add_test(NAME unit COMMAND wpm_tests)
endif()
