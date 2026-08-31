#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps3hdd::app { struct gameos_mount; }
namespace ps3hdd::fs { class ufs2_filesystem; }

namespace ps3hdd::ui {

struct job {
    enum kind { install_pkg, license_rif_only, full_activation, consistency, rebuild_database, restore_db, repair_counts, verify_pkg, license_batch, sync_exdata };

    kind type = consistency;
    QString device;
    std::vector<std::byte> eid;
    QString pkg_path;
    QString rap_path;
    QStringList rap_paths;
    QStringList exdata_users;
    bool verify_quick = false;
    QString content_id;
    std::vector<std::byte> idps;
    std::uint64_t account_id = 0;
    bool force = false;
    bool rebuild_db = false;
    bool skip_consistency = false;
    QString host_dir;

    quint16 broker_port = 0;
    QByteArray broker_token;

    enum fileop { fop_none, fop_delete, fop_copy, fop_move, fop_import, fop_extract };
    struct fs_item {
        std::uint64_t parent = 0;
        QString name;
        std::uint64_t inode = 0;
        bool is_dir = false;
    };
    fileop file_operation = fop_none;
    std::vector<fs_item> fop_items;
    std::uint64_t fop_dest = 0;
    QStringList fop_import_paths;
    QString fop_host_dest;         // fop_extract: host file path (file) or base folder (dir)
    bool set_rebuild = false;
};

class worker : public QObject {
    Q_OBJECT
public:
    explicit worker(job j) : job_(std::move(j)) {}

public slots:
    void run();
    void cancel() { cancel_.store(true); }

signals:
    void progress(QString line, int percent);
    void finished(bool ok, QString summary);

private:
    void run_verify_pkg();
    void run_file_operation(app::gameos_mount& m, fs::ufs2_filesystem& ufs);
    void run_consistency(app::gameos_mount& m, fs::ufs2_filesystem& ufs);
    void run_repair_counts(app::gameos_mount& m, fs::ufs2_filesystem& ufs);
    void run_rebuild_database(app::gameos_mount& m, fs::ufs2_filesystem& ufs);
    void run_restore_db(app::gameos_mount& m, fs::ufs2_filesystem& ufs);
    void run_install_pkg(app::gameos_mount& m, fs::ufs2_filesystem& ufs);
    void run_license_batch(app::gameos_mount& m, fs::ufs2_filesystem& ufs);
    void run_sync_exdata(app::gameos_mount& m, fs::ufs2_filesystem& ufs);
    void run_license_single(app::gameos_mount& m, fs::ufs2_filesystem& ufs);

    job job_;
    std::atomic<bool> cancel_{false};
};

} // namespace ps3hdd::ui