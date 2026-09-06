# 觸控最佳化的純邏輯測試（PRD-UI-013）。不需要 QApplication：命中區域、
# 長按判定、雙指縮放數學全部是 QPointF/QSizeF 上的運算，走 QTEST_GUILESS_MAIN。
set(ALIOTH_TEST_LIBS Alioth::touch)
alioth_add_test(test_touch_gestures test_touch_gestures.cpp)
