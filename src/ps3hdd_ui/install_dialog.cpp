#include "install_dialog.h"

#include "settings_dialog.h" // app_setting + verify before install key
#include "sfo_util.h"

#include <ps3hdd_disk/disk_source.h>
#include <ps3hdd_pkg/ps3_pkg_reader.h>

#include <QFileInfo>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSize>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <exception>

namespace ps3hdd::ui {

namespace {
int ver_num(const QString& s) {
    QString d;
    for (QChar c : s)
        if (c.isDigit()) d += c;
    return d.isEmpty() ? 0 : d.toInt();
}

QString platform_label(quint32 content_type) {
    switch (content_type) {
    case 0x06: return QStringLiteral("PS1 Classics");
    case 0x07: return QStringLiteral("PC Engine");
    case 0x09: return QStringLiteral("Theme");
    case 0x0B: return QStringLiteral("License");
    case 0x0F: return QStringLiteral("minis");
    case 0x10: return QStringLiteral("NeoGeo");
    case 0x12: return QStringLiteral("PS2 Classics");
    default:   return {};
    }
}

bool is_zero_content_name(const QString& content_id) {
    const QString tail = content_id.section(QChar('-'), -1);
    return !tail.isEmpty() && tail.count(QLatin1Char('0')) == tail.size();
}
} // namespace

install_dialog::install_dialog(QStringList paths, QString device, std::vector<std::byte> eid, quint16 broker_port, QByteArray broker_token, QWidget* parent) : QDialog(parent),
      paths_(std::move(paths)),
      device_(std::move(device)),
      eid_(std::move(eid)),
      port_(broker_port),
      token_(std::move(broker_token)) {
    verify_first_ = app_setting(settings_keys::verify_before_install, true);
    setWindowTitle(QStringLiteral("Install packages"));
    setWindowIcon(QIcon(QStringLiteral(":/icons/package.bmp")));
    resize(640, 460);

    parse_all();
    classify();
    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
        if (items_[i].error.isEmpty()) last_ok_index_ = i;

    auto* lay = new QVBoxLayout(this);
    auto* top = new QHBoxLayout();
    top->setSpacing(14);
    icon_label_ = new QLabel();
    icon_label_->setFixedSize(120, 68);
    icon_label_->setAlignment(Qt::AlignCenter);
    for (const pkg_item& it : items_)
        if (!it.icon.isEmpty()) {
            QPixmap pm;
            if (pm.loadFromData(it.icon)) {
                icon_label_->setPixmap(pm.scaled(icon_label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)); // keep aspect ratio
                break;
            }
        }
    header_ = new QLabel();
    header_->setWordWrap(true);
    top->addWidget(icon_label_);
    top->addWidget(header_, 1);
    lay->addLayout(top);

    QString main_title;
    for (const pkg_item& it : items_)
        if (it.rank == 0 && !it.title.isEmpty()) { main_title = it.title; break; }
    if (main_title.isEmpty())
        for (const pkg_item& it : items_)
            if (!it.title.isEmpty()) { main_title = it.title; break; }
    if (main_title.isEmpty()) main_title = QStringLiteral("%1 package(s)").arg(items_.size());

    int n_game = 0, n_upd = 0, n_dlc = 0, n_other = 0;
    for (const pkg_item& it : items_) {
        if (!it.error.isEmpty()) continue;
        if (it.rank == 0) ++n_game;
        else if (it.rank == 1) ++n_upd;
        else if (it.rank == 2) ++n_dlc;
        else ++n_other;
    }
    auto plural = [](int n, const QString& one, const QString& many) {
        return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
    };

    QSet<QString> distinct_titles;
    int installable = 0;
    for (const pkg_item& it : items_)
        if (it.error.isEmpty()) { distinct_titles.insert(it.title_id); ++installable; }

    QString line1, line2;
    if (distinct_titles.size() > 1) {
        line1 = QStringLiteral("<b>Install %1</b>").arg(plural(installable, QStringLiteral("package"), QStringLiteral("packages")));
        QStringList parts;
        if (n_game) parts << plural(n_game, QStringLiteral("title"), QStringLiteral("titles"));
        if (n_upd) parts << plural(n_upd, QStringLiteral("update"), QStringLiteral("updates"));
        if (n_dlc) parts << (n_dlc == 1 ? QStringLiteral("1 DLC") : QStringLiteral("%1 DLC").arg(n_dlc));
        if (n_other) parts << plural(n_other, QStringLiteral("extra"), QStringLiteral("extras"));
        if (parts.size() > 1) line2 = parts.join(QStringLiteral(", "));
    } else if (n_game > 0) {
        line1 = QStringLiteral("<b>Install %1</b>").arg(main_title.toHtmlEscaped());
        QStringList extras;
        if (n_upd) extras << plural(n_upd, QStringLiteral("update"), QStringLiteral("updates"));
        if (n_dlc) extras << (n_dlc == 1 ? QStringLiteral("1 DLC") : QStringLiteral("%1 DLC").arg(n_dlc));
        if (n_other) extras << plural(n_other, QStringLiteral("extra"), QStringLiteral("extras"));
        if (!extras.isEmpty()) line2 = QStringLiteral("with %1").arg(extras.join(QStringLiteral(" and ")));
    } else {
        QStringList klabels;
        if (n_upd) klabels << QStringLiteral("Update");
        if (n_dlc) klabels << QStringLiteral("DLC");
        if (n_other) klabels << QStringLiteral("Content");
        const int total = n_upd + n_dlc + n_other;
        line1 = klabels.isEmpty() ? QStringLiteral("<b>Install %1</b>").arg(main_title.toHtmlEscaped()) : QStringLiteral("<b>Install %1 %2</b>").arg(main_title.toHtmlEscaped(), klabels.join(QStringLiteral(" + ")));
        if (total > 0) line2 = plural(total, QStringLiteral("item"), QStringLiteral("items"));
    }
    header_->setText(line2.isEmpty() ? line1 : line1 + QStringLiteral("<br>") + line2);

    table_ = new QTableWidget(static_cast<int>(items_.size()), 4, this);
    table_->setHorizontalHeaderLabels({QStringLiteral("Type"), QStringLiteral("Name"), QStringLiteral("Content"), QStringLiteral("Size")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->verticalHeader()->setVisible(false);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const pkg_item& it = items_[i];
        table_->setItem(i, 0, new QTableWidgetItem(it.error.isEmpty() ? it.kind : QStringLiteral("(unreadable)")));
        QString name;
        if (!it.error.isEmpty() || it.rank == 2 || it.title.isEmpty())
            name = QFileInfo(it.path).completeBaseName();
        else
            name = it.title;
        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setToolTip(it.error.isEmpty() ? QFileInfo(it.path).fileName() : it.error);
        table_->setItem(i, 1, nameItem);
        const QString tail = it.content_id.section(QChar('-'), -1);
        table_->setItem(i, 2, new QTableWidgetItem(tail.isEmpty() ? it.title_id : tail));
        table_->setItem(i, 3, new QTableWidgetItem(QString::fromStdString(disk::format_size(it.size))));
    }
    table_->resizeColumnsToContents();
    auto* hh = table_->horizontalHeader();
    hh->setStretchLastSection(false);
    hh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(1, QHeaderView::Stretch);
    hh->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    lay->addWidget(table_, 1);

    auto* obar_row = new QHBoxLayout();
    obar_row->addWidget(new QLabel(QStringLiteral("Total:")));
    overall_bar_ = new QProgressBar();
    overall_bar_->setRange(0, 100);
    obar_row->addWidget(overall_bar_, 1);
    auto* cbar_row = new QHBoxLayout();
    cbar_row->addWidget(new QLabel(QStringLiteral("Current:")));
    bar_ = new QProgressBar();
    cbar_row->addWidget(bar_, 1);
    auto* bars = new QWidget();
    auto* bars_lay = new QVBoxLayout(bars);
    bars_lay->setContentsMargins(0, 0, 0, 0);
    bars_lay->addLayout(obar_row);
    bars_lay->addLayout(cbar_row);
    bars->setVisible(false);
    bars_ = bars;
    lay->addWidget(bars);

    for (const pkg_item& it : items_)
        if (it.error.isEmpty()) total_bytes_ += it.size;

    status_ = new QLabel(QStringLiteral("The content database will be flagged for rebuild automatically."));
    status_->setWordWrap(true);
    lay->addWidget(status_);

    auto* row = new QHBoxLayout();
    install_btn_ = new QPushButton(QStringLiteral("Install"));
    install_btn_->setIcon(QIcon(QStringLiteral(":/icons/package.bmp")));
    cancel_btn_ = new QPushButton(QStringLiteral("Cancel"));
    row->addStretch(1);
    row->addWidget(install_btn_);
    row->addWidget(cancel_btn_);
    lay->addLayout(row);

    bool any_ok = false;
    for (const pkg_item& it : items_) any_ok = any_ok || it.error.isEmpty();
    install_btn_->setEnabled(any_ok);

    connect(install_btn_, &QPushButton::clicked, this, &install_dialog::start_install);
    connect(cancel_btn_, &QPushButton::clicked, this, &install_dialog::request_cancel);

    int bh = 0;
    for (QPushButton* b : {install_btn_, cancel_btn_}) {
        b->setAutoDefault(false);
        b->setDefault(false);
        b->setIconSize(QSize(16, 16));
        bh = std::max(bh, b->sizeHint().height());
    }
    for (QPushButton* b : {install_btn_, cancel_btn_}) b->setMinimumHeight(bh);
}

void install_dialog::parse_all() {
    for (const QString& path : paths_) {
        pkg_item it;
        it.path = path;
        it.size = static_cast<std::uint64_t>(QFileInfo(path).size());
        try {
            auto reader = pkg::ps3_pkg_reader::from_file(path.toStdString());
            it.title_id = QString::fromStdString(reader.title_id());
            it.content_id = QString::fromStdString(reader.content_id());
            it.content_type = reader.content_type();
            it.files = static_cast<int>(reader.entries().size());
            for (const auto& e : reader.entries()) {
                const QString n = QString::fromStdString(e.name);
                if (n == QStringLiteral("PARAM.SFO") || n.endsWith(QStringLiteral("/PARAM.SFO"))) {
                    std::vector<std::byte> buf(static_cast<std::size_t>(e.data_size));
                    reader.decrypt_range(e, 0, {buf.data(), buf.size()});
                    const auto kv = parse_sfo(buf);
                    it.title = kv.value(QStringLiteral("TITLE"));
                    it.category = kv.value(QStringLiteral("CATEGORY"));
                    it.app_ver = kv.value(QStringLiteral("APP_VER"));
                } else if (n == QStringLiteral("ICON0.PNG") || n.endsWith(QStringLiteral("/ICON0.PNG"))) {
                    std::vector<std::byte> buf(static_cast<std::size_t>(e.data_size));
                    reader.decrypt_range(e, 0, {buf.data(), buf.size()});
                    it.icon = QByteArray(reinterpret_cast<const char*>(buf.data()), static_cast<qsizetype>(buf.size()));
                }
            }
        } catch (const std::exception& ex) {
            it.error = QString::fromUtf8(ex.what());
        }
        items_.push_back(std::move(it));
    }
}

void install_dialog::classify() {
    QHash<QString, int> base_ver;
    for (const pkg_item& it : items_) {
        const QString c = it.category.toUpper();
        if (c == QStringLiteral("HG") || c == QStringLiteral("DG")) {
            const int v = ver_num(it.app_ver);
            if (!base_ver.contains(it.content_id) || v < base_ver[it.content_id])
                base_ver[it.content_id] = v;
        }
    }
    for (pkg_item& it : items_) {
        const QString c = it.category.toUpper();
        const int v = ver_num(it.app_ver);
        const QString plat = platform_label(it.content_type);
        if (!plat.isEmpty()) {
            it.kind = plat;
            it.rank = 0;
        } else if (c == QStringLiteral("GD") || c == QStringLiteral("AC")) {
            it.kind = QStringLiteral("DLC");
            it.rank = 2;
        } else if (c == QStringLiteral("HG") || c == QStringLiteral("DG")) {
            const bool is_update = is_zero_content_name(it.content_id) || v > base_ver.value(it.content_id, v);
            it.kind = is_update ? QStringLiteral("Game Update") : QStringLiteral("Game");
            it.rank = is_update ? 1 : 0;
        } else {
            it.kind = category_name(it.category);
            it.rank = 0;
        }
    }

    std::stable_sort(items_.begin(), items_.end(), [](const pkg_item& a, const pkg_item& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        return ver_num(a.app_ver) < ver_num(b.app_ver);
    });
}

void install_dialog::start_install() {
    if (installing_) return;
    installing_ = true;
    install_btn_->setVisible(false);
    table_->setEnabled(false);
    bars_->setVisible(true);
    bar_->setRange(0, 0);
    overall_bar_->setValue(0);
    current_ = 0;
    run_current();
}

void install_dialog::run_current() {
    while (current_ < static_cast<int>(items_.size()) && !items_[current_].error.isEmpty()) ++current_;
    if (current_ >= static_cast<int>(items_.size())) {
        installing_ = false;
        installed_ok_ = true;
        bar_->setRange(0, 100);
        bar_->setValue(100);
        overall_bar_->setValue(100);
        status_->setText(QStringLiteral("All packages installed. The console will re-index on next boot."));
        cancel_btn_->setText(QStringLiteral("Close"));
        return;
    }
    const pkg_item& it = items_[current_];
    table_->selectRow(current_);
    const QString name = it.title.isEmpty() ? QFileInfo(it.path).fileName() : it.title;

    int total_ok = 0, pos = 0;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
        if (items_[i].error.isEmpty()) { ++total_ok; if (i <= current_) ++pos; }

    const bool need_verify = verify_first_ && !items_[current_].verified;
    verifying_ = need_verify;
    if (!need_verify) wrote_ = true;
    status_->setText((need_verify ? QStringLiteral("Verifying %1  (item %2 of %3)") : QStringLiteral("Installing %1  (item %2 of %3)")).arg(name).arg(pos).arg(total_ok));

    job j;
    j.type = need_verify ? job::verify_pkg : job::install_pkg;
    j.verify_quick = true;
    j.device = device_;
    j.eid = eid_;
    j.pkg_path = it.path;
    j.rebuild_db = true;
    j.skip_consistency = current_ != last_ok_index_;
    j.broker_port = port_;
    j.broker_token = token_;

    thread_ = new QThread(this);
    worker_ = new worker(std::move(j));
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::started, worker_, &worker::run);
    connect(worker_, &worker::progress, this, &install_dialog::on_progress);
    connect(worker_, &worker::finished, this, &install_dialog::on_finished);
    thread_->start();
}

void install_dialog::on_progress(const QString& line, int percent) {
    if (percent >= 0) { bar_->setRange(0, 100); bar_->setValue(percent); }
    else bar_->setRange(0, 0);
    if (!line.isEmpty()) status_->setText(line);

    if (!verifying_ && overall_bar_ && total_bytes_ > 0 &&
        current_ >= 0 && current_ < static_cast<int>(items_.size())) {
        const double frac = percent >= 0 ? percent / 100.0 : 0.0;
        const double done = static_cast<double>(bytes_done_) + frac * static_cast<double>(items_[current_].size);
        overall_bar_->setValue(static_cast<int>(100.0 * done / static_cast<double>(total_bytes_)));
    }
}

void install_dialog::on_finished(bool ok, const QString& summary) {
    thread_->quit();
    thread_->wait();
    worker_->deleteLater();
    thread_->deleteLater();
    worker_ = nullptr;
    thread_ = nullptr;

    if (!ok) {
        installing_ = false;
        status_->setText(QStringLiteral("Failed: %1").arg(summary));
        cancel_btn_->setText(QStringLiteral("Close"));
        return;
    }
    if (verifying_) {
        items_[current_].verified = true;
        verifying_ = false;
        run_current();
        return;
    }
    if (current_ >= 0 && current_ < static_cast<int>(items_.size()))
        bytes_done_ += items_[current_].size;
    ++current_;
    run_current();
}

void install_dialog::request_cancel() {
    if (installing_ && worker_) {
        worker_->cancel();
        status_->setText(QStringLiteral("Cancelling at the next safe point..."));
        cancel_btn_->setEnabled(false);
        return;
    }
    if (installed_ok_) accept();
    else reject();
}

} // namespace ps3hdd::ui