#include "ui/ribbon/ribbon_default_layout.h"

#include <QCoreApplication>

namespace alioth::ui::ribbon {
namespace {

// 標籤要能被 lupdate 收走（PRD-UI-005 多語系），但這個檔沒有 QObject 可以掛 tr()。
[[nodiscard]] QString label(const char* text) {
    return QCoreApplication::translate("alioth::ui::ribbon", text);
}

// 圖示名稱預設就是動作 id：icon_resolver 認得的名字用的是同一套 id，
// 因此不需要在這裡逐條重複一次，也不會有「id 改了、圖示名忘了改」的落差。
// 認不得的 id 回空圖示，按鈕自動退回純文字。
[[nodiscard]] Item large(const char* id, const char* text, const char* icon = nullptr) {
    Item item;
    item.actionId = QString::fromLatin1(id);
    item.label = label(text);
    item.iconName = icon != nullptr ? QString::fromLatin1(icon) : item.actionId;
    item.size = ItemSize::Large;
    return item;
}

// 小按鈕一律純文字。Office 系 Ribbon 的節奏就是「大按鈕帶圖、小按鈕列文字」——
// 每顆都配圖會讓一個群組裡出現七八個爭搶注意力的小圖，反而更難掃。
[[nodiscard]] Item small(const char* id, const char* text) {
    Item item = large(id, text, "");
    item.size = ItemSize::Small;
    return item;
}

[[nodiscard]] Group group(const char* id, const char* title, QList<Item> items) {
    Group result;
    result.id = QString::fromLatin1(id);
    result.title = label(title);
    result.items = std::move(items);
    return result;
}

[[nodiscard]] Page page(const char* id, const char* title, const char* keyTip,
                        QList<Group> groups) {
    Page result;
    result.id = QString::fromLatin1(id);
    result.title = label(title);
    // 分頁的 KeyTip 手工指定：自動推導會拿到中文標籤的第一個字，那按不出來。
    result.keyTip = QString::fromLatin1(keyTip);
    result.groups = std::move(groups);
    return result;
}

}  // namespace

Layout defaultLayout() {
    Layout layout;
    layout.version = 1;
    layout.quickAccessActionIds = {
        QStringLiteral("file.open"), QStringLiteral("file.save"), QStringLiteral("file.print"),
        QStringLiteral("edit.undo"), QStringLiteral("edit.redo"),
    };

    layout.pages = {
        page("file", "檔案", "F",
             {
                 group("file.document", "文件",
                       {large("file.open", "開啟"), small("file.close", "關閉"),
                        small("file.recent", "最近使用"), small("file.properties", "文件屬性")}),
                 group("file.save", "儲存",
                       {large("file.save", "儲存"), small("file.saveAs", "另存新檔"),
                        small("file.revert", "還原至上次儲存"),
                        small("file.rename", "重新命名")}),
                 group("file.output", "輸出",
                       {large("file.print", "列印"), small("file.printPreview", "預覽列印"),
                        small("file.exportText", "匯出文字"),
                        small("file.exportImage", "匯出影像")}),
                 group("file.create", "建立",
                       {large("file.newBlank", "空白文件"),
                        small("file.newFromText", "從文字檔"),
                        small("file.newFromImages", "從影像檔")}),
                 group("file.share", "傳送",
                       {small("file.email", "以郵件傳送"),
                        small("file.emailAll", "寄出全部開啟的")}),
                 group("file.session", "工作階段",
                       {small("session.save", "儲存工作階段"),
                        small("session.restore", "復原工作階段"),
                        small("app.preferences", "偏好設定"),
                        small("app.manageSettings", "管理設定")}),
             }),

        page("home", "常用", "H",
             {
                 group("home.clipboard", "剪貼簿",
                       {large("edit.copy", "複製"), small("edit.cut", "剪下"),
                        small("edit.paste", "貼上"), small("edit.selectAll", "全選")}),
                 group("home.tools", "工具",
                       {large("tool.select", "選取文字"), large("tool.hand", "手形"),
                        large("tool.selectComments", "選取註解"),
                        small("tool.snapshot", "快照"), small("tool.zoomArea", "區域縮放"),
                        small("tool.persistent", "工具持續模式")}),
                 group("home.markup", "快速標記",
                       {large("annot.highlight", "螢光筆"), small("annot.underline", "底線"),
                        small("annot.strikeout", "刪除線"), small("annot.stickyNote", "便利貼")}),
                 group("home.find", "尋找",
                       {large("search.find", "尋找"), small("search.advanced", "進階搜尋"),
                        small("search.findNext", "找下一個")}),
             }),

        page("view", "檢視", "V",
             {
                 group("view.zoom", "縮放",
                       {large("view.zoomIn", "放大"), large("view.zoomOut", "縮小"),
                        small("view.actualSize", "實際大小"), small("view.fitPage", "符合頁面"),
                        small("view.fitWidth", "符合寬度"),
                        small("view.fitVisible", "符合內容")}),
                 group("view.layout", "版面",
                       {large("view.singlePage", "單頁"), small("view.continuous", "連續"),
                        small("view.twoPage", "雙頁"), small("view.horizontal", "左右排列"),
                        small("view.coverPage", "封面模式")}),
                 // 瀏覽歷史（PRD-NAV-001）。放在檢視分頁而不是常用分頁：
                 // 它改變的是「看哪裡」，與縮放、版面同一類。
                 // 跳頁（對標 PDF-XChange 的 View / Go To）。鍵盤本來就走得到，
                 // 但只有鍵盤走得到等於沒有——使用者要先知道有這回事。
                 group("view.goto", "跳頁",
                       {large("nav.previousPage", "上一頁"),
                        large("nav.nextPage", "下一頁"),
                        small("nav.firstPage", "第一頁"),
                        small("nav.lastPage", "最後一頁"),
                        small("nav.goToPage", "跳至頁碼")}),
                 group("view.navigate", "瀏覽",
                       {large("nav.back", "上一個位置"),
                        large("nav.forward", "下一個位置")}),
                 // 繪圖輔助（PRD-VIEW-015 / 016）。
                 group("view.aids", "輔助",
                       {small("view.rulers", "尺規"), small("view.guides", "參考線"),
                        small("view.clearGuides", "清除參考線"), small("view.grid", "格線"),
                        small("view.snap", "貼齊")}),
                 group("view.rotate", "旋轉",
                       {small("view.rotateClockwise", "順時針旋轉"),
                        small("view.rotateCounterClockwise", "逆時針旋轉")}),
                 group("view.colors", "色彩",
                       {small("view.customColors", "自訂配色"),
                        small("view.resetColors", "回復原始色彩")}),
                 // 十三個面板全部列出。面板改成預設收起之後，沒有出現在這裡的
                 // 面板就等於不存在——先前 標籤／閱讀順序／無障礙檢查器
                 // 就是這樣只能靠「反正一開始就開著」才碰得到。
                 group("view.panels", "導覽面板",
                       {small("view.panel.thumbnails", "縮圖"),
                        small("view.panel.bookmarks", "書籤"),
                        small("view.panel.destinations", "命名目標"),
                        small("view.panel.links", "連結"),
                        small("view.panel.layers", "圖層"),
                        small("view.panel.signatures", "簽章"),
                        small("view.panel.history", "開啟記錄")}),
                 group("view.panels.review", "審閱面板",
                       {small("view.panel.search", "搜尋"),
                        small("view.panel.comments", "註解"),
                        small("view.panel.properties", "註解屬性"),
                        small("view.panel.fields", "欄位"),
                        small("view.panel.attachments", "附件"),
                        small("view.panel.tags", "標籤"),
                        small("view.panel.order", "閱讀順序"),
                        small("view.panel.inspector", "無障礙檢查器"),
                        small("view.panel.panZoom", "Pan & Zoom"),
                        small("view.panel.loupe", "放大鏡")}),
                 group("view.window", "視窗",
                       {large("view.fullScreen", "全螢幕"),
                        small("view.presentation", "簡報模式"),
                        small("view.splitHorizontal", "水平分割"),
                        small("view.splitVertical", "垂直分割"),
                        small("view.classicMenu", "切換傳統選單")}),
             }),

        page("comment", "註解", "C",
             {
                 group("comment.markup", "文字標記",
                       {large("annot.highlight", "螢光筆"), small("annot.underline", "底線"),
                        small("annot.strikeout", "刪除線"), small("annot.squiggly", "波浪線")}),
                 group("comment.notes", "備註",
                       {large("annot.stickyNote", "便利貼"), small("annot.textBox", "文字方塊"),
                        small("annot.callout", "圖說"), small("annot.typewriter", "打字機")}),
                 group("comment.drawing", "繪圖",
                       {large("annot.ink", "鉛筆"), small("annot.rectangle", "矩形"),
                        small("annot.ellipse", "橢圓"), small("annot.line", "直線"),
                        small("annot.arrow", "箭頭"), small("annot.polygon", "多邊形"),
                        small("annot.polyline", "折線"),
                        small("annot.cloud", "雲線")}),
                 group("comment.stamp", "圖章",
                       {large("annot.stamp", "圖章"), small("annot.attachment", "附加檔案"),
                        small("annot.measure", "測量")}),
                 // 逐則瀏覽（對標 PDF-XChange 的 Previous / Next Comment）。
                 // 審一份兩百則註解的文件時，這是最常按的兩個動作。
                 group("comment.navigate", "瀏覽註解",
                       {large("comment.previous", "上一則"), large("comment.next", "下一則"),
                        small("comment.showAll", "顯示所有註解")}),
                 group("comment.manage", "管理",
                       {large("comment.list", "註解清單"), small("comment.reply", "回覆"),
                        small("comment.delete", "刪除註解"), small("comment.setStatus", "設定狀態"), small("comment.summarize", "彙整"),
                        small("comment.import", "匯入"), small("comment.export", "匯出"),
                        small("comment.exportSelected", "匯出選定")}),
             }),

        page("protect", "保護", "P",
             {
                 group("protect.security", "安全性",
                       {large("protect.password", "密碼保護"),
                        small("protect.permissions", "權限設定"),
                        small("protect.removeSecurity", "移除保護")}),
                 group("protect.redact", "塗黑",
                       {large("protect.redactMark", "標記塗黑"),
                        small("protect.redactApply", "套用塗黑"),
                        small("protect.sanitize", "清除隱藏資訊")}),
                 group("protect.signature", "簽章",
                       {large("sign.digitalSign", "數位簽署"), small("sign.certify", "認證文件"),
                        small("sign.validate", "驗證簽章"), small("sign.timestamp", "時間戳記"),
                        small("sign.clearAll", "清除所有簽章欄位")}),
             }),

        page("form", "表單", "M",
             {
                 group("form.fields", "欄位",
                       {large("form.textField", "文字欄位"), small("form.checkBox", "核取方塊"),
                        small("form.radioButton", "選項按鈕"), small("form.comboBox", "下拉方塊"),
                        small("form.listBox", "清單方塊"), small("form.pushButton", "按鈕")}),
                 group("form.signature", "簽章欄位",
                       {large("form.signatureField", "簽章欄位")}),
                 group("form.tools", "表單工具",
                       {large("form.highlightFields", "標示欄位"),
                        small("form.tabOrder", "Tab 順序"), small("form.resetForm", "重設表單"),
                        small("form.importData", "匯入資料"),
                        small("form.exportData", "匯出資料")}),
             }),

        page("organize", "組織", "O",
             {
                 group("organize.pages", "頁面",
                       {large("page.insert", "插入"), small("page.delete", "刪除"),
                        small("page.extract", "擷取"), small("page.replace", "取代"),
                        small("page.move", "移動"), small("page.labels", "頁面標籤"),
                        small("page.insertFromFile", "從檔案插入"),
                        small("page.insertFromText", "插入文字頁面"),
                        small("page.duplicate", "複製頁面")}),
                 group("organize.transform", "版面調整",
                       {large("page.rotate", "旋轉頁面"), small("page.crop", "裁切"),
                        small("page.resize", "調整尺寸")}),
                 group("organize.documents", "文件",
                       {large("document.merge", "合併"), small("document.split", "分割"),
                        small("document.compare", "比較")}),
                 group("organize.stamping", "頁面標記",
                       {large("page.watermark", "浮水印"), small("page.headerFooter", "頁首頁尾"),
                        small("page.background", "背景"), small("page.bates", "Bates 編號"),
                        small("page.removeStamps", "移除所有標記")}),
                 group("organize.bookmarks", "書籤",
                       {large("bookmark.add", "新增書籤"), small("bookmark.manage", "管理書籤"),
                        small("bookmark.expandAll", "全部展開"),
                        small("bookmark.collapseAll", "全部收合")}),
             }),

        page("help", "說明", "Y",
             {
                 group("help.docs", "說明文件",
                       {large("help.contents", "說明主題"), small("help.shortcuts", "快捷鍵一覽"),
                        small("help.releaseNotes", "版本資訊")}),
                 group("help.support", "支援",
                       {large("help.checkUpdates", "檢查更新"),
                        small("help.reportIssue", "回報問題"), small("help.license", "授權資訊"),
                        small("help.about", "關於")}),
             }),
    };
    return layout;
}

}  // namespace alioth::ui::ribbon
