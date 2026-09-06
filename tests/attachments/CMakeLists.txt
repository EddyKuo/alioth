# 附件讀取（PRD-ANN-015）。純物件層，不連 PDFium。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::objects Alioth::attachments)
alioth_add_test(test_attachment_reader test_attachment_reader.cpp)

# 寫入側。輸出要通得過 qpdf --check：名稱樹的鍵沒排序或重複時，
# 用二分搜尋的解析器會找不到項目，而 PDFium 對這種錯很寬容。
alioth_add_test(test_attachment_writer test_attachment_writer.cpp)
target_include_directories(test_attachment_writer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../qa)
set(ALIOTH_ATTACH_QPDF_BIN "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(NOT EXISTS "${ALIOTH_ATTACH_QPDF_BIN}")
    find_program(ALIOTH_ATTACH_QPDF_BIN NAMES qpdf)
endif()
if(ALIOTH_ATTACH_QPDF_BIN AND EXISTS "${ALIOTH_ATTACH_QPDF_BIN}")
    target_compile_definitions(test_attachment_writer PRIVATE
        ALIOTH_QPDF_EXECUTABLE="${ALIOTH_ATTACH_QPDF_BIN}")
endif()

# 音訊／視訊註解（PRD-ANN-016）。本產品不播放內嵌媒體（理由見
# engine/attachments/media_annotations.h），因此這裡驗的是列出、標示與取出。
alioth_add_test(test_media_annotations test_media_annotations.cpp)
