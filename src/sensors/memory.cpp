// SPDX-License-Identifier: MIT
#include "sensors/memory.h"

#include "config/catalog.h"

#include <windows.h>

namespace wpm::sensors {

bool MemoryProvider::Init(std::string& detail) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    available_ = GlobalMemoryStatusEx(&ms) != FALSE;
    detail = available_ ? std::string{} : "GlobalMemoryStatusEx 失败";

    for (size_t i = 0; i < config::MetricCatalog().size(); ++i) {
        const auto id = config::MetricCatalog()[i].id;
        if (id == "mem.usage" || id == "mem.used") slots_.push_back(i);
    }
    return available_;
}

void MemoryProvider::Sample(Snapshot& snap) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) {
        for (size_t s : slots_) {
            snap.metrics[s].status = Status::Error;
            snap.metrics[s].detail = "GlobalMemoryStatusEx 失败";
        }
        return;
    }

    const double totalGiB = static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    const double usedGiB =
        static_cast<double>(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);

    for (size_t s : slots_) {
        const auto id = config::MetricCatalog()[s].id;
        Reading& r = snap.metrics[s];
        r.status = Status::Ok;
        r.detail.clear();
        if (id == "mem.usage") {
            r.value = static_cast<double>(ms.dwMemoryLoad);
        } else if (id == "mem.used") {
            r.value = usedGiB;
        }
    }
    (void)totalGiB;
}

}  // namespace wpm::sensors
