# TrueType 子集化（ADR-007）。
#
# 不連結 PDFium：子集化是純位元組層的表格重建，與渲染引擎無關。
# 因此它也不受「只有引擎轉接層可以碰 PDFium」那條規則牽制。
add_library(alioth_fonts STATIC
    truetype_subset.cpp
    truetype_subset.h
    cid_font_writer.cpp
    cid_font_writer.h
    cjk_font_library.cpp
    cjk_font_library.h
    text_runs.cpp
    text_runs.h
)

target_include_directories(alioth_fonts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
# Alioth::objects 是 PUBLIC：標頭直接用 IncrementalAppender 當參數型別。
# 它不連結 pdfium，因此本 target 仍然與渲染引擎無關。
target_link_libraries(alioth_fonts PUBLIC Alioth::domain Alioth::objects
    # 字型檔的位置是作業系統差異，收斂在平台層（CLAUDE.md）。
    PRIVATE Alioth::platform Qt6::Core)

add_library(Alioth::fonts ALIAS alioth_fonts)
