# 多語系建置流程（PRD-UI-005）。
#
# .ts 是設計期產物：來源字面量（繁中）掃描自 src/ 的 tr() 呼叫，用
#   lupdate -no-obsolete -source-language zh_TW -target-language en -recursive src -ts i18n/alioth_en.ts
# 手動重新產生。-recursive src 而不是逐一列檔案：列表一定會漏，而漏掉的檔案
# 不會有任何錯誤訊息，只會讓那些字串永遠停在中文。-no-obsolete 則是不留下
# 已經從程式碼移除的訊息——留著它們會讓覆蓋率的分母失真。
#
# 漏跑 lupdate 的後果由 tests/uisystem/test_locale_manager.cpp 的
# 「英文覆蓋率必須是 100%」守著：新增 tr() 之後沒補翻譯，下次跑那支測試就會紅。
# 手動重新產生（不在建置時自動跑 lupdate——那會讓建置行為依賴當時 src/ 的
# 完整內容與掃描結果，在多個 agent 平行修改原始碼時特別不穩定）。.ts 因此
# 和一般原始碼一樣進版控，變更可以在 code review 時看見。
#
# .qm 是執行期產物，本檔在建置時用 lrelease 把 .ts 編成 .qm 並放進建置輸出
# 目錄下的 i18n/ 子目錄——LocaleManager::switchTo() 的 qmDirectory 參數即指向
# 這裡；alioth 主程式的最終安裝包如何擺放 .qm 屬於 WBS 8.1 安裝包工作，
# 這裡只保證「建置後找得到 .qm」。
find_program(ALIOTH_LRELEASE lrelease HINTS "${QT6_INSTALL_PREFIX}/bin")

if(ALIOTH_LRELEASE)
    set(_ts_files "${CMAKE_CURRENT_SOURCE_DIR}/alioth_en.ts")
    set(_qm_dir "${CMAKE_BINARY_DIR}/i18n")
    set(_qm_files "${_qm_dir}/alioth_en.qm")

    add_custom_command(
        OUTPUT "${_qm_files}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_qm_dir}"
        COMMAND "${ALIOTH_LRELEASE}" "${_ts_files}" -qm "${_qm_files}"
        DEPENDS "${_ts_files}"
        COMMENT "lrelease：編譯 .ts 為 .qm（PRD-UI-005）"
        VERBATIM
    )
    add_custom_target(alioth_translations ALL DEPENDS "${_qm_files}")
else()
    message(WARNING "找不到 lrelease，i18n/*.qm 不會被產生——多語系切換到英文會失敗（見 locale_manager.h 的退回原文機制，UI 仍可運作但沒有翻譯）")
endif()
