#pragma once

// 標籤化 PDF 的最小語料（PRD-A11Y-001 / PRD-A11Y-004）。
//
// 自產語料而不依賴外部檔案：無障礙的判定要能在乾淨的 CI 機器上跑，
// 而且「這份檔案到底有沒有標籤」必須由測試自己決定，不能靠某個下載來的
// 樣本碰巧有或沒有——那會讓失敗訊息無法解讀。
//
// 產生器自己算 xref 偏移量，順便驗證我們對檔案結構的理解是對的。

#include <QByteArray>

#include <string>
#include <vector>

namespace alioth::test {

inline std::string toStdString(const QByteArray& bytes) {
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

// 把物件清單組成一份合法的 PDF（傳統 xref 表）。
inline QByteArray assemblePdf(const std::vector<QByteArray>& objects) {
    QByteArray pdf = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::vector<int> offsets;
    offsets.reserve(objects.size());

    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(static_cast<int>(pdf.size()));
        pdf += QByteArray::number(static_cast<int>(i) + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }

    const int xrefOffset = static_cast<int>(pdf.size());
    pdf += "xref\n0 " + QByteArray::number(static_cast<int>(objects.size()) + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (const int offset : offsets) {
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    pdf += "trailer\n<< /Size " + QByteArray::number(static_cast<int>(objects.size()) + 1) +
           " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

// 一份有標籤結構的 PDF：Document > (H1, P, Figure)。
//
// Figure 帶 /Alt，另外再放一個不帶 /Alt 的 Figure——後者正是替代文字
// 檢查要抓到的東西，而「檢查抓不到問題」與「文件真的沒問題」在測試裡
// 必須能分辨，所以語料同時含有合格與不合格的元素。
inline QByteArray makeTaggedPdf() {
    std::vector<QByteArray> objects;
    // 1: Catalog
    objects.push_back(
        "<< /Type /Catalog /Pages 2 0 R /StructTreeRoot 5 0 R "
        "/MarkInfo << /Marked true >> /Lang (zh-TW) >>");
    // 2: Pages
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    // 3: Page
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents 4 0 R "
        "/Resources << >> >>");
    // 4: Contents
    objects.push_back("<< /Length 0 >>\nstream\n\nendstream");
    // 5: StructTreeRoot
    objects.push_back("<< /Type /StructTreeRoot /K 6 0 R >>");
    // 6: Document
    objects.push_back(
        "<< /Type /StructElem /S /Document /P 5 0 R /K [7 0 R 8 0 R 9 0 R 10 0 R] >>");
    // 7: H1
    objects.push_back("<< /Type /StructElem /S /H1 /P 6 0 R /Pg 3 0 R /T (\xE7\xAB\xA0\xE7\xAF\x80) /K 0 >>");
    // 8: P
    objects.push_back("<< /Type /StructElem /S /P /P 6 0 R /Pg 3 0 R /K 1 >>");
    // 9: Figure，有替代文字
    objects.push_back(
        "<< /Type /StructElem /S /Figure /P 6 0 R /Pg 3 0 R /Alt (\xE6\xB5\x81\xE7\xA8\x8B\xE5\x9C\x96) /K 2 >>");
    // 10: Figure，缺替代文字——這是檢查要抓到的那一個
    objects.push_back("<< /Type /StructElem /S /Figure /P 6 0 R /Pg 3 0 R /K 3 >>");
    return assemblePdf(objects);
}

// 沒有 /StructTreeRoot 的一般 PDF。
inline QByteArray makeUntaggedPdf() {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents 4 0 R "
        "/Resources << >> >>");
    objects.push_back("<< /Length 0 >>\nstream\n\nendstream");
    return assemblePdf(objects);
}

// /StructTreeRoot 存在但 /K 指回祖先，形成迴圈。惡意或損毀的檔案會這樣，
// 而遞迴實作碰到它會直接把堆疊吃光。
inline QByteArray makeCyclicStructTreePdf() {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R /StructTreeRoot 5 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents 4 0 R "
        "/Resources << >> >>");
    objects.push_back("<< /Length 0 >>\nstream\n\nendstream");
    objects.push_back("<< /Type /StructTreeRoot /K 6 0 R >>");
    // 6 的子節點是 7，7 的子節點又指回 6。
    objects.push_back("<< /Type /StructElem /S /Document /K 7 0 R >>");
    objects.push_back("<< /Type /StructElem /S /Sect /K 6 0 R >>");
    return assemblePdf(objects);
}

// 有 /StructTreeRoot 但沒有 /K：結構上合法卻不可用，等同未標籤，
// 但修復方式與完全沒有結構樹不同，因此必須能分辨。
inline QByteArray makeEmptyStructTreePdf() {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R /StructTreeRoot 5 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents 4 0 R "
        "/Resources << >> >>");
    objects.push_back("<< /Length 0 >>\nstream\n\nendstream");
    objects.push_back("<< /Type /StructTreeRoot >>");
    return assemblePdf(objects);
}

}  // namespace alioth::test
