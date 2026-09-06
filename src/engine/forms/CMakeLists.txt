# 表單子系統（WBS 6.1–6.4，PRD-FORM-001 ~ 005）。
#
# 獨立成 target 的理由與 alioth_text 相同：它持有自己的專用執行緒與獨立的
# 文件把手，是 PDFium 非執行緒安全前提下唯一合法的並行方式。
add_library(alioth_forms STATIC
    form_types.cpp
    form_types.h
    form_data.cpp
    form_data.h
    form_environment.cpp
    form_environment.h
    form_document.cpp
    form_document.h
)

target_include_directories(alioth_forms PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(alioth_forms
    PUBLIC Alioth::domain
    PRIVATE pdfium Alioth::pdfium_runtime Alioth::engine
)

add_library(Alioth::forms ALIAS alioth_forms)
