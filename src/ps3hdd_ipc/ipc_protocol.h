#pragma once

#include <QByteArray>
#include <QDataStream>
#include <QString>
#include <QVector>

#include <cstdint>

class QIODevice;

namespace ps3hdd::ipc {

inline constexpr quint32 kProtocolVersion = 2;

enum class Op : quint32 {
    Hello = 1,
    Enumerate = 2,
    Open = 3,
    Read = 4,
    Write = 5,
    Quit = 6,
    Eject = 7,
};

enum class Status : quint32 {
    Ok = 0,
    Error = 1,
    AuthFailed = 2,
};

struct DeviceInfo {
    QString path;
    QString description;
    quint64 size = 0;
    QString serial;
    bool raw = false;
};


inline void setStreamVersion(QDataStream& ds) { ds.setVersion(QDataStream::Qt_6_0); }
bool writeFrame(QIODevice* io, const QByteArray& payload);
bool readFrameBlocking(QIODevice* io, QByteArray& payload, int timeoutMs);
class FrameParser {
public:
    void feed(const QByteArray& bytes) { buffer_.append(bytes); }
    bool next(QByteArray& payload);

private:
    QByteArray buffer_;
};

} // namespace ps3hdd::ipc