#pragma once

// 空間使用稽核（對標 PDF-XChange 的 File → Audit Space Usage）。
//
// 回答一個具體的問題：「這份 120 MB 的檔案，到底是什麼占掉的？」
//
// 使用者問這個問題時通常正打算把檔案寄出去或上傳，而選項只有幾個——
// 重新壓縮影像、丟掉不可達物件、攤平註解。沒有這份分類，他只能一個個試，
// 而每一個都要重寫整份檔案才知道有沒有用。
//
// 分類依物件自己宣告的型別，不猜：/Subtype /Image 就是影像，/Type /Font
// 就是字型。認不出來的歸「其他」而不是硬塞進某一類——一個膨脹的「其他」
// 至少誠實地說出「我不知道這是什麼」，而錯誤的分類會把人引到錯的方向。
//
// 統計的是**序列化後的位元組**（含 stream 資料），因為使用者關心的是檔案
// 大小，不是物件個數。

#include <cstdint>
#include <string>
#include <vector>

namespace alioth::engine::objects {

enum class SpaceCategory : std::uint8_t {
    Images,        // /Subtype /Image
    Fonts,         // /Type /Font、/FontFile*、/Type /FontDescriptor
    ContentStreams,  // 頁面 /Contents 指到的串流
    Annotations,   // /Type /Annot
    Metadata,      // /Metadata、/Type /Info 指向的字典、XMP
    Structure,     // 頁面樹、catalog、/StructTreeRoot、名稱樹等骨架
    Other,         // 認不出來的
};

[[nodiscard]] const char* describe(SpaceCategory category) noexcept;

struct SpaceCategoryUsage {
    SpaceCategory category{SpaceCategory::Other};
    std::uint64_t bytes{0};
    int objectCount{0};
};

struct SpaceAuditResult {
    bool ok{false};
    std::string diagnostic;

    std::uint64_t fileBytes{0};
    // 各類別的用量，依位元組由多到少。占最多的那一類是使用者唯一會採取
    // 行動的對象，所以排在最前面。
    std::vector<SpaceCategoryUsage> categories;

    // 所有物件序列化後的總長。與 fileBytes 的差額是 xref、trailer、
    // 物件之間的空白，以及**不可達物件**——後者正是「另存新檔會變小多少」
    // 的來源，所以這個差額要讓呼叫端看得到，不能藏起來。
    std::uint64_t objectBytes{0};

    [[nodiscard]] std::uint64_t overheadBytes() const noexcept {
        return fileBytes > objectBytes ? fileBytes - objectBytes : 0;
    }
};

[[nodiscard]] SpaceAuditResult auditSpaceUsage(const std::string& sourceBytes);

}  // namespace alioth::engine::objects
