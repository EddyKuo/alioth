# 文字選取與搜尋（WP31，PRD-TXT-003/005、PRD-SRCH-002/004）。

# 表格切欄分列是純幾何運算，不需要 PDFium。
set(ALIOTH_TEST_LIBS Alioth::domain)
alioth_add_test(test_table_extraction test_table_extraction.cpp)

# 有界正規表示式同樣是純邏輯：它存在的理由就是「使用者輸入的樣式不可以讓程式卡死」，
# 那件事不需要文件也驗得出來。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::text)
alioth_add_test(test_bounded_regex test_bounded_regex.cpp)

# 多文件搜尋要真的開多份文件：PDFium 非執行緒安全，每份自己的把手與執行緒，
# 這條規則只有在真的併行時才驗得出來。
alioth_add_test(test_multi_document_search test_multi_document_search.cpp)
target_include_directories(test_multi_document_search PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../text)
set_property(TEST test_multi_document_search APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 快照走 enhance 的整頁渲染器（使用者明確要求輸出點陣，是允許的例外）。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::enhance)
alioth_add_test(test_snapshot test_snapshot.cpp)
target_include_directories(test_snapshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
set_property(TEST test_snapshot APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 拼字檢查只作用於註解與表單文字（與 PRD-SRCH-003 的範圍一致），走物件層讀取。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::objects Alioth::compare)
alioth_add_test(test_spellcheck test_spellcheck.cpp)
set_property(TEST test_spellcheck APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
