// SPDX-License-Identifier: MIT
#include "config/store.h"

#include <windows.h>

#include <utility>

namespace wpm::config {

ConfigStore::ConfigStore(ConfigPaths paths, Config initial)
    : paths_(std::move(paths)), readonly_(!paths_.writable) {
    auto owned = std::make_shared<Config>(std::move(initial));
    current_ = std::move(owned);
    if (readonly_ && !paths_.file.empty()) {
        lastError_ = "配置只读";
    }
}

std::shared_ptr<const Config> ConfigStore::Snapshot() const noexcept {
    // 只在发布瞬间短暂持锁；读取本身是 shared_ptr 原子拷贝
    std::lock_guard<std::mutex> lk(mu_);
    return current_;
}

void ConfigStore::Publish(Config next, std::vector<Diagnostic> diags) {
    std::vector<Callback> toCall;
    std::shared_ptr<const Config> snapshot;
    uint64_t rev = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        current_ = std::make_shared<const Config>(std::move(next));
        snapshot = current_;
        rev = revision_.fetch_add(1, std::memory_order_acq_rel) + 1;
        for (auto& d : diags) diags_.push_back(std::move(d));
        toCall = subs_;
        dirty_ = true;
        dirtySince_ = std::chrono::steady_clock::now();
    }
    for (auto& cb : toCall) {
        if (cb) cb(rev, *snapshot);
    }
}

bool ConfigStore::Apply(const Config& next, bool persist) {
    Config work = next;
    std::vector<Diagnostic> diags;
    ValidateAndRepair(work, ValidationPolicy{}, diags);
    Publish(std::move(work), std::move(diags));
    if (persist) {
        if (autoSave_) dirty_ = true;
        else SaveNow();
    }
    return true;
}

bool ConfigStore::Mutate(const std::function<void(Config&)>& fn, bool persist) {
    Config work;
    {
        std::lock_guard<std::mutex> lk(mu_);
        work = *current_;
    }
    fn(work);
    return Apply(work, persist);
}

void ConfigStore::SetAutoSave(bool on, int debounceMs) {
    std::lock_guard<std::mutex> lk(mu_);
    autoSave_ = on;
    debounceMs_ = debounceMs;
}

void ConfigStore::SetFileWatch(bool on, int pollMs) {
    std::lock_guard<std::mutex> lk(mu_);
    fileWatch_ = on;
    pollMs_ = pollMs;
    if (on && !paths_.file.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(paths_.file.c_str(), GetFileExInfoStandard, &fad)) {
            watchedMtime_ = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                            fad.ftLastWriteTime.dwLowDateTime;
        }
    }
}

void ConfigStore::SaveNow() {
    std::shared_ptr<const Config> snapshot;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!dirty_ && lastError_.empty()) {
            // 即使未标脏也允许 Flush 调用；无实际变化时直接返回
        }
        snapshot = current_;
    }
    if (readonly_) {
        std::lock_guard<std::mutex> lk(mu_);
        lastError_ = "配置只读，未写入";
        dirty_ = false;
        return;
    }
    std::string err;
    const bool ok = SaveConfig(*snapshot, paths_, &err);
    std::lock_guard<std::mutex> lk(mu_);
    if (ok) {
        dirty_ = false;
        lastError_.clear();
    } else {
        lastError_ = err;
    }
}

void ConfigStore::Tick() {
    const auto now = std::chrono::steady_clock::now();
    bool doSave = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (autoSave_ && dirty_ &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - dirtySince_).count() >=
                debounceMs_) {
            doSave = true;
        }
    }
    if (doSave) SaveNow();

    bool doReload = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!fileWatch_ || paths_.file.empty()) return;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPoll_).count();
        if (elapsed < pollMs_) return;
        lastPoll_ = now;
        doReload = true;
    }
    if (!doReload) return;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(paths_.file.c_str(), GetFileExInfoStandard, &fad)) return;
    const uint64_t mtime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                           fad.ftLastWriteTime.dwLowDateTime;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (mtime == watchedMtime_ || dirty_) return;  // 自己刚写的，跳过
        watchedMtime_ = mtime;
    }
    LoadResult lr = LoadConfig(paths_);
    if (!lr.error.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        lastError_ = lr.error;  // 外部文件坏了 → 保留内存中的配置，不覆盖用户文件
        return;
    }
    Publish(std::move(lr.config), std::move(lr.diagnostics));
    {
        std::lock_guard<std::mutex> lk(mu_);
        dirty_ = false;
    }
}

void ConfigStore::Flush() {
    bool need = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        need = dirty_;
    }
    if (need) SaveNow();
}

void ConfigStore::Subscribe(Callback cb) {
    std::lock_guard<std::mutex> lk(mu_);
    subs_.push_back(std::move(cb));
}

std::vector<Diagnostic> ConfigStore::Diagnostics() const {
    std::lock_guard<std::mutex> lk(mu_);
    return diags_;
}

void ConfigStore::ClearDiagnostics() {
    std::lock_guard<std::mutex> lk(mu_);
    diags_.clear();
}

std::string ConfigStore::LastError() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lastError_;
}

}  // namespace wpm::config
