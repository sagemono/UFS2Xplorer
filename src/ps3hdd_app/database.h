#pragma once

#include <ps3hdd_fs/ufs2_filesystem.h>
#include <ps3hdd_fs/ufs2_writer.h>

namespace ps3hdd::app {

int invalidate_content_database(fs::ufs2_filesystem& fs, fs::ufs2_writer& writer);

} // namespace ps3hdd::app