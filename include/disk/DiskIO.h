#include <cstddef>
#include <fstream>
#include <span>

namespace disk {
    std::size_t file_size(std::fstream& file);
    void seek_read_to(std::fstream& file, std::streamoff offset);
    void seek_write_to(std::fstream& file, std::streamoff offset);
    void read_exact(std::fstream& file, std::span<char> buffer);
    void write_exact(std::fstream& file, std::span<const char> buffer);
}