#include "ufs2_model.h"

#include "sfo_util.h"

#include <ps3hdd_disk/disk_source.h>

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QPixmap>
#include <QFileInfo>
#include <QIcon>
#include <QIODevice>
#include <QMetaType>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <exception>
#include <functional>
#include <memory>

namespace ps3hdd::ui {
static const QString kNodesMime = QStringLiteral("application/x-ps3hdd-nodes");

namespace {
QString file_extension(const QString& name) {
    const int dot = name.lastIndexOf('.');
    return dot > 0 ? name.mid(dot + 1).toLower() : QString();
}
QString file_icon_key(const QString& name) {
    static const QHash<QString, QString> kMap = {
        {QStringLiteral("self"), QStringLiteral("application")},
        {QStringLiteral("fself"), QStringLiteral("application")},
        {QStringLiteral("elf"), QStringLiteral("application")},
        {QStringLiteral("eboot"), QStringLiteral("application")},
        {QStringLiteral("bin"), QStringLiteral("box")},
        {QStringLiteral("sprx"), QStringLiteral("plugin")},
        {QStringLiteral("prx"), QStringLiteral("plugin")},
        {QStringLiteral("pkg"), QStringLiteral("package")},
        {QStringLiteral("pup"), QStringLiteral("package")},
        {QStringLiteral("iso"), QStringLiteral("drive_disk")},
        {QStringLiteral("sfo"), QStringLiteral("page_white_gear")},
        {QStringLiteral("sft"), QStringLiteral("page_white_gear")},
        {QStringLiteral("ini"), QStringLiteral("page_white_gear")},
        {QStringLiteral("cfg"), QStringLiteral("page_white_gear")},
        {QStringLiteral("scm"), QStringLiteral("page_white_gear")},
        {QStringLiteral("xml"), QStringLiteral("page_white_code")},
        {QStringLiteral("json"), QStringLiteral("page_white_code")},
        {QStringLiteral("txt"), QStringLiteral("page_white_text")},
        {QStringLiteral("log"), QStringLiteral("report")},
        {QStringLiteral("dat"), QStringLiteral("page_white_gear")},
        {QStringLiteral("rif"), QStringLiteral("lock")},
        {QStringLiteral("rap"), QStringLiteral("lock")},
        {QStringLiteral("edat"), QStringLiteral("lock")},
        {QStringLiteral("png"), QStringLiteral("picture")},
        {QStringLiteral("jpg"), QStringLiteral("picture")},
        {QStringLiteral("jpeg"), QStringLiteral("picture")},
        {QStringLiteral("bmp"), QStringLiteral("picture")},
        {QStringLiteral("gif"), QStringLiteral("picture")},
        {QStringLiteral("dds"), QStringLiteral("picture")},
        {QStringLiteral("gim"), QStringLiteral("picture")},
        {QStringLiteral("pam"), QStringLiteral("film")},
        {QStringLiteral("mp4"), QStringLiteral("film")},
        {QStringLiteral("m4v"), QStringLiteral("film")},
        {QStringLiteral("bik"), QStringLiteral("film")},
        {QStringLiteral("at3"), QStringLiteral("music")},
        {QStringLiteral("msf"), QStringLiteral("music")},
        {QStringLiteral("vag"), QStringLiteral("music")},
        {QStringLiteral("wav"), QStringLiteral("music")},
        {QStringLiteral("mp3"), QStringLiteral("music")},
        {QStringLiteral("zip"), QStringLiteral("compress")},
        {QStringLiteral("gz"), QStringLiteral("compress")},
        {QStringLiteral("psarc"), QStringLiteral("compress")},
        {QStringLiteral("ttf"), QStringLiteral("font")},
        {QStringLiteral("pfd"), QStringLiteral("font")},
    };
    const auto it = kMap.constFind(file_extension(name));
    return it == kMap.constEnd() ? QStringLiteral("page_white") : *it;
}
QString type_label(const QString& name) {
    static const QHash<QString, QString> kLabel = {
        {QStringLiteral("application"), QStringLiteral("Executable")},
        {QStringLiteral("plugin"), QStringLiteral("Module")},
        {QStringLiteral("package"), QStringLiteral("Package")},
        {QStringLiteral("drive_disk"), QStringLiteral("Disc image")},
        {QStringLiteral("picture"), QStringLiteral("Image")},
        {QStringLiteral("film"), QStringLiteral("Video")},
        {QStringLiteral("music"), QStringLiteral("Audio")},
        {QStringLiteral("compress"), QStringLiteral("Archive")},
        {QStringLiteral("font"), QStringLiteral("Font")},
        {QStringLiteral("page_white_gear"), QStringLiteral("Config / data")},
        {QStringLiteral("page_white_code"), QStringLiteral("Code")},
        {QStringLiteral("page_white_text"), QStringLiteral("Text")},
        {QStringLiteral("report"), QStringLiteral("Log")},
        {QStringLiteral("lock"), QStringLiteral("License")},
        {QStringLiteral("box"), QStringLiteral("Binary")},
    };
    const QString ext = file_extension(name);
    const QString base = kLabel.value(file_icon_key(name), QStringLiteral("File"));
    return ext.isEmpty() ? base : QStringLiteral("%1 (%2)").arg(base, ext.toUpper());
}
} // namespace

const QString& ufs2_model::nodes_mime() { return kNodesMime; }

namespace {

struct drag_item {
    std::uint64_t inode;
    QString name;
    bool dir;
};

class drag_mime : public QMimeData {
public:
    drag_mime(fs::ufs2_filesystem* fs, const QByteArray& nodes, std::vector<drag_item> items)
        : fs_(fs), items_(std::move(items)) {
        setData(kNodesMime, nodes);
    }

    QStringList formats() const override {
        QStringList f = QMimeData::formats();
        if (fs_ && !items_.empty() && !f.contains(QStringLiteral("text/uri-list")))
            f << QStringLiteral("text/uri-list");
        return f;
    }
    bool hasFormat(const QString& m) const override {
        if (m == QLatin1String("text/uri-list")) return fs_ && !items_.empty();
        return QMimeData::hasFormat(m);
    }

protected:
    QVariant retrieveData(const QString& m, QMetaType type) const override {
        if (m == QLatin1String("text/uri-list")) ensure_extracted();
        return QMimeData::retrieveData(m, type);
    }

private:
    void ensure_extracted() const {
        if (done_) return;
        done_ = true;
        if (!fs_ || items_.empty()) return;
        tmp_ = std::make_unique<QTemporaryDir>();
        if (!tmp_->isValid()) return;
        QList<QUrl> urls;
        for (const drag_item& it : items_) {
            const QString out = tmp_->path() + QStringLiteral("/") + it.name;
            try {
                if (it.dir) extract_dir(it.inode, out);
                else extract_file(it.inode, out);
                urls << QUrl::fromLocalFile(out);
            } catch (...) {
            }
        }
        const_cast<drag_mime*>(this)->setUrls(urls);
    }
    void extract_file(std::uint64_t inode, const QString& out) const {
        const auto data = fs_->read_inode_data(fs_->read_inode(inode));
        QFile f(out);
        if (f.open(QIODevice::WriteOnly))
            f.write(reinterpret_cast<const char*>(data.data()), static_cast<qint64>(data.size()));
    }
    void extract_dir(std::uint64_t inode, const QString& out) const {
        QDir().mkpath(out);
        for (const auto& e : fs_->read_directory(fs_->read_inode(inode))) {
            if (e.name == "." || e.name == "..") continue;
            const QString child = out + QStringLiteral("/") + QString::fromStdString(e.name);
            if (e.type == fs::dirent_type::directory) extract_dir(e.inode_number, child);
            else extract_file(e.inode_number, child);
        }
    }

    fs::ufs2_filesystem* fs_ = nullptr;
    std::vector<drag_item> items_;
    mutable std::unique_ptr<QTemporaryDir> tmp_;
    mutable bool done_ = false;
};

} // namespace

ufs2_model::ufs2_model(QObject* parent) : QAbstractItemModel(parent) {
    dir_icon_ = QIcon(QStringLiteral(":/icons/folder.bmp"));
    file_icon_ = QIcon(QStringLiteral(":/icons/page_white.bmp"));
    root_ = std::make_unique<node>();
    root_->loaded = true;
}

void ufs2_model::set_backend(fs::ufs2_filesystem* fs, fs::ufs2_writer* writer) {
    beginResetModel();
    fs_ = fs;
    writer_ = writer;
    cover_cache_.clear();
    tooltip_cache_.clear();
    root_ = std::make_unique<node>();
    root_->loaded = true;
    if (fs_) {
        auto slash = std::make_unique<node>();
        slash->inode = fs::ufs2_filesystem::root_inode;
        slash->name = QStringLiteral("dev_hdd0");
        slash->dir = true;
        slash->parent = root_.get();
        root_->kids.push_back(std::move(slash));
    }
    endResetModel();
}

void ufs2_model::rebind(fs::ufs2_filesystem* fs, fs::ufs2_writer* writer) {
    fs_ = fs;
    writer_ = writer;
    cover_cache_.clear();
    tooltip_cache_.clear();
}

ufs2_model::node* ufs2_model::to_node(const QModelIndex& index) const {
    return index.isValid() ? static_cast<node*>(index.internalPointer()) : root_.get();
}

int ufs2_model::row_of(node* n) const {
    if (!n || !n->parent) return 0;
    const auto& sibs = n->parent->kids;
    for (int i = 0; i < static_cast<int>(sibs.size()); ++i)
        if (sibs[i].get() == n) return i;
    return 0;
}

ufs2_model::node* ufs2_model::child_named(node* n, const QString& name) const {
    if (!n) return nullptr;
    for (auto& c : n->kids)
        if (c->name == name) return c.get();
    return nullptr;
}

void ufs2_model::load_children(node* n) const {
    n->kids.clear();
    n->loaded = true;
    if (!fs_ || !n->dir) return;
    try {
        for (const auto& e : fs_->read_directory(fs_->read_inode(n->inode))) {
            if (e.name == "." || e.name == "..") continue;
            auto c = std::make_unique<node>();
            c->inode = e.inode_number;
            c->name = QString::fromStdString(e.name);
            c->dir = (e.type == fs::dirent_type::directory);
            c->parent = n;
            if (c->dir) {
                try {
                    bool any = false;
                    for (const auto& ce : fs_->read_directory(fs_->read_inode(c->inode))) {
                        if (ce.name == "." || ce.name == "..") continue;
                        any = true;
                        if (ce.name == "ICON0.PNG") { c->has_cover = true; c->cover_inode = ce.inode_number; }
                        else if (ce.name == "PARAM.SFO") c->sfo_inode = ce.inode_number;
                    }
                    c->has_kids_known = true;
                    c->has_kids = any;
                } catch (...) {
                }
            } else {
                try { c->size = fs_->read_inode(e.inode_number).size; } catch (...) {}
            }
            n->kids.push_back(std::move(c));
        }
    } catch (...) {
    }
    sort_kids(n);
}

void ufs2_model::set_show_covers(bool on) {
    if (show_covers_ == on) return;
    show_covers_ = on;
    emit layoutChanged();
}

void ufs2_model::set_show_tooltips(bool on) { show_tooltips_ = on; }

void ufs2_model::sort_kids(node* n) const {
    if (!n || sort_col_ < 0) return;
    const bool asc = sort_order_ == Qt::AscendingOrder;
    std::stable_sort(n->kids.begin(), n->kids.end(),
        [&](const std::unique_ptr<node>& a, const std::unique_ptr<node>& b) {
            if (a->dir != b->dir) return a->dir;
            bool less = sort_col_ == 2 ? a->size < b->size : a->name.compare(b->name, Qt::CaseInsensitive) < 0;
            return asc ? less : !less;
        });
}
void ufs2_model::sort(int column, Qt::SortOrder order) {
    sort_col_ = column;
    sort_order_ = order;
    emit layoutAboutToBeChanged();
    const QModelIndexList old = persistentIndexList();
    std::vector<node*> nodes;
    nodes.reserve(old.size());
    for (const QModelIndex& i : old) nodes.push_back(to_node(i));
    std::function<void(node*)> rec = [&](node* n) {
        if (!n) return;
        sort_kids(n);
        for (auto& c : n->kids) rec(c.get());
    };
    rec(root_.get());
    QModelIndexList updated;
    updated.reserve(old.size());
    for (int k = 0; k < old.size(); ++k) {
        node* n = nodes[k];
        if (!n || n == root_.get() || !n->parent)
            updated.append(QModelIndex());
        else
            updated.append(createIndex(row_of(n), old[k].column(), n));
    }
    changePersistentIndexList(old, updated);
    emit layoutChanged();
}

QModelIndex ufs2_model::index(int row, int column, const QModelIndex& parent) const {
    if (!hasIndex(row, column, parent)) return {};
    node* p = to_node(parent);
    if (!p || row < 0 || row >= static_cast<int>(p->kids.size())) return {};
    return createIndex(row, column, p->kids[row].get());
}

QModelIndex ufs2_model::parent(const QModelIndex& child) const {
    node* c = to_node(child);
    if (!c || !c->parent || c->parent == root_.get()) return {};
    return createIndex(row_of(c->parent), 0, c->parent);
}

int ufs2_model::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) return 0;
    node* p = to_node(parent);
    return p ? static_cast<int>(p->kids.size()) : 0;
}

int ufs2_model::columnCount(const QModelIndex&) const { return 3; }

bool ufs2_model::hasChildren(const QModelIndex& parent) const {
    node* p = to_node(parent);
    if (!p) return false;
    if (!p->dir && p->parent) return false;
    if (p->loaded) return !p->kids.empty();
    if (p->has_kids_known) return p->has_kids;
    return true;
}

bool ufs2_model::canFetchMore(const QModelIndex& parent) const {
    node* p = to_node(parent);
    if (!p || !p->dir || p->loaded) return false;
    if (p->has_kids_known && !p->has_kids) return false; // known empty, nothing 2 fetch
    return true;
}

void ufs2_model::fetchMore(const QModelIndex& parent) {
    node* p = to_node(parent);
    if (!p || p->loaded) return;
    load_children(p);
    if (!p->kids.empty()) {
        beginInsertRows(parent, 0, static_cast<int>(p->kids.size()) - 1);
        endInsertRows();
    }
}

QVariant ufs2_model::data(const QModelIndex& index, int role) const {
    node* n = to_node(index);
    if (!n || n == root_.get()) return {};
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case 0: return n->name;
            case 1: return n->dir ? QStringLiteral("Folder") : type_label(n->name);
            case 2: return n->dir ? QString() : QString::fromStdString(disk::format_size(static_cast<std::uint64_t>(n->size)));
        }
    } else if (role == Qt::DecorationRole && index.column() == 0) {
        if (n->dir) {
            if (show_covers_ && n->has_cover && fs_) {
                auto ci = cover_cache_.constFind(n->cover_inode);
                if (ci == cover_cache_.constEnd()) {
                    QIcon cover;
                    try {
                        const auto d = fs_->read_inode_data(fs_->read_inode(n->cover_inode));
                        QPixmap pm;
                        if (!d.empty() &&
                            pm.loadFromData(reinterpret_cast<const uchar*>(d.data()), static_cast<uint>(d.size())))
                            cover = QIcon(pm);
                    } catch (...) {
                    }
                    ci = cover_cache_.insert(n->cover_inode, cover);
                }
                if (!ci->isNull()) return *ci;
            }
            return dir_icon_;
        }
        const QString key = file_icon_key(n->name);
        auto it = icon_cache_.constFind(key);
        if (it == icon_cache_.constEnd())
            it = icon_cache_.insert(key, QIcon(QStringLiteral(":/icons/%1.bmp").arg(key)));
        return *it;
    } else if (role == Qt::ToolTipRole && show_tooltips_ && n->dir && n->sfo_inode && fs_) {
        auto ti = tooltip_cache_.constFind(n->inode);
        if (ti == tooltip_cache_.constEnd()) {
            QString tip;
            try {
                const auto kv = parse_sfo(fs_->read_inode_data(fs_->read_inode(n->sfo_inode)));
                const QString title = kv.value(QStringLiteral("TITLE"));
                const QString tid = kv.value(QStringLiteral("TITLE_ID"));
                const QString cat = category_name(kv.value(QStringLiteral("CATEGORY")));
                QString ver = kv.value(QStringLiteral("APP_VER"));
                if (ver.isEmpty()) ver = kv.value(QStringLiteral("VERSION"));
                const QString sub = kv.value(QStringLiteral("SUB_TITLE"));
                QStringList meta;
                meta << (tid.isEmpty() ? n->name : tid);
                if (!cat.isEmpty()) meta << cat;
                tip = QStringLiteral("<b>%1</b>").arg((title.isEmpty() ? n->name : title).toHtmlEscaped());
                tip += QStringLiteral("<br>%1").arg(meta.join(QStringLiteral("  ·  ")).toHtmlEscaped());
                if (!ver.isEmpty()) tip += QStringLiteral("<br>Version %1").arg(ver.toHtmlEscaped());
                if (!sub.isEmpty()) tip += QStringLiteral("<br>%1").arg(sub.toHtmlEscaped());
            } catch (...) {
            }
            ti = tooltip_cache_.insert(n->inode, tip);
        }
        if (!ti->isEmpty()) return *ti;
    }
    return {};
}

QVariant ufs2_model::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case 0: return QStringLiteral("Name");
        case 1: return QStringLiteral("Type");
        case 2: return QStringLiteral("Size");
    }
    return {};
}

Qt::ItemFlags ufs2_model::flags(const QModelIndex& index) const {
    if (!index.isValid())
        return writer_ ? Qt::ItemIsDropEnabled : Qt::NoItemFlags;
    node* n = to_node(index);
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    const bool is_slash = n && n->parent == root_.get();
    if (index.column() == 0 && writer_ && n && !is_slash) f |= Qt::ItemIsEditable;
    if (fs_ && n && !is_slash) f |= Qt::ItemIsDragEnabled;
    if (writer_ && n && n->dir) f |= Qt::ItemIsDropEnabled;
    return f;
}

bool ufs2_model::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (role != Qt::EditRole || index.column() != 0) return false;
    node* n = to_node(index);
    if (!n || !writer_ || !n->parent || n->parent == root_.get()) return false;
    const QString newname = value.toString().trimmed();
    if (newname.isEmpty() || newname == n->name) return false;
    try {
        writer_->move_entry(n->parent->inode, n->name.toStdString(), n->parent->inode, newname.toStdString());
        writer_->update_superblock();
    } catch (const std::exception& ex) {
        emit wrote(QStringLiteral("rename error: %1").arg(QString::fromUtf8(ex.what())));
        return false;
    }
    n->name = newname;
    emit dataChanged(index, index.siblingAtColumn(2));
    emit wrote(QStringLiteral("Renamed to %1").arg(newname));
    return true;
}

std::uint64_t ufs2_model::inode_of(const QModelIndex& index) const {
    node* n = to_node(index);
    return n ? n->inode : fs::ufs2_filesystem::root_inode;
}

std::uint64_t ufs2_model::parent_inode_of(const QModelIndex& index) const {
    node* n = to_node(index);
    return (n && n->parent) ? n->parent->inode : fs::ufs2_filesystem::root_inode;
}

bool ufs2_model::is_dir(const QModelIndex& index) const {
    node* n = to_node(index);
    return n && n->dir;
}

bool ufs2_model::is_root(const QModelIndex& index) const {
    node* n = to_node(index);
    return n && n->parent == root_.get();
}

QString ufs2_model::name_of(const QModelIndex& index) const {
    node* n = to_node(index);
    return n ? n->name : QString();
}

QModelIndex ufs2_model::root_index() const {
    if (!root_ || root_->kids.empty()) return {};
    return createIndex(0, 0, root_->kids[0].get());
}

QModelIndex ufs2_model::index_for_inode(std::uint64_t inode) const {
    return idx_for(find_by_inode(root_.get(), inode));
}

void ufs2_model::refresh(const QModelIndex& dir) {
    node* n = to_node(dir);
    if (!n) return;
    if (!n->kids.empty()) {
        beginRemoveRows(dir, 0, static_cast<int>(n->kids.size()) - 1);
        n->kids.clear();
        endRemoveRows();
    }
    n->loaded = false;
    load_children(n);
    if (!n->kids.empty()) {
        beginInsertRows(dir, 0, static_cast<int>(n->kids.size()) - 1);
        endInsertRows();
    }
}

void ufs2_model::note_child_added(const QModelIndex& dir, std::uint64_t inode, const QString& name, bool dir_flag) {
    node* n = to_node(dir);
    if (!n || !n->loaded) return; // if not loaded, it will read fresh on expand
    if (child_named(n, name)) { refresh(dir); return; } // replaced an existing entry
    const int row = static_cast<int>(n->kids.size());
    beginInsertRows(dir, row, row);
    auto c = std::make_unique<node>();
    c->inode = inode;
    c->name = name;
    c->dir = dir_flag;
    c->parent = n;
    if (!dir_flag && fs_) {
        try { c->size = fs_->read_inode(inode).size; } catch (...) {}
    }
    n->kids.push_back(std::move(c));
    endInsertRows();
}

void ufs2_model::note_child_removed(const QModelIndex& dir, const QString& name) {
    node* n = to_node(dir);
    if (!n || !n->loaded) return;
    for (int i = 0; i < static_cast<int>(n->kids.size()); ++i) {
        if (n->kids[i]->name == name) {
            beginRemoveRows(dir, i, i);
            n->kids.erase(n->kids.begin() + i);
            endRemoveRows();
            return;
        }
    }
}

Qt::DropActions ufs2_model::supportedDropActions() const { return Qt::MoveAction | Qt::CopyAction; }
Qt::DropActions ufs2_model::supportedDragActions() const { return Qt::MoveAction; }

QStringList ufs2_model::mimeTypes() const {
    return {kNodesMime, QStringLiteral("text/uri-list")};
}

QModelIndex ufs2_model::idx_for(node* n) const {
    if (!n || n == root_.get()) return {};
    return createIndex(row_of(n), 0, n);
}

ufs2_model::node* ufs2_model::find_by_inode(node* from, std::uint64_t inode) const {
    if (!from) return nullptr;
    if (from != root_.get() && from->inode == inode) return from;
    for (auto& c : from->kids)
        if (node* hit = find_by_inode(c.get(), inode)) return hit;
    return nullptr;
}

bool ufs2_model::is_ancestor(node* maybe_ancestor, node* n) const {
    for (node* p = n; p; p = p->parent)
        if (p == maybe_ancestor) return true;
    return false;
}

ufs2_model::node* ufs2_model::resolve_drop_dir(const QModelIndex& parent) const {
    node* dest = to_node(parent);
    if (dest == root_.get())
        dest = root_->kids.empty() ? nullptr : root_->kids[0].get();
    if (dest && !dest->dir) dest = dest->parent;
    if (dest == root_.get())
        dest = root_->kids.empty() ? nullptr : root_->kids[0].get();
    return (dest && dest->dir) ? dest : nullptr;
}

QMimeData* ufs2_model::mimeData(const QModelIndexList& indexes) const {
    QByteArray buf;
    QDataStream ds(&buf, QIODevice::WriteOnly);
    std::vector<drag_item> items;
    for (const QModelIndex& idx : indexes) {
        if (idx.column() != 0) continue;
        node* c = to_node(idx);
        if (!c || !c->parent || c->parent == root_.get()) continue;
        ds << quint64(c->parent->inode) << c->name;
        items.push_back({c->inode, c->name, c->dir});
    }
    if (items.empty()) return nullptr;
    return new drag_mime(fs_, buf, std::move(items));
}

bool ufs2_model::canDropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex& parent) const {
    if (!writer_) return false;
    node* dest = resolve_drop_dir(parent);
    if (!dest) return false;
    if (data->hasFormat(kNodesMime)) {
        QByteArray buf = data->data(kNodesMime);
        QDataStream ds(&buf, QIODevice::ReadOnly);
        while (!ds.atEnd()) {
            quint64 pinode = 0; QString name;
            ds >> pinode >> name;
            node* src_parent = find_by_inode(root_.get(), pinode);
            node* src = src_parent ? child_named(src_parent, name) : nullptr;
            if (src && src->dir && is_ancestor(src, dest)) return false;
        }
        return true;
    }
    return data->hasUrls();
}

bool ufs2_model::removeRows(int, int, const QModelIndex&) {
    return false;
}

bool ufs2_model::dropMimeData(const QMimeData* data, Qt::DropAction action, int, int, const QModelIndex& parent) {
    if (action == Qt::IgnoreAction) return true;
    if (!writer_) return false;
    node* dest = resolve_drop_dir(parent);
    if (!dest) return false;

    if (data->hasFormat(kNodesMime)) {
        QByteArray buf = data->data(kNodesMime);
        QDataStream ds(&buf, QIODevice::ReadOnly);
        int moved = 0;
        while (!ds.atEnd()) {
            quint64 pinode = 0; QString name;
            ds >> pinode >> name;
            node* src_parent = find_by_inode(root_.get(), pinode);
            node* src = src_parent ? child_named(src_parent, name) : nullptr;
            if (!src) continue;
            if (src_parent == dest) continue;
            if (src->dir && is_ancestor(src, dest)) continue;
            if (child_named(dest, name)) {
                emit wrote(QStringLiteral("skipped %1 (already exists in target)").arg(name));
                continue;
            }
            try {
                writer_->move_entry(src_parent->inode, name.toStdString(), dest->inode, name.toStdString());
            } catch (const std::exception& ex) {
                emit wrote(QStringLiteral("move error: %1").arg(QString::fromUtf8(ex.what())));
                continue;
            }
            const int r = row_of(src);
            beginRemoveRows(idx_for(src_parent), r, r);
            std::unique_ptr<node> owned = std::move(src_parent->kids[r]);
            src_parent->kids.erase(src_parent->kids.begin() + r);
            endRemoveRows();
            if (dest->loaded) {
                const int dr = static_cast<int>(dest->kids.size());
                beginInsertRows(idx_for(dest), dr, dr);
                owned->parent = dest;
                dest->kids.push_back(std::move(owned));
                endInsertRows();
            }
            ++moved;
        }
        if (moved == 0) return false;
        writer_->update_superblock();
        emit wrote(QStringLiteral("Moved %1 item(s)").arg(moved));
        return true;
    }

    if (data->hasUrls()) {
        QStringList paths;
        for (const QUrl& url : data->urls())
            if (url.isLocalFile()) paths << url.toLocalFile();
        if (paths.isEmpty()) return false;
        emit external_drop(paths, dest->inode);
        return true;
    }
    return false;
}

} // namespace ps3hdd::ui