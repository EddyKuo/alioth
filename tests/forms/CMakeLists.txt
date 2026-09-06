# 表單子系統測試（WBS 6.1–6.4）。
#
# 只在本子目錄追加連結清單，不影響其他測試。
set(ALIOTH_TEST_LIBS ${ALIOTH_TEST_LIBS} Alioth::forms)

# FDF / XFDF 是純函數，不需要 PDFium 也不需要暫存檔。
alioth_add_test(test_form_data test_form_data.cpp)

alioth_add_test(test_form_fields test_form_fields.cpp form_pdf_fixture.h)
# 共用上層的 pdf_fixture.h（writeTempPdf），不另複製一份暫存檔工具。
target_include_directories(test_form_fields PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
# 這個測試會實際載入 pdfium.dll，執行檔旁邊必須有它。
add_custom_command(TARGET test_form_fields POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:test_form_fields>"
)

# CSV 灌入表單（PRD-FORM-023）。runCsvFormFill 在 app 層（跨執行緒編組需要
# QMetaObject::invokeMethod，engine 層不連結 Qt），所以這裡額外連 Alioth::app。
# 同樣需要 pdfium.dll——批次流程實際開檔多次。
set(ALIOTH_TEST_LIBS ${ALIOTH_TEST_LIBS} Alioth::app)
alioth_add_test(test_csv_form_fill test_csv_form_fill.cpp form_pdf_fixture.h)
set_property(TEST test_csv_form_fill APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_custom_command(TARGET test_csv_form_fill POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:test_csv_form_fill>"
)
