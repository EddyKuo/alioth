# 領域層：純 C++，不得依賴 Qt 或 PDFium。
add_library(alioth_domain STATIC
    document.cpp
    annotation.h
    document.h
    page_layout.cpp
    page_layout.h
    page_range.h
    quad_point.h
    text_layer.h
    geometry.h
    tile.h
    ocg.h
    guides.h
    zoom_aids.h
    presentation.h
    measurement.h
    csv.h
    ink_pressure.h
    ink_smoothing.h
    annotation_eraser.h
    annotation_layout_tools.h
    annotation_rotation.h
    annotation_style.h
    annotation_flags_ops.h
    table_extraction.cpp
    table_extraction.h
)

target_include_directories(alioth_domain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_library(Alioth::domain ALIAS alioth_domain)
