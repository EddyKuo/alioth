# 從其他來源建立 PDF（WBS 15；PRD-IO-009 / IO-010 / IO-012）。
#
# 刻意不連結 PDFium：這一層是「產生位元組」而不是「讀取文件」，讓 PDFium
# 參與只會多一次重新序列化，位元組層級的測試就失去意義。產出能不能被開啟，
# 由測試那一側用 PdfiumEngine 驗證，那才是獨立的第二意見。
#
# 也不連結 Alioth::bookmarks：Markdown 的標題只輸出「書籤意圖」資料
# （domain::create::BookmarkIntent），要不要寫成 /Outlines 由呼叫端決定。
# 相依過去會把整個大綱子系統拖進來，而四種來源裡只有一種用得到。
#
# 連 Qt6::Core 只為了 zlib（qCompress）：影像與內容串流一律 FlateDecode，
# 而專案的引擎級相依上限是三個元件，不能為了壓縮再加一個。
add_library(alioth_create STATIC
    pdf_document_builder.cpp
    pdf_document_builder.h
    image_to_pdf.cpp
    image_to_pdf.h
    text_to_pdf.cpp
    text_to_pdf.h
    markdown_to_pdf.cpp
    markdown_to_pdf.h
    url_document_source.cpp
    url_document_source.h
    csv_to_pdf.cpp
    csv_to_pdf.h
    web_page_to_pdf.cpp
    web_page_to_pdf.h
)

target_link_libraries(alioth_create
    PUBLIC Alioth::domain Alioth::objects
    PRIVATE Qt6::Core
)

add_library(Alioth::create ALIAS alioth_create)
