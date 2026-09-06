#pragma once

// PDF 物件模型與序列化（ADR-002）。
//
// PDFium 的公開 API 不讓我們寫任意的 PDF 物件：/AP 串流沒有 /Resources、
// 字典之間沒有辦法建立參照（/Popup、/IRT）、/L 與 /BS /D 沒有 setter。
// 逐項去找替代 API 只會拼湊出一堆在下次升版時要重新確認的特例，因此改為
// 自己把需要精確控制的物件寫成位元組，附加在檔尾。
//
// 這一層刻意是純函數且不連結 PDFium：輸入是物件、輸出是位元組，
// 因此可以逐位元組驗證 ISO 32000 的語法，而不必開啟一份真實 PDF。
// 物件圖的走訪、xref 與檔案結構屬於上面兩層（pdf_source_document 與
// incremental_appender），不在這裡。

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace alioth::engine::objects {

class PdfObject;

// std::vector 允許元素型別在實例化時不完整（C++17 起），因此陣列與字典
// 可以直接遞迴含有 PdfObject，不需要額外的 pimpl 或指標間接層。
using PdfArray = std::vector<PdfObject>;

struct PdfNull {};

struct PdfName {
    std::string value;  // 不含前導斜線；序列化時才做 #XX 跳脫
};

struct PdfRef {
    int number{0};
    int generation{0};

    [[nodiscard]] bool valid() const noexcept { return number > 0; }
    friend constexpr bool operator==(const PdfRef&, const PdfRef&) = default;
};

// 字串一律以「已解碼的位元組」保存，跳脫與十六進位編碼是序列化時的事。
// 把兩者混在一起是 PDF 寫入最常見的錯誤來源：同一份資料被跳脫兩次，
// 症狀是括號配對失衡導致其後所有物件解析錯位，而不是單一字串顯示怪異。
struct PdfString {
    std::string bytes;
    bool hex{false};
};

class PdfDictionary {
public:
    using Entry = std::pair<std::string, PdfObject>;

    PdfDictionary();
    ~PdfDictionary();
    PdfDictionary(const PdfDictionary&);
    PdfDictionary(PdfDictionary&&) noexcept;
    PdfDictionary& operator=(const PdfDictionary&);
    PdfDictionary& operator=(PdfDictionary&&) noexcept;

    // 既有鍵就地覆寫，新鍵接在尾端。維持插入順序讓輸出位元組可預期，
    // 測試才有辦法逐位元組比對；用 map 的話順序由字典序決定，
    // 與 Acrobat 產出的檔案差異會大到無法目視比對。
    void set(std::string key, PdfObject value);
    void remove(const std::string& key);

    [[nodiscard]] bool has(const std::string& key) const;
    [[nodiscard]] const PdfObject* find(const std::string& key) const;
    [[nodiscard]] PdfObject* find(const std::string& key);

    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    std::vector<Entry> entries_;
};

struct PdfStream {
    PdfDictionary dict;
    std::string data;  // 已是最終要寫進檔案的位元組（本層不做壓縮）
};

// 逐位元組原樣輸出，不經過任何跳脫或格式化（WBS 6.11，PRD-SIG-004 的一部分）。
//
// 存在理由是數位簽章的 /ByteRange 與 /Contents：兩者的最終數值只有在整份輸出
// 序列化完成、量出實際位移之後才知道，但序列化完成之後又不能再改動任何位元組的
// 寬度——改寬度就會讓後面所有物件的位移全部錯位。唯一可行的順序是「先佔位、
// 量出位移、原地覆寫成等寬的最終值」，而一般的 PdfDictionary／PdfArray 走數字
// 格式化（formatReal／std::to_string）不保證輸出寬度固定（前導零、位數都可能
// 隨數值變動）。PdfRawLiteral 讓呼叫端自己保證「佔位符與最終值等寬」，
// 其餘物件——包含同一個簽章字典裡不涉及位移的鍵——仍然透過正常的
// PdfDictionary／serializeInto 產生，只有這一個逃生艙口繞過格式化。
struct PdfRawLiteral {
    std::string bytes;  // 已是最終要寫進檔案的位元組，呼叫端自行保證合法性
};

class PdfObject {
public:
    using Value = std::variant<PdfNull, bool, std::int64_t, double, PdfName, PdfString, PdfArray,
                               PdfDictionary, PdfStream, PdfRef, PdfRawLiteral>;

    PdfObject();
    ~PdfObject();
    PdfObject(const PdfObject&);
    PdfObject(PdfObject&&) noexcept;
    PdfObject& operator=(const PdfObject&);
    PdfObject& operator=(PdfObject&&) noexcept;

    PdfObject(PdfNull);            // NOLINT(google-explicit-constructor)
    PdfObject(bool value);         // NOLINT(google-explicit-constructor)
    PdfObject(std::int64_t value); // NOLINT(google-explicit-constructor)
    PdfObject(int value);          // NOLINT(google-explicit-constructor)
    PdfObject(double value);       // NOLINT(google-explicit-constructor)
    PdfObject(PdfName value);      // NOLINT(google-explicit-constructor)
    PdfObject(PdfString value);    // NOLINT(google-explicit-constructor)
    PdfObject(PdfArray value);     // NOLINT(google-explicit-constructor)
    PdfObject(PdfDictionary value);// NOLINT(google-explicit-constructor)
    PdfObject(PdfStream value);    // NOLINT(google-explicit-constructor)
    PdfObject(PdfRef value);       // NOLINT(google-explicit-constructor)
    PdfObject(PdfRawLiteral value);// NOLINT(google-explicit-constructor)

    [[nodiscard]] const Value& value() const noexcept { return value_; }
    [[nodiscard]] Value& value() noexcept { return value_; }

    [[nodiscard]] bool isNull() const noexcept;
    [[nodiscard]] bool isNumber() const noexcept;
    [[nodiscard]] bool isName(const char* expected = nullptr) const noexcept;
    [[nodiscard]] bool isRef() const noexcept;
    [[nodiscard]] bool isArray() const noexcept;
    [[nodiscard]] bool isDictionary() const noexcept;
    [[nodiscard]] bool isStream() const noexcept;

    // 取值。型別不符時回傳 nullptr 或預設值，讓呼叫端能明確判斷，
    // 而不是在不可信任的輸入上丟例外。
    [[nodiscard]] double asNumber(double fallback = 0.0) const noexcept;
    [[nodiscard]] std::int64_t asInteger(std::int64_t fallback = 0) const noexcept;
    [[nodiscard]] std::string asName() const;
    [[nodiscard]] PdfRef asRef() const noexcept;
    [[nodiscard]] const PdfArray* asArray() const noexcept;
    [[nodiscard]] PdfArray* asArray() noexcept;
    // 串流物件也有字典，因此兩種形態都回傳字典指標。
    [[nodiscard]] const PdfDictionary* asDictionary() const noexcept;
    [[nodiscard]] PdfDictionary* asDictionary() noexcept;
    [[nodiscard]] const PdfStream* asStream() const noexcept;
    [[nodiscard]] PdfStream* asStream() noexcept;

private:
    Value value_;
};

// PDF 實數格式化：最多四位小數、去尾隨零、不受地區設定影響。
//
// 不走 printf 系列是因為 LC_NUMERIC 被設成以逗號為小數點的地區時，
// 產出的位元組會讓整份物件損毀，而那種錯誤只在特定機器上重現。
// 實作直接沿用 annotations::formatNumber，避免同一個規則有兩份真相。
[[nodiscard]] std::string formatReal(double value);

// 依 ISO 32000 §7.3.5 把名稱做 #XX 跳脫。
[[nodiscard]] std::string escapeName(const std::string& name);

// 依 §7.3.4.2 跳脫常值字串。反斜線與括號沒跳脫會讓括號配對失衡，
// 其後的物件會整段解析錯位。
[[nodiscard]] std::string escapeLiteralString(const std::string& bytes);

// 文字字串（/T、/Contents、/Subj 等）。
//
// 純 ASCII 走常值字串，其餘一律轉成 UTF-16BE 並加上 BOM 後以十六進位輸出：
// PDFDocEncoding 涵蓋不了中文，而十六進位字串不受跳脫規則影響，
// 是唯一不會因為某個位元組剛好是 0x28 就毀掉整份檔案的寫法。
[[nodiscard]] PdfObject makeTextString(const std::string& utf8);

// 純位元組字串（/ID、/NM 這類本來就是 ASCII 的識別碼）。
[[nodiscard]] PdfObject makeLiteralString(const std::string& bytes);

[[nodiscard]] PdfObject makeName(std::string name);
[[nodiscard]] PdfObject makeRef(int number, int generation = 0);
[[nodiscard]] PdfObject makeNumberArray(const std::vector<double>& values);

// 序列化成 ISO 32000 的位元組。串流的 /Length 由序列化器依實際資料長度覆寫，
// 呼叫端不需要（也不應該）自行維護它。
void serializeInto(std::string& out, const PdfObject& object);
[[nodiscard]] std::string serialize(const PdfObject& object);

// 間接物件：「N G obj ... endobj\n」。
[[nodiscard]] std::string serializeIndirect(int number, int generation, const PdfObject& object);

}  // namespace alioth::engine::objects
