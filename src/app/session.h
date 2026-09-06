#pragma once

// Session 儲存與復原（PRD-UI-012）。
//
// 「上次關掉時在看什麼」比它聽起來重要：審閱一份 500 頁的工程圖是跨天的工作，
// 每次重開都要自己捲回第 217 頁的話，使用者會改用別的工具。
//
// 這裡刻意分成兩層：Session 是純資料（可序列化、可測試），
// 由誰去套用它是呼叫端的事。原因是套用涉及非同步開檔——
// 把「記住什麼」與「怎麼還原」混在一起會讓兩者都難測。

#include <QString>

#include <cstdint>
#include <vector>

#include "domain/page_layout.h"

namespace alioth::app {

// 一份文件的閱讀狀態。
struct DocumentSession {
    QString path;
    std::int32_t pageIndex{0};
    double scale{1.0};
    domain::LayoutMode layoutMode{domain::LayoutMode::Continuous};
    bool coverPageSeparate{true};
    bool rightToLeft{false};

    [[nodiscard]] bool isValid() const noexcept { return !path.isEmpty(); }
};

// 整個工作階段：這個視窗開著的所有頁籤（PRD-UI-001）。
//
// 檢視狀態（頁碼、縮放、版面）只有 activeIndex 那一份是即時的；其餘頁籤的
// 閱讀位置由 HistoryStore 負責，這裡只記路徑。
struct Session {
    std::vector<DocumentSession> documents;
    std::int32_t activeIndex{0};

    // 視窗幾何與面板佈局由 Qt 的 saveState/saveGeometry 產生的位元組直接保存。
    // 自己解讀那份格式沒有意義，而且 Qt 版本升級時它會自己處理相容性。
    QByteArray windowGeometry;
    QByteArray windowState;

    [[nodiscard]] bool isEmpty() const noexcept { return documents.empty(); }
};

// 讀寫走 QSettings，與偏好設定同一個位置。
// 失敗時回傳空的 Session 而不是拋例外：Session 壞掉不該讓程式開不起來。
[[nodiscard]] Session loadSession();
void saveSession(const Session& session);
void clearSession();

}  // namespace alioth::app
