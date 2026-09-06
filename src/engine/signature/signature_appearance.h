#pragma once

// 可見簽章外觀（PRD-SIG-004 的補完，也是 PRD-SIG-005 的前置條件，見 ADR-004）。
//
// 無實體簽章（/Rect 全零、無 /AP）在密碼學上完全有效，但審圖流程需要看得見的
// 簽核欄——「每一頁都簽了」而畫面上什麼都沒有，對審核者沒有價值。
//
// **這一層畫的是給人看的東西，沒有任何安全意義。** 外觀可以偽造：任何人都能
// 畫一個長得一模一樣的圖章。真正的保證來自 /Contents 裡的 CMS 與 /ByteRange
// 的涵蓋範圍，那是 pkcs7_verifier 的職責。因此外觀上刻意**不寫「已驗證」
// 「有效」這類字樣**——那會讓一個未經驗證的檔案在畫面上宣稱自己是有效的。
//
// 文字一律限可列印 ASCII。CJK 需要字型子集內嵌，那是 CLAUDE.md 待決策清單裡
// 的項目；在它定案前，非 ASCII 明確失敗而不是靜默丟字（與 engine/annotations/
// text_layout.h 同一條策略）。簽署者姓名是 CJK 的情況很常見，因此呼叫端
// 可以改用手寫簽名影像那條路。

#include <cstdint>
#include <string>
#include <vector>

#include "domain/geometry.h"

namespace alioth::engine::signature {

// 影像來源（通常是使用者的手寫簽名掃描圖）。與 domain::StampGeometry 同一種
// 表示：RGB 或 RGBA 的原始像素，逐列由上而下。
struct AppearanceImage {
    std::vector<std::uint8_t> pixels;
    std::int32_t width{0};
    std::int32_t height{0};
    std::int32_t channels{3};

    [[nodiscard]] bool isValid() const;
};

struct SignatureAppearanceOptions {
    // 簽章欄位在頁面上的位置。空矩形代表無實體簽章（不產生 /AP）。
    domain::RectF rectPt{};

    std::string signerName;   // 通常取自憑證的 CN
    std::string reason;
    std::string location;
    std::string signingTime;  // 已格式化的時間字串；空字串代表不顯示

    // 手寫簽名影像。有影像時畫在左側，文字排在右側；沒有影像時文字佔滿整框。
    AppearanceImage image;

    bool drawBorder{true};
};

struct AppearanceResult {
    bool ok{false};
    std::string diagnostic;

    std::string content;  // /AP /N 的內容串流
    domain::RectF bbox{};

    // 內容串流是否引用了 /Helv 與 /Im0。寫入層據此組出 /Resources——
    // 缺了資源項目就是懸空名稱，外觀會是一片空白而且沒有任何錯誤訊息。
    bool needsFont{false};
    bool needsImage{false};
};

// 產生外觀串流。純函數，同輸入必得同輸出。
[[nodiscard]] AppearanceResult buildSignatureAppearance(const SignatureAppearanceOptions& options);

}  // namespace alioth::engine::signature
