# 自研 Ribbon 的測試（WBS 3.2 / 3.22）。
#
# 只連 Alioth::ribbon：Ribbon 不依賴文件、引擎或主視窗，測試的連結清單就是這條
# 界線的守門員——哪天有人在 Ribbon 裡直接呼叫控制器，這裡會先斷。
set(ALIOTH_TEST_LIBS Alioth::ribbon)

# 資料模型不需要 GUI（QTEST_GUILESS_MAIN），元件測試需要，走 offscreen。
alioth_add_test(test_ribbon_model test_ribbon_model.cpp)
alioth_add_test(test_ribbon_bar test_ribbon_bar.cpp)
set_property(TEST test_ribbon_bar APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
