# Alioth 程式碼審查與未完成項目

審查日期：2026-09-10  
審查基準：`9cfdae8`，並包含當時工作區 11 個尚未提交的檔案  
範圍：建置流程、目前工作區差異、核心檢視路徑、測試登錄、`TRACEABILITY.md`、Sprint 狀態與例外文件

## 結論

目前版本不可交付：Debug 建置在 `src/engine/create/text_to_pdf.cpp` 編譯失敗，因此 181 個已登錄測試無法執行。除此之外，大型或混合頁面尺寸文件在第 33 頁之後會永久使用 A4 估算值，會造成版面、命中座標與圖磚範圍錯誤。

需求矩陣顯示 163 個表列需求中有 124 個完成、37 個部分完成、2 個經 ADR 排除；沒有「未開始」項目。不過「部分完成」仍包含多個 R1/M 級缺口，不能把 76% 的完成率解讀成已達到 R1 發佈條件。

## Code review findings

### P0 — 工作區無法編譯，所有測試被阻斷

- 位置：`src/engine/create/text_to_pdf.cpp:112`、`:132`
- 重現：`build.bat windows-x64-debug`
- 結果：MSVC 回報 `QList<uint>` 是未定義型別；`QString::toUcs4()` 的回傳值需要完整的 `QList` 定義。
- 影響：Ninja 停在 `alioth_create`，應用程式和測試執行檔均無法完成建置。
- 建議：補上直接相依的 Qt container include，重新跑完整建置與測試。不要依賴其他標頭偶然傳遞 include。

### P1 — CJK 行為已改成成功，但既有契約與測試仍要求失敗

- 位置：`src/engine/create/text_to_pdf.h:8-15`、`tests/create/test_text_import.cpp:98-105`、`tests/create/test_web_page_import.cpp:120`、`src/engine/create/web_page_to_pdf.cpp:61`
- 現況：`createPdfFromPlainText()` 已加入 CJK 字型子集並嘗試成功輸出；標頭仍宣告「非 ASCII 明確失敗」，測試也斷言中文輸入必須失敗。
- 影響：修正 P0 後，舊測試預期將與新實作衝突；上層錯誤訊息也仍把失敗歸因於 CJK/ASCII 限制。這讓 API 使用者無法判斷哪個契約才正確。
- 建議：先決定 CJK 現在是否正式支援。若是，更新契約、上層診斷與測試，新增中文、混合拉丁/CJK、換行、換頁、缺字與無字型環境的端到端案例，並用 PDFium 文字擷取及 qpdf 驗證輸出。

### P1 — 第 33 頁之後從未載入真實頁面尺寸

- 位置：`src/app/document_controller.cpp:96-115`
- 證據：`DocumentController` 只對前 32 頁呼叫 `pageInfo()`；註解聲稱其餘頁面會在捲動接近時補載，但 `src/app` 與 `src/ui` 沒有其他 `pageInfo()` 呼叫。
- 影響：第 33 頁起永久以 A4 `595 × 842 pt` 代替真實尺寸。Letter、A3、橫向頁或混合尺寸 PDF 會產生錯誤的頁面間距、捲動位置、座標轉換及圖磚邊界。
- 建議：增加按需幾何載入 API，由可視頁範圍觸發並去重請求；用至少 40 頁且第 33 頁尺寸不同的文件做回歸測試。

### P1 — 進度文件互相矛盾，無法作為 release gate

- 位置：`sprint/current/status.md:54`、`:193`、`:214-250`
- 現況：同一文件同時宣稱 13 與 24 個測試目標全綠，實際 `ctest -N` 登錄 181 個測試（其中 1 個停用）。已知缺口仍寫「qpdf 未接」及「Bates 未寫進文件」，但文件後段與 `TRACEABILITY.md` 已宣稱兩者完成。
- 影響：專案狀態可能讓維護者誤判已完成項目、重複投入，或錯放 release gate。
- 建議：把 Sprint 狀態改成當期摘要，歷史紀錄移至獨立封存檔；測試數量與需求統計由 CI 產生，避免手寫數字。

### P2 — 影像重壓縮設定已成為無作用的公開選項

- 位置：`src/domain/enhance.h:249-255`、`tests/enhance/test_recompress.cpp:271-310`
- 現況：`includeMasks` 的 true/false 現在走完全相同行為；`masksLosslessOnly` 也沒有被重壓縮路徑讀取。新增的資料驅動測試反而固定兩種設定都保留遮罩。
- 影響：呼叫端看到可設定欄位，實際上無法改變行為，容易形成錯誤期待；報告的 `originalBytes` 也不再計入遮罩，與舊語意不同。
- 建議：若尺寸不變時確定只能保留遮罩，移除或標記 deprecated，待重取樣功能實作時再加入；同步修正 `TRACEABILITY.md` 中「/SMask 併回」的舊描述。

### P2 — 新增功能沒有對應的直接測試與追蹤更新

- 位置：`src/ui/main_window.cpp:1286-1307`、`src/app/document_controller.cpp:426-436`、`docs/TRACEABILITY.md:72`
- 現況：灰階、線條／文字／影像平滑選項已接進選單與 Ribbon，但沒有測試 UI 勾選值到 `RenderOptions` 的對映，也沒有測試切換後清掉舊圖磚及拒收舊的非同步渲染結果。矩陣仍寫「尚未接上檢視選單」。
- 影響：布林反向語意（`smoothPaths` 對 `strokeAdjust`）很容易在維護時接反；追蹤文件也低估實際進度。
- 建議：加入 controller 層選項／generation 回歸測試及最小 UI wiring 測試，完成後重生追蹤矩陣。

### P2 — 測試套件保留一個永久停用項目

- 位置：`tests/CMakeLists.txt:111-121`
- 現況：`test_parallel_search` 因可能造成 heap corruption 而標成 `DISABLED`；對應例外已標記 resolved，ADR 則決定放棄並行搜尋。
- 影響：測試名稱仍像正式產品能力，且日常 CI 永遠不會驗它。這是風險記錄，不是有效的自動化保護。
- 建議：把破壞性重現程式移到明確的 diagnostic/unsupported 區，或改成安全地驗證「並行路徑不會被產品啟用」，使預設 suite 不含永久停用測試。

## 尚未完成的需求

以下以 `docs/TRACEABILITY.md` 的 37 個「部分」項目為準。完整限制與證據仍以該矩陣各列備註為準。

### R1 且優先級 M：先處理

| ID | 尚缺內容 |
|---|---|
| PRD-VIEW-006 | 不同 DPI 的多螢幕實機重算驗證 |
| PRD-VIEW-010 | Thin Lines；PDFium 不支援，現有例外仍為 open |
| PRD-NAV-002 | 60 fps 捲動的 GUI 事件迴圈量測 |
| PRD-ANN-028 | 註解摘要並排版面；既有 ASCII 限制描述亦需配合 CJK 改動重審 |
| PRD-PAGE-003 | 頁面尺寸調整及內容縮放政策 |
| PRD-IO-001 | 功能大致完成，但需求仍因完整儲存／復原語意與實機效能驗收列為部分 |
| PRD-IO-006 | 安裝階段的系統檔案關聯 |
| PRD-UI-002 | Ribbon 仍有停用佔位動作及窄視窗裁切問題 |
| PRD-UI-004 | Acrobat / PDF-XChange 單鍵快捷鍵實機對齊 |
| PRD-A11Y-005 | Windows UIA、macOS NSAccessibility、Linux AT-SPI 與螢幕閱讀器實測 |

### R1 其他缺口

| ID | 尚缺內容 |
|---|---|
| PRD-VIEW-008 | OCG 面板能改模型，無法改 PDFium 實際渲染可見性 |
| PRD-ANN-013 | 仍有部分匯入／匯出或攤平範圍未達完整需求 |
| PRD-SEC-002 | 無法設定新密碼與權限；例外仍為 open |
| PRD-IO-002 | PDF 線性化未支援 |
| PRD-IO-008 | 列印內容為點陣，未保留向量品質 |
| PRD-UI-013 | 觸控模式仍需完整實機驗收 |

### R2 / R3 功能缺口

- 搜尋：PRD-SRCH-002 的正規表示式與多文件搜尋未接 UI；PRD-SRCH-004 的字典規模有限。
- 書籤與批次：PRD-BM-020 只涵蓋部分巨集；PRD-MAC-001 目前只有旋轉、色彩轉換、條碼。
- 表單與安全：PRD-FORM-024 尚未移植 macOS/Linux Email；PRD-SEC-003 只能套用完全開放政策。
- 簽章：PRD-SIG-005 缺逐欄位批次簽署流程；PRD-SIG-006 缺 TSA 信任驗證、判色整合、既有 DSS 合併及撤銷證據自動蒐集；PRD-SIG-007 尚未經 Acrobat 驗證 DocMDP 相容性。
- 建立與輸出：PRD-IO-013 缺 `.eml` MIME 解析；PRD-IO-014 只有文字擷取降級路徑，沒有完整 HTML/CSS 網頁輸出。
- 影像與條碼：PRD-ENH-004 缺 target DPI 重取樣及多種色彩／壓縮格式；PRD-ENH-006 缺 Separation、DeviceN、ICCBased、Indexed 與混合模式；PRD-ENH-007 缺 QR Code。
- UI：PRD-UI-015 缺部署工具與唯讀位置載入；PRD-UI-019 字數統計未接 UI。
- 無障礙：PRD-A11Y-001 缺 Tags 編輯；PRD-A11Y-002 缺真正的幾何閱讀順序；PRD-A11Y-006 一般未標記段落讀不到且翻頁狀態機無測試。
- 媒體：PRD-ANN-016 可辨識與取出內嵌媒體，但刻意不播放。

## 尚待人工驗證或產品決策

- Acrobat：註解外觀、簽章與 DocMDP 相容性；步驟見 `docs/MANUAL_VERIFICATION.md`。
- 真實語料：目前效能數據仍來自合成語料；缺 JBIG2/JPEG2000 掃描件、CJK 直排、真實 CAD 病態圖樣及至少 300 份相容性語料庫。
- 效能：全文搜尋先前約 4 秒，超過 500 頁 2 秒預算；並行方案已因 PDFium 執行緒安全問題放棄，需以預建文字索引解決。
- 發佈指標：尚缺捲動幀率、主執行緒阻塞、安裝包大小與超大型文件記憶體的正式量測。
- 開放例外：`EXC_20260906_RD_SA_set_password_unsupported.md`、`EXC_20260906_RD_SA_thin_lines_unsupported.md`。

## 建議執行順序

1. 修正 P0 編譯問題，統一 CJK 契約、上層訊息與測試，跑完 181 個測試。
2. 修正第 33 頁後的按需頁面幾何載入，加入混合尺寸長文件回歸測試。
3. 更新或重寫 Sprint 狀態，讓 CI 自動寫入測試數與需求統計。
4. 完成 R1/M 的人工驗證與缺口；無法實作的項目需有 ADR/例外及產品可見的降級說明。
5. 再排 R2/R3 的 21 個部分完成項目，避免以功能數量取代 release gate。

## 驗證紀錄

- `git diff --check`：沒有 whitespace error；只有 Git 的 LF/CRLF 提示。
- `test.bat windows-x64-debug -N`：登錄 181 個測試，`test_parallel_search` 停用。
- `build.bat windows-x64-debug`：失敗，原因為 `text_to_pdf.cpp` 缺少 `QList<uint>` 完整定義。
- 完整測試：未執行，因建置失敗而被阻斷。

