# 儲存子系統測試（WBS 5.1 / 5.3）。
#
# 除了 Alioth::save 之外還連 Alioth::engine 與 Alioth::platform：
# 存檔結果刻意用另一條獨立路徑（PdfiumEngine）重新開啟來驗證，
# 而原子寫入的中途放棄行為只有直接操作 AtomicFileWriter 才測得到。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine Alioth::save Alioth::platform)

alioth_add_test(test_incremental_saver test_incremental_saver.cpp save_fixture.h reopen_probe.h)
alioth_add_test(test_autosave test_autosave.cpp save_fixture.h reopen_probe.h)

foreach(_target test_incremental_saver test_autosave)
    # 測試會實際載入 pdfium.dll，執行檔旁邊必須有它，否則會是 0xc0000135。
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:${_target}>"
    )
endforeach()


# PRD-IO-001 的兩條驗收數字：100 MB 文件加一個註解的耗時與增量段大小。
# 需要應用層的註解服務（真正的寫入路徑就在那裡）。
#
# 語料是 100 MB，產生與寫入都要時間，因此把 TIMEOUT 放寬——預設的 120 秒
# 在慢速磁碟上會逾時，而逾時看起來像效能缺陷，實際只是測試自己的預算太緊。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app Alioth::objects Alioth::platform)
alioth_add_test(test_large_incremental_save test_large_incremental_save.cpp save_fixture.h)
set_property(TEST test_large_incremental_save APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(test_large_incremental_save PROPERTIES TIMEOUT 300)
