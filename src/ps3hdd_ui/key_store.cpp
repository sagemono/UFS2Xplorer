#include "key_store.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace ps3hdd::ui {

namespace {
QString clean_key(const QString& s) {
    return QString(s).remove('-').remove(' ').trimmed().toUpper();
}
} // namespace

key_store::key_store() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    path_ = dir + QStringLiteral("/eid_keys.json");
    // One-time carry-over from the pre-rename location (PS3HDDTool -> UFS2Xplorer) so
    // saved drive profiles are not lost by the rebrand.
    if (!QFile::exists(path_)) {
        const QString legacy =
            QString(dir).replace(QStringLiteral("UFS2Xplorer"), QStringLiteral("PS3HDDTool"))
            + QStringLiteral("/eid_keys.json");
        if (legacy != path_ && QFile::exists(legacy)) QFile::copy(legacy, path_);
    }
    load();
}

QString key_store::key_for_serial(const QString& serial) const {
    if (serial.isEmpty()) return {};
    QString found;
    for (const auto& e : entries_)
        if (e.serial == serial) found = e.hex_key;
    return found;
}

key_entry key_store::entry_for_serial(const QString& serial) const {
    key_entry found;
    if (serial.isEmpty()) return found;
    for (const auto& e : entries_)
        if (e.serial == serial) found = e;
    return found;
}

void key_store::add_or_update(const key_entry& in) {
    const QString key = clean_key(in.hex_key);
    for (auto& e : entries_) {
        if (clean_key(e.hex_key) == key) {
            e.nickname = in.nickname;
            if (!in.encryption_type.isEmpty()) e.encryption_type = in.encryption_type;
            if (!in.serial.isEmpty()) e.serial = in.serial;
            if (!in.idps.isEmpty()) e.idps = in.idps;
            if (!in.account_id.isEmpty()) e.account_id = in.account_id;
            save();
            return;
        }
    }
    entries_.push_back(in);
    save();
}

void key_store::remove(const QString& hex_key) {
    const QString key = clean_key(hex_key);
    const auto before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const key_entry& e) { return clean_key(e.hex_key) == key; }), entries_.end());
    if (entries_.size() != before) save();
}

void key_store::load() {
    entries_.clear();
    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;
    for (const auto v : doc.array()) {
        const auto o = v.toObject();
        key_entry e;
        e.nickname = o.value(QStringLiteral("Nickname")).toString();
        e.hex_key = o.value(QStringLiteral("HexKey")).toString();
        e.encryption_type = o.value(QStringLiteral("EncryptionType")).toString();
        e.serial = o.value(QStringLiteral("Serial")).toString();
        e.date_added = o.value(QStringLiteral("DateAdded")).toString();
        e.idps = o.value(QStringLiteral("Idps")).toString();
        e.account_id = o.value(QStringLiteral("AccountId")).toString();
        if (!e.hex_key.isEmpty()) entries_.push_back(e);
    }
}

void key_store::save() const {
    QJsonArray arr;
    for (const auto& e : entries_) {
        QJsonObject o;
        o[QStringLiteral("Nickname")] = e.nickname;
        o[QStringLiteral("HexKey")] = e.hex_key;
        o[QStringLiteral("EncryptionType")] = e.encryption_type;
        o[QStringLiteral("Serial")] = e.serial;
        o[QStringLiteral("DateAdded")] = e.date_added;
        if (!e.idps.isEmpty()) o[QStringLiteral("Idps")] = e.idps;
        if (!e.account_id.isEmpty()) o[QStringLiteral("AccountId")] = e.account_id;
        arr.push_back(o);
    }
    QFile f(path_);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}

} // namespace ps3hdd::ui