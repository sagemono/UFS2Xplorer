#pragma once

#include <QString>
#include <QVector>

namespace ps3hdd::ui {

struct key_entry {
    QString nickname;
    QString hex_key;
    QString encryption_type;
    QString serial;
    QString date_added;
    QString idps;
    QString account_id;
};

class key_store {
public:
    key_store();

    const QVector<key_entry>& entries() const { return entries_; }
    QString path() const { return path_; }
    QString key_for_serial(const QString& serial) const;
    key_entry entry_for_serial(const QString& serial) const;
    void add_or_update(const key_entry& e);
    void remove(const QString& hex_key);

private:
    void load();
    void save() const;

    QString path_;
    QVector<key_entry> entries_;
};

} // namespace ps3hdd::ui