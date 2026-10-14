// SPDX-License-Identifier: MIT
// 运行时配置持有：原子快照（采集热路径零锁）+ 防抖落盘 + 订阅 + mtime 轮询热重载。
// config-schema.md §6 ConfigStore。
#pragma once

#include "config/config.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wpm::config {

class ConfigStore {
public:
    ConfigStore(ConfigPaths paths, Config initial);

    // 任意线程可调用；返回不可变快照，调用方持 shared_ptr 期间内容稳定。
    [[nodiscard]] std::shared_ptr<const Config> Snapshot() const noexcept;

    // 单调递增的变更计数（每次实际内容变化 +1）
    [[nodiscard]] uint64_t Revision() const noexcept { return revision_.load(std::memory_order_acquire); }

    // 校验 → 修复 → 发布 →（可选）落盘。返回是否发生了修订。
    bool Apply(const Config& next, bool persist);
    // UI 侧批量修改：在副本上执行 fn 再 Apply
    bool Mutate(const std::function<void(Config&)>& fn, bool persist);

    void SetAutoSave(bool on, int debounceMs = 500);
    void SetFileWatch(bool on, int pollMs = 2000);
    void Flush();

    using Callback = std::function<void(uint64_t rev, const Config&)>;
    void Subscribe(Callback cb);

    // 自上次 Apply 起累计的诊断（UI「有 N 项被修正」用）
    [[nodiscard]] std::vector<Diagnostic> Diagnostics() const;
    void ClearDiagnostics();
    [[nodiscard]] std::string LastError() const;

    [[nodiscard]] const ConfigPaths& Paths() const noexcept { return paths_; }
    [[nodiscard]] bool Readonly() const noexcept { return readonly_; }

private:
    void Publish(Config next, std::vector<Diagnostic> diags);
    void SaveNow();
    void Tick();  // 由主循环调用：处理防抖落盘与文件轮询

public:
    // 主循环每帧调用（低成本：一次 mtime 查询 + 原子读）
    void Poll() { Tick(); }

private:
    ConfigPaths paths_;
    mutable std::mutex mu_;
    std::shared_ptr<const Config> current_;
    std::atomic<uint64_t> revision_{1};
    std::vector<Callback> subs_;
    std::vector<Diagnostic> diags_;
    std::string lastError_;
    bool readonly_ = false;

    bool autoSave_ = true;
    int debounceMs_ = 500;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point dirtySince_{};

    bool fileWatch_ = false;
    int pollMs_ = 2000;
    std::chrono::steady_clock::time_point lastPoll_{};
    uint64_t watchedMtime_ = 0;
};

}  // namespace wpm::config
