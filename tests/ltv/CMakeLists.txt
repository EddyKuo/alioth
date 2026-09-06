# 長期驗證與時間戳的測試（WBS 6.11，PRD-SIG-005 / 006）。
#
# 憑證與 CMS 語料沿用 tests/signature/signature_fixture.h（與
# tests/signcreate 相同做法），因此需要 OpenSSL 與上層目錄的 include path；
# qpdf 的探索邏輯與 tests/qa、tests/signcreate 各自獨立一份區域變數，
# 不會互相污染。
#
# CI 不能依賴網路：所有時間戳測試都透過 timestamp_test_server.h 在本地產生
# TimeStampResp，沒有任何測試會連真實 TSA。

set(OPENSSL_ROOT_DIR "C:/vcpkg/installed/x64-windows" CACHE PATH "OpenSSL 安裝位置")
find_package(OpenSSL QUIET)

if(OpenSSL_FOUND)
    set(ALIOTH_LTV_OPENSSL_LIBS OpenSSL::Crypto)
    set(ALIOTH_LTV_OPENSSL_INCLUDE "")
else()
    find_library(ALIOTH_LTV_LIBCRYPTO
        NAMES libcrypto crypto
        PATHS "${OPENSSL_ROOT_DIR}/lib"
        NO_DEFAULT_PATH
    )
    if(NOT ALIOTH_LTV_LIBCRYPTO)
        message(FATAL_ERROR "找不到 OpenSSL 的 libcrypto，LTV 測試無法建置。")
    endif()
    set(ALIOTH_LTV_OPENSSL_LIBS "${ALIOTH_LTV_LIBCRYPTO}")
    set(ALIOTH_LTV_OPENSSL_INCLUDE "${OPENSSL_ROOT_DIR}/include")
endif()

set(ALIOTH_LTV_QPDF_BIN "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(NOT EXISTS "${ALIOTH_LTV_QPDF_BIN}")
    find_program(ALIOTH_LTV_QPDF_BIN NAMES qpdf)
endif()
if(NOT ALIOTH_LTV_QPDF_BIN OR NOT EXISTS "${ALIOTH_LTV_QPDF_BIN}")
    message(WARNING
        "找不到 qpdf，tests/ltv 的結構檢查段落將在執行期 skip。"
        " 執行 tools/qpdf/fetch_qpdf.bat 取得後重新 configure。")
    set(ALIOTH_LTV_QPDF_BIN "")
endif()

set(ALIOTH_TEST_LIBS ${ALIOTH_TEST_LIBS} Alioth::signature Alioth::objects)

function(alioth_ltv_test name)
    alioth_add_test(${name} ${ARGN})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..            # pdf_fixture.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../signature   # signature_fixture.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../qa          # qpdf_check.h
        ${ALIOTH_LTV_OPENSSL_INCLUDE}
    )
    target_link_libraries(${name} PRIVATE ${ALIOTH_LTV_OPENSSL_LIBS})
    if(ALIOTH_LTV_QPDF_BIN)
        target_compile_definitions(${name} PRIVATE ALIOTH_QPDF_EXECUTABLE="${ALIOTH_LTV_QPDF_BIN}")
    endif()
    # OpenSSL 與 PDFium 都是動態庫，執行檔旁邊需要各自的 DLL，否則退出碼是
    # 0xc0000135——看起來像崩潰，實際只是找不到 DLL。
    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll" "$<TARGET_FILE_DIR:${name}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:${name}>"
    )
endfunction()

alioth_ltv_test(test_timestamp_client test_timestamp_client.cpp timestamp_test_server.h)
alioth_ltv_test(test_dss_builder test_dss_builder.cpp)
alioth_ltv_test(test_pades_t test_pades_t.cpp timestamp_test_server.h)
alioth_ltv_test(test_batch_sign test_batch_sign.cpp)

# 可見簽章外觀（PRD-SIG-004 的補完，PRD-SIG-005 的前置條件，見 ADR-004）。
# 純函數，不需要 PDFium 也不需要 OpenSSL。
alioth_add_test(test_visible_signature test_visible_signature.cpp)
