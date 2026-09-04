// held together with duct tape and hope

#include "main_window.h"
#include "console_wizard.h"
#include "drives_dialog.h"
#include "game_preview.h"
#include "fs_util.h"
#include "game_size_scanner.h"
#include "install_dialog.h"
#include "license_dialog.h"
#include "settings_dialog.h"
#include "sfo_editor.h"
#include "sfo_util.h"

#include <ps3hdd_app/database.h>
#include <ps3hdd_disk/disk_source.h>
#include <ps3hdd_pkg/ps3_pkg_reader.h>

#include <QAbstractItemView>
#include <QAction>
#include <QCloseEvent>
#include <QSettings>

#ifndef UFS2XPLORER_VERSION
#define UFS2XPLORER_VERSION "0.9.5"
#endif

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QSysInfo>
#include <QUrl>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMap>
#include <QMenu>
#include <QMenuBar>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QModelIndex>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProxyStyle>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStyleHintReturn>
#include <QStyleOption>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cctype>
#include <exception>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <span>
#include <vector>

namespace {
QString log_dir() { return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/logs"); }
} // namespace


#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace ps3hdd::ui {

namespace {

QString allow_low_integrity_drops(QWidget* w) {
#ifdef _WIN32
    const BOOL a = ChangeWindowMessageFilter(WM_DROPFILES, MSGFLT_ADD);
    const BOOL b = ChangeWindowMessageFilter(WM_COPYDATA, MSGFLT_ADD);
    const BOOL c = ChangeWindowMessageFilter(0x0049 /*WM_COPYGLOBALDATA*/, MSGFLT_ADD);
    auto hwnd = reinterpret_cast<HWND>(w->winId());
    ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, 0x0049, MSGFLT_ALLOW, nullptr);
    return QStringLiteral("UIPI drop filter installed (dropfiles=%1 copydata=%2 globaldata=%3)") // could remove this later since UAC is required here now
        .arg(a).arg(b).arg(c);
#else
    (void)w;
    return QStringLiteral("UIPI filter: not applicable (unix)");
#endif
}

std::vector<std::byte> parse_hex(const QString& s) {
    std::string h;
    for (QChar c : s)
        if (std::isxdigit(static_cast<unsigned char>(c.toLatin1()))) h.push_back(c.toLatin1());
    std::vector<std::byte> out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(static_cast<std::byte>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

class fs_tree_view : public QTreeView {
public:
    using QTreeView::QTreeView;

protected:
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (wanted(e->mimeData())) e->acceptProposedAction();
        else QTreeView::dragEnterEvent(e);
    }
    void dragMoveEvent(QDragMoveEvent* e) override {
        QTreeView::dragMoveEvent(e);
        if (wanted(e->mimeData())) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override {
        if (!wanted(e->mimeData())) { QTreeView::dropEvent(e); return; }
        const QModelIndex idx = indexAt(e->position().toPoint());
        if (model() && model()->dropMimeData(e->mimeData(), e->dropAction(), -1, -1, idx))
            e->acceptProposedAction();
        else
            e->ignore();
    }

private:
    static bool wanted(const QMimeData* m) {
        return m && (m->hasUrls() || m->hasFormat(ufs2_model::nodes_mime()));
    }
};

class fast_tooltip_style : public QProxyStyle {
public:
    int styleHint(StyleHint hint, const QStyleOption* opt, const QWidget* w, QStyleHintReturn* ret) const override {
        if (hint == SH_ToolTip_WakeUpDelay) return 120;
        if (hint == SH_ToolTip_FallAsleepDelay) return 0;
        return QProxyStyle::styleHint(hint, opt, w, ret);
    }
};
} // namespace

main_window::main_window() {
    setWindowTitle(QStringLiteral("UFS2Xplorer"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/drive.bmp")));
    resize(900, 640);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    build_menus();

    QPushButton* refresh = nullptr;
    QPushButton* open = nullptr;
    build_disk_row(root, refresh, open);

    status_ = new QLabel(QStringLiteral("No disk mounted."));
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addWidget(status_, 1);
    speed_label_ = new QLabel();
    statusBar()->addPermanentWidget(speed_label_);

    build_action_row(root);
    build_browser(root);

    setCentralWidget(central);

    wire_signals(refresh, open);

    {
        const auto btns = this->findChildren<QPushButton*>();
        int h = 0;
        for (QPushButton* b : btns) {
            b->setAutoDefault(false);
            b->setDefault(false);
            b->setIconSize(QSize(16, 16));
            h = std::max(h, b->sizeHint().height());
        }
        for (QPushButton* b : btns) b->setMinimumHeight(h);
    }

    log(allow_low_integrity_drops(this));

    apply_settings();

    {
        QSettings s(settings_keys::org(), settings_keys::app());
        if (s.contains(QStringLiteral("win/geometry")))
            restoreGeometry(s.value(QStringLiteral("win/geometry")).toByteArray());
        if (tree_ && tree_->header() && s.contains(QStringLiteral("win/treeHeader")))
            tree_->header()->restoreState(s.value(QStringLiteral("win/treeHeader")).toByteArray());
    }
    if (back_btn_) back_btn_->setEnabled(false);
    if (fwd_btn_) fwd_btn_->setEnabled(false);

    if (keys_.entries().isEmpty())
        QTimer::singleShot(0, this, &main_window::add_console);
    else
        log(QStringLiteral("Click 'Refresh' to detect drives (prompts for admin once)."));
}

void main_window::build_menus() {
    auto* tools_menu = menuBar()->addMenu(QStringLiteral("&Tools"));
    tools_menu->addAction(QStringLiteral("Manage Drives..."), this, &main_window::manage_drives);
    tools_menu->addAction(QStringLiteral("Settings..."), this, &main_window::open_settings);
    tools_menu->addAction(QStringLiteral("Verify PKG..."), this, &main_window::verify_package);
    tools_menu->addAction(QStringLiteral("Install Licenses (RAPs)..."), this, [this] {
        const QStringList raps = QFileDialog::getOpenFileNames(
            this, QStringLiteral("Select .rap license files (multi-select)"), QString(),
            QStringLiteral("RAP files (*.rap);;All files (*)"));
        if (!raps.isEmpty()) install_licenses_batch(raps);
    });
    tools_menu->addAction(QStringLiteral("Sync Licenses to All Users..."), this, &main_window::sync_user_licenses);
    tools_menu->addSeparator();
    tools_menu->addAction(QStringLiteral("Export Log..."), this, &main_window::export_log);
    tools_menu->addAction(QStringLiteral("Open Log Folder..."), this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(log_dir())); });

    auto* adv_menu = tools_menu->addMenu(QStringLiteral("Advanced"));
    lv2_policy_act_ = adv_menu->addAction(QStringLiteral("lv2 1:1 placement policy (EXPERIMENTAL)"));
    lv2_policy_act_->setCheckable(true);
    lv2_policy_act_->setChecked(app_setting(settings_keys::lv2_policy, false));
    lv2_policy_act_->setToolTip(QStringLiteral("Place directories and blocks the way lv2's own ffs_dirpref/ffs_blkpref do, instead of our simpler allocator. Only affects new installs."));
    connect(lv2_policy_act_, &QAction::toggled, this, [this](bool on) {
        if (on && QMessageBox::warning(this, QStringLiteral("Experimental placement policy"), QStringLiteral("Install new games using lv2's own allocation policy?\n\n"
                                                      "This reimplements the placement the PS3 kernel itself uses. It has been hardware tested - files read back correctly and installed games launch - but it is NOT yet proven to lay a tree out identically to the console.\n\n"
                                                      "Use it on a drive you can restore. Run Check Consistency after every install."),
                                       QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            QSignalBlocker block(lv2_policy_act_);
            lv2_policy_act_->setChecked(false);
            return;
        }
        QSettings st(settings_keys::org(), settings_keys::app());
        st.setValue(settings_keys::lv2_policy, on);
        set_status(on ? QStringLiteral("lv2 1:1 placement policy enabled for new installs.")
                            : QStringLiteral("lv2 1:1 placement policy disabled; using the normal allocator."));
    });

    auto* help_menu = menuBar()->addMenu(QStringLiteral("&Help"));
    help_menu->addAction(QStringLiteral("About"), this, [this] {
        QMessageBox::about(
            this, QStringLiteral("About UFS2Xplorer"),
            QStringLiteral(
                "<b>UFS2Xplorer</b> %1<br>A cross-platform PS3 hard-drive explorer and package manager.<br>"
                "Built %2, Qt %3.<br><br>"
                "Icons: <b>Silk icon set 1.3</b> by Mark James "
                "(<a href=\"https://www.famfamfam.com/lab/icons/silk/\">famfamfam.com/lab/icons/silk</a>), "
                "licensed under <a href=\"https://creativecommons.org/licenses/by/2.5/\">"
                "Creative Commons Attribution 2.5 Generic (CC BY 2.5)</a>.<br><br>"
                "Not affiliated with Sony Interactive Entertainment.")
                .arg(QStringLiteral(UFS2XPLORER_VERSION), QStringLiteral(__DATE__), QStringLiteral(QT_VERSION_STR)));
    });
}

void main_window::build_disk_row(QVBoxLayout* root, QPushButton*& refresh, QPushButton*& open) {
    auto* row1 = new QHBoxLayout();
    device_combo_ = new QComboBox();
    device_combo_->setEditable(true);
    device_combo_->setMinimumWidth(320);
    refresh = new QPushButton(QStringLiteral("Refresh"));
    refresh->setIcon(QIcon(QStringLiteral(":/icons/arrow_refresh.bmp")));
    auto* add_console_btn = new QPushButton(QStringLiteral("Add Drive..."));
    add_console_btn->setIcon(QIcon(QStringLiteral(":/icons/drive_add.png")));
    add_console_btn->setToolTip(QStringLiteral("Guided setup: pick the disk, enter the EID key, IDPS and account id, and name the console."));
    open = new QPushButton(QStringLiteral("Open"));
    open->setIcon(QIcon(QStringLiteral(":/icons/drive_go.png")));
    eject_btn_ = new QPushButton(QStringLiteral("Eject"));
    eject_btn_->setIcon(QIcon(QStringLiteral(":/icons/disconnect.bmp")));
    eject_btn_->setToolTip(QStringLiteral("Safely stop the selected drive before pulling it from the caddy: flush + OS eject (USB), or spin it down (parks the heads)."));
    row1->addWidget(new QLabel(QStringLiteral("Disk:")));
    row1->addWidget(device_combo_, 1);
    row1->addWidget(refresh);
    row1->addWidget(add_console_btn);
    row1->addWidget(open);
    row1->addWidget(eject_btn_);
    root->addLayout(row1);
    connect(add_console_btn, &QPushButton::clicked, this, &main_window::add_console);
    connect(eject_btn_, &QPushButton::clicked, this, &main_window::eject_drive);
}

void main_window::build_action_row(QVBoxLayout* root) {
    // action row + progress
    auto* actions = new QHBoxLayout();
    install_btn_ = new QPushButton(QStringLiteral("Install PKG"));
    license_btn_ = new QPushButton(QStringLiteral("Install License"));
    fsck_btn_ = new QPushButton(QStringLiteral("Check Consistency"));
    repair_btn_ = new QPushButton(QStringLiteral("Repair Filesystem"));
    repair_btn_->setToolTip(QStringLiteral("Rebuild each cylinder group's free-space counts, cluster map and cluster summary from the allocation bitmaps, and mark any in-use fragment that was wrongly flagged free back as allocated. Fixes what an interrupted write leaves behind. Never frees a block a file still references."));
    reclaim_btn_ = new QPushButton(QStringLiteral("Reclaim Space"));
    reclaim_btn_->setToolTip(QStringLiteral("Free inodes that are marked in use but are unreachable from the root, and release their data blocks. These are left behind when a game is deleted on the console and the power is cut before it finishes freeing them. Blocks that any reachable file still points at are never touched."));
    games_btn_ = new QPushButton(QStringLiteral("Installed Games"));
    games_btn_->setToolTip(QStringLiteral("List the games under /dev_hdd0/game and uninstall them."));
    rebuilddb_btn_ = new QPushButton(QStringLiteral("Rebuild DB"));
    rebuilddb_btn_->setToolTip(QStringLiteral("Sets the mms/db.err rebuild flag so the console re-indexes the game list on next boot (non-destructive)."));
    backupdb_btn_ = new QPushButton(QStringLiteral("Backup DB"));
    backupdb_btn_->setToolTip(QStringLiteral("Copy the mms/db content-database files to a folder on your PC, for recovery."));
    restoredb_btn_ = new QPushButton(QStringLiteral("Restore DB"));
    restoredb_btn_->setToolTip(QStringLiteral("Write mms/db files back from a backup folder, replacing the current database (recovery)."));
    set_disk_actions_enabled(false);
    auto silk = [](const char* key) { return QIcon(QStringLiteral(":/icons/%1.bmp").arg(QLatin1String(key))); };
    auto png = [](const char* key) { return QIcon(QStringLiteral(":/icons/%1.png").arg(QLatin1String(key))); };
    install_btn_->setIcon(silk("package"));
    license_btn_->setIcon(png("key_add"));
    fsck_btn_->setIcon(png("drive_magnify"));
    repair_btn_->setIcon(png("drive_error"));
    reclaim_btn_->setIcon(png("drive_delete"));
    games_btn_->setIcon(png("joystick"));
    rebuilddb_btn_->setIcon(png("database_refresh"));
    backupdb_btn_->setIcon(png("database_save"));
    restoredb_btn_->setIcon(png("database_go"));
    actions->addWidget(install_btn_);
    actions->addWidget(license_btn_);
    actions->addWidget(fsck_btn_);
    actions->addWidget(repair_btn_);
    actions->addWidget(reclaim_btn_);
    actions->addWidget(games_btn_);
    actions->addWidget(rebuilddb_btn_);
    actions->addWidget(backupdb_btn_);
    actions->addWidget(restoredb_btn_);
    actions->addStretch(1);
    cancel_btn_ = new QPushButton(QStringLiteral("Cancel"));
    cancel_btn_->setIcon(silk("cross"));
    cancel_btn_->setVisible(false);
    actions->addWidget(cancel_btn_);
    progress_ = new QProgressBar();
    progress_->setVisible(false);
    progress_->setMinimumWidth(220);
    actions->addWidget(progress_);
    root->addLayout(actions);
}

void main_window::build_browser(QVBoxLayout* root) {
    auto* split = new QSplitter(Qt::Vertical);
    model_ = new ufs2_model(this);
    tree_ = new fs_tree_view();
    tree_->setModel(model_);
    {
        auto* st = new fast_tooltip_style();
        st->setParent(tree_);
        tree_->setStyle(st);
    }
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setSortingEnabled(true);
    tree_->setUniformRowHeights(true);
    tree_->setAllColumnsShowFocus(true);
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    tree_->setDragEnabled(true);
    tree_->setAcceptDrops(true);
    tree_->setDropIndicatorShown(true);
    tree_->setDragDropMode(QAbstractItemView::DragDrop);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree_->setDragDropOverwriteMode(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    auto* fm_panel = new QWidget();
    auto* fm_lay = new QVBoxLayout(fm_panel);
    fm_lay->setContentsMargins(0, 0, 0, 0);
    auto* addr_row = new QHBoxLayout();
    back_btn_ = new QPushButton();
    back_btn_->setIcon(QIcon(QStringLiteral(":/icons/arrow_left.bmp")));
    back_btn_->setToolTip(QStringLiteral("Back (Alt+Left)"));
    fwd_btn_ = new QPushButton();
    fwd_btn_->setIcon(QIcon(QStringLiteral(":/icons/arrow_right.bmp")));
    fwd_btn_->setToolTip(QStringLiteral("Forward (Alt+Right)"));
    auto* up_btn = new QPushButton();
    up_btn->setIcon(QIcon(QStringLiteral(":/icons/arrow_up.bmp")));
    up_btn->setToolTip(QStringLiteral("Up to parent folder (Backspace)"));
    path_edit_ = new QLineEdit(QStringLiteral("/dev_hdd0"));
    path_edit_->setPlaceholderText(QStringLiteral("/dev_hdd0/path - press Enter to go"));
    auto* go_btn = new QPushButton(QStringLiteral("Go"));
    go_btn->setIcon(QIcon(QStringLiteral(":/icons/arrow_right.bmp")));
    filter_edit_ = new QLineEdit();
    filter_edit_->setPlaceholderText(QStringLiteral("Filter…"));
    filter_edit_->setClearButtonEnabled(true);
    filter_edit_->setMaximumWidth(180);
    addr_row->addWidget(back_btn_);
    addr_row->addWidget(fwd_btn_);
    addr_row->addWidget(up_btn);
    addr_row->addWidget(new QLabel(QStringLiteral("Path:")));
    addr_row->addWidget(path_edit_, 1);
    addr_row->addWidget(go_btn);
    addr_row->addWidget(filter_edit_);
    fm_lay->addLayout(addr_row);
    connect(back_btn_, &QPushButton::clicked, this, &main_window::nav_back);
    connect(fwd_btn_, &QPushButton::clicked, this, &main_window::nav_forward);
    connect(up_btn, &QPushButton::clicked, this, &main_window::nav_up);
    connect(filter_edit_, &QLineEdit::textChanged, this, &main_window::apply_filter);

    auto* fm_split = new QSplitter(Qt::Horizontal);
    fm_split->addWidget(tree_);
    preview_ = new game_preview();
    preview_->hide();
    fm_split->addWidget(preview_);
    fm_split->setStretchFactor(0, 3);
    fm_split->setStretchFactor(1, 1);
    fm_lay->addWidget(fm_split, 1);
    split->addWidget(fm_panel);

    connect(path_edit_, &QLineEdit::returnPressed, this, [this] { navigate_to_path(path_edit_->text()); });
    connect(go_btn, &QPushButton::clicked, this, [this] { navigate_to_path(path_edit_->text()); });
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) {
                QStringList parts;
                for (QModelIndex i = cur; i.isValid(); i = i.parent()) {
                    const QString n = model_->name_of(i);
                    if (n != QStringLiteral("/")) parts.prepend(n);
                }
                path_edit_->setText(QStringLiteral("/") + parts.join(QChar('/')));
                update_preview(cur);
                if (!nav_navigating_ && cur.isValid()) {
                    while (nav_history_.size() > nav_pos_ + 1) nav_history_.removeLast();
                    nav_history_.push_back(QPersistentModelIndex(cur));
                    nav_pos_ = nav_history_.size() - 1;
                    if (back_btn_) back_btn_->setEnabled(nav_pos_ > 0);
                    if (fwd_btn_) fwd_btn_->setEnabled(false);
                }
            });
    connect(tree_, &QTreeView::activated, this, [this](const QModelIndex& idx) {
        if (!idx.isValid()) return;
        if (model_->is_dir(idx)) tree_->setExpanded(idx, !tree_->isExpanded(idx));
        else act_properties();
    });
    {
        auto sc = [this](QKeySequence k, void (main_window::*slot)()) {
            auto* a = new QAction(this);
            a->setShortcut(k);
            a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            connect(a, &QAction::triggered, this, slot);
            tree_->addAction(a);
        };
        sc(QKeySequence(Qt::Key_F5), &main_window::refresh_view);
        sc(QKeySequence(Qt::Key_Backspace), &main_window::nav_up);
        sc(QKeySequence(Qt::ALT | Qt::Key_Left), &main_window::nav_back);
        sc(QKeySequence(Qt::ALT | Qt::Key_Right), &main_window::nav_forward);
        sc(QKeySequence(Qt::ALT | Qt::Key_Return), &main_window::act_properties);
    }

    log_ = new QPlainTextEdit();
    open_log_file();
    log_session_header();
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(5000);
    split->addWidget(log_);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    root->addWidget(split, 1);
}

void main_window::wire_signals(QPushButton* refresh, QPushButton* open) {
    connect(refresh, &QPushButton::clicked, this, &main_window::refresh_devices);
    connect(open, &QPushButton::clicked, this, &main_window::open_device);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeView::customContextMenuRequested, this, &main_window::show_tree_menu);
    auto add_shortcut = [this](QKeySequence keys, void (main_window::*slot)()) {
        auto* a = new QAction(this);
        a->setShortcut(keys);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(a, &QAction::triggered, this, slot);
        tree_->addAction(a);
    };
    add_shortcut(QKeySequence::Copy, &main_window::act_copy);
    add_shortcut(QKeySequence::Cut, &main_window::act_cut);
    add_shortcut(QKeySequence::Paste, &main_window::act_paste);
    add_shortcut(QKeySequence::Delete, &main_window::act_delete);

    connect(model_, &ufs2_model::wrote, this, [this](const QString& msg) {
        log(msg);
        remount();
    }, Qt::QueuedConnection);

    connect(model_, &ufs2_model::external_drop, this, &main_window::on_external_drop, Qt::QueuedConnection);
    connect(install_btn_, &QPushButton::clicked, this, &main_window::install_pkg);
    connect(license_btn_, &QPushButton::clicked, this, &main_window::install_license);
    connect(fsck_btn_, &QPushButton::clicked, this, &main_window::check_consistency);
    connect(repair_btn_, &QPushButton::clicked, this, &main_window::repair_counts);
    connect(reclaim_btn_, &QPushButton::clicked, this, &main_window::reclaim_orphans);
    connect(games_btn_, &QPushButton::clicked, this, &main_window::show_games);
    connect(rebuilddb_btn_, &QPushButton::clicked, this, &main_window::rebuild_database);
    connect(backupdb_btn_, &QPushButton::clicked, this, &main_window::backup_database);
    connect(restoredb_btn_, &QPushButton::clicked, this, &main_window::restore_database);
    connect(cancel_btn_, &QPushButton::clicked, this, &main_window::cancel_job);

    connect(device_combo_, &QComboBox::activated, this, [this](int) { // quick load
        if (!current_eid_hex().isEmpty()) open_device();
    });
}

void main_window::open_log_file() {
    QDir().mkpath(log_dir());
    QDir d(log_dir());
    QStringList old = d.entryList(QStringList{QStringLiteral("ufs2xplorer-*.log")}, QDir::Files, QDir::Name);
    while (old.size() >= 10) d.remove(old.takeFirst());
    log_file_.setFileName(log_dir() + QStringLiteral("/ufs2xplorer-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) + QStringLiteral(".log"));
    log_file_.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append);
}

void main_window::log_session_header() {
    log(QStringLiteral("UFS2Xplorer %1  (built %2, Qt %3)").arg(QStringLiteral(UFS2XPLORER_VERSION), QStringLiteral(__DATE__), QStringLiteral(QT_VERSION_STR)));
    log(QStringLiteral("%1  |  %2").arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture()));
    log(QStringLiteral("lv2 1:1 placement policy: %1").arg(app_setting(settings_keys::lv2_policy, false) ? QStringLiteral("ON (experimental)") : QStringLiteral("off")));
    if (log_file_.isOpen()) log(QStringLiteral("log file: %1").arg(log_file_.fileName()));
    else log(QStringLiteral("NOTE: could not open a log file in %1; only this window has the log.").arg(log_dir()));
}

void main_window::log(const QString& line) {
    if (log_) log_->appendPlainText(line);
    if (log_file_.isOpen()) {
        log_file_.write(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")).toUtf8() + "  " + line.toUtf8() + "\n");
        log_file_.flush();
    }
}

void main_window::set_status(const QString& line) {
    if (status_) status_->setText(line);
    log(line);
}

bool main_window::ensure_broker() {
    if (broker_ready_ && broker_.connected()) return true;
    broker_ready_ = false;

    if (broker_port_ == 0) {
        broker_port_ = static_cast<quint16>(QRandomGenerator::system()->bounded(49152, 65535));
        broker_token_.resize(32);
        for (int i = 0; i < broker_token_.size(); ++i)
            broker_token_[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
    }

    const QString helper = QCoreApplication::applicationDirPath() +
#ifdef _WIN32
        QStringLiteral("/ps3hdd_helper.exe");
#else
        QStringLiteral("/ps3hdd_helper");
#endif
    if (!QFileInfo::exists(helper)) {
        set_status(QStringLiteral("Disk helper not found next to the app: %1").arg(helper));
        return false;
    }

    log(QStringLiteral("Starting the elevated disk helper..."));
    if (!ps3hdd::ipc::launch_helper(helper, broker_port_, broker_token_)) {
        set_status(QStringLiteral("Elevation was declined; disk access is unavailable."));
        broker_port_ = 0; // regenerate + reprompt next time
        return false;
    }

    //wait a sec
    QString last_error;
    for (int attempt = 0; attempt < 12; ++attempt) {
        try {
            broker_.connect_to(broker_port_, broker_token_, 800);
            broker_ready_ = true;
            log(QStringLiteral("Disk helper connected on port %1.").arg(broker_port_));
            return true;
        } catch (const std::exception& ex) {
            last_error = QString::fromUtf8(ex.what());
            QThread::msleep(100);
        }
    }
    log(QStringLiteral("Could not connect to the disk helper on port %1: %2").arg(broker_port_).arg(last_error));
    set_status(QStringLiteral("Could not connect to the disk helper."));
    broker_port_ = 0;
    return false;
}

void main_window::refresh_devices() {
    device_combo_->clear();
    if (!ensure_broker()) return;
    try {
        int known = -1;
        for (const auto& d : broker_.enumerate()) {
            const key_entry prof = keys_.entry_for_serial(d.serial);
            QString tag;
            if (!prof.hex_key.isEmpty())
                tag = prof.nickname.isEmpty() ? QStringLiteral("  - saved PS3") : QStringLiteral("  - %1 [PS3]").arg(prof.nickname);
            else if (d.raw)
                tag = QStringLiteral("  - unformatted (maybe PS3)");
            else
                tag = QStringLiteral("  - has partitions");
            device_combo_->addItem(
                QStringLiteral("%1  (%2)%3").arg(d.path, QString::fromStdString(disk::format_size(d.size)), tag),
                d.path);
            device_combo_->setItemData(device_combo_->count() - 1, d.serial, Qt::UserRole + 1);
            if (!prof.hex_key.isEmpty() && known < 0) known = device_combo_->count() - 1;
        }
        if (known >= 0) device_combo_->setCurrentIndex(known); // jump 2 a known piastri
    } catch (const std::exception& ex) {
        log(QStringLiteral("Device enumeration failed: %1").arg(QString::fromUtf8(ex.what())));
    }
    if (device_combo_->count() == 0)
        log(QStringLiteral("No physical disks detected."));
}

void main_window::add_console() {
    if (!ensure_broker()) return;
    QVector<console_wizard::device> devs;
    try {
        for (const auto& d : broker_.enumerate())
            devs.push_back({d.path, d.serial, d.size, d.raw});
    } catch (const std::exception& ex) {
        log(QStringLiteral("Device enumeration failed: %1").arg(QString::fromUtf8(ex.what())));
        return;
    }
    if (devs.isEmpty()) { set_status(QStringLiteral("No disks detected.")); return; }

    auto test = [this](const QString& path, const QString& eid_hex) -> QString {
        const auto eid = parse_hex(eid_hex);
        if (eid.size() != 48) return QStringLiteral("EID must be 48 bytes.");
        if (!ensure_broker()) return QStringLiteral("disk helper unavailable");
        try {
            ps3hdd::ipc::broker_client probe;
            probe.connect_to(broker_port_, broker_token_, 8000);
            const auto info = probe.open(path, /*writable=*/false);
            auto src = std::make_shared<ps3hdd::ipc::ipc_disk_source>(probe, info);
            auto m = app::open_gameos(src, {eid.data(), eid.size()});
            if (!m) return QStringLiteral("No GameOS partition found (wrong disk or key).");
            fs::ufs2_filesystem ufs(*m->decrypted, m->partition_sector);
            if (!ufs.mount()) return QStringLiteral("GameOS found but the filesystem did not mount.");
            return QString();
        } catch (const std::exception& ex) {
            return QString::fromUtf8(ex.what());
        }
    };

    console_wizard wiz(devs, test, this);
    if (wiz.exec() != QDialog::Accepted) return;

    key_entry e;
    e.nickname = wiz.nickname();
    e.hex_key = wiz.eid_hex();
    e.serial = wiz.device_serial();
    e.idps = wiz.idps_hex();
    e.account_id = wiz.account_id();
    e.date_added = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    keys_.add_or_update(e);
    log(QStringLiteral("Saved console '%1'.").arg(e.nickname));

    refresh_devices();
    for (int i = 0; i < device_combo_->count(); ++i)
        if (device_combo_->itemData(i).toString() == wiz.device_path()) {
            device_combo_->setCurrentIndex(i);
            break;
        }
}

void main_window::manage_drives() {
    drives_dialog dlg(keys_, this);
    dlg.exec();
    if (dlg.changed()) refresh_devices(); 
}

void main_window::open_device() {
    model_->set_backend(nullptr, nullptr);
    writer_.reset();
    fs_.reset();
    mount_.reset();
    raw_.reset();

    const QString path = current_device_path();
    const auto eid = parse_hex(current_eid_hex());
    if (eid.size() != 48) {
        set_status(QStringLiteral("No saved key for this drive. Use 'Add Drive...' (or Tools > Manage Drives) to set up its EID key first."));
        return;
    }

    try {
        if (!ensure_broker()) return;
        const auto info = broker_.open(path, /*writable=*/true);
        const bool writable = info.can_write;
        raw_ = std::make_shared<ps3hdd::ipc::ipc_disk_source>(broker_, info);
        log(QStringLiteral("Opened %1 (%2)").arg(QString::fromStdString(raw_->description()),
            writable ? QStringLiteral("read-write") : QStringLiteral("read-only")));
        if (!writable)
            log(QStringLiteral("Read-only: writes are disabled (the disk may be locked by another process)."));

        mount_ = app::open_gameos(raw_, {eid.data(), eid.size()});
        if (!mount_) {
            set_status(QStringLiteral("Could not locate/mount a GameOS partition on this disk."));
            return;
        }
        fs_ = std::make_unique<fs::ufs2_filesystem>(*mount_->decrypted, mount_->partition_sector);
        if (!fs_->mount()) {
            set_status(QStringLiteral("GameOS superblock found but mount failed."));
            return;
        }
        const auto& sb = fs_->sb();
        mount_desc_ = QStringLiteral("GameOS @ sector 0x%1  |  %2 (bswap16 %3)  |  %4 CGs").arg(mount_->partition_sector, 0, 16).arg(QString::fromStdString(mount_->cipher)).arg(mount_->bswap16 ? QStringLiteral("on") : QStringLiteral("off")).arg(sb.cylinder_groups);
        if (writable) writer_ = std::make_unique<fs::ufs2_writer>(*fs_, *mount_->decrypted);
        model_->set_backend(fs_.get(), writer_.get());
        tree_->expand(model_->root_index());
        set_disk_actions_enabled(true);
        update_capacity();
        log(QStringLiteral("mounted %1").arg(mount_desc_));
        log(QStringLiteral("geometry: bsize=%1 fsize=%2 ipg=%3 fpg=%4 ncg=%5  |  free %6").arg(sb.block_size).arg(sb.fragment_size).arg(sb.inodes_per_group).arg(sb.frags_per_group).arg(sb.cylinder_groups).arg(QString::fromStdString(disk::format_size(static_cast<std::uint64_t>(sb.free_space_bytes() > 0 ? sb.free_space_bytes() : 0)))));
        if (sb.min_free_percent != 8 || sb.optim != 0)
            log(QStringLiteral("NOTE: minfree=%1%% optim=%2 - this disk was patched by the 'unlock HDD space' homebrew (stock is minfree=8%% optim=0).").arg(sb.min_free_percent).arg(sb.optim));
    } catch (const std::exception& ex) {
        set_status(QStringLiteral("Error: %1").arg(QString::fromUtf8(ex.what())));
    }
}

void main_window::eject_drive() {
    if (job_thread_) return; // never mid-job
    const QString path = current_device_path();
    if (path.isEmpty()) { set_status(QStringLiteral("Select a drive to eject first.")); return; }
    if (!ensure_broker()) { set_status(QStringLiteral("No disk-helper connection to eject through.")); return; }

    if (QMessageBox::question(
            this, QStringLiteral("Eject drive"),
            QStringLiteral("Safely stop %1 so it can be pulled from the caddy?\n\nThis flushes and ejects or parks the heads.").arg(path),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;

    model_->set_backend(nullptr, nullptr);
    writer_.reset();
    fs_.reset();
    mount_.reset();
    raw_.reset();
    art_cache_.clear();
    mount_desc_.clear();

    try {
        const auto r = broker_.eject(path);
        log(r.message);
        set_status(r.message);
        update_capacity();
        set_disk_actions_enabled(false);
        QMessageBox::information(this, QStringLiteral("Eject drive"), r.removed ? QStringLiteral("Ejected. It is safe to remove the drive now.") : QStringLiteral("%1\n\nThe drive is parked, you can now remove it.").arg(r.message));
    } catch (const std::exception& ex) {
        const QString msg = QString::fromUtf8(ex.what());
        log(QStringLiteral("eject failed: %1").arg(msg));
        QMessageBox::warning(this, QStringLiteral("Eject drive"), QStringLiteral("Could not eject or spin down the drive:\n%1").arg(msg));
    }
}

bool main_window::base_job(job& j) {
    const QString path = current_device_path();
    const auto eid = parse_hex(current_eid_hex());
    if (path.isEmpty() || eid.size() != 48) {
        set_status(QStringLiteral("This drive has no saved key. Use 'Add Drive...' (or Tools > Manage Drives) to set it up first."));
        return false;
    }
    if (!ensure_broker()) return false;
    j.device = path;
    j.eid = eid;
    j.broker_port = broker_port_;
    j.broker_token = broker_token_;
    return true;
}

QString main_window::current_serial() const {
    return device_combo_->currentData(Qt::UserRole + 1).toString();
}

QString main_window::current_eid_hex() const {
    return keys_.key_for_serial(current_serial());
}

void main_window::navigate_to_path(const QString& path) {
    if (!model_) return;
    QModelIndex cur = model_->root_index(); // the dev_hdd0 mount node
    if (!cur.isValid()) return;
    tree_->expand(cur);
    QStringList parts = path.split(QChar('/'), Qt::SkipEmptyParts);
    if (!parts.isEmpty() && parts.first().compare(model_->name_of(cur), Qt::CaseInsensitive) == 0)
        parts.removeFirst(); // a leading "dev_hdd0" is the root itself
    for (const QString& part : parts) {
        if (model_->canFetchMore(cur)) model_->fetchMore(cur);
        QModelIndex found;
        for (int r = 0; r < model_->rowCount(cur); ++r) {
            const QModelIndex ch = model_->index(r, 0, cur);
            if (model_->name_of(ch).compare(part, Qt::CaseInsensitive) == 0) { found = ch; break; }
        }
        if (!found.isValid()) { log(QStringLiteral("Path not found: %1").arg(part)); break; }
        cur = found;
        tree_->expand(cur);
    }
    tree_->setCurrentIndex(cur);
    tree_->scrollTo(cur);
}

void main_window::refresh_view() {
    if (!fs_) return;
    remount();
    if (model_) {
        model_->refresh(model_->root_index());
        tree_->expand(model_->root_index());
    }
    if (filter_edit_ && !filter_edit_->text().isEmpty()) apply_filter(filter_edit_->text());
    log(QStringLiteral("Refreshed."));
}

void main_window::nav_up() {
    const QModelIndex cur = tree_->currentIndex();
    if (!cur.isValid()) return;
    const QModelIndex parent = cur.parent();
    if (parent.isValid()) { tree_->setCurrentIndex(parent); tree_->scrollTo(parent); }
}

void main_window::nav_back() {
    if (nav_pos_ <= 0) return;
    nav_navigating_ = true;
    while (nav_pos_ > 0) {
        --nav_pos_;
        if (nav_history_[nav_pos_].isValid()) break; // skip entries whose item is gone
    }
    const QModelIndex idx = nav_history_[nav_pos_];
    if (idx.isValid()) { tree_->setCurrentIndex(idx); tree_->scrollTo(idx); }
    nav_navigating_ = false;
    back_btn_->setEnabled(nav_pos_ > 0);
    fwd_btn_->setEnabled(nav_pos_ < nav_history_.size() - 1);
}

void main_window::nav_forward() {
    if (nav_pos_ >= nav_history_.size() - 1) return;
    nav_navigating_ = true;
    ++nav_pos_;
    const QModelIndex idx = nav_history_[nav_pos_];
    if (idx.isValid()) { tree_->setCurrentIndex(idx); tree_->scrollTo(idx); }
    nav_navigating_ = false;
    back_btn_->setEnabled(nav_pos_ > 0);
    fwd_btn_->setEnabled(nav_pos_ < nav_history_.size() - 1);
}

void main_window::act_properties() {
    const QModelIndex idx = tree_->currentIndex();
    if (!idx.isValid() || !fs_) return;
    const std::uint64_t inode = model_->inode_of(idx);
    const bool is_root = model_->is_root(idx);
    const QString name = is_root ? QStringLiteral("dev_hdd0") : model_->name_of(idx);
    const bool is_dir = model_->is_dir(idx);

    QStringList parts;
    for (QModelIndex i = idx; i.isValid(); i = i.parent()) {
        const QString n = model_->name_of(i);
        if (n != QStringLiteral("/")) parts.prepend(n);
    }
    QString info = QStringLiteral("Name:\t%1\nType:\t%2\nPath:\t/%3\nInode:\t%4\n").arg(name, is_dir ? QStringLiteral("Folder") : QStringLiteral("File"), parts.join(QChar('/'))).arg(inode);
    try {
        if (is_dir) {
            int n = 0;
            for (const auto& e : fs_->read_directory(fs_->read_inode(inode)))
                if (e.name != "." && e.name != "..") ++n;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const std::uint64_t sz = dir_size(*fs_, inode);
            QApplication::restoreOverrideCursor();
            info += QStringLiteral("Items:\t%1\nSize:\t%2 (total)").arg(n).arg(QString::fromStdString(disk::format_size(sz)));
        } else {
            const auto in = fs_->read_inode(inode);
            info += QStringLiteral("Size:\t%1").arg(QString::fromStdString(disk::format_size(in.size)));
        }
    } catch (...) {
    }
    QMessageBox::information(this, QStringLiteral("Properties - %1").arg(name), info);
}

void main_window::export_log() {
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save log"), QStringLiteral("ps3hddtool_log.txt"), QStringLiteral("Text files (*.txt);;All files (*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        f.write(log_->toPlainText().toUtf8());
        log(QStringLiteral("Log saved to %1").arg(path));
    } else {
        log(QStringLiteral("Could not write %1").arg(path));
    }
}

void main_window::apply_filter(const QString& text) {
    if (!model_) return;
    std::function<bool(const QModelIndex&)> walk = [&](const QModelIndex& parent) -> bool {
        bool any = false;
        for (int r = 0; r < model_->rowCount(parent); ++r) {
            const QModelIndex idx = model_->index(r, 0, parent);
            const bool self_match = text.isEmpty() || model_->name_of(idx).contains(text, Qt::CaseInsensitive);
            const bool child_match = walk(idx);
            const bool vis = self_match || child_match;
            tree_->setRowHidden(r, parent, !vis);
            if (child_match && !text.isEmpty()) tree_->expand(idx);
            any = any || vis;
        }
        return any;
    };
    walk(QModelIndex());
}

void main_window::closeEvent(QCloseEvent* e) {
    QSettings s(settings_keys::org(), settings_keys::app());
    s.setValue(QStringLiteral("win/geometry"), saveGeometry());
    if (tree_ && tree_->header())
        s.setValue(QStringLiteral("win/treeHeader"), tree_->header()->saveState());
    QMainWindow::closeEvent(e);
}

void main_window::set_disk_actions_enabled(bool on) {
    for (auto* b : {install_btn_, license_btn_, fsck_btn_, repair_btn_, reclaim_btn_, games_btn_, rebuilddb_btn_, backupdb_btn_, restoredb_btn_})
        if (b) b->setEnabled(on);
}

QString main_window::current_device_path() const {
    return device_combo_->currentData().isValid() ? device_combo_->currentData().toString() : device_combo_->currentText();
}

void main_window::set_speed(double bps) {
    if (!speed_label_) return;
    speed_label_->setText(bps > 0 ? QStringLiteral("%1/s").arg(QString::fromStdString(disk::format_size(static_cast<std::uint64_t>(bps)))) : QString());
}

void main_window::set_busy(bool busy) {
    set_disk_actions_enabled(!busy);
    if (eject_btn_) eject_btn_->setEnabled(!busy);
    tree_->setEnabled(!busy);
    cancel_btn_->setVisible(busy);
    cancel_btn_->setEnabled(busy);
    progress_->setVisible(busy);
    if (busy) progress_->setRange(0, 0);
    if (!busy && speed_label_) speed_label_->clear();
}

void main_window::start_job(job j, const QString& title) {
    if (job_thread_) return;
    log(QStringLiteral("== %1 ==").arg(title));
    set_busy(true);

    const bool is_fileop = j.file_operation != job::fop_none;
    const bool is_pkg_install = j.type == job::install_pkg;
    if (is_pkg_install) j.lv2_policy = lv2_policy_act_ && lv2_policy_act_->isChecked();
    const bool was_write = j.type != job::consistency && j.type != job::verify_pkg;
    if (was_write && !is_fileop) { writer_.reset(); fs_.reset(); mount_.reset(); raw_.reset(); }

    job_thread_ = new QThread(this);
    auto* w = new worker(std::move(j));
    job_worker_ = w;
    w->moveToThread(job_thread_);
    connect(job_thread_, &QThread::started, w, &worker::run);
    connect(w, &worker::progress, this, [this](const QString& line, int percent) {
        if (percent >= 0) { progress_->setRange(0, 100); progress_->setValue(percent); }
        else progress_->setRange(0, 0);
        if (!line.isEmpty()) log(line);
    });
    connect(w, &worker::speed, this, &main_window::set_speed);
    connect(w, &worker::finished, this,
            [this, w, was_write, is_fileop, is_pkg_install](bool ok, const QString& summary) {
        log(summary);
        set_status(summary);
        job_thread_->quit();
        job_thread_->wait();
        w->deleteLater();
        job_thread_->deleteLater();
        job_thread_ = nullptr;
        job_worker_ = nullptr;
        set_busy(false);
        if (is_fileop) {
            remount();
            for (const QPersistentModelIndex& idx : pending_refresh_)
                if (idx.isValid()) model_->refresh(idx);
            pending_refresh_.clear();
        } else if (was_write && !device_combo_->currentText().isEmpty()) {
            open_device();
        }
        if (is_pkg_install && !pending_rap_.isEmpty()) {
            const QString rap = pending_rap_;
            pending_rap_.clear();
            if (ok) do_license(rap);
        }
    });
    job_thread_->start();
}

void main_window::install_pkg() {
    if (device_combo_->currentText().isEmpty()) {
        set_status(QStringLiteral("Select a drive first (with a saved key)."));
        return;
    }
    const QStringList pkgs = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Select PKG(s)"), QString(),
        QStringLiteral("PKG files (*.pkg);;All files (*)"));
    if (pkgs.isEmpty()) return;
    start_pkg_batch(pkgs);
}

void main_window::start_pkg_batch(const QStringList& pkgs, const QStringList& raps) {
    if (pkgs.isEmpty()) return;
    job base;
    if (!base_job(base)) return;
    const QString db_backup = backup_db_to_temp();

    install_dialog dlg(pkgs, base.device, base.eid, base.broker_port, base.broker_token, this);
    connect(&dlg, &install_dialog::speed, this, &main_window::set_speed);
    dlg.exec();
    set_speed(0.0);
    remount();
    if (model_) {
        model_->refresh(model_->root_index());
        tree_->expand(model_->root_index());
    }

    if (dlg.wrote() && !dlg.installed_ok() && !db_backup.isEmpty()) {
        if (QMessageBox::warning(
                this, QStringLiteral("Install did not complete cleanly"),
                QStringLiteral("The batch install stopped before the disk verified clean.\n\nRoll the content database (mms/db) back to the automatic backup taken just before install? Game files already written stay in place; only the XMB database is reverted.\n\nBackup folder:\n%1").arg(db_backup),
                QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
            job j;
            if (base_job(j)) {
                j.type = job::restore_db;
                j.host_dir = db_backup;
                start_job(std::move(j), QStringLiteral("Revert DB"));
            }
        }
        return;
    }
    if (dlg.installed_ok() && !raps.isEmpty()) {
        if (raps.size() == 1) do_license(raps.first());
        else install_licenses_batch(raps);
    }
}

QString main_window::backup_db_to_temp() {
    if (!fs_) return {};
    const auto dbdir = fs_->resolve_path_to_inode_number("mms/db");
    if (!dbdir) return {};
    const QString sub = QDir::tempPath() + QStringLiteral("/ps3hddtool/db_backup_") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    QDir().mkpath(sub);
    int n = 0;
    try {
        for (const auto& e : fs_->read_directory(fs_->read_inode(*dbdir))) {
            if (e.name == "." || e.name == ".." || e.type == fs::dirent_type::directory) continue;
            const auto data = fs_->read_inode_data(fs_->read_inode(e.inode_number));
            QFile f(sub + QStringLiteral("/") + QString::fromStdString(e.name));
            if (f.open(QIODevice::WriteOnly)) {
                f.write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size()));
                ++n;
            }
        }
    } catch (const std::exception& ex) {
        log(QStringLiteral("preinstall DB backup skipped: %1").arg(QString::fromUtf8(ex.what())));
        return {};
    }
    if (n == 0) return {};
    log(QStringLiteral("Backed up %1 mms/db file(s) before install to %2").arg(n).arg(sub));
    return sub;
}

void main_window::on_external_drop(const QStringList& paths, quint64 dir_inode) {
    if (paths.isEmpty() || !require_writer()) return;

    QStringList pkgs, raps, others;
    for (const QString& p : paths) {
        if (p.endsWith(QStringLiteral(".pkg"), Qt::CaseInsensitive)) pkgs << p;
        else if (p.endsWith(QStringLiteral(".rap"), Qt::CaseInsensitive)) raps << p;
        else others << p;
    }

    if (!pkgs.isEmpty()) {
        if (!others.isEmpty())
            log(QStringLiteral("Ignoring %1 non-package file(s) in this drop.").arg(others.size()));
        start_pkg_batch(pkgs, raps);
        return;
    }

    if (!raps.isEmpty() && others.isEmpty()) {
        const auto choice = QMessageBox::question(
            this, QStringLiteral("Install licenses?"),
            QStringLiteral("Install %1 license(s) (.rap) into home/00000001/exdata?\n\nChoose No to import them as raw files instead.").arg(raps.size()),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Yes);
        if (choice == QMessageBox::Cancel) return;
        if (choice == QMessageBox::Yes) { install_licenses_batch(raps); return; }
    }

    job j;
    if (!base_job(j)) return;
    j.file_operation = job::fop_import;
    j.fop_dest = dir_inode;
    j.fop_import_paths = paths;
    pending_refresh_.clear();
    const QModelIndex di = model_->index_for_inode(dir_inode);
    if (di.isValid()) pending_refresh_.push_back(QPersistentModelIndex(di));
    start_job(std::move(j), QStringLiteral("Import"));
}

void main_window::install_license() { do_license(QString()); }

void main_window::sync_user_licenses() {
    job j;
    if (!base_job(j)) return;
    if (QMessageBox::question(
            this, QStringLiteral("Sync licenses to all users"),
            QStringLiteral("Copy one local user's licenses (act.dat + every .rif) into every other user's exdata, so all users can play the same games and DLC?\n\nLocal users share the account id the licenses are bound to, so this is safe."),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    j.type = job::sync_exdata;
    start_job(std::move(j), QStringLiteral("Sync licenses to all users"));
}

void main_window::do_license(const QString& rap_prefill) {
    job j;
    if (!base_job(j)) return;
    license_dialog dlg(this);
    if (!rap_prefill.isEmpty()) dlg.set_rap(rap_prefill);

    const QString serial = current_serial();
    const key_entry prof = keys_.entry_for_serial(serial);
    if (!prof.idps.isEmpty()) dlg.set_idps(prof.idps);
    if (!prof.account_id.isEmpty()) dlg.set_account(prof.account_id);

    if (fs_ && !fs_->resolve_path_to_inode_number("home/00000001/exdata/act.dat")) {
        dlg.prefer_full_activation();
        log(QStringLiteral("No act.dat on this console yet, defaulting to Full Activation (enter the account ID you created)."));
    }

    if (dlg.exec() != QDialog::Accepted) return;
    if (!dlg.fill(j)) {
        set_status(QStringLiteral("License needs a RAP file and a 16-byte IDPS."));
        return;
    }

    if (dlg.remember() && !serial.isEmpty() && !dlg.idps_text().trimmed().isEmpty()) {
        key_entry e = prof;
        if (e.hex_key.isEmpty()) e.hex_key = current_eid_hex();
        e.serial = serial;
        e.idps = dlg.idps_text().trimmed();
        e.account_id = dlg.account_text().trimmed();
        if (e.date_added.isEmpty())
            e.date_added = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (!e.hex_key.isEmpty()) keys_.add_or_update(e);
    }

    start_job(std::move(j), QStringLiteral("Install License"));
}

void main_window::install_licenses_batch(const QStringList& raps) {
    if (raps.isEmpty()) return;
    job j;
    if (!base_job(j)) return;

    const key_entry prof = keys_.entry_for_serial(current_serial());
    QString idps_hex = prof.idps.trimmed();
    if (idps_hex.isEmpty()) {
        bool okd = false;
        idps_hex = QInputDialog::getText(
            this, QStringLiteral("Console IDPS"),
            QStringLiteral("Enter this console's IDPS (16 bytes hex) to sign the %1 license(s):").arg(raps.size()),
            QLineEdit::Normal, QString(), &okd);
        if (!okd) return;
    }
    std::vector<std::byte> idps;
    {
        std::string h;
        for (QChar c : idps_hex)
            if (std::isxdigit(static_cast<unsigned char>(c.toLatin1()))) h.push_back(c.toLatin1());
        for (std::size_t i = 0; i + 1 < h.size(); i += 2)
            idps.push_back(static_cast<std::byte>(std::stoi(h.substr(i, 2), nullptr, 16)));
    }
    if (idps.size() != 16) {
        QMessageBox::warning(this, QStringLiteral("Install Licenses"), QStringLiteral("The IDPS must be exactly 16 bytes (32 hex characters)."));
        return;
    }

    j.type = job::license_batch;
    j.idps = std::move(idps);
    j.rap_paths = raps;
    start_job(std::move(j), QStringLiteral("Install %1 license(s)").arg(raps.size()));
}

void main_window::check_consistency() {
    job j;
    if (!base_job(j)) return;
    j.type = job::consistency;
    start_job(std::move(j), QStringLiteral("Check Consistency"));
}

void main_window::reclaim_orphans() {
    job j;
    if (!base_job(j)) return;
    if (QMessageBox::warning(this, QStringLiteral("Reclaim leaked space"), QStringLiteral("Free every inode that is marked in use but cannot be reached from the root directory, and release its data blocks?\n\n"
                                            "These are normally left behind when a game is deleted on the console and it is powered off before the console finishes freeing them.\n\n"
                                            "A live-block map is built first, so a block that any reachable file still points at is never freed. Anything the tree walk cannot reach is destroyed, though - run Check Consistency first and read the findings."),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    j.type = job::reclaim_orphans;
    start_job(std::move(j), QStringLiteral("Reclaim Space"));
}

void main_window::repair_counts() {
    job j;
    if (!base_job(j)) return;
    if (QMessageBox::question(this, QStringLiteral("Repair free-space counts"), QStringLiteral("Recompute every cylinder group's free-space counts from the on-disk bitmaps and write back any that disagree?\n\nThis fixes \"summary-mismatches\" left by an interrupted write. It reads all cylinder groups, so it can take a minute."), QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    j.type = job::repair_counts;
    start_job(std::move(j), QStringLiteral("Repair Free Counts"));
}

std::vector<main_window::game_row> main_window::list_installed_games() {
    std::vector<game_row> out;
    if (!fs_) return out;

    try {
        if (auto gdir = fs_->resolve_path_to_inode_number("game")) {
            for (const auto& e : fs_->read_directory(fs_->read_inode(*gdir))) {
                if (e.name == "." || e.name == ".." || e.type != fs::dirent_type::directory) continue;
                game_row r;
                r.title_id = QString::fromStdString(e.name);
                r.game_inode = e.inode_number;
                try {
                    const auto sfo = read_child(*fs_, e.inode_number, "PARAM.SFO");
                    if (!sfo.empty()) {
                        const auto kv = parse_sfo(sfo);
                        r.title = kv.value(QStringLiteral("TITLE"));
                        r.category = kv.value(QStringLiteral("CATEGORY"));
                    }
                    const auto icon = read_child(*fs_, e.inode_number, "ICON0.PNG");
                    if (!icon.empty())
                        r.icon = QByteArray(reinterpret_cast<const char*>(icon.data()), static_cast<qsizetype>(icon.size()));
                } catch (...) {
                }
                out.push_back(std::move(r));
            }
        }
    } catch (...) {
    }

    try {
        for (const std::string& user : local_users(*fs_)) {
            auto sd = fs_->resolve_path_to_inode_number("home/" + user + "/savedata");
            if (!sd) continue;
            for (const auto& e : fs_->read_directory(fs_->read_inode(*sd))) {
                if (e.name == "." || e.name == ".." || e.type != fs::dirent_type::directory) continue;
                const QString sname = QString::fromStdString(e.name);
                for (game_row& g : out) {
                    if (!g.title_id.isEmpty() && sname.startsWith(g.title_id)) {
                        g.save_inodes.push_back(e.inode_number); // sized lazily
                        break;
                    }
                }
            }
        }
    } catch (...) {
    }
    return out;
}

void main_window::show_games() {
    if (!fs_) return;
    auto games = list_installed_games();

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Installed Games"));
    dlg.resize(700, 440);
    auto* lay = new QVBoxLayout(&dlg);

    auto* table = new QTableWidget(static_cast<int>(games.size()), 5, &dlg);
    table->setHorizontalHeaderLabels({QStringLiteral("Title"), QStringLiteral("Title ID"), QStringLiteral("Category"), QStringLiteral("Game Data"), QStringLiteral("Save Data")});
    table->horizontalHeaderItem(3)->setToolTip(QStringLiteral("Base game + updates + DLC (everything in /dev_hdd0/game/<id>)"));
    table->horizontalHeaderItem(4)->setToolTip(QStringLiteral("Save data across all users (/dev_hdd0/home/<user>/savedata/<id>*)"));
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);
    table->setIconSize(QSize(120, 68)); //ICON0
    for (int i = 0; i < static_cast<int>(games.size()); ++i) {
        auto* titleItem = new QTableWidgetItem(games[i].title.isEmpty() ? QStringLiteral("(no PARAM.SFO title)") : games[i].title);
        if (!games[i].icon.isEmpty()) {
            QPixmap pm;
            if (pm.loadFromData(games[i].icon)) titleItem->setIcon(QIcon(pm));
        }
        table->setItem(i, 0, titleItem);
        table->setItem(i, 1, new QTableWidgetItem(games[i].title_id));
        auto* catItem = new QTableWidgetItem(category_name(games[i].category));
        catItem->setToolTip(games[i].category); // the raw SFO
        table->setItem(i, 2, catItem);
        table->setItem(i, 3, new QTableWidgetItem(QStringLiteral("…")));
        table->setItem(i, 4, new QTableWidgetItem(QStringLiteral("…")));
    }
    table->resizeColumnsToContents();
    table->resizeRowsToContents();
    table->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(new QLabel(QStringLiteral("%1 game(s) under /dev_hdd0/game").arg(games.size()), &dlg));
    lay->addWidget(table);

    auto* row = new QHBoxLayout;
    auto* uninstall = new QPushButton(QStringLiteral("Uninstall"), &dlg);
    auto* close = new QPushButton(QStringLiteral("Close"), &dlg);
    uninstall->setEnabled(writer_ != nullptr);
    row->addWidget(uninstall);
    row->addStretch();
    row->addWidget(close);
    lay->addLayout(row);

    connect(close, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(uninstall, &QPushButton::clicked, &dlg, [this, &dlg, table, games] {
        const int r = table->currentRow();
        if (r < 0 || r >= static_cast<int>(games.size())) return;
        const QString tid = games[r].title_id;
        if (QMessageBox::warning(&dlg, QStringLiteral("Uninstall"), QStringLiteral("Delete /dev_hdd0/game/%1 and flag the content database for rebuild so it leaves the XMB on next boot?\n\nThis cannot be undone.").arg(tid), QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        dlg.accept();
        uninstall_game(tid);
    });

    QThread scan_thread;
    std::vector<game_size_scanner::target> targets;
    for (int i = 0; i < static_cast<int>(games.size()); ++i)
        targets.push_back({i, games[i].game_inode, games[i].save_inodes});

    job b;
    game_size_scanner* scanner = nullptr;
    if (!targets.empty() && base_job(b)) {
        scanner = new game_size_scanner(b.device, b.eid, b.broker_port, b.broker_token, std::move(targets));
        scanner->moveToThread(&scan_thread);
        connect(&scan_thread, &QThread::started, scanner, &game_size_scanner::run);
        connect(scanner, &game_size_scanner::sized, &dlg,
                [table](int i, quint64 g, quint64 s) {
                    if (auto* c3 = table->item(i, 3))
                        c3->setText(QString::fromStdString(disk::format_size(g)));
                    if (auto* c4 = table->item(i, 4))
                        c4->setText(s ? QString::fromStdString(disk::format_size(s)) : QStringLiteral("-"));
                });
        connect(&dlg, &QDialog::finished, &dlg, [scanner](int) { scanner->cancel(); });
        scan_thread.start();
    } else {
        for (int i = 0; i < static_cast<int>(games.size()); ++i) {
            if (auto* c3 = table->item(i, 3)) c3->setText(QStringLiteral("-"));
            if (auto* c4 = table->item(i, 4)) c4->setText(QStringLiteral("-"));
        }
    }

    dlg.exec();

    if (scanner) {
        scanner->cancel();
        scan_thread.quit();
        scan_thread.wait();
        delete scanner;
    }
}

void main_window::uninstall_game(const QString& title_id) {
    if (!require_writer() || !fs_) return;
    const auto gdir = fs_->resolve_path_to_inode_number("game");
    if (!gdir) { log(QStringLiteral("No /dev_hdd0/game directory found.")); return; }
    job j;
    if (!base_job(j)) return;
    j.file_operation = job::fop_delete;
    j.fop_items.push_back({*gdir, title_id, 0, true});
    j.set_rebuild = true;
    pending_refresh_.clear();
    const QModelIndex gi = model_->index_for_inode(*gdir);
    if (gi.isValid()) pending_refresh_.push_back(QPersistentModelIndex(gi));
    start_job(std::move(j), QStringLiteral("Uninstall %1").arg(title_id));
}

void main_window::rebuild_database() {
    job j;
    if (!base_job(j)) return;
    if (QMessageBox::question(this, QStringLiteral("Rebuild database"), QStringLiteral("Set the mms/db.err rebuild flag so the console re-indexes the game list on next boot? This is non-destructive: it does not delete mms/db (deleting it can wedge the rebuild at 0%)."), QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    j.type = job::rebuild_database;
    start_job(std::move(j), QStringLiteral("Rebuild Database"));
}

void main_window::backup_database() {
    if (!fs_) return;
    const auto dbdir = fs_->resolve_path_to_inode_number("mms/db");
    if (!dbdir) {
        QMessageBox::information(this, QStringLiteral("Backup DB"), QStringLiteral("No mms/db directory on this disk."));
        return;
    }
    const QString base = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose a folder to back up mms/db into"));
    if (base.isEmpty()) return;
    const QString sub = base + QStringLiteral("/mms_db_backup_") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    QDir().mkpath(sub);
    int n = 0;
    try {
        for (const auto& e : fs_->read_directory(fs_->read_inode(*dbdir))) {
            if (e.name == "." || e.name == ".." || e.type == fs::dirent_type::directory) continue;
            const auto data = fs_->read_inode_data(fs_->read_inode(e.inode_number));
            QFile f(sub + QStringLiteral("/") + QString::fromStdString(e.name));
            if (f.open(QIODevice::WriteOnly)) {
                f.write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size()));
                ++n;
            }
        }
        log(QStringLiteral("Backed up %1 mms/db file(s) to %2").arg(n).arg(sub));
        QMessageBox::information(this, QStringLiteral("Backup DB"), QStringLiteral("Backed up %1 file(s) to:\n%2").arg(n).arg(sub));
    } catch (const std::exception& ex) {
        log(QStringLiteral("backup error: %1").arg(QString::fromUtf8(ex.what())));
    }
}

void main_window::restore_database() {
    job j;
    if (!base_job(j)) return;
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose the backup folder to restore mms/db from"));
    if (dir.isEmpty()) return;
    if (QMessageBox::warning(this, QStringLiteral("Restore DB"), QStringLiteral("Write the files in\n%1\ninto mms/db, replacing the current content database on %2?\n\nUse this to recover from a bad database.").arg(dir, j.device), QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;
    j.type = job::restore_db;
    j.host_dir = dir;
    start_job(std::move(j), QStringLiteral("Restore DB"));
}

void main_window::cancel_job() {
    if (!job_worker_) return;
    job_worker_->cancel();
    cancel_btn_->setEnabled(false);
    log(QStringLiteral("Cancelling at the next safe point..."));
}

bool main_window::require_writer() {
    if (writer_) return true;
    QMessageBox::information(this, QStringLiteral("Read-only"), QStringLiteral("This disk is open read-only. Run as Administrator to edit."));
    return false;
}

void main_window::remount() {
    if (!mount_) return;
    const bool had_writer = writer_ != nullptr;
    writer_.reset();
    fs_ = std::make_unique<fs::ufs2_filesystem>(*mount_->decrypted, mount_->partition_sector);
    if (!fs_->mount()) { log(QStringLiteral("remount failed")); return; }
    if (had_writer) writer_ = std::make_unique<fs::ufs2_writer>(*fs_, *mount_->decrypted);
    if (model_) model_->rebind(fs_.get(), writer_.get());
    art_cache_.clear();
    update_capacity();
}

void main_window::update_capacity() {
    if (!status_) return;
    if (!fs_) { set_status(QStringLiteral("No disk mounted.")); return; }
    const auto& sb = fs_->sb();
    const std::uint64_t total =
        static_cast<std::uint64_t>(sb.total_data_fragments) * static_cast<std::uint64_t>(sb.fragment_size);
    const std::uint64_t free = static_cast<std::uint64_t>(sb.free_space_bytes());
    const QString capacity = QStringLiteral("%1 free of %2").arg(QString::fromStdString(disk::format_size(free)), QString::fromStdString(disk::format_size(total)));
    set_status(mount_desc_.isEmpty() ? capacity : mount_desc_ + QStringLiteral("   |   ") + capacity);
}

const main_window::art_entry& main_window::art_for_dir(std::uint64_t inode) {
    auto it = art_cache_.find(inode);
    if (it != art_cache_.end()) return it.value();
    art_entry a;
    if (fs_) {
        try {
            std::uint64_t icon0 = 0, pic1 = 0, sfo = 0;
            for (const auto& e : fs_->read_directory(fs_->read_inode(inode))) {
                if (e.type == fs::dirent_type::directory) continue;
                if (e.name == "ICON0.PNG") icon0 = e.inode_number;
                else if (e.name == "PIC1.PNG") pic1 = e.inode_number;
                else if (e.name == "PARAM.SFO") sfo = e.inode_number;
            }
            if (icon0) {
                auto read = [&](std::uint64_t in) {
                    const auto d = fs_->read_inode_data(fs_->read_inode(in));
                    return QByteArray(reinterpret_cast<const char*>(d.data()), static_cast<qsizetype>(d.size()));
                };
                a.icon0 = read(icon0);
                if (pic1) a.pic1 = read(pic1);
                if (sfo) {
                    const auto kv = parse_sfo(fs_->read_inode_data(fs_->read_inode(sfo)));
                    a.title = kv.value(QStringLiteral("TITLE"));
                    a.subtitle = kv.value(QStringLiteral("SUB_TITLE"));
                    if (a.subtitle.isEmpty()) a.subtitle = kv.value(QStringLiteral("DETAIL"));
                    const QString cat = kv.value(QStringLiteral("CATEGORY"));
                    if (a.subtitle.isEmpty() && !cat.isEmpty()) a.subtitle = category_name(cat);
                }
                a.has = true;
            }
        } catch (...) {
        }
    }
    return *art_cache_.insert(inode, a);
}

void main_window::update_preview(const QModelIndex& index) {
    if (!preview_) return;
    if (!show_art_panel_) { preview_->hide(); return; } // opt in, folders use tooltips
    if (!fs_ || !index.isValid() || !model_) {
        preview_->clear_content();
        preview_->hide();
        return;
    }

    QModelIndex cur = index;
    if (!model_->is_dir(cur)) cur = cur.parent();
    for (int up = 0; up < 6 && cur.isValid(); ++up) {
        const art_entry& a = art_for_dir(model_->inode_of(cur));
        if (a.has) {
            preview_->set_content(a.title, a.subtitle, a.icon0, a.pic1);
            preview_->show();
            return;
        }
        cur = cur.parent();
    }
    preview_->clear_content();
    preview_->hide();
}

void main_window::apply_settings() {
    show_art_panel_ = app_setting(settings_keys::show_art_panel, false);
    if (model_) {
        model_->set_show_covers(app_setting(settings_keys::show_covers, true));
        model_->set_show_tooltips(app_setting(settings_keys::show_tooltips, true));
    }
    if (show_art_panel_) update_preview(tree_->currentIndex());
    else if (preview_) preview_->hide();
}

void main_window::open_settings() {
    settings_dialog dlg(this);
    if (dlg.exec() == QDialog::Accepted) apply_settings();
}

void main_window::verify_package() {
    const QString p = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select a PKG to verify"), QString(),
        QStringLiteral("PKG files (*.pkg);;All files (*)"));
    if (p.isEmpty()) return;
    job j;
    j.type = job::verify_pkg;
    j.pkg_path = p;
    start_job(std::move(j), QStringLiteral("Verify %1").arg(QFileInfo(p).fileName()));
}

void main_window::act_edit_sfo() {
    if (!require_writer()) return;
    const QModelIndex idx = tree_->currentIndex();
    if (!idx.isValid() || model_->is_dir(idx)) return;
    const QString fname = model_->name_of(idx);
    if (fname.compare(QStringLiteral("PARAM.SFO"), Qt::CaseInsensitive) != 0) return;

    const std::uint64_t inode = model_->inode_of(idx);
    const std::uint64_t parent = model_->parent_inode_of(idx);
    std::vector<sfo_field> fields;
    try {
        fields = parse_sfo_fields(fs_->read_inode_data(fs_->read_inode(inode)));
    } catch (const std::exception& ex) {
        QMessageBox::warning(this, QStringLiteral("Edit PARAM.SFO"), QStringLiteral("Could not read the SFO: %1").arg(QString::fromUtf8(ex.what())));
        return;
    }
    if (fields.empty()) {
        QMessageBox::warning(this, QStringLiteral("Edit PARAM.SFO"), QStringLiteral("This file is not a valid PARAM.SFO."));
        return;
    }

    QStringList parts;
    for (QModelIndex i = idx.parent(); i.isValid(); i = i.parent()) {
        const QString n = model_->name_of(i);
        if (n != QStringLiteral("/")) parts.prepend(n);
    }
    sfo_editor dlg(fields, QStringLiteral("/%1/PARAM.SFO").arg(parts.join(QChar('/'))), this);
    if (dlg.exec() != QDialog::Accepted) return;

    const auto bytes = serialize_sfo(dlg.fields());
    try {
        writer_->delete_file(parent, fname.toStdString());
        std::vector<std::byte> v(bytes.begin(), bytes.end());
        writer_->write_file(parent, fname.toStdString(), v);
        app::invalidate_content_database(*fs_, *writer_);
        writer_->update_superblock();
    } catch (const std::exception& ex) {
        QMessageBox::warning(this, QStringLiteral("Edit PARAM.SFO"), QStringLiteral("Write failed: %1").arg(QString::fromUtf8(ex.what())));
        return;
    }
    log(QStringLiteral("Saved %1 SFO field(s) to PARAM.SFO; rebuild flag set.").arg(dlg.fields().size()));
    remount();
    const QModelIndex di = idx.parent();
    if (di.isValid()) model_->refresh(di);
    QMessageBox::information(this, QStringLiteral("Edit PARAM.SFO"), QStringLiteral("Saved. The console will re-index the content on next boot."));
}

void main_window::show_tree_menu(const QPoint& pos) {
    const QModelIndex idx = tree_->indexAt(pos);
    if (!idx.isValid()) return;
    tree_->setCurrentIndex(idx);
    const bool is_dir = model_->is_dir(idx);
    const bool is_root = model_->is_root(idx);

    QMenu menu(this);
    menu.addAction(QStringLiteral("Extract..."), this, &main_window::act_extract);
    if (is_dir) {
        menu.addAction(QStringLiteral("New folder..."), this, &main_window::act_new_folder);
        menu.addAction(QStringLiteral("Import file..."), this, &main_window::act_import);
        menu.addAction(QStringLiteral("Import folder..."), this, &main_window::act_import_folder);
    }
    menu.addSeparator();
    if (!is_root) {
        menu.addAction(QStringLiteral("Cut"), this, &main_window::act_cut)
            ->setShortcut(QKeySequence::Cut);
        menu.addAction(QStringLiteral("Copy"), this, &main_window::act_copy)
            ->setShortcut(QKeySequence::Copy);
    }
    QAction* paste = menu.addAction(QStringLiteral("Paste"), this, &main_window::act_paste);
    paste->setShortcut(QKeySequence::Paste);
    paste->setEnabled(!clipboard_.empty() && writer_ != nullptr);
    if (!is_root) {
        menu.addSeparator();
        QAction* ren = menu.addAction(QStringLiteral("Rename"));
        ren->setShortcut(Qt::Key_F2);
        connect(ren, &QAction::triggered, this, [this] {
            const QModelIndex cur = tree_->currentIndex();
            if (cur.isValid()) tree_->edit(cur.siblingAtColumn(0)); // inline rename
        });
        menu.addAction(QStringLiteral("Delete"), this, &main_window::act_delete)
            ->setShortcut(QKeySequence::Delete);
    }
    if (!is_dir && model_->name_of(idx).compare(QStringLiteral("PARAM.SFO"), Qt::CaseInsensitive) == 0) {
        menu.addSeparator();
        QAction* edit = menu.addAction(QStringLiteral("Edit PARAM.SFO..."), this, &main_window::act_edit_sfo);
        edit->setEnabled(writer_ != nullptr);
    }
    menu.addSeparator();
    menu.addAction(QStringLiteral("Properties"), this, &main_window::act_properties)
        ->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

void main_window::act_extract() {
    const QModelIndex idx = tree_->currentIndex();
    if (!idx.isValid() || !fs_) return;
    const std::uint64_t inode = model_->inode_of(idx);
    const bool is_dir = model_->is_dir(idx);
    const QString name = model_->is_root(idx) ? QStringLiteral("root") : model_->name_of(idx);

    QString host_dest;
    if (is_dir) {
        const QString base = QFileDialog::getExistingDirectory(this, QStringLiteral("Extract folder into"));
        if (base.isEmpty()) return;
        host_dest = base; // the worker writes it under base/<name>
    } else {
        const QString out = QFileDialog::getSaveFileName(this, QStringLiteral("Extract file"), name);
        if (out.isEmpty()) return;
        host_dest = out;
    }

    job j;
    if (!base_job(j)) return;
    j.file_operation = job::fop_extract;
    j.fop_items.push_back({0, name, inode, is_dir});
    j.fop_host_dest = host_dest;
    start_job(std::move(j), QStringLiteral("Extract"));
}

void main_window::act_new_folder() {
    const QModelIndex idx = tree_->currentIndex();
    if (!idx.isValid() || !model_->is_dir(idx) || !require_writer()) return;
    bool ok = false;
    const QString nm = QInputDialog::getText(this, QStringLiteral("New folder"), QStringLiteral("Folder name:"), QLineEdit::Normal, {}, &ok);
    if (!ok || nm.isEmpty()) return;
    try {
        writer_->create_directory(model_->inode_of(idx), nm.toStdString());
        writer_->update_superblock();
        remount();
        model_->refresh(idx);
        tree_->expand(idx);
        log(QStringLiteral("Created folder %1").arg(nm));
    } catch (const std::exception& ex) {
        log(QStringLiteral("mkdir error: %1").arg(QString::fromUtf8(ex.what())));
    }
}

void main_window::act_import() {
    const QModelIndex idx = tree_->currentIndex();
    if (!idx.isValid() || !model_->is_dir(idx) || !require_writer()) return;
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Import file(s)"));
    if (paths.isEmpty()) return;
    job j;
    if (!base_job(j)) return;
    j.file_operation = job::fop_import;
    j.fop_dest = model_->inode_of(idx);
    j.fop_import_paths = paths;
    pending_refresh_.clear();
    pending_refresh_.push_back(QPersistentModelIndex(idx));
    start_job(std::move(j), QStringLiteral("Import"));
}

void main_window::act_import_folder() {
    const QModelIndex idx = tree_->currentIndex();
    if (!idx.isValid() || !model_->is_dir(idx) || !require_writer()) return;
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Import folder"));
    if (dir.isEmpty()) return;
    job j;
    if (!base_job(j)) return;
    j.file_operation = job::fop_import;
    j.fop_dest = model_->inode_of(idx);
    j.fop_import_paths = QStringList{dir};
    pending_refresh_.clear();
    pending_refresh_.push_back(QPersistentModelIndex(idx));
    start_job(std::move(j), QStringLiteral("Import folder"));
}

void main_window::act_delete() {
    if (!require_writer()) return;
    struct target { std::uint64_t pinode; QString name; QPersistentModelIndex parent; };
    std::vector<target> targets;
    QModelIndexList sel = tree_->selectionModel()->selectedRows();
    if (sel.isEmpty() && tree_->currentIndex().isValid()) sel << tree_->currentIndex();
    for (const QModelIndex& idx : sel) {
        if (!idx.isValid() || model_->is_root(idx)) continue;
        targets.push_back({model_->parent_inode_of(idx), model_->name_of(idx), QPersistentModelIndex(idx.parent())});
    }
    if (targets.empty()) return;

    const QString prompt = targets.size() == 1 ? QStringLiteral("Delete \"%1\"? This cannot be undone.").arg(targets.front().name) : QStringLiteral("Delete %1 items? This cannot be undone.").arg(targets.size());
    if (QMessageBox::warning(this, QStringLiteral("Delete"), prompt, QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;

    job j;
    if (!base_job(j)) return;
    j.file_operation = job::fop_delete;
    pending_refresh_.clear();
    for (const target& t : targets) {
        j.fop_items.push_back({t.pinode, t.name, 0, false});
        if (!pending_refresh_.contains(t.parent)) pending_refresh_.push_back(t.parent);
    }
    start_job(std::move(j), QStringLiteral("Delete"));
}

void main_window::act_copy() {
    clipboard_.clear();
    clip_is_cut_ = false;
    for (const QModelIndex& idx : tree_->selectionModel()->selectedRows()) {
        if (!idx.isValid() || model_->is_root(idx)) continue;
        clipboard_.push_back({model_->parent_inode_of(idx), model_->name_of(idx), model_->is_dir(idx), model_->inode_of(idx)});
    }
    if (!clipboard_.empty()) log(QStringLiteral("Copied %1 item(s).").arg(clipboard_.size()));
}

void main_window::act_cut() {
    act_copy();
    clip_is_cut_ = !clipboard_.empty();
    if (clip_is_cut_) log(QStringLiteral("Cut %1 item(s). Paste to move.").arg(clipboard_.size()));
}

void main_window::act_paste() {
    if (clipboard_.empty() || !require_writer()) return;
    QModelIndex dir_idx = tree_->currentIndex();
    if (dir_idx.isValid() && !model_->is_dir(dir_idx)) dir_idx = dir_idx.parent();
    if (!dir_idx.isValid()) dir_idx = model_->root_index();
    const std::uint64_t dst = model_->inode_of(dir_idx);
    std::set<std::uint64_t> dst_chain;
    for (QModelIndex a = dir_idx; a.isValid(); a = a.parent())
        dst_chain.insert(model_->inode_of(a));

    job j;
    if (!base_job(j)) return;
    const bool was_cut = clip_is_cut_;
    j.file_operation = was_cut ? job::fop_move : job::fop_copy;
    j.fop_dest = dst;
    pending_refresh_.clear();
    pending_refresh_.push_back(QPersistentModelIndex(dir_idx));
    int queued = 0;
    for (const clip_entry& it : clipboard_) {
        if (it.is_dir && dst_chain.count(it.inode)) {
            log(QStringLiteral("Skipped %1 (cannot paste a folder into itself).").arg(it.name));
            continue;
        }
        j.fop_items.push_back({it.parent_inode, it.name, it.inode, it.is_dir});
        if (was_cut) { // a move empties the source dir too, so reload it afterwards
            const QPersistentModelIndex p(model_->index_for_inode(it.parent_inode));
            if (p.isValid() && !pending_refresh_.contains(p)) pending_refresh_.push_back(p);
        }
        ++queued;
    }
    if (queued == 0) return;
    if (was_cut) { clipboard_.clear(); clip_is_cut_ = false; }
    start_job(std::move(j), was_cut ? QStringLiteral("Move") : QStringLiteral("Paste"));
}

} // namespace ps3hdd::ui
