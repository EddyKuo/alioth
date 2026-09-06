# 頁面標籤（PRD-PAGE-013）。
#
# 不連結 PDFium：FPDF_GetPageLabel 只給得出標籤字串，範圍的樣式、前綴與起始編號
# 一個都拿不到，而那些正是編輯對話框需要的；寫入端 PDFium 更沒有 API。
# 因此輸入是位元組、輸出是位元組，走 ADR-002 的物件層通道。
add_library(alioth_labels STATIC
    page_label_codec.cpp
    page_label_codec.h
)

target_include_directories(alioth_labels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(alioth_labels
    PUBLIC Alioth::domain Alioth::objects
)

add_library(Alioth::labels ALIAS alioth_labels)
