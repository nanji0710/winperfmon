// SPDX-License-Identifier: MIT
// winperfmon — Windows 极致轻量化性能监控
#pragma once

#define WPM_VERSION_MAJOR 0
#define WPM_VERSION_MINOR 2
#define WPM_VERSION_PATCH 0
#define WPM_VERSION_STRING "0.2.0"
#define WPM_PRODUCT_NAME L"winperfmon"
#define WPM_PRODUCT_NAME_NARROW "winperfmon"

// 配置/注册表写入者标识：用于识别"这份配置是哪个版本写的"（config-schema.md §3.1）
#define WPM_WRITTEN_BY "winperfmon " WPM_VERSION_STRING
