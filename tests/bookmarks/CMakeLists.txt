# 書籤全套的測試（WBS 9，PRD-BM-002 ~ 019）。
#
# 批次操作的純邏輯不連任何引擎：它的失敗不該被 PDFium 的載入問題掩蓋，
# 而且純函數才跑得起密到足以涵蓋空樹／單節點／深巢狀的測試。
set(ALIOTH_TEST_LIBS Alioth::domain)
alioth_add_test(test_bookmark_ops test_bookmark_ops.cpp)

# 寫入的驗證一律由**別人**執行：PdfiumEngine::outline() 走 PDFium 的
# FPDFBookmark_* API，qpdf --check 是獨立於 PDFium 的第二意見。
# 用自己的剖析器驗自己寫的位元組等於沒驗。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::bookmarks Alioth::engine)
alioth_add_test(test_outline_writer test_outline_writer.cpp bookmark_fixture.h)
target_include_directories(test_outline_writer PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_outline_writer APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

alioth_add_test(test_bookmark_batch test_bookmark_batch.cpp bookmark_fixture.h)
target_include_directories(test_bookmark_batch PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_bookmark_batch APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# qpdf 的位置由建置期常數提供；沒有它結構檢查只會 skip，而 skip 掉的檢查等於沒檢查。
set(_qpdf "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(EXISTS "${_qpdf}")
    target_compile_definitions(test_outline_writer PRIVATE ALIOTH_QPDF_EXECUTABLE="${_qpdf}")
    target_compile_definitions(test_bookmark_batch PRIVATE ALIOTH_QPDF_EXECUTABLE="${_qpdf}")
endif()

# 引擎測試會載入 pdfium.dll，執行檔旁邊必須有它。
foreach(_target test_outline_writer test_bookmark_batch)
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:${_target}>"
    )
endforeach()
