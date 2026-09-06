#include "engine/file_source.h"

#include <fpdfview.h>

namespace alioth::engine {
namespace {

int readBlock(void* param, unsigned long position, unsigned char* buffer, unsigned long size) {
    auto* file = static_cast<platform::SharedReadFile*>(param);
    // PDFium 的回呼約定：完全讀滿才算成功，短讀一律視為失敗。
    // 回傳部分成功會讓 PDFium 拿到半截物件，症狀是「檔案損毀」而不是「讀取失敗」。
    const std::size_t read = file->read(position, buffer, size);
    return read == static_cast<std::size_t>(size) ? 1 : 0;
}

}  // namespace

struct FileSource::Impl {
    platform::SharedReadFile file;
    FPDF_FILEACCESS access{};
};

FileSource::FileSource() : impl_(std::make_unique<Impl>()) {}

FileSource::~FileSource() = default;

bool FileSource::open(const std::string& utf8Path) {
    if (!impl_->file.open(utf8Path)) return false;

    // PDFium 的 m_FileLen 是 32 位元。超過 4 GB 的 PDF 不在本產品的支援範圍內，
    // 但必須明確拒絕而不是靜默截斷成一個看起來合法的長度。
    constexpr std::uint64_t kMaxSupported = 0xFFFFFFFFull;
    if (impl_->file.size() > kMaxSupported) {
        impl_->file.close();
        return false;
    }

    impl_->access.m_FileLen = static_cast<unsigned long>(impl_->file.size());
    impl_->access.m_GetBlock = &readBlock;
    impl_->access.m_Param = &impl_->file;
    return true;
}

void FileSource::close() {
    impl_->file.close();
    impl_->access = {};
}

bool FileSource::isOpen() const { return impl_->file.isOpen(); }

void* FileSource::fileAccess() noexcept { return &impl_->access; }

}  // namespace alioth::engine
