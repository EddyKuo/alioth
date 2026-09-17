# 簽章驗證子系統測試（WBS 6.5–6.7）。
#
# 測試也需要 OpenSSL：可控的簽章語料只能自己用同一套密碼學函式庫造出來。
# 這裡重複一次尋找邏輯而不是沿用 src 的變數，讓測試目錄能單獨設定成功。

set(OPENSSL_ROOT_DIR "C:/vcpkg/installed/x64-windows" CACHE PATH "OpenSSL 安裝位置")
find_package(OpenSSL QUIET)

if(OpenSSL_FOUND)
    set(ALIOTH_TEST_OPENSSL_LIBS OpenSSL::Crypto)
    set(ALIOTH_TEST_OPENSSL_INCLUDE "")
else()
    find_library(ALIOTH_TEST_LIBCRYPTO
        NAMES libcrypto crypto
        PATHS "${OPENSSL_ROOT_DIR}/lib"
        NO_DEFAULT_PATH
    )
    if(NOT ALIOTH_TEST_LIBCRYPTO)
        message(FATAL_ERROR "找不到 OpenSSL 的 libcrypto，簽章測試無法建置。")
    endif()
    set(ALIOTH_TEST_OPENSSL_LIBS "${ALIOTH_TEST_LIBCRYPTO}")
    set(ALIOTH_TEST_OPENSSL_INCLUDE "${OPENSSL_ROOT_DIR}/include")
endif()

set(ALIOTH_TEST_LIBS ${ALIOTH_TEST_LIBS} Alioth::signature)

# /ByteRange 與三態分類是純函數，不需要 OpenSSL 也不需要 PDFium。
alioth_add_test(test_byte_range test_byte_range.cpp)

alioth_add_test(test_pkcs7_verify test_pkcs7_verify.cpp signature_fixture.h)
alioth_add_test(test_signature_scanner test_signature_scanner.cpp signature_fixture.h)

foreach(_target test_pkcs7_verify test_signature_scanner)
    target_link_libraries(${_target} PRIVATE ${ALIOTH_TEST_OPENSSL_LIBS})
    if(ALIOTH_TEST_OPENSSL_INCLUDE)
        target_include_directories(${_target} PRIVATE ${ALIOTH_TEST_OPENSSL_INCLUDE})
    endif()
    # OpenSSL 是動態庫。alioth_add_test 只把 Qt 與 PDFium 的 bin 目錄注入測試環境，
    # 少了 libcrypto-3-x64.dll 的症狀是退出碼 0xc0000135——看起來像崩潰，
    # 實際只是找不到 DLL。直接複製到執行檔旁邊最不容易出錯。
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll" "$<TARGET_FILE_DIR:${_target}>"
    )
endforeach()

# 端到端測試會實際載入 pdfium.dll 並共用上層的 pdf_fixture.h。
target_include_directories(test_signature_scanner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_custom_command(TARGET test_signature_scanner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:test_signature_scanner>"
)

# 簽章面板（PRD-SIG-002）。守的是「狀態不能只靠顏色傳達」——
# 那條規則沒有任何機制在看著，某天被「簡化」掉也不會有測試變紅。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app Alioth::ui)
alioth_add_test(test_signature_panel test_signature_panel.cpp)
set_property(TEST test_signature_panel APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 簽署對話框的欄位狀態（PRD-SIG-006）。守的是「做不到的功能不可以長得像做得到」。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app Alioth::ui Alioth::signature)
alioth_add_test(test_sign_dialog test_sign_dialog.cpp)
set_property(TEST test_sign_dialog APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
