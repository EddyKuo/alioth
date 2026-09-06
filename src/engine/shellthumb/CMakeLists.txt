# 檔案總管縮圖用的最小渲染器（PRD-UI-020）。
#
# 這個 target 刻意**不連結 Alioth::platform**，因此也不會拖進 Qt——它的產物
# 最終會被載進 explorer.exe，而把 Qt 載進每一個開著資料夾的檔案總管視窗
# 是使用者不會同意的代價。相依只有 pdfium 與它的全域初始化。
add_library(alioth_shellthumb STATIC
    thumbnail_renderer.cpp
    thumbnail_renderer.h
)

target_include_directories(alioth_shellthumb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(alioth_shellthumb
    PRIVATE pdfium Alioth::pdfium_runtime
)

add_library(Alioth::shellthumb ALIAS alioth_shellthumb)
