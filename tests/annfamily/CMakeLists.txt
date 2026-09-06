# WP24 註解工具家族測試：PRD-ANN-005/017/018/019/021/022/030。
#
# 分五個執行檔而不是一個，是為了讓失敗訊息一眼看出是哪一層壞的——
# 領域層的座標算錯、外觀串流語法錯、物件層漏寫必要鍵、還是像素真的沒變色，
# 四種失敗原因混在同一支測試裡，紅燈時還要先猜是哪一段。

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::annotations)
alioth_add_test(test_annfamily_domain test_annfamily_domain.cpp)
alioth_add_test(test_annfamily_text_layout test_annfamily_text_layout.cpp)

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::annotations)
alioth_add_test(test_annfamily_appearance test_annfamily_appearance.cpp)
target_include_directories(test_annfamily_appearance PRIVATE ${CMAKE_SOURCE_DIR}/tests)

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::annotations Alioth::objects)
alioth_add_test(test_annfamily_object_writer test_annfamily_object_writer.cpp)
target_include_directories(test_annfamily_object_writer PRIVATE ${CMAKE_SOURCE_DIR}/tests)

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app)
alioth_add_test(test_annfamily_builders test_annfamily_builders.cpp)
target_include_directories(test_annfamily_builders PRIVATE ${CMAKE_SOURCE_DIR}/tests)

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine Alioth::objects)
alioth_add_test(test_annfamily_pixel test_annfamily_pixel.cpp)
target_include_directories(test_annfamily_pixel PRIVATE ${CMAKE_SOURCE_DIR}/tests)

# qpdf --check（PRD §9 明列的驗收條件）：只在 test_annfamily_object_writer 跑，
# 其餘測試不落地成檔案，沒有東西可以檢查。
set(_annfamily_qpdf_bin "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(NOT EXISTS "${_annfamily_qpdf_bin}")
    find_program(_annfamily_qpdf_bin NAMES qpdf)
endif()
if(_annfamily_qpdf_bin AND EXISTS "${_annfamily_qpdf_bin}")
    target_compile_definitions(test_annfamily_object_writer PRIVATE
        ALIOTH_QPDF_EXECUTABLE="${_annfamily_qpdf_bin}")
endif()

# alioth_annotations 私下連了 pdfium：即使某支測試不直接呼叫 PDFium API，
# 連結器仍可能拉進需要它的物件檔。統一複製 pdfium.dll 到每個執行檔旁邊，
# 比逐一判斷「這支到底需不需要」更省事，也不會在改動內部實作後悄悄壞掉。
foreach(_target test_annfamily_domain test_annfamily_text_layout test_annfamily_appearance
               test_annfamily_object_writer test_annfamily_builders test_annfamily_pixel)
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:${_target}>"
    )
endforeach()
