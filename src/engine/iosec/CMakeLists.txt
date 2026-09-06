# 檔案輸出入（WP27，PRD-IO-007）：匯出頁面為點陣影像、匯出純文字。
#
# 依 image_codec / page_rasterizer 的既有分工原則：需要 Qt 影像外掛
# （PNG/JPEG/TIFF 檔案寫出，而非 PDF 內嵌串流）的部分放這裡，不進 alioth_enhance——
# 那個 target 的相依理由是「PDF 內嵌影像串流編解碼」，混進「獨立影像檔匯出」
# 會讓兩種完全不同的輸出目的共用同一份 include 表面。
add_library(alioth_iosec STATIC
    page_image_export.cpp
    page_image_export.h
    text_export.cpp
    text_export.h
)

target_include_directories(alioth_iosec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Alioth::text 是 PUBLIC：text_export.h 的函式簽章直接用到
# engine::text::TextExtractor，包含這個標頭的下游也需要看得到它的定義。
target_link_libraries(alioth_iosec
    PUBLIC Alioth::domain Alioth::text
    PRIVATE Qt6::Gui Qt6::Core Alioth::enhance Alioth::engine
)

add_library(Alioth::iosec ALIAS alioth_iosec)
