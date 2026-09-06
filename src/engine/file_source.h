#pragma once

// PDFium 的回呼式檔案存取（WBS 2.2）。
//
// 兩個理由讓我們不用 FPDF_LoadDocument：
//   1. 它會整份把檔案讀進 PDFium 的記憶體，10,000 頁的文件在開檔瞬間就爆掉預算
//   2. 它持有的檔案控制代碼會讓存檔時的原子更名失敗（見 platform/shared_file.h）
//
// 這個類別把 platform::SharedReadFile 包成 FPDF_FILEACCESS。它必須活得比
// FPDF_DOCUMENT 久——PDFium 在整個文件生命週期內都會回頭呼叫這個結構。

#include <cstdint>
#include <memory>
#include <string>

#include "platform/shared_file.h"

namespace alioth::engine {

class FileSource {
public:
    FileSource();
    ~FileSource();

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    [[nodiscard]] bool open(const std::string& utf8Path);
    void close();

    [[nodiscard]] bool isOpen() const;

    // 回傳 FPDF_FILEACCESS*，型別以 void* 藏起來讓 PDFium 標頭不外洩。
    [[nodiscard]] void* fileAccess() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::engine
