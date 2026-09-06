# 塗黑與中繼資料清除（WBS 11，PRD-ANN-032 / 033 / 034）。
#
# 這個 target 有兩條輸入完全不同的路徑：
#
#   標記與套用走 Alioth::objects 的物件層，不碰 PDFium。這讓「哪些內容被刪掉」
#   的判斷可以在純位元組層驗到底——那是本工作包唯一不能出錯的地方，
#   而繞一趟 PDFium 只會讓失敗原因多一個嫌疑犯。
#
#   Find and Redact 走 Alioth::text（PDFium 的文字層）。它與套用階段自行重放
#   內容串流所算出來的幾何是兩條獨立推導，刻意不共用：共用的話推導錯了
#   兩邊會一起錯，測試也就驗不出來。
add_library(alioth_redaction STATIC
    pdf_document_rewriter.cpp
    pdf_document_rewriter.h
    content_font_metrics.cpp
    content_font_metrics.h
    content_redactor.cpp
    content_redactor.h
    redaction_marks.cpp
    redaction_marks.h
    redaction_applier.cpp
    redaction_applier.h
    sanitizer.cpp
    sanitizer.h
    find_and_redact.cpp
    find_and_redact.h
)

target_link_libraries(alioth_redaction
    PUBLIC Alioth::domain Alioth::objects Alioth::text
)

add_library(Alioth::redaction ALIAS alioth_redaction)
