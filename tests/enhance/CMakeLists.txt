# 掃描增強的測試（WBS 14，PRD-ENH-001 ~ 004）。
#
# 像素演算法與 PDF 讀寫分成兩支：前者不需要 PDF 語料也不需要 PDFium，
# 跑得快而且失敗訊息只指向一個地方；後者才需要造檔案與外部裁判（qpdf）。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::enhance)
alioth_add_test(test_image_ops test_image_ops.cpp)

# 寫入路徑的驗證要有外部裁判：自己寫的東西由自己的解析器讀回來，
# 只會證明我們前後一致地寫錯。文字層用 Alioth::text（PDFium），
# 結構用 qpdf，兩者都獨立於寫入端。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::enhance Alioth::text)
alioth_add_test(test_enhance_pdf test_enhance_pdf.cpp)
target_include_directories(test_enhance_pdf PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_enhance_pdf APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::enhance)
alioth_add_test(test_recompress test_recompress.cpp)
target_include_directories(test_recompress PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_recompress APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

alioth_add_test(test_scan_enhance test_scan_enhance.cpp)
target_include_directories(test_scan_enhance PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_scan_enhance APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 色彩轉換與貼條碼（PRD-ENH-006 / PRD-ENH-007）：純附加寫入，不需要 PDFium，
# 與 test_image_ops 同一組相依。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::enhance)
alioth_add_test(test_color_transform test_color_transform.cpp)
alioth_add_test(test_barcode_stamp test_barcode_stamp.cpp)

set(_qpdf "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(EXISTS "${_qpdf}")
    target_compile_definitions(test_enhance_pdf PRIVATE ALIOTH_QPDF_EXECUTABLE="${_qpdf}")
    target_compile_definitions(test_recompress PRIVATE ALIOTH_QPDF_EXECUTABLE="${_qpdf}")
    target_compile_definitions(test_scan_enhance PRIVATE ALIOTH_QPDF_EXECUTABLE="${_qpdf}")
endif()
