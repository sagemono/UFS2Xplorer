#pragma once

#include "worker.h"

#include <QByteArray>
#include <QDialog>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QThread;

namespace ps3hdd::ui {

class install_dialog : public QDialog {
    Q_OBJECT
public:
    struct pkg_item {
        QString path;
        QString title;
        QString title_id;
        QString content_id;
        QString category;
        QString app_ver;
        std::uint32_t content_type = 0;
        std::uint64_t size = 0;
        int files = 0;
        QByteArray icon;
        QString error;
        bool verified = false;
        QString kind;
        int rank = 3;
    };

    install_dialog(QStringList paths, QString device, std::vector<std::byte> eid, quint16 broker_port, QByteArray broker_token, QWidget* parent = nullptr);

    bool installed_ok() const { return installed_ok_; }
    bool wrote() const { return wrote_; }

private slots:
    void start_install();
    void request_cancel();
    void on_progress(const QString& line, int percent);
    void on_finished(bool ok, const QString& summary);

private:
    void parse_all();
    void classify();
    void run_current();

    QStringList paths_;
    QString device_;
    std::vector<std::byte> eid_;
    quint16 port_ = 0;
    QByteArray token_;

    std::vector<pkg_item> items_;
    int current_ = -1;
    bool installing_ = false;
    bool installed_ok_ = false;
    bool wrote_ = false;
    bool verify_first_ = false;
    bool verifying_ = false;
    int last_ok_index_ = -1;

    QLabel* icon_label_ = nullptr;
    QLabel* header_ = nullptr;
    QTableWidget* table_ = nullptr;
    QWidget* bars_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QProgressBar* overall_bar_ = nullptr;
    std::uint64_t total_bytes_ = 0;
    std::uint64_t bytes_done_ = 0;
    QLabel* status_ = nullptr;
    QPushButton* install_btn_ = nullptr;
    QPushButton* cancel_btn_ = nullptr;
    QThread* thread_ = nullptr;
    worker* worker_ = nullptr;
};

} // namespace ps3hdd::ui
