# 與 PDF-XChange Editor 的功能對照

| 項目 | 內容 |
|---|---|
| 對標版本 | PDF-XChange Editor V10 / Plus（官方線上手冊，2026-09-18 查閱） |
| 來源 | `help.pdf-xchange.com` 的 Tabs Guide 各分頁 |
| 用途 | 找出「我們以為做完、實際上對方有而我們沒有」的功能 |

## 這份文件怎麼讀

PDF-XChange 的 Ribbon 有 **15 個分頁**，我們有 8 個。分頁數不同不代表缺 7 個
功能——對方有幾個分頁整個落在我們的範圍之外（Portfolio、Share 的 SharePoint、
Convert 的 OCR 與 Office 轉換），那是 PRD §2.1 的刻意排除，不是遺漏。

因此每一項的判定只有三種：

- **缺口** —— 在我們的範圍內、而且真的沒有。要補。
- **範圍外** —— PRD §2.1 明確排除，或 ADR 已決議不做。附上依據。
- **設計差異** —— 兩邊都做得到同一件事，只是互動方式不同。附上我們為什麼這樣選。

「對方有一顆按鈕、我們有一個選單項」不算缺口。使用者要的是做得到那件事。

---

## 分頁層級的對照

| PDF-XChange 分頁 | 我們 | 判定 |
|---|---|---|
| File | 檔案 | 對應 |
| Home | 常用 | 對應 |
| View | 檢視 | 對應 |
| Comment | 註解 | 對應 |
| Protect | 保護 | 對應 |
| Form | 表單 | 對應 |
| Organize | 組織 | 對應 |
| Bookmarks | （併入「組織」與書籤面板） | 設計差異 |
| Review | （併入「註解」） | 設計差異 |
| Accessibility | （併入「檢視」與三個無障礙面板） | 設計差異 |
| Format | （併入註解屬性面板） | 設計差異 |
| Help | 說明 | 對應 |
| Convert | — | 部分範圍外（OCR、Office 轉換） |
| Share | — | 部分範圍外（SharePoint、DocuSign） |
| Portfolio | — | 範圍外 |

分頁數刻意不追平。八個分頁已經容得下範圍內的全部功能，而分頁一多，
使用者找一個功能要先猜它在哪一頁——那是 Ribbon 最常見的失敗方式。

---

## 缺口（已補上）

| PDF-XChange | 我們原本 | 處置 |
|---|---|---|
| Home / Tools → Select Comments Tool | 沒有。註解只能從清單面板點選 | 補「選取註解」工具 |
| Comment → Previous / Next Comment | 沒有 | 補上下則註解導覽 |
| Comment → Show Comments | 只有清單面板的篩選 | 補「顯示／隱藏所有註解」 |
| View / Go To → First / Prev / Next / Last Page | 只有瀏覽歷史前後 | 補四個跳頁動作 |
| View → Show Page Size / Position | 狀態列只有頁碼與縮放 | 補頁面尺寸與游標座標 |
| Organize / Pages → Duplicate Page | 引擎有 `duplicatePages`，沒有接線 | 接上服務層與選單 |
| Organize / Page Marks → Remove All | 只能加，不能一次移除 | 補「移除所有頁面標記」 |
| Protect → Clear All Signatures | 沒有 | 補清除所有簽章 |
| File → Manage Settings | `settings_profile` 有實作，沒有接線 | 接上匯出／匯入／重設 |
| File / Share → Email All Open Documents | 只能寄目前這一份 | 補寄出全部 |
| Bookmarks → Expand / Collapse All | 沒有 | 補全展開／全收合 |
| File → New Document（空白／文字／影像） | 引擎側三條路徑都完成並測過，但**沒有任何入口** | 補「檔案 → 建立」子選單與 Ribbon 群組 |

全部十二項都已接上選單與 Ribbon 預設配置。Ribbon 的按鈕數從 148 增為 169，
未接上的佔位數維持 14（新增的全部都接了真動作）。

---

## 仍然缺的（在範圍內，但另有相依）

| PDF-XChange | 我們的狀況 |
|---|---|
| File → Open from URL、Convert → From Web Page | 引擎側完成並測過（PRD-IO-010 / 014），但**產品裡沒有 HTTP 用戶端**——`WebPageFetcher` 目前只有測試注入的假實作。要接上得先在平台層做一個 HTTP 傳輸，那是獨立的工作包，且會牽動 PRD §8.2 的連線確認流程 |
| Protect → Digital IDs | 信任存放區（`TrustStore`）可以載入憑證，但沒有「管理」介面，也沒有把使用者選的憑證存進設定。要做得先決定憑證存哪裡與怎麼保護 |
| File → Audit Space Usage | 物件層有完整的解析能力，缺的是分類與統計那一層 |

這三項都不是「忘了做」，是各自缺一個前置決策或前置元件。動手前先把那件事定了。

---

## 範圍外（不做，附依據）

| PDF-XChange | 依據 |
|---|---|
| Home / Objects（Edit Text、Edit Objects、Add Text/Image） | PRD §2.1 排除內文編輯：PDFium 無文字重排能力，需自建排版層 + HarfBuzz，估 320 PD |
| Convert → OCR Pages | ADR-007 決議不做。需 Tesseract，安裝包 +200 MB，上限是 130 MB |
| Convert → To MS-Word / Excel / PowerPoint | PRD §2.1 排除。需 LibreOffice headless（+500 MB）或商用 SDK |
| Convert → Scan | 需 TWAIN 驅動整合；掃描來源不在 R1 範圍 |
| Form → JavaScript Console / Document JavaScript / Document Actions | PRD §8.2 與 ADR-001：V8 已關閉，表單計算走自建受限運算式引擎 |
| Protect → DocuSign（五項） | PRD §2.1 排除，需商務洽談 |
| Protect → Microsoft Purview Sensitivity | 同上 |
| Share → SharePoint（Check In/Out 等） | PRD §2.1 排除 |
| Portfolio 分頁 | PRD §2.1 排除；PDF Portfolio 屬容器格式，與審閱工作站的定位無關 |
| Home / Add → QR Code | PRD-ENH-007，R3。CLAUDE.md：不提前實作 |
| Form → Barcode / Date / Image 欄位 | 條碼欄位屬 R2；日期與影像欄位未列於 PRD 的九種欄位 |
| Protect → Security Policies | PRD-SEC-003，R3 |
| Protect → Multi-Place Signature | PRD-SIG-005，R3 |
| Protect → Timestamp | PRD-SIG-006，R3（UI 已明確標示規劃中） |
| Organize → Background | PRD 列為 R2 |
| Form → Tab Order | R2 |
| Review → Word Count | PRD-UI-019，R2 |

---

## 設計差異（做得到同一件事，互動不同）

**Open / Close All Pop-ups** —— PDF-XChange 讓每則註解各自浮一個視窗，因此需要
「全部開／全部關」。我們一次只開一個註釋視窗，並以註解清單面板承擔「同時看到
很多則」這件事。一份文件有兩百則註解時，兩百個浮動視窗不是功能而是災難；
清單面板可以排序、篩選、搜尋，浮動視窗不行。

**Bookmarks 分頁** —— 對方把書籤的 20 幾個操作放成一個分頁。我們把建立與管理
放在「組織」，其餘批次操作（加前後綴、每 N 頁加書籤、大小寫轉換、目錄頁產生、
合併重複、依書籤排序頁面等 18 項）放在書籤面板的右鍵選單與批次巨集裡——
那些操作一年用不到一次，佔一個分頁的成本是「其他七個分頁每次都要多掃過它」。

**Review 分頁** —— 對方的 Review 與 Comment 兩個分頁有大量重複命令（Add／Delete／
Flatten／Previous／Next 兩邊都有）。我們合併成一個「註解」分頁。

**Accessibility 分頁** —— 對方的 Accessibility 分頁只有色彩覆寫與游標大小。
我們把色彩覆寫放在「檢視 → 自訂配色」、游標大小放在偏好設定，另外多了三個
對方沒有的面板（標籤、閱讀順序、無障礙檢查器）。

**Format 分頁** —— 對方用一個分頁承載選取物件的樣式編輯。我們用註解屬性面板，
它會跟著選取變更即時更新，不需要切分頁。

---

*本文件在功能有增減時更新。判定為「範圍外」的項目若要改做，需先建立 ADR。*
