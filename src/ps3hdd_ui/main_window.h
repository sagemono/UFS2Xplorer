#pragma once

#include "key_store.h"
#include "ufs2_model.h"
#include "worker.h"

#include <ps3hdd_app/gameos.h>
#include <ps3hdd_disk/disk_source.h>
#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>
#include <ps3hdd_ipc/ipc_client.h>

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QAction>
#include <QMainWindow>
#include <QPersistentModelIndex>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLineEdit;
class QLabel;
class QTreeView;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QCheckBox;
class QThread;
class QVBoxLayout;

namespace ps3hdd::ui {

class game_preview;

class main_window : public QMainWindow {
    Q_OBJECT
public:
    main_window();

private slots:
    void refresh_devices();
    void add_console();
    void manage_drives();
    void open_device();
    void eject_drive();
    void install_pkg();
    void verify_package();
    void open_settings();
    void act_edit_sfo();
    void install_license();
    void sync_user_licenses();
    void check_consistency();
    void repair_counts();
    void reclaim_orphans();
    void show_games();
    void rebuild_database();
    void backup_database();
    void restore_database();
    void cancel_job();
    void show_tree_menu(const QPoint& pos);
    void act_extract();
    void act_new_folder();
    void act_import();
    void act_import_folder();
    void act_delete();
    void act_copy();
    void act_cut();
    void act_paste();
    void act_properties();
    void refresh_view();
    void nav_up();
    void nav_back();
    void nav_forward();
    void export_log();
    void apply_filter(const QString& text);
    void on_external_drop(const QStringList& paths, quint64 dir_inode);

protected:
    void closeEvent(QCloseEvent* e) override;

private:

    void build_menus();
    void build_disk_row(QVBoxLayout* root, QPushButton*& refresh, QPushButton*& open);
    void build_action_row(QVBoxLayout* root);
    void build_browser(QVBoxLayout* root);
    void wire_signals(QPushButton* refresh, QPushButton* open);

    void log(const QString& line);
    QString current_serial() const;
    QString current_eid_hex() const;
    void navigate_to_path(const QString& path);
    void update_preview(const QModelIndex& index);
    void apply_settings();
    bool base_job(job& j);
    void start_job(job j, const QString& title);
    void start_pkg_batch(const QStringList& pkg_paths, const QStringList& raps = {});
    void do_license(const QString& rap_prefill);
    void install_licenses_batch(const QStringList& raps);
    QString backup_db_to_temp();
    void set_busy(bool busy);
    void set_speed(double bytes_per_sec);
    void remount(); 
    bool require_writer();
    void update_capacity();
    bool ensure_broker();

    struct game_row {
        QString title_id;
        QString title;
        QString category;
        std::uint64_t size = 0;
        std::uint64_t save_size = 0;
        QByteArray icon;
        std::uint64_t game_inode = 0;
        std::vector<std::uint64_t> save_inodes;
    };
    std::vector<game_row> list_installed_games();
    void uninstall_game(const QString& title_id);
    void set_disk_actions_enabled(bool on);
    QString current_device_path() const;

    key_store keys_;
    QComboBox* device_combo_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* speed_label_ = nullptr;
    QString mount_desc_;
    QTreeView* tree_ = nullptr;
    ufs2_model* model_ = nullptr;
    game_preview* preview_ = nullptr;
    bool show_art_panel_ = false;
    QLineEdit* path_edit_ = nullptr;
    QLineEdit* filter_edit_ = nullptr;
    QPushButton* back_btn_ = nullptr;
    QPushButton* fwd_btn_ = nullptr;
    QList<QPersistentModelIndex> nav_history_;
    int nav_pos_ = -1;
    bool nav_navigating_ = false;

    struct art_entry {
        bool has = false;
        QString title;
        QString subtitle;
        QByteArray icon0;
        QByteArray pic1;
    };
    QHash<quint64, art_entry> art_cache_;
    const art_entry& art_for_dir(std::uint64_t inode);
    QPlainTextEdit* log_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* install_btn_ = nullptr;
    QPushButton* license_btn_ = nullptr;
    QPushButton* fsck_btn_ = nullptr;
    QPushButton* repair_btn_ = nullptr;
    QPushButton* reclaim_btn_ = nullptr;
    QAction* lv2_policy_act_ = nullptr;
    QPushButton* games_btn_ = nullptr;
    QPushButton* rebuilddb_btn_ = nullptr;
    QPushButton* backupdb_btn_ = nullptr;
    QPushButton* restoredb_btn_ = nullptr;
    QPushButton* cancel_btn_ = nullptr;
    QPushButton* eject_btn_ = nullptr;
    QThread* job_thread_ = nullptr;
    worker* job_worker_ = nullptr;

    struct clip_entry {
        std::uint64_t parent_inode = 0;
        QString name;
        bool is_dir = false;
        std::uint64_t inode = 0;
    };
    std::vector<clip_entry> clipboard_;
    bool clip_is_cut_ = false;
    QList<QPersistentModelIndex> pending_refresh_;
    QString pending_rap_;
    ps3hdd::ipc::broker_client broker_;
    quint16 broker_port_ = 0;
    QByteArray broker_token_;
    bool broker_ready_ = false;

    std::shared_ptr<disk::disk_source> raw_;
    std::optional<app::gameos_mount> mount_;
    std::unique_ptr<fs::ufs2_filesystem> fs_;
    std::unique_ptr<fs::ufs2_writer> writer_;
};

} // namespace ps3hdd::ui