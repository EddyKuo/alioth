# 頁面管理子系統（WBS 5.6–5.8）。
#
# 存檔一律轉給 Alioth::save，所以這裡 PUBLIC 連結它：page_editor.h 的
# PageSaveResult 直接暴露 save::SaveResult，呼叫端必須看得到那個型別
# （尤其是 fullRewriteFallback——結構性操作退回整份重寫時的唯一告知管道）。
add_library(alioth_pages STATIC
    page_editor.cpp
    page_editor.h
)

target_link_libraries(alioth_pages
    PUBLIC Alioth::domain Alioth::save
    PRIVATE pdfium Alioth::pdfium_runtime Qt6::Core
)

add_library(Alioth::pages ALIAS alioth_pages)
