# 掃描增強（WBS 14，PRD-ENH-001 ~ 004）。
#
# 這個 target 橫跨三種相依，刻意讓相依只出現在需要它的檔案裡：
#
#   image_ops      純像素運算，不連 Qt 也不連 PDFium。去斜是本工作包唯一會
#                  「安靜地做錯」的部分，它必須能用合成影像密集驗證，
#                  而多一個相依就多一個測試失敗時的嫌疑犯。
#   image_codec    需要 Qt6::Gui：JPEG（/DCTDecode）的編解碼在專案既有的三個
#                  引擎級元件裡只有 Qt 提供，PDFium 沒有公開的編碼器。
#   page_rasterizer 需要 PDFium：點陣化本來就是把向量頁面渲染成影像。
#
# 寫入一律走 Alioth::objects 的物件層（純附加），因此點陣化與加背景
# 不會破壞既有的數位簽章。
add_library(alioth_enhance STATIC
    image_ops.cpp
    image_ops.h
    image_codec.cpp
    image_codec.h
    pdf_image_writer.cpp
    pdf_image_writer.h
    background_writer.cpp
    background_writer.h
    page_rasterizer.cpp
    page_rasterizer.h
    image_recompressor.cpp
    image_recompressor.h
    scan_enhancer.cpp
    scan_enhancer.h
    barcode_stamp.cpp
    barcode_stamp.h
    color_transform.cpp
    color_transform.h
)

target_include_directories(alioth_enhance PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(alioth_enhance
    PUBLIC Alioth::domain Alioth::objects Alioth::engine
    PRIVATE Qt6::Gui pdfium Alioth::pdfium_runtime
)

add_library(Alioth::enhance ALIAS alioth_enhance)
