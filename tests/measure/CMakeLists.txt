# 量測與註解摘要測試（WP25：PRD-ANN-014 / 024 / 025 / 026 / 027 / 028）。
#
# 分四支執行檔，理由與 tests/annotations、tests/objects 一致：純函數層
# （domain 的比例／幾何計算、CSV）不該被 PDFium 的載入問題掩蓋，
# 物件層讀寫需要 PDFium 重新開啟驗證，應用層服務再多一層檔案 I/O。

set(ALIOTH_TEST_LIBS Alioth::domain)
alioth_add_test(test_measurement_domain test_measurement_domain.cpp)

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::objects)
alioth_add_test(test_measure_reader test_measure_reader.cpp)

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::objects Alioth::annotations)
alioth_add_test(test_measure_writer test_measure_writer.cpp)
target_include_directories(test_measure_writer PRIVATE
    ${CMAKE_SOURCE_DIR}/tests/objects
    ${CMAKE_SOURCE_DIR}/tests/qa
)

# ADR-002 驗收條件 1：產出的檔案要通過 qpdf --check。沿用 tests/objects 的探測邏輯。
set(ALIOTH_MEASURE_QPDF_BIN "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(NOT EXISTS "${ALIOTH_MEASURE_QPDF_BIN}")
    find_program(ALIOTH_MEASURE_QPDF_BIN NAMES qpdf)
endif()
if(ALIOTH_MEASURE_QPDF_BIN AND EXISTS "${ALIOTH_MEASURE_QPDF_BIN}")
    target_compile_definitions(test_measure_writer PRIVATE
        ALIOTH_QPDF_EXECUTABLE="${ALIOTH_MEASURE_QPDF_BIN}")
endif()

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app)
alioth_add_test(test_measurement_service test_measurement_service.cpp)
target_include_directories(test_measurement_service PRIVATE ${CMAKE_SOURCE_DIR}/tests/objects)
set_property(TEST test_measurement_service APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

alioth_add_test(test_comment_summary_service test_comment_summary_service.cpp)
set_property(TEST test_comment_summary_service APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 會實際載入 pdfium.dll 的執行檔，旁邊必須有它，否則退出碼是 0xc0000135。
foreach(_target test_measure_writer test_measurement_service test_comment_summary_service)
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:${_target}>"
    )
endforeach()
