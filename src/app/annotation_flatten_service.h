#pragma once

// 註解攤平（PRD-ANN-013 的「攤平」半段）。
//
// 攤平＝把註解的外觀燒進頁面內容，並把 /Annots 裡的參照摘掉。做完之後那些
// 標記在任何檢視器裡都只是頁面的一部分，選不到、改不了、也匯不出去。
//
// 兩個決定值得寫下來：
//
//   **走增量附加，不是整份重寫。** 攤平在語意上不可逆（註解沒得編了），但在
//   檔案層次上它仍然是純附加，所以復原還是「把檔案截回原長度」。這讓「攤平
//   之後發現攤錯了」有救，也讓既有簽章維持在「有效，簽章後有變更」而不是無效。
//   註解物件本身留在檔案裡不刪——附加式寫入本來就不能刪東西。
//
//   **不支援的子型不算失敗，但要說出來。** Widget（表單欄位）、Popup、Stamp
//   讀不回領域模型，攤平時只能略過。靜靜跳過會讓使用者以為整份都攤平了，
//   於是把檔案寄出去——所以略過幾則要出現在回傳訊息裡。

#include <QObject>
#include <QString>

#include "app/annotation_service.h"
#include "domain/redaction.h"

namespace alioth::app {

class AnnotationFlattenService : public QObject {
    Q_OBJECT

public:
    explicit AnnotationFlattenService(QObject* parent = nullptr);

    struct FlattenSummary {
        int flattened{0};
        int skipped{0};  // 讀不回領域模型的子型（Widget / Popup / Stamp…）
    };

    // 這份文件目前有幾則可攤平的註解、有幾則會被略過。UI 用它決定選單要不要
    // 啟用，以及在確認對話框裡說清楚會影響幾則——「確定要攤平嗎」不說數量，
    // 使用者沒有辦法判斷自己是不是開錯了檔案。
    [[nodiscard]] FlattenSummary preview(const QString& path) const;

    // 攤平整份文件的所有註解。需要 IrreversibleConsent：呼叫端必須先問過使用者。
    [[nodiscard]] HighlightResult flattenAll(const QString& path, domain::IrreversibleConsent);
};

}  // namespace alioth::app
