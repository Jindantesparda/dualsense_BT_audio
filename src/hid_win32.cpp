#include "hid_win32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <setupapi.h>

// MinGW's hidsdi.h/hidpi.h are C headers without extern "C" guards.
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <cstring>

namespace {

std::wstring Lower(std::wstring s) {
    for (auto& c: s) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return s;
}

std::string Win32Error(DWORD code) {
    wchar_t* text = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<wchar_t*>(&text),
        0, nullptr);
    std::string result;
    if (len != 0 && text != nullptr) {
        result = ToUtf8(std::wstring(text, len));
        while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) {
            result.pop_back();
        }
        LocalFree(text);
    }
    if (result.empty()) {
        result = "Win32 error " + std::to_string(code);
    }
    return result;
}

struct HandleCloser {
    HDEVINFO set = INVALID_HANDLE_VALUE;
    ~HandleCloser() {
        if (set != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(set);
        }
    }
};

} // namespace

std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                            nullptr, nullptr);
    }
    return out;
}

std::vector<HidDeviceInfo> EnumerateHidDevices() {
    std::vector<HidDeviceInfo> devices;

    GUID guid {};
    HidD_GetHidGuid(&guid);

    HandleCloser owner;
    owner.set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (owner.set == INVALID_HANDLE_VALUE) {
        return devices;
    }

    SP_DEVICE_INTERFACE_DATA interface_data {};
    interface_data.cbSize = sizeof(interface_data);
    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(owner.set, nullptr, &guid, index, &interface_data); index++) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(owner.set, &interface_data, nullptr, 0, &needed, nullptr);
        if (needed == 0) {
            continue;
        }
        std::vector<uint8_t> buffer(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA device_info {};
        device_info.cbSize = sizeof(device_info);
        if (!SetupDiGetDeviceInterfaceDetailW(owner.set, &interface_data, detail, needed, nullptr,
                                              &device_info)) {
            continue;
        }

        // Skip everything that is not a Sony device before opening it.
        wchar_t  hardware_ids[1024] {};
        DWORD    type = 0, bytes = 0;
        bool     sony = false;
        if (SetupDiGetDeviceRegistryPropertyW(owner.set, &device_info, SPDRP_HARDWAREID, &type,
                                              reinterpret_cast<PBYTE>(hardware_ids),
                                              sizeof(hardware_ids), &bytes)) {
            const std::wstring ids =
                Lower(std::wstring(hardware_ids, bytes / sizeof(wchar_t)));
            sony = ids.find(L"vid_054c") != std::wstring::npos;
        }
        if (!sony) {
            continue;
        }

        HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                    nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            continue;
        }

        HIDD_ATTRIBUTES attributes {};
        attributes.Size = sizeof(attributes);
        if (!HidD_GetAttributes(handle, &attributes)) {
            CloseHandle(handle);
            continue;
        }

        HidDeviceInfo info;
        info.path    = detail->DevicePath;
        info.instance = ToUtf8(info.path);
        info.vid     = attributes.VendorID;
        info.pid     = attributes.ProductID;

        wchar_t product[256] {};
        if (HidD_GetProductString(handle, product, sizeof(product))) {
            info.name = ToUtf8(product);
        }

        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (HidD_GetPreparsedData(handle, &preparsed)) {
            HIDP_CAPS caps {};
            if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
                info.output_report_bytes = caps.OutputReportByteLength;
                info.usage_page          = caps.UsagePage;
                info.usage               = caps.Usage;
            }
            HidD_FreePreparsedData(preparsed);
        }
        CloseHandle(handle);

        const std::wstring lower_path = Lower(info.path);
        info.bluetooth = lower_path.find(L"bthenum") != std::wstring::npos ||
                         lower_path.find(L"00001124") != std::wstring::npos;

        devices.push_back(std::move(info));
    }
    return devices;
}

HidDevice::~HidDevice() {
    Close();
}

bool HidDevice::Open(const std::wstring& path) {
    Close();
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        error_ = Win32Error(GetLastError());
        handle_ = nullptr;
        return false;
    }
    // Keep writes from blocking forever if the link drops.
    COMMTIMEOUTS timeouts {};
    timeouts.WriteTotalTimeoutConstant = 100;
    SetCommTimeouts(handle_, &timeouts);
    return true;
}

void HidDevice::Close() {
    if (handle_ != nullptr) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

bool HidDevice::Write(const uint8_t* data, size_t len) {
    if (handle_ == nullptr) {
        error_ = "device is not open";
        return false;
    }
    DWORD written = 0;
    if (WriteFile(handle_, data, static_cast<DWORD>(len), &written, nullptr) &&
        written == len) {
        control_transfer_ = false;
        return true;
    }
    const DWORD write_error = GetLastError();

    // Some Bluetooth stacks reject oversized interrupt writes; retry as a
    // control transfer (HidD_SetOutputReport), which is what the DS5 accepts
    // for report 0x39 on those systems.
    if (HidD_SetOutputReport(handle_, const_cast<uint8_t*>(data), static_cast<ULONG>(len))) {
        control_transfer_ = true;
        return true;
    }
    error_ = Win32Error(write_error) + " / " + Win32Error(GetLastError());
    return false;
}
