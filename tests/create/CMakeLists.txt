# 從其他來源建立 PDF 的測試（WBS 15）。
#
# 產出必須同時通過三種互相獨立的檢驗，缺一不可：
#   1. 我們自己的位元組斷言（例如嵌入的 JPEG 與來源逐位元組相同）
#   2. PdfiumEngine 能開啟且頁數正確——證明檔案真的能用
#   3. qpdf --check 零警告——PDFium 對壞結構的容忍度太高，需要外部裁判
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::create Alioth::engine)

alioth_add_test(test_image_import test_image_import.cpp create_test_support.h)
target_include_directories(test_image_import PRIVATE ${CMAKE_SOURCE_DIR}/tests)
# JPEG 語料現場編碼，需要 Qt 的影像 I/O。
target_link_libraries(test_image_import PRIVATE Qt6::Gui)
set_property(TEST test_image_import APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

alioth_add_test(test_text_import test_text_import.cpp create_test_support.h)
target_include_directories(test_text_import PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_text_import APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

alioth_add_test(test_markdown_import test_markdown_import.cpp create_test_support.h)
target_include_directories(test_markdown_import PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_markdown_import APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# URL 測試一個位元組都不連網：傳輸由測試自己注入。
alioth_add_test(test_url_source test_url_source.cpp create_test_support.h)
target_include_directories(test_url_source PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_url_source APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# CSV 匯入（PRD-IO-013）：版面計算是純領域邏輯，寫出的 PDF 仍要通過
# PDFium 開啟與 qpdf 結構檢查，判準與其餘三種來源一致。
alioth_add_test(test_csv_import test_csv_import.cpp create_test_support.h)
target_include_directories(test_csv_import PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_csv_import APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 網頁 URL 匯入（PRD-IO-014，降級為純文字擷取）：與 URL 測試一樣不連網，
# HTTP 回應全部注入。
alioth_add_test(test_web_page_import test_web_page_import.cpp create_test_support.h)
target_include_directories(test_web_page_import PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_web_page_import APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 引擎測試會實際載入 pdfium.dll，執行檔旁邊必須有它。
foreach(_target test_image_import test_text_import test_markdown_import test_url_source
                test_csv_import test_web_page_import)
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:${_target}>"
    )
    set(_qpdf "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
    if(EXISTS "${_qpdf}")
        target_compile_definitions(${_target} PRIVATE ALIOTH_QPDF_EXECUTABLE="${_qpdf}")
    endif()
endforeach()
