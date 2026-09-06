# 檢視輔助（WP26）：圖層面板、尺規／參考線、格線貼齊、區域縮放、Loupe、
# Pan & Zoom 面板。純邏輯核心測試，不需要 offscreen 平台外掛
# （這裡沒有任何 QWidget 被建構）。
set(ALIOTH_TEST_LIBS Alioth::domain)
alioth_add_test(test_ocg test_ocg.cpp)
alioth_add_test(test_guides test_guides.cpp)
alioth_add_test(test_zoom_aids test_zoom_aids.cpp)
alioth_add_test(test_presentation test_presentation.cpp)

# OCG 讀取需要我們自己的物件剖析器（不需要 PDFium）。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::layers)
alioth_add_test(test_ocg_reader test_ocg_reader.cpp)

# 圖層攤平（PRD-VIEW-014）：驗收要靠真的渲染回讀，所以需要 Alioth::engine
# （PdfiumEngine），不能只驗我們自己剖析器讀回來的位元組。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::layers Alioth::engine)
alioth_add_test(test_ocg_flatten test_ocg_flatten.cpp)
