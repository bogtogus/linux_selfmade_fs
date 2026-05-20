#include "selfs_ioctl.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

void usage(const char *argv0)
{
    std::cerr <<
        "Usage:\n"
        "  " << argv0 << " test    <mountpoint>\n"
        "  " << argv0 << " zero    <path>\n"
        "  " << argv0 << " erase   <path>\n"
        "  " << argv0 << " meta    <path>\n"
        "  " << argv0 << " sectors <path> <filename>\n";
}


int cmd_test(const std::string &mountpoint)
{
    std::error_code ec;
    if (!fs::is_directory(mountpoint, ec)) {
        std::cerr << "test_selfs: '" << mountpoint
                  << "' is not a directory\n";
        return 1;
    }

    std::random_device rd;
    std::mt19937       rng(rd());
    std::uniform_int_distribution<uint32_t> dist(1, UINT32_MAX);

    unsigned ok = 0, fail = 0, skipped = 0;

    for (const auto &entry : fs::directory_iterator(mountpoint, ec)) {
        if (ec) {
            std::cerr << "test_selfs: directory iteration failed: "
                      << ec.message() << "\n";
            return 1;
        }
        if (!entry.is_regular_file())
            continue;

        const std::string path = entry.path().string();
        const uint32_t    want = dist(rng);

        // ---- write phase ------------------------------------------------
        int fd = ::open(path.c_str(), O_WRONLY);
        if (fd < 0) {
            std::cerr << "  [SKIP] open(O_WRONLY) " << path
                      << ": " << std::strerror(errno) << "\n";
            ++skipped;
            continue;
        }
        ssize_t w = ::write(fd, &want, sizeof(want));
        ::close(fd);
        if (w != static_cast<ssize_t>(sizeof(want))) {
            std::cerr << "  [FAIL] write " << path << ": w=" << w
                      << " errno=" << std::strerror(errno) << "\n";
            ++fail;
            continue;
        }

        // ---- read phase -------------------------------------------------
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            std::cerr << "  [FAIL] open(O_RDONLY) " << path
                      << ": " << std::strerror(errno) << "\n";
            ++fail;
            continue;
        }
        uint32_t got = 0;
        ssize_t  r = ::read(fd, &got, sizeof(got));
        ::close(fd);
        if (r != static_cast<ssize_t>(sizeof(got))) {
            std::cerr << "  [FAIL] read " << path << ": r=" << r
                      << " errno=" << std::strerror(errno) << "\n";
            ++fail;
            continue;
        }

        if (got == want) {
            ++ok;
        } else {
            std::cerr << "  [FAIL] " << path
                      << ": wrote 0x" << std::hex << want
                      << ", read 0x" << got << std::dec << "\n";
            ++fail;
        }
    }

    std::cout << "test_selfs: " << ok << " ok, " << fail << " fail";
    if (skipped)
        std::cout << ", " << skipped << " skipped";
    std::cout << "\n";

    return fail == 0 ? 0 : 2;
}


int open_for_ioctl(const std::string &path)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "test_selfs: open '" << path << "': "
                  << std::strerror(errno) << "\n";
    }
    return fd;
}

int cmd_zero(const std::string &path)
{
    int fd = open_for_ioctl(path);
    if (fd < 0)
        return 1;
    int rc = ::ioctl(fd, SELFS_IOCTL_ZERO_ALL);
    int saved_errno = errno;
    ::close(fd);
    if (rc != 0) {
        std::cerr << "ioctl ZERO_ALL: " << std::strerror(saved_errno) << "\n";
        return 1;
    }
    std::cout << "ZERO_ALL: ok\n";
    return 0;
}

int cmd_erase(const std::string &path)
{
    int fd = open_for_ioctl(path);
    if (fd < 0)
        return 1;
    int rc = ::ioctl(fd, SELFS_IOCTL_ERASE_FS);
    int saved_errno = errno;
    ::close(fd);
    if (rc != 0) {
        std::cerr << "ioctl ERASE_FS: " << std::strerror(saved_errno) << "\n";
        return 1;
    }
    std::cout << "ERASE_FS: ok (unmount the FS now)\n";
    return 0;
}

int cmd_meta(const std::string &path)
{
    int fd = open_for_ioctl(path);
    if (fd < 0)
        return 1;

    auto *meta = static_cast<selfs_meta_list *>(
        std::calloc(1, sizeof(selfs_meta_list)));
    if (!meta) {
        std::cerr << "test_selfs: out of memory\n";
        ::close(fd);
        return 1;
    }

    int rc = ::ioctl(fd, SELFS_IOCTL_GET_META, meta);
    int saved_errno = errno;
    ::close(fd);
    if (rc != 0) {
        std::cerr << "ioctl GET_META: " << std::strerror(saved_errno) << "\n";
        std::free(meta);
        return 1;
    }

    std::printf("%-20s  %12s  %8s  %10s\n",
                "name", "offset_sec", "size_sec", "crc32");
    std::printf("%-20s  %12s  %8s  %10s\n",
                "--------------------", "------------",
                "--------", "----------");
    for (uint32_t i = 0; i < meta->num_files; ++i) {
        const auto &f = meta->files[i];
        std::printf("%-20s  %12llu  %8u  0x%08x\n",
                    f.name,
                    static_cast<unsigned long long>(f.offset_sector),
                    f.size_sectors,
                    f.hash);
    }
    std::printf("Total: %u file(s)\n", meta->num_files);

    std::free(meta);
    return 0;
}

int cmd_sectors(const std::string &path, const std::string &filename)
{
    if (filename.size() >= SELFS_NAME_LEN) {
        std::cerr << "test_selfs: filename too long (max "
                  << (SELFS_NAME_LEN - 1) << ")\n";
        return 1;
    }

    int fd = open_for_ioctl(path);
    if (fd < 0)
        return 1;

    selfs_sector_map map{};
    std::strncpy(map.name, filename.c_str(), SELFS_NAME_LEN - 1);

    int rc = ::ioctl(fd, SELFS_IOCTL_GET_SECTORS, &map);
    int saved_errno = errno;
    ::close(fd);
    if (rc != 0) {
        std::cerr << "ioctl GET_SECTORS: "
                  << std::strerror(saved_errno) << "\n";
        return 1;
    }

    std::printf("File '%s' occupies %u sector(s):\n",
                map.name, map.num_sectors);
    for (uint32_t i = 0; i < map.num_sectors; ++i)
        std::printf("  [%u] sector %llu\n",
                    i,
                    static_cast<unsigned long long>(map.sectors[i]));
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    const std::string cmd = argv[1];

    if (cmd == "test"    && argc == 3) return cmd_test   (argv[2]);
    if (cmd == "zero"    && argc == 3) return cmd_zero   (argv[2]);
    if (cmd == "erase"   && argc == 3) return cmd_erase  (argv[2]);
    if (cmd == "meta"    && argc == 3) return cmd_meta   (argv[2]);
    if (cmd == "sectors" && argc == 4) return cmd_sectors(argv[2], argv[3]);

    usage(argv[0]);
    return 1;
}
