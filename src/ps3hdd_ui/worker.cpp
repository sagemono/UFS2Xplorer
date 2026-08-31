#include "worker.h"

#include "fs_util.h" // local_users, dir_size helpers

#include <ps3hdd_app/database.h>
#include <ps3hdd_app/gameos.h>
#include <ps3hdd_disk/disk_source.h> // disk::format_size
#include <ps3hdd_ipc/ipc_client.h>
#include <ps3hdd_fs/ufs2_checker.h>
#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>
#include <ps3hdd_license/activation.h>
#include <ps3hdd_license/ecdsa.h>
#include <ps3hdd_license/rap.h>
#include <ps3hdd_pkg/pkg_installer.h>
#include <ps3hdd_pkg/ps3_pkg_reader.h>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ps3hdd::ui {

namespace {

std::uint64_t bswap64(std::uint64_t v) {
    std::uint64_t r = 0;
    for (int i = 0; i < 8; ++i) { r = (r << 8) | (v & 0xFF); v >>= 8; }
    return r;
}

std::string unique_child_name(fs::ufs2_filesystem& ufs, std::uint64_t dir_inode, const std::string& base) {
    auto taken = [&](const std::string& n) {
        for (const auto& e : ufs.read_directory(ufs.read_inode(dir_inode)))
            if (e.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    const QString q = QString::fromStdString(base);
    const int dot = q.lastIndexOf('.');
    const QString stem = dot > 0 ? q.left(dot) : q;
    const QString ext = dot > 0 ? q.mid(dot) : QString();
    for (int i = 1;; ++i) {
        const QString cand = i == 1 ? QStringLiteral("%1 (copy)%2").arg(stem, ext) : QStringLiteral("%1 (copy %2)%3").arg(stem).arg(i).arg(ext);
        if (!taken(cand.toStdString())) return cand.toStdString();
    }
}

int count_files(fs::ufs2_filesystem& ufs, std::uint64_t inode, bool is_dir, int depth = 0) {
    if (depth > 128) return 0;
    if (!is_dir) return 1;
    int n = 0;
    for (const auto& e : ufs.read_directory(ufs.read_inode(inode))) {
        if (e.name == "." || e.name == "..") continue;
        n += count_files(ufs, e.inode_number, e.type == fs::dirent_type::directory, depth + 1);
    }
    return n;
}

qint64 count_bytes(fs::ufs2_filesystem& ufs, std::uint64_t inode, bool is_dir, int depth = 0) {
    if (depth > 128) return 0;
    if (!is_dir) return static_cast<qint64>(ufs.read_inode(inode).size);
    qint64 n = 0;
    for (const auto& e : ufs.read_directory(ufs.read_inode(inode))) {
        if (e.name == "." || e.name == "..") continue;
        n += count_bytes(ufs, e.inode_number, e.type == fs::dirent_type::directory, depth + 1);
    }
    return n;
}

void copy_tree(fs::ufs2_filesystem& ufs, fs::ufs2_writer& writer, std::uint64_t src_inode, bool is_dir, const std::string& name, std::uint64_t dst_dir, const std::function<void(qint64)>& on_bytes, int depth = 0) {
    if (depth > 128) throw std::runtime_error("copy aborted: directory nesting too deep");
    if (is_dir) {
        const std::uint64_t newdir = writer.create_directory(dst_dir, name);
        for (const auto& e : ufs.read_directory(ufs.read_inode(src_inode))) {
            if (e.name == "." || e.name == "..") continue;
            copy_tree(ufs, writer, e.inode_number, e.type == fs::dirent_type::directory, e.name, newdir, on_bytes, depth + 1);
        }
    } else {
        const auto src_in = ufs.read_inode(src_inode);
        const auto blocks = ufs.block_pointers(src_in);
        std::uint64_t pos = 0;
        writer.write_file(
            dst_dir, name, src_in.size,
            [&](std::span<std::byte> dst) {
                ufs.read_range(blocks, pos, dst);
                pos += dst.size();
            },
            on_bytes); // report @ disk write, not source read!
    }
}

void extract_tree(fs::ufs2_filesystem& ufs, std::uint64_t src_inode, bool is_dir, const QString& host_path, const std::function<void(qint64)>& on_bytes) {
    if (is_dir) {
        QDir().mkpath(host_path);
        for (const auto& e : ufs.read_directory(ufs.read_inode(src_inode))) {
            if (e.name == "." || e.name == "..") continue;
            extract_tree(ufs, e.inode_number, e.type == fs::dirent_type::directory, host_path + QStringLiteral("/") + QString::fromStdString(e.name), on_bytes);
        }
    } else {
        QFile f(host_path);
        if (!f.open(QIODevice::WriteOnly)) throw std::runtime_error("cannot write " + host_path.toStdString());
        ufs.extract_inode(ufs.read_inode(src_inode), [&](std::span<const std::byte> chunk) {
            f.write(reinterpret_cast<const char*>(chunk.data()), static_cast<qint64>(chunk.size()));
            if (on_bytes) on_bytes(static_cast<qint64>(chunk.size()));
        });
    }
}

int count_host_files(const QString& path) {
    QFileInfo fi(path);
    if (!fi.isDir()) return 1;
    int n = 0;
    for (const QFileInfo& c :
         QDir(path).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden))
        n += count_host_files(c.absoluteFilePath());
    return n;
}

qint64 count_host_bytes(const QString& path) {
    QFileInfo fi(path);
    if (!fi.isDir()) return fi.size();
    qint64 n = 0;
    for (const QFileInfo& c : QDir(path).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden))
        n += count_host_bytes(c.absoluteFilePath());
    return n;
}

void stream_write(fs::ufs2_writer& writer, std::uint64_t dst_dir, const std::string& name, QFile& f, const std::function<void(qint64)>& on_bytes) {
    writer.write_file( /// report as they hit the disk instead of the read speed of the file]
        dst_dir, name, static_cast<std::int64_t>(f.size()),
        [&f](std::span<std::byte> dst) {
            qint64 got = 0;
            const qint64 want = static_cast<qint64>(dst.size());
            while (got < want) {
                const qint64 n = f.read(reinterpret_cast<char*>(dst.data()) + got, want - got);
                if (n <= 0) break;
                got += n;
            }
        },
        on_bytes);
}

void import_contents(fs::ufs2_filesystem& ufs, fs::ufs2_writer& writer, const QString& host_dir, std::uint64_t dst_dir, const std::function<void(qint64)>& on_bytes, int depth = 0) {
    if (depth > 128) throw std::runtime_error("import aborted: directory nesting too deep");
    std::map<std::string, std::pair<std::uint64_t, bool>> existing; // name -> (inode, is_dir)
    for (const auto& e : ufs.read_directory(ufs.read_inode(dst_dir)))
        if (e.name != "." && e.name != "..")
            existing[e.name] = {e.inode_number, e.type == fs::dirent_type::directory};
    for (const QFileInfo& child :
         QDir(host_dir).entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden)) {
        const std::string name = child.fileName().toStdString();
        if (child.isDir()) {
            std::uint64_t sub;
            auto it = existing.find(name);
            if (it != existing.end() && it->second.second) {
                sub = it->second.first;
            } else {
                if (it != existing.end()) writer.delete_tree(dst_dir, name);
                sub = writer.create_directory(dst_dir, name);
            }
            import_contents(ufs, writer, child.absoluteFilePath(), sub, on_bytes, depth + 1);
        } else {
            if (existing.count(name)) writer.delete_tree(dst_dir, name);
            QFile f(child.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly)) continue;
            stream_write(writer, dst_dir, name, f, on_bytes);
        }
    }
}

void import_path(fs::ufs2_filesystem& ufs, fs::ufs2_writer& writer, const QString& host_path, std::uint64_t dst_dir, const std::function<void(qint64)>& on_bytes) {
    QFileInfo fi(host_path);
    const std::string name = fi.fileName().toStdString();
    if (fi.isDir()) {
        std::uint64_t sub = 0;
        bool found = false;
        for (const auto& e : ufs.read_directory(ufs.read_inode(dst_dir)))
            if (e.name == name && e.type == fs::dirent_type::directory) { sub = e.inode_number; found = true; break; }
        if (!found) sub = writer.create_directory(dst_dir, name);
        import_contents(ufs, writer, host_path, sub, on_bytes);
    } else {
        for (const auto& e : ufs.read_directory(ufs.read_inode(dst_dir)))
            if (e.name == name) { writer.delete_tree(dst_dir, name); break; }
        QFile f(host_path);
        if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot read " + host_path.toStdString());
        stream_write(writer, dst_dir, name, f, on_bytes);
    }
}

bool consistency_ok(fs::ufs2_filesystem& check, disk::disk_source& dec, QString& msg) {
    const auto rep = fs::check_consistency(check, dec);
    msg = QStringLiteral("consistency: cross-links=%1 out-of-range=%2 used-but-free=%3 summary-mismatches=%4").arg(rep.cross_links).arg(rep.out_of_range).arg(rep.used_but_free).arg(rep.summary_mismatches);
    return rep.clean();
}

bool consistency_ok(const app::gameos_mount& m, QString& msg) {
    fs::ufs2_filesystem check(*m.decrypted, m.partition_sector);
    check.mount();
    return consistency_ok(check, *m.decrypted, msg);
}

QStringList local_users_q(fs::ufs2_filesystem& ufs) {
    QStringList out;
    for (const std::string& u : local_users(ufs)) out << QString::fromStdString(u);
    return out;
}

std::vector<std::byte> find_any_act_dat(fs::ufs2_filesystem& ufs) {
    if (auto home = ufs.resolve_path_to_inode_number("home"))
        for (const auto& e : ufs.read_directory(ufs.read_inode(*home))) {
            if (e.type != fs::dirent_type::directory || e.name == "." || e.name == "..") continue;
            if (auto a = ufs.resolve_path("home/" + e.name + "/exdata/act.dat")) {
                auto d = ufs.read_inode_data(*a);
                if (d.size() == 0x1038) return d;
            }
        }
    return {};
}

bool invalidate_db(fs::ufs2_filesystem& ufs, fs::ufs2_writer& writer, QString& msg) {
    const int n = app::invalidate_content_database(ufs, writer);
    if (n < 0) { msg = QStringLiteral("/dev_hdd0/mms not found"); return false; }
    msg = n == 0 ? QStringLiteral("mms/db.err already flags a rebuild; left as-is") : QStringLiteral("set mms/db.err rebuild flag; the console will re-index on next boot");
    return true;
}

} // namespace

void worker::run() {
    ps3hdd::ipc::broker_client broker;
    try {
        if (job_.file_operation == job::fop_none && job_.type == job::verify_pkg) {
            run_verify_pkg();
            return;
        }

        const bool writable = job_.file_operation != job::fop_none ? job_.file_operation != job::fop_extract : job_.type != job::consistency;
        emit progress(QStringLiteral("Connecting to the disk helper ..."), -1);
        broker.connect_to(job_.broker_port, job_.broker_token, 8000);
        emit progress(QStringLiteral("Opening %1 ...").arg(job_.device), -1);
        const auto info = broker.open(job_.device, writable);
        if (writable && !info.can_write) {
            emit finished(false, QStringLiteral("Could not open the disk for writing (it may be locked by another process)."));
            return;
        }
        auto src = std::make_shared<ps3hdd::ipc::ipc_disk_source>(broker, info);
        auto m = app::open_gameos(src, {job_.eid.data(), job_.eid.size()});
        if (!m) { emit finished(false, QStringLiteral("Could not locate/mount the GameOS partition.")); return; }
        emit progress(QStringLiteral("GameOS mounted (%1).").arg(QString::fromStdString(m->cipher)), -1);

        fs::ufs2_filesystem ufs(*m->decrypted, m->partition_sector);
        if (!ufs.mount()) { emit finished(false, QStringLiteral("mount failed")); return; }

        if (job_.file_operation != job::fop_none) { run_file_operation(*m, ufs); return; }
        if (job_.type == job::consistency) { run_consistency(*m, ufs); return; }
        if (job_.type == job::repair_counts) { run_repair_counts(*m, ufs); return; }
        if (job_.type == job::rebuild_database) { run_rebuild_database(*m, ufs); return; }
        if (job_.type == job::restore_db) { run_restore_db(*m, ufs); return; }
        if (job_.type == job::install_pkg) { run_install_pkg(*m, ufs); return; }
        if (job_.type == job::license_batch) { run_license_batch(*m, ufs); return; }
        if (job_.type == job::sync_exdata) { run_sync_exdata(*m, ufs); return; }
        run_license_single(*m, ufs);
    } catch (const std::exception& ex) {
        if (cancel_.load())
            emit finished(false, QStringLiteral("Cancelled. The disk may be partially written - run Check Consistency, or Restore DB from a backup, before booting."));
        else
            emit finished(false, QStringLiteral("Error: %1").arg(QString::fromUtf8(ex.what())));
    }
}

void worker::run_verify_pkg() {
    try {
        QFile f(job_.pkg_path);
        if (!f.open(QIODevice::ReadOnly)) {
            emit finished(false, QStringLiteral("Cannot open the package file."));
            return;
        }
        const qint64 sz = f.size();
        auto pkg = pkg::ps3_pkg_reader::from_file(job_.pkg_path.toStdString());
        const int files = static_cast<int>(pkg.entries().size());
        const QString cid = QString::fromStdString(pkg.content_id());

        const QByteArray zero8(8, '\0');
        auto is8 = [](const QByteArray& a) { return a.size() == 8; };

        QString header_note = QStringLiteral("header SHA-1 n/a");
        QString sig_note = QStringLiteral("signature n/a");
        bool header_bad = false, sig_bad = false;
        f.seek(0);
        const QByteArray hdr = f.read(0xC0);
        if (hdr.size() >= 0xC0) {
            const QByteArray dg = QCryptographicHash::hash(hdr.left(0x80), QCryptographicHash::Sha1);
            const QByteArray stored = hdr.mid(0xB8, 8);
            if (stored != zero8) {
                header_bad = is8(stored) && dg.right(8) != stored;
                header_note = header_bad ? QStringLiteral("header SHA-1 MISMATCH") : QStringLiteral("header SHA-1 OK");
            }

            bool sig_present = false;
            for (int i = 0x90; i < 0xB8; ++i)
                if (hdr[i] != '\0') { sig_present = true; break; }
            if (sig_present && dg.size() == 20) {
                std::array<std::uint8_t, 21> r{}, s{};
                for (int i = 0; i < 20; ++i) {
                    r[1 + i] = static_cast<std::uint8_t>(hdr[0x90 + i]);
                    s[1 + i] = static_cast<std::uint8_t>(hdr[0xA4 + i]);
                }
                const bool ok = license::ps3_pkg_ecdsa_verify(
                    reinterpret_cast<const std::uint8_t*>(dg.constData()), r.data(), s.data());
                sig_bad = !ok;
                sig_note = ok ? QStringLiteral("signature VALID (Sony NPDRM)") : QStringLiteral("signature INVALID (not Sony signed)");
            }
        }

        QString tail_note = QStringLiteral("not truncated");
        if (!job_.verify_quick) {
            f.seek(0);
            qint64 done = 0;
            const qint64 chunk = 1 << 20;
            while (done < sz) {
                if (cancel_.load()) { emit finished(false, QStringLiteral("Verification cancelled.")); return; }
                const QByteArray b = f.read(chunk);
                if (b.isEmpty()) break;
                done += b.size();
                emit progress(QStringLiteral("Reading %1 / %2 ...").arg(QString::fromStdString(disk::format_size(static_cast<std::uint64_t>(done))), QString::fromStdString(disk::format_size(static_cast<std::uint64_t>(sz)))), sz > 0 ? static_cast<int>(100LL * done / sz) : -1);
            }
            if (done < sz) {
                emit finished(false, QStringLiteral("Package is truncated or unreadable (%1 of %2 read).").arg(QString::fromStdString(disk::format_size(static_cast<std::uint64_t>(done))), QString::fromStdString(disk::format_size(static_cast<std::uint64_t>(sz)))));
                return;
            }
        } else {
            tail_note = QStringLiteral("header+signature (quick)");
        }

        if (header_bad || sig_bad) {
            emit finished(false, QStringLiteral("FAILED: %1, %2 (content %3). The package is damaged, tampered, or not genuinely Sony-signed; do not install it.").arg(header_note, sig_note, cid));
            return;
        }
        emit finished(true, QStringLiteral("Package OK: %1 file(s), %2, content %3.  %4; %5; %6.").arg(QString::number(files), QString::fromStdString(disk::format_size(static_cast<std::uint64_t>(sz))), cid, sig_note, header_note, tail_note));
    } catch (const std::exception& ex) {
        emit finished(false, QStringLiteral("Not a valid/complete package: %1").arg(QString::fromUtf8(ex.what())));
    }
}

void worker::run_file_operation(app::gameos_mount& m, fs::ufs2_filesystem& ufs) {
    fs::ufs2_writer writer(ufs, *m.decrypted);
    int done = 0, skipped = 0;

    QElapsedTimer clock;
    clock.start();
    qint64 total = 0, bytes_done = 0, last_bytes = 0, last_ms = 0;
    int last_pct = -1;
    double peak_bps = 0.0;
    auto report = [&](qint64 n) {
        bytes_done += n;
        const int pct = total > 0 ? static_cast<int>(100 * bytes_done / total) : 100;
        const qint64 now = clock.elapsed();
        if (pct != last_pct || now - last_ms >= 400) {
            const double bps = now > last_ms ? (bytes_done - last_bytes) * 1000.0 / (now - last_ms) : 0.0;
            if (bps > peak_bps) peak_bps = bps;
            last_pct = pct;
            last_bytes = bytes_done;
            last_ms = now;
            emit progress({}, pct);
            emit speed(bps);
        }
    };

    switch (job_.file_operation) {
    case job::fop_delete: {
        const int total = static_cast<int>(job_.fop_items.size());
        for (const auto& it : job_.fop_items) {
            if (cancel_.load()) break;
            emit progress(QStringLiteral("Deleting %1 ...").arg(it.name), total > 0 ? 100 * done / total : -1);
            writer.delete_tree(it.parent, it.name.toStdString());
            ++done;
            emit progress({}, total > 0 ? 100 * done / total : 100);
        }
        break;
    }
    case job::fop_move: {
        const int total = static_cast<int>(job_.fop_items.size());
        for (const auto& it : job_.fop_items) {
            if (cancel_.load()) break;
            if (it.parent != job_.fop_dest) {
                bool exists = false;
                for (const auto& e : ufs.read_directory(ufs.read_inode(job_.fop_dest)))
                    if (e.name == it.name.toStdString()) { exists = true; break; }
                if (exists) { ++skipped; continue; }
                emit progress(QStringLiteral("Moving %1 ...").arg(it.name), total > 0 ? 100 * done / total : -1);
                writer.move_entry(it.parent, it.name.toStdString(), job_.fop_dest, it.name.toStdString());
            }
            ++done;
            emit progress({}, total > 0 ? 100 * done / total : 100);
        }
        break;
    }
    case job::fop_copy: {
        emit progress(QStringLiteral("Scanning ..."), -1);
        for (const auto& it : job_.fop_items) total += count_bytes(ufs, it.inode, it.is_dir);
        for (const auto& it : job_.fop_items) {
            if (cancel_.load()) break;
            emit progress(QStringLiteral("Copying %1 ...").arg(it.name),
                          total > 0 ? static_cast<int>(100 * bytes_done / total) : -1);
            copy_tree(ufs, writer, it.inode, it.is_dir, unique_child_name(ufs, job_.fop_dest, it.name.toStdString()), job_.fop_dest, report);
            ++done;
        }
        break;
    }
    case job::fop_import: {
        emit progress(QStringLiteral("Scanning ..."), -1);
        for (const QString& p : job_.fop_import_paths) total += count_host_bytes(p);
        for (const QString& path : job_.fop_import_paths) {
            if (cancel_.load()) break;
            emit progress(QStringLiteral("Importing %1 ...").arg(QFileInfo(path).fileName()),
                          total > 0 ? static_cast<int>(100 * bytes_done / total) : -1);
            try {
                import_path(ufs, writer, path, job_.fop_dest, report);
                ++done;
            } catch (const std::exception&) {
                ++skipped;
            }
        }
        break;
    }
    case job::fop_extract: {
        emit progress(QStringLiteral("Scanning ..."), -1);
        for (const auto& it : job_.fop_items) total += count_bytes(ufs, it.inode, it.is_dir);
        for (const auto& it : job_.fop_items) {
            if (cancel_.load()) break;
            emit progress(QStringLiteral("Extracting %1 ...").arg(it.name),
                          total > 0 ? static_cast<int>(100 * bytes_done / total) : -1);
            const QString dest =
                it.is_dir ? job_.fop_host_dest + QStringLiteral("/") + it.name : job_.fop_host_dest;
            extract_tree(ufs, it.inode, it.is_dir, dest, report);
            ++done;
        }
        break;
    }
    default:
        break;
    }
    emit progress(QStringLiteral("Updating free-space summary ..."), -1);
    writer.update_superblock();
    QString extra;
    if (job_.set_rebuild && !cancel_.load()) {
        QString dbmsg;
        if (invalidate_db(ufs, writer, dbmsg)) {
            writer.update_superblock();
            extra = QStringLiteral("  |  %1").arg(dbmsg);
        }
    }

    QString xfer;
    if (bytes_done > 0) {
        const qint64 ms = clock.elapsed();
        const double avg = ms > 0 ? bytes_done * 1000.0 / ms : 0.0;
        const double secs = ms / 1000.0;
        const QString dur = secs < 60 ? QStringLiteral("%1 s").arg(secs, 0, 'f', 1) : QStringLiteral("%1m %2s").arg(static_cast<int>(secs) / 60).arg(static_cast<int>(secs) % 60);
        auto sz = [](double b) { return QString::fromStdString(disk::format_size(static_cast<std::uint64_t>(b))); };
        xfer = QStringLiteral("  |  %1 in %2 (avg %3/s, peak %4/s)").arg(sz(static_cast<double>(bytes_done)), dur, sz(avg), sz(peak_bps));
    }
    const bool cancelled = cancel_.load();
    QString msg = QStringLiteral("%1 %2 item(s)%3%4%5").arg(cancelled ? QStringLiteral("Cancelled after") : QStringLiteral("Done:")).arg(done).arg(skipped ? QStringLiteral(" (%1 skipped)").arg(skipped) : QString()).arg(xfer).arg(extra);
    emit finished(true, msg);
    return;
}

void worker::run_consistency(app::gameos_mount& m, fs::ufs2_filesystem& ufs) {
    const auto rep = fs::check_consistency(ufs, *m.decrypted);
    for (const auto& f : rep.findings) emit progress(QString::fromStdString(f), -1);
    const QString msg =
        QStringLiteral("cross-links=%1 out-of-range=%2 used-but-free=%3 summary-mismatches=%4").arg(rep.cross_links).arg(rep.out_of_range).arg(rep.used_but_free).arg(rep.summary_mismatches);
    emit finished(rep.clean(), msg + (rep.clean() ? QStringLiteral("  -> CLEAN") : QStringLiteral("  -> INCONSISTENT")));
    return;
}

void worker::run_repair_counts(app::gameos_mount& m, fs::ufs2_filesystem& ufs) {
    fs::ufs2_writer writer(ufs, *m.decrypted);
    emit progress(QStringLiteral("Recomputing free-space counts from the bitmaps ..."), 0);
    const int fixed = writer.repair_free_counts([this](int done, int total) {
        emit progress(QStringLiteral("Repairing cylinder group %1/%2").arg(done).arg(total), total > 0 ? static_cast<int>(100LL * done / total) : -1);
    });
    QString cmsg;
    const bool okc = consistency_ok(m, cmsg);
    emit finished(okc, QStringLiteral("Repaired %1 cylinder group(s).  |  %2").arg(fixed).arg(cmsg));
    return;
}

void worker::run_rebuild_database(app::gameos_mount& m, fs::ufs2_filesystem& ufs) {
    fs::ufs2_writer writer(ufs, *m.decrypted);
    QString dbmsg;
    const bool okdb = invalidate_db(ufs, writer, dbmsg);
    writer.update_superblock();
    emit finished(okdb, dbmsg);
    return;
}

void worker::run_restore_db(app::gameos_mount& m, fs::ufs2_filesystem& ufs) {
    fs::ufs2_writer writer(ufs, *m.decrypted);
    auto dbdir = ufs.resolve_path_to_inode_number("mms/db");
    if (!dbdir) {
        auto mmsdir = ufs.resolve_path_to_inode_number("mms");
        if (!mmsdir) { emit finished(false, QStringLiteral("no mms directory on GameOS")); return; }
        writer.create_directory(*mmsdir, "db");
        dbdir = ufs.resolve_path_to_inode_number("mms/db");
    }
    if (!dbdir) { emit finished(false, QStringLiteral("failed to create mms/db")); return; }

    QDir srcdir(job_.host_dir);
    const QStringList files = srcdir.entryList(QDir::Files, QDir::Name);
    if (files.isEmpty()) { emit finished(false, QStringLiteral("No files to restore in the chosen folder.")); return; }

    int done = 0;
    for (const QString& name : files) {
        if (cancel_.load()) throw std::runtime_error("cancelled");
        QFile f(srcdir.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) { emit progress(QStringLiteral("skip (cannot read): %1").arg(name), -1); continue; }
        const QByteArray bytes = f.readAll();
        const auto* p = reinterpret_cast<const std::byte*>(bytes.constData());
        std::vector<std::byte> data(p, p + bytes.size());
        const std::string nm = name.toStdString();
        for (const auto& e : ufs.read_directory(ufs.read_inode(*dbdir)))
            if (e.name == nm) { writer.delete_tree(*dbdir, nm); break; }
        writer.write_file(*dbdir, nm, data);
        emit progress(QStringLiteral("restored mms/db/%1 (%2 bytes)").arg(name).arg(bytes.size()), ++done * 100 / files.size());
    }
    writer.update_superblock();
    QString msg;
    const bool okc = consistency_ok(m, msg);
    emit finished(okc, QStringLiteral("Restored %1 file(s) to mms/db. %2").arg(done).arg(msg));
    return;
}

void worker::run_install_pkg(app::gameos_mount& m, fs::ufs2_filesystem& ufs) {
    auto pkg = pkg::ps3_pkg_reader::from_file(job_.pkg_path.toStdString());
    emit progress(QStringLiteral("Installing %1 (%2) ...").arg(QString::fromStdString(pkg.content_id()), QString::fromStdString(pkg.title_id())), 0);
    fs::ufs2_writer writer(ufs, *m.decrypted);
    pkg::pkg_installer installer(ufs, writer, pkg);
    const std::string path = installer.install([&](const std::string& name, int done, int total) {
        if (cancel_.load()) throw std::runtime_error("cancelled");
        const QString line = total > 0 ? QStringLiteral("[%1/%2] %3").arg(done).arg(total).arg(QString::fromStdString(name)) : QString::fromStdString(name);
        emit progress(line, total > 0 ? done * 100 / total : -1);
    });
    if (job_.rebuild_db) {
        QString dbmsg;
        invalidate_db(ufs, writer, dbmsg);
        writer.update_superblock();
        emit progress(QStringLiteral("database: %1").arg(dbmsg), -1);
    }

    if (job_.skip_consistency) {
        emit finished(true, QStringLiteral("Installed to %1.  (batch: verification deferred)").arg(QString::fromStdString(path)));
        return;
    }

    auto recheck = [&] {
        fs::ufs2_filesystem c(*m.decrypted, m.partition_sector); c.mount();
        return fs::check_consistency(c, *m.decrypted);
    };
    auto rep = recheck();
    if (!rep.clean() && rep.cross_links == 0 && rep.out_of_range == 0 &&
        rep.used_but_free == 0 && rep.summary_mismatches > 0) {
        emit progress(QStringLiteral("Auto-repairing free-space counts (%1 summary-mismatch(es)) ...").arg(rep.summary_mismatches), -1);
        const int fixed = writer.repair_free_counts({});
        emit progress(QStringLiteral("Repaired %1 cylinder group(s).").arg(fixed), -1);
        rep = recheck();
    }
    const QString msg =
        QStringLiteral("consistency: cross-links=%1 out-of-range=%2 used-but-free=%3 summary-mismatches=%4").arg(rep.cross_links).arg(rep.out_of_range).arg(rep.used_but_free).arg(rep.summary_mismatches);
    if (!rep.clean()) {
        emit finished(false, QStringLiteral("Installed to %1 BUT %2 - do NOT boot; restore the disk.").arg(QString::fromStdString(path), msg));
        return;
    }
    const QString tail = job_.rebuild_db ? QStringLiteral("  Rebuild flag set (mms/db.err) - just reboot; no safe mode needed.") : QStringLiteral("  On the console: Safe Mode -> Rebuild Database to list the game.");
    emit finished(true, QStringLiteral("Installed to %1. %2%3").arg(QString::fromStdString(path), msg, tail));
    return;
}

void worker::run_license_batch(app::gameos_mount& m, fs::ufs2_filesystem& ufs) {
    fs::ufs2_writer writer(ufs, *m.decrypted);

    QStringList users = job_.exdata_users;
    if (users.isEmpty()) {
        users = local_users_q(ufs);
        if (users.isEmpty()) users << QStringLiteral("00000001");
    }

    std::vector<std::byte> actbytes = find_any_act_dat(ufs);
    if (actbytes.size() != 0x1038) {
        emit finished(false, QStringLiteral("No act.dat on any user; run Full Activation on one title first, then batch-install the rest."));
        return;
    }

    struct target { QString user; std::uint64_t exinode; };
    std::vector<target> targets;
    for (const QString& u : users) {
        const std::string udir = "home/" + u.toStdString();
        auto uino = ufs.resolve_path_to_inode_number(udir);
        if (!uino) continue; // that user does not exist
        std::uint64_t exino = 0;
        if (auto ex = ufs.resolve_path_to_inode_number(udir + "/exdata")) {
            exino = *ex;
        } else {
            exino = writer.create_directory(*uino, "exdata"); // returns the new inode
        }
        if (!ufs.resolve_path(udir + "/exdata/act.dat")) {
            std::vector<std::byte> av(actbytes);
            writer.write_file(exino, "act.dat", av); // seed act.dat for a fresh user
        }
        targets.push_back({u, exino});
    }
    if (targets.empty()) { emit finished(false, QStringLiteral("No valid users found under home/.")); return; }

    const int total = static_cast<int>(job_.rap_paths.size());
    int done = 0, ok = 0, skipped = 0;
    for (const QString& rp : job_.rap_paths) {
        if (cancel_.load()) break;
        ++done;
        QString name = QFileInfo(rp).fileName();
        if (name.endsWith(QStringLiteral(".rap"), Qt::CaseInsensitive)) name.chop(4);
        emit progress(QStringLiteral("[%1/%2] %3").arg(done).arg(total).arg(name), total > 0 ? 100 * done / total : -1);
        QFile f(rp);
        if (!f.open(QIODevice::ReadOnly)) { ++skipped; continue; }
        const QByteArray rb = f.read(64);
        if (rb.size() != 16) { ++skipped; continue; } // free/empty rap -> no license needed
        std::vector<std::byte> rap;
        for (char c : rb) rap.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
        const auto klic = license::rap_to_klicensee({rap.data(), rap.size()});
        const auto rif = license::build_rif({job_.idps.data(), job_.idps.size()}, name.toStdString(), {klic.data(), klic.size()}, {actbytes.data(), actbytes.size()});
        const std::string fname = name.toStdString() + ".rif";
        std::vector<std::byte> rv(rif.begin(), rif.end());
        for (const target& t : targets) {
            if (ufs.resolve_path("home/" + t.user.toStdString() + "/exdata/" + fname))
                writer.delete_file(t.exinode, fname);
            writer.write_file(t.exinode, fname, rv);
        }
        ++ok;
    }
    writer.update_superblock(); // small writes; no full fsck
    emit finished(true, QStringLiteral("Installed %1 license(s)%2 for %3 user(s).  Rebuild Database on the console to pick up DLC.").arg(ok).arg(skipped ? QStringLiteral(", skipped %1 (free/empty .rap)").arg(skipped) : QString()).arg(targets.size()));
    return;
}


void worker::run_sync_exdata(app::gameos_mount& m, fs::ufs2_filesystem& ufs) {
    fs::ufs2_writer writer(ufs, *m.decrypted);
    QStringList users = local_users_q(ufs);
    if (users.size() < 2) { emit finished(true, QStringLiteral("Only one local user - nothing to sync.")); return; }

    QString src;
    std::uint64_t src_ino = 0;
    int best = -1;
    for (const QString& u : users) {
        auto ex = ufs.resolve_path_to_inode_number("home/" + u.toStdString() + "/exdata");
        if (!ex) continue;
        int cnt = 0;
        bool act = false;
        for (const auto& e : ufs.read_directory(ufs.read_inode(*ex))) {
            if (e.type == fs::dirent_type::directory || e.name == "." || e.name == "..") continue;
            ++cnt;
            if (e.name == "act.dat") act = true;
        }
        const int score = cnt + (act ? 100000 : 0);
        if (score > best) { best = score; src = u; src_ino = *ex; }
    }
    if (src.isEmpty()) { emit finished(false, QStringLiteral("No user has an exdata folder to copy from.")); return; }

    struct exfile { std::string name; std::vector<std::byte> data; };
    std::vector<exfile> files;
    for (const auto& e : ufs.read_directory(ufs.read_inode(src_ino))) {
        if (e.type == fs::dirent_type::directory || e.name == "." || e.name == "..") continue;
        files.push_back({e.name, ufs.read_inode_data(ufs.read_inode(e.inode_number))});
    }

    int copied = 0, udone = 0;
    for (const QString& u : users) {
        if (u == src) continue;
        if (cancel_.load()) break;
        const std::string udir = "home/" + u.toStdString();
        auto uino = ufs.resolve_path_to_inode_number(udir);
        if (!uino) continue;
        std::uint64_t exino = 0;
        if (auto ex = ufs.resolve_path_to_inode_number(udir + "/exdata")) exino = *ex;
        else exino = writer.create_directory(*uino, "exdata");
        emit progress(QStringLiteral("Copying %1 file(s) to user %2 ...").arg(files.size()).arg(u), -1);
        for (const exfile& f : files) {
            if (ufs.resolve_path(udir + "/exdata/" + f.name)) writer.delete_file(exino, f.name);
            std::vector<std::byte> d(f.data);
            writer.write_file(exino, f.name, d);
            ++copied;
        }
        ++udone;
    }
    writer.update_superblock();
    emit finished(true, QStringLiteral("Copied %1 license file(s) from user %2 to %3 other user(s). Rebuild Database on the console.") .arg(copied).arg(src).arg(udone));
    return;
}

void worker::run_license_single(app::gameos_mount& m, fs::ufs2_filesystem& ufs) {
    std::vector<std::byte> rap;
    QFile rf(job_.rap_path);
    if (!rf.open(QIODevice::ReadOnly)) { emit finished(false, QStringLiteral("cannot open RAP file")); return; }
    const QByteArray rb = rf.read(64);
    for (char c : rb) rap.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(c)));
    if (rap.size() != 16) { emit finished(false, QStringLiteral("RAP must be 16 bytes")); return; }
    const auto klic = license::rap_to_klicensee({rap.data(), rap.size()});

    fs::ufs2_writer writer(ufs, *m.decrypted);
    QStringList users = job_.exdata_users;
    if (users.isEmpty()) {
        users = local_users_q(ufs);
        if (users.isEmpty()) users << QStringLiteral("00000001");
    }

    std::array<std::byte, 0x98> rif{};
    std::vector<std::byte> actbytes;
    if (job_.type == job::license_rif_only) {
        actbytes = find_any_act_dat(ufs);
        if (actbytes.size() != 0x1038) {
            emit finished(false, QStringLiteral("No act.dat on the console; use Full Activation.")); return;
        }
        rif = license::build_rif({job_.idps.data(), job_.idps.size()}, job_.content_id.toStdString(), {klic.data(), klic.size()}, {actbytes.data(), actbytes.size()});
    } else { // full_activation
        for (const QString& u : users)
            if (ufs.resolve_path("home/" + u.toStdString() + "/exdata/act.dat") && !job_.force) {
                emit finished(false, QStringLiteral("act.dat already exists - use Reuse act.dat, or enable Force."));
                return;
            }
        const std::uint64_t acct = bswap64(job_.account_id);
        const auto act = license::build_activation({job_.idps.data(), job_.idps.size()}, job_.content_id.toStdString(), {klic.data(), klic.size()}, acct);
        actbytes.assign(act.act_dat.begin(), act.act_dat.end());
        rif = act.rif;
    }

    const std::string fname = job_.content_id.toStdString() + ".rif";
    std::vector<std::byte> rv(rif.begin(), rif.end());
    int wrote = 0;
    for (const QString& u : users) {
        if (cancel_.load()) break;
        const std::string udir = "home/" + u.toStdString();
        auto uino = ufs.resolve_path_to_inode_number(udir);
        if (!uino) continue;
        std::uint64_t exino = 0;
        if (auto ex = ufs.resolve_path_to_inode_number(udir + "/exdata")) exino = *ex;
        else exino = writer.create_directory(*uino, "exdata");
        // act.dat: seed a user that has none, overwrite on a forced full activation.
        const bool has_act = ufs.resolve_path(udir + "/exdata/act.dat").has_value();
        if (!has_act || (job_.type == job::full_activation && job_.force)) {
            if (has_act) writer.delete_file(exino, "act.dat");
            std::vector<std::byte> av(actbytes);
            writer.write_file(exino, "act.dat", av);
        }
        if (ufs.resolve_path(udir + "/exdata/" + fname)) writer.delete_file(exino, fname);
        std::vector<std::byte> rvc(rv);
        writer.write_file(exino, fname, rvc);
        ++wrote;
    }
    writer.update_superblock(); // flush it, no full fsck for a small license write since it takes long af

    emit finished(true, QStringLiteral("Wrote %1 for %2 user(s).  (Rebuild Database on the console.)").arg(QString::fromStdString(fname)).arg(wrote));
}

} // namespace ps3hdd::ui
