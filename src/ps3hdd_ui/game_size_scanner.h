#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps3hdd::ui {

class game_size_scanner : public QObject {
    Q_OBJECT
public:
    struct target {
        int index = 0;
        std::uint64_t game_inode = 0;
        std::vector<std::uint64_t> save_inodes;
    };

    game_size_scanner(QString device, std::vector<std::byte> eid, quint16 broker_port, QByteArray broker_token, std::vector<target> targets) : 
        device_(std::move(device)),
        eid_(std::move(eid)),
        port_(broker_port),
        token_(std::move(broker_token)),
        targets_(std::move(targets)) {}

    void cancel() { cancel_.store(true); }

public slots:
    void run();

signals:
    void sized(int index, quint64 game_size, quint64 save_size);
    void finished();

private:
    QString device_;
    std::vector<std::byte> eid_;
    quint16 port_ = 0;
    QByteArray token_;
    std::vector<target> targets_;
    std::atomic<bool> cancel_{false};
};

} // namespace ps3hdd::ui