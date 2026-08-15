#pragma once
#include <cstddef>
#include <fstream>
#include <span>
#include <string>
#include <sys/types.h>

//TODO: add another param to the seek functions to pass the direction of seeking
namespace disk {
    // fstream API
    std::size_t file_size(std::fstream& file);
    void seek_read_to(std::fstream& file, std::streamoff offset);
    void seek_write_to(std::fstream& file, std::streamoff offset);
    void read_exact(std::fstream& file, std::span<char> buffer);
    void write_exact(std::fstream& file, std::span<const char> buffer);
    void sync_file_to_disk(const std::string& path);
    std::streamoff get_curr_write_offset(std::fstream& file);
    std::streamoff get_curr_read_offset(std::fstream& file);

    // fd-based API
    int open_file(const std::string &path, int flags, mode_t mode = 0644);
    int close_file(int fd);
    std::size_t file_size(int fd);
    void read_exact_at(int fd, std::span<char> buffer, std::streamoff offset);
    void write_exact_at(int fd, std::span<const char> buffer, std::streamoff offset);
    void truncate_file(int fd, std::streamoff size);
    void sync_file_to_disk_fd(int fd);
    void sync_directory(const std::string& path);
}
