#pragma once

#include "ipc_protocol.h"

#include <ps3hdd_disk/disk_source.h>

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>

class QTcpSocket;

namespace ps3hdd::ipc {

class broker_client {
public:
    broker_client();
    ~broker_client();
    void connect_to(quint16 port, const QByteArray& token, int timeout_ms = 8000);
    bool connected() const;

    QVector<DeviceInfo> enumerate();

    struct open_result {
        std::uint64_t total_size = 0;
        std::uint32_t sector_size = 512;
        bool can_write = false;
        std::string description;
    };
    open_result open(const QString& path, bool writable);

    std::vector<std::byte> read(std::uint64_t offset, std::uint64_t count);
    void write(std::uint64_t offset, std::span<const std::byte> data);

    struct eject_result {
        bool removed = false;
        bool spundown = false;
        QString message;
    };
    eject_result eject(const QString& path);

    void quit();

private:
    QByteArray request(const QByteArray& payload);

    std::unique_ptr<QTcpSocket> sock_;
    static constexpr int kIoTimeoutMs = 60000;
};

class ipc_disk_source : public disk::disk_source {
public:
    ipc_disk_source(broker_client& client, const broker_client::open_result& info);

    std::uint64_t total_size() const override { return total_size_; }
    std::uint32_t sector_size() const override { return sector_size_; }
    std::string description() const override { return description_; }
    bool can_write() const override { return can_write_; }

    std::vector<std::byte> read_sectors(std::uint64_t start_sector, std::uint64_t count) override;
    std::vector<std::byte> read_bytes(std::uint64_t offset, std::size_t count) override;
    void write_sectors(std::uint64_t start_sector, std::span<const std::byte> data) override;
    void write_bytes(std::uint64_t offset, std::span<const std::byte> data) override;

private:
    broker_client& client_;
    std::uint64_t total_size_;
    std::uint32_t sector_size_;
    bool can_write_;
    std::string description_;
};

bool launch_helper(const QString& helper_path, quint16 port, const QByteArray& token);

} // namespace ps3hdd::ipc