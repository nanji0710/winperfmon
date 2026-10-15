// SPDX-License-Identifier: MIT
// PawnIO 设备客户端实现。许可依据见 pawnio.h 顶部注释（只走 DeviceIoControl 接口，不链接 PawnIOLib）。
#include "sensors/pawnio.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "core/text.h"

namespace wpm::sensors {
namespace {

// pawnio_um.h：k_device_type = 41394，三个 IOCTL 均为 METHOD_BUFFERED + FILE_ANY_ACCESS，
// 因此 CTL_CODE 退化为 (type << 16) | (function << 2)。
constexpr ULONG kDeviceType = 41394;
constexpr DWORD Ctl(ULONG function) { return (kDeviceType << 16) | (function << 2); }
constexpr DWORD kIoctlLoadBinary = Ctl(0x821);
constexpr DWORD kIoctlExecuteFn = Ctl(0x841);

constexpr size_t kFnNameLength = 32;  // 模块函数名的定长 ASCII 前缀（NUL 结尾）

constexpr const wchar_t* kDevicePaths[] = {
    L"\\\\?\\GLOBALROOT\\Device\\PawnIO",  // LHM/PawnIOLib 的用法（GLOBALROOT → \Device\PawnIO）
    L"\\\\.\\PawnIO",                      // 旧版 DOS 设备名，仍作为兜底
};

/// Win32 错误码 → "错误 2（找不到指定的文件）" 形式，便于用户在 --probe 里直接看懂。
std::string Win32ErrorText(DWORD code) {
  wchar_t* buffer = nullptr;
  const DWORD n = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<wchar_t*>(&buffer),
      0, nullptr);
  std::string text = "错误 " + std::to_string(code);
  if (n != 0 && buffer != nullptr) {
    std::wstring w(buffer, n);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) w.pop_back();
    text += "（" + text::toUtf8(w) + "）";
    LocalFree(buffer);
  }
  return text;
}

/// 可执行文件所在目录（不带尾随反斜杠）。失败返回空串。
std::wstring ExecutableDir() {
  wchar_t buf[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  if (n == 0 || n >= std::size(buf)) return {};
  std::wstring path(buf, n);
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>* out, std::string* detail) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (detail) *detail = "打不开 " + text::toUtf8(path) + "：" + Win32ErrorText(GetLastError());
    return false;
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (16 * 1024 * 1024)) {
    CloseHandle(h);
    if (detail) *detail = "文件大小异常：" + text::toUtf8(path);
    return false;
  }
  out->assign(static_cast<size_t>(size.QuadPart), 0);
  DWORD read = 0;
  const BOOL ok = ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &read, nullptr);
  CloseHandle(h);
  if (!ok) {
    if (detail) *detail = "读失败：" + text::toUtf8(path) + "：" + Win32ErrorText(GetLastError());
    return false;
  }
  out->resize(read);
  return true;
}

/// 读取注册表安装版本（HKLM\...\Uninstall\PawnIO 的 DisplayVersion），LHM 同款做法。
std::string InstalledVersion() {
  HKEY key = nullptr;
  const wchar_t* path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO";
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS &&
      RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return {};
  }
  wchar_t buf[128] = {};
  DWORD size = sizeof(buf);
  DWORD type = 0;
  const LSTATUS st =
      RegQueryValueExW(key, L"DisplayVersion", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
  RegCloseKey(key);
  if (st != ERROR_SUCCESS || type != REG_SZ || size == 0) return {};
  std::wstring w(buf);
  while (!w.empty() && (w.back() == L'\0' || w.back() == L' ')) w.pop_back();
  return text::toUtf8(w);
}

}  // namespace

// ---------------------------------------------------------------- PawnIoDevice

PawnIoDevice* PawnIoDevice::Instance() {
  static PawnIoDevice instance;
  return &instance;
}

PawnIoDevice::~PawnIoDevice() {
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
}

bool PawnIoDevice::Open(std::string* detail) {
  if (handle_ != nullptr) return true;
  DWORD last = 0;
  for (const wchar_t* candidate : kDevicePaths) {
    HANDLE h = CreateFileW(candidate, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      handle_ = h;
      devicePath_ = text::toUtf8(candidate);
      return true;
    }
    last = GetLastError();
  }
  openError_ = Win32ErrorText(last);
  if (detail != nullptr) {
    *detail = "打不开 PawnIO 设备（" + openError_ + "）；需先安装官方 PawnIO 驱动"
              "（https://pawnio.eu 或 github.com/namazso/PawnIO.Setup/releases）";
  }
  return false;
}

bool PawnIoDevice::LoadModule(std::string_view fileName, PawnIoModule* module, std::string* detail) {
  if (module == nullptr) return false;
  if (!Open(detail)) return false;
  module->handle_ = handle_;
  module->devicePath_ = devicePath_;
  return module->LoadFile(fileName, detail);
}

// ---------------------------------------------------------------- PawnIoModule

PawnIoModule::~PawnIoModule() {
  // 设备句柄由 PawnIoDevice 持有；模块本身随进程退出（卸载由驱动在句柄关闭时完成）。
  handle_ = nullptr;
}

bool PawnIoModule::Load(const std::vector<uint8_t>& blob, std::string* detail) {
  if (handle_ == nullptr) {
    if (detail != nullptr) *detail = "设备未打开";
    return false;
  }
  if (blob.empty()) {
    if (detail != nullptr) *detail = "模块字节为空";
    return false;
  }
  DWORD read = 0;
  if (!DeviceIoControl(static_cast<HANDLE>(handle_), kIoctlLoadBinary,
                       const_cast<uint8_t*>(blob.data()), static_cast<DWORD>(blob.size()), nullptr, 0,
                       &read, nullptr)) {
    if (detail != nullptr) {
      *detail = "驱动拒绝载入模块（" + Win32ErrorText(GetLastError()) + "，blob " +
                std::to_string(blob.size()) + " 字节）";
    }
    return false;
  }
  loaded_ = true;
  return true;
}

bool PawnIoModule::LoadFile(std::string_view fileName, std::string* detail) {
  const std::wstring dir = ExecutableDir();
  const std::wstring name = text::toWide(fileName);
  std::vector<std::wstring> candidates;
  if (!dir.empty()) {
    candidates.push_back(dir + L"\\modules\\" + name);
    candidates.push_back(dir + L"\\" + name);
  }
  candidates.push_back(name);  // 当前工作目录（开发期方便）
  std::string tried;
  for (const std::wstring& path : candidates) {
    std::vector<uint8_t> blob;
    std::string why;
    if (!ReadWholeFile(path, &blob, &why)) {
      if (!tried.empty()) tried += "；";
      tried += why;
      continue;
    }
    std::string loadDetail;
    if (!Load(blob, &loadDetail)) {
      if (detail != nullptr) {
        *detail = text::toUtf8(path) + " 载入失败：" + loadDetail;
      }
      return false;
    }
    path_ = text::toUtf8(path);
    if (detail != nullptr) *detail = "已载入 " + path_;
    return true;
  }
  if (detail != nullptr) {
    *detail = "找不到模块文件 " + std::string(fileName) + "（已尝试 " + tried + "）";
  }
  return false;
}

bool PawnIoModule::Execute(const char* function, const uint64_t* in, size_t inCount, uint64_t* out,
                           size_t outCount, size_t* outWritten, std::string* detail) {
  if (handle_ == nullptr || !loaded_) {
    if (detail != nullptr) *detail = "模块未载入";
    return false;
  }
  if (function == nullptr || *function == '\0') {
    if (detail != nullptr) *detail = "函数名为空";
    return false;
  }
  // 输入布局（与 PawnIOLib/LHM 一致）：32 字节 ASCII 函数名（NUL 填充）+ 入参数组（每项 8 字节）。
  std::vector<uint8_t> input(kFnNameLength + inCount * sizeof(uint64_t), 0);
  const size_t nameLen = std::strlen(function);
  std::memcpy(input.data(), function, nameLen < kFnNameLength - 1 ? nameLen : kFnNameLength - 1);
  if (inCount != 0 && in != nullptr) {
    std::memcpy(input.data() + kFnNameLength, in, inCount * sizeof(uint64_t));
  }
  std::vector<uint8_t> output(outCount * sizeof(uint64_t), 0);
  DWORD read = 0;
  if (!DeviceIoControl(static_cast<HANDLE>(handle_), kIoctlExecuteFn, input.data(),
                       static_cast<DWORD>(input.size()), output.empty() ? nullptr : output.data(),
                       static_cast<DWORD>(output.size()), &read, nullptr)) {
    if (detail != nullptr) {
      *detail = std::string("调用 ") + function + " 失败（" + Win32ErrorText(GetLastError()) + "）";
    }
    return false;
  }
  const size_t written = read / sizeof(uint64_t);
  if (outWritten != nullptr) *outWritten = written;
  if (out != nullptr && outCount != 0) {
    const size_t n = written < outCount ? written : outCount;
    std::memcpy(out, output.data(), n * sizeof(uint64_t));
  }
  if (written < outCount) {
    if (detail != nullptr) {
      *detail = std::string("调用 ") + function + " 只返回 " + std::to_string(written) + " / " +
                std::to_string(outCount) + " 个结果";
    }
    return false;
  }
  return true;
}

bool PawnIoModule::Call1(const char* function, uint64_t arg, uint64_t* outValue, std::string* detail) {
  const uint64_t in[1] = {arg};
  uint64_t out[1] = {};
  size_t written = 0;
  if (!Execute(function, in, 1, out, 1, &written, detail)) return false;
  if (outValue != nullptr) *outValue = out[0];
  return true;
}

bool PawnIoModule::Call2(const char* function, uint64_t arg0, uint64_t arg1, std::string* detail) {
  const uint64_t in[2] = {arg0, arg1};
  size_t written = 0;
  return Execute(function, in, 2, nullptr, 0, &written, detail);
}

// ---------------------------------------------------------------- 探测

PawnIoStatus ProbePawnIo() {
  PawnIoStatus status;
  status.version = InstalledVersion();
  status.installed = !status.version.empty();

  PawnIoDevice* device = PawnIoDevice::Instance();
  std::string openDetail;
  status.deviceOpened = device->Open(&openDetail);
  status.deviceDetail = status.deviceOpened ? ("已打开 " + device->devicePath()) : openDetail;

  // 模块探测：只检查文件是否存在（不在探测阶段真的载入，避免副作用）。
  const std::wstring dir = ExecutableDir();
  const wchar_t* names[] = {L"AMDFamily17.bin", L"IntelMSR.bin", L"LpcACPIEC.bin", L"LpcIO.bin"};
  std::string found;
  std::string missing;
  for (const wchar_t* name : names) {
    bool ok = false;
    if (!dir.empty()) {
      ok = GetFileAttributesW((dir + L"\\modules\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES ||
           GetFileAttributesW((dir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    std::string& target = ok ? found : missing;
    if (!target.empty()) target += ", ";
    target += text::toUtf8(name);
  }
  status.modulesAvailable = !found.empty();
  status.modulesDetail = status.modulesAvailable ? ("可用：" + found) : ("未找到模块文件：" + missing);
  if (!missing.empty() && status.modulesAvailable) status.modulesDetail += "；缺：" + missing;
  return status;
}

}  // namespace wpm::sensors
