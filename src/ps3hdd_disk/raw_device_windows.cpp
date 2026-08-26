#include "raw_device.h"

#if defined(_WIN32)

#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace ps3hdd::disk {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n ? n - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

sector_info query_sizes(HANDLE h) {
    sector_info info{};
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageAccessAlignmentProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR desc{};
    DWORD returned = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, &desc, sizeof desc, &returned, nullptr) &&
        returned >= sizeof desc) {
        info.logical = desc.BytesPerLogicalSector ? desc.BytesPerLogicalSector : 512;
        info.physical = desc.BytesPerPhysicalSector ? desc.BytesPerPhysicalSector : info.logical;
    }
    return info;
}

class windows_raw_device : public raw_device {
public:
    windows_raw_device(const std::string& path, bool writable) : path_(path), writable_(writable) {
        const std::wstring wpath = widen(path);
        DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0);
        read_ = CreateFileW(wpath.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (read_ == INVALID_HANDLE_VALUE)
            throw device_io_error("CreateFileW failed for " + path + " (err " + std::to_string(GetLastError()) + ")");

        sectors_ = query_sizes(read_);

        GET_LENGTH_INFORMATION len{};
        DWORD returned = 0;
        if (DeviceIoControl(read_, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len, sizeof len, &returned, nullptr))
            size_ = static_cast<std::uint64_t>(len.Length.QuadPart);
    }

    ~windows_raw_device() override {
        if (read_ != INVALID_HANDLE_VALUE) CloseHandle(read_);
    }

    std::uint64_t size() const override { return size_; }
    sector_info sectors() const override { return sectors_; }
    bool writable() const override { return writable_; }
    std::uint32_t required_alignment() const override { return sectors_.logical; }
    std::string describe() const override { return path_; }

    std::size_t read_at(std::uint64_t offset, std::span<std::byte> buf) override {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(read_, buf.data(), static_cast<DWORD>(buf.size()), &got, &ov)) {
            const DWORD err = GetLastError();
            if (err == ERROR_HANDLE_EOF) return got;
            throw device_io_error("ReadFile failed at offset " + std::to_string(offset) + " (err " + std::to_string(err) + ")");
        }
        return got;
    }

    void write_at(std::uint64_t offset, std::span<const std::byte> buf) override {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xffffffffu);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD wrote = 0;
        if (!WriteFile(read_, buf.data(), static_cast<DWORD>(buf.size()), &wrote, &ov) ||
            wrote != buf.size())
            throw device_io_error("WriteFile failed at offset " + std::to_string(offset) + " (err " + std::to_string(GetLastError()) + ")");
    }

private:
    std::string path_;
    bool writable_;
    HANDLE read_ = INVALID_HANDLE_VALUE;
    std::uint64_t size_ = 0;
    sector_info sectors_{};
};

} // namespace

std::unique_ptr<raw_device> open_raw_device(const std::string& path, bool writable) {
    return std::make_unique<windows_raw_device>(path, writable);
}

std::string query_serial_number(const std::string& path) {
    HANDLE h = CreateFileW(widen(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    std::vector<char> buf(1024, 0);
    DWORD returned = 0;
    std::string serial;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof query, buf.data(), static_cast<DWORD>(buf.size()), &returned, nullptr)) {
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
        if (desc->SerialNumberOffset != 0 && desc->SerialNumberOffset < returned)
            serial = std::string(buf.data() + desc->SerialNumberOffset);
    }
    CloseHandle(h);

    const auto b = serial.find_first_not_of(" \t");
    const auto e = serial.find_last_not_of(" \t");
    return b == std::string::npos ? std::string{} : serial.substr(b, e - b + 1);
}

namespace {

const GUID kDiskInterfaceGuid = {
    0x53f56307, 0xb6bf, 0x11d0, {0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b}};

int device_number_of(HANDLE h) {
    STORAGE_DEVICE_NUMBER sdn{};
    DWORD ret = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &sdn, sizeof sdn, &ret, nullptr))
        return static_cast<int>(sdn.DeviceNumber);
    return -1;
}

bool ata_standby_immediate(HANDLE h) {
    ATA_PASS_THROUGH_DIRECT ap{};
    ap.Length = sizeof ap;
    ap.AtaFlags = ATA_FLAGS_DRDY_REQUIRED;
    ap.TimeOutValue = 10;
    ap.DataTransferLength = 0;
    ap.DataBuffer = nullptr;
    ap.CurrentTaskFile[6] = 0xE0; // STANDBY IMMEDIATE
    DWORD ret = 0;
    return DeviceIoControl(h, IOCTL_ATA_PASS_THROUGH_DIRECT, &ap, sizeof ap, &ap, sizeof ap, &ret, nullptr) != 0;
}

bool scsi_stop_unit(HANDLE h) {
    struct sptd_t {
        SCSI_PASS_THROUGH_DIRECT s;
        UCHAR sense[32];
    } sptd{};
    sptd.s.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    sptd.s.CdbLength = 6;
    sptd.s.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
    sptd.s.DataTransferLength = 0;
    sptd.s.DataBuffer = nullptr;
    sptd.s.TimeOutValue = 10;
    sptd.s.SenseInfoLength = sizeof sptd.sense;
    sptd.s.SenseInfoOffset = offsetof(sptd_t, sense);
    sptd.s.Cdb[0] = 0x1B; // START STOP UNIT
    DWORD ret = 0;
    return DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptd, sizeof sptd, &sptd, sizeof sptd, &ret, nullptr) != 0;
}

bool devinst_for_disk(int devnum, DEVINST& out) {
    HDEVINFO info = SetupDiGetClassDevsW(&kDiskInterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &kDiskInterfaceGuid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifd, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        std::vector<char> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dd{};
        dd.cbSize = sizeof dd;
        if (!SetupDiGetDeviceInterfaceDetailW(info, &ifd, detail, need, nullptr, &dd)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        const int n = device_number_of(h);
        CloseHandle(h);
        if (n == devnum) { out = dd.DevInst; found = true; break; }
    }
    SetupDiDestroyDeviceInfoList(info);
    return found;
}

bool try_eject(DEVINST dev) {
    DEVINST cur = dev;
    for (int up = 0; up < 6; ++up) {
        if (CM_Request_Device_EjectW(cur, nullptr, nullptr, 0, 0) == CR_SUCCESS) return true;
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, cur, 0) != CR_SUCCESS) break;
        cur = parent;
    }
    return false;
}

} // namespace

bool eject_device(const std::string& path, bool& removed, bool& spundown, std::string& message) {
    removed = false;
    spundown = false;
    const std::wstring wpath = widen(path);

    int devnum = -1;
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        devnum = device_number_of(h);
        CloseHandle(h); // must be closed before an OS eject!
    }

    // usb caddies, docks, etc.
    if (devnum >= 0) {
        DEVINST dev;
        if (devinst_for_disk(devnum, dev) && try_eject(dev)) {
            removed = true;
            message = "Drive ejected, you can now remove it from the caddy";
            return true;
        }
    }
    
    // physical disk
    HANDLE h2 = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h2 == INVALID_HANDLE_VALUE)
        h2 = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h2 != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h2);
        const bool ok = ata_standby_immediate(h2) || scsi_stop_unit(h2);
        CloseHandle(h2);
        if (ok) {
            spundown = true;
            message = "Drive heads parked, you can now remove the drive safely";
            return true;
        }
    }

    message = "Could not eject or spin down the drive (it may not support it, or is still in use)";
    return false;
}

} // namespace ps3hdd::disk

#endif // _WIN32