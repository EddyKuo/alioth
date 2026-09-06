# 檔案輸出入與文件安全測試（WP27，PRD-IO-002/004/007、PRD-SEC-002、PRD-SIG-003）。

# 最佳化／另存新檔：只需要 save 與 objects（驗孤兒物件真的被清掉）。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine Alioth::save Alioth::objects)
alioth_add_test(test_optimize_saver test_optimize_saver.cpp)
target_include_directories(test_optimize_saver PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set(_qpdf_bin "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(EXISTS "${_qpdf_bin}")
    target_compile_definitions(test_optimize_saver PRIVATE ALIOTH_QPDF_EXECUTABLE="${_qpdf_bin}")
endif()

# 密碼與權限：另外需要 OpenSSL（自建 RC4 加密語料）。做法與 tests/signature 相同，
# 這裡重複一次尋找邏輯而不是沿用 src 的變數，讓測試目錄能單獨設定成功。
set(OPENSSL_ROOT_DIR "C:/vcpkg/installed/x64-windows" CACHE PATH "OpenSSL 安裝位置")
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    set(ALIOTH_IOSEC_OPENSSL_LIBS OpenSSL::Crypto)
    set(ALIOTH_IOSEC_OPENSSL_INCLUDE "")
else()
    find_library(ALIOTH_IOSEC_LIBCRYPTO
        NAMES libcrypto crypto
        PATHS "${OPENSSL_ROOT_DIR}/lib"
        NO_DEFAULT_PATH
    )
    if(NOT ALIOTH_IOSEC_LIBCRYPTO)
        message(FATAL_ERROR "找不到 OpenSSL 的 libcrypto，iosec 測試無法建置。")
    endif()
    set(ALIOTH_IOSEC_OPENSSL_LIBS "${ALIOTH_IOSEC_LIBCRYPTO}")
    set(ALIOTH_IOSEC_OPENSSL_INCLUDE "${OPENSSL_ROOT_DIR}/include")
endif()

set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine Alioth::save)
alioth_add_test(test_security_saver test_security_saver.cpp encrypted_fixture.h)
target_include_directories(test_security_saver PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_security_saver PRIVATE ${ALIOTH_IOSEC_OPENSSL_LIBS})
if(ALIOTH_IOSEC_OPENSSL_INCLUDE)
    target_include_directories(test_security_saver PRIVATE ${ALIOTH_IOSEC_OPENSSL_INCLUDE})
endif()
add_custom_command(TARGET test_security_saver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll" "$<TARGET_FILE_DIR:test_security_saver>"
)

# 外部變更偵測：純平台層，不需要 PDFium 也不需要 OpenSSL。
set(ALIOTH_TEST_LIBS Alioth::platform)
alioth_add_test(test_external_change test_external_change.cpp)

# 匯出影像：需要 iosec 本身（含 Qt 影像外掛相依）。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::iosec)
alioth_add_test(test_page_image_export test_page_image_export.cpp)
# alioth_iosec 對 Qt6::Gui 是 PRIVATE（公開標頭不外洩 QImage），但測試本身
# 直接用 QImage/QImageReader 讀回驗證像素，因此測試目標自己另外連 Qt6::Gui。
target_link_libraries(test_page_image_export PRIVATE Qt6::Gui)
target_include_directories(test_page_image_export PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_page_image_export APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 匯出純文字：需要 iosec 與文字子系統。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::iosec Alioth::text)
alioth_add_test(test_text_export test_text_export.cpp)
target_include_directories(test_text_export PRIVATE ${CMAKE_SOURCE_DIR}/tests)
set_property(TEST test_text_export APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# SIG-003 端到端：核心迴圈（app 層）+ 簽章驗證，兩者都要 OpenSSL 造語料。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::engine Alioth::app Alioth::annotations Alioth::signature)
alioth_add_test(test_signed_incremental_save test_signed_incremental_save.cpp)
target_include_directories(test_signed_incremental_save PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_signed_incremental_save PRIVATE ${ALIOTH_IOSEC_OPENSSL_LIBS})
if(ALIOTH_IOSEC_OPENSSL_INCLUDE)
    target_include_directories(test_signed_incremental_save PRIVATE ${ALIOTH_IOSEC_OPENSSL_INCLUDE})
endif()
add_custom_command(TARGET test_signed_incremental_save POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll"
            "$<TARGET_FILE_DIR:test_signed_incremental_save>"
)
set_property(TEST test_signed_incremental_save APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 這幾支會實際載入 pdfium.dll，執行檔旁邊必須有它，否則退出碼是 0xc0000135。
foreach(_target test_optimize_saver test_security_saver test_page_image_export
                test_text_export test_signed_incremental_save)
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:${_target}>"
    )
endforeach()
