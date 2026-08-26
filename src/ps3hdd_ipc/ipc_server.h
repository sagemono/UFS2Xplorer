#pragma once

#include "ipc_protocol.h"

#include <ps3hdd_disk/physical_disk_source.h>

#include <QByteArray>
#include <QObject>

#include <memory>

class QTcpServer;
class QTcpSocket;

namespace ps3hdd::ipc {

class disk_session : public QObject {
    Q_OBJECT
public:
    disk_session(QTcpSocket* socket, QByteArray token, QObject* parent = nullptr);

signals:
    void closed(disk_session* self);
    void quit_requested();

private slots:
    void on_ready_read();
    void on_disconnected();

private:
    void handle_frame(const QByteArray& payload);
    void reply_ok(const QByteArray& body = {});
    void reply_error(const QString& message);

    QTcpSocket* socket_;
    QByteArray token_;
    FrameParser parser_;
    bool authed_ = false;
    std::unique_ptr<disk::physical_disk_source> device_;
};

class disk_server : public QObject {
    Q_OBJECT
public:
    explicit disk_server(QByteArray token, QObject* parent = nullptr);
    ~disk_server() override;

    bool listen(quint16 port);
    bool has_client() const { return saw_client_; }

signals:
    void finished();

private slots:
    void on_new_connection();
    void on_session_closed(disk_session* s);

private:
    QByteArray token_;
    QTcpServer* server_ = nullptr;
    int live_sessions_ = 0;
    bool saw_client_ = false;
};

} // namespace ps3hdd::ipc