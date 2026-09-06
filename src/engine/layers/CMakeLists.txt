# 圖層（OCG）面板讀取與攤平（PRD-VIEW-008、PRD-VIEW-014，WBS 2.11，ADR-003）。
#
# 刻意不直接連結 PDFium：見 ocg_reader.h 開頭的說明。這裡走的是 ADR-002 那條
# 物件層讀取通道（Alioth::bookmarks 已經把 Alioth::objects 與文字字串解碼
# 一起帶出來），輸入是檔案位元組，輸出是純 C++ 的 domain::OcgTree。
#
# ocg_flatten 額外借用 Alioth::redaction 的 PdfDocumentRewriter：那是一個
# 通用的「可達性標記—清除」全檔重寫器，不是塗黑專屬的邏輯，攤平圖層同樣
# 需要「整份重寫、輸出到新檔案」（ADR-003：真正可切換的版本必須靠這個而不是
# PDFium 的執行期可見性），重寫一份幾乎一樣的程式碼風險更高。這個依賴只出現
# 在 .cpp 裡（PRIVATE），ocg_flatten.h 的公開介面只看得到 domain 型別。
#
# 代價要說清楚：Alioth::redaction 為了 Find and Redact 而公開連到 Alioth::text，
# 而 Alioth::text 連結 PDFium（PRIVATE）。這條依賴鏈讓 alioth_layers 在建置圖上
# 間接需要 PDFium 才能連結成功，但 ocg_flatten.cpp 本身沒有任何一行呼叫 PDFium
# API——SDD §2.0 子系統表把 Alioth::layers 標成「不連 PDFium」指的是「不直接
# 呼叫 PDFium API」，這條間接連結沒有違反那個判準，但 docs/SDD.md 的子系統表
# 目前沒有註記這個間接依賴，SA 應該在下次修訂時補上（見 WP37 完成回報）。
add_library(alioth_layers STATIC
    ocg_reader.cpp
    ocg_reader.h
    ocg_flatten.cpp
    ocg_flatten.h
)

target_include_directories(alioth_layers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(alioth_layers
    PUBLIC Alioth::domain Alioth::bookmarks
    PRIVATE Alioth::platform Alioth::objects Alioth::redaction
)

add_library(Alioth::layers ALIAS alioth_layers)
