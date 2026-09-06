#pragma once

// 註解的剪貼簿格式與跨文件複製（PRD-ANN-011）。
//
// 剪貼簿上同時放兩種東西：
//   1. 自訂 MIME 型別，內容是 XFDF。這是 Alioth 之間（含跨文件、跨視窗）
//      真正用來還原註解的那一份——幾何、顏色、作者、時間全都在。
//   2. text/plain，內容是人看得懂的摘要。貼到郵件或聊天視窗時得到的是
//      一段可讀的文字，而不是一坨 XML。
//
// 用 XFDF 而不是自創格式，是因為它已經是這個專案裡「一批註解」的序列化形式
// （app/xfdf_io.h），另立一套只會讓同一件事有兩份實作，而分歧的那一份
// 遲早會少寫一個鍵。
//
// 只認自己放上去的那個 MIME 型別。別的程式放的 XFDF 檔案要走「匯入註解」
// 那條路：那裡有大小上限與 DOCTYPE 拒絕，剪貼簿路徑不該繞過它們。

#include <QString>

#include <vector>

#include "app/xfdf_io.h"

class QMimeData;

namespace alioth::app {

// 自訂型別名稱。帶 +xfdf 尾綴讓人一眼看出裡面是什麼。
[[nodiscard]] QString annotationMimeType();

// 產生可直接交給 QClipboard::setMimeData() 的物件（所有權歸呼叫端／剪貼簿）。
// entries 為空時回傳 nullptr——放一份空的剪貼簿只會讓「貼上」看起來可用。
[[nodiscard]] QMimeData* makeAnnotationMimeData(const std::vector<XfdfEntry>& entries,
                                                const QString& sourceFilename = {});

// 從剪貼簿讀回註解。沒有本程式的型別時回傳空 vector（不去猜 text/plain 的內容）。
[[nodiscard]] std::vector<XfdfEntry> annotationsFromMimeData(const QMimeData* mime);

}  // namespace alioth::app
