#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace ps3hdd::license {

std::array<std::byte, 16> rap_to_klicensee(std::span<const std::byte> rap);

} // namespace ps3hdd::license