#pragma once

// 表單測試語料產生器。
//
// 與 tests/pdf_fixture.h 同一個立場：語料自產、不依賴外部檔案，
// 讓引擎測試能在乾淨的 CI 機器上跑。差別是這裡要造出帶 AcroForm 的文件，
// 因此每個 widget 都必須有 /AP /N——沒有外觀串流的 widget，
// PDFium 會當成殘缺欄位處理，測到的行為就不是真實情況。

#include <QByteArray>

#include <vector>

namespace alioth::test {

// 依物件清單組出完整 PDF 並自算 xref 偏移量。物件編號由 1 起算，
// 對應 objects[0]。呼叫端自行維持交互參照的一致性。
inline QByteArray assemblePdf(const std::vector<QByteArray>& objects, const QByteArray& trailerExtra) {
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
           " /Root 1 0 R" + trailerExtra + " >>\nstartxref\n" +
           QByteArray::number(xrefOffset) + "\n%%EOF\n";
    return pdf;
}

inline QByteArray streamObject(const QByteArray& dictBody, const QByteArray& content) {
    return "<< " + dictBody + " /Length " + QByteArray::number(content.size()) + " >>\nstream\n" +
           content + "\nendstream";
}

// 一頁 300×500 的表單：文字欄位、核取方塊、下拉方塊、清單方塊、單選群組（兩顆）。
//
// 單選群組刻意做成「一個欄位、兩個 widget」的標準形狀，因為那正是
// 「用欄位名定位會改到錯的按鈕」這個陷阱的來源，測試必須覆蓋它。
inline QByteArray makeAcroFormPdf() {
    const QByteArray pageContent = "0.9 0.9 0.9 rg\n0 0 300 500 re\nf\n";

    const QByteArray formResources = "/Resources << /Font << /Helv 5 0 R >> >>";
    const QByteArray textAp =
        "q BT /Helv 12 Tf 0 g 2 5 Td (Ada) Tj ET Q";
    const QByteArray onAp = "q 0 0 0 rg 2 2 16 16 re f Q";
    const QByteArray offAp = "q 1 1 1 rg 0 0 20 20 re f Q";
    const QByteArray comboAp = "q BT /Helv 12 Tf 0 g 2 5 Td (Taipei) Tj ET Q";
    const QByteArray listAp = "q BT /Helv 12 Tf 0 g 2 55 Td (zh) Tj ET Q";

    std::vector<QByteArray> objects;
    objects.push_back(
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R 7 0 R 8 0 R 9 0 R 10 0 R] "
        "/DA (/Helv 0 Tf 0 g) /DR << /Font << /Helv 5 0 R >> >> >> >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 500] /Contents 4 0 R "
        "/Resources << /Font << /Helv 5 0 R >> >> /Annots [6 0 R 7 0 R 8 0 R 9 0 R 11 0 R 12 0 R] >>");
    objects.push_back(streamObject("", pageContent));
    objects.push_back(
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");

    // 6：文字欄位
    objects.push_back(
        // /TU 用 UTF-16BE 十六進位字串。PDF 的字面字串沒有編碼宣告，
        // 直接塞 UTF-8 位元組會被當成 PDFDocEncoding 解讀成亂碼。
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (fullName) /TU <FEFF59D3540D> "
        "/V (Ada) /DV (Ada) /Ff 0 /F 4 /Rect [20 440 200 460] /DA (/Helv 12 Tf 0 g) "
        "/P 3 0 R /AP << /N 13 0 R >> >>");
    // 7：核取方塊。/AS 決定目前畫哪一個外觀，缺了它 PDFium 會拒絕切換狀態。
    objects.push_back(
        "<< /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /V /Off /DV /Off /AS /Off /F 4 "
        "/Rect [20 400 40 420] /P 3 0 R /MK << /BC [0 0 0] >> "
        "/AP << /N << /Yes 14 0 R /Off 15 0 R >> >> >>");
    // 8：下拉方塊（/Ff 位元 18 = Combo）
    objects.push_back(
        "<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 131072 /T (city) /V (Taipei) /DV (Taipei) "
        "/Opt [(Taipei) (Tokyo) (Seoul)] /F 4 /Rect [20 350 200 370] /DA (/Helv 12 Tf 0 g) "
        "/P 3 0 R /AP << /N 16 0 R >> >>");
    // 9：清單方塊（複選，/Ff 位元 22 = MultiSelect）
    objects.push_back(
        "<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 2097152 /T (langs) /V (zh) "
        "/Opt [(zh) (en) (ja)] /F 4 /Rect [20 260 200 330] /DA (/Helv 12 Tf 0 g) "
        "/P 3 0 R /AP << /N 17 0 R >> >>");
    // 10：單選群組欄位本體（/Ff 位元 16 = Radio），widget 在 11 / 12
    objects.push_back(
        "<< /FT /Btn /Ff 32768 /T (gender) /V /Off /DV /Off /Kids [11 0 R 12 0 R] >>");
    objects.push_back(
        "<< /Type /Annot /Subtype /Widget /Parent 10 0 R /AS /Off /F 4 /Rect [20 210 40 230] "
        "/P 3 0 R /MK << /BC [0 0 0] >> /AP << /N << /F 18 0 R /Off 19 0 R >> >> >>");
    objects.push_back(
        "<< /Type /Annot /Subtype /Widget /Parent 10 0 R /AS /Off /F 4 /Rect [60 210 80 230] "
        "/P 3 0 R /MK << /BC [0 0 0] >> /AP << /N << /M 20 0 R /Off 21 0 R >> >> >>");

    objects.push_back(streamObject(
        "/Type /XObject /Subtype /Form /BBox [0 0 180 20] " + formResources, textAp));
    objects.push_back(streamObject("/Type /XObject /Subtype /Form /BBox [0 0 20 20]", onAp));
    objects.push_back(streamObject("/Type /XObject /Subtype /Form /BBox [0 0 20 20]", offAp));
    objects.push_back(streamObject(
        "/Type /XObject /Subtype /Form /BBox [0 0 180 20] " + formResources, comboAp));
    objects.push_back(streamObject(
        "/Type /XObject /Subtype /Form /BBox [0 0 180 70] " + formResources, listAp));
    objects.push_back(streamObject("/Type /XObject /Subtype /Form /BBox [0 0 20 20]", onAp));
    objects.push_back(streamObject("/Type /XObject /Subtype /Form /BBox [0 0 20 20]", offAp));
    objects.push_back(streamObject("/Type /XObject /Subtype /Form /BBox [0 0 20 20]", onAp));
    objects.push_back(streamObject("/Type /XObject /Subtype /Form /BBox [0 0 20 20]", offAp));

    return assemblePdf(objects, {});
}

// 無表單文件。用來確認「有 /Annots 不等於有表單」——
// 誤報有表單會讓 UI 對每份純文件都亮起表單工具列。
inline QByteArray makeNoFormPdf() {
    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 400] /Contents 4 0 R "
        "/Resources << >> /Annots [5 0 R] >>");
    objects.push_back(streamObject("", "0 0 0 rg\n0 0 100 200 re\nf\n"));
    objects.push_back(
        "<< /Type /Annot /Subtype /Square /Rect [10 10 60 60] /C [1 0 0] /F 4 /P 3 0 R >>");
    return assemblePdf(objects, {});
}

// XFA 文件。dynamic 為真時加上 /NeedsRendering，PDFium 會回報 FORMTYPE_XFA_FULL；
// 為假則是 XFAF（疊加）。兩者的降級提示不同，必須分開測。
inline QByteArray makeXfaPdf(bool dynamic, bool withAcroFallback) {
    const QByteArray xfaPacket = "<?xml version=\"1.0\"?><xdp:xdp xmlns:xdp=\"http://ns.adobe.com/xdp/\"></xdp:xdp>";
    const QByteArray fields = withAcroFallback ? "[6 0 R]" : "[]";
    const QByteArray annots = withAcroFallback ? " /Annots [6 0 R]" : "";

    std::vector<QByteArray> objects;
    objects.push_back("<< /Type /Catalog /Pages 2 0 R" +
                      QByteArray(dynamic ? " /NeedsRendering true" : "") +
                      " /AcroForm << /Fields " + fields +
                      " /XFA [ (form) 5 0 R ] /DA (/Helv 0 Tf 0 g) >> >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 500] /Contents 4 0 R "
                      "/Resources << >>" + annots + " >>");
    objects.push_back(streamObject("", "1 1 1 rg\n0 0 300 500 re\nf\n"));
    objects.push_back(streamObject("", xfaPacket));
    if (withAcroFallback) {
        objects.push_back(
            "<< /Type /Annot /Subtype /Widget /FT /Tx /T (fallbackName) /V (fallback) /F 4 "
            "/Rect [20 440 200 460] /DA (/Helv 12 Tf 0 g) /P 3 0 R /AP << /N 7 0 R >> >>");
        objects.push_back(streamObject("/Type /XObject /Subtype /Form /BBox [0 0 180 20]",
                                       "q BT 0 g ET Q"));
    }
    return assemblePdf(objects, {});
}

}  // namespace alioth::test
