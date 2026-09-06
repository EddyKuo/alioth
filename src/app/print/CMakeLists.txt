# 列印與戳記（WBS 5.9 / 5.11，PRD-IO-008 / PRD-PAGE-004）。
#
# 屬於應用層：可以用 Qt，但不得連結 PDFium。頁面點陣一律經由 Alioth::engine 取得，
# 若這個目標出現 target_link_libraries(... pdfium) 那就是分層違規。
#
# PrintSupport 只在這裡 find_package：根目錄的 find_package 沒有列它，
# 而列印是唯一需要它的子系統，就近宣告可以讓相依範圍一目了然。
find_package(Qt6 REQUIRED COMPONENTS PrintSupport)

add_library(alioth_print STATIC
    bates_numbering.cpp
    bates_numbering.h
    page_range.cpp
    page_range.h
    print_plan.cpp
    print_plan.h
    print_service.cpp
    print_service.h
    stamp_content_stream.cpp
    stamp_content_stream.h
    stamp_layout.cpp
    stamp_layout.h
    stamp_painter.cpp
    stamp_painter.h
)

target_link_libraries(alioth_print
    # Alioth::engine 是 PUBLIC：列印服務的建構與渲染都需要引擎的標頭，
    # 但 PDFium 本身仍然只被 alioth_engine 連結，不會滲到這裡。
    PUBLIC Alioth::domain Alioth::engine Alioth::fonts Qt6::Core Qt6::Gui Qt6::PrintSupport
)

add_library(Alioth::print ALIAS alioth_print)
