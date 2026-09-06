# 文字擷取與搜尋的測試（WBS 2.8 / 2.9）。

# 只在本子目錄追加，不影響其他測試的連結清單。
set(ALIOTH_TEST_LIBS ${ALIOTH_TEST_LIBS} Alioth::text)

alioth_add_test(test_text_layer test_text_layer.cpp)
alioth_add_test(test_text_extractor test_text_extractor.cpp text_pdf_fixture.h)
alioth_add_test(test_text_search test_text_search.cpp text_pdf_fixture.h)

foreach(_target test_text_extractor test_text_search)
    # 共用上層的 pdf_fixture.h（writeTempPdf），不另複製一份暫存檔工具。
    target_include_directories(${_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    # 這兩個測試會實際載入 pdfium.dll，執行檔旁邊必須有它。
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:${_target}>"
    )
endforeach()
