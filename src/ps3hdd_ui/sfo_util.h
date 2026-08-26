#pragma once

#include <QMap>
#include <QString>

#include <cstddef>
#include <vector>

namespace ps3hdd::ui {

QMap<QString, QString> parse_sfo(const std::vector<std::byte>& data);

QString category_name(const QString& code);
struct sfo_field {
    QString key;
    quint16 fmt = 0x0204;
    QString value;
    quint32 max_len = 0;
};

std::vector<sfo_field> parse_sfo_fields(const std::vector<std::byte>& data);
std::vector<std::byte> serialize_sfo(const std::vector<sfo_field>& fields);

QString sfo_key_help(const QString& key);

} // namespace ps3hdd::ui