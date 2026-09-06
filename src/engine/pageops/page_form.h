#pragma once

// 把一個頁面包成 Form XObject（WBS 12，PRD-PAGE-007 / 008 / 009 的共同基礎）。
//
// 為什麼不直接把來源頁的內容串流接在目標頁後面：
//
//   內容串流的圖形狀態是流水式的。來源頁 A 若在結尾少了一個 Q（真實檔案裡
//   非常常見，因為單獨一頁時沒人看得出差別），它設定的顏色、線寬、裁切區、
//   CTM 會原封不動外溢到接在後面的來源頁 B。症狀是 B 整片被染色或整個消失，
//   而 A 與 B 單獨開都完全正常——這種缺陷幾乎不可能從錯誤訊息回推原因。
//
//   Form XObject 是規格層級的隔離邊界：呼叫 Do 之前的圖形狀態會在 Do 之後恢復。
//   本檔另外在串流內部再包一層 q/Q，是因為「規格保證」與「每一家檢視器都照做」
//   之間仍有距離，而多這兩個位元組的成本是零。
//
// /Rotate 也在這裡處理。它的語意是「顯示時再轉」，內容串流本身沒有轉；
// 把頁面當素材放進別的頁面時若不自己烘進矩陣，2-up 出來會有一格是倒的。

#include <string>

#include "domain/geometry.h"
#include "domain/page_compose.h"
#include "engine/pageops/compose_document.h"
#include "engine/pageops/object_copier.h"

namespace alioth::engine::pageops {

struct PageFormResult {
    bool ok{false};
    std::string diagnostic;
    int objectNumber{0};

    domain::RectF sourceBox{};  // 來源頁的可見框，座標仍是來源頁的（原點可能非零）
    int rotation{0};

    // 來源頁座標 → 「已套用 /Rotate、原點在 (0,0)」的素材空間。
    // 呼叫端把版面矩陣接在它後面，就得到最終要寫進 cm 的矩陣；
    // 註解幾何套用的必須是**同一個**最終矩陣，否則圖對了標記會跑掉。
    domain::compose::Matrix baseMatrix{};
    domain::SizeF size{};  // 素材空間的尺寸（旋轉 90/270 時寬高已對調）
};

// source 與 dest 可以是同一份文件（合併頁面就是），此時 copier 是恆等的。
[[nodiscard]] PageFormResult makePageFormXObject(const ComposeDocument& source, int sourcePageIndex,
                                                 ComposeDocument& dest, ObjectCopier& copier);

}  // namespace alioth::engine::pageops
