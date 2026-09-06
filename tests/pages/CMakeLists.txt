# 頁面管理測試（WBS 5.6–5.8）。
#
# 連 Alioth::text 的理由：存檔結果一律用另一條獨立路徑（文字擷取）重新讀回每頁的
# 標記來驗證順序，而不是用 alioth_pages 自己的 API 證明自己是對的。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::pages Alioth::text)

# 領域層測試不碰 PDFium，也就不需要 pdfium.dll。
alioth_add_test(test_page_operations test_page_operations.cpp)

alioth_add_test(test_page_editor test_page_editor.cpp page_fixture.h)
alioth_add_test(test_page_assembly test_page_assembly.cpp page_fixture.h)
alioth_add_test(test_page_crop test_page_crop.cpp page_fixture.h)

foreach(_target test_page_editor test_page_assembly test_page_crop)
    # 這些測試會實際載入 pdfium.dll，執行檔旁邊必須有它，否則退出碼是 0xc0000135。
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:${_target}>"
    )
endforeach()

# 應用層的刪除／擷取：擋掉「刪光所有頁面」與「擷取覆蓋來源」兩個不可復原的錯。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app Alioth::pages)
alioth_add_test(test_page_range_service test_page_range_service.cpp)
set_property(TEST test_page_range_service APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_custom_command(TARGET test_page_range_service POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:test_page_range_service>"
)
