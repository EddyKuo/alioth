qt_add_executable(alioth WIN32 main.cpp)

target_link_libraries(alioth PRIVATE Alioth::ui)

set_target_properties(alioth PROPERTIES
    OUTPUT_NAME "Alioth"
    WIN32_EXECUTABLE ON
)

# PDFium 是動態庫，執行檔旁邊必須有 pdfium.dll。
add_custom_command(TARGET alioth POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:alioth>"
    COMMENT "複製 pdfium.dll 到輸出目錄"
)

# 隨附的 CJK 字型（ADR-007）。放在執行檔旁邊的 fonts/，與 pdfium.dll 同一個作法。
#
# 優先使用隨附的那一份而不是系統字型，理由不是效能而是**可預測性**：
# 使用者機器上的同名字型可能是不同版本、不同字重，甚至是同名的別家字型，
# 而內嵌進 PDF 的必須是我們驗證過的那一份。
#
# 檔案不存在時只警告不失敗：開發時仍可用系統字型跑，但那是降級狀態，
# 產出的 PDF 在沒有該字型的機器上會缺字。
set(ALIOTH_BUNDLED_CJK_FONT "${CMAKE_SOURCE_DIR}/third_party/fonts/NotoSansTC-Regular.ttf")
if(EXISTS "${ALIOTH_BUNDLED_CJK_FONT}")
    add_custom_command(TARGET alioth POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:alioth>/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ALIOTH_BUNDLED_CJK_FONT}" "$<TARGET_FILE_DIR:alioth>/fonts/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/third_party/fonts/OFL.txt"
                "$<TARGET_FILE_DIR:alioth>/fonts/"
        COMMENT "複製隨附的 CJK 字型與其授權到輸出目錄"
    )
else()
    message(WARNING
        "找不到隨附的 CJK 字型 ${ALIOTH_BUNDLED_CJK_FONT}。"
        "程式仍可建置，但會退回系統字型——那是降級狀態，"
        "產出的 PDF 在沒有該字型的機器上會缺字。取得方式見 CLAUDE.md。")
endif()

# 開發便利：把 Qt 執行期相依部署到輸出目錄，讓 Alioth.exe 可以直接雙擊執行。
# 正式安裝包另見 WBS 8.1。
if(WIN32)
    find_program(ALIOTH_WINDEPLOYQT windeployqt HINTS "${QT6_INSTALL_PREFIX}/bin"
                 "$<TARGET_FILE_DIR:Qt6::qmake>")
    if(ALIOTH_WINDEPLOYQT)
        add_custom_command(TARGET alioth POST_BUILD
            COMMAND "${ALIOTH_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler
                    --no-opengl-sw "$<TARGET_FILE:alioth>"
            COMMENT "部署 Qt 執行期相依"
        )
    endif()
endif()
