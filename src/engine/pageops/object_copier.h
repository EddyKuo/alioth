#pragma once

// 跨文件的物件圖複製（WBS 12，PRD-PAGE-008 / 009）。
//
// 覆蓋與取代都要把另一份文件的東西搬進主文件。物件編號在兩份文件裡各說各話，
// 因此不能照抄參照——照抄的結果是新頁面指到主文件裡剛好同號的某個無關物件，
// 而那通常不會報錯，只會顯示成別的東西。
//
// 兩個必須守住的界線：
//
//   1. **同文件時不複製。** 合併頁面是在同一份文件裡進行，資源字典直接沿用
//      原本的參照即可；複製一份只會讓檔案變大，還讓兩份資源日後可能不同步。
//   2. **頁面字典不做整份深拷貝。** 頁面的 /Parent 指回來源文件的頁面樹，
//      /StructParents 牽到整棵結構樹；順著複製會把整份來源文件搬進來。
//      因此頁面一律由呼叫端逐鍵挑選後再交給本類別複製「值」。

#include <map>
#include <string>

#include "engine/objects/pdf_object.h"
#include "engine/redaction/pdf_document_rewriter.h"

namespace alioth::engine::pageops {

using objects::PdfObject;
using objects::PdfRef;
using redaction::PdfDocumentRewriter;

class ObjectCopier {
public:
    ObjectCopier(const PdfDocumentRewriter& from, PdfDocumentRewriter& to) noexcept
        : from_(&from), to_(&to), same_(&from == &to) {}

    // 同一份文件時所有複製都是恆等，呼叫端不需要為兩種情況寫兩條路。
    [[nodiscard]] bool sameDocument() const noexcept { return same_; }

    // 深拷貝一個值，途中遇到的間接參照一律改指到目的文件的新編號。
    [[nodiscard]] PdfObject copyValue(const PdfObject& value);

    // 複製一個間接物件，回傳目的文件的編號；失敗回傳 0。
    // 先佔號再填內容，因此互相指涉的物件（頁面 ↔ 註解的 /P）不會無限遞迴。
    [[nodiscard]] int copyObject(int number);

private:
    [[nodiscard]] PdfObject copyValue(const PdfObject& value, int depth);

    const PdfDocumentRewriter* from_;
    PdfDocumentRewriter* to_;
    std::map<int, int> map_;
    bool same_{false};
};

}  // namespace alioth::engine::pageops
