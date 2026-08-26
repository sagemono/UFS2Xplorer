#include "game_size_scanner.h"

#include "fs_util.h"

#include <ps3hdd_app/gameos.h>
#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_ipc/ipc_client.h>

#include <memory>

namespace ps3hdd::ui {

void game_size_scanner::run() {
    try {
        ps3hdd::ipc::broker_client broker;
        broker.connect_to(port_, token_, 8000);
        const auto info = broker.open(device_, /*writable=*/false);
        auto src = std::make_shared<ps3hdd::ipc::ipc_disk_source>(broker, info);
        auto m = app::open_gameos(src, {eid_.data(), eid_.size()});
        if (m) {
            fs::ufs2_filesystem ufs(*m->decrypted, m->partition_sector);
            if (ufs.mount()) {
                for (const auto& t : targets_) {
                    if (cancel_.load()) break;
                    const std::uint64_t g = dir_size(ufs, t.game_inode, &cancel_);
                    std::uint64_t s = 0;
                    for (std::uint64_t si : t.save_inodes) {
                        if (cancel_.load()) break;
                        s += dir_size(ufs, si, &cancel_);
                    }
                    if (cancel_.load()) break;
                    emit sized(t.index, g, s);
                }
            }
        }
    } catch (...) {
    }
    emit finished();
}

} // namespace ps3hdd::ui