# 無障礙（WP32，PRD-A11Y-001 / 004 / 005）。
#
# 三支測試分開而不是併成一支，理由是失敗訊息要能指出是哪一類退步：
# 鍵盤可達性、對比與高對比、標籤結構，三者的修法完全不同。
#
# 全部需要 QT_QPA_PLATFORM=offscreen：它們會真的建構主視窗並走訪 widget 樹，
# 而 QAccessible::queryAccessibleInterface 在沒有 QApplication 的情況下
# 回傳 nullptr——那會讓稽核假性通過（沒有介面就沒有缺失可報）。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine Alioth::app Alioth::ui)

alioth_add_test(test_a11y_keyboard test_keyboard_access.cpp)
set_property(TEST test_a11y_keyboard APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

alioth_add_test(test_a11y_contrast test_contrast.cpp)
set_property(TEST test_a11y_contrast APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

alioth_add_test(test_a11y_struct_tree test_struct_tree.cpp tagged_pdf_fixture.h)
set_property(TEST test_a11y_struct_tree APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
