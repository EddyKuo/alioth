# Alioth 呈現層操作流程審查（2026-09-19）

審查基準：`9815ff9` 加上本工作階段尚未提交的變更（縮圖委派、關檔清理、縮圖點擊修正；
見文末「本次一併修掉的」）。
範圍：`src/ui/` 全部（`main_window.cpp`、`page_view.cpp`、十三個面板、九個對話框、
Ribbon 預設配置與綁定），以及它們呼叫到的 `src/app/` 介面。
方法：逐一追每個 QAction／Ribbon id／面板訊號「按下去之後走到哪、寫了什麼、畫面怎麼變」。
以靜態閱讀為主；標示「已驗證」的項目是用 grep 或測試確認過的，其餘是從程式碼推導。

操作流程本身（每個按鈕、每個面板、每個手勢做什麼）另寫成 `docs/UI_OPERATIONS.md`，
本文件只列缺陷。兩份文件用同一組編號（UI-xx）互相參照。

## 處理紀錄（2026-09-19，本文件成立後）

**32 項全部處理完畢**，debug 與 release 兩種組態各跑三次全綠（191 個測試，
比審查前多 1 個測試目標、多 14 條測試案例）。

| 編號 | 處置 |
|---|---|
| UI-01 | `DocumentController::reloadDocument()` 與 `documentReloaded` 訊號；`PageView` 與 `MainWindow` 各自只更新該更新的部分 |
| UI-02 | `applyTextMarkup` 以值取一份 Selection |
| UI-03 | `requestAnnotations` 改成增量（每頁一個「掃過了」旗標）；`pageChanged` 時補目前頁前後 16 頁 |
| UI-04 | `RibbonGroupWidget::rebind` 對指向子選單的動作 `setMenu` + `InstantPopup` |
| UI-05 | 新增真正的「移動頁面」（範圍 + 目的位置）；N 頁併一頁改用 `page.nUp` |
| UI-06 | 縮圖拖放延到事件迴圈下一輪，先還原清單再 `confirmRewrite` |
| UI-07 | `WindowRole`：分離視窗不還原也不寫入工作階段 |
| UI-08 | 偏好設定的「確定」不再呼叫 `restoreSession()` |
| UI-09 | 新增 `AnnotationService::updateAnnotation`（沿用物件編號、重產 /AP、拒絕改寫回覆）；面板接上選取與寫回 |
| UI-10 | `fieldsReady` 餵矩形給檢視區；`pageChanged` 餵目前頁給欄位面板 |
| UI-11 | Esc 集中成一個處理器：自動捲動 → 進行中的手勢 → 簡報模式 |
| UI-12 | 工作階段、區域縮放、安全性與權限、移除密碼補上實作；認證文件、時間戳、Tab 順序、背景、說明四項從預設配置移除。棘輪門檻 14 → **0** |
| UI-13 | 訊息先取原檔名；原頁籤關掉 |
| UI-14 | 八個頁面操作改成「先收參數，最後才確認重寫」 |
| UI-15 | `annot.highlight` 改成工具；套用到選取的動作獨立為 `annot.highlightSelection` |
| UI-16 | 三個旋轉動作改成可輸入範圍，預設目前頁 |
| UI-17 | 「新增書籤」改名「加入閱讀標記」並移到瀏覽群組；`bookmark.manage` 改稱「書籤面板」 |
| UI-18 | 檢視分頁新增「顯示」群組（朗讀、自動捲動、灰階、平滑文字、三種主題）；視窗群組補四格分割與自訂 Ribbon |
| UI-19 | 權限檢查移到 `commitAnnotation` 唯一入口；灰化改以 `annot.` 前綴掃描 |
| UI-20 | `PrintService::print` 增加每張紙的回呼；UI 接上可取消的進度對話框 |
| UI-21 | 同一份文件上的閱讀標記直接跳頁 |
| UI-22 | 圖章樣式選擇對話框（14 種，記住上次選擇） |
| UI-23 | 每個頁籤各自一份 `CommandStack`，關頁籤時丟掉 |
| UI-24 | `applyPermissionRestrictions` 依前綴統一停用「需要文件」的動作，並在啟動時就套用一次 |
| UI-25 | 便利貼的零尺寸 /Rect 撐成 20×20 點 |
| UI-26 | 重載不再寫開啟記錄（UI-01 的附帶結果） |
| UI-27 | 刪掉那個到不了的「屬性」面板 |
| UI-28 | 搜尋面板加上「區分大小寫」「全字比對」，改動即重跑 |
| UI-29 | 狀態列的縮放改成可輸入的下拉 |
| UI-30 | 多檔拖放，每份一個頁籤 |
| UI-31 | 剪下與複製改用與刪除相同的目標判斷 |
| UI-32 | 塗黑標記與表單欄位用完切回選取 |

### 修的過程中另外找到的三個缺陷

這三個都不在原本的 32 項裡，是被新加的測試逼出來的：

1. **P0 — `pageAnnotations` 的任務可以被 `discardPending` 丟掉。**
   `src/engine/pdfium_engine.cpp`。與 `pageInfo`、`renderThumbnail` 同一個坑，
   而那兩個的說明就寫在同一個檔案裡。被丟掉的任務不呼叫 callback，
   而呼叫端用「最後一頁回來才發訊號」的計數器在等——少一個回呼，計數永遠
   到不了零，`annotationsReady` 永遠不發。症狀是註解清單停在空的，
   而模型裡其實已經有資料。開檔後幾毫秒內檢視區必然排版一次
   （`scheduleTiles` → `discardPending(Prefetch)`），所以這條路徑幾乎每次
   開檔都會中。已改為 `discardable=false`。

2. **P1 — `commitAnnotation` 與 `commitPageOperation` 寫完不更新檔案快照。**
   `src/ui/main_window.cpp`。重載完成時會更新，但重載是非同步的：跨頁標記
   在同一輪事件裡連續寫兩次，第二次的外部變更守衛拿開檔時的快照去比對一個
   已經長大的檔案，然後對使用者謊稱「這份文件在開啟之後被其他程式修改過」。

3. **P2 — `applyPermissionRestrictions` 在沒有文件時反而啟用動作。**
   `restrict()` 裡是 `!open || allowed`。改成 `open && allowed`，
   並讓「權限不允許」的說明只在真的有文件時才掛上。

### 沒有處理的

`sign.certify`（PRD-SIG-007）、`sign.timestamp`（PRD-SIG-006）、
`protect.password` 的「真的設定密碼」、Tab 順序編輯、頁面背景、說明主題／
版本資訊／檢查更新／回報問題。前兩者排在 R3（CLAUDE.md：不要提前實作）；
設定密碼在預編譯 PDFium 的公開 API 下不可行（見
`exceptions/EXC_20260906_RD_SA_set_password_unsupported.md`），現在按下去會
明確說明原因而不是灰著；其餘四項沒有實作也沒有內容，已從 Ribbon 預設配置
移除——灰色按鈕對使用者的意思是「壞了」，不是「還沒做」。

### 新增的測試

| 檔案 | 釘住的東西 |
|---|---|
| `tests/test_ui_reload.cpp`（新增，8 條） | 加註解後停在原頁與原倍率、縮圖清單不重建、第 33 頁之後的註解進得了清單、跨頁螢光筆兩頁都有、屬性面板收得到選取、沒有文件時動作是灰的 |
| `tests/annmgmt/test_annotation_update.cpp`（新增，3 條） | 改屬性沿用物件編號與 /NM、/Annots 不增加、寫入仍是純附加且復原點對得上、回覆註解被明確拒絕 |
| `tests/test_ribbon_wiring.cpp`（+2 條，棘輪 14 → 0） | 選單型動作在 Ribbon 上真的彈得出選單、`page.move` 與 `page.nUp` 是兩個動作 |

---

## 結論

呈現層的骨架是完整的：兩百多個動作幾乎都接到了真的實作，錯誤處理與狀態列回饋也普遍到位。
問題集中在三類，而且都是**流程**問題，不是單一功能壞掉：

1. **每一次寫入都把使用者丟回第一頁**（UI-01）。所有文件修改都走 `reloadCurrentDocument()`，
   而重載被當成「開新檔」處理：檢視區回到第 1 頁、縮放重設為符合頁寬、縮圖清單重建。
   使用者在第 40 頁畫一個矩形，畫面立刻跳回第 1 頁。這一條會讓「很多操作流程有問題」
   的感受涵蓋幾乎所有註解與頁面操作。
2. **接錯或沒接的入口**：Ribbon 上三顆選單型按鈕按了沒反應（UI-04）、「移動」按到的是
   N 頁併一頁（UI-05）、14 顆永遠灰色的按鈕（UI-12）、「註解屬性」面板永遠空白（UI-09）、
   「標示表單欄位」無作用（UI-10）。
3. **狀態沒有跟著流程走**：註解清單只知道前 33 頁（UI-03）、偏好設定按確定會把工作階段
   重跑一遍（UI-08）、分離頁籤會把上次的工作階段整批開進新視窗（UI-07）。

另外有一條記憶體安全問題（UI-02，跨頁文字標記迭代已釋放的容器）與一條資料安全問題
（UI-06，縮圖拖曳重排不經確認就重寫檔案、簽章失效無警告）。

| 嚴重度 | 數量 | 編號 |
|---|---|---|
| P0 | 2 | UI-01, UI-02 |
| P1 | 10 | UI-03 ~ UI-12 |
| P2 | 13 | UI-13 ~ UI-25 |
| P3 | 7 | UI-26 ~ UI-32 |

建議處理順序：UI-01 → UI-02 → UI-03 → UI-06 → UI-04/05/12（Ribbon 一起清）→
UI-09/10（兩個死面板）→ UI-07/08（工作階段）→ 其餘。

---

## P0

### UI-01 每一次文件修改後，檢視區跳回第一頁並重設縮放

- 位置：`src/ui/main_window.cpp:6469-6475`（`reloadCurrentDocument`）、
  `src/ui/page_view.cpp:177-183`（`documentOpened` 處理器）、
  `src/ui/main_window.cpp:332-366`（`documentOpened` 處理器）
- 現況：所有寫入路徑（`commitAnnotation`、`commitPageOperation`、`deleteSelectedAnnotation`、
  `replyToSelectedAnnotation`、`commitNoteContents`、`applyPencilStroke`、`applyStamp`、
  `importAnnotationsFromFile`、`pasteAnnotations`、`markRedaction`、`createFormField`、
  `attachFileAt`、`flattenAnnotations`、`editAlternateText`、`clearAllSignatures`、
  `signCurrentDocument`、`editPageLabels`、移除頁面標記，以及每一次復原／重做）結尾都呼叫
  `reloadCurrentDocument()` → `controller_->openDocument(currentPath_)` → 引擎重新開檔 →
  `documentOpened`。`PageView` 收到 `documentOpened` 時無條件 `pageIndex_ = 0`、捲軸歸零、
  `fitWidth()`；`MainWindow` 的處理器把狀態列寫成「第 1 / N 頁」、重建縮圖清單
  （捲動位置歸零）、`recordHistory` 再寫一次開啟記錄、重讀所有導覽面板、重開表單控制器。
  `CommandStack::push` 是同步執行 `redo`（`src/app/command_stack.cpp:10`），所以這一切在
  使用者放開滑鼠的當下就發生。已驗證：`documentOpened` 是唯一的重載訊號，沒有「原地重載」
  的路徑；`pendingRestorePage_` 只有開啟記錄面板會設。
- 重現：開一份 50 頁的文件，捲到第 40 頁，拉一個矩形註解。畫面跳回第 1 頁、縮放變成
  符合頁寬、縮圖面板捲回頂端。按 Ctrl+Z 再跳一次。
- 影響：全部的審閱動作。這是使用者手動操作時「流程有問題」最主要的來源。跨頁螢光筆
  （UI-02）每頁跳一次。
- 建議：把「重載」與「開新檔」分開。`DocumentController` 增加 `reloadDocument()`，完成時
  發 `documentReloaded` 而不是 `documentOpened`；`PageView` 對 `documentReloaded` 只做
  `syncLayoutSizes()` + 重排 + 保持 `pageIndex_`／捲軸值／`scale_`；`MainWindow` 對
  `documentReloaded` 只更新縮圖圖示（不 `clear()`）、重讀註解與書籤、不寫開啟記錄、
  不重設狀態列頁碼。最低限度的替代方案：在 `reloadCurrentDocument()` 記下
  `pageIndex/scrollValue/scale`，在下一次 `documentOpened` 還原並跳過 `fitWidth()`。
  釘一條測試：在第 N 頁加註解後 `pageIndex()` 與 `scale()` 不變。

### UI-02 跨頁文字標記在迭代過程中釋放了自己正在迭代的容器

- 位置：`src/ui/main_window.cpp:5802-5841`（`applyTextMarkup` 的多頁分支）
- 現況：`const app::Selection& current = selection_->selection();` 是對
  `SelectionController::selection_` 的參照。多頁分支用 range-for 走
  `current.pages`，迴圈內每一頁呼叫 `commitAnnotation()` → `push` 同步執行 `redo` →
  `reloadCurrentDocument()` → `selection_->clearSelection()` → `selection_ = Selection{}`
  （`src/app/selection_controller.cpp:400-406`）。第一頁寫完之後 `current.pages` 的儲存體
  已經被釋放，第二次迭代解參照的是懸空迭代器。已驗證：`selection()` 回傳 `const&`，
  `clearSelection()` 整份重新指派。
- 重現：選取跨兩頁的文字，按 Ctrl+H。結果依配置而異：只標到第一頁、標到垃圾座標，
  或崩潰。release 組態下最可能的表現是「第二頁沒有標記，也沒有錯誤」。
- 建議：進迴圈前先複製 `const app::Selection current = selection_->selection();`。
  更好的做法是把 N 頁合成一次 `addAnnotations()` 呼叫、一個命令（`applyPencilStroke` 已經
  這樣做），復原才會一次退掉整組——現在的註解說「每一頁各自是一個命令」是為了讓復原
  只退最後一頁，但那與使用者對「復原一次螢光筆」的預期相反。

---

## P1

### UI-03 註解清單、逐則瀏覽與頁面上的註解選取只涵蓋前 33 頁

- 位置：`src/ui/main_window.cpp:364-365`
- 現況：`requestAnnotations(0, min(pageCount-1, 32))` 只在 `documentOpened` 呼叫一次。
  註解裡寫「捲動到哪再補」，但已驗證整個 `src/ui`、`src/app` 沒有第二個呼叫點。
  `controller_->annotations()` 因此永遠只有前 33 頁的內容，而它同時是註解清單、
  上一則／下一則、「選取註解」工具的命中測試、頁面點擊開便利貼、匯出選定註解、
  Delete 刪註解的唯一資料來源。
- 重現：開一份 100 頁、第 80 頁有便利貼的文件。註解清單看不到它，點它不會開註釋視窗，
  「下一則註解」在第 33 頁之後停住。
- 建議：在 `pageChanged` 時對可視頁前後一段呼叫 `requestAnnotations`（控制器側要合併
  重複請求並保留既有結果，現在的 `annotations_` 看起來是整份替換）；或者開檔後在背景
  分批把整份掃完（PRD-ANN-008 的 500 毫秒預算是指首批，不是全部）。

### UI-04 Ribbon 上的「最近使用」「管理設定」「設定狀態」按了沒有任何反應

- 位置：`src/ui/main_window.cpp:631`、`:702-703`、`:1864`；
  `src/ui/ribbon/ribbon_group.cpp:105`（`rebind`）
- 現況：這三個 id 註冊的是 `QMenu::menuAction()`。Ribbon 按鈕的點擊只做
  `action->trigger()`，而觸發一個選單動作只會發 `triggered`，不會彈出選單——彈出是
  `QMenuBar`／`QToolButton::setMenu` 的行為，Ribbon 按鈕沒有掛選單。已驗證：
  `RibbonGroupWidget` 沒有任何 `setMenu` 呼叫。
- 影響：Ribbon 模式（預設）下最近開啟的檔案、匯出／匯入／重設設定、註解的接受／拒絕／
  完成狀態都到不了；只有切到傳統選單才有。
- 建議：`ribbon::Item` 增加 `ItemType::Menu`（或由 `rebind` 偵測 `action->menu() != nullptr`），
  對這類按鈕 `setMenu(action->menu())` + `setPopupMode(QToolButton::InstantPopup)`。

### UI-05 Ribbon「移動」按鈕接到的是「合併頁面（N 頁併一頁）」

- 位置：`src/ui/main_window.cpp:6173-6174`；`src/ui/ribbon/ribbon_default_layout.cpp`
  組織分頁 `small("page.move", "移動")`
- 現況：`registerRibbonAction("page.move", mergeAction)`，而 `mergeAction` 的文字是
  「合併頁面（N 頁併一頁）」。使用者按「移動」會被問「每張要放幾頁？」，確認後前 N 頁被
  合成一頁——這是一次全檔重寫。
- 建議：`page.move` 接一個真正的移動頁面對話框（範圍 + 目的位置，`pageOps_->movePages`
  已存在，縮圖拖曳就在用）；N 頁併一頁另給 `page.nUp`。

### UI-06 縮圖拖曳重排不經確認就重寫整份檔案

- 位置：`src/ui/main_window.cpp:1666-1687`
- 現況：`rowsMoved` 直接 `commitPageOperation` + `RewriteConsent::confirmed()`。其他所有
  重寫類操作都先走 `confirmRewrite()`（會說明「會重寫整份檔案」，有簽章時加註「簽章將失效」）。
  拖曳是最容易誤觸的手勢（在縮圖上按住稍微滑一下就是一次拖放）。
- 影響：含簽章的文件被無預警重寫成簽章無效；權限允許重組時任何誤觸都是一次全檔改寫。
- 建議：在 `rowsMoved` 裡先 `confirmRewrite(tr("移動頁面"))`，取消時 `populateThumbnails()`
  還原清單；或把拖放改成「放開後才問」的兩段式。

### UI-07 「在新視窗開啟」會把上次的工作階段整批開進新視窗，關閉時還覆寫工作階段

- 位置：`src/ui/main_window.cpp:5653-5668`（`detachTab`）、`:326-330`（建構式呼叫
  `restoreSession()`）、`:6430-6434`（`closeEvent` 呼叫 `storeSession()`）
- 現況：`detachTab` 建一個新的 `MainWindow`，而建構式無條件 `restoreSession()`——新視窗會
  先還原上次關閉時的所有頁籤與作用中文件，然後才 `openPath(path)`。關掉那個視窗時
  `closeEvent` 把它的頁籤存成工作階段，蓋掉主視窗的。
- 重現：上次關閉時開著 A、B 兩份。這次開 C、D，把 D「在新視窗開啟」：新視窗出現 A、B、D
  三個頁籤。關掉新視窗，再關主視窗，下次啟動只剩 A、B、D。
- 建議：建構式加一個「不還原工作階段」的參數給分離視窗；工作階段只由主視窗寫入
  （或合併所有視窗的頁籤，PRD-UI-001 的多視窗語意要先定）。

### UI-08 偏好設定按「確定」會重跑 `restoreSession()`

- 位置：`src/ui/main_window.cpp:986-990`
- 現況：接受對話框後呼叫 `applySettings(); restoreSession();`。`restoreSession` 做四件事，
  每一件在這個時機都是錯的：`restoreGeometry/restoreState` 把視窗與面板版面打回**上次關閉時**
  的狀態（這個 session 裡開的面板全部消失）；對工作階段裡每一份文件再 `tabs_.openTab` 一次
  （`TabLayoutModel::openTab` 不去重，已驗證 `src/app/uisystem/tab_layout_model.cpp:39-51`），
  頁籤列出現重複；`openPath(document.path)` 重載文件（跳頁，UI-01）；再掛一個
  `documentOpened` 處理器套用上次的頁碼與縮放。
- 建議：拿掉那一行。偏好設定的每一項都已在 `applySettings()` 生效。

### UI-09 「註解屬性」面板永遠停在「選取一則註解以檢視並修改它的屬性」

- 位置：`src/ui/main_window.cpp:1954-1955`；`src/ui/annotation_properties_panel.cpp`
- 現況：`AnnotationPropertiesPanel` 有完整的編輯器（顏色、填色、不透明度、線寬、線型、
  行距、縮排、作者、主旨、內容、鎖定／隱藏／列印旗標）與 `annotationEdited` 訊號，但
  `MainWindow` 只建立它，從未呼叫 `setAnnotation()`，也沒有接 `annotationEdited`。
  已驗證：`propertiesPanel_` 在 `main_window.cpp` 只出現兩次（建立與加進停靠）。
  PRD-ANN-009／ANN-031 的入口 `Ctrl+'` 與 Ribbon「註解屬性」都只是把一個空面板叫出來。
- 建議：`selectAnnotation()` 時用 `readAnnotationsForExport` 找出對應那一則（與
  `copySelectedAnnotation` 同一套比對）交給 `setAnnotation()`；`annotationEdited` 接到
  `annotations_->updateAnnotation`（若沒有這個服務，就是要補的那一段）並走命令堆疊。

### UI-10 「標示表單欄位」與欄位面板的「只顯示本頁欄位」都沒有作用

- 位置：`src/ui/page_view.cpp:536-539`（`setFormFieldRects`）、
  `src/ui/fields_panel.cpp:1871-1874`（`setCurrentPage`）
- 現況：兩個函式都沒有任何呼叫者（已驗證）。`setFormFieldHighlight(true)` 只是把旗標打開，
  `fieldRects_` 永遠是空的，畫面上什麼都不會亮；欄位面板的 `currentPage_` 永遠是 -1，
  勾了「只顯示本頁欄位」仍然顯示全部。
- 建議：`fieldsReady` 時把 `forms_->fields()` 的 `(pageIndex, rect)` 餵給
  `pageView_->setFormFieldRects()`；`pageChanged` 時呼叫 `fieldsPanel_->setCurrentPage()`。
  前者需要 `FieldSummary` 帶矩形；沒有的話要先補。

### UI-11 全域 Esc 捷徑吞掉了檢視區的 Esc；「Esc 停止自動捲動」是假的

- 位置：`src/ui/main_window.cpp:1330-1334`（`QShortcut(Qt::Key_Escape)`，
  `ApplicationShortcut`）；`src/ui/page_view.cpp:458-467`；`src/ui/main_window.cpp:4330`
- 現況：`QShortcut` 只要序列符合就消耗按鍵事件並發 `activated`，不管處理函式有沒有做事。
  `PageView` 沒有攔 `ShortcutOverride`，所以 `keyPressEvent` 裡「Esc 取消多邊形／折線」
  永遠收不到 Esc。自動捲動的狀態列訊息寫「Esc 停止」，但整個 `main_window.cpp` 只有簡報模式
  那一個 Esc 處理器（已驗證），自動捲動要按 Ctrl+Shift+H 才停。
- 建議：把 Esc 集中成一個處理器，依序嘗試：自動捲動 → 檢視區的進行中手勢 → 簡報模式；
  或改成 `WindowShortcut` 並讓 `PageView::event()` 在有進行中手勢時 accept `ShortcutOverride`。

### UI-12 Ribbon 預設配置有 14 顆按鈕沒有對應的動作，永遠灰色

- 位置：`src/ui/ribbon/ribbon_default_layout.cpp`；`src/ui/main_window.cpp:4993-4996`
  只在啟動時於狀態列閃 4 秒「Ribbon 有 14 個動作尚未接上」
- 已驗證清單：`tool.zoomArea`（常用／工具）、`form.tabOrder`（表單／表單工具）、
  `page.background`（組織／頁面標記）、`protect.password`、`protect.permissions`、
  `protect.removeSecurity`（保護／安全性——整個群組三顆都是空的）、`sign.certify`、
  `sign.timestamp`（保護／簽章）、`session.save`、`session.restore`（檔案／工作階段）、
  `help.contents`、`help.releaseNotes`、`help.checkUpdates`、`help.reportIssue`（說明）。
- 影響：使用者看到的是「這個功能壞了」而不是「還沒做」。保護分頁的第一個群組整個不能用。
- 建議：還沒排程的從預設配置拿掉（配置檔支援自訂，之後補回不會影響使用者存的 ribbon.json）；
  屬於 R1 的（`protect.*` 若在 PRD-SEC 範圍內）補實作；`session.save/restore` 其實
  `storeSession()/restoreSession()` 都在，只差註冊。

---

## P2

### UI-13 「另存新檔」的狀態列訊息兩個檔名一樣，而且原頁籤留著

- 位置：`src/ui/main_window.cpp:3040-3044`
- 現況：先 `openPath(target)`（`currentPath_` 變成新檔），再用 `currentPath_` 當「原檔」
  組訊息，兩個 `%1 %2` 都是新檔名。`openPath` 又開了一個新頁籤，原檔的頁籤沒關，
  變成兩個頁籤。註解說「與 Acrobat 一致」，Acrobat 是切換而不是多開。
- 建議：先取 `QFileInfo(currentPath_).fileName()` 存起來；用 `tabs_.closeTab` 關掉原頁籤
  再 `openPath(target)`（`renameCurrentDocument` 已經是這個寫法）。

### UI-14 頁面操作的確認順序反了：先問「會重寫整份檔案」，再問參數

- 位置：`src/ui/main_window.cpp:6114-6126`（從檔案插入）、`:6146-6152`（插入文字頁面）、
  `:6175-6180`（N 頁併一頁）、`:6199-6203`（疊加）、`:6214-6224`（取代頁面）、
  `:6252-6254`（裁切）、`:6321-6323`（旋轉全部）、`:6344-6345`（正規化）
- 現況：`confirmRewrite()` 在選檔案、輸入頁碼之前。使用者同意重寫之後還可以取消選檔，
  而確認對話框本身沒有告訴他接下來要選什麼。`deletePagesByRange`、`resizePagesWithDialog`
  是正確的順序（先收參數，最後確認）。
- 建議：一律把 `confirmRewrite` 放在收齊參數之後、`commitPageOperation` 之前。

### UI-15 螢光筆與底線／刪除線／波浪線的行為不一致；螢光筆工具模式沒有入口

- 位置：`src/ui/main_window.cpp:801-804`、`:937-940`；`src/ui/page_view.cpp:778-782`、
  `:876-880`
- 現況：`annot.highlight`（Ribbon 常用與註解分頁的大按鈕）是「立即把目前選取標成螢光筆」，
  沒有選取時跳 modal「請先選取文字」。旁邊的底線／刪除線／波浪線是切換成工具、拖過文字
  放開就標。`PageView` 有 `Tool::Highlight`（`markupSelectionCompleted` 也處理它），但沒有
  任何 QAction 會 `setTool(Tool::Highlight)`。
- 建議：`annot.highlight` 改成工具（與其他三個一致），Ctrl+H 保留為「套用到目前選取」
  另給 id（例如 `annot.highlightSelection`）。

### UI-16 「旋轉頁面」只能旋轉全部頁面

- 位置：`src/ui/main_window.cpp:6318-6340`
- 現況：三個旋轉動作都把 0..N-1 全部丟進 `rotatePages`。Ribbon 標籤「旋轉頁面」與
  PDF-XChange 的 Rotate Pages（預設當前頁，可選範圍）不同；掃描件只有一頁歪掉時沒有辦法。
- 建議：用與刪除頁面相同的範圍輸入（預設當前頁）。

### UI-17 Ribbon 的「新增書籤」「管理書籤」名實不符

- 位置：`src/ui/main_window.cpp:4700-4713`、`:4580-4582`
- 現況：`bookmark.add` 註冊的是 `markAction`——加一個**使用者端的閱讀標記**到開啟記錄面板
  （PRD-NAV-005），不寫進文件的 `/Outlines`。`bookmark.manage` 只是打開書籤面板，面板本身
  沒有新增／改名／刪除。使用者按「新增書籤」後在書籤面板找不到剛加的東西。
- 建議：改名為「加入閱讀標記」並移到瀏覽群組；書籤（Outline）編輯若在 PRD-BM 範圍內，
  是一個獨立工作包。

### UI-18 一批檢視功能在 Ribbon 模式下沒有入口

- 位置：`src/ui/main_window.cpp:1487-1522`（夜間模式、顯示品質四項、透明度格線）、
  `:1433-1435`（四格分割）、`:1461`、`:1478-1484`（雙頁連續、右至左）、
  `:1542-1578`（朗讀）、`:4679-4683`（自動捲動）、`:4608-4626`（主題）
- 現況：這些動作只掛在傳統選單（預設隱藏）或只有快捷鍵。主題連偏好設定都沒有，
  Ribbon 模式下無法切換深色。`view.theme.*`、`view.autoscroll`、`view.splitQuad`、
  `view.grayscale` 等 id 都註冊了，只是預設配置沒放。
- 建議：檢視分頁加「顯示」群組（夜間、透明度格線、品質下拉）與「主題」；朗讀與自動捲動
  放瀏覽群組；主題同時進偏好設定。

### UI-19 權限檢查不一致：一半在介面灰化、一半在寫入前檢查、有幾條完全沒檢查

- 位置：`src/ui/main_window.cpp:4084-4148`（`applyPermissionRestrictions`）、
  `:5712-5748`（`commitAnnotation`）、`:5864-6007`（`applyShape`）、
  `:5801-5862`（`applyTextMarkup`）
- 現況：`commitAnnotation`、`applyShape`、`applyTextMarkup` 都不檢查
  `permissions.annotate`；灰化清單只涵蓋 `annot.highlight/underline/strikeout/stickyNote/
  rectangle/ellipse/line/arrow/polygon/polyline/cloud` 與 `comment.delete`。
  文字方塊、打字機、指示框、鉛筆（`applyPencilStroke` 自己有檢查）、校正符號、圖章、量測、
  回覆、設定狀態、貼上、匯入註解都不在灰化清單，而其中文字方塊／打字機／指示框／校正／
  圖章／量測在寫入前也沒檢查。禁止加註的文件仍然可以被加上這些註解。
  `AnnotationService` 內部也沒有權限檢查（已驗證 grep 無 `permissions`）。
- 建議：在 `commitAnnotation` 單一入口檢查 `permissions.annotate`；灰化清單改成掃
  `annot.*` 前綴。

### UI-20 列印在 GUI 執行緒同步執行，沒有進度也不能取消

- 位置：`src/ui/main_window.cpp:4910-4944`、`:4879-4908`
- 現況：`PrintService::print` 同步跑完整份文件。500 頁工程圖會讓視窗凍住幾十秒，
  違反 CLAUDE.md「主執行緒單次阻塞 ≤ 16 毫秒」。同類但較輕的：`fitVisible`、
  `copySnapshotToClipboard`、`exportCurrentPageImage`、`auditSpaceUsage`、
  `reloadNavigationPanels` 都在 GUI 執行緒把整份檔案讀進記憶體。
- 建議：列印走 `QProgressDialog` + 分頁排程（每頁 `processEvents` 或搬到工作執行緒，
  `PrintService` 已自持引擎實體，可以在別的執行緒跑）。

### UI-21 開啟記錄面板裡的閱讀標記點在目前文件上沒有反應

- 位置：`src/ui/main_window.cpp:1907-1911`
- 現況：`markRequested` 只在路徑不同時 `openPath`，然後設 `pendingRestorePage_`。同一份
  文件時沒有人消費那個值，畫面不動；下一次開任何文件時才會突然跳到那一頁。
- 建議：同一份文件直接 `navigateToPage(pageIndex)`。

### UI-22 圖章工具沒有選擇樣式的介面

- 位置：`src/ui/main_window.cpp:5949-5954`；`src/domain/annotation.h:300-301`
- 現況：`Tool::Stamp` 拖一個框就寫出 `StampGeometry{}`，`kind` 預設 `Approved`。
  沒有任何地方能選 Draft／Confidential／自訂影像。
- 建議：拖框之後跳一個選圖章的小對話框（既有的 `StampKind` 列舉 + 從影像檔）。

### UI-23 切換頁籤會清空復原堆疊

- 位置：`src/ui/main_window.cpp:5696-5705`（`loadDocument`）
- 現況：`commands_->clear()`。在 A 加了三則註解、切到 B 再切回 A，A 的三步不能復原。
  這可能是刻意的（單一堆疊），但沒有提示，而使用者對頁籤的預期是「切過去再切回來什麼都沒變」。
- 建議：每個頁籤一個 `CommandStack`（命令的 lambda 已經以值捕捉 `path`，切換不會互相污染）；
  若維持單一堆疊，切換時在狀態列說明。

### UI-24 未開啟文件時的行為不一致：有的動作跳訊息、有的沉默、有的按鈕永遠可按

- 位置：例如 `src/ui/main_window.cpp:957-963`（儲存：沉默）、`:3251-3252`（刪除頁面：沉默）、
  `:2036-2040`（匯出影像：訊息）、`:6300-6303`（移除頁面標記：訊息）
- 現況：沒有文件時，快速存取列的「儲存」「列印」可按；「刪除頁面」「複製頁面」「擷取頁面」
  「剪下註解」「貼上註解」「復原所有修改」「移除塗黑標記」按了沒有任何回饋；另外二十幾個
  動作會跳「尚未開啟文件」。
- 建議：`applyPermissionRestrictions` 已經有 `open` 這個變數，把「需要文件」的動作在
  同一處統一停用（開檔／關檔時各呼叫一次），之後函式內的 `isOpen()` 檢查只是防呆。

### UI-25 便利貼點一下建立時矩形為零尺寸（待實機驗證）

- 位置：`src/ui/page_view.cpp:902-912`、`src/ui/main_window.cpp:5938-5944`
- 現況：便利貼刻意允許「點一下就放」，此時 `rect` 是一個點。`/Rect` 為零尺寸的 `/Text`
  註解在 Acrobat 通常仍會畫圖示（Acrobat 忽略 /Rect 大小），但 Chrome／預覽對
  `/AP` 的 `/BBox` 為零時可能不畫。需要用 Acrobat 與 Chrome 各開一次確認；若不畫，
  `applyShape` 應把點擴成 20×20 pt。

---

## P3

### UI-26 每一次重載都寫一次開啟記錄檔

- 位置：`src/ui/main_window.cpp:352`（`recordHistory` 在 `documentOpened` 裡）
- 現況：每加一則註解就 `history_.save()` 一次，開啟記錄的「最後開啟」時間也一直刷新。
  修 UI-01 時一併移到真正開檔的路徑。

### UI-27 有一個沒有任何入口的死面板「屬性」

- 位置：`src/ui/main_window.cpp:1892-1896`
- 現況：`propertiesDock`（一個從未填入的 `QListWidget`，標題「屬性」）沒有 toggleViewAction
  綁定、預設隱藏、F6 循環跳過隱藏面板——完全到不了。`propertiesDock_` 成員指的反而是
  `annotationPropertiesDock`，命名會誤導下一個改的人。文件屬性已經有對話框（Ctrl+D）。
- 建議：刪掉。

### UI-28 搜尋沒有選項

- 位置：`src/ui/main_window.cpp:4399-4405`
- 現況：`selection_->search(query, page, false, false)` 兩個布林寫死；面板上沒有區分大小寫／
  全字比對的核取方塊，也沒有「找上一個」（只有 F3 找下一個）。

### UI-29 縮放比例只能看不能打

- 位置：`src/ui/main_window.cpp:4772-4776`
- 現況：狀態列的縮放是 `QLabel`。對標產品有可輸入的縮放框；這裡要到指定倍率只能反覆
  Ctrl+加號。

### UI-30 拖放只接受單一 PDF

- 位置：`src/ui/main_window.cpp:5596-5604`
- 現況：註解寫「多檔拖放要等多頁籤接上才有意義」，多頁籤早已完成。拖兩個檔進來會被
  靜默拒絕（游標顯示禁止）。

### UI-31 「剪下註解」與「刪除註解」對「頁面上選到的那一則」判斷不同

- 位置：`src/ui/main_window.cpp:770-777`、`:3805-3810`、`:3914-3925`
- 現況：刪除優先用 `selectedAnnotation_`（頁面選取），剪下只看清單目前列。用「選取註解」
  工具點到一則被篩選條件濾掉的註解時，Delete 會刪它，Ctrl+X 卻說「請先在註解清單選一則」。

### UI-32 部分工具用完不會切回選取

- 位置：`src/ui/main_window.cpp:2937-2974`（標記待塗黑）、`:3521-3583`（建立表單欄位）、
  `:2523-2602`（鉛筆）
- 現況：`applyToolPersistence()` 只在 `commitAnnotation` 與 `attachFileAt` 呼叫。塗黑標記、
  表單欄位、鉛筆用完仍停在該工具；工具持續模式關閉時，使用者下一次點頁面又畫了一個。
  對鉛筆這可能是刻意的（連續筆畫），對表單欄位則會連續建立欄位。

---

## Ribbon 覆蓋率（供對照）

預設配置共 8 個分頁、194 個按鈕位置（含重複 id）。已註冊 id 中**不在**預設配置的：
`annot.properties`、`comment.copy`、`comment.paste`、`comment.flatten`、
`comment.summaryOnly/WithDocument/SideBySide`（Ribbon 用 `comment.summarize` 指向純文字那一種）、
`edit.undo/redo`（在快速存取列）、`file.manageExternalTools`、`nav.mark`、`protect.redactClear`、
`view.autoscroll`、`view.customizeRibbon`、`view.grayscale/smoothPaths/smoothText/smoothImages`、
`view.readAloudToggle/Pause`、`view.splitQuad`、`view.theme.system/light/dark`。
其中 `comment.flatten`、`protect.redactClear`、`view.customizeRibbon` 是使用者會找的功能。

---

## 本次一併修掉的（不在上面編號內）

這個工作階段在審查前已經修了三條，與本文件的發現同源，一起列出讓對照完整：

- 縮圖點一下不會跳頁：選取不跟檢視區走、再點已選取那一格沒訊號（`clicked` 補接）。
- 縮圖的可點範圍與選取框比格子小：換成固定尺寸的 `ThumbnailDelegate`。
- 關檔後縮圖與所有面板留著上一份文件：`documentClosed` 原本沒有人接，補了
  `clearDocumentUi()` 與 `PageView` 的關檔清理，並把復原堆疊一起清掉。

以上尚未提交。
