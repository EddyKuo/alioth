# 表單建立與受限運算式引擎的測試（WBS 10）。
#
# 運算式引擎與外觀產生器是純函數，刻意不連 PDFium：它們的失敗不該被
# PDFium 的載入問題掩蓋，而且純函數的測試可以跑得夠密集去涵蓋畸形輸入。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::formbuild)
alioth_add_test(test_expression test_expression.cpp)
alioth_add_test(test_form_calculation test_form_calculation.cpp)
alioth_add_test(test_field_tree test_field_tree.cpp)
alioth_add_test(test_field_appearance_ascii test_field_appearance_ascii.cpp)
alioth_add_test(test_field_duplicate test_field_duplicate.cpp)
alioth_add_test(test_barcode_field test_barcode_field.cpp)

# 欄位寫入必須由 PDFium 的表單 API 讀回來驗證，所以這一支多連 Alioth::forms；
# qpdf 的結構檢查輔助在 tests/qa 下，需要它的 include 路徑。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::formbuild Alioth::forms)
alioth_add_test(test_field_writer test_field_writer.cpp)
target_include_directories(test_field_writer PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_field_writer APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# qpdf 的位置由 tests/qa 設定成編譯期常數；沒有它這支測試只會 skip 掉結構檢查，
# 而 skip 掉的檢查等於沒檢查。
set(_qpdf "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(EXISTS "${_qpdf}")
    target_compile_definitions(test_field_writer PRIVATE ALIOTH_QPDF_EXECUTABLE="${_qpdf}")
endif()

# 應用層的欄位建立：撞名擋下、空矩形擋下、寫入純附加。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app Alioth::formbuild Alioth::objects)
alioth_add_test(test_form_build_service test_form_build_service.cpp)
target_include_directories(test_form_build_service PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../objects)
set_property(TEST test_form_build_service APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
