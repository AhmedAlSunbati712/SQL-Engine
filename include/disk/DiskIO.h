#pragma once
#include <cstddef>
#include <fstream>
#include <span>
#include <string>

//TODO: add another param to the seek functions to pass the direction of seeking
namespace disk {
    std::size_t file_size(std::fstream& file);
    void seek_read_to(std::fstream& file, std::streamoff offset);
    void seek_write_to(std::fstream& file, std::streamoff offset);
    void read_exact(std::fstream& file, std::span<char> buffer);
    void write_exact(std::fstream& file, std::span<const char> buffer);
    void sync_file_to_disk(const std::string& path);
    std::streamoff get_curr_write_offset(std::fstream& file);
    std::streamoff get_curr_read_offset(std::fstream& file);
}
