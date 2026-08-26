#include "ipc_protocol.h"

#include <QIODevice>

namespace ps3hdd::ipc {

bool writeFrame(QIODevice* io, const QByteArray& payload) {
    QByteArray frame;
    {
        QDataStream ds(&frame, QIODevice::WriteOnly);
        setStreamVersion(ds);
        ds << static_cast<quint32>(payload.size());
    }
    frame.append(payload);
    qint64 total = 0;
    while (total < frame.size()) {
        const qint64 n = io->write(frame.constData() + total, frame.size() - total);
        if (n < 0) return false;
        total += n;
    }
    return true;
}

bool readFrameBlocking(QIODevice* io, QByteArray& payload, int timeoutMs) {
    auto pull = [&](qint64 need, QByteArray& out) -> bool {
        while (out.size() < need) {
            const QByteArray chunk = io->read(need - out.size());
            if (!chunk.isEmpty()) {
                out.append(chunk);
                continue;
            }
            if (!io->waitForReadyRead(timeoutMs)) return false;
        }
        return true;
    };
    QByteArray lenBuf;
    if (!pull(4, lenBuf)) return false;
    quint32 len = 0;
    {
        QDataStream ds(lenBuf);
        setStreamVersion(ds);
        ds >> len;
    }
    payload.clear();
    if (len == 0) return true;
    return pull(len, payload);
}

bool FrameParser::next(QByteArray& payload) {
    if (buffer_.size() < 4) return false;
    quint32 len = 0;
    {
        QDataStream ds(buffer_.left(4));
        setStreamVersion(ds);
        ds >> len;
    }
    if (static_cast<quint32>(buffer_.size()) < 4 + len) return false;
    payload = buffer_.mid(4, len);
    buffer_.remove(0, 4 + len);
    return true;
}

} // namespace ps3hdd::ipc