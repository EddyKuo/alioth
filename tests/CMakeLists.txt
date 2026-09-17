# 單元測試。領域層與引擎轉接層覆蓋率門檻 ≥ 80%（PRD §8.4）。
#
# 測試框架用 Qt Test 而非 PRD §9 所寫的 GoogleTest/Catch2：Qt 已是必要相依，
# 用它可以少一個第三方元件，符合「相依總數僅 3 個引擎級元件」的立場。
# 若之後渲染回歸框架需要 GoogleTest 的參數化能力，再另行評估。
find_package(Qt6 REQUIRED COMPONENTS Test)

function(alioth_add_test name)
    qt_add_executable(${name} ${ARGN})
    # 測試必須是 console 子系統，否則 QTest 的輸出會被丟掉，
    # 失敗的測試在 CI 上看起來會像是「沒訊息就通過」。
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE OFF)
    target_link_libraries(${name} PRIVATE Qt6::Test ${ALIOTH_TEST_LIBS})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name})
    get_target_property(_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT _qt_core_location)
        get_target_property(_qt_core_location Qt6::Core IMPORTED_LOCATION_DEBUG)
    endif()
    get_filename_component(_qt_bin_dir "${_qt_core_location}" DIRECTORY)
    # 測試執行檔需要 Qt 與 PDFium 的 DLL，否則退出碼是 0xc0000135；那個碼看起來像崩潰，
    # 實際只是找不到 DLL，很容易誤診成程式碼有問題。
    #
    # 這裡用 ENVIRONMENT_MODIFICATION 而不是 ENVIRONMENT：後者的值是分號分隔的清單，
    # 而 Windows 的 PATH 本身就含分號，整段塞進去會被切成一堆無效項目，
    # 結果是系統 PATH 被靜默清空——測試可能仍然通過，直到某天需要某個系統 DLL 才爆。
    #
    # QT_PLUGIN_PATH 同樣必要：windeployqt 只複製主程式用到的平台外掛，
    # 測試要用 offscreen 就找不到，症狀是 "no Qt platform plugin could be initialized"
    # 外加一個看起來像記憶體錯誤的 SegFault。
    get_filename_component(_qt_root "${_qt_bin_dir}" DIRECTORY)
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 120
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${_qt_bin_dir};PATH=path_list_prepend:${PDFium_DIR}/bin"
        ENVIRONMENT "QT_PLUGIN_PATH=${_qt_root}/plugins"
    )
endfunction()

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine)

alioth_add_test(test_csv test_csv.cpp)
alioth_add_test(test_geometry test_geometry.cpp)

# 巨集動作綱要與 Code 128 條碼編碼：純領域層邏輯，不需要 PDFium。
set(ALIOTH_TEST_LIBS Alioth::domain)
alioth_add_test(test_barcode test_barcode.cpp)
# 頁碼範圍解析：純領域邏輯，邊界條件比正常路徑重要得多。
alioth_add_test(test_page_range test_page_range.cpp)
alioth_add_test(test_macro test_macro.cpp)
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine)
alioth_add_test(test_page_layout test_page_layout.cpp)
alioth_add_test(test_horizontal_layout test_horizontal_layout.cpp)
alioth_add_test(test_tile_cache test_tile_cache.cpp)
alioth_add_test(test_pdfium_engine test_pdfium_engine.cpp pdf_fixture.h)

# 命令堆疊只依賴 app 層，不需要引擎。
set(ALIOTH_TEST_LIBS Alioth::app)
alioth_add_test(test_command_stack test_command_stack.cpp)
# 顯示品質選項（PRD-VIEW-013）。守「平滑線條」與 strokeAdjust 的反向對映，
# 以及改了選項一定要作廢舊圖磚——兩者寫錯都不會有錯誤訊息。
alioth_add_test(test_render_quality test_render_quality.cpp)
alioth_add_test(test_settings test_settings.cpp)
alioth_add_test(test_session test_session.cpp)
alioth_add_test(test_history_store test_history_store.cpp)
# 瀏覽歷史前進後退（PRD-NAV-001）。純邏輯，不需要 GUI。
alioth_add_test(test_view_history test_view_history.cpp)
# 註解列表篩選與排序（PRD-ANN-008）。純領域邏輯。
set(ALIOTH_TEST_LIBS Alioth::domain)
alioth_add_test(test_annotation_filter test_annotation_filter.cpp)
set(ALIOTH_TEST_LIBS Alioth::app)
alioth_add_test(test_form_controller test_form_controller.cpp)
alioth_add_test(test_handwritten_signature test_handwritten_signature.cpp)
alioth_add_test(test_settings_profile test_settings_profile.cpp)
alioth_add_test(test_cloud_folders test_cloud_folders.cpp)

# 檔案總管縮圖渲染器（PRD-UI-020）。這支程式碼會被載進 explorer.exe，
# 因此重點是每一條失敗路徑都安靜地失敗——崩潰會把使用者的檔案總管一起帶走。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::shellthumb)
alioth_add_test(test_shell_thumbnail test_shell_thumbnail.cpp pdf_fixture.h)
set_property(TEST test_shell_thumbnail APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine Alioth::app)
set_property(TEST test_cloud_folders APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_property(TEST test_settings_profile APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_property(TEST test_handwritten_signature APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_property(TEST test_form_controller APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_security_policy test_security_policy.cpp)
set_property(TEST test_security_policy APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_form_email test_form_email.cpp)
set_property(TEST test_form_email APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_document_email test_document_email.cpp)
set_property(TEST test_document_email APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_batch_runner test_batch_runner.cpp)
set_property(TEST test_batch_runner APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_navigation_service test_navigation_service.cpp)
set_property(TEST test_navigation_service APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_property(TEST test_history_store APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_property(TEST test_session APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_property(TEST test_settings APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_property(TEST test_render_quality APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 預建文字索引（PRD-SRCH-001，ADR-005）。與逐頁 PDFium 搜尋互相驗證：
# 2 秒／500 頁的預算由單一擷取器預建索引達成，不靠並行。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::text)
alioth_add_test(test_text_index test_text_index.cpp)
set_property(TEST test_text_index APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::text Alioth::engine)
alioth_add_test(test_pdfium_concurrency test_pdfium_concurrency.cpp)
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::text)
set_property(TEST test_pdfium_concurrency APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 並行搜尋的缺陷重現器移到 tests/diagnostics/：它會間歇性堆積毀損（0xc0000374）
# 或 SEGFAULT，那正是它要記錄的現象本身（EXC_20260905_RD_SA_parallel_search、ADR-005）。
#
# 曾經用 DISABLED TRUE 留在套件裡，但那讓 ctest 報「182 個測試」而實際只有 181 個
# 提供保護——被停用的那一個還是缺陷重現器，不是產品行為測試，永遠不會被啟用。
# 現在它只是一個可手動執行的診斷程式，而「產品不得啟用並行 PDFium 路徑」
# 這條真正該守的規則由 tools/check_layering.py 靜態檢查（CI 會跑）。
add_subdirectory(diagnostics)

# 戳記寫入需要應用層與文字擷取（讀回驗證），以及 qpdf 的結構檢查輔助。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app Alioth::text)
alioth_add_test(test_stamp_service test_stamp_service.cpp)
target_include_directories(test_stamp_service PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_stamp_service APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set(_qpdf_bin "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(EXISTS "${_qpdf_bin}")
    target_compile_definitions(test_stamp_service PRIVATE ALIOTH_QPDF_EXECUTABLE="${_qpdf_bin}")
endif()

# 連結需要引擎與應用層。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine Alioth::app)
alioth_add_test(test_links test_links.cpp)
alioth_add_test(test_page_geometry test_page_geometry.cpp)
set_property(TEST test_page_geometry APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_outline_traversal test_outline_traversal.cpp)
set_property(TEST test_outline_traversal APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_property(TEST test_links APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 核心迴圈整合測試：跨文字擷取、註解外觀、增量儲存三個子系統。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine Alioth::app Alioth::annotations)
alioth_add_test(test_highlight_flow test_highlight_flow.cpp)
set_property(TEST test_highlight_flow APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 呈現層冒煙測試需要完整的 UI 相依，並在 offscreen 平台上跑，不需要顯示器。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine Alioth::app Alioth::ui)
alioth_add_test(test_ui_smoke test_ui_smoke.cpp pdf_fixture.h)
set_property(TEST test_ui_smoke APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_annotation_properties test_annotation_properties.cpp)
set_property(TEST test_annotation_properties APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_ribbon_customize test_ribbon_customize.cpp)
set_property(TEST test_ribbon_customize APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_document_properties_dialog test_document_properties_dialog.cpp)
set_property(TEST test_document_properties_dialog APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_ribbon_wiring test_ribbon_wiring.cpp)
set_property(TEST test_ribbon_wiring APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_stamp_dialog test_stamp_dialog.cpp)
set_property(TEST test_stamp_dialog APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_sticky_note_popup test_sticky_note_popup.cpp)
set_property(TEST test_sticky_note_popup APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_links_panel test_links_panel.cpp)
set_property(TEST test_links_panel APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
alioth_add_test(test_shortcuts_page test_shortcuts_page.cpp)
set_property(TEST test_shortcuts_page APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine)

# 引擎測試會實際載入 pdfium.dll，執行檔旁邊必須有它。
add_custom_command(TARGET test_pdfium_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:test_pdfium_engine>"
)

# 各子系統的測試。alioth_add_test 在此已定義，子目錄可直接使用。
add_subdirectory(annotations)
add_subdirectory(text)
add_subdirectory(save)
add_subdirectory(pages)
add_subdirectory(print)
add_subdirectory(ribbon)
add_subdirectory(objects)
add_subdirectory(forms)
add_subdirectory(signature)
add_subdirectory(signcreate)
add_subdirectory(ltv)

# 品質關卡測試（WBS 7.3 / 7.4）：qpdf 結構檢查與基準影像回歸。
add_subdirectory(qa)
add_subdirectory(redaction)
add_subdirectory(formbuild)
add_subdirectory(compare)
add_subdirectory(bookmarks)
add_subdirectory(pageops)
add_subdirectory(enhance)
add_subdirectory(labels)
add_subdirectory(attachments)
add_subdirectory(create)
add_subdirectory(iosec)
add_subdirectory(annfamily)
add_subdirectory(viewaids)
add_subdirectory(measure)
add_subdirectory(a11y)
add_subdirectory(a11y2)
add_subdirectory(textsearch)
add_subdirectory(uisystem)
add_subdirectory(annmgmt)
add_subdirectory(touch)
add_subdirectory(externaltools)

add_subdirectory(fonts)
