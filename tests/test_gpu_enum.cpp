// SPDX-License-Identifier: MIT
// gpu.selectedIndex 的解析契约（A1 决策②，docs/config-schema.md §3.5）。
//
// 这一组用例不需要任何显卡：ResolveSelectedGpu 是纯函数，
// 因此可以在这台机器上把"绝不回退到另一个 GPU 的读数值"这条红线钉死。
#include "test.h"

#include "sensors/gpu_enum.h"

#include <string>
#include <vector>

using wpm::sensors::GpuAdapter;
using wpm::sensors::ResolveSelectedGpu;

namespace {

GpuAdapter Make(int index, std::string name, std::string backend) {
    GpuAdapter a;
    a.index = index;
    a.name = std::move(name);
    a.backend = std::move(backend);
    return a;
}

// 本机真实形状：AMD 核显（有 ADL2）+ WARP（无后端）
std::vector<GpuAdapter> RealWorldShape() {
    return {
        Make(0, "AMD Radeon (TM) Graphics", "ADL2"),
        Make(1, "Microsoft Basic Render Driver", ""),
    };
}

bool Contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

WPM_TEST(gpu_resolve_auto_picks_first_with_backend) {
    std::string reason;
    WPM_CHECK_EQ(ResolveSelectedGpu(RealWorldShape(), -1, reason), 0);
    WPM_CHECK(Contains(reason, "自动"));
    WPM_CHECK(Contains(reason, "ADL2"));
}

WPM_TEST(gpu_resolve_auto_skips_backendless_first) {
    // 虚拟/软件适配器排在前面时，自动模式必须跳过它（本机 |--probe| 就是这么走的）
    std::vector<GpuAdapter> v = {
        Make(0, "OrayIddDriver Device", ""),
        Make(1, "AMD Radeon (TM) Graphics", "ADL2"),
    };
    std::string reason;
    WPM_CHECK_EQ(ResolveSelectedGpu(v, -1, reason), 1);
}

WPM_TEST(gpu_resolve_explicit_honored) {
    std::string reason;
    WPM_CHECK_EQ(ResolveSelectedGpu(RealWorldShape(), 0, reason), 0);
    WPM_CHECK(Contains(reason, "按配置选中"));
}

WPM_TEST(gpu_resolve_out_of_range_never_picks_another_gpu) {
    // 越界时禁用 GPU 遥测：绝不能"顺手"返回 0 把核显的数字当成用户的独显
    std::string reason;
    WPM_CHECK_EQ(ResolveSelectedGpu(RealWorldShape(), 9, reason), -1);
    WPM_CHECK(Contains(reason, "越界"));
    WPM_CHECK(Contains(reason, "停用"));
    // 必须告诉用户怎样才能恢复自动
    WPM_CHECK(Contains(reason, "-1"));
}

WPM_TEST(gpu_resolve_explicit_backendless_disables_not_fallback) {
    // 用户显式选了 WARP（下标合法）：停用遥测，且措辞必须明说不会借用别的卡
    std::string reason;
    WPM_CHECK_EQ(ResolveSelectedGpu(RealWorldShape(), 1, reason), -1);
    WPM_CHECK(Contains(reason, "不会改用其它适配器的读数"));
    WPM_CHECK(Contains(reason, "停用"));
}

WPM_TEST(gpu_resolve_auto_without_any_backend) {
    std::vector<GpuAdapter> v = {Make(0, "Microsoft Basic Render Driver", "")};
    std::string reason;
    WPM_CHECK_EQ(ResolveSelectedGpu(v, -1, reason), -1);
    WPM_CHECK(Contains(reason, "没有适配器带免驱遥测后端"));
}

WPM_TEST(gpu_resolve_empty_adapter_list) {
    std::vector<GpuAdapter> v;
    std::string reason;
    WPM_CHECK_EQ(ResolveSelectedGpu(v, -1, reason), -1);
    WPM_CHECK(Contains(reason, "未枚举到任何显示适配器"));
    // 空列表 + 显式下标同样不能崩、不能返回 0
    std::string reason2;
    WPM_CHECK_EQ(ResolveSelectedGpu(v, 3, reason2), -1);
}

WPM_TEST(gpu_resolve_reason_always_written) {
    // reason 是 --probe / UI 唯一的解释来源：任何分支都不许留空
    const int probes[] = {-1, 0, 1, 2, 15};
    for (int sel : probes) {
        std::string reason = "脏值";
        const int got = ResolveSelectedGpu(RealWorldShape(), sel, reason);
        WPM_CHECK(got >= -1 && got <= 1);
        WPM_CHECK(!reason.empty());
    }
}
