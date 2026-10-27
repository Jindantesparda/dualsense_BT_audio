// Minimal Win32 HID access: enumerate interfaces, open one, write output reports.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct HidDeviceInfo {
    std::wstring path;
    std::string  name;          // UTF-8 product string
    std::string  instance;      // UTF-8 interface path, for display
    uint16_t     vid = 0;
    uint16_t     pid = 0;
    bool         bluetooth = false;
    uint16_t     output_report_bytes = 0;
    uint16_t     usage_page = 0;
    uint16_t     usage = 0;
};

std::vector<HidDeviceInfo> EnumerateHidDevices();
std::string                ToUtf8(const std::wstring& wide);

class HidDevice {
public:
    HidDevice() = default;
    ~HidDevice();
    HidDevice(const HidDevice&)            = delete;
    HidDevice& operator=(const HidDevice&) = delete;

    bool Open(const std::wstring& path);
    void Close();
    bool Write(const uint8_t* data, size_t len);

    // Returns true if the last write fell back to HidD_SetOutputReport.
    bool used_control_transfer() const { return control_transfer_; }
    const std::string& last_error() const { return error_; }

private:
    void*       handle_ = nullptr;
    bool        control_transfer_ = false;
    std::string error_;
};
