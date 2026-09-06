# 文件比對的測試（PRD-CMP-001/002、PRD-SRCH-003）。
#
# diff 與頁面對齊是純邏輯，不連 PDFium：它們的正確性最需要密集測試，
# 而純函數的測試可以跑得夠快去涵蓋邊界情形。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::compare)
alioth_add_test(test_token_diff test_token_diff.cpp)
alioth_add_test(test_page_align test_page_align.cpp)

# 端到端比對需要文字擷取，因此連 Alioth::text；用 offscreen 平台跑。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::compare Alioth::text)
alioth_add_test(test_document_compare test_document_compare.cpp compare_pdf_fixture.h)
set_property(TEST test_document_compare APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 尋找並取代（PRD-SRCH-003）。這一塊先前只有間接使用，沒有直接測試；
# 最重要的性質是「頁面內容串流絕不被碰」，因此以位元組層級驗證。
# qpdf 是獨立於 PDFium 的第二意見，找不到時測試會 skip 並說明原因。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::objects Alioth::compare)
alioth_add_test(test_find_replace test_find_replace.cpp)
target_include_directories(test_find_replace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../qa)
set(ALIOTH_FINDREPLACE_QPDF_BIN "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(NOT EXISTS "${ALIOTH_FINDREPLACE_QPDF_BIN}")
    find_program(ALIOTH_FINDREPLACE_QPDF_BIN NAMES qpdf)
endif()
if(ALIOTH_FINDREPLACE_QPDF_BIN AND EXISTS "${ALIOTH_FINDREPLACE_QPDF_BIN}")
    target_compile_definitions(test_find_replace PRIVATE
        ALIOTH_QPDF_EXECUTABLE="${ALIOTH_FINDREPLACE_QPDF_BIN}")
endif()

# 比較結果的呈現（PRD-CMP-001）。差異怎麼算由上面的測試守，
# 這裡守的是「使用者會不會誤判自己看到了什麼」。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::ui)
alioth_add_test(test_compare_dialog test_compare_dialog.cpp)
set_property(TEST test_compare_dialog APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
