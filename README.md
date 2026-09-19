# Alioth

跨平台專業 PDF 審閱工作站。目前 R1，僅 Windows x64。

**這份文件只是索引。** 每一項的內容都住在別的地方，這裡不重述——重述的那一份
遲早會過時，而讀的人分不出哪一份才是對的。

## 從哪裡開始讀

| 想知道 | 看哪裡 |
|---|---|
| 產品要做什麼、不做什麼 | PRD v2.1（需求的唯一真相來源，**只在開發機上**，見下方說明） |
| 做到哪裡了 | `docs/TRACEABILITY.md`（由 `python tools/traceability.py` 從 PRD 產生） |
| 為什麼這樣設計 | `docs/SDD.md` |
| 為什麼某件事沒做 | `docs/TRACEABILITY.md` 的備註欄（寫的是「做到什麼程度、哪裡沒做、為什麼」） |
| 機器測不到的驗收怎麼做 | `docs/MANUAL_VERIFICATION.md`（Acrobat 相容性、螢幕閱讀器、TSA 等） |
| 介面的設計意圖 | `docs/UX_CONCEPT.md` |
| 在這個倉庫裡工作的規矩 | `CLAUDE.md` |

進度請看勾稽矩陣，不要從程式碼行數或測試數量推測——那會高估。

**不在這個倉庫裡的東西**：PRD 本體、`.decisions/`（ADR）、`exceptions/`（例外報告）、
`.claude/`、`sprint/` 與 `.github/` 都只留在開發機（見 `.gitignore`）。因此
`docs/TRACEABILITY.md` 重新產生不了（`tools/traceability.py` 讀的就是 PRD），
`ci.bat` 的勾稽那一關必失敗，GitHub 端也不跑 CI。本文件與 `CLAUDE.md`、
`docs/` 各處對 ADR 編號與 PRD 章節的引用，在這裡都指不到東西。

## 建置與測試

需要 MSVC 環境，所以一律透過批次腳本（它們負責進 vcvars64），不要直接呼叫 cmake：

```
build.bat                                        建置 windows-x64-debug
build.bat windows-x64-release                    建置 release
test.bat                                         跑全部測試
test.bat windows-x64-debug -R test_tile_cache    跑單一測試
ci.bat                                           建置 → 測試 → 效能回歸 → 勾稽矩陣 → 分層檢查
```

`ci.bat` 是合併前的門檻。注意勾稽矩陣那一關在這個倉庫裡過不了（PRD 不在版控裡），
其餘四關照常。環境需求與 PDFium 的取得方式見 `CLAUDE.md`。

## 技術堆疊

Qt 6.8 LTS（LGPL 動態連結）＋ PDFium（預編譯，V8/XFA 已關閉）＋ OpenSSL 3.x，
就這三件。安裝包上限 130 MB，不得新增引擎級元件。

## 架構

五層，方向單向，跨層呼叫是設計錯誤（由 `python tools/check_layering.py` 把關）：

```
呈現層     Qt 主視窗 / Ribbon / 頁籤 / 檢視元件 / 停靠面板    絕不直接呼叫 PDFium
應用層     文件、可視區、註解、選取控制器；命令匯流排 + 復原堆疊
領域層     Document / Page / Annotation / TextLayer / ...    純 C++，不依賴 Qt
引擎轉接層 PdfiumEngine、圖磚渲染器、快取、文字擷取、AP 產生器、增量儲存、簽章
平台層     檔案 I/O、列印、剪貼簿、設定、當機回報、自動更新    OS 差異的唯一收斂點
```

各層的邊界為什麼畫在這裡、以及四個決定架構形狀的硬性限制（PDFium 非執行緒安全、
嚴禁整頁光柵化、零複製渲染的 stride、註解必須自產 /AP），見 `CLAUDE.md` 的「架構」一節
與 `docs/SDD.md`。

## 安全立場

PDF 一律視為不可信任輸入：不執行內嵌 JavaScript、禁止 Launch Action、開啟外部網址
前需確認且只允許 http/https、附件不自動開啟。表單計算走自建的受限運算式引擎，
不引入 JS 引擎——引入它會直接推翻關閉 V8 的理由。
