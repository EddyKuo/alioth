# qpdf — PDF 結構檢查工具

PRD-IO-001 與 PRD §9 都把「產出的 PDF 通過 qpdf 結構檢查」列為驗收條件。
本目錄放的是為了執行這道檢查而取得的 qpdf 執行檔。

目錄內容已 gitignore（與 `third_party/pdfium/` 同樣的處理），取得方式：

```
tools\qpdf\fetch_qpdf.bat
```

取得後需要重新 configure（`build_agent.bat <name>`），CMake 才會把路徑編進測試。

## 為什麼用官方 Windows 發行版，而不是 vcpkg port

兩條路都試得通，選官方發行版的理由：

1. **qpdf 對本專案是工具而不是相依。** 測試是用 `QProcess` 執行 `qpdf --check`，
   不連結 `libqpdf`。走 vcpkg 會為了一個只在測試中被 shell out 的工具，
   把 zlib、jpeg、gnutls 等傳遞相依拉進專案的相依樹裡。
   PRD §4.1 的立場是「引擎級元件只有三個」，多一條相依線就多一次要解釋的例外。

2. **建置時間。** vcpkg 的 qpdf port 連同相依要編十幾分鐘，而且會綁定 vcpkg 的
   工具鏈設定。官方發行版是 9 MB 的 zip，數秒完成。CI 上這個差距每次都要付。

3. **版本可釘死且與我們的編譯器無關。** 官方 msvc64 發行版自帶 VC++ runtime DLL，
   不受本機 MSVC 版本影響。檢查結果因此只取決於 qpdf 版本，
   而不取決於誰的機器上編的——這對「零警告」這種嚴格判定很重要。

代價：升版需要手動改 `fetch_qpdf.bat` 的預設版本號，vcpkg 則可由 manifest 管理。
以一個測試期工具而言，這個代價可以接受。若日後需要在程式內直接呼叫 libqpdf
（例如為了解決 `/AP /Resources` 的物件層輸出問題），這個決定要重新評估，
屆時應建立 ADR。

## 目前使用的版本

qpdf 12.4.1（`qpdf-12.4.1-msvc64.zip`），只取用 `bin/` 目錄。

## 判定標準

`qpdf --check` 的退出碼：0 = 乾淨、2 = 有錯誤、3 = 只有警告。
測試要求**零警告**，不只是零錯誤。qpdf 的典型警告是
「物件 N 的 /Length 不對，已自行修正」——檢視器會幫忙修正，
但簽章驗證不會，而那正是本產品的核心賣點所在。

## 授權

qpdf 採 Apache License 2.0。本專案未連結 libqpdf、也不散布 qpdf，
它只是開發與 CI 階段的測試工具，不進入安裝包（PRD §8.1 的 130 MB 上限不受影響）。
