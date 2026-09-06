# 註解子系統測試（WBS 4.1–4.3）。
#
# 外觀串流測試不需要 PDFium，寫入器測試需要。兩者刻意分開執行檔，
# 讓純函數層的失敗不會被 PDFium 的載入問題掩蓋。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::annotations)

alioth_add_test(test_appearance_stream test_appearance_stream.cpp content_stream_check.h)
alioth_add_test(test_annotation_writer test_annotation_writer.cpp content_stream_check.h)

# pdf_fixture.h 在 tests/ 根目錄。
target_include_directories(test_annotation_writer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# 寫入器測試會實際載入 pdfium.dll，執行檔旁邊必須有它。
add_custom_command(TARGET test_annotation_writer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:test_annotation_writer>"
)
