# 表單建立與受限運算式引擎（WBS 10，PRD-FORM-010 ~ 022）。
#
# 刻意**不連結 PDFium**：PDFium 沒有建立表單欄位的公開 API，這個 target 存在的
# 理由正是走 ADR-002 的物件層通道自己寫字典。輸入是規格與位元組、輸出是位元組，
# 因此整個工作包可以在不開啟任何 PDF 的情況下逐位元組測到底。
#
# 驗證「寫出來的檔案 PDFium 讀不讀得到」是測試的責任，由 tests/formbuild
# 連結 Alioth::forms 完成——那是一條完全獨立的路徑，用我們自己的剖析器
# 驗自己寫的位元組等於沒驗。
add_library(alioth_formbuild STATIC
    expression.cpp
    expression.h
    form_calculation.cpp
    form_calculation.h
    field_definition.cpp
    field_definition.h
    field_appearance.cpp
    field_appearance.h
    field_tree.cpp
    field_tree.h
    form_field_writer.cpp
    form_field_writer.h
)

target_include_directories(alioth_formbuild PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(alioth_formbuild
    # CJK 內嵌字型（ADR-007）。
    PUBLIC Alioth::fonts
    PUBLIC Alioth::domain Alioth::objects
    PRIVATE Alioth::annotations
)

add_library(Alioth::formbuild ALIAS alioth_formbuild)
