#pragma once

#include <cstdint>
#include <span>

std::uint32_t crc32c(std::span<const char> data);
