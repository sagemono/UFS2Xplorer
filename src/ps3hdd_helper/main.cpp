#include <ps3hdd_ipc/ipc_server.h>

#include <QCoreApplication>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTimer>

namespace {
QString arg_value(const QStringList& args, const QString& name) {
    const int i = args.indexOf(name);
    if (i >= 0 && i + 1 < args.size()) return args.at(i + 1);
    return {};
}

QFile* g_log = nullptr;
void log_line(const QString& s) {
    if (!g_log) return;
    g_log->write((QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + "  " + s + "\n").toUtf8());
    g_log->flush();
}
void msg_handler(QtMsgType, const QMessageLogContext&, const QString& msg) { log_line(msg); }
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    g_log = new QFile(QCoreApplication::applicationDirPath() + QStringLiteral("/helper_debug.log"));
    g_log->open(QIODevice::WriteOnly | QIODevice::Truncate);
    qInstallMessageHandler(msg_handler);
    log_line(QStringLiteral("=== helper start, args: %1").arg(args.join(QStringLiteral(" | "))));

    bool ok = false;
    const quint16 port = static_cast<quint16>(arg_value(args, QStringLiteral("--port")).toUInt(&ok));
    const QByteArray token = QByteArray::fromHex(arg_value(args, QStringLiteral("--token")).toLatin1());
    if (!ok || port == 0 || token.isEmpty()) {
        log_line(QStringLiteral("bad invocation: ok=%1 port=%2 token_len=%3") .arg(ok).arg(port).arg(token.size()));
        return 2; // bad invocation
    }

    ps3hdd::ipc::disk_server server(token);
    QObject::connect(&server, &ps3hdd::ipc::disk_server::finished, &app, &QCoreApplication::quit);
    if (!server.listen(port)) {
        log_line(QStringLiteral("listen(%1) FAILED").arg(port));
        return 3; // could not bind the loopback port?
    }
    log_line(QStringLiteral("listening on 127.0.0.1:%1").arg(port));

    QTimer::singleShot(30000, &app, [&app, &server]() {
        if (!server.has_client()) app.quit();
    });

    return app.exec();
}