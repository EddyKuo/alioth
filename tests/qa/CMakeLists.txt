# 品質關卡測試（WBS 7.3 / 7.4，PRD §9）。
#
# 這個目錄放的是「驗證產出物本身」的測試，與各子系統的功能測試分開：
#   test_qpdf_structure   — 產出的 PDF 是否通過獨立於 PDFium 的結構檢查
#   test_render_regression — 渲染畫面是否與基準影像一致
#
# 兩者都刻意不修改既有測試，而是自己再走一次同樣的產出路徑。

set(ALIOTH_TEST_LIBS
    Alioth::domain Alioth::engine Alioth::annotations Alioth::save
    Alioth::platform Alioth::app Alioth::text)

# qpdf 的位置在 configure 時決定，執行時仍可用環境變數 ALIOTH_QPDF 覆蓋
# （見 qpdf_check.h 的搜尋順序）。找不到不是錯誤——測試會 skip 並說明原因，
# 但這裡要留下訊息，否則「為什麼 CI 上全部 skip」得翻半天才查得到。
set(ALIOTH_QPDF_BIN "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(NOT EXISTS "${ALIOTH_QPDF_BIN}")
    find_program(ALIOTH_QPDF_BIN NAMES qpdf)
endif()
if(ALIOTH_QPDF_BIN AND EXISTS "${ALIOTH_QPDF_BIN}")
    message(STATUS "qpdf 結構檢查: ${ALIOTH_QPDF_BIN}")
else()
    message(WARNING
        "找不到 qpdf，結構檢查測試將全部 skip（PRD-IO-001 的驗收條件因此未被驗證）。"
        " 執行 tools/qpdf/fetch_qpdf.bat 取得後重新 configure。")
    set(ALIOTH_QPDF_BIN "")
endif()

alioth_add_test(test_qpdf_structure test_qpdf_structure.cpp qpdf_check.h)
target_include_directories(test_qpdf_structure PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..          # pdf_fixture.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../save     # save_fixture.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../text     # text_pdf_fixture.h
    ${CMAKE_SOURCE_DIR}/tools/corpus)       # engineering_corpus.h
if(ALIOTH_QPDF_BIN)
    target_compile_definitions(test_qpdf_structure PRIVATE
        ALIOTH_QPDF_EXECUTABLE="${ALIOTH_QPDF_BIN}")
endif()
# 應用層要跑事件迴圈，但不需要顯示器。
set_property(TEST test_qpdf_structure APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

alioth_add_test(test_render_regression test_render_regression.cpp golden_image.h)
target_include_directories(test_render_regression PRIVATE ${CMAKE_SOURCE_DIR}/tools/corpus)
# 基準影像放在原始碼樹裡（tests/golden/），因為它們是要進版控的驗收資產，
# 不是建置產物。路徑在 configure 時綁死，測試不必猜自己被從哪個目錄啟動。
target_compile_definitions(test_render_regression PRIVATE
    ALIOTH_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
set_property(TEST test_render_regression APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# 整頁拼圖磚比單元測試慢，預設的 120 秒對五個情境不夠。
set_property(TEST test_render_regression PROPERTY TIMEOUT 600)

foreach(_target test_qpdf_structure test_render_regression)
    # 測試會實際載入 pdfium.dll，執行檔旁邊必須有它，否則退出碼是 0xc0000135。
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:${_target}>"
    )
endforeach()
