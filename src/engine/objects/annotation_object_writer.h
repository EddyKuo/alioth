#pragma once

// 註解的物件層寫入通道（ADR-002，取代 FPDFAnnot_SetAP 的路徑）。
//
// FPDFAnnot_SetAP 只建立 Form XObject 串流本身，不建立 /Resources，因此串流內
// 不能引用 /ExtGState：螢光筆的 /BM /Multiply 與 /CA /ca 全部遺失，蓋住文字時
// 文字不會透出來。/Popup 與 /IRT 沒有「設定字典參照」的 API，回覆串
// （PRD-ANN-007）做不出來；/L 與 /BS /D 也沒有對應的 setter。
//
// 這裡直接把註解字典、/AP /N Form XObject（含 /Resources）與 /Popup 寫成物件
// 附加在檔尾，再把註解掛上頁面的 /Annots。外觀串流的內容位元組取自既有的
// engine/annotations 產生器，這一層不重畫任何幾何。

#include <optional>
#include <string>

#include "domain/annotation.h"
#include "domain/geometry.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::objects {

struct AnnotationWriteOptions {
    // 頁面 /Rotate。只影響便利貼圖示的反向旋轉矩陣，不影響座標。
    domain::Rotation pageRotation{domain::Rotation::None};

    // 建立 /Popup 子註解。Acrobat 以 /Popup 決定註解的註釋視窗位置；
    // 沒有它時註解仍可見，但雙擊不會開出視窗。
    bool createPopup{true};

    // 回覆串（PRD-ANN-007）：父註解的物件編號。設定後會寫 /IRT 與 /RT /R，
    // 並且不再另建 /Popup——回覆共用父註解的視窗，各自帶一個會讓
    // Acrobat 顯示成獨立註解而不是串接。
    std::optional<int> inReplyToObject{};

    // 狀態標記（PRD-ANN-007「已接受／已拒絕／已完成」）。Acrobat 的慣例是
    // 用一則獨立的回覆註解承載狀態，而不是改寫原註解——這樣「誰在什麼時候
    // 把它標成已完成」才留得下來（見 ISO 32000-1 §12.5.6.19 表 172）。
    // 因此 stateModel／state 只在 inReplyToObject 有值時才有意義；
    // 未設定 inReplyToObject 時這兩個欄位會被忽略，不會單獨寫出。
    //
    // /StateModel 目前只支援兩種常見模型："Marked"（None/Marked/Unmarked）與
    // "Review"（None/Accepted/Rejected/Cancelled/Completed）；PRD-ANN-007
    // 需要的「已接受／已拒絕／已完成」屬於 Review 模型。
    std::optional<std::string> stateModel{};
    std::optional<std::string> state{};

    // 就地改寫一則既有的註解（PRD-ANN-009 的屬性面板）。
    //
    // 帶值時不配置新的註解物件編號，也不把它掛上 /Annots——兩者都已經在了。
    // 沿用原本的物件編號是必要的而不是最佳化：回覆串的 /IRT 是指向**物件**
    // 的參照，換一個編號會讓既有的回覆全部變成指向孤兒的參照，而 Acrobat
    // 會把它們顯示成一堆獨立註解而不是一條串。
    //
    // 外觀串流仍然重新產生並配新物件：屬性改了（顏色、線寬、不透明度）
    // 外觀就不同，沿用舊的 /AP 會讓檔案裡的值與畫面不一致——Acrobat 以
    // /AP 為準，其他檢視器以字典為準，於是同一份文件在兩邊長得不一樣。
    std::optional<int> replaceObject{};
    // 就地改寫時原本那則的 /Popup 物件編號（0 或未設代表沒有）。
    std::optional<int> reusePopupObject{};
};

struct AnnotationWriteResult {
    bool ok{false};
    std::string diagnostic;      // 失敗原因；成功時為空
    int annotationObject{0};     // 註解字典的物件編號，可作為回覆的 inReplyToObject
    int appearanceObject{0};     // /AP /N 的 Form XObject
    int popupObject{0};          // 0 代表未建立
};

// 把一則註解寫進指定頁面。所有物件都是新增的，頁面字典（或 /Annots 陣列物件）
// 會被寫出新版本——仍然是純附加，舊版本的位元組留在原處。
[[nodiscard]] AnnotationWriteResult writeAnnotation(IncrementalAppender& appender, int pageIndex,
                                                    const domain::Annotation& annotation,
                                                    const AnnotationWriteOptions& options = {});

}  // namespace alioth::engine::objects
