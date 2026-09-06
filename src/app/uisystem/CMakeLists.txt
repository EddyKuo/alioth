# 介面體系（WP30：多頁籤、快捷鍵、多語系、主題、工具持續模式、頁籤標題／重新命名、
# 字數統計）。獨立成一個 target 而不是併進 alioth_app：這裡的每一件事都是純邏輯或
# 只依賴 Qt 的資料模型，不需要文件、引擎或命令堆疊，獨立出來讓測試不必拖進整個
# 應用層的相依圖，也讓「widget 只是薄殼」這件事在建置系統層級被強制——
# 這個 target 不連結 Alioth::ui，因此它不可能反過來去操作某個具體的 widget。
add_library(alioth_uisystem STATIC
    cursor_scale.cpp
    cursor_scale.h
    locale_manager.cpp
    locale_manager.h
    shortcut_scheme.cpp
    shortcut_scheme.h
    tab_layout_model.cpp
    tab_layout_model.h
    tab_title_model.cpp
    tab_title_model.h
    text_statistics.cpp
    text_statistics.h
    theme_controller.cpp
    theme_controller.h
    theme_palette.cpp
    theme_palette.h
    tool_persistence.h
)

target_include_directories(alioth_uisystem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(alioth_uisystem PUBLIC Qt6::Core Qt6::Gui)

add_library(Alioth::uisystem ALIAS alioth_uisystem)
