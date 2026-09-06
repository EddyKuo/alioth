# 頁面標籤（PRD-PAGE-013）。
#
# 領域層的編號規則與 /PageLabels 的讀寫放同一個執行檔：兩者的失敗訊息要放在
# 一起看才有意義（標籤算錯與寫錯，症狀在使用者眼中是同一件事）。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::objects Alioth::labels)
alioth_add_test(test_page_labels test_page_labels.cpp)

# qpdf 是獨立於 PDFium 的第二意見：PDFium 對壞掉的 xref 與錯的 /Length 容忍度很高，
# 用它驗自己寫的位元組等於沒驗。找不到 qpdf 時測試會 skip 並說明原因。
target_include_directories(test_page_labels PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../qa)
set(ALIOTH_LABELS_QPDF_BIN "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(NOT EXISTS "${ALIOTH_LABELS_QPDF_BIN}")
    find_program(ALIOTH_LABELS_QPDF_BIN NAMES qpdf)
endif()
if(ALIOTH_LABELS_QPDF_BIN AND EXISTS "${ALIOTH_LABELS_QPDF_BIN}")
    target_compile_definitions(test_page_labels PRIVATE
        ALIOTH_QPDF_EXECUTABLE="${ALIOTH_LABELS_QPDF_BIN}")
endif()
