#pragma once

// 註解攤平(PRD-ANN-013 的「攤平」半段)。
//
// 攤平是不可逆操作:註解一旦燒進頁面內容串流,原本可編輯的 /Annot 物件
// 語意上就沒有意義了(即使它仍然留在檔案裡)。因此入口函式要求呼叫端
// 提供 domain::IrreversibleConsent——與 engine/redaction 的既有慣例一致,
// 讓「使用者已經明確同意」在原始碼裡留下一個看得到的字面痕跡。
//
// 實作手法:攤平後的內容不透過 /AP Form XObject 間接引用,而是直接把
// 外觀串流的位元組內嵌進頁面的 /Contents。外觀產生器的座標約定本來就是
// 「直接以頁面預設使用者空間作圖」(engine/annotations/appearance_stream.h),
// 因此不需要額外的座標轉換,只有 /Text(便利貼圖示)的反向旋轉矩陣需要
// 補一個 cm。這個做法比「建一個新的 Form XObject 再 Do 它」更省一個間接層,
// 也讓攤平後的內容能直接用文字擷取/內容串流檢查驗證(見 SDD §8 測試策略)。
//
// flattenAnnotation() 只做「把外觀燒進頁面內容」這一半;要讓檢視器不再把它
// 當成可編輯註解,還得把 /Annots 裡的參照摘掉,那是 flattenAndDetachAnnotation()。
// 兩個函式分開是因為「燒進去」對新產生的註解也成立(那時根本還沒有 /Annots 項目)。

#include <string>

#include "domain/annotation.h"
#include "domain/geometry.h"
#include "domain/redaction.h"
#include "engine/objects/incremental_appender.h"

namespace alioth::engine::objects {

struct FlattenOptions {
    domain::Rotation pageRotation{domain::Rotation::None};
};

struct FlattenResult {
    bool ok{false};
    std::string diagnostic;
    int contentObject{0};  // 新增的內容串流物件編號
    bool detached{false};  // 是否已從頁面的 /Annots 移除參照
};

// 把一則註解的外觀燒進指定頁面的內容串流。consent 的存在本身就是呼叫端
// 已確認使用者同意攤平的證明,函式不再額外檢查。
[[nodiscard]] FlattenResult flattenAnnotation(IncrementalAppender& appender, int pageIndex,
                                              const domain::Annotation& annotation,
                                              domain::IrreversibleConsent consent,
                                              const FlattenOptions& options = {});

// 攤平既有註解:燒進頁面內容,並把 /Annots 裡的參照摘掉。
//
// annotationObjectNumber 是該註解在檔案裡的物件編號(來自 annotation_reader
// 或 pageAnnotationRefs)。被指向的物件本身不刪除——附加式寫入不能刪東西,
// 留著它也讓「復原」仍然只是把檔案截回原長度(ADR-002)。
//
// 順序是先燒後摘。反過來的話,外觀產生失敗時註解已經從 /Annots 消失,
// 結果是那則註解人間蒸發,比完全沒攤平糟得多。
[[nodiscard]] FlattenResult flattenAndDetachAnnotation(IncrementalAppender& appender,
                                                       int pageIndex,
                                                       const domain::Annotation& annotation,
                                                       int annotationObjectNumber,
                                                       domain::IrreversibleConsent consent,
                                                       const FlattenOptions& options = {});

}  // namespace alioth::engine::objects
