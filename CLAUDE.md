# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 專案現況

Alioth 是一款跨平台專業 PDF 審閱工作站（對標 PDF-XChange Editor Plus V11）。

需求真相來源是 `PRD_跨平台專業PDF審閱工作站_v2.1.md`（961 行、217 條範圍內需求、
3,750 PD / 21 個月）。任何實作決策若與 PRD 衝突以 PRD 為準；PRD 未涵蓋的，建立 ADR
於 `.decisions/` 而不是自行補全。

程式碼進度：**進度看 `docs/TRACEABILITY.md`，不要從程式碼或測試數量推測——那會高估。**
那份矩陣由 `python tools/traceability.py` 從 PRD 產生，狀態欄是人工維護的，
備註欄寫的是「做到什麼程度、哪裡沒做、為什麼」。

`build.bat` 建置（第二個參數可指定單一 target）、`test.bat` 跑測試、`ci.bat` 跑完整關卡
（建置 → 測試 → 效能回歸 → 勾稽矩陣 → 分層檢查）。

已完成的骨幹：五層架構與所有引擎子系統（見「架構」一節的 target 表）、Ribbon、
十二個停靠面板、註解全家族與自產外觀串流、表單建立與填寫、增量儲存、簽章驗證與建立、
頁面管理、列印、比對、塗黑、掃描增強、批次巨集。

**未完成的性質已經變了**：現在剩下的不是「還沒開始寫」，而是三類——
需要商務決策的（雲端原生 API、OCR 選配模組）、預編譯 PDFium 做不到的
（見 `.decisions/ADR_003` 與 `exceptions/` 的實測報告）、以及需要實機驗證的
（Acrobat 相容性、螢幕閱讀器、TSA 伺服器）。動手前先讀 `exceptions/` 與 `.decisions/`，
那裡記的是「為什麼沒做」而不是「忘了做」。

## 已定案的技術決策

PRD 第 12 章列了 10 個開放問題，其中四項已於 init 階段定案：

| 項目 | 決定 | 影響 |
|---|---|---|
| PDFium 取得方式 | 預編譯 `bblanchon/pdfium-binaries`（已關閉 V8/XFA） | 不維護 GN 建置機；升版受上游節奏限制，由引擎轉接層隔離 |
| Qt | 6.8 LTS，LGPL v3 **動態連結** | 不可靜態連結 Qt；必須讓使用者能替換 Qt 動態庫 |
| Ribbon | **自研**（QToolBar / QTabBar 基礎上實作），不引入 SARibbon 或商業元件 | 無額外第三方相依；外觀完全自控 |
| 平台順序 | **先做純 Windows**，之後再移植 macOS / Linux | 與 PRD「三平台行為一致」承諾有張力，見下方警告 |

> **平台決策的代價**：PRD 的核心賣點之一是 Linux 原生支援與三平台一致。純 Windows 優先會累積平台相依假設（路徑、字型、DPI、列印、簽章信任存放區）。移植成本要能控制，就必須從第一天起把作業系統差異**全部收斂在平台層**，其他四層不得出現 `#ifdef _WIN32` 或 Win32 API。這是本專案最容易被違反、也最貴的一條規則。

固定不變的技術堆疊（PRD §4.1，不得新增引擎級元件）：Qt 6 Widgets + PDFium + OpenSSL 3.x，三件而已。安裝包上限 130 MB。

## 建置

需要 MSVC 環境，所以一律透過批次腳本進入 vcvars64，不要直接呼叫 cmake：

```
build.bat                      # 建置 windows-x64-debug
build.bat windows-x64-release  # 建置 release
test.bat                       # 跑全部測試
test.bat windows-x64-debug -R test_tile_cache   # 跑單一測試
```

多個 agent 並行工作時，各自用隔離的建置目錄，避免同時寫同一個 ninja 目錄：

```
build_agent.bat <name>   # 建到 out/build/agent-<name>
test_agent.bat <name>
```

每個建置目錄約 1 GB。工作包完成後記得刪掉對應的 `out/build/agent-*`，
否則磁碟會被塞滿，而症狀是 CMake configure 或連結莫名失敗，看起來完全不像空間問題。

環境（已驗證可建置）：CMake 4.3 + Ninja + MSVC 14.44（VS2022 BuildTools，位於
`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`）+ Qt 6.8.1 msvc2022_64
（`C:\Qt\6.8.1\msvc2022_64`）+ C++20。

PDFium 是預編譯的 `bblanchon/pdfium-binaries` chromium/8035，解壓在 `third_party/pdfium/`
（已 gitignore，重新取得見下方）。它的 `args.gn` 確認了 `pdf_enable_v8 = false` 與
`pdf_enable_xfa = false`，符合 PRD §8.2 的安全立場。PDFium 是動態庫，建置後
`pdfium.dll` 會自動複製到執行檔旁邊。

CJK 內嵌字型是思源黑體（Noto Sans TC，SIL OFL 1.1），解壓在 `third_party/fonts/`
（已 gitignore，`OFL.txt` 保留在版控裡——OFL 要求散布時附帶授權條款）。建置會把它
複製到執行檔旁邊的 `fonts/`，與 `pdfium.dll` 同一個作法。**檔案不存在時只警告不失敗**：
程式會退回系統字型，功能正常，但那是降級狀態——產出的 PDF 在沒有該字型的機器上會缺字。

重新取得字型：
```
mkdir -p third_party/fonts
curl -sSL -o third_party/fonts/NotoSansTC-Regular.ttf \
  "https://github.com/google/fonts/raw/main/ofl/notosanstc/NotoSansTC%5Bwght%5D.ttf"
curl -sSL -o third_party/fonts/OFL.txt \
  "https://github.com/google/fonts/raw/main/ofl/notosanstc/OFL.txt"
```
那是可變字型；子集器取它的預設實例（Regular），丟掉變化表——保留變化表會產生
多數檢視器不支援的內嵌字型。**不要換成 Noto CJK 官方發行的 OTF**：那是 CFF 輪廓，
子集器只支援 glyf，會明確拒絕。

重新取得 PDFium：
```
curl -sSL -o pdfium.tgz https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/8035/pdfium-win-x64.tgz
mkdir -p third_party/pdfium && tar -xzf pdfium.tgz -C third_party/pdfium
```

### 建置與測試的坑

- **測試必須是 console 子系統**。`qt_add_executable` 預設走 WIN32 子系統，QTest 的輸出會被
  整個丟掉，失敗的測試在 CI 上看起來像「安靜地通過」。tests/CMakeLists.txt 已強制
  `WIN32_EXECUTABLE OFF`。
- **測試執行檔需要 Qt 與 PDFium 的 DLL 在 PATH 上**，否則退出碼是 `0xc0000135`。那個碼看起來
  像崩潰，實際只是找不到 DLL，很容易誤診成程式有問題。`alioth_add_test` 已把兩個 bin 目錄
  注入測試環境。
- **測試環境變數要用 `ENVIRONMENT_MODIFICATION` 而非 `ENVIRONMENT`**。後者的值是分號分隔的
  清單，而 Windows 的 PATH 本身含分號，整段塞進去會被切成一堆無效項目，系統 PATH 被靜默清空。
  症狀不會馬上出現——測試照樣通過，直到某天需要某個系統 DLL 才爆。
- **UI 測試需要 `QT_PLUGIN_PATH`**。windeployqt 只複製主程式用到的平台外掛，offscreen 不在其中，
  缺了它的症狀是 "no Qt platform plugin could be initialized" 加上一個看起來像記憶體錯誤的
  SegFault。
- **產生 C++ 原始碼時小心跳脫字元被多解讀一層**。用 Python 腳本或 heredoc 寫出含
  `
` 的字串常數時，`
` 很容易變成真正的換行，落到檔案裡就是
  `error C2001: 常數中包含新行字元`。這個坑在本專案已經出現過八次以上，而且**每一次都是因為圖方便用了 Python 或 heredoc**。
  規則沒有例外：只要要寫的 C++ 字串裡有反斜線，就用 Write/Edit 工具，
  不要經過任何會再解讀一次的中介。
- **Qt 型別的前置宣告要放在 namespace 外**。寫在 `namespace alioth::ui {` 裡面會宣告出
  `alioth::ui::QLabel` 這種不存在的型別，錯誤訊息是「使用未定義類型」，看起來像缺 include。
  這個坑已經踩過兩次。
- **`QMainWindow::setMenuWidget()` 會刪掉原本的選單列，連帶殺掉它擁有的所有 QAction**。
  症狀是工具列按鈕變成沒有動作的空殼（滑鼠鍵盤都沒反應）、切回傳統選單只剩一個項目，
  而且完全沒有錯誤訊息。本專案曾因此讓 46 個動作在啟動當下就被銷毀，是無障礙稽核
  掃出「五個工具列按鈕鍵盤不可達」才追到的。Ribbon 現在放在中央版面的最上方，
  選單列與它各自獨立顯示或隱藏（見 `MainWindow` 建構式）。
- **多個代理平行建置時會產生陳舊目標檔**。ninja 以 mtime 判斷是否重編；別人在你建置的
  同時寫入原始碼，ninja 可能記下新的 mtime 卻編到舊內容，之後就再也不會重編。症狀是
  「程式碼看起來完全正確，測試卻失敗」，而單獨重建那個 target 就好了。所有代理回報完成後，
  先把 `src/` 與 `tests/` 的檔案時間戳全部更新再完整重建一次，再相信測試結果。
- **結構化繫結不能出現在被 `Q_OBJECT` 測試檔引入的標頭裡**。moc 會靜靜地產不出
  `metaObject`/`qt_metacast`，症狀是連結期缺符號而不是編譯錯誤。改用 `.first`/`.second`。
- **持有工作執行緒的物件，不可以讓它的最後一個參照握在「投遞出去的函式物件」手上**。
  那個函式物件被銷毀的時機與執行緒取決於交錯順序，而解構子裡的 `thread.join()`
  在自己那條執行緒上就是自我 join，結果是 `std::terminate`。要在確定的執行緒上
  明確 `reset()`，不要靠 shared_ptr 的自然生命週期。這個缺陷在 `app/csv_form_fill.cpp`
  上表現為 `ctest -j` 下大約每九十次崩潰一次，而且**不是斷言失敗**——測試輸出
  在中途整個斷掉，沒有 FAIL 也沒有 Totals，很容易被當成環境問題。
- **平行執行才失敗的測試要當成缺陷處理**。`ctest -j` 下逾時而單獨跑會過，多半是等待預算
  抓得太緊；而逾時後仍持有堆疊參照的回呼會寫進已死的記憶體，症狀出現在**後面某個
  不相干的測試**上。等待狀態放 `shared_ptr`，預算放寬到 ctest 自己的 TIMEOUT 之下。
- **C++ 的 `\x` 跳脫是貪婪的**：`"\x8CAlioth"` 會被讀成一個超出範圍的 `\x8CA`，
  不是 `\x8C` 後面接 `Alioth`。含十六進位跳脫的字面值要斷成兩段相鄰字串。
- **`Q_ASSERT` 裡不可以放有副作用的呼叫**。release 建置定義 NDEBUG，整個運算式連同副作用
  一起被編掉。`Q_ASSERT(writeBytesTo(path, bytes))` 在 debug 通過、在 release 檔案根本沒產生，
  而症狀是遙遠的「找不到檔案」。先把結果存進變數再斷言。CI 跑 release，debug 全綠不代表沒事。
- **QTest 的失敗明細在 ctest 下看不到**。`--output-on-failure` 只給得出「Failed」與耗時，
  失敗的斷言、實得值、期望值全部不見（直接跑執行檔也一樣）。要看原因必須用
  `-o <檔案>,txt` 把報告寫出來，並自行把 Qt 的 bin 目錄放進 PATH、設好 `QT_PLUGIN_PATH`；
  Qt 裝在 `C:/Qt/6.8.1/msvc2022_64`（見 CMakePresets.json 的 `CMAKE_PREFIX_PATH`）。
  退出碼仍然正確，所以紅綠判定沒問題，只是失敗時看不到理由。
- **多個代理平行改樹時，configure 失敗常常是別人的暫態**：CMakeLists 先列了檔案、檔案還沒寫出來。
  症狀是 `Cannot find source file: xxx.cpp` 或某個剛加的標頭找不到。不要去「修好」別人的檔案，
  等它把檔案寫出來再建置即可。已經發生過四次（tests/create、src/domain/presentation.h、
  app/document_controller.h、tests/annfamily）。
- **版面問題只能用看的**。`alioth_uishot <輸出.png>` 會以 offscreen 建構主視窗、截圖，
  並印出所有停靠面板與工具列的可見狀態與幾何。純讀程式碼判斷不出「Ribbon 被舊工具列
  擠到視窗中段」「面板收起來了但別的東西佔住那塊」——那些在程式碼上都完全正常。
  **手動跑它要自己設 `QT_PLUGIN_PATH`**（`C:/Qt/6.8.1/msvc2022_64/plugins`）：uishot 不是
  ctest 目標，沒有 `alioth_add_test` 幫忙注入環境，而 windeployqt 只複製了 `qwindows.dll`，
  offscreen 外掛不在執行檔旁邊。少了它會跳「找不到 Qt platform plugin」的對話框，
  看起來像產品壞了，其實只是這一次呼叫的環境不對。
- **跑 uishot 或任何會建構 MainWindow 的東西之前，先清掉 QSettings**
  （Windows 在登錄檔 `HKCU\Software\Alioth`，不是檔案）。主視窗會還原上次存下的版面，
  忘了清就會拿上一次執行的殘留來判斷「預設版面」，而且它看起來完全像是修改沒生效。
  這個坑已經誤導過一次，浪費了兩輪建置。
- **Ribbon 必須放在頂部工具列區，不能放進中央版面**。QMainWindow 只有工具列區
  會橫跨整個視窗並壓在停靠區之上；放在中央版面時它只能佔「停靠面板之間剩下的那段」，
  左側面板一開，Ribbon 就從視窗中段才開始。另外要 `addToolBarBreak` 讓它獨佔一列，
  否則會和既有工具列擠在同一列。（但仍然不可以用 `setMenuWidget()`，理由見上。）
- **`/Rotate` 對文字擷取的影響是「順序變、座標不變」**。PDFium 依顯示後的方向決定
  閱讀順序（`/Rotate 90` 會讓原本在上方的那一行排到後面），但字元框座標仍在未旋轉的
  頁面使用者空間。兩者同時成立，看起來矛盾但都是對的：選取錨點用座標，複製出來的
  文字用順序。跨旋轉頁比對字元時要用**內容**定位（例如找出某個字串的索引），
  不能用索引 0——那在兩份文件裡是不同的字。已釘在 tests/text/test_text_extractor.cpp。
- **不要用 `python3` 呼叫 Python**。這台機器上的 `python3` 是 Windows Store 的轉接殼，
  執行後什麼都不做、也不印任何東西就退出——包含 heredoc 裡的 `assert` 與 `print`。
  症狀是「腳本看起來成功了，檔案卻沒變」，本 session 已誤診多次並造成改動遺失。
  要用就用 `python`（3.14）。改 C++ 一律用 Write/Edit 工具，不經任何中介。
- `.bat` 檔一律只用 ASCII：主控台 codepage 會把 UTF-8 註解變成亂碼並導致語法錯誤。

## 架構

五層分層（PRD §4.5）。層與層之間的方向是單向的，跨層呼叫是設計錯誤：

```
呈現層    Qt 主視窗 / Ribbon / 頁籤 / 檢視元件 / 12 個停靠面板   ← 絕不直接呼叫 PDFium
應用層    文件、可視區、註解、選取控制器；命令匯流排 + 復原堆疊
領域層    Document / Page / Annotation / TextLayer / Outline / FormField（純 C++，與 PDFium 無關）
引擎轉接層 PdfiumEngine、圖磚渲染器、快取、文字擷取、AP 產生器、增量儲存器、簽章驗證器
平台層    檔案 I/O、列印、剪貼簿、設定、當機回報、自動更新
```

- **引擎轉接層是唯一可以呼叫 PDFium 與 OpenSSL 的地方。** 這條規則同時服務三個目的：PDFium 每季升版時的隔離、領域層可單元測試（覆蓋率門檻 80%）、以及未來從預編譯切換到自建的可能性。
- **領域層不得依賴 Qt。** 讓核心模型能在無 GUI 環境下跑測試。
- **平台層是作業系統差異的唯一收斂點。**
- 引擎層拆成數個 target：`alioth_engine`（渲染與文件生命週期）、`alioth_text`（文字與搜尋）、
  `alioth_annotations`（外觀串流與註解寫入）、`alioth_save`（增量儲存與自動儲存），
  `alioth_pages`（頁面管理），加上共用的 `alioth_pdfium_runtime`（PDFium 全域初始化的唯一入口）。
  列印（`alioth_print`）與 Ribbon（`alioth_ribbon`）各自獨立，前者自持一個引擎實體，
  因為列印是大量長時間渲染，塞進檢視器的佇列會讓畫面整段卡住。
  拆開的理由是它們各自持有獨立的 PDFium 文件把手與執行緒——那是 PDFium 非執行緒安全的
  唯一合法並行方式。
- **開檔一律走 `FPDF_LoadCustomDocument`**，不用 `FPDF_LoadDocument`。後者會整份讀進記憶體，
  而且它持有的檔案控制代碼會讓存檔時的原子更名失敗——「檢視器開著文件時存檔」是存檔的
  唯一情境，所以這不是最佳化而是必要條件。檔案來源見 `platform/shared_file.h`
  （關鍵在 `FILE_SHARE_DELETE`）。

### 四個決定架構形狀的硬性限制

這四點不是最佳實踐建議，是違反了就要重寫的約束：

1. **PDFium 可並存、不可並行（ADR-005，實測結論）。** 多份文件把手同時存在沒有問題——
   四個擷取器同時活著、嚴格輪流動作時每個都 10/10。但**任何時刻只能有一條執行緒在
   PDFium 裡**：兩條執行緒同時動作會非決定性地掉資料（同一組設定第一輪 60/60、
   第二輪 5/60），之後整個行程的開檔開始失敗。壞掉的方式是安靜地少給資料，不是崩潰，
   所以「跑起來沒事」不是證據。
   因此：同一份文件只能有一個專用 PDFium 執行緒，配優先權工作佇列（可見圖磚 > 預取 >
   縮圖）與取消權杖；禁止用執行緒池平行渲染。**需要加速時要改設計，不能開更多執行緒**
   （全文搜尋因此改走單一擷取器預建索引）。恆真的兩條釘在 `tests/test_pdfium_concurrency.cpp`。

   **每一個 PDFium 呼叫點都必須在 `engine/pdfium_lock.h` 的 `PdfiumGuard` 底下。**
   行程一把遞迴鎖，粒度是「一件工作」（一張圖磚、一頁文字層、一次存檔），
   靠的是每個子系統的工作單元本來就有界——列印也走圖磚。CI 的分層檢查會擋下
   「呼叫 FPDF_* 卻沒取鎖」的新檔案，要免除得寫進 `PDFIUM_GUARD_EXEMPT` 並說明理由。
   驗證方式是**把鎖拿掉再跑**：拿掉之後並行測試 15 次失敗 2 次，裝上去 25 次全過。

2. **嚴禁整頁光柵化。** 有兩個明確的例外——縮圖與「裁切至白邊」的內容邊界偵測，
   兩者都是離線、一次性、低解析度的用途，且都在程式碼裡標明了理由。
   除此之外一律切 512×512 圖磚，只渲染與可視區相交者加外圍一圈預取，走 `FPDF_RenderPageBitmapWithMatrix` 傳裁切區。500 頁 A0 工程圖要在 2 秒內見到首頁、閒置記憶體 ≤ 512 MB，整頁光柵化做不到。縮放中可暫時拉伸舊圖磚，但**拉伸結果不得成為最終畫面**，停止 80 毫秒後必須以新倍率重算。

3. **零複製渲染的 stride 陷阱。** `FPDFBitmap_CreateEx` 直接寫入 `QImage::Format_ARGB32_Premultiplied` 的緩衝區時，stride 必須傳 `qimage.bytesPerLine()`，**不可假設為 寬×4**。Qt 會做 4 位元組對齊，假設寬×4 在多數寬度下會產生斜切畫面。

4. **註解必須自產外觀串流（/AP）。** PDFium 對多數註解類型不會自動產生 `/AP`；缺了它，Acrobat 會自行補畫但 macOS 預覽與 Chrome 可能顯示不一致甚至不顯示。所有註解都要輸出符合 ISO 32000-2 §12.5.5 的 `/AP /N` Form XObject，並寫齊 `/C /CA /BS /IC /QuadPoints /InkList /Rect /T /M /CreationDate /NM /F /Popup /IRT`。這是全案最高風險節點（PRD WBS 4.2），Acrobat 是最終裁決者。

### 其他不可退讓的行為

- **註解不動內容串流。** 一切標記以獨立物件寫入 `/Annots`。這是「不破壞原檔與簽章」賣點的技術基礎。
- **存檔預設走增量儲存**（`FPDF_SaveWithVersion` 加 `FPDF_INCREMENTAL`），並採「寫暫存 → 落盤同步 → 原子更名」。100 MB 文件加一個註解要 ≤ 300 毫秒、增量 ≤ 20 KB、既有簽章在 Acrobat 顯示為「有效，簽章後有變更」而非「無效」。
- **所有文件修改包成命令物件**走命令匯流排（`app/command_stack.h`），統一復原重做（≥ 100 步）
  與未存檔狀態。復原不需要整份快照：增量儲存是純附加，把檔案截回原長度就是精確的反操作，
  但截之前必須比對邊界守衛，確認這段期間沒有別人改過檔案。
- **PDF 視為不可信任輸入。** 不執行任何 PDF 內嵌 JavaScript；表單計算走自建受限運算式引擎（四則、SUM/AVG、欄位參照、簡單條件），不引入 JS 引擎。開啟網址前需確認；一律禁止 Launch Action。
- 主執行緒單次阻塞 ≤ 16 毫秒，任務取消延遲 ≤ 16 毫秒。

## 範圍邊界

本產品叫「審閱工作站」不叫「編輯器」，是刻意的定位決定。PRD §2.1 明確排除 38 條功能，**不要因為看起來「順手就能加」而實作它們**：

| 排除項 | 原因 |
|---|---|
| 內文文字 / 影像 / 路徑編輯 | PDFium 無文字重排能力，需自建排版層 + HarfBuzz，估 320 PD |
| OCR | 需 Tesseract，安裝包 +200 MB。選配模組於 M3 依 Beta 回饋決策 |
| Office 格式互轉、PDF/A、PDF/X | 需 LibreOffice headless（+500 MB）或商用 SDK |
| 3D PDF | 無成熟開源方案，工程 PDF 使用率 < 1% |
| JavaScript 引擎 | 與關閉 V8 的安全立場直接衝突 |
| DocuSign / MS Purview | 需商務洽談 |

遇到含 XFA / 3D / JavaScript 的文件，走明確的降級提示，不得誤導使用者。

## 需求與驗收

- 需求編號 `PRD-<模組>-<序號>`（模組如 VIEW / ZOOM / NAV / ANN / TXT / SRCH / PAGE / BM / FORM / SEC / SIG / IO / ENH / CMP / UI / A11Y / CLD）。實作與測試都以此編號勾稽。
- 優先級 MoSCoW：M 必做 / S 應做 / C 有餘力。版本欄位 R1 / R2 / R3 決定何時做——**不要提前實作 R2、R3 的項目**。
- 217 條範圍內需求每條都要有對應的自動化測試並於 CI 通過。進度看 `docs/TRACEABILITY.md`
  （由 `python tools/traceability.py` 產生），不要從程式碼或測試數量推測——那會高估。
- 效能是驗收條件不是願望：冷啟動 ≤ 2.0 秒（P95）、翻頁 ≤ 50/250 毫秒、縮放首圖磚 ≤ 150 毫秒、捲動 ≥ 60 fps、全文搜尋 ≤ 2 秒/500 頁。效能基準每次 PR 執行，退步 > 10% 阻擋合併。
- 產出的 PDF 要通過 qpdf 結構檢查，並在 Acrobat 開啟無警告。

R1 里程碑：M0 技術驗證（三個 PoC：零複製渲染、圖磚化單執行緒佇列、外觀串流）→ M1 Alpha → M2 Beta1 → M3 Beta2 → M4 GA。**M0 Gate 未通過不進 M1。**

## PDFium API 對照

PRD 附錄 C 有完整速查表（載入、圖磚渲染、文字、註解、表單、頁面管理、存檔、書籤連結、簽章、安全、字型）。寫引擎轉接層前先查那張表，不要憑記憶叫 API。

簽章驗證的分工要記清楚：PDFium 只能列舉簽章、取出 `/ByteRange` 與 contents；PKCS#7 解析、憑證鏈、信任存放區、CRL/OCSP 吊銷查詢全部由 OpenSSL 自建，三平台共用同一份實作。

## 團隊框架

> `.claude/`、`sprint/`、`exceptions/`、`.decisions/`、`.github/` 與 PRD 本體
> 只存在於開發機，不進版控（見 `.gitignore`）。下面這一節，以及本文件各處對
> `exceptions/`、`.decisions/ADR_*`、PRD 章節的引用，在 clone 下來的樹裡都指不到東西。
> 兩個直接的代價：`ci.bat` 第 4 關（`tools/traceability.py`）讀的就是那份 PRD，
> 沒有它必失敗；`.github/workflows/ci.yml` 不在，GitHub 端也不再跑 CI。

`.claude/CLAUDE.md` 是團隊憲法（三層架構、契約系統、例外處理、品質門檻），與本文件並存：憲法規範**怎麼協作**，本文件規範**這個專案本身**。憲法本體不可直接修改，需建立 ADR 並升版。

- 憲法 Section 7（Global Tech Stack）與 7.1（project_complexity）**尚未填寫**。依 PRD 規模，本案為 `L`（Enterprise）：跨模組、多平台、21 個月。SA / DBA / RD 節點在填寫前應視為 BLOCKED。
- 角色定義於 `.claude/agents/`，技術能力於 `.claude/skills/`。C++ 相關可用 `cpp-expert`、`cpp-build`、`debugger`、`perf-profiler`、`rd/lang-cpp`。
- 契約產出走 `.claude/contracts/inbox/` → `approved/`；例外走 `.claude/exceptions/`；架構決策走 `.decisions/ADR_{N}_{SLUG}.md`。

## 待決策（阻塞後續工作）

PRD 第 12 章尚未定案、且會影響實作的項目：

- ~~Text Box 與表單的 CJK 字型內嵌授權策略（M1 前，影響 WBS 4.8）~~
  已定案，見 `.decisions/ADR_007_cjk_font_embedding.md`：內嵌思源黑體
  （Noto Sans TC）子集，SIL OFL 1.1 授權可自由散布與內嵌。需要繪製文字的路徑
  （Text Box、Callout、Typewriter、表單欄位外觀、塗黑覆蓋文字、註解摘要頁、
  頁首頁尾／浮水印／Bates）都已接上。**「畫不出來就要失敗」的原則沒有變**，
  只是條件變了：現在的界線是「可列印 ASCII 或子集裡有字形」，落在範圍外
  仍然明確失敗，不輸出只剩拉丁字的殘缺內容——靜默掉字的表單欄位會讓 `/V`
  有值但畫面空白，使用者不會發現。
  尚未接上的只剩文字／CSV 轉 PDF（`domain/document_source.h`），那屬於 R3 的
  PRD-IO-013。
- ~~圖層（OCG）切換的 PDFium 支援度需實測（M0，影響 WBS 2.11）~~
  已實測結案，見 `.decisions/ADR_003_ocg_visibility_downgrade.md`：預編譯 PDFium
  的公開 API 沒有 OCG 可見性控制，R1 圖層面板降級為唯讀，真正可切換的版本
  併入 R2 的 PRD-VIEW-014（圖層攤平）。
- ~~OCR 選配模組是否啟動（M3，+180 PD）~~
  已定案，見 ADR-007：**不做**，排除於範圍外。掃描件仍可檢視、註解、列印，
  只是不能搜尋或選取文字；安裝包維持精簡的價值高於這個功能。
- 產品正式名稱與品牌（M2 前）

碰到這些，發 BLOCKED 或建立 ADR，不要自行假設。
