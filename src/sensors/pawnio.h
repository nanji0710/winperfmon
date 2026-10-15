// SPDX-License-Identifier: MIT
// PawnIO 设备客户端（Ring0 唯一通道）。
//
// 为什么是 PawnIO：本项目铁律「不注入、不挂钩、不自研内核驱动」，而 Windows 用户态无法直接
// 访问 MSR/SMN/EC 端口（WinRing0 又有 CVE-2020-14979 且被 Defender 判为 HackTool）。
// PawnIO 是 2025 年后业界标准通路（LibreHardwareMonitor v0.9.5 起也从 WinRing0 切换到它）。
//
// 许可依据（重要，勿删）：PawnIO 驱动本身是 GPL-2.0，但其许可带明确例外——
// 「the copyright holders of PawnIO give you permission to combine PawnIO program with …
//   independent modules that communicate with PawnIO solely through the device IO control
//   interface」。本文件只通过 DeviceIoControl 与该设备通信，不链接 PawnIOLib、不含驱动代码，
// 因此 winperfmon 整体仍是 MIT。载入的 PawnIO 模块（*.bin）是 LGPL-2.1，作为独立文件随包分发
// （modules\ 目录），不编入本可执行文件。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wpm::sensors {

/// PawnIO 设备当前状态（给 --probe 与降级文案共用）。
struct PawnIoStatus {
  bool installed = false;        ///< 注册表里能读到安装版本号
  std::string version;           ///< DisplayVersion，未安装为空
  bool elevated = false;         ///< 当前进程是否提升（设备 ACL 只给 SYSTEM + Administrators，见下）
  bool deviceOpened = false;     ///< \\.\PawnIO 打开成功
  bool accessDenied = false;     ///< 驱动在但被 ACL 拒绝（错误 5）——非管理员进程的必然结果
  std::string deviceDetail;      ///< 打开失败原因或所用设备路径
  bool modulesAvailable = false; ///< 至少一个模块文件能在预期位置找到
  std::string modulesDetail;     ///< 找模块的结果（找到的目录、缺失的文件名）
};

/// 单个 PawnIO 模块的句柄（一次 DeviceIoControl 载入，进程内可重复执行其函数）。
class PawnIoModule {
 public:
  PawnIoModule() = default;
  ~PawnIoModule();
  PawnIoModule(const PawnIoModule&) = delete;
  PawnIoModule& operator=(const PawnIoModule&) = delete;

  /// 载入模块字节（官方 PawnIO.Modules 发行版里的 *.bin，原样使用，签名因此保持有效）。
  bool Load(const std::vector<uint8_t>& blob, std::string* detail);
  /// 从磁盘读模块文件（先查 <exe目录>\modules\<name>，再查 <exe目录>\<name>）。
  bool LoadFile(std::string_view fileName, std::string* detail);

  bool loaded() const noexcept { return loaded_; }
  /// 已载入的模块路径（诊断用）。
  const std::string& path() const noexcept { return path_; }

  /// 调用模块函数：函数名 + 64 位参数数组 → 64 位输出数组。
  /// 返回 false 时 detail 写明 Win32 错误码（失败绝不返回假数据）。
  bool Execute(const char* function, const uint64_t* in, size_t inCount, uint64_t* out,
               size_t outCount, size_t* outWritten, std::string* detail);

  // 便捷封装：单入单出 / 单入无出。
  bool Call1(const char* function, uint64_t arg, uint64_t* outValue, std::string* detail);
  bool Call2(const char* function, uint64_t arg0, uint64_t arg1, std::string* detail);

 private:
  void* handle_ = nullptr;  // HANDLE；nullptr = 未打开（避免在头文件里拖进 windows.h）
  bool loaded_ = false;
  std::string path_;
  std::string devicePath_;

  friend class PawnIoDevice;
};

/// 打开设备（内部单例式：同一进程内只打开一次，`--probe` 与 provider 共用）。
/// 失败返回 nullptr，detail 写明原因（未安装 / 驱动未启动 / 权限）。
class PawnIoDevice {
 public:
  static PawnIoDevice* Instance();

  bool opened() const noexcept { return handle_ != nullptr; }
  const std::string& devicePath() const noexcept { return devicePath_; }
  /// 上一次打开失败是否因权限（错误 5）。驱动在但进程未提升时为 true。
  bool accessDenied() const noexcept { return accessDenied_; }

  /// 打开设备（幂等）。失败返回 false 并写 detail（未安装 / 驱动未启动 / 权限）。
  bool Open(std::string* detail);

  /// 载入模块（转发到 PawnIoModule）。
  bool LoadModule(std::string_view fileName, PawnIoModule* module, std::string* detail);

 private:
  PawnIoDevice() = default;
  ~PawnIoDevice();
  PawnIoDevice(const PawnIoDevice&) = delete;
  PawnIoDevice& operator=(const PawnIoDevice&) = delete;

  void* handle_ = nullptr;
  std::string devicePath_;
  std::string openError_;
  bool accessDenied_ = false;

  friend class PawnIoModule;
};

/// 注册表探测 + 设备探测 + 模块探测的汇总（给 --probe 与 Collector 的降级文案）。
PawnIoStatus ProbePawnIo();

}  // namespace wpm::sensors
