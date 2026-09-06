# 自研 Ribbon（WBS 3.2 + 3.22；ADR-001 第 3 節決定不引入 SARibbon 或商業元件）。
#
# 刻意獨立成一個 target 而不是併進 alioth_ui：Ribbon 只依賴 Qt，不認識文件、
# 註解或引擎，這條界線由連結清單守住——只要它沒有 Alioth::app，就不可能長成
# 第二個 main_window，測試也不必為了按一顆按鈕而拖進 PDFium。
add_library(alioth_ribbon STATIC
    action_registry.cpp
    action_registry.h
    icon_resolver.cpp
    icon_resolver.h
    key_tip_controller.cpp
    key_tip_controller.h
    ribbon_bar.cpp
    ribbon_bar.h
    ribbon_button.cpp
    ribbon_button.h
    ribbon_default_layout.cpp
    ribbon_default_layout.h
    ribbon_group.cpp
    ribbon_group.h
    ribbon_model.cpp
    ribbon_model.h
    ribbon_page.cpp
    ribbon_page.h
)

# 標頭一律以 "ui/ribbon/..." 引用，與其他層的慣例一致。
target_include_directories(alioth_ribbon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(alioth_ribbon PUBLIC Qt6::Widgets)

add_library(Alioth::ribbon ALIAS alioth_ribbon)
