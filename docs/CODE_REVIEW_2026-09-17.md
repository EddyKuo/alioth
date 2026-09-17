# Alioth 程式碼審查與未完成項目（2026-09-17）

審查基準：`edfffac`  
工作區狀態：乾淨  
本文件取代 `docs/CODE_REVIEW_2026-09-10.md` 的狀態判斷；舊文件保留作為歷史紀錄。

## 處理紀錄（2026-09-17，本文件成立後）

本輪六項 findings 全部處理完畢，建議順序 1–3 完成，第 4 項處理掉四個不需要
實機的 R1 缺口，第 5 項寫成可執行的驗收程序（見 `docs/MANUAL_VERIFICATION.md`
新增的第 8–11 節）。

| 項目 | 處置 | 提交 |
|---|---|---|
| P1 CJK 數字 HTML entity | 編成 UTF-8；surrogate／超界／空數字／尾端垃圾明確拒絕 | `986372e` |
| P1 Sprint 狀態文件 | 歷史移到 `sprint/archive/`；數字改由 `tools/sprint_status.py` 產生，納入 ci.bat | `986372e` |
| P2 追蹤矩陣過期敘述 | VIEW-013 / ANN-028 / ENH-004 更新；備註引用的路徑由 CI 檢查是否存在 | `986372e` |
| P2 頁面幾何失敗狀態 | 四態 + 有限次退避重試 | `986372e` |
| P2 永久停用的並行搜尋測試 | 移到 `tests/diagnostics/`（只建置不登錄）；規則改由靜態檢查守 | `986372e` |
| P2 TSA 欄位 | 停用並標示規劃中；controller 的拒絕保留 | `986372e` |
| PRD-UI-002 窄視窗裁切 | 分頁橫向捲動 + QAT 溢位選單 | `549a1f8` |
| PRD-ANN-028 並排版面 | 完成（新增 `mergePageGroups`） | `d8d7298` |
| PRD-PAGE-003 頁面尺寸調整 | 完成（兩種縮放政策，使用者選） | `6880075` |
| PRD-ANN-013 匯出選定註解 | 範圍補齊；仍待 Acrobat 實機互通 | `9b8c3c7` |

CTest 從 182 個登錄（181 執行、1 停用）變成 185 個登錄、185 個執行、零停用。
debug 與 release 兩種組態都全綠。

**沒有處理的，以及為什麼**：R1 剩下的 VIEW-006 / NAV-002 / UI-004 / UI-013 /
A11Y-005 / IO-001 的效能項需要實體硬體或外部程式；VIEW-008 / VIEW-010 /
SEC-002 受限於預編譯 PDFium 的公開 API（見 `.decisions/ADR_003` 與
`exceptions/` 的兩支 open 例外）；IO-006 需要安裝程式、IO-008 需要另一條
列印實作路徑，兩者都是獨立工作包。R2 / R3 的項目依 CLAUDE.md
「不要提前實作」不動。

---

## 結論

目前 Debug 建置成功。CTest 登錄 182 個測試，其中 181 個通過、1 個停用，沒有失敗。9 月 10 日報告中的編譯阻斷、CJK 契約衝突、無作用的 `includeMasks` 選項，以及第 33 頁後永久使用 A4 佔位尺寸等問題，皆已由後續提交修正並補上測試。

專案仍未達到完整 release gate。需求矩陣的 163 個表列項目中，124 個完成、37 個部分完成、2 個經 ADR 排除。部分完成項目中有 16 個屬於 R1；此外 Sprint 狀態和追蹤矩陣仍保留多筆已過期敘述。

## 本輪 code review findings

### P1 — CJK 數字 HTML entity 仍被轉成非法 UTF-8

- 位置：`src/domain/document_source.h:1529-1559`
- 現況：CJK 網頁正文直接使用 UTF-8 時可以走內嵌字型，但 `decodeEntities()` 只接受值小於 128 的數字 entity。`&#20013;` 或 `&#x4E2D;` 會被替換成單一 `0xFF`。
- 影響：兩份視覺內容相同的網頁，`<p>中</p>` 可以成功，`<p>&#20013;</p>` 卻會在 `createPdfFromPlainText()` 被判為無效 UTF-8。數字 entity 是合法且常見的 HTML 表達方式。
- 建議：把合法 Unicode scalar value 編成 UTF-8；拒絕 surrogate、超過 `U+10FFFF`、空數字及尾端垃圾。新增十進位 CJK、十六進位 CJK、輔助平面字元及非法 entity 的端到端測試。

### P1 — Sprint 狀態文件仍無法反映目前狀態

- 位置：`sprint/current/status.md:54`、`:193`、`:214-250`
- 現況：同一文件仍寫著 13 與 24 個測試目標全綠，實際是 182 個登錄、181 個執行通過。文件也仍列「qpdf 結構檢查未接」與「Bates 只印在紙上」，但兩者早已完成。
- 影響：這份文件不能作為 release gate 或排程依據，會造成重複工作與錯誤的完成度判斷。
- 建議：將歷史進度移到封存文件；`sprint/current/status.md` 只保留當前狀態。測試數、停用數與需求統計應由 CI 產生。

### P2 — 追蹤矩陣由最新來源產生，但來源內容本身已過期

- 位置：`tools/traceability.py:102`、`:305-311`、`:399`；輸出位於 `docs/TRACEABILITY.md:55`、`:117`、`:240`
- 現況：重新產生的矩陣與目前檔案逐位元組一致，但 STATUS 表仍有三筆舊描述：
  - PRD-VIEW-013 寫「尚未接上檢視選單」，實際已在 `main_window.cpp` 接上灰階及三種平滑選項。
  - PRD-ANN-028 寫摘要頁限 ASCII；底層文字 PDF 路徑已支援 CJK，但摘要流程及 UI 註解尚未重新驗證與更新。
  - PRD-ENH-004 寫 `/SMask` 會併回 alpha 再輸出；目前實作是在尺寸不變的前提下原樣保留遮罩。
- 影響：自動重生只能防止輸出檔忘記更新，無法防止手動 STATUS 資料與程式碼分歧。
- 建議：更新 STATUS 備註，並讓關鍵敘述對應可執行測試或程式符號；CI 可檢查備註引用的測試檔是否存在。

### P2 — 頁面幾何載入失敗後沒有重試或失敗狀態

- 位置：`src/app/document_controller.cpp:114-142`
- 現況：送出 `pageInfo()` 前先把 `geometryRequested_[i]` 設成 1；若 PDFium 回傳 `nullopt`，函式直接返回，該頁不會再請求，也沒有公開的失敗狀態。
- 影響：頁面載入失敗時會永久顯示 A4 佔位尺寸，呼叫端只能看到 `pageGeometryKnown() == false`，無法區分「仍在載入」和「已失敗」。
- 建議：至少建立 `NotRequested / Pending / Ready / Failed` 狀態；若要重試，使用有限次數與退避，避免損壞頁面形成無限請求。

### P2 — 測試套件仍包含永久停用的並行搜尋測試

- 位置：`tests/CMakeLists.txt:111-121`
- 現況：`test_parallel_search` 會造成 heap corruption，日常測試固定不執行；對應例外已 resolved，ADR 也已決定放棄 PDFium 並行搜尋。
- 影響：CTest 顯示 182 個測試，但實際只有 181 個提供保護；停用項目是缺陷重現器，不是產品行為測試。
- 建議：把破壞性重現器移至 diagnostics，並新增安全測試確認產品不會啟用並行 PDFium 路徑。

### P2 — 簽章對話框提供可輸入但一定失敗的 TSA 欄位

- 位置：`src/ui/sign_dialog.cpp:52-55`、`src/app/signature_controller.cpp:50-55`
- 現況：使用者可以輸入時間戳伺服器網址，但 controller 對任何非空值都直接失敗。底層 RFC 3161 元件已存在，應用層傳輸尚未接上。
- 影響：介面暗示功能可用，使用者完成整份簽章表單後才得知不支援。
- 建議：完成傳輸與同意連網流程前，隱藏或停用欄位並清楚標示規劃狀態；完成後補 TSA 信任、逾時與錯誤回復測試。

## 9 月 10 日問題的處理狀態

| 舊問題 | 現況 | 證據 |
|---|---|---|
| `text_to_pdf.cpp` 缺少 `QList` 完整定義 | 已修正 | `23e8c6f`，Debug 建置通過 |
| CJK 實作與舊測試／契約衝突 | 已修正 | 純文字與網頁 CJK 測試已更新並通過 |
| 第 33 頁後不載入真實尺寸 | 已修正 | `edfffac`，新增 `test_page_geometry` |
| 舊圖磚可能在清除後回填快取 | 已修正 | `renderGeneration_` 守衛及快取清除 |
| `includeMasks` 是無作用選項 | 已移除 | `23e8c6f` |
| 顯示品質功能無直接測試與追蹤更新 | 部分未解 | 功能已接線；追蹤備註及直接 UI/controller 測試仍待補 |
| Sprint 狀態互相矛盾 | 未解 | 仍保留 13／24 個測試與過期缺口 |
| 永久停用的並行搜尋測試 | 未解 | CTest #28 仍 Disabled |

## 尚未完成需求：R1

| ID | 優先 | 尚缺內容 |
|---|---|---|
| PRD-VIEW-006 | M | 不同 DPI 多螢幕的實機重算驗證 |
| PRD-VIEW-008 | S | OCG 面板模型無法控制 PDFium 實際渲染可見性 |
| PRD-VIEW-010 | M | Thin Lines；PDFium 不支援，例外仍為 open |
| PRD-NAV-002 | M | GUI 事件迴圈上的 60 fps 捲動量測 |
| PRD-ANN-013 | S | 匯入／匯出／攤平仍未涵蓋完整需求範圍 |
| PRD-ANN-028 | M | 並排摘要版面未做；CJK 摘要頁需重新驗證 |
| PRD-PAGE-003 | M | 頁面尺寸調整及內容縮放政策 |
| PRD-SEC-002 | S | 無法設定新密碼與權限；例外仍為 open |
| PRD-IO-001 | M | 完整儲存／復原語意與目標機實機效能驗收 |
| PRD-IO-002 | S | PDF 線性化未支援 |
| PRD-IO-006 | M | 安裝階段的系統檔案關聯 |
| PRD-IO-008 | M | 列印內容仍是點陣，未保留向量品質 |
| PRD-UI-002 | M | Ribbon 仍有停用佔位動作及窄視窗裁切問題 |
| PRD-UI-004 | M | Acrobat / PDF-XChange 單鍵快捷鍵實機對齊 |
| PRD-UI-013 | S | 觸控模式完整實機驗收 |
| PRD-A11Y-005 | M | UIA、NSAccessibility、AT-SPI 與螢幕閱讀器實測 |

## 尚未完成需求：R2 / R3

| ID | 版本 | 尚缺內容 |
|---|---|---|
| PRD-VIEW-013 | R2 | Stroke Adjust 只是 PDFium 旗標近似；直接測試與追蹤更新待補 |
| PRD-ANN-016 | R3 | 可辨識與取出媒體，但不播放 |
| PRD-SRCH-002 | R2 | 正規表示式與多文件搜尋未接 UI |
| PRD-SRCH-004 | R2 | 內建拼字字典規模有限 |
| PRD-BM-020 | R3 | 進階書籤巨集只涵蓋部分操作 |
| PRD-FORM-024 | R2 | macOS / Linux Email 表單寄送未實作 |
| PRD-SEC-003 | R3 | 只能套用完全開放的安全性原則 |
| PRD-SIG-005 | R3 | 缺逐欄位批次簽署 UI 與流程 |
| PRD-SIG-006 | R3 | 缺應用層 TSA、TSA 信任驗證、判色整合、DSS 合併及撤銷證據自動蒐集 |
| PRD-SIG-007 | R3 | DocMDP 尚未經 Acrobat 實機驗證 |
| PRD-IO-013 | R3 | `.eml` MIME 解析未做 |
| PRD-IO-014 | R3 | 只有安全的文字擷取降級路徑，沒有完整 HTML/CSS 網頁輸出 |
| PRD-ENH-004 | R2 | target DPI 重取樣及索引色／CMYK／CCITT 等格式未支援 |
| PRD-ENH-006 | R3 | 缺 Separation、DeviceN、ICCBased、Indexed 與混合模式 |
| PRD-ENH-007 | R3 | QR Code 未做 |
| PRD-UI-015 | R2 | 缺企業部署工具與唯讀位置載入 |
| PRD-UI-019 | R2 | 字數統計尚未接 UI |
| PRD-A11Y-001 | R2 | Tags 編輯未做 |
| PRD-A11Y-002 | R3 | 缺真正的幾何閱讀順序 |
| PRD-A11Y-006 | R3 | 一般未標記段落讀不到，翻頁狀態機無測試 |
| PRD-MAC-001 | R3 | 巨集目前只涵蓋旋轉、色彩轉換與條碼 |

## 人工驗證、效能與外部依賴

- Acrobat：註解外觀、PAdES-B、DocMDP 相容性尚需實機驗證。
- 顯示環境：不同 DPI 多螢幕、觸控、Windows UIA 與其他平台輔助技術尚需實機驗證。
- 真實語料：缺 JBIG2/JPEG2000 掃描件、CJK 直排、真實 CAD 病態圖樣，以及至少 300 份相容性語料庫。
- 效能：全文搜尋先前約 4 秒，超過 500 頁 2 秒預算；需以預建文字索引解決，不能重新啟用 PDFium 並行搜尋。
- 發佈指標：捲動幀率、主執行緒阻塞、安裝包大小與超大型文件記憶體仍缺正式量測。
- 開放例外：`EXC_20260906_RD_SA_set_password_unsupported.md`、`EXC_20260906_RD_SA_thin_lines_unsupported.md`。

## 建議順序

1. 修正 CJK 數字 HTML entity，補齊 Unicode scalar 驗證及端到端測試。
2. 清理 Sprint 狀態及追蹤 STATUS 的過期敘述，讓文件重新可用於排程。
3. 為頁面幾何加入明確失敗狀態，整理永久停用的並行搜尋重現器。
4. 優先完成 R1/M 的人工驗證與產品缺口，再排 R2/R3。
5. 建立真實相容性語料庫與目標機效能基準，避免只靠合成測試判斷可交付性。

## 驗證紀錄

- `build.bat windows-x64-debug`：通過。
- `test.bat windows-x64-debug --output-on-failure`：181/181 已執行測試通過；另有 1 個 Disabled。
- `python tools/check_layering.py`：通過。
- 重新執行 `tools/traceability.py`：輸出與 `docs/TRACEABILITY.md` 一致；資料語意過期屬 STATUS 來源問題。
- 工作區在新增本報告前為乾淨狀態；未修改產品程式碼。

