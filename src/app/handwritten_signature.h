#pragma once

// 手寫／輸入式簽名（PRD-SIG-008）。
//
// **這不是數位簽章。** 它只是一張圖或一段手繪筆跡，沒有任何密碼學保證：
// 任何人都能複製、貼到別的文件上，而且沒有任何機制能證明是誰放的、
// 放上去之後文件有沒有被改。
//
// 把它顯示得像數位簽章是這個產品最不該犯的錯，因此：
//   1. 型別上完全分開——這一層不碰 engine/signature，也不產生任何 /Sig 字典
//   2. 產出的註解一律帶固定的 /Subj 標記，讓簽章面板能認出來並排除
//   3. 建立時一律寫入作者與時間，那是它唯一能提供的「可追溯性」
//
// 兩種來源：使用者手繪的筆跡（走 /Ink）與匯入的影像（走 /Stamp 的自訂圖片）。
// 兩者都存在使用者端的簽名庫裡重複使用——每次簽名都重畫一次，
// 產出的筆跡不會一樣，那對「這看起來像我的簽名」是致命的。

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

#include "domain/annotation.h"

namespace alioth::app {

// 簽章面板據此排除手寫簽名。值刻意寫得明白，因為它會出現在別的檢視器的
// 註解屬性裡，而那時沒有我們的 UI 幫忙解釋。
inline constexpr const char* kHandwrittenSignatureSubject = "手寫簽名（非數位簽章）";

enum class SignatureSourceKind : std::uint8_t {
    Ink,    // 手繪筆跡
    Image,  // 匯入的影像
};

struct SavedSignature {
    QString name;  // 使用者取的名字，例如「正式」「草簽」
    SignatureSourceKind kind{SignatureSourceKind::Ink};

    // Ink：筆畫，座標在 0..1 的單位方框內，套用時再依目標矩形縮放。
    // 存正規化座標而不是頁面座標，因為同一個簽名要能貼到不同大小的欄位裡。
    std::vector<std::vector<domain::PointF>> strokes;

    // Image：RGB 或 RGBA 的原始像素。與 domain::StampGeometry 同一種表示。
    std::vector<std::uint8_t> pixels;
    std::int32_t width{0};
    std::int32_t height{0};
    std::int32_t channels{3};

    [[nodiscard]] bool isValid() const;
};

// 建立要蓋上去的註解。rect 是目標位置（頁面座標）。
//
// 回傳 nullopt 代表這份簽名資料無效——絕不回傳一個空的註解，
// 那會在文件裡留下一個看不見的東西，使用者以為簽了但其實沒有。
[[nodiscard]] std::optional<domain::Annotation> buildSignatureAnnotation(
    const SavedSignature& signature, const domain::RectF& rect, const QString& author);

// 這則註解是不是手寫簽名。簽章面板用它來排除，避免與 /Sig 混在同一份清單。
[[nodiscard]] bool isHandwrittenSignature(const domain::Annotation& annotation);

// 簽名庫。存在使用者端（QSettings），不寫進文件——簽名是使用者的私人資產，
// 跟著文件走出去等於把它送給每個拿到檔案的人。
class SignatureLibrary {
public:
    static constexpr int kMaxSignatures = 32;

    void load();
    void save() const;

    [[nodiscard]] const std::vector<SavedSignature>& signatures() const noexcept {
        return signatures_;
    }

    // 同名視為取代。回傳 false 代表資料無效或已達上限。
    bool add(const SavedSignature& signature);
    bool remove(const QString& name);
    void clear() noexcept { signatures_.clear(); }

    [[nodiscard]] const SavedSignature* find(const QString& name) const;

private:
    std::vector<SavedSignature> signatures_;
};

}  // namespace alioth::app
