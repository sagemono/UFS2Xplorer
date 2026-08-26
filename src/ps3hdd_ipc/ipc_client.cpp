#include "ipc_client.h"

#include <QDataStream>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QProcess>
#include <QTcpSocket>
#include <QThread>

#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif

namespace ps3hdd::ipc {

namespace {
QByteArray make_request(Op op) {
    QByteArray payload;
    QDataStream ds(&payload, QIODevice::WriteOnly);
    setStreamVersion(ds);
    ds << static_cast<quint32>(op);
    return payload;
}
} // namespace

broker_client::broker_client() = default;
broker_client::~broker_client() = default;

void broker_client::connect_to(quint16 port, const QByteArray& token, int timeout_ms) {
    sock_ = std::make_unique<QTcpSocket>();
    sock_->setProxy(QNetworkProxy::NoProxy);
    sock_->connectToHost(QHostAddress::LocalHost, port);
    if (!sock_->waitForConnected(timeout_ms))
        throw std::runtime_error("could not connect to disk helper: " + sock_->errorString().toStdString());

    QByteArray req = make_request(Op::Hello);
    {
        QDataStream ds(&req, QIODevice::WriteOnly | QIODevice::Append);
        setStreamVersion(ds);
        ds << token << kProtocolVersion;
    }
    QByteArray reply = request(req);
    (void)reply;
}

bool broker_client::connected() const {
    return sock_ && sock_->state() == QAbstractSocket::ConnectedState;
}

QByteArray broker_client::request(const QByteArray& payload) {
    if (!sock_) throw std::runtime_error("disk helper not connected");
    if (!writeFrame(sock_.get(), payload))
        throw std::runtime_error("disk helper write failed: " + sock_->errorString().toStdString());
    QByteArray reply;
    if (!readFrameBlocking(sock_.get(), reply, kIoTimeoutMs))
        throw std::runtime_error("disk helper did not respond!");

    QDataStream ds(reply);
    setStreamVersion(ds);
    quint32 status = 0;
    ds >> status;
    if (static_cast<Status>(status) != Status::Ok) {
        QString msg;
        ds >> msg;
        if (msg.isEmpty()) msg = QStringLiteral("disk helper error %1").arg(status);
        throw std::runtime_error(msg.toStdString());
    }
    return reply.mid(sizeof(quint32));
}

QVector<DeviceInfo> broker_client::enumerate() {
    const QByteArray body = request(make_request(Op::Enumerate));
    QDataStream ds(body);
    setStreamVersion(ds);
    quint32 count = 0;
    ds >> count;
    QVector<DeviceInfo> out;
    out.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        DeviceInfo d;
        ds >> d.path >> d.description >> d.size >> d.serial >> d.raw;
        out.push_back(d);
    }
    return out;
}

broker_client::open_result broker_client::open(const QString& path, bool writable) {
    QByteArray req = make_request(Op::Open);
    {
        QDataStream ds(&req, QIODevice::WriteOnly | QIODevice::Append);
        setStreamVersion(ds);
        ds << writable << path;
    }
    const QByteArray body = request(req);
    QDataStream ds(body);
    setStreamVersion(ds);
    open_result r;
    quint64 size = 0;
    quint32 ssize = 0;
    bool cw = false;
    QString desc;
    ds >> size >> ssize >> cw >> desc;
    r.total_size = size;
    r.sector_size = ssize;
    r.can_write = cw;
    r.description = desc.toStdString();
    return r;
}

std::vector<std::byte> broker_client::read(std::uint64_t offset, std::uint64_t count) {
    QByteArray req = make_request(Op::Read);
    {
        QDataStream ds(&req, QIODevice::WriteOnly | QIODevice::Append);
        setStreamVersion(ds);
        ds << static_cast<quint64>(offset) << static_cast<quint64>(count);
    }
    const QByteArray body = request(req);
    QDataStream ds(body);
    setStreamVersion(ds);
    QByteArray data;
    ds >> data;
    std::vector<std::byte> out(data.size());
    std::memcpy(out.data(), data.constData(), data.size());
    return out;
}

void broker_client::write(std::uint64_t offset, std::span<const std::byte> data) {
    QByteArray req = make_request(Op::Write);
    {
        QDataStream ds(&req, QIODevice::WriteOnly | QIODevice::Append);
        setStreamVersion(ds);
        ds << static_cast<quint64>(offset) << QByteArray(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size()));
    }
    request(req);
}

broker_client::eject_result broker_client::eject(const QString& path) {
    QByteArray req = make_request(Op::Eject);
    {
        QDataStream ds(&req, QIODevice::WriteOnly | QIODevice::Append);
        setStreamVersion(ds);
        ds << path;
    }
    const QByteArray body = request(req);
    QDataStream ds(body);
    setStreamVersion(ds);
    eject_result r;
    ds >> r.removed >> r.spundown >> r.message;
    return r;
}

void broker_client::quit() {
    if (!sock_) return;
    try {
        writeFrame(sock_.get(), make_request(Op::Quit));
        sock_->flush();
        sock_->waitForBytesWritten(1000);
    } catch (...) {
    }
}

ipc_disk_source::ipc_disk_source(broker_client& client, const broker_client::open_result& info) : 
client_(client), total_size_(info.total_size), sector_size_(info.sector_size ? info.sector_size : 512), can_write_(info.can_write), description_(info.description) {}

std::vector<std::byte> ipc_disk_source::read_bytes(std::uint64_t offset, std::size_t count) {
    return client_.read(offset, count);
}

std::vector<std::byte> ipc_disk_source::read_sectors(std::uint64_t start_sector, std::uint64_t count) {
    return client_.read(start_sector * sector_size_, count * sector_size_);
}

void ipc_disk_source::write_bytes(std::uint64_t offset, std::span<const std::byte> data) {
    client_.write(offset, data);
}

void ipc_disk_source::write_sectors(std::uint64_t start_sector, std::span<const std::byte> data) {
    client_.write(start_sector * sector_size_, data);
}


bool launch_helper(const QString& helper_path, quint16 port, const QByteArray& token) {
    const QString args = QStringLiteral("--port %1 --token %2").arg(port).arg(QString::fromLatin1(token.toHex()));
#ifdef _WIN32
    const std::wstring exe = helper_path.toStdWString();
    const std::wstring params = args.toStdWString();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
#else
    return QProcess::startDetached(helper_path, args.split(' '));
#endif
}

} // namespace ps3hdd::ipc