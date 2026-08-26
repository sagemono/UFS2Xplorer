#include "sfo_util.h"

#include <QByteArray>

#include <algorithm>

namespace ps3hdd::ui {

QMap<QString, QString> parse_sfo(const std::vector<std::byte>& d) {
    QMap<QString, QString> out;
    auto byte = [&](std::size_t o) { return o < d.size() ? std::to_integer<quint32>(d[o]) : 0u; };
    auto u16 = [&](std::size_t o) { return byte(o) | (byte(o + 1) << 8); };
    auto u32 = [&](std::size_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (d.size() < 0x14 || byte(0) != 0x00 || byte(1) != 0x50 || byte(2) != 0x53 || byte(3) != 0x46)
        return out; // not "\0PSF"
    const quint32 key_start = u32(0x08), data_start = u32(0x0C), n = u32(0x10);
    for (quint32 i = 0; i < n; ++i) {
        const std::size_t base = 0x14 + static_cast<std::size_t>(i) * 16;
        if (base + 16 > d.size()) break;
        const quint32 key_off = u16(base), fmt = u16(base + 2), len = u32(base + 4), data_off = u32(base + 0x0C);
        std::string key;
        for (std::size_t k = key_start + key_off; k < d.size() && byte(k); ++k)
            key.push_back(static_cast<char>(byte(k)));
        QString value;
        const std::size_t dpos = data_start + data_off;
        if (fmt == 0x0404) {
            value = QString::number(u32(dpos));
        } else { // 0x0004 / 0x0204: utf8 str
            std::string s;
            for (std::size_t k = 0; k < len && dpos + k < d.size() && byte(dpos + k); ++k)
                s.push_back(static_cast<char>(byte(dpos + k)));
            value = QString::fromUtf8(s.c_str());
        }
        out.insert(QString::fromStdString(key), value);
    }
    return out;
}

std::vector<sfo_field> parse_sfo_fields(const std::vector<std::byte>& d) {
    std::vector<sfo_field> out;
    auto byte = [&](std::size_t o) { return o < d.size() ? std::to_integer<quint32>(d[o]) : 0u; };
    auto u16 = [&](std::size_t o) { return byte(o) | (byte(o + 1) << 8); };
    auto u32 = [&](std::size_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (d.size() < 0x14 || byte(0) != 0x00 || byte(1) != 0x50 || byte(2) != 0x53 || byte(3) != 0x46)
        return out;
    const quint32 key_start = u32(0x08), data_start = u32(0x0C), n = u32(0x10);
    for (quint32 i = 0; i < n; ++i) {
        const std::size_t base = 0x14 + static_cast<std::size_t>(i) * 16;
        if (base + 16 > d.size()) break;
        const quint32 key_off = u16(base), fmt = u16(base + 2), len = u32(base + 4), max_len = u32(base + 8), data_off = u32(base + 0x0C);
        sfo_field f;
        f.fmt = static_cast<quint16>(fmt);
        f.max_len = max_len;
        std::string key;
        for (std::size_t k = key_start + key_off; k < d.size() && byte(k); ++k)
            key.push_back(static_cast<char>(byte(k)));
        f.key = QString::fromStdString(key);
        const std::size_t dpos = data_start + data_off;
        if (fmt == 0x0404) {
            f.value = QString::number(u32(dpos));
        } else {
            std::string s;
            for (std::size_t k = 0; k < len && dpos + k < d.size() && byte(dpos + k); ++k)
                s.push_back(static_cast<char>(byte(dpos + k)));
            f.value = QString::fromUtf8(s.c_str());
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<std::byte> serialize_sfo(const std::vector<sfo_field>& in) {
    std::vector<sfo_field> fields = in;
    std::sort(fields.begin(), fields.end(), [](const sfo_field& a, const sfo_field& b) { return a.key < b.key; });

    auto round4 = [](quint32 v) { return (v + 3u) & ~3u; };

    //perfield data payloads and reserved slot sizes
    struct built { QByteArray data; quint32 len; quint32 slot; };
    std::vector<built> b;
    b.reserve(fields.size());
    for (const sfo_field& f : fields) {
        built x;
        if (f.fmt == 0x0404) {
            x.data.resize(4);
            const quint32 v = f.value.toUInt();
            for (int i = 0; i < 4; ++i) x.data[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
            x.len = 4;
            x.slot = std::max<quint32>(f.max_len, 4);
        } else {
            QByteArray utf8 = f.value.toUtf8();
            if (f.fmt != 0x0004) utf8.append('\0'); //nul termintatse
            x.data = utf8;
            x.len = static_cast<quint32>(utf8.size());
            x.slot = std::max<quint32>(f.max_len, round4(x.len));
        }
        b.push_back(std::move(x));
    }

    const quint32 n = static_cast<quint32>(fields.size());
    //key table
    QByteArray keytab;
    std::vector<quint32> key_off(n);
    for (quint32 i = 0; i < n; ++i) {
        key_off[i] = static_cast<quint32>(keytab.size());
        keytab.append(fields[i].key.toUtf8());
        keytab.append('\0');
    }
    while (keytab.size() % 4 != 0) keytab.append('\0');

    const quint32 index_start = 0x14;
    const quint32 key_start = index_start + n * 16;
    const quint32 data_start = key_start + static_cast<quint32>(keytab.size());

    auto put16 = [](QByteArray& o, quint16 v) {
        o.append(static_cast<char>(v & 0xFF));
        o.append(static_cast<char>((v >> 8) & 0xFF));
    };
    auto put32 = [](QByteArray& o, quint32 v) {
        for (int i = 0; i < 4; ++i) o.append(static_cast<char>((v >> (8 * i)) & 0xFF));
    };

    QByteArray out;
    // hdr
    out.append('\0'); out.append('P'); out.append('S'); out.append('F');
    put32(out, 0x00000101); // version 1.01
    put32(out, key_start);
    put32(out, data_start);
    put32(out, n);
    // idx table
    quint32 doff = 0;
    for (quint32 i = 0; i < n; ++i) {
        put16(out, static_cast<quint16>(key_off[i]));
        put16(out, fields[i].fmt);
        put32(out, b[i].len);
        put32(out, b[i].slot);
        put32(out, doff);
        doff += b[i].slot;
    }
    // key table
    out.append(keytab);
    // data table, each value padded to its slot
    for (quint32 i = 0; i < n; ++i) {
        out.append(b[i].data);
        for (quint32 p = b[i].len; p < b[i].slot; ++p) out.append('\0');
    }

    const auto* p = reinterpret_cast<const std::byte*>(out.constData());
    return std::vector<std::byte>(p, p + out.size());
}

QString sfo_key_help(const QString& key) {
    static const QMap<QString, QString> m = {
        {QStringLiteral("TITLE"), QStringLiteral("Display name shown on the XMB.")},
        {QStringLiteral("TITLE_ID"), QStringLiteral("Content title id, e.g. NPEA00252 / BLES01234.")},
        {QStringLiteral("CATEGORY"), QStringLiteral("Content type code (HG game, GD update, AC DLC, ...).")},
        {QStringLiteral("APP_VER"), QStringLiteral("Application version, e.g. 01.00.")},
        {QStringLiteral("VERSION"), QStringLiteral("Package/content version.")},
        {QStringLiteral("PARENTAL_LEVEL"), QStringLiteral("Parental control level (0-11); lower = less restricted.")},
        {QStringLiteral("RESOLUTION"), QStringLiteral("Supported video-mode bitfield (480/576/720/1080).")},
        {QStringLiteral("SOUND_FORMAT"), QStringLiteral("Supported audio-format bitfield (LPCM / Dolby / DTS).")},
        {QStringLiteral("ATTRIBUTE"), QStringLiteral("Feature/behaviour bitfield (warning screen, in-game XMB, remote play, ...).")},
        {QStringLiteral("REGION_DENY"), QStringLiteral("Region restriction bitfield.")},
        {QStringLiteral("BOOTABLE"), QStringLiteral("1 if the title can be launched.")},
        {QStringLiteral("PS3_SYSTEM_VER"), QStringLiteral("Minimum required firmware, e.g. 04.8000.")},
        {QStringLiteral("ACCOUNT_ID"), QStringLiteral("Owning PSN account id (save data ownership).")},
        {QStringLiteral("SAVEDATA_DIRECTORY"), QStringLiteral("Save-data folder name.")},
    };
    return m.value(key.trimmed().toUpper(), QString());
}

QString category_name(const QString& code) {
    static const QMap<QString, QString> m = {
        {QStringLiteral("HG"), QStringLiteral("Game")},
        {QStringLiteral("DG"), QStringLiteral("Disc Game")},
        {QStringLiteral("GD"), QStringLiteral("Game Data / DLC")},
        {QStringLiteral("AC"), QStringLiteral("DLC / Add-on")},
        {QStringLiteral("HM"), QStringLiteral("Home")},
        {QStringLiteral("AP"), QStringLiteral("App")},
        {QStringLiteral("AM"), QStringLiteral("App")},
        {QStringLiteral("AT"), QStringLiteral("TV / Video")},
        {QStringLiteral("AV"), QStringLiteral("Video")},
        {QStringLiteral("2P"), QStringLiteral("PS2 Classics")},
        {QStringLiteral("2G"), QStringLiteral("PS2 Game")},
        {QStringLiteral("2D"), QStringLiteral("PS2 Data")},
        {QStringLiteral("1P"), QStringLiteral("PS1 Classics")},
        {QStringLiteral("PP"), QStringLiteral("PSP")},
        {QStringLiteral("PE"), QStringLiteral("PSP")},
        {QStringLiteral("MN"), QStringLiteral("minis")},
        {QStringLiteral("SF"), QStringLiteral("Store")},
        {QStringLiteral("TZ"), QStringLiteral("Theme")},
        {QStringLiteral("WT"), QStringLiteral("Web TV")},
        {QStringLiteral("CB"), QStringLiteral("Network")},
        {QStringLiteral("SD"), QStringLiteral("Save Data")},
    };
    const QString c = code.trimmed().toUpper();
    if (c.isEmpty()) return QStringLiteral("-");
    return m.value(c, c);
}

} // namespace ps3hdd::ui