# 觸控最佳化的純邏輯核心（PRD-UI-013）。獨立成一個 target 而不是併進
# alioth_app：這裡的每一件事（命中區域尺寸、長按判定、雙指縮放數學）都是
# 純數學與狀態機，只依賴 Qt::Core（QPointF/QSizeF），不需要文件、引擎或
# 任何 widget，讓測試不必拖進整個應用層的相依圖。
add_library(alioth_touch STATIC
    touch_gestures.cpp
    touch_gestures.h
)

target_include_directories(alioth_touch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../)
target_link_libraries(alioth_touch PUBLIC Qt6::Core)

add_library(Alioth::touch ALIAS alioth_touch)
