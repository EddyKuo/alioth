# 書籤全套（WBS 9，PRD-BM-001 ~ 019）。
#
# 刻意不連結 PDFium：PDFium 有 FPDFBookmark_* 的**讀取** API，卻完全沒有建立、
# 刪除或搬移書籤的公開 API，連 /Dest 的縮放類型都讀不出來。這個 target 存在的
# 理由正是走 ADR-002 的物件層通道自己寫字典，因此輸入是位元組、輸出是位元組。
#
# 「寫出來的東西別人讀不讀得到」由 tests/bookmarks 連結 Alioth::engine 之後用
# PdfiumEngine::outline() 驗證——用我們自己的剖析器驗自己寫的位元組等於沒驗。
add_library(alioth_bookmarks STATIC
    destination_codec.cpp
    destination_codec.h
    outline_reader.cpp
    outline_reader.h
    outline_writer.cpp
    outline_writer.h
    toc_page_builder.cpp
    toc_page_builder.h
    page_order_writer.cpp
    page_order_writer.h
    bookmark_document.cpp
    bookmark_document.h
    annotation_sources.cpp
    annotation_sources.h
)

target_include_directories(alioth_bookmarks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(alioth_bookmarks
    PUBLIC Alioth::domain Alioth::objects
)

add_library(Alioth::bookmarks ALIAS alioth_bookmarks)
