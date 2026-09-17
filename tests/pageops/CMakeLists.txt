# 頁面進階操作（WBS 12，PRD-PAGE-007 ~ 011）。
#
# 版面數學與檔案寫入分成兩組測試：前者是純函數、不連 PDFium，後者要把產出的
# 檔案交給 PDFium 與 qpdf 讀回來。混在一起的話，任何一個失敗都要先花時間
# 排除另一個的嫌疑。
set(ALIOTH_TEST_LIBS Alioth::domain)
alioth_add_test(test_page_compose test_page_compose.cpp)

# 寫入測試需要三條互相獨立的讀取路徑（見 pageops_readback.h）：
# Alioth::text 的文字層、Alioth::objects 的物件層、以及 qpdf。
# 合併還多要 Alioth::engine：圖形狀態有沒有互相污染只有渲染看得出來，
# 看串流長得對不對證明不了畫出來是對的。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::pageops Alioth::objects Alioth::text Alioth::engine)
alioth_add_test(test_page_merge test_page_merge.cpp pageops_fixture.h pageops_readback.h
    pageops_render.h)
alioth_add_test(test_page_overlay test_page_overlay.cpp pageops_fixture.h pageops_readback.h
    pageops_render.h)
alioth_add_test(test_page_boxes test_page_boxes.cpp pageops_fixture.h pageops_readback.h)

# 「文件加摘要」走應用層的服務入口（PRD-ANN-028）：底層排版支援中文之後，
# 還要確認摘要這條路徑沒有自己再擋一次非 ASCII。
set(ALIOTH_TEST_LIBS ${ALIOTH_TEST_LIBS} Alioth::app Alioth::fonts)
alioth_add_test(test_summary_pages test_summary_pages.cpp pageops_fixture.h pageops_readback.h)
target_include_directories(test_summary_pages PRIVATE ${CMAKE_SOURCE_DIR}/tests)

foreach(_target test_page_merge test_page_overlay test_page_boxes test_summary_pages)
    target_include_directories(${_target} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    set_property(TEST ${_target} APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # qpdf 的位置由建置期常數提供；沒有它結構檢查只會 skip，而 skip 掉的檢查等於沒檢查。
    set(_qpdf "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
    if(EXISTS "${_qpdf}")
        target_compile_definitions(${_target} PRIVATE ALIOTH_QPDF_EXECUTABLE="${_qpdf}")
    endif()
endforeach()
