# 塗黑與清理的測試（WBS 11）。
#
# 需要 Alioth::text：PRD-ANN-032 的驗收標準是「以文字擷取驗證原文不可還原」，
# 自己解內容串流只會驗到「我們以為自己刪了」，讀不到別人怎麼看這份檔案。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::redaction Alioth::text)
alioth_add_test(test_redaction_apply test_redaction_apply.cpp redaction_fixture.h)
set_property(TEST test_redaction_apply APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_sanitize test_sanitize.cpp redaction_fixture.h)

# Find and Redact 的標記階段（PRD-ANN-033）。需要 Alioth::text（命中外框取自
# PDFium 的字元外框），因此走 offscreen 平台。
alioth_add_test(test_find_and_redact test_find_and_redact.cpp redaction_fixture.h)
set_property(TEST test_find_and_redact APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 應用層的塗黑服務：標記走增量附加（可逆）、套用走整份重寫（不可逆）。
# 需要 Alioth::app 與 Alioth::objects（object_fixture.h 沿用 tests/objects 的版本）。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app Alioth::redaction Alioth::objects)
alioth_add_test(test_redaction_service test_redaction_service.cpp)
target_include_directories(test_redaction_service PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../objects)
set_property(TEST test_redaction_service APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
