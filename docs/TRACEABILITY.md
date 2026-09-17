# PRD 對標矩陣

| 項目 | 內容 |
|---|---|
| 對應 WBS | 7.10 |
| 產生方式 | `python tools/traceability.py`，需求由 PRD 直接抽取 |
| 狀態來源 | `tools/traceability.py` 的 STATUS 表（手動維護，隨實作更新） |

PRD §13 的完成定義是「217 條範圍內需求逐條勾稽，每條有對應自動化測試且於 CI 通過」。
本表是那項驗收的載體。

狀態的意義：

- **完成**：功能可用，且有自動化測試涵蓋
- **部分**：主要路徑可用，但有明確缺口（備註欄說明）
- **未開始**：尚未實作
- **範圍外**：PRD §2.1 明確排除，或經 ADR 決議排除（備註須寫明依據）

未開始的項目不是遺漏，是還沒排到——版本欄標示了它屬於哪一輪。

備註寫著「需要實機／外部資源」的項目，其執行步驟與通過判準見
`docs/MANUAL_VERIFICATION.md`——只寫「未驗證」而不寫怎麼驗，實際效果是
那幾項永遠不會被驗。


## 總覽

| 狀態 | 條目數 | 佔比 |
|---|---|---|
| 完成 | 126 | 77% |
| 部分 | 35 | 21% |
| 未開始 | 0 | 0% |
| 範圍外 | 2 | 1% |
| **表列合計** | **163** | |

> 表列合計是 PRD 需求表格中有獨立編號的條目。PRD §2.4 的「範圍內 217 條」包含以範圍表示的條目（例如 `PRD-BM-002~019`），逐條展開後多於此數。


## VIEW

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-VIEW-001 | 可視區／圖磚化渲染，嚴禁整頁光柵化 | M | R1 | 完成 | 512 圖磚化渲染，`tests/test_pdfium_engine.cpp` 驗零複製與像素落點 |
| PRD-VIEW-002 | 非同步載入渲染於單一 PDFium 執行緒，主執行緒不阻塞 | M | R1 | 完成 | 單一 PDFium 執行緒 + 優先權佇列，見 tests/test_pdfium_engine.cpp 的 concurrentCallersAreSerialisedOnOneThread |
| PRD-VIEW-003 | 漸進式渲染與任務取消 | M | R1 | 完成 | 佇列層取消已實作並測試，且量到「取消會丟掉排隊中的工作」而不只是回報成取消——只驗數量不驗時間的話，一個照樣跑完卻回報取消的實作會通過，而使用者感受到的仍然是卡住。單塊渲染中途不可中斷（PDFium 無此 API），上限因此是一塊圖磚的渲染時間：效能基準量到 tile P95 約 11 ms，在 CLAUDE.md 的 16 ms 取消延遲預算之內。測試見 tests/test_pdfium_engine.cpp |
| PRD-VIEW-004 | 版面模式：單頁／連續／雙頁／雙頁連續／封面獨立／頁間間隙 | M | R1 | 完成 | 單頁／連續／雙頁／雙頁連續／封面獨立，`tests/test_page_layout.cpp` 11 例 |
| PRD-VIEW-005 | 檢視層頁面旋轉（不修改文件） | M | R1 | 完成 | 檢視層旋轉四向，座標往返自洽，見 tests/test_geometry.cpp 與 tests/test_page_layout.cpp |
| PRD-VIEW-006 | 高解析與多螢幕不同 DPI | M | R1 | 部分 | 圖磚以裝置像素比渲染、版面維持邏輯像素；跨螢幕拖曳的重算需要兩台不同 DPI 的實體螢幕，步驟與判準見 docs/MANUAL_VERIFICATION.md §2 |
| PRD-VIEW-007 | 夜間模式／自訂背景與文字色 | S | R1 | 完成 | 夜間模式（像素反相）與自訂背景／文字色，兩者互斥且夜間模式優先。自訂配色**只換接近灰階的像素**（R/G/B 極差 ≤ 32），照片與圖表維持原色——整幅無條件替換在黑白語料上看起來也有效，卻會把照片染成單色，而使用者換配色是因為白底刺眼不是要把圖變單色。灰階依亮度在兩色之間內插而非二值化，否則抗鋸齒的字緣會變鋸齒。兩條像素測試見 tests/test_pdfium_engine.cpp |
| PRD-VIEW-008 | 圖層（OCG）面板與可見性切換 | S | R1 | 部分 | /OCProperties 解析、面板樹與可見性模型完成，圖層面板已接上主視窗；R1 依 ADR-003 降級為唯讀（預編譯 PDFium 無 OCG 可見性 API），真正可切換的版本併入 R2 的 PRD-VIEW-014，面板固定顯示降級說明 |
| PRD-VIEW-009 | 全螢幕／簡報模式與頁面轉場 | S | R1 | 完成 | 全螢幕（F11）與簡報模式（Shift+F11）已接上主視窗，Esc 退出且回到進入前的模式。兩者刻意不同：全螢幕只是視窗佔滿螢幕、Ribbon 與面板都在，簡報模式藏掉一切介面。**退出時把藏掉的面板原樣還原**（不是套用預設版面）——使用者排好的版面不該因為看了一次簡報就消失，見 tests/test_ui_smoke.cpp 的 presentationModeRestoresTheInterfaceItHid |
| PRD-VIEW-010 | **Thin Lines**：所有註解線條以 1 像素顯示 | M | R1 | 部分 | 檢視期覆寫線寬在預編譯 PDFium 上不可行（沒有對應旗標，實測見 exceptions/EXC_20260906_RD_SA_thin_lines_unsupported.md）；折衷是讓線寬 0（PDF 規格的裝置最細線）成為屬性面板的合法選項，使用者自己畫的線因此永遠看得見 |
| PRD-VIEW-011 | **Ribbon Layout**：頁面左右排列 | S | R1 | 完成 | Ribbon Layout（頁面左右排列）：同列相鄰、不等高垂直置中、座標往返自洽、RTL 反向，`tests/test_horizontal_layout.cpp` 10 例；已接上檢視選單與 Ribbon 版面群組 |
| PRD-VIEW-012 | **右至左版面** | S | R1 | 完成 | 右至左版面，`rightToLeftSwapsVisualOrderNotPageOrder` |
| PRD-VIEW-013 | Stroke Adjust 渲染選項 | C | R2 | 部分 | Stroke Adjust 對映到 FPDF_RENDER_NO_SMOOTHPATH（近似而非規格等價，已在標頭寫明限制），另補灰階與關閉平滑化旗標。已接上「檢視 → 顯示品質」四個項目並註冊為 Ribbon 動作；「平滑線條」與 strokeAdjust 是反向對映，連同「改了選項一定要作廢舊圖磚」由 tests/test_render_quality.cpp 釘住。仍是部分：近似不等於規格語意，工程圖上 0.1 pt 的線在低倍率下仍可能整條消失（取樣問題，不是抗鋸齒問題） |
| PRD-VIEW-014 | 圖層攤平為基礎內容 | S | R2 | 完成 | 圖層攤平（ADR-003 的還款）：BDC/BMC/EMC 巢狀正確配對（DP/MP 不參與巢狀但仍受可見性判斷）、OCMD 的 /P 四種政策、XObject 自身的 /OC 與 Form XObject 遞迴、註解的 /OC 移除。以真正的 PdfiumEngine 渲染回讀驗證隱藏圖層真的消失而可見圖層不受影響——只驗位元組會漏掉「刪錯地方」。/VE 可見性運算式未求值：遇到時保守保留內容（寧可多顯示也不要少顯示，攤平後刪錯的救不回來）並回報，呼叫端必須把這個降級顯示給使用者 |
| PRD-VIEW-015 | **尺規與參考線**（可從尺規拖出） | M | R1 | 完成 | 尺規（Ctrl+R）沿檢視上緣與左緣，以網格版面對齊檢視區原點（左上角留空格，否則兩把尺規的零點各差一個尺規厚度）；從尺規拖出參考線，放開才加入（拖曳中每格都加的話一次會產生幾十條）。刻度跟著捲動、縮放、換頁同步——對不準的尺規比沒有尺規更糟，使用者會照著它量。測試見 tests/viewaids/test_guides.cpp |
| PRD-VIEW-016 | **格線與貼齊（Snapping）** | M | R1 | 完成 | 格線與貼齊（含物件邊緣、優先序）已接上主視窗：格線與參考線畫在頁面內容之上、選取之下，**不寫進 PDF**（畫進內容串流的話使用者列印或寄出時會多出一堆線，且不可逆）。四個開關分開而非合成一個「繪圖輔助」——貼齊是行為、格線是視覺提示，使用者常常只要其中一個。貼齊容差以點為單位而非像素，理由同命中容差。測試見 tests/viewaids/test_guides.cpp |
| PRD-VIEW-017 | **Split View**：水平／垂直／試算表式分割 | M | R1 | 完成 | 四種模式：不分割／水平／垂直／試算表式（四格）。四格是外層垂直加兩排水平，兩排的直向分隔線連動——不連動的話四格會歪成階梯，那就只是「隨便切四塊」。切換模式一律整個重建，狀態只有一種；主檢視不被連帶刪掉（測試釘住，那個錯的症狀是切回不分割後中央區一片空白）。分割模式的檢視數量與主檢視存活見 tests/test_ui_smoke.cpp |
| PRD-VIEW-018 | 透明度格線 | C | R2 | 完成 | 透明度格線：引擎在開啟時以 alpha=0 起始而不是把白底烘進圖磚（烘死了的話呈現層畫得再漂亮也透不出來，而那個缺陷從程式碼上看完全正常），呈現層畫棋盤格，檢視選單可切換。以像素測試驗證未繪製區 alpha=0、有內容處 alpha=255。測試見 tests/test_pdfium_engine.cpp |

## ZOOM

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-ZOOM-001 | 向量級平滑縮放，最終畫面以新倍率重算 | M | R1 | 完成 | 縮放停止後以精確倍率重算，兩階段圖磚鍵，見 tests/test_tile_cache.cpp |
| PRD-ZOOM-002 | 對準游標縮放（Ctrl+滾輪／觸控板雙指） | M | R1 | 完成 | 對準游標縮放（applyZoomAnchored），版面換算見 tests/test_page_layout.cpp |
| PRD-ZOOM-003 | 縮放 8%–6400%；實際大小／符合頁面／符合頁寬／**Fit Visible（忽略白邊）** | M | R1 | 完成 | 8%–6400%、實際大小、符合頁面、符合頁寬、Fit Visible（忽略白邊，Ctrl+3）。Fit Visible 與「裁切至白邊」共用同一個內容邊界偵測器——兩處各自實作的話，使用者會看到「縮放的範圍」與「裁掉的白邊」對不起來。整頁皆白時退回符合頁面並說明，而不是留在原倍率讓按鈕看起來壞掉。測試見 tests/test_ui_smoke.cpp（上下界）與 tests/pages/test_page_crop.cpp（Fit Visible 共用的內容邊界） |
| PRD-ZOOM-004 | 區域框選縮放、**Loupe 放大鏡**（含頁框與參考線顯示） | S | R1 | 完成 | 區域框選縮放與 Loupe 放大鏡（走既有圖磚管線，不整頁光柵化），放大鏡面板已接上主視窗並隨游標取樣；游標落在頁面之間的空白處時維持上一個畫面而不是顯示灰底。測試見 tests/viewaids/test_zoom_aids.cpp |
| PRD-ZOOM-005 | **Pan & Zoom 面板**（動態導覽窗格） | M | R1 | 完成 | Pan & Zoom 面板已接上主視窗：拖曳視框改變主視圖捲動位置，捲動／縮放／換頁三種都會同步視框（缺任一種就會出現「視框停在原地」，比沒有這個面板更誤導），面板叫出來當下也立刻同步一次。無障礙角色以 QAccessibleWidget 工廠設為 Canvas——QWidget 預設的 Client 對輔助技術等於「不知道是什麼的區域」，這是本專案的無障礙稽核閘門當場擋下來的。測試見 tests/viewaids/test_zoom_aids.cpp |

## NAV

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-NAV-001 | 頁碼跳轉、上下頁、首末頁、瀏覽歷史前進後退 | M | R1 | 完成 | 頁碼跳轉、上下頁、首末頁、瀏覽歷史前進後退（app/view_history.h，Alt+左右，選單與 Ribbon 檢視分頁）。只有離散跳轉入堆（面板、搜尋命中、連結），連續捲動不記——每次捲動都記一筆的話後退只會退幾十像素，等於功能不存在。11 例，見 tests/test_view_history.cpp |
| PRD-NAV-002 | 平滑捲動與鍵盤導覽 | M | R1 | 部分 | 平滑捲動與鍵盤導覽可用；60 fps 需要 GUI 事件迴圈上的幀率量測，步驟見 docs/MANUAL_VERIFICATION.md §7 |
| PRD-NAV-003 | 書籤面板解析、跳轉、折疊 | M | R1 | 完成 | 書籤解析與跳轉，迭代走訪限深 32 層、以索引路徑避免容器重新配置；/A GoTo 形態也解析得出頁碼。測試見 tests/test_outline_traversal.cpp |
| PRD-NAV-004 | 縮圖面板：即時縮圖、拖曳重排、動態選取選項 | M | R1 / R3 | 完成 | 縮圖面板、跳頁與拖曳重排（InternalMove，放開時換成一次頁面移動操作，走命令堆疊可復原）。索引語意有陷阱：Qt 的 rowsMoved 給的 destination 是「來源還在原位時」的插入點，PageEditor 要的是移除來源之後的位置，往後拖時差一——沒換算的話頁面會落在放開處後面一格，使用者每次都看得到。兩條方向各一的測試見 tests/pages/test_page_editor.cpp。重建清單期間擋掉 currentRowChanged，否則重建過程的每次選取變化都會把使用者帶到別的頁 |
| PRD-NAV-005 | 使用者書籤與閱讀位置記憶 | S | R1 | 完成 | 使用者書籤（Ctrl+B，存使用者端不寫進文件）與閱讀位置記憶，已接上主視窗：換檔與關閉時寫入、重開時套用並在頁數變短時停在最後一頁。`tests/test_history_store.cpp` 18 例 |
| PRD-NAV-006 | 連結註解點擊與建立 | M | R1 | 完成 | 連結點擊與外部網址確認；Launch action 一律忽略（PRD §8.2），只放行 http/https。測試見 tests/test_links.cpp |
| PRD-NAV-007 | 命名目標（Destinations）面板與繼承縮放 | S | R2 | 完成 | 命名目標面板已接上主視窗：新舊兩代語法都讀、破損目標列出但不可點、/XYZ 倍率 0 視為沿用目前倍率、名稱搜尋。測試見 tests/test_navigation_service.cpp |
| PRD-NAV-008 | 自動捲動（Ctrl+Shift+H，可調速與反向） | S | R1 | 完成 | 自動捲動已接上（Ctrl+Shift+H，Ctrl+Shift+方向鍵調速與反向）：低速不會因取整而不動、失焦回來不暴衝、到底自動停止。測試見 tests/test_navigation_service.cpp |
| PRD-NAV-009 | History 面板（已開啟文件歷史） | S | R1 | 完成 | History 面板已接上主視窗：搜尋、相對時間、檔案不在時保留並標示為不可開、Delete 移除。測試見 tests/test_history_store.cpp |

## ANN

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-ANN-001 | 文字標記：螢光筆／底線／刪除線 | M | R1 | 完成 | 螢光筆／底線／刪除線／波浪線四種文字標記皆可用，走物件層通道（/AP 帶 /Resources 與 Multiply、一併建 /Popup）。**修掉一個沉默的缺陷**：底線與刪除線工具原本只會選字，放開滑鼠後什麼都不會發生——只有螢光筆有選單入口會真的寫入。現在四種工具都在放開滑鼠時提交，PageView 只回報「選好了」，寫哪一種由應用層決定。Acrobat 實機比對仍待補 |
| PRD-ANN-002 | 幾何：矩形／橢圓／直線／箭頭／多邊形／折線／雲線 | M | R1 | 完成 | 矩形／橢圓／直線／箭頭（拖曳）與多邊形／折線／雲線（逐點點擊，雙擊或 Enter 結束、Esc 取消，換頁自動結束——跨頁多邊形在 PDF 裡不存在）。雲線是多邊形加 /BE << /S /C /I >>（規格的表達方式，不另建 CloudGeometry，否則「雲線算不算多邊形」在量測與編輯路徑上會各有一套答案）：弧沿邊等距擺放、每邊至少一個（短邊否則會退化成直線）、**凸向由有向面積決定**（凸錯邊時形狀像被咬過一口，且沒有任何錯誤訊息），/I 夾在規格的 0–2、為 0 時退回直邊。四條測試見 tests/annfamily/test_annfamily_appearance.cpp |
| PRD-ANN-003 | 鉛筆手繪：壓力感測、平滑化 | M | R1 | 完成 | 鉛筆工具已接上：拖曳逐點收集，壓力取自 QTabletEvent（滑鼠一律 1.0），即時預覽的線寬跟著壓力變化。平滑化含每點最大偏差上限（不得把使用者畫的形狀改到認不出來）。壓力→筆寬：/Ink 只有單一 /BS /W，因此依壓力分檔切段，每檔寫成一則 /Ink；相鄰段共用交界點（否則變粗處看得到缺口），壓力全程相同時退回單一註解。整筆的數則註解寫在同一個增量段裡，復原是一步。壓力分段見 tests/annmgmt/test_ink_pressure.cpp |
| PRD-ANN-004 | 便利貼與彈出視窗 | M | R1 | 完成 | 便利貼可放置、可開註釋視窗編輯內容（增量寫入 /Contents 與 /M，包成命令物件可復原）；雙向連結：點頁面上的便利貼開視窗並選中清單那一列，清單雙擊開視窗。頁面點擊只認 /Text，避免在已標記的文字上開始選取時被視窗打斷；/FreeText 與 /Redact 明確拒絕——外觀串流畫的就是 /Contents，改字不重畫 /AP 會讓Acrobat 與其他檢視器顯示不同文字 |
| PRD-ANN-005 | Text Box 文字框 | M | R1 | 完成 | Text Box：自製 /AP、換行、/Q 對齊、/Resources /Font 寫入。**CJK 已可用**（ADR-007）：思源黑體子集內嵌，Type0 + Identity-H + CIDFontType2 + /ToUnicode，一行內拉丁與中文各自切換字型（編碼方式不同，同字型畫會變亂碼或被當成雙位元組讀掉）。中文可在任兩字之間換行且不插空白。缺字時指名是哪個碼點而非籠統失敗。端到端測試含 qpdf --check 零警告，見 tests/annfamily/test_annfamily_object_writer.cpp |
| PRD-ANN-006 | 圖章（內建 + 自訂圖片） | S | R1 | 完成 | 圖章：14 種標準 /Name 與自訂圖片兩路。內建圖章自產外觀（PDFium 不替 /Stamp 產生 /AP，缺了就是一片空白）、字級隨框寬收斂；自訂圖片內嵌成 XObject 並註冊 /Resources，RGBA 的 alpha 抽成 /SMask（PDF 影像沒有交錯式 alpha，直接寫會整張偏色）。`tests/annmgmt/test_stamp_annotation.cpp` 12 例，qpdf 檢查乾淨 |
| PRD-ANN-007 | 回覆串與狀態（已接受／已拒絕／已完成） | S | R1 | 完成 | 回覆串走 /IRT + /RT /R，狀態走回覆註解上的 /StateModel /State（不改父註解，否則「誰在什麼時候標成已完成」會消失）。已接上註解清單的右鍵選單：回覆與四種審閱狀態共用同一條寫入路徑，因為在 PDF 裡它們本來就是同一件事。測試釘住「原註解一個位元組都不變」與「新增的那一則帶 /IRT」——少了 /IRT 它會顯示成一則獨立的註解 |
| PRD-ANN-008 | 註解列表面板：依頁/作者/類型/日期篩選排序 | M | R1 | 完成 | 註解列表面板：依作者／類型／內容關鍵字篩選，依頁碼／作者/類型/日期排序。日期排序**解析 PDF 日期字串為 UTC 再比**，不是比字串——同一時刻在不同時區寫出來的字串不同，直接比字串會排錯而且沒有任何徵兆。排序在相同鍵值下穩定且次要鍵永遠是文件順序，否則每次重排結果都不同，使用者會覺得列表自己在跳。篩選後回傳的是原陣列索引，點第 N 列就跳第 N 列那一則（拿列號直接索引原陣列正是「點了第 3 列卻跳到第 7 則」的來源）。14 例見 tests/test_annotation_filter.cpp |
| PRD-ANN-009 | 屬性側邊欄連動（Ctrl+' 開啟） | M | R1 | 完成 | 註解屬性側邊欄已接上主視窗與 Ctrl+' 快捷鍵：顏色／填色／不透明度／線寬（含 PDF 的 0 = 裝置最細線）／線型／作者／主旨／內容／三個旗標；旗標逐位元設定不覆寫別的工具設的 NoZoom 等位元，選取時抑制訊號避免灌爆復原堆疊。`tests/test_annotation_properties.cpp` 9 例 |
| PRD-ANN-010 | 復原／重做涵蓋所有註解操作 | M | R1 | 完成 | 所有註解操作都走命令物件（CLAUDE.md）：文字標記三種、矩形／橢圓／便利貼、直線／箭頭、多邊形／折線、替代文字、以及刪除。復原以截回原長度實作——增量寫入是純附加，那就是精確的反操作——並以邊界守衛確認這段期間沒有別人改過檔案。刪除連同該註解的 /Popup 一起移除：只刪父註解會留下 /Parent 指向已不存在物件的孤兒 popup，數量對不上且部分檢視器會畫成空黃框（這是測試抓到的）。刪除後復原逐位元組還原，見 tests/test_highlight_flow.cpp |
| PRD-ANN-011 | 多選、對齊、跨頁跨文件複製貼上、鍵盤微調 | M | R1 | 完成 | 對齊六邊、水平垂直分布、鍵盤微調、同頁與跨頁貼上位移，皆尊重鎖定旗標。剪貼簿：自訂 MIME 型別載 XFDF（沿用既有序列化，不自創格式）＋ text/plain 摘要，因此貼到郵件得到的是可讀文字。跨文件與跨視窗貼上會依來源／目的頁尺寸以左上角為錨點換算，同頁則固定位移；貼上一律重新蓋章（沿用來源 /NM 會讓兩則註解共用同一個唯一 ID）。剪貼簿內容視為不可信任，走與檔案匯入相同的解析器。剪貼簿往返與「不亂認」見 tests/annmgmt/test_annotation_clipboard.cpp |
| PRD-ANN-012 | 鎖定／隱藏／列印旗標 | S | R1 | 完成 | 鎖定／隱藏／列印旗標，setLocked 一併設 LockedContents、setHidden 清掉 Print |
| PRD-ANN-013 | 匯入匯出 XFDF/FDF、匯出選定註解、攤平 | S | R1 | 部分 | XFDF 與 FDF 兩種格式都可匯入匯出，已接上「註解」選單（格式依內容判定，不依副檔名——收到的檔案常常名實不符）；匯出走 engine/objects/annotation_reader 讀回完整幾何，與寫入端共用同一組欄位定義；匯入整批寫在一次增量段裡，復原是一步。安全：XFDF 拒絕 <!DOCTYPE（billion laughs 與外部實體），FDF 不讀 /JavaScript 但會回報，兩者皆有 10 MB 上限，都有測試。攤平已補齊兩半：flattenAndDetachAnnotation 燒進內容之後把 /Annots 的參照摘掉——少了後半，同一個標記會出現兩份，浮在上面的那一份照樣選得到、改得動、匯得出去；順序是先燒後摘，反過來的話外觀產生失敗時註解已經消失，比完全沒攤平糟。應用層 AnnotationFlattenService 走增量附加（語意不可逆，但檔案層次仍是純附加，因此復原＝截回原長度、既有簽章維持「有效，簽章後有變更」），全有全無，沒有可攤平的註解時不寫檔（空附加段只會讓簽章狀態變差而使用者什麼都沒得到）。已接上「註解 → 攤平註解」，確認框說出會攤幾則與略過幾則。測試見 tests/annmgmt/test_annotation_flatten_service.cpp。匯出選定註解已補上：註解清單改為可多選，比對用「頁碼＋子型＋外框＋作者＋內容」而不是索引——畫面上的清單來自 PDFium 列舉、匯出的內容來自物件層 /Annots 走訪，兩條路徑對 Popup 之類的附屬註解是否計入並不保證一致，索引一旦錯開使用者就會選 A 匯出 B 而輸出檔看起來完全正常。選了 5 則只對上 4 則會明說。比對規則見 tests/annmgmt/test_annotation_selection.cpp。**仍是部分的唯一原因**：驗收條件寫的是「與 Acrobat 互通」，而互通只驗到格式層（往返序列化、XXE 防線），沒有真的用 Acrobat 匯出的檔案匯入過、也沒有讓 Acrobat 讀過我們匯出的檔案。範圍本身已經齊備 |
| PRD-ANN-014 | 量測：距離／周長／面積 | M | R1 | 完成 | 距離／周長／面積，/Measure 依 ISO 32000-1 §12.5.6.11 寫入，面積因子為距離因子平方；自相交多邊形行為已定義並釘死數值。測試見 tests/measure/test_measurement_domain.cpp 與 tests/measure/test_measure_writer.cpp |
| PRD-ANN-015 | 檔案附件註解與 Attachments 面板 | S | R1 | 完成 | 檔案附件註解已接上工具列（先框位置再問檔案——拖出矩形的當下就已經決定了「放這裡」，先跳檔案對話框會讓那個手勢懸在半空）；擋掉「把文件附加到它自己裡面」。附件讀寫兩側齊備：文件層 /EmbeddedFiles 與檔案附件註解都可列可加、名稱樹合併不會弄丟既有附件、二進位內容往返無損、qpdf 檢查乾淨；面板只提供另存不提供開啟，檔名經清理防路徑穿越 |
| PRD-ANN-016 | 音訊／視訊註解 | C | R3 | 部分 | 音訊／視訊註解：/Screen、/Movie 舊形態、/RichMedia 三種都認得，可列出、標示、取出內嵌位元組；外部參照明確與內嵌資料分開（把網址當成可另存的資料，使用者按下去只會拿到空檔案）。**刻意不播放**：播放需要媒體框架（超出固定的三件技術堆疊），而把不可信位元組餵給解碼器，等於在一個以「不執行內嵌內容」為賣點的產品裡開一個執行任意程式碼的洞。面板明講不會播放，而不是放一個按了沒反應的播放鍵 |
| PRD-ANN-017 | **Highlight Area**：區域螢光筆（不依賴文字層） | M | R1 | 完成 | Highlight Area 不依賴文字層，像素測試在中性灰底上驗證真的變色且是 Multiply 混合 |
| PRD-ANN-018 | **Free Highlight**：手繪螢光筆 | M | R1 | 完成 | Free Highlight：筆畫轉逐段 QuadPoints，共用 /Highlight 外觀路徑；急轉處有微小三角形缺口（已記錄） |
| PRD-ANN-019 | **Caret**：文字校正符號（插入／取代標記） | M | R1 | 完成 | Caret：/Sy None 與 P 兩種，純向量字形不需字型。測試見 tests/annfamily/test_annfamily_object_writer.cpp |
| PRD-ANN-020 | **Eraser**：擦除手繪註解 | M | R1 | 完成 | Eraser：點半徑擦除手繪筆畫，擦中間會把一筆拆成兩筆，過短的碎片直接丟棄。測試見 tests/annmgmt/test_annotation_tools.cpp |
| PRD-ANN-021 | **Typewriter**：打字機標註（無邊框直接打字） | M | R1 | 完成 | Typewriter：FreeTextIntent 強制無邊框，與註解自身的邊框設定無關 |
| PRD-ANN-022 | **Callout**：引線文字框（含箭頭指向） | M | R1 | 完成 | Callout：/CL 引線與 /LE 端點樣式（單一名稱，非 /Line 的二元陣列）。CJK 已可用（ADR-007，與 Text Box 共用繪製流程）——共用不等於驗過，會發生的失敗是引線畫出來了、文字整段沒有，因此測試同時驗 /CJK 註冊與 /CL 仍在。測試見 tests/annfamily/test_annfamily_object_writer.cpp |
| PRD-ANN-023 | **註解旋轉**：頂端綠色控制點，Shift 以 15 度為增量 | M | R1 | 完成 | 註解旋轉（各幾何型別繞框心）與 Shift 的 15 度吸附；圖章的 Rect 旋轉後取外接矩形，非 90 度倍數會變大而不是斜擺（已在程式碼註明）。測試見 tests/annmgmt/test_annotation_tools.cpp |
| PRD-ANN-024 | **量測比例校正（Calibrate）** | M | R1 | 完成 | 比例校正讀寫皆備；讀不到 /Measure 明確回報「未校正」而非預設 1:1，非等向縮放明確拒絕。測試見 tests/measure/test_measure_reader.cpp 與 tests/measure/test_measurement_service.cpp |
| PRD-ANN-025 | 清除距離註解的測量值 | S | R1 | 完成 | 清除量測值定義為清 /Contents 保留 /Measure 與幾何，見 tests/measure/test_measure_writer.cpp |
| PRD-ANN-026 | 線→距離、多邊形→面積、折線→周長 轉換 | S | R1 | 完成 | 線→距離、多邊形→面積、折線→周長 轉換。量測工具已接上：第一次使用時先用剛拉的那條線校正比例尺（那條線本身不寫成註解——它是一把尺，不是一則量測），之後沿用。未校正時**不寫出一個看似合理的假數字**，而是先問。轉換與未校正的失敗路徑見 tests/measure/test_measurement_service.cpp |
| PRD-ANN-027 | **量測結果匯出 CSV** | M | R1 | 完成 | 量測 CSV 匯出，含比例與單位；RFC 4180 跳脫與回讀見 tests/measure/test_measurement_service.cpp |
| PRD-ANN-028 | **註解摘要（Summarize Comments）** | M | R1 | 完成 | 「僅摘要」（純文字）與「文件加摘要」（每頁後插入該頁摘要頁，產生新檔不動原檔）皆已接上「註解 → 摘要註解」選單。插入走新增的 interleavePagesFrom：收下原始頁碼、一次算完順序、整份只重寫一次——逐次插入會位移且是每頁一次全檔重寫。摘要頁的分頁靠 layoutPlainText 新增的 \f 支援。中文摘要已可用：走 ADR-007 的內嵌思源黑體子集，內嵌字型不在時明確失敗而不是靜默丟字——空白的摘要頁會被誤讀成「這頁沒有註解」。服務層入口的插入位置、CJK 與空白摘要三條路徑見 tests/pageops/test_summary_pages.cpp。並排版面已做：每頁與它的摘要併成一張，目標頁兩倍寬、等高，原內容維持原尺寸（把正文縮成一半來騰出摘要空間，等於為了看註解而讓正文變難讀，而使用者正是為了對照才選並排的）。沒有註解的頁面原樣保留，不會併出右半空白的頁；摘要長到跨頁時只併第一張，其餘自成整頁。走新增的 mergePageGroups：所有並排頁在同一次開檔裡合成完，整份文件只重寫兩次而不是每頁一次，見 tests/pageops/test_page_merge.cpp 的四條多組合成測試 |
| PRD-ANN-029 | **Comment Styles 樣式面板** | M | R1 | 完成 | Comment Styles：只搬顏色／內部色／透明度／邊框，明確不動內容、作者、位置、旗標與幾何。測試見 tests/annmgmt/test_annotation_tools.cpp |
| PRD-ANN-030 | Fit Box by Text Content（文字框自動貼合內容） | S | R1 | 完成 | Fit Box by Text Content 兩種模式：預設維持左上錨點調整高度（寬度未設時一併調整）；fixedBox 時高度不動、改成逐級縮字直到塞得下——框已經是使用者拖出來的大小，撐高會蓋掉他剛刻意避開的內容。每一級都重新換行（字小了可能少斷一行，按比例換算會系統性縮過頭）。縮到 minFontSize 仍塞不下時回報 overflows，不靜默裁字 |
| PRD-ANN-031 | 註解段落屬性編輯（行距、對齊、縮排） | S | R2 | 完成 | 註解段落屬性：行距（係數而非點數——存絕對點數會讓使用者每次改字級都要重調一次）與縮排的引擎能力完成，並接上註解屬性側邊欄；段落屬性只對 FreeText 家族有意義，其他型別整列隱藏而非灰掉——灰掉的控制項會讓使用者一直在找怎麼啟用它 |
| PRD-ANN-032 | Redaction（塗黑並移除底層內容） | M | R2 | 完成 | 塗黑套用：文字／影像／註解真的被移除，以文字擷取與位元組雙重驗證不可還原。已接上「保護」選單：標記與套用是兩個分開的動作而不是一個帶確認的動作——標記可逆（純附加的 /Redact 註解，測試釘住原檔位元組不變），套用不可逆；合併成一個動作的話使用者一次按錯就永久刪掉內容。確認對話框會說出會處理幾塊，只問「確定要塗黑嗎」使用者無從判斷自己標對了沒有。不可還原的雙重驗證見 tests/redaction/test_redaction_apply.cpp，服務層分界見 tests/redaction/test_redaction_service.cpp |
| PRD-ANN-033 | Find and Redact（尋找並塗黑） | S | R2 | 完成 | Find and Redact 產生標記，`tests/redaction/test_find_and_redact.cpp` 13 例：命中外框以語料已知座標驗證、上限截斷會回報、標記後輸入檔逐位元組不變（標記絕不套用） |
| PRD-ANN-034 | Sanitize（清除隱藏中繼資料） | S | R2 | 完成 | Sanitize 清除 /Info、XMP、/PieceInfo、嵌入檔案、JavaScript，逐項驗位元組。已接上「保護 → 清除隱藏資訊」，與塗黑刻意分成兩個獨立動作——使用者常常只想在對外發布前清乾淨中繼資料，文件裡一個字都不用塗黑。確認對話框逐項列出會刪掉什麼：嵌入檔案是文件的一部分，刪掉之後收件人拿不到附件，而他不會知道是這一步刪的 |

## TXT

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-TXT-001 | 精準文字選取，半透明疊加提示 | M | R1 | 完成 | 文字選取與命中測試；旋轉頁與直排的閱讀順序已量測並釘住。實測結果是兩件看起來矛盾但同時成立的事：擷取順序照**顯示方向**走（/Rotate 90 時原本在上方的那一行會排到後面），而字元框座標照**未旋轉的**頁面使用者空間走。呈現層據此分工——選取錨點用座標，複製出來的文字用順序。直排（往下堆疊）驗到由上而下的順序與「一行不等於一整欄」（否則三擊選行會選走整欄）。旋轉與直排的實測見 tests/text/test_text_extractor.cpp |
| PRD-TXT-002 | 選取不改文件；複製寫入剪貼簿（純文字 + 富文字） | M | R1 | 完成 | 選取不改文件；跨頁選取拼接（頁面依頁碼排序而非拖曳方向；頁與頁之間插入換行，否則前頁末字與次頁首字黏成一個不存在的詞；全部頁面收齊才更新一次，逐頁更新會讓拖曳中每過一頁閃一次不完整選取）；複製同時放入純文字與富文字——富文字不是為了樣式（PDF 文字層沒有可靠的樣式資訊），而是為了保住換行結構，純文字貼進文書處理器後跨頁分行常被當成自動換行而重排。跨頁標記產生**每頁一則**註解且各自可復原：註解屬於單一頁面，塞成一則會讓第二頁之後的 quad 落在第一頁座標系上，畫出來是跑到頁面外的色塊。3 例見 tests/test_highlight_flow.cpp |
| PRD-TXT-003 | 矩形（區域）選取、表格複製為 TSV | S | R1 | 完成 | 矩形（區域）選取與表格切欄分列（列看 y 重疊、欄看 x 自然斷點，不假設 PDF 有表格結構——絕大多數 PDF 沒有結構資訊，假設有的實作在真實文件上會整個失效）、複製為 TSV。已接上「區域選取」工具，框選放開即複製到剪貼簿。測試見 tests/textsearch/test_table_extraction.cpp |
| PRD-TXT-004 | 雙擊選詞、三擊選行、Ctrl+A 全頁 | M | R1 | 完成 | 雙擊選詞、三擊選行、Ctrl+A 全頁。Qt 不送三擊事件（序列是 press/release/doubleClick/release/press/release），因此以雙擊時間戳加位移門檻判定緊接著的那一次 press；位移門檻是必要的——少了它，快速雙擊之後在別處點一下會選到那裡的整行 |
| PRD-TXT-005 | 文件快照（Snapshot 工具，區域複製為影像） | M | R1 | 完成 | 快照工具：只渲染框選區域而非整頁（允許的光柵化例外，理由寫在程式碼），含裁切與邊界；已接上工具列與剪貼簿。dpi 依目前檢視倍率換算而非固定 150——固定值會讓放大檢視時截到比畫面還糊的圖，而那正是使用者用快照的時機。測試見 tests/textsearch/test_snapshot.cpp |

## SRCH

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-SRCH-001 | 全文搜尋：大小寫、全字、上下一個、結果列表含上下文 | M | R1 | 完成 | 預建文字索引（ADR-005）：500 頁 A0 語料搜尋 4.2 毫秒，預算 2000。索引建置為開檔後一次性 7.2 秒的背景成本，期間退回逐頁搜尋並顯示進度。索引與逐頁 PDFium 搜尋的結果一致性由 test_text_index 交叉比對。並行搜尋已否決（ADR-005） |
| PRD-SRCH-002 | 正規表示式搜尋、多文件搜尋 | C | R2 | 部分 | 自建線性時間正規表示式引擎：明確拒絕回溯參照、非貪婪與 lookahead，(a+)+b 與 (a|a)*c 這兩個對回溯引擎會指數爆炸的樣式以實際耗時上限驗證；多文件搜尋逐份獨立把手與執行緒。未接 UI |
| PRD-SRCH-003 | 尋找並取代文字 | S | R2 | 完成 | 尋找並取代限註解內文與表單欄位值（PRD §2.1 排除內文編輯），`tests/compare/test_find_replace.cpp` 15 例含「頁面內容串流逐位元組未被碰」與 /NeedAppearances |
| PRD-SRCH-004 | 拼字檢查 | S | R2 | 部分 | 拼字檢查只作用於註解與表單文字（與 PRD-SRCH-003 同範圍），含分詞、自訂字典與忽略清單；字典來源受限於「不新增第三方相依」，內建字典規模小 |

## PAGE

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-PAGE-001 | 插入（空白／從檔案／RTF／文字）、刪除、刪除空白頁、旋轉、縮圖拖曳重排 | M | R1 | 完成 | 插入（空白／從檔案，含只取來源的部分頁／從純文字）、刪除、刪除空白頁、旋轉、縮圖拖曳重排。insertPagesFrom 獨立成一個入口而不是要呼叫端自己用 replacePages(pageCount=0)——「取代零頁」讀起來像沒有作用，寫錯成 1 就會靜默刪掉插入點那一頁，總頁數只差一，使用者好一陣子不會發現。從文字插入是先排版成獨立 PDF 再走**同一條**插入路徑，不另寫一套：兩條插入邏輯會在頁面樹處理上慢慢分歧，分歧的那一條遲早弄丟註解或搞錯順序。4 例見 tests/pageops/test_page_overlay.cpp |
| PRD-PAGE-002 | 擷取、合併、分割（依頁／依大小／依參考線） | M | R1 / R2 | 完成 | 擷取／合併／分割（依頁數、依指定頁、依大小）。刪除與擷取已接上「組織」選單，頁碼以「1,3,5-8」的範圍寫法輸入——逐頁勾選在 500 頁的文件上不可用。範圍解析遇到壞掉的片段整串作廢而不是跳過：跳過的話「1,x,5」會安靜地只刪 1 與 5，而使用者以為輸入的是三段。兩個不可復原的錯由服務層擋下並各有測試：刪光所有頁面（產出的檔案多數檢視器打不開）、擷取覆蓋來源（等於一次不可復原的刪頁）。擷取不動原檔，因此不進命令堆疊——沒有東西要復原，佔一步會讓 Ctrl+Z 看起來能取消那個已經寫出去的檔案。應用層的防呆見 tests/pages/test_page_range_service.cpp，頁碼範圍字串的解析見 tests/test_page_range.cpp |
| PRD-PAGE-003 | 裁切、裁切至白邊、頁面尺寸調整 | M | R1 | 完成 | 裁切與裁切至白邊已接上「組織」選單（整頁皆白的頁明確跳過——「沒有偵測到內容邊界」，而不是回報成功卻什麼都沒變）；頁面尺寸調整走 resizePages：兩種縮放政策由使用者選而不給預設，因為「改紙張大小」的兩種意思從操作本身推斷不出來——ScaleContent 讓內容等比縮放（A4 報告印成 A3），KeepContent 只換紙並置中、內容維持原尺寸（工程圖的唯一正解：圖上標的 1:100 是紙上的事實，縮放過的圖再量就是錯的）。實作重用 mergePageGroups 的 1×1 版面，所以 Form XObject 包裝、/Rotate 烘焙、註解與內容共用同一個矩陣全是同一份實作。五條測試見 tests/pageops/test_page_boxes.cpp |
| PRD-PAGE-004 | 頁首頁尾、頁碼、浮水印、**Bates 編號** | M | R1 | 完成 | Bates／頁首頁尾／頁碼／浮水印可印在紙上，也能純附加寫進文件並由文字層讀回。已接上「組織」選單：三者共用一個對話框三種預設而不是三份幾乎一樣的程式碼（會慢慢分岔，分岔的那份遲早少支援一個符號）；預設只決定開啟時的樣子，每個欄位都還能改——做成不可逾越的模式會逼使用者為了「浮水印放頁尾」去找別的功能。寫進文件的 Bates 依文件頁碼而非列印張數（雙面與 N-up 會讓兩者脫鉤）。頁面範圍以「1,3,5-8」輸入，留空＝全部頁面（與刪除頁面相反——多蓋一頁可以復原，多刪一頁不行）；不合法的範圍明確擋下而不是靜靜當成全部，否則整份文件會被蓋滿戳記。解析見 tests/test_page_range.cpp，對話框行為見 tests/test_stamp_dialog.cpp。浮水印目前不做旋轉與透明度（需 /ExtGState 與旋轉矩陣，屬外觀產生器範圍）；CJK 已可用（ADR-007，內嵌思源黑體子集）：拉丁與 CJK 分段畫——同一段混用的話，不是中文變亂碼就是拉丁字被當成雙位元組讀掉；子集整份只嵌一份再登記到每一頁，逐頁各嵌一份會讓五百頁的浮水印多出五百份相同字型；字寬走內嵌字型的實際值，用拉丁的 0.5 em 去算會讓置中的戳記偏出頁面一半。缺字形時整則失敗，不輸出只剩拉丁字的殘缺戳記 |
| PRD-PAGE-005 | 複製頁面（Duplicate） | M | R1 | 完成 | 複製頁面，見 tests/pages/test_page_editor.cpp |
| PRD-PAGE-006 | 交換頁面（Swap） | S | R1 | 完成 | 交換頁面，見 tests/pages/test_page_editor.cpp |
| PRD-PAGE-007 | 合併頁面（Merge Pages，多頁疊為一頁） | S | R2 | 完成 | N 頁併一頁：每頁包成 Form XObject 避免圖形狀態互相污染，註解跟著同一個矩陣搬移。測試見 tests/pageops/test_page_compose.cpp |
| PRD-PAGE-008 | 覆蓋頁面（Overlay） | S | R2 | 完成 | 疊加：九種對齊、上下層、循環對應；底頁內容先包 q/Q 再堆疊。測試見 tests/pageops/test_page_overlay.cpp |
| PRD-PAGE-009 | 取代頁面（Replace） | S | R2 | 完成 | 取代頁面，跨文件深拷貝有 key 白名單避免拖進整份來源文件。測試見 tests/pageops/test_page_overlay.cpp |
| PRD-PAGE-010 | 設定文件邊界 | S | R2 | 完成 | 五種頁面框皆可設定，子框超出 MediaBox 時夾住並回報 |
| PRD-PAGE-011 | 正規化頁面與 MediaBox 偏移 | S | R2 | 完成 | 正規化 MediaBox 原點，內容與註解一併平移；已正規化的頁面跳過不重寫 |
| PRD-PAGE-012 | 反轉頁序 | S | R1 | 完成 | 反轉頁序，見 tests/pages/test_page_editor.cpp |
| PRD-PAGE-013 | 建立編號範圍（頁面標籤） | S | R1 | 完成 | 頁面標籤 /PageLabels 讀寫與編號規則（羅馬、PDF 規格的重複字母序列、前綴、起始編號、標籤反查頁碼），qpdf 檢查乾淨；編輯對話框已完成（組織選單）。編輯單位是**範圍**不是單頁——PDF 的 /PageLabels 就是一串範圍，做成逐頁編輯會讓使用者以為可以只改一頁。範圍起點重疊時擋下來而不是自己挑一段贏，猜錯會讓整份文件頁碼跑掉。走增量儲存，因此既有簽章仍是「有效、簽章後有變更」而非「無效」 |

## BM

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-BM-001 | 書籤面板檢視編輯、使用者自建、縮放類型設定 | M | R1 | 完成 | 書籤讀寫、八種縮放類型、10 層巢狀，走物件層通道純附加。書籤讀寫見 tests/bookmarks/test_outline_writer.cpp |
| PRD-BM-002~019 | 標題加文字、每 N 頁加書籤、由書籤建目錄頁、大小寫轉換、命名目標雙向轉換、由書籤建連結、匯出 HTML/TXT、文字尋找取代、由目錄頁／文字檔／高亮／頁面文字產生書籤、合併重複、移除動作、依書籤排序頁面、驗證書籤 | S | R2 | 完成 | 18 條批次操作：加前後綴、每 N 頁、大小寫、目錄頁、命名目標雙向、建連結、匯出 HTML/TXT、尋找取代、合併重複、移除動作、依書籤排序頁面、驗證。由頁面文字產生書籤已實作但未接文字層。測試見 tests/bookmarks/test_bookmark_ops.cpp 與 tests/bookmarks/test_bookmark_batch.cpp |
| PRD-BM-020 | 進階書籤巨集 | C | R3 | 部分 | 五種書籤批次操作（加前後綴、每 N 頁、大小寫、尋找取代、合併重複）接進既有的巨集系統（domain::macro::MacroActionKind + app::BatchRunner），沒有另建第二套機制，見 tests/test_macro.cpp／tests/test_batch_runner.cpp 的 Bookmark* 測試。移除動作（PRD-BM-017）刻意不做成巨集步驟：批次情境下一步做錯就是整批書籤失去跳轉能力，風險與其餘五種不對稱，留在互動式書籤面板 |

## FORM

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-FORM-001 | 表單填寫：文字、核取、單選、下拉、清單 | M | R1 | 完成 | 文字/核取/單選/下拉/清單填寫，`tests/forms/test_form_fields.cpp` |
| PRD-FORM-002 | 資料匯入匯出（FDF/XFDF）、重設、攤平 | S | R1 | 完成 | FDF/XFDF 匯入匯出、重設、攤平，已接上「表單」選單。匯入的格式由**內容**判定而不是副檔名（收到的檔案常常名實不符）；重設會先問，因為表單填寫走 PDFium 的表單環境而不是物件層通道，沒有「截回原長度」那條復原路徑，所以它不進命令堆疊。重設只讀 widget 的 /DV，繼承自父欄位的預設值讀不到。資料往返見 tests/forms/test_form_data.cpp |
| PRD-FORM-003 | XFA 偵測與降級提示（僅顯示後備內容） | M | R1 | 完成 | XFA 偵測與降級提示，永不呼叫 FPDF_LoadXFA；偵測見 tests/forms/test_form_fields.cpp，降級文字見 tests/test_document_properties_dialog.cpp |
| PRD-FORM-004 | 表單欄位高亮 | M | R1 | 完成 | 欄位高亮已接上「表單 → 標示表單欄位」：半透明填色加實線外框——只畫外框在深色內容上看不見，只填色又會蓋掉欄位裡已填好的文字，兩者都是「標示」變成「妨礙」的方式。純檢視層疊加，不改文件。測試見 tests/forms/test_form_fields.cpp |
| PRD-FORM-005 | 辨識表單（Identify Forms） | S | R1 | 完成 | 辨識表單，見 tests/test_form_controller.cpp |
| PRD-FORM-010~020 | **建立/編輯欄位**：文字、核取、單選、下拉、清單、按鈕、日期、影像、數位簽章欄位 | M | R2 | 完成 | 九種欄位皆可建立（文字、核取、單選、下拉、清單、按鈕、日期、影像、簽章），加上屬性編輯、對齊、外觀特徵、Tab 順序（/Tabs /W，只在頁面尚無 /Tabs 時寫入不覆蓋既有選擇）、複製欄位。日期與影像在 PDF 規格裡不是獨立 /FT——Acrobat 用 JavaScript 格式化器實作日期，本產品零腳本執行因此改以私有鍵記錄格式並直接畫出已格式化的文字 |
| PRD-FORM-021 | Fields 面板 | M | R2 | 完成 | Fields 面板資料模型：點分層建樹、中介節點、同名既是欄位又是父節點。測試見 tests/formbuild/test_field_tree.cpp 與 tests/test_form_controller.cpp |
| PRD-FORM-022 | **表單計算與驗證**（受限運算式引擎，非完整 JavaScript） | S | R2 | 完成 | 受限運算式引擎：四則／SUM/AVG/MIN/MAX/IF、循環參照偵測、深度與欄位數上限。不引入 JavaScript |
| PRD-FORM-023 | CSV 灌入表單 | S | R2 | 完成 | CSV 灌入表單：表頭對不上視為整份 CSV 的結構問題（記錄忽略欄，全部對不上才中止），資料列欄數不符由政策明確決定中止或跳過，報告逐列列出結果。批次協調移到 app 層——原本在引擎回呼裡解構 FormDocument 會自我 join 導致 std::terminate，是整合測試抓到的真 crash |
| PRD-FORM-024 | Email 表單資料 | C | R2 | 部分 | 產生 FDF/XFDF 並交給系統郵件用戶端，Windows 走動態載入的 Simple MAPI（找不到用戶端時明確回報而非崩潰），寄送函式可注入因此測試不觸碰真實 MAPI；macOS/Linux 未實作 |
| PRD-FORM-025 | 條碼欄位 | C | R3 | 完成 | 條碼欄位：新的 Barcode 欄位型別，一律唯讀，外觀走同一支 Code 128 繪製器；矩形扣掉靜區與留白後無可畫區域時明確失敗而非留白——留白的條碼會被誤判成「這裡本來就沒內容」。測試見 tests/formbuild/test_barcode_field.cpp 與 tests/test_barcode.cpp |

## SEC

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-SEC-001 | 開啟密碼保護文件（RC4/AES-128/AES-256），尊重權限旗標 | M | R1 | 完成 | 開啟密碼保護文件（RC4/AES-128/AES-256）並尊重權限旗標：列印、複製／擷取（含快照、區域選取、匯出）、加註（全部十一個註解工具與刪除）、頁面重組（含關掉縮圖拖放——動作灰化擋不住拖曳）、表單填寫，開檔後一次套用。**灰化而不隱藏**：隱藏會讓使用者以為產品沒有這個功能，灰化加 tooltip 才說得出「是這份文件不允許」。標頭與程式碼都寫明這**不是安全機制**——權限旗標只有在文件加密下才有強制力，任何人都能用別的工具拿掉；這裡尊重的是文件作者的意圖，不是防止規避加密讀取與權限旗標見 tests/iosec/test_security_saver.cpp，權限對 UI 的灰化見 tests/test_security_policy.cpp |
| PRD-SEC-002 | 設定／移除密碼與權限 | S | R1 | 部分 | 讀出加密演算法與權限旗標、移除密碼（整份解密重寫）皆完成並以真實 RC4 語料端到端驗證；設定新密碼在 PDFium 公開 API 下不可行，明確回報不支援，見 exceptions/EXC_20260906_RD_SA_set_password_unsupported.md |
| PRD-SEC-003 | 安全性原則建立與套用 | S | R3 | 部分 | 安全性原則的建立、列舉、刪除完成；套用只有「完全開放」的原則能生效，涉及密碼或任何權限限制一律回報 blocked——權限旗標只有在文件加密下才有強制力，而 PDFium 公開 API 無法輸出加密文件 |

## SIG

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-SIG-001 | **簽章驗證**：`/ByteRange` 完整性、PKCS#7 解析、憑證鏈、吊銷查詢（CRL/OCSP） | M | R1 | 完成 | /ByteRange 完整性、PKCS#7、憑證鏈、CRL 吊銷、三態燈號；端到端以自簽 CA 語料驗過，含增量儲存攻擊偵測 |
| PRD-SIG-002 | Signatures 面板 | M | R1 | 完成 | 簽章面板：三態燈號同時有文字標籤（不靠顏色單獨傳達）、findings 逐條列出不摘要、部分涵蓋明確標示未受保護的位元組數。tests/signature/test_signature_panel.cpp 釘住「狀態不只靠顏色」——那條規則先前沒有任何機制看著，被「簡化」掉也不會有測試變紅 |
| PRD-SIG-003 | **增量儲存不破壞既有簽章** | M | R1 | 完成 | 增量儲存不動原簽章位元組、密碼學驗證維持通過、新增內容被正確標示為未涵蓋，端到端測試見 tests/iosec/test_signed_incremental_save.cpp。部分涵蓋的判定已由 EXC_20260906_RD_SA_sig003_trust_gap 裁決為黃燈（無法確認）而非紅燈（無效）——判紅燈等於讓每一份被自己編輯過的簽章文件都顯示無效，那是對本產品核心迴圈的誤報 |
| PRD-SIG-004 | 建立數位簽章（PAdES-B） | M | R3 | 完成 | PAdES-B 建立簽章：CMS SignedData、/SubFilter ETSI.CAdES.detached、signingCertificateV2 signed attribute（擋憑證替換攻擊）、等寬佔位覆寫不動其餘位元組；可見簽章外觀（文字加可選的手寫簽名影像，等比例縮放不變形，RGBA 走 /SMask）與無實體兩種都支援。以專案自己的驗證側判定為 Trusted、qpdf 檢查乾淨，加了外觀之後密碼學保證不變也有測試釘住。外觀上刻意不寫「已驗證」字樣——外觀可以偽造，讓它宣稱有效是誤導。未在 Acrobat 實機開啟。已接上「保護 → 數位簽署」：憑證走 PKCS#12，密碼只在該次呼叫的期間存在於記憶體（不進設定、不進 log），私鑰位元組用完立刻抹掉；憑證載入失敗的訊息刻意不區分「密碼錯」與「檔案壞」，否則等於給攻擊者一個判斷密碼的預言機。簽署不進命令堆疊——「復原簽署」的實作就是刪掉簽章，而那不是 Ctrl+Z 的預期 |
| PRD-SIG-005 | 多頁批次簽章 | S | R3 | 部分 | 語意由 ADR-004 裁定為「同一份文件、多頁上的簽章欄位依序簽署」（PRD 用字是「多頁」不是「多份文件」）。共同基礎 signBatch() 已備，前置條件的可見簽章外觀也已完成；剩下的是逐欄位簽署的 UI 與流程 |
| PRD-SIG-006 | 長期驗證（LTV）與時間戳伺服器 | S | R3 | 部分 | PAdES-B-T：RFC 3161 時間戳用戶端（engine::signature::timestamp_client.h，Transport 注入、CI 不連網）已接上 createSignature()，一次序列化內把 TimeStampToken 嵌進 CMS unsigned attribute；驗證側（pkcs7_verifier）偵測並回報 hasTimestamp/timestampGenTimeUnix。DSS／PAdES-LT：dss_builder.h 已可附加 /Root /DSS 含 /Certs /CRLs /OCSPs /VRI，去重共用證據物件。均以 qpdf --check 與自建驗證側驗證，見 tests/ltv/。已知缺口：時間戳權杖本身的簽章未驗證（需要 TSA 信任錨，TrustStore 目前只承載簽署鏈）、timestampGenTimeUnix 未接進 classify() 判色邏輯、/DSS 不支援與既有 DSS 合併、OCSP/CRL 證據需呼叫端自行蒐集後餵入（本身的自動蒐集流程未做） |
| PRD-SIG-007 | 認證文件（Certify，含無實體簽名） | S | R3 | 部分 | 認證文件（Certify）：/Reference /DocMDP 與 /Root /Perms /DocMDP，三種變更層級；「無實體簽名」與 SIG-004 的限制天然吻合。DocMDP /Reference 省略 /Data 是否完全符合 Acrobat 解讀未經實機驗證 |
| PRD-SIG-008 | 手寫／輸入式簽名（非密碼學簽章） | S | R2 | 完成 | 手寫／輸入式簽名：手繪筆跡走 /Ink、匯入影像走自訂圖片 /Stamp，簽名庫存使用者端不寫進文件。與數位簽章型別上完全分開，且一律帶固定 /Subj 標記讓簽章面板排除——把手寫簽名顯示得像數位簽章是本產品最不該犯的錯。`tests/test_handwritten_signature.cpp` 12 例 |

## IO

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-IO-001 | 增量儲存（僅附加變更物件並更新 xref） | M | R1 | 部分 | 增量儲存前綴逐位元組驗證，qpdf 結構檢查已接且全部乾淨。「儲存／另存／復原所有修改」已接上檔案選單，語意見 ADR-008——這個架構下每一次修改都當下就寫進磁碟，沒有「未存檔的修改」這個狀態，所以儲存是一次明確的落盤與確認訊息、另存是分岔出副本並切換過去、復原是把命令堆疊倒回本次開啟時（回不到更早，那需要保留每次開啟的完整副本）。PRD §8.1 的兩條數字已量測（tests/save/test_large_incremental_save.cpp）：100 MB 文件加一個註解 **245 ms**（目標 ≤ 300）、**增量段 1170 位元組**（目標 ≤ 20 KB）。增量段大小是行為而非機器特性，因此寫成硬門檻；耗時與磁碟速度強相關，斷言放寬到一個數量級並把實測值 qInfo 出來——在 CI 上寫死 300 ms 只會得到偶發紅燈，真正的驗收要在目標機器上看那個數字 |
| PRD-IO-002 | 另存新檔與最佳化（移除未使用物件、線性化） | S | R1 | 部分 | 移除未使用物件（整份重寫的可達性標記，已用孤兒物件實測驗證）與簽章防呆完成；線性化在 PDFium 公開 API 下不可行，明確回報不支援，見 engine/save/optimize_saver.h |
| PRD-IO-003 | 自動儲存（預設 2 分鐘）與異常復原 | M | R1 | 完成 | 自動儲存與 side-car 復原偵測。測試見 tests/save/test_autosave.cpp |
| PRD-IO-004 | 外部變更偵測 | S | R1 | 完成 | 外部變更偵測：大小 + mtime + 尾端雜湊三層判準，`platform/external_change.h`，`tests/iosec/test_external_change.cpp`。已接上主視窗：開檔時取快照，任何寫入前比對，變更時給重新載入／仍要覆蓋／取消三選一（預設取消，覆蓋不是預設因為它會毀掉別人的版本且無法復原）；檔案被刪除時不給覆蓋選項，導向另存新檔 |
| PRD-IO-005 | 唯讀與雲端同步資料夾相容（OneDrive/Drive 桌面用戶端） | M | R1 | 完成 | 唯讀偵測，以及雲端同步資料夾的偵測與歸屬判定（`tests/test_cloud_folders.cpp` 10 例） |
| PRD-IO-006 | 最近檔案、拖放、命令列選項、系統檔案關聯 | M | R1 | 部分 | 最近檔案、拖放、命令列已做，見 tests/test_settings.cpp 與 tests/test_session.cpp。系統檔案關聯未做，且不打算由程式自己做：Windows 的關聯是安裝時寫進 HKCR 的，由執行中的應用程式偷寫登錄檔既需要提權、也會在使用者解除安裝後留下孤兒項目。與 help.checkUpdates 同屬發佈流程（WP8），要等安裝包才做得完整 |
| PRD-IO-007 | 匯出頁面為 PNG/JPEG/TIFF（可設 DPI）、匯出純文字 | S | R1 | 完成 | 匯出頁面為 PNG/JPEG/TIFF（可設 DPI）與匯出純文字皆完成並測過，已接上檔案選單與 Ribbon。格式清單只列這台機器真的有 Qt 影像外掛的（isFormatSupported），格式取自使用者選的篩選器而非副檔名——依副檔名靜默改寫會讓「選了 PNG 卻得到 JPEG」 |
| PRD-IO-008 | 列印：範圍、縮放、雙面、含／不含註解、海報分割 | M | R1 | 部分 | 列印範圍／縮放／雙面／含註解／海報分割已做，列印預覽已接上檔案選單（與實際列印共用同一個 PrintService——兩條各自渲染的路徑會讓「預覽看到的」與「印出來的」慢慢分岔，而那要印出來才會發現）。頁面內容是點陣非向量：PDFium 的公開 API 只給得出點陣輸出 |
| PRD-IO-009 | 從影像／掃描器建立 PDF | S | R2 | 完成 | 從影像建立 PDF：JPEG 直通不重編、其餘走 Flate，多頁與頁面尺寸策略，`tests/create/` |
| PRD-IO-010 | 從 URL 開啟文件 | S | R2 | 完成 | 從 URL 開啟：僅允許 http/https、需使用者確認、大小與型別上限，下載後以一般文件路徑處理。測試見 tests/create/test_url_source.cpp 與 tests/create/test_web_page_import.cpp |
| PRD-IO-011 | Email 文件 | S | R2 | 完成 | 以電子郵件傳送文件，沿用既有的 Simple MAPI 封裝；文件尚未存檔時在呼叫寄送前就明確失敗——不寄一封看起來附了檔案實際沒附的信。測試見 tests/test_document_email.cpp |
| PRD-IO-012 | 從剪貼簿／文字檔／Markdown 建立 PDF | S | R2 | 完成 | 從文字/Markdown 建立：內建字型度量斷行、標題／清單／程式碼區塊，qpdf 檢查乾淨 |
| PRD-IO-013 | 從 CSV／Email 建立 PDF | C | R3 | 部分 | 從 CSV 建立 PDF：欄寬協商、跨頁表頭重複、超長儲存格截斷；.eml 解析未做（MIME 是另一個獨立問題，做半套會誤導使用者） |
| PRD-IO-014 | 從網頁 URL 建立 PDF | C | R3 | 部分 | 完整 HTML/CSS 排版 BLOCKED：Qt WebEngine 會超出 130 MB 安裝包上限並帶進完整 Chromium 與 JS 執行環境，與 PRD §8.2「不執行不可信程式碼」直接衝突。已交付降級管線：URL 驗證、大小與型別把關、剝除 script/style 後抽出文字走既有的純文字排版，程式碼標頭明確標示這是閱讀檢視匯出而非網頁列印 |

## CMP

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-CMP-001 | **文件比對**（文字差異 + 並排標示） | M | R1 基礎 / R2 完整 | 完成 | 頁面對齊 + 頁內文字差異，端到端測試含「插入一頁不連鎖誤報」。已接上「檢視 → 比較文件」（放檢視而不是組織：它不改任何東西，而組織底下每一項都會改寫檔案）。呈現先列有差異的頁面、選了才看該頁的段落——整份文件的差異動輒數千條，平鋪沒有人讀得完，而使用者的問題是「哪幾頁改了」。沒變的頁面不列，否則真正要看的那幾頁被埋掉。兩件事一定要說出來並各有測試：完全相同（空清單無法分辨「真的一樣」與「比對失敗」）、以及降級（結果仍正確但某些段落是整段替換而非最小差異，SDD §7）。呈現面見 tests/compare/test_compare_dialog.cpp，比對核心見 tests/compare/test_document_compare.cpp |
| PRD-CMP-002 | 尋找重複頁面 | S | R2 | 完成 | 重複頁偵測，文字與尺寸雙重指紋。測試見 tests/compare/test_document_compare.cpp 與 tests/compare/test_page_align.cpp |

## ENH

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-ENH-001 | 新增背景 | S | R2 | 完成 | 頁面背景：純色與影像、Fit/Fill/Stretch、九宮格對齊、留白、旋轉、透明度；插在既有內容之前。測試見 tests/enhance/test_enhance_pdf.cpp |
| PRD-ENH-002 | 掃描頁去斜（Deskew）與增強 | S | R2 | 完成 | 去斜（投影剖面法，±15 度內）與對比／亮度／灰階／Otsu 二值化；整頁重新點陣化，既有文字層會被光柵化。測試見 tests/enhance/test_scan_enhance.cpp 與 tests/enhance/test_image_ops.cpp |
| PRD-ENH-003 | 頁面點陣化（Rasterize） | S | R2 | 完成 | 指定 dpi 點陣化，/Annots 與 /Rotate 不動，DCT/Flate/Auto 三種編碼。測試見 tests/enhance/test_enhance_pdf.cpp |
| PRD-ENH-004 | 影像重壓縮 | S | R2 | 部分 | 影像重壓縮：DCT/Flate、8-bit Gray/RGB。目前不重取樣、尺寸不變，因此 /SMask 與間接 /Mask 原樣保留（不是併回 alpha 再輸出）——遮罩必須與主影像同尺寸，重取樣一旦實作就得連遮罩一起處理，見 src/engine/enhance/image_recompressor.h。targetDpi 重取樣未實作，索引色／CMYK／CCITT 回報不支援 |
| PRD-ENH-005 | 檢視／編輯文件屬性 | M | R1 | 完成 | 文件屬性對話框，含不支援項目的降級說明；tests/test_document_properties_dialog.cpp 驗 XFA 與 JavaScript 的降級文字真的出現，且普通文件不出現多餘警告——把警告常態化，使用者就不再讀它了 |
| PRD-ENH-006 | 色彩轉換與 Recolor | S | R3 | 部分 | 色彩轉換：灰階／去飽和／指定色替換，涵蓋影像與內容串流的裝置色運算子（g/G/rg/RG/k/K/sc/scn）。**未涵蓋** Separation/DeviceN（需要色調轉換函式的直譯器）、ICCBased/Indexed、以及 /ExtGState 的混合模式，未涵蓋的部分會計入報告的 skipped 計數而非靜默略過 |
| PRD-ENH-007 | 新增條碼 | C | R3 | 部分 | Code 128 自繪向量條碼（編碼正確性以已知參考編碼比對，不只驗有畫出東西）；QR 未做，糾錯碼與遮罩選擇的複雜度與本工作包其餘部分相當 |

## UI

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-UI-001 | 多頁籤；頁籤拖曳分離為獨立視窗、再合併 | M | R1 | 完成 | QTabBar 已接上 TabLayoutModel（狀態模型 11 例）：開啟／切換／關閉／拖曳重排，同一份文件只會有一個頁籤，主視窗關到剩零頁籤不關閉。分離走頁籤右鍵「在新視窗開啟」，新視窗是完整的 MainWindow（自己的控制器與 PDFium 執行緒）；合併＝在該視窗關掉頁籤。不用拖曳分離：拖出視窗邊界的判定在多螢幕與高 DPI 下不穩定，而拖出去找不回來的成本遠高於多按一次右鍵。一個視窗只有一個 DocumentController——十個頁籤各持一份把手就是十條 PDFium 執行緒與十份圖磚快取，而非作用中的頁籤只需要閱讀位置（HistoryStore 已有）。所有頁籤都寫進 session，下次啟動全部還原但只開作用中的那一份 |
| PRD-UI-002 | Ribbon（File/Home/View/Comment/Protect/Form/Organize/Help）+ 快速存取列 + 可切回傳統選單 | M | R1 | 部分 | 自研 Ribbon 八分頁 + QAT + 收合 + KeyTips。預設配置 108 顆按鈕，目前 94 顆已接上真正的動作、14 顆仍是停用佔位，且全部有明確歸屬：PDFium 公開 API 做不到（password / permissions / removeSecurity，見 exceptions/）、R2 或 R3 的項目（sign.timestamp / sign.certify / page.background / tool.zoomArea / form.tabOrder，依 CLAUDE.md 不提前實作）、屬於發佈流程 WP8 的項目（help.checkUpdates / releaseNotes / contents / reportIssue，需要真實的更新伺服器與說明網址）、以及架構上不成立的 session.save / restore（session 是自動的，見 ADR-008）。test_ribbon_wiring 以棘輪門檻守住這個數字——把按鈕加進配置卻忘了註冊動作不會有任何錯誤，那顆按鈕只是永遠停用，看起來像壞掉。窄視窗的裁切已修：分頁內容改走橫向捲動容器，快速存取列放不下的按鈕收進「»」溢位選單，兩者都不再把「內容的自然寬度」當成主視窗的最小寬度（那會讓視窗縮不小，等於用另一種方式讓按鈕到不了）。捲軸高度一律預留，Ribbon 的高度因此不隨視窗寬度跳動。見 tests/ribbon/test_ribbon_bar.cpp 的三條窄視窗測試 |
| PRD-UI-003 | 可停靠面板 12 個：縮圖、書籤、註解、搜尋、屬性、圖層、附件、簽章、History、命名目標、連結、欄位 | M | R1 / R2 | 完成 | PRD 列的十二個面板全數接上主視窗並可從 Ribbon 開關：縮圖／書籤／註解／搜尋／文件屬性／圖層／附件／簽章／History／命名目標／連結／欄位，另加註解屬性。（順手修掉註解與搜尋兩個面板的開關因成員未指派而無作用的缺陷。）連結面板只列目前這一頁——連結是逐頁向引擎要的，開檔掃 500 頁會塞爆渲染佇列；外部網址完整顯示不截短（截短的網址是釣魚連結最好用的偽裝）；按下去只發訊號交回 MainWindow::followLink 的確認流程，面板自己開啟等於多一個繞過 PRD §8.2 的側門。面板收起來時不跟著翻頁，打開的當下補讀一次。測試見 tests/test_links_panel.cpp 與 tests/test_ui_smoke.cpp |
| PRD-UI-004 | 快捷鍵體系與自訂，預設對齊 Acrobat / PDF-XChange | M | R1 | 部分 | 快捷鍵表已接上主視窗（依 id 套到已註冊動作，表比程式新時靜靜跳過而非報錯）：可序列化、情境感知衝突偵測（衝突時拒絕而非靜默覆蓋）、匯入匯出為全有全無、還原預設，15 例；單鍵工具捷徑的 Acrobat 對齊未經實機驗證，自訂鍵位的偏好設定頁已補上（偏好設定分「一般」與「快捷鍵」兩頁）：改鍵、清除、全部還原、匯入匯出，衝突明確拒絕並說明而不是靜默覆蓋；鍵位即時套用並存進 QSettings，啟動時整批驗證，不合就整批落回預設。測試見 tests/test_shortcuts_page.cpp 與 tests/uisystem/test_shortcut_scheme.cpp |
| PRD-UI-005 | 多語系：繁中、英（必做）；簡中、日 `[待確認]` | M | R1 | 完成 | 繁中（原文）與英文皆可用：lupdate 全樹掃描 701 條字串，英文 701/701，並由 test_locale_manager 的 100% 覆蓋率門檻擋住漏跑 lupdate 的腐化；語言選項在偏好設定，於 main() 建出任何 widget 之前安裝翻譯器，未設定時跟隨系統語言。切換語言需重新啟動才生效（tr() 在建構式即求值，線上重繪需要每個 widget 覆寫 changeEvent/retranslateUi）；PRD 未要求線上切換 |
| PRD-UI-006 | 淺色／深色主題，可跟隨系統 | S | R1 | 完成 | 主題已接上主視窗：色票以資料描述（非散落的 setStyleSheet 字串）套成 QPalette 交給 qApp，跟隨系統走 QStyleHints::colorSchemeChanged，選單可切三種模式；明暗兩套的每組文字／背景對比度都 ≥ 4.5:1 並以測試釘住 |
| PRD-UI-007 | 工具持續模式 | S | R1 | 完成 | 工具持續模式的判定函式與測試，並已真正套用：註解寫入成功後把工具切回選取（持續模式開啟時留在原工具），同時把工具列上的打勾一起改掉——兩者脫鉤時沒有任何錯誤，但工具其實已經切回選取而按鈕仍反白著，使用者下一次拖曳會以為自己在畫第二個。設定存在偏好設定裡，預設關閉。預設關閉與鍵位見 tests/test_ui_smoke.cpp |
| PRD-UI-008 | 偏好設定（渲染、快取、自動儲存、預設縮放、作者名稱） | M | R1 | 完成 | 偏好設定（快取、自動儲存、作者、夜間模式），`tests/test_settings.cpp` |
| PRD-UI-010 | **自訂工具列** | M | R1 | 完成 | RibbonCustomizeDialog 編輯 ribbon::Layout，結果寫回 ribbon.json；只列已註冊的動作，避免加入永遠停用的按鈕 |
| PRD-UI-011 | **自訂 Ribbon**（自組常用工具分頁） | M | R1 | 完成 | 快速存取列與 Ribbon 共用同一個對話框（同一份 Layout 資料）；不允許刪整個分頁或群組，避免無法復原的操作。快速存取列的編輯見 tests/test_ribbon_customize.cpp |
| PRD-UI-012 | **Session 儲存與復原** | M | R1 | 完成 | Session 儲存與復原：閱讀位置、縮放、版面、視窗與面板佈局；壞掉的設定退回預設。測試見 tests/test_session.cpp |
| PRD-UI-013 | **觸控最佳化 UI**（大圖示、點按取代點擊、長按開選單） | S | R1 | 部分 | 觸控手勢核心：44 CSS px 等效的觸控目標換算、長按判定（時間與位移門檻分離，**位移超過門檻即永久取消長按資格**，滑回原點也不恢復——這是最容易做錯的一點）、雙指縮放以兩指中點為錨點；PageView 已接上 QTouchEvent 與長按選單。Ribbon 的觸控模式已補上（偏好設定可開關）：開啟時每顆按鈕的 sizeHint 不低於 44 CSS px 等效，依 logicalDpi 換算而非寫死像素，換配置後重新套用（rebuild 造的是新按鈕，漏掉就會靜靜退回滑鼠尺寸而開關看起來還開著）。不預設開啟——滑鼠使用者按 44px 沒有好處，而整條 Ribbon 會明顯變高。逐顆按鈕的門檻檢查見 tests/ribbon/test_ribbon_bar.cpp |
| PRD-UI-014 | 設定檔匯出／匯入／重設 | S | R2 | 完成 | 設定檔匯出／匯入／重設：偏好設定、快捷鍵表、主題、Ribbon 配置一次搬移；版本比程式新時明確拒絕而非半套讀取，快捷鍵表壞掉不會讓整份匯入失敗但會列進 skippedKeys，重設回報改了哪些項目。**刻意不含最近檔案、文件歷史與簽名庫**——那是個人足跡，跟著設定檔散出去等於洩漏使用者看過哪些文件。`tests/test_settings_profile.cpp` 11 例 |
| PRD-UI-015 | 建立自訂設定檔（企業部署用） | S | R2 | 部分 | 企業部署：設定檔可標記鎖定項目，套用時列入 skippedKeys 供 UI 停用對應控制項；鎖定清單存在設定檔而非程式裡（每個組織要鎖的不同）。部署工具與唯讀位置的載入流程未做 |
| PRD-UI-016 | 啟動第三方程式工具列 | C | R3 | 完成 | 第三方程式工具列，四條安全限制逐條落實：只用 startDetached 不做權限提升、參數樣板只認 {file} 一個佔位字元（其餘一律拒絕，因此 PDF 內容無法決定參數）、啟動前顯示完整命令列供確認、與 Launch Action 之間沒有任何路徑連通 |
| PRD-UI-017 | 分頁標題控制與文件重新命名 | S | R1 | 完成 | 分頁標題（自訂名稱 > 檔名 > 未命名、未存檔標記、同名時以上層目錄消歧）與文件重新命名，已接上檔案選單。改名後**重新開檔**而不是只改標題——所有子系統握的都是舊路徑，之後任何一次存檔都會寫回一個已經不存在的檔名。目標已存在時不提供覆蓋選項：改名覆蓋另一份 PDF 無法復原，而使用者要的幾乎一定是換個名字。檔案開著仍能改名，靠的是 platform/shared_file.h 的 FILE_SHARE_DELETE |
| PRD-UI-018 | 可調整游標大小 | C | R2 | 完成 | 可調整游標大小：四級縮放與依 DPI 重畫的自畫十字準星（系統游標由 Windows 自行縮放，只有自畫點陣圖需要自己處理），設定持久化並接上偏好設定。測試見 tests/uisystem/test_cursor_scale.cpp 與 tests/test_ui_smoke.cpp |
| PRD-UI-019 | 字數統計 | C | R2 | 部分 | 字數統計：CJK 逐字、拉丁以空白分詞，含空白與不含空白的字元數分開；屬 R2，刻意未接任何選單 |
| PRD-UI-020 | 動態 Shell 擴充（檔案總管預覽與縮圖） | C | R3 | 範圍外 | **經 ADR-007 決議排除**：擁有者決定不做 COM 註冊。三個風險換不到相稱的價值——需要安裝程式與權限提升、會搶走系統唯一的 .pdf 縮圖處理常式（移除時必須正確還原，做錯會讓使用者永久失去 PDF 縮圖）、擴充崩潰會把整個檔案總管帶走。已完成的縮圖渲染程式碼保留（不帶 Qt、每條失敗路徑都安靜失敗並有測試） |

## A11Y

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-A11Y-001 | Tags 面板（結構化 PDF 檢視編輯） | S | R2 | 部分 | Tags 面板的 /StructTreeRoot 解析與結構樹建立（走物件層，不連 PDFium），沒有標籤結構時明確顯示而非空面板；編輯未做 |
| PRD-A11Y-002 | 閱讀順序（Order）面板 | S | R3 | 部分 | 閱讀順序面板：同時顯示結構順序（權威）與 MCID 推得的內容順序並標出不一致——只顯示一種等於看不出問題在哪。第二軸是內容串流順序而非真正的幾何位置，那需要一套新的文字定位子系統，限制已寫在 reading_order.h 並在面板上標示 |
| PRD-A11Y-003 | 無障礙檢查器與報告面板 | S | R3 | 完成 | 無障礙檢查器：八項檢查（標題、語言、標籤結構、替代文字、表頭、標題階層、閱讀順序不一致、部分對比度），**每次都輸出涵蓋清單與完全未檢查的項目清單**——讓使用者以為「檢查通過等於沒問題」比不檢查更危險。開檔時自動執行。測試見 tests/a11y2/test_accessibility_checker.cpp |
| PRD-A11Y-004 | 內容替代文字設定 | S | R3 | 完成 | 替代文字寫進結構元素的 /Alt，走增量附加，可復原重做，入口在 Tags 面板的右鍵選單。測試見 tests/a11y/test_struct_tree.cpp 與 tests/a11y2/test_struct_alt_text_writer.cpp |
| PRD-A11Y-005 | 螢幕閱讀器支援（Windows UIA / macOS NSAccessibility / Linux AT-SPI）、全鍵盤、高對比 | M | R1 | 部分 | 可自動化的無障礙稽核：走訪 widget 樹找出鍵盤不可達的控制項、缺 objectName／可及性名稱者、硬編色票；主視窗目前零缺失並以測試釘住。明暗兩套色票的對比度 ≥ 4.5:1。UIA 的實機驗證與螢幕閱讀器實測未做 |
| PRD-A11Y-006 | 朗讀（Read Out Loud） | C | R3 | 部分 | 朗讀走 Windows SAPI（平台層），播放暫停停止與自動翻頁可用；文字來源依 /ActualText → /Alt → /T 的優先序，**沒有 /ActualText 的一般段落讀不到**（需要 MCID 對應字符的關聯，屬於另一個子系統），跳過的項目有計數不靜默。翻頁狀態機沒有單元測試 |

## CLD

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-CLD-001~005 | SharePoint / OneDrive / Google Drive / Dropbox / Box 原生開檔存檔 | S | R3 | 範圍外 | **經 ADR-006 決議排除**：專案擁有者明確指示本產品定位為單機工具，不需要雲端功能。OAuth + REST 原生 API 路徑因此不做。已完成的同步資料夾偵測保留——它是純本機功能（偵測 OneDrive／Google Drive／Dropbox／Box 的同步根目錄、最長前綴優先判定歸屬、在 UI 標示來源），解決的是「存下去會不會同步出去」這個會影響使用者要不要寫敏感註解的問題，與雲端帳號無關 |

## MAC

| ID | 需求 | 優先 | 版本 | 狀態 | 備註 |
|---|---|---|---|---|---|
| PRD-MAC-001 | 巨集系統與批次處理 | S | R3 | 部分 | 巨集是一串具名參數化的內建動作（JSON 可序列化、可重播），**刻意不引入腳本引擎**——與關閉 V8 的安全立場一致，而使用者要的是「對 200 個檔案做同一件事」不是圖靈完備。批次含真正的 dry-run（不碰任何檔案）與預設寫新檔的碰撞防護；目前只涵蓋旋轉、色彩轉換、條碼三種動作 |
