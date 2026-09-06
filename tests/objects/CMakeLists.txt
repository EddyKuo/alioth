# 物件層寫入通道測試（ADR-002）。
#
# 序列化那一層是純函數，刻意不連 PDFium：它的失敗不該被 PDFium 的載入問題掩蓋。
# 另外兩個執行檔要重新開啟產出的檔案來驗證，那必須走 PDFium 這條完全獨立的
# 路徑——用我們自己的剖析器驗自己寫的位元組，等於用同一份程式碼證明自己是對的。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::objects)
alioth_add_test(test_pdf_object test_pdf_object.cpp)

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::objects Alioth::annotations)
alioth_add_test(test_incremental_appender test_incremental_appender.cpp object_fixture.h)
alioth_add_test(test_annotation_objects test_annotation_objects.cpp object_fixture.h)
alioth_add_test(test_annotation_flattener test_annotation_flattener.cpp object_fixture.h)
alioth_add_test(test_annotation_note test_annotation_note.cpp object_fixture.h)
alioth_add_test(test_annotation_reader test_annotation_reader.cpp object_fixture.h)

# ADR-002 驗收條件 1：產出的檔案要通過 qpdf --check。
#
# qpdf 是獨立於 PDFium 的第二意見，這正是它存在的理由：PDFium 對壞掉的 xref、
# 錯的 /Length、懸空參照容忍度很高，用它驗自己寫的 xref 等於沒驗。
# 檢查工具與判定規則沿用 tests/qa/qpdf_check.h，不另建一份。
target_include_directories(test_incremental_appender PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../qa)
set(ALIOTH_OBJECTS_QPDF_BIN "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(NOT EXISTS "${ALIOTH_OBJECTS_QPDF_BIN}")
    find_program(ALIOTH_OBJECTS_QPDF_BIN NAMES qpdf)
endif()
if(ALIOTH_OBJECTS_QPDF_BIN AND EXISTS "${ALIOTH_OBJECTS_QPDF_BIN}")
    target_compile_definitions(test_incremental_appender PRIVATE
        ALIOTH_QPDF_EXECUTABLE="${ALIOTH_OBJECTS_QPDF_BIN}")
endif()

foreach(_target test_incremental_appender test_annotation_objects test_annotation_flattener)
    # 測試會實際載入 pdfium.dll，執行檔旁邊必須有它，否則退出碼是 0xc0000135。
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:${_target}>"
    )
endforeach()
