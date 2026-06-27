#include <DiskIO.h>
#include <span>
#include <fstream>
#include <stdexcept>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// An assumption that is made throughout here is that a file is already open.
namespace disk {
    namespace {
        void ensure_nonnegative_offset(std::streamoff offset, const char *context) {
            if (offset < 0) {
                throw std::runtime_error(std::string("Error (") + context + "): Negative offsets are invalid!");
            }
        }
    }

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

    std::streamoff get_curr_write_offset(std::fstream& file) {
        file.clear();
        std::streamoff curr = file.tellp();
        if (curr == static_cast<std::streamoff>(-1)) {
            throw std::runtime_error("Error (get_curr_write_offset): Failed to read current write offset!");
        }
        return curr;
    }

    std::streamoff get_curr_read_offset(std::fstream& file) {
        file.clear();
        std::streamoff curr = file.tellg();
        if (curr == static_cast<std::streamoff>(-1)) {
            throw std::runtime_error("Error (get_curr_read_offset): Failed to read current read offset!");
        }
        return curr;
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

    int open_file(const std::string &path, int flags, mode_t mode) {
        int fd = ::open(path.c_str(), flags, mode);
        if (fd == -1) {
            throw std::runtime_error("Error (open_file): Failed to open file!");
        }
        return fd;
    }

    int close_file(int fd) {
        if (::close(fd) == -1) {
            throw std::runtime_error("Error (close_file): Failed to close file!");
        }
        return 0;
    }

    std::size_t file_size(int fd) {
        struct stat file_stat {};
        if (::fstat(fd, &file_stat) == -1) {
            throw std::runtime_error("Error (file_size): Failed to get file size!");
        }
        return static_cast<std::size_t>(file_stat.st_size);
    }

    void read_exact_at(int fd, std::span<char> buffer, std::streamoff offset) {
        ensure_nonnegative_offset(offset, "read_exact_at");

        std::size_t bytes_to_read = buffer.size();
        std::size_t total_read = 0;
        // QUOTE
        /**
         * this may happen for example because fewer bytes are actually available right now 
         * (maybe because we were close to end-of-file, or because we are reading from a pipe, 
         * or from a terminal), or because read() was interrupted by a signal. On error, -1 is 
         * returned, and errno is set appropriately. In this case it is left unspecified whether 
         * the file position (if any) changes.
         */
        // Just loop until EOF or we fulfilled our request
        while (total_read < bytes_to_read) {
            ssize_t bytes_read = ::pread(
                fd,
                buffer.data() + total_read,
                bytes_to_read - total_read,
                static_cast<off_t>(offset + static_cast<std::streamoff>(total_read))
            );

            if (bytes_read == 0) {
                throw std::runtime_error("Error (read_exact_at): Reached EOF before reading the requested number of bytes!");
            }

            if (bytes_read == -1) {
                if (errno == EINTR) continue;
                throw std::runtime_error("Error (read_exact_at): Failed to read from file!");
            }

            total_read += static_cast<std::size_t>(bytes_read);
        }
    }

    void write_exact_at(int fd, std::span<const char> buffer, std::streamoff offset) {
        ensure_nonnegative_offset(offset, "write_exact_at");

        std::size_t bytes_to_write = buffer.size();
        std::size_t total_written = 0;
        // Read the quote in read_exact_at
        while (total_written < bytes_to_write) {
            ssize_t bytes_written = ::pwrite(
                fd,
                buffer.data() + total_written,
                bytes_to_write - total_written,
                static_cast<off_t>(offset + static_cast<std::streamoff>(total_written))
            );

            if (bytes_written == -1) {
                if (errno == EINTR) continue;
                throw std::runtime_error("Error (write_exact_at): Failed to write to file!");
            }

            total_written += static_cast<std::size_t>(bytes_written);
        }
    }

    void truncate_file(int fd, std::streamoff size) {
        ensure_nonnegative_offset(size, "truncate_file");
        if (::ftruncate(fd, static_cast<off_t>(size)) == -1) {
            throw std::runtime_error("Error (truncate_file): Failed to truncate file!");
        }
    }

    void sync_file_to_disk_fd(int fd) {
#ifdef __APPLE__
        if (::fcntl(fd, F_FULLFSYNC) == -1) {
            throw std::runtime_error("Error (sync_file_to_disk_fd): Failed to fully sync file!");
        }
#else
        if (::fsync(fd) == -1) {
            throw std::runtime_error("Error (sync_file_to_disk_fd): Failed to sync file!");
        }
#endif
    }
}
