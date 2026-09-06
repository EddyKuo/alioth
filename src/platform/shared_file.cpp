#include "platform/shared_file.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace alioth::platform {

#ifdef _WIN32

struct SharedReadFile::Impl {
    HANDLE handle{INVALID_HANDLE_VALUE};
    std::uint64_t size{0};
};

SharedReadFile::SharedReadFile() : impl_(std::make_unique<Impl>()) {}

SharedReadFile::~SharedReadFile() { close(); }

bool SharedReadFile::open(const std::string& utf8Path) {
    close();

    std::wstring widePath;
    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(),
                                               static_cast<int>(utf8Path.size()), nullptr, 0);
    if (wideLength <= 0) return false;
    widePath.resize(static_cast<std::size_t>(wideLength));
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), static_cast<int>(utf8Path.size()),
                        widePath.data(), wideLength);

    // FILE_SHARE_DELETE 是這整個類別存在的理由：少了它，存檔時的原子更名會被
    // 這個控制代碼擋下來，而錯誤訊息只會說「存取被拒絕」。
    const HANDLE handle = CreateFileW(
        widePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        CloseHandle(handle);
        return false;
    }

    impl_->handle = handle;
    impl_->size = static_cast<std::uint64_t>(size.QuadPart);
    return true;
}

void SharedReadFile::close() {
    if (impl_->handle != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->handle);
        impl_->handle = INVALID_HANDLE_VALUE;
    }
    impl_->size = 0;
}

bool SharedReadFile::isOpen() const noexcept { return impl_->handle != INVALID_HANDLE_VALUE; }

std::uint64_t SharedReadFile::size() const noexcept { return impl_->size; }

std::size_t SharedReadFile::read(std::uint64_t offset, void* buffer, std::size_t length) const {
    if (!isOpen() || buffer == nullptr || length == 0) return 0;
    if (offset >= impl_->size) return 0;

    const std::uint64_t available = impl_->size - offset;
    const auto toRead = static_cast<DWORD>(std::min<std::uint64_t>(available, length));

    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);

    DWORD read = 0;
    if (!ReadFile(impl_->handle, buffer, toRead, &read, &overlapped)) return 0;
    return read;
}

#else

struct SharedReadFile::Impl {
    int fd{-1};
    std::uint64_t size{0};
};

SharedReadFile::SharedReadFile() : impl_(std::make_unique<Impl>()) {}

SharedReadFile::~SharedReadFile() { close(); }

bool SharedReadFile::open(const std::string& utf8Path) {
    close();
    const int fd = ::open(utf8Path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        ::close(fd);
        return false;
    }

    impl_->fd = fd;
    impl_->size = static_cast<std::uint64_t>(info.st_size);
    return true;
}

void SharedReadFile::close() {
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
    impl_->size = 0;
}

bool SharedReadFile::isOpen() const noexcept { return impl_->fd >= 0; }

std::uint64_t SharedReadFile::size() const noexcept { return impl_->size; }

std::size_t SharedReadFile::read(std::uint64_t offset, void* buffer, std::size_t length) const {
    if (!isOpen() || buffer == nullptr || length == 0) return 0;
    if (offset >= impl_->size) return 0;
    const ssize_t read = ::pread(impl_->fd, buffer, length, static_cast<off_t>(offset));
    return read > 0 ? static_cast<std::size_t>(read) : 0;
}

#endif

}  // namespace alioth::platform
