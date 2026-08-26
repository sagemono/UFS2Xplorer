#include "database.h"

#include <cstddef>
#include <vector>

namespace ps3hdd::app {

int invalidate_content_database(fs::ufs2_filesystem& fs, fs::ufs2_writer& writer) {
    const auto mms = fs.resolve_path_to_inode_number("mms");
    if (!mms) return -1;

    const std::vector<std::byte> flag{std::byte{0x00}, std::byte{0x00}, std::byte{0x03}, std::byte{0xE9}};
    if (const auto err = fs.resolve_path("mms/db.err")) {
        if (fs.read_inode_data(*err) == flag) return 0; // alr flagged
        writer.delete_file(*mms, "db.err");
    }
    writer.write_file(*mms, "db.err", flag);
    writer.update_superblock();
    return 1;
}

} // namespace ps3hdd::app