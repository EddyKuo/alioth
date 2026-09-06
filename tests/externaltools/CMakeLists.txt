# 第三方程式工具列（PRD-UI-016）。安全敏感度最高的功能之一，測試重點是
# 「驗證階段擋不擋得住不該放行的樣板」與「請求組得對不對」，而不是真的去
# 驗證某個外部程式的行為。純邏輯，QTEST_GUILESS_MAIN 即可，不需要 offscreen。
set(ALIOTH_TEST_LIBS Alioth::app Alioth::platform)
alioth_add_test(test_external_tools test_external_tools.cpp)
alioth_add_test(test_external_process test_external_process.cpp)
