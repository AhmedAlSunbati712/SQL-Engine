#pragma once
#include <cstdint>

void put_u32_be(char *out, std::uint32_t value);
std::uint32_t get_u32_be(const char *in);