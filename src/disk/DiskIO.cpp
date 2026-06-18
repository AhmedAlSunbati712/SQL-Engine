#include <DiskIO.h>
#include <span>
#include <fstream>
#include <stdexcept>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

// An assumption that is made throughout here is that a file is already open.
namespace disk {
    std::size_t file_size(std::fstream& file) {
        /**
         * Description: Seeks a file until the end and calculates the difference. leaves filestream at
         *              the same offset as before
         */
        file.clear();
        auto curr = file.tellg();

        file.seekg(0, std::ios::beg); // go to the start in case cursor is not at the start of the file already
        auto begin = file.tellg();

        file.seekg(0, std::ios::end);
        auto end = file.tellg();

        file.seekg(curr, std::ios::beg); // restore the cursor back to its original position
        return static_cast<size_t>(end - begin);
    }
    void seek_read_to(std::fstream& file, std::streamoff offset) {
        /**
         * clear the flag and just seek.
         */
        file.clear();
        file.seekg(offset, std::ios::beg);
        if (file.fail()) {
            throw std::runtime_error("Error(seek_read_to): Failed to seek!");
        }
        return;
    }
    void seek_write_to(std::fstream& file, std::streamoff offset) {
        /**
         * clear the flag and just seek.
         */
        file.clear();
        file.seekp(offset, std::ios::beg);
        if (file.fail()) {
            throw std::runtime_error("Error(seek_write_to): Failed to seek!");
        }
        return;
    }
    void read_exact(std::fstream& file, std::span<char> buffer) {
        /**
         * Assumes files is already seeked to the correct read position
         */
        file.clear();
        std::streamsize bytes_to_read = static_cast<std::streamsize>(buffer.size());
        file.read(buffer.data(), bytes_to_read);

        if (file.fail()) {
            throw std::runtime_error("Error (read_exact): Failed to read from file!");
        }
        if (file.gcount() != bytes_to_read) {
            throw std::runtime_error("Error (read_exact): Reached EOF before reading a full page!");
        }

        return;
    }

    void write_exact(std::fstream& file, std::span<const char> buffer) {
        file.clear();
        std::streamsize bytes_to_write = static_cast<std::streamsize>(buffer.size());
        file.write(buffer.data(), bytes_to_write);
        if (file.fail()) {
            throw std::runtime_error("Error (write_exact): Failed to write to file!");
        }
        return;
    }

    void sync_file_to_disk(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDWR);
        if (fd == -1) {
            throw std::runtime_error("Error (sync_file_to_disk): Failed to open file for sync!");
        }

#ifdef __APPLE__
        if (::fcntl(fd, F_FULLFSYNC) == -1) {
            int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            throw std::runtime_error("Error (sync_file_to_disk): Failed to fully sync file!");
        }
#else
        if (::fsync(fd) == -1) {
            int saved_errno = errno;
            ::close(fd);
            errno = saved_errno;
            throw std::runtime_error("Error (sync_file_to_disk): Failed to sync file!");
        }
#endif

        if (::close(fd) == -1) {
            throw std::runtime_error("Error (sync_file_to_disk): Failed to close synced file descriptor!");
        }
    }
}
