#include <gtest/gtest.h>

#include <DiskIO.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <string>

namespace {

class TempFile {
    public:
        TempFile() {
            auto unique_suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            path = std::filesystem::temp_directory_path() / ("stoneleafdb_diskio_test_" + unique_suffix + ".bin");
        }

        ~TempFile() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        std::filesystem::path path;
};

void write_file_bytes(const std::filesystem::path &path, const std::string &contents) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.close();
}

std::string read_file_bytes(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

TEST(DiskIOTest, FileSizeReturnsNumberOfBytesAndPreservesReadPosition) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "abcdef");

    std::fstream file(temp_file.path, std::ios::in | std::ios::binary);
    file.seekg(2, std::ios::beg);

    std::size_t size = disk::file_size(file);

    EXPECT_EQ(size, 6u);
    EXPECT_EQ(file.tellg(), 2);
}

TEST(DiskIOTest, SeekReadToMovesReadCursor) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "abcdef");

    std::fstream file(temp_file.path, std::ios::in | std::ios::binary);

    disk::seek_read_to(file, 3);

    char value = '\0';
    file.get(value);
    EXPECT_EQ(value, 'd');
}

TEST(DiskIOTest, SeekReadToThrowsForClosedFile) {
    std::fstream file;

    EXPECT_THROW(disk::seek_read_to(file, 0), std::runtime_error);
}

TEST(DiskIOTest, SeekWriteToMovesWriteCursor) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "abcdef");

    std::fstream file(temp_file.path, std::ios::in | std::ios::out | std::ios::binary);

    disk::seek_write_to(file, 2);
    std::array<char, 2> bytes{'X', 'Y'};
    disk::write_exact(file, bytes);
    file.close();

    EXPECT_EQ(read_file_bytes(temp_file.path), "abXYef");
}

TEST(DiskIOTest, SeekWriteToThrowsForClosedFile) {
    std::fstream file;

    EXPECT_THROW(disk::seek_write_to(file, 0), std::runtime_error);
}

TEST(DiskIOTest, ReadExactReadsRequestedBytes) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "abcdef");

    std::fstream file(temp_file.path, std::ios::in | std::ios::binary);
    std::array<char, 4> buffer{};

    disk::seek_read_to(file, 1);
    disk::read_exact(file, buffer);

    EXPECT_EQ(std::string(buffer.begin(), buffer.end()), "bcde");
}

TEST(DiskIOTest, ReadExactThrowsWhenBufferWouldCrossEof) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "abc");

    std::fstream file(temp_file.path, std::ios::in | std::ios::binary);
    std::array<char, 4> buffer{};

    EXPECT_THROW(disk::read_exact(file, buffer), std::runtime_error);
}

TEST(DiskIOTest, WriteExactWritesRequestedBytes) {
    TempFile temp_file;
    std::fstream file(temp_file.path, std::ios::out | std::ios::binary | std::ios::trunc);
    std::array<char, 5> buffer{'h', 'e', 'l', 'l', 'o'};

    disk::write_exact(file, buffer);
    file.close();

    EXPECT_EQ(read_file_bytes(temp_file.path), "hello");
}

TEST(DiskIOTest, GetCurrWriteOffsetReturnsCurrentWritePosition) {
    TempFile temp_file;
    std::fstream file(temp_file.path, std::ios::out | std::ios::binary | std::ios::trunc);

    std::array<char, 3> prefix{'a', 'b', 'c'};
    disk::write_exact(file, prefix);

    std::streamoff offset = disk::get_curr_write_offset(file);

    EXPECT_EQ(offset, 3);
}

TEST(DiskIOTest, GetCurrReadOffsetReturnsCurrentReadPosition) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "abcdef");

    std::fstream file(temp_file.path, std::ios::in | std::ios::binary);
    disk::seek_read_to(file, 4);

    std::streamoff offset = disk::get_curr_read_offset(file);

    EXPECT_EQ(offset, 4);
}

TEST(DiskIOTest, GetCurrWriteOffsetThrowsForClosedFile) {
    std::fstream file;

    EXPECT_THROW(disk::get_curr_write_offset(file), std::runtime_error);
}

TEST(DiskIOTest, GetCurrReadOffsetThrowsForClosedFile) {
    std::fstream file;

    EXPECT_THROW(disk::get_curr_read_offset(file), std::runtime_error);
}

TEST(DiskIOTest, SyncFileToDiskSucceedsForExistingFile) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "journal");

    EXPECT_NO_THROW(disk::sync_file_to_disk(temp_file.path.string()));
}

TEST(DiskIOTest, SyncFileToDiskThrowsForMissingFile) {
    TempFile temp_file;

    EXPECT_THROW(disk::sync_file_to_disk(temp_file.path.string()), std::runtime_error);
}

TEST(DiskIOTest, OpenFileAndCloseFileSucceedsForExistingFile) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "abcdef");

    int fd = disk::open_file(temp_file.path.string(), O_RDWR);

    EXPECT_GE(fd, 0);
    EXPECT_NO_THROW(disk::close_file(fd));
}

TEST(DiskIOTest, OpenFileCreatesMissingFileWhenRequested) {
    TempFile temp_file;

    int fd = disk::open_file(temp_file.path.string(), O_RDWR | O_CREAT, 0644);
    EXPECT_TRUE(std::filesystem::exists(temp_file.path));

    EXPECT_NO_THROW(disk::close_file(fd));
}

TEST(DiskIOTest, FdFileSizeReturnsNumberOfBytes) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "abcdef");

    int fd = disk::open_file(temp_file.path.string(), O_RDWR);

    EXPECT_EQ(disk::file_size(fd), 6u);

    EXPECT_NO_THROW(disk::close_file(fd));
}

TEST(DiskIOTest, ReadExactAtReadsRequestedBytesFromOffset) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "abcdef");

    int fd = disk::open_file(temp_file.path.string(), O_RDWR);
    std::array<char, 4> buffer{};

    disk::read_exact_at(fd, buffer, 1);

    EXPECT_EQ(std::string(buffer.begin(), buffer.end()), "bcde");
    EXPECT_NO_THROW(disk::close_file(fd));
}

TEST(DiskIOTest, ReadExactAtThrowsWhenBufferWouldCrossEof) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "abc");

    int fd = disk::open_file(temp_file.path.string(), O_RDWR);
    std::array<char, 4> buffer{};

    EXPECT_THROW(disk::read_exact_at(fd, buffer, 0), std::runtime_error);
    EXPECT_NO_THROW(disk::close_file(fd));
}

TEST(DiskIOTest, WriteExactAtWritesRequestedBytesAtOffset) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "abcdef");

    int fd = disk::open_file(temp_file.path.string(), O_RDWR);
    std::array<char, 2> buffer{'X', 'Y'};

    disk::write_exact_at(fd, buffer, 2);
    EXPECT_NO_THROW(disk::close_file(fd));

    EXPECT_EQ(read_file_bytes(temp_file.path), "abXYef");
}

TEST(DiskIOTest, TruncateFileShrinksFileToRequestedSize) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "abcdef");

    int fd = disk::open_file(temp_file.path.string(), O_RDWR);

    disk::truncate_file(fd, 3);
    EXPECT_NO_THROW(disk::close_file(fd));

    EXPECT_EQ(read_file_bytes(temp_file.path), "abc");
}

TEST(DiskIOTest, SyncFileToDiskFdSucceedsForExistingFile) {
    TempFile temp_file;
    write_file_bytes(temp_file.path, "journal");

    int fd = disk::open_file(temp_file.path.string(), O_RDWR);

    EXPECT_NO_THROW(disk::sync_file_to_disk_fd(fd));
    EXPECT_NO_THROW(disk::close_file(fd));
}

}
