# 主視窗截圖工具。console 子系統，讓錯誤訊息看得見。
add_executable(alioth_uishot main.cpp)
target_link_libraries(alioth_uishot PRIVATE Alioth::ui)
set_target_properties(alioth_uishot PROPERTIES WIN32_EXECUTABLE OFF)
