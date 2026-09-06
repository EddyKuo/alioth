# 介面體系測試（WP30）。多數是純邏輯，不需要 QApplication，走 QTEST_GUILESS_MAIN；
# 只有 ThemeController 用到 qApp（QStyleHints 的系統主題訂閱）需要 offscreen。
set(ALIOTH_TEST_LIBS Alioth::uisystem)

alioth_add_test(test_shortcut_scheme test_shortcut_scheme.cpp)
alioth_add_test(test_tab_layout_model test_tab_layout_model.cpp)
alioth_add_test(test_tab_title_model test_tab_title_model.cpp)
alioth_add_test(test_theme_palette test_theme_palette.cpp)
alioth_add_test(test_text_statistics test_text_statistics.cpp)
alioth_add_test(test_tool_persistence test_tool_persistence.cpp)

alioth_add_test(test_theme_controller test_theme_controller.cpp)
set_property(TEST test_theme_controller APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

alioth_add_test(test_cursor_scale test_cursor_scale.cpp)
set_property(TEST test_cursor_scale APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

alioth_add_test(test_locale_manager test_locale_manager.cpp)
set_property(TEST test_locale_manager APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
if(TARGET alioth_translations)
    add_dependencies(test_locale_manager alioth_translations)
endif()
target_compile_definitions(test_locale_manager PRIVATE
    ALIOTH_I18N_SOURCE_DIR="${CMAKE_SOURCE_DIR}/i18n"
    ALIOTH_I18N_BUILD_DIR="${CMAKE_BINARY_DIR}/i18n")
