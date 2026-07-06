#pragma once
#include <cstdint>

void put_u32_be(char *out, std::uint32_t value);
std::uint32_t get_u32_be(const char *in);
void put_u16_be(char *out, std::uint16_t value);
std::uint16_t get_u16_be(const char *in);
void put_u8_be(char *out, std::uint8_t value);
std::uint8_t get_u8_be(const char *in);
