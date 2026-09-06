# 註解子系統（WBS 4.1–4.3）。
#
# appearance_stream 是純函數層，只依賴領域層；annotation_writer 與
# annotation_document 才碰 PDFium。兩者放在同一個 target 是為了讓上層只需要
# 連一個庫，分層由 include 方向與 CLAUDE.md 的規則維持，不由 target 邊界維持。
add_library(alioth_annotations STATIC
    appearance_stream.cpp
    appearance_stream.h
    text_layout.cpp
    text_layout.h
    annotation_writer.cpp
    annotation_writer.h
    annotation_document.cpp
    annotation_document.h
)

target_link_libraries(alioth_annotations
    # CJK 內嵌字型（ADR-007）：量測與內嵌必須用同一份字型。
    PUBLIC Alioth::fonts
    PUBLIC Alioth::domain
    PRIVATE pdfium Alioth::pdfium_runtime
)

add_library(Alioth::annotations ALIAS alioth_annotations)
