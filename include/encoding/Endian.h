#pragma once
#include <cstdint>

void put_u32_be(char out[4], std::uint32_t value);

std::uint32_t get_u32_be(char in[4]);