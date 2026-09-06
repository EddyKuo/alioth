# 無障礙其餘項目（WP36，PRD-A11Y-002 / 003 / 004 / 006）。
#
# 四支測試分開而不是併成一支，理由與 tests/a11y 相同：失敗訊息要能指出
# 是閱讀順序比對、檢查器、替代文字寫入，還是朗讀文字擷取哪一類退步。
#
# 都不需要 QApplication（沒有 widget），也不需要 PDFium（走物件層），
# 因此用 QTEST_APPLESS_MAIN 而不是 QTEST_MAIN，執行更快也更少環境相依。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::objects)

alioth_add_test(test_reading_order test_reading_order.cpp)
alioth_add_test(test_reading_text_extractor test_reading_text_extractor.cpp)
alioth_add_test(test_accessibility_checker test_accessibility_checker.cpp a11y_checker_fixture.h)
alioth_add_test(test_struct_alt_text_writer test_struct_alt_text_writer.cpp)

# 應用層服務需要完整的原子寫入與應用層相依，走獨立的 ALIOTH_TEST_LIBS。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine Alioth::app)
alioth_add_test(test_accessibility_service test_accessibility_service.cpp)
set_property(TEST test_accessibility_service APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# SAPI 包裝只依賴平台層。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::platform)
alioth_add_test(test_speech_synthesis test_speech_synthesis.cpp)
