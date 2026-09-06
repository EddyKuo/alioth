#pragma once

// 可被取代的唯讀檔案控制代碼。
//
// 存在理由是一個 Windows 上很容易踩到、而且症狀完全不像原因的問題：
// 只要有人開著檔案，ReplaceFileW / MoveFileExW 就會失敗，於是「檢視器開著文件時
// 存檔」必然失敗。而檢視器開著文件正是存檔的唯一情境。
//
// 解法不是關掉再開，而是開檔時就宣告允許他人刪除／更名（FILE_SHARE_DELETE）。
// 已開啟的控制代碼在更名後仍指向舊內容，這沒關係——存完檔本來就要重新載入。
//
// 順帶滿足 PRD WBS 2.2 的另一個要求：以隨機存取讀取而不是把整份檔案讀進記憶體。
// 100 MB 的工程圖不該在開檔瞬間就吃掉 100 MB 常駐記憶體。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace alioth::platform {

class SharedReadFile {
public:
    SharedReadFile();
    ~SharedReadFile();

    SharedReadFile(const SharedReadFile&) = delete;
    SharedReadFile& operator=(const SharedReadFile&) = delete;

    // 路徑是 UTF-8。刻意不用 QString：平台層以外的呼叫端（引擎轉接層）不相依 Qt，
    // Windows 需要的寬字元轉換收在實作檔裡。
    [[nodiscard]] bool open(const std::string& utf8Path);
    void close();

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] std::uint64_t size() const noexcept;

    // 從指定位移讀取。回傳實際讀到的位元組數；越界讀取回傳 0 而不是未定義行為。
    [[nodiscard]] std::size_t read(std::uint64_t offset, void* buffer, std::size_t length) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::platform
