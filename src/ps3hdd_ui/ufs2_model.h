#pragma once

#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>

#include <QAbstractItemModel>
#include <QHash>
#include <QIcon>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <vector>

namespace ps3hdd::ui {

class ufs2_model : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit ufs2_model(QObject* parent = nullptr);
    void set_backend(fs::ufs2_filesystem* fs, fs::ufs2_writer* writer);
    void rebind(fs::ufs2_filesystem* fs, fs::ufs2_writer* writer);
    static const QString& nodes_mime();
    QModelIndex index(int row, int column, const QModelIndex& parent) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent) const override;
    int columnCount(const QModelIndex& parent) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    bool hasChildren(const QModelIndex& parent) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;
    Qt::DropActions supportedDropActions() const override;
    Qt::DropActions supportedDragActions() const override;
    QStringList mimeTypes() const override;
    QMimeData* mimeData(const QModelIndexList& indexes) const override;
    bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) override;
    bool removeRows(int row, int count, const QModelIndex& parent) override;

    std::uint64_t inode_of(const QModelIndex& index) const;
    std::uint64_t parent_inode_of(const QModelIndex& index) const;
    bool is_dir(const QModelIndex& index) const;
    bool is_root(const QModelIndex& index) const;
    QString name_of(const QModelIndex& index) const;
    QModelIndex root_index() const;
    QModelIndex index_for_inode(std::uint64_t inode) const;
    void refresh(const QModelIndex& dir);
    void set_show_covers(bool on);
    void set_show_tooltips(bool on);
    void note_child_added(const QModelIndex& dir, std::uint64_t inode, const QString& name, bool dir_flag);
    void note_child_removed(const QModelIndex& dir, const QString& name);

signals:
    void wrote(QString message);
    void external_drop(QStringList paths, quint64 dir_inode);

private:
    struct node {
        std::uint64_t inode = 0;
        QString name;
        bool dir = false;
        std::int64_t size = 0;
        node* parent = nullptr;
        bool loaded = false;
        bool has_kids_known = false;
        bool has_kids = false;
        bool has_cover = false;
        std::uint64_t cover_inode = 0;
        std::uint64_t sfo_inode = 0;
        std::vector<std::unique_ptr<node>> kids;
    };

    node* to_node(const QModelIndex& index) const;
    int row_of(node* n) const;
    void load_children(node* n) const;
    node* child_named(node* n, const QString& name) const;
    void sort_kids(node* n) const;
    QModelIndex idx_for(node* n) const;
    node* find_by_inode(node* from, std::uint64_t inode) const;
    node* resolve_drop_dir(const QModelIndex& parent) const;   
    bool is_ancestor(node* maybe_ancestor, node* n) const;     

    fs::ufs2_filesystem* fs_ = nullptr;
    fs::ufs2_writer* writer_ = nullptr;
    std::unique_ptr<node> root_;
    QIcon dir_icon_;
    QIcon file_icon_;
    mutable QHash<QString, QIcon> icon_cache_;
    mutable QHash<quint64, QIcon> cover_cache_;
    mutable QHash<quint64, QString> tooltip_cache_;
    int sort_col_ = 0;
    Qt::SortOrder sort_order_ = Qt::AscendingOrder;
    bool show_covers_ = true;
    bool show_tooltips_ = true;
};

} // namespace ps3hdd::ui