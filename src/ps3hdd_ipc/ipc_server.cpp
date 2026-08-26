#include "ipc_server.h"

#include <ps3hdd_disk/raw_device.h>

#include <QDataStream>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>

#include <cstring>
#include <exception>

namespace ps3hdd::ipc {

disk_session::disk_session(QTcpSocket* socket, QByteArray token, QObject* parent) : QObject(parent), socket_(socket), token_(std::move(token)) {
    socket_->setParent(this);
    connect(socket_, &QTcpSocket::readyRead, this, &disk_session::on_ready_read);
    connect(socket_, &QTcpSocket::disconnected, this, &disk_session::on_disconnected);
}

void disk_session::on_disconnected() {
    emit closed(this);
}

void disk_session::on_ready_read() {
    parser_.feed(socket_->readAll());
    QByteArray frame;
    while (parser_.next(frame)) handle_frame(frame);
}

void disk_session::reply_ok(const QByteArray& body) {
    QByteArray payload;
    {
        QDataStream ds(&payload, QIODevice::WriteOnly);
        setStreamVersion(ds);
        ds << static_cast<quint32>(Status::Ok);
    }
    payload.append(body);
    writeFrame(socket_, payload);
}

void disk_session::reply_error(const QString& message) {
    QByteArray payload;
    QDataStream ds(&payload, QIODevice::WriteOnly);
    setStreamVersion(ds);
    ds << static_cast<quint32>(Status::Error) << message;
    writeFrame(socket_, payload);
}

void disk_session::handle_frame(const QByteArray& payload) {
    QDataStream ds(payload);
    setStreamVersion(ds);
    quint32 op_raw = 0;
    ds >> op_raw;
    const Op op = static_cast<Op>(op_raw);

    if (op == Op::Hello) {
        QByteArray token;
        quint32 version = 0;
        ds >> token >> version;
        if (version != kProtocolVersion || token != token_) {
            QByteArray p;
            QDataStream d(&p, QIODevice::WriteOnly);
            setStreamVersion(d);
            d << static_cast<quint32>(Status::AuthFailed);
            writeFrame(socket_, p);
            socket_->disconnectFromHost();
            return;
        }
        authed_ = true;
        qWarning("disk_session: client authenticated");
        reply_ok();
        return;
    }
    if (!authed_) {
        reply_error(QStringLiteral("not authenticated"));
        return;
    }

    try {
        switch (op) {
        case Op::Enumerate: {
            QByteArray body;
            QDataStream out(&body, QIODevice::WriteOnly);
            setStreamVersion(out);
            QByteArray devs;
            QDataStream dv(&devs, QIODevice::WriteOnly);
            setStreamVersion(dv);
            quint32 count = 0;
            for (int n = 0; n < 16; ++n) {
                const std::string path = "\\\\.\\PhysicalDrive" + std::to_string(n);
                try {
                    auto raw = disk::open_raw_device(path, /*writable=*/false);
                    disk::physical_disk_source src(std::move(raw));
                    const QString serial = QString::fromStdString(disk::query_serial_number(path));
                    bool is_raw = true;
                    try {
                        const auto boot = src.read_bytes(0, 1024);
                        const bool mbr = boot.size() >= 512 && std::to_integer<int>(boot[0x1FE]) == 0x55 && std::to_integer<int>(boot[0x1FF]) == 0xAA;
                        const bool gpt = boot.size() >= 520 && std::memcmp(boot.data() + 512, "EFI PART", 8) == 0;
                        is_raw = !mbr && !gpt;
                    } catch (const std::exception&) {
                    }
                    dv << QString::fromStdString(path)
                       << QString::fromStdString(src.description())
                       << static_cast<quint64>(src.total_size()) << serial << is_raw;
                    ++count;
                } catch (const std::exception&) {
                    // a
                }
            }
            out << count;
            body.append(devs);
            reply_ok(body);
            break;
        }
        case Op::Open: {
            bool writable = false;
            QString path;
            ds >> writable >> path;
            device_.reset();
            std::unique_ptr<disk::raw_device> raw;
            bool opened_writable = writable;
            try {
                raw = disk::open_raw_device(path.toStdString(), writable);
            } catch (const std::exception&) {
                if (!writable) throw;
                opened_writable = false; // fall back to readonly
                raw = disk::open_raw_device(path.toStdString(), false);
            }
            device_ = std::make_unique<disk::physical_disk_source>(std::move(raw));
            QByteArray body;
            QDataStream out(&body, QIODevice::WriteOnly);
            setStreamVersion(out);
            out << static_cast<quint64>(device_->total_size())
                << static_cast<quint32>(device_->sector_size())
                << (opened_writable && device_->can_write())
                << QString::fromStdString(device_->description());
            reply_ok(body);
            break;
        }
        case Op::Read: {
            quint64 offset = 0, count = 0;
            ds >> offset >> count;
            if (!device_) { reply_error(QStringLiteral("no device open")); break; }
            const auto data = device_->read_bytes(offset, static_cast<std::size_t>(count));
            QByteArray body;
            QDataStream out(&body, QIODevice::WriteOnly);
            setStreamVersion(out);
            out << QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size()));
            reply_ok(body);
            break;
        }
        case Op::Write: {
            quint64 offset = 0;
            QByteArray data;
            ds >> offset >> data;
            if (!device_) { reply_error(QStringLiteral("no device open")); break; }
            device_->write_bytes(offset, {reinterpret_cast<const std::byte*>(data.constData()), static_cast<std::size_t>(data.size())});
            reply_ok();
            break;
        }
        case Op::Eject: {
            QString path;
            ds >> path;
            device_.reset(); // release handle so the OS can eject or spindown
            bool removed = false, spundown = false;
            std::string message;
            const bool ok = disk::eject_device(path.toStdString(), removed, spundown, message);
            if (!ok) { reply_error(QString::fromStdString(message)); break; }
            QByteArray body;
            QDataStream out(&body, QIODevice::WriteOnly);
            setStreamVersion(out);
            out << removed << spundown << QString::fromStdString(message);
            reply_ok(body);
            break;
        }
        case Op::Quit:
            reply_ok();
            emit quit_requested();
            break;
        default:
            reply_error(QStringLiteral("unknown op"));
            break;
        }
    } catch (const std::exception& ex) {
        reply_error(QString::fromUtf8(ex.what()));
    }
}


disk_server::disk_server(QByteArray token, QObject* parent) : QObject(parent), token_(std::move(token)) {}

disk_server::~disk_server() = default;

bool disk_server::listen(quint16 port) {
    server_ = new QTcpServer(this);
    server_->setProxy(QNetworkProxy::NoProxy);
    connect(server_, &QTcpServer::newConnection, this, &disk_server::on_new_connection);
    return server_->listen(QHostAddress::LocalHost, port);
}

void disk_server::on_new_connection() {
    while (QTcpSocket* sock = server_->nextPendingConnection()) {
        qWarning("disk_server: accepted a connection from %s", qPrintable(sock->peerAddress().toString()));
        auto* session = new disk_session(sock, token_, this);
        ++live_sessions_;
        saw_client_ = true;
        connect(session, &disk_session::closed, this, &disk_server::on_session_closed);
        connect(session, &disk_session::quit_requested, this, &disk_server::finished);
    }
}

void disk_server::on_session_closed(disk_session* s) {
    if (s) s->deleteLater();
    if (--live_sessions_ <= 0)
        emit finished();
}

} // namespace ps3hdd::ipc