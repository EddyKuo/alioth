# 列印與戳記測試（WBS 5.9 / 5.11）。alioth_add_test 由父層 tests/CMakeLists.txt 定義。

# 純邏輯測試：不需要引擎，也不需要事件迴圈。
set(ALIOTH_TEST_LIBS Alioth::print)
alioth_add_test(test_bates_numbering test_bates_numbering.cpp)
alioth_add_test(test_print_plan test_print_plan.cpp)

# 端到端測試會真的開 PDF、真的輸出 PDF，需要引擎與 QPA 平台外掛。
set(ALIOTH_TEST_LIBS Alioth::print Alioth::engine)
alioth_add_test(test_print_output test_print_output.cpp print_fixture.h)
set_property(TEST test_print_output APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 端到端測試會載入 pdfium.dll，執行檔旁邊必須有它。
add_custom_command(TARGET test_print_output POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:test_print_output>"
)
