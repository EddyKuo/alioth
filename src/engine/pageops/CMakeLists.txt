# 頁面進階操作（WBS 12，PRD-PAGE-007 ~ 011）。
#
# 刻意不連 PDFium：這五項全都是物件層的手術（頁面樹、Form XObject、座標系平移），
# 而 PDFium 的公開 API 一項都做不到。走物件層的額外好處是產出可以逐位元組驗證，
# 不必先相信 PDFium 讀回來的東西就是我們寫進去的東西。
#
# 連 Alioth::redaction 是為了 PdfDocumentRewriter：全檔重寫器已經處理過
# 可達性標記清除、xref 子區段、trailer 鍵過濾這些會讓 qpdf 發警告的細節，
# 再寫第二份只會有第二套 bug。
add_library(alioth_pageops STATIC
    compose_document.cpp
    compose_document.h
    object_copier.cpp
    object_copier.h
    page_form.cpp
    page_form.h
    annotation_transform.cpp
    annotation_transform.h
    page_merge.cpp
    page_merge.h
    page_overlay.cpp
    page_overlay.h
    page_boxes.cpp
    page_boxes.h
)

target_link_libraries(alioth_pageops
    PUBLIC Alioth::domain Alioth::objects Alioth::redaction
)

add_library(Alioth::pageops ALIAS alioth_pageops)
