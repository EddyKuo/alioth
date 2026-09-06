#include "engine/pageops/page_form.h"

namespace alioth::engine::pageops {

PageFormResult makePageFormXObject(const ComposeDocument& source, int sourcePageIndex,
                                   ComposeDocument& dest, ObjectCopier& copier) {
    PageFormResult result;
    PdfRef page{};
    if (!source.pageAt(sourcePageIndex, page)) {
        result.diagnostic = "頁碼超出範圍";
        return result;
    }

    const ContentBytes content = readPageContent(source.rewriter(), page);
    if (!content.ok) {
        result.diagnostic = content.diagnostic.empty() ? "內容串流無法解開" : content.diagnostic;
        return result;
    }

    result.sourceBox = source.cropBox(page);
    result.rotation = source.rotation(page);
    result.baseMatrix = domain::compose::rotationMatrix(result.sourceBox, result.rotation);
    result.size = domain::compose::rotatedSize(result.sourceBox, result.rotation);

    objects::PdfStream stream;
    // 內層 q/Q：見檔頭。規格已保證 Do 前後會存還圖形狀態，這一層是給
    // 「規格照做程度不一」的現實用的，成本是兩個位元組。
    stream.data = "q\n";
    stream.data += content.data;
    stream.data += "\nQ\n";

    stream.dict.set("Type", objects::makeName("XObject"));
    stream.dict.set("Subtype", objects::makeName("Form"));
    stream.dict.set("FormType", PdfObject{static_cast<std::int64_t>(1)});
    // /BBox 的座標系是 form space，而 form space 就是來源頁的座標系
    // （/Matrix 留白等同單位矩陣）。這裡刻意不做原點正規化：正規化在
    // baseMatrix 裡，寫進 /BBox 會變成平移兩次。
    stream.dict.set("BBox", makeRectArray(result.sourceBox));

    const PdfDictionary* pageDict = pageDictionary(source.rewriter(), page);
    if (pageDict != nullptr) {
        if (const PdfObject* resources = pageDict->find("Resources")) {
            stream.dict.set("Resources", copier.copyValue(*resources));
        } else {
            // 攤平時已經把繼承來的 /Resources 下推，走到這裡代表原本就沒有。
            // 空字典比省略好：省略時部分解析器會往 form 的呼叫者找資源，
            // 那正是我們要隔離掉的東西。
            stream.dict.set("Resources", PdfObject{PdfDictionary{}});
        }
        // 透明群組要跟著搬，否則含透明度的頁面疊上去會出現黑底。
        if (const PdfObject* group = pageDict->find("Group")) {
            stream.dict.set("Group", copier.copyValue(*group));
        }
    }

    result.objectNumber = dest.rewriter().addObject(PdfObject{std::move(stream)});
    result.ok = result.objectNumber > 0;
    if (!result.ok) result.diagnostic = "無法建立 Form XObject";
    return result;
}

}  // namespace alioth::engine::pageops
