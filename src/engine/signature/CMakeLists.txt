# 簽章驗證子系統（WBS 6.5–6.7，PRD-SIG-001 / 002）。
#
# 這是全專案唯一連結 OpenSSL 的地方。PRD §4.1 保留 OpenSSL 的理由是
# 「三平台簽章驗證行為一致」，若哪天有第二個 target 連上它，那個承諾就開始鬆動。

set(OPENSSL_ROOT_DIR "C:/vcpkg/installed/x64-windows" CACHE PATH "OpenSSL 安裝位置")
set(OPENSSL_USE_STATIC_LIBS OFF)
find_package(OpenSSL QUIET)

if(OpenSSL_FOUND)
    set(ALIOTH_OPENSSL_LIBS OpenSSL::Crypto)
    set(ALIOTH_OPENSSL_INCLUDE "")
else()
    # find_package 失敗時直接指向匯入庫。能建置起來比漂亮重要——
    # 找不到 OpenSSL 就等於整個 WP6 簽章包無法驗證，那是比醜陋的 CMake 更大的問題。
    find_library(ALIOTH_LIBCRYPTO
        NAMES libcrypto crypto
        PATHS "${OPENSSL_ROOT_DIR}/lib"
        NO_DEFAULT_PATH
    )
    if(NOT ALIOTH_LIBCRYPTO)
        message(FATAL_ERROR
            "找不到 OpenSSL 的 libcrypto。請確認 ${OPENSSL_ROOT_DIR}/lib 下有 libcrypto.lib，"
            "或以 -DOPENSSL_ROOT_DIR=<路徑> 指定。")
    endif()
    set(ALIOTH_OPENSSL_LIBS "${ALIOTH_LIBCRYPTO}")
    set(ALIOTH_OPENSSL_INCLUDE "${OPENSSL_ROOT_DIR}/include")
endif()

add_library(alioth_signature STATIC
    signature_appearance.cpp
    signature_appearance.h
    byte_range.cpp
    byte_range.h
    signature_types.cpp
    signature_types.h
    trust_store.cpp
    trust_store.h
    revocation.cpp
    revocation.h
    pkcs7_verifier.cpp
    pkcs7_verifier.h
    signature_scanner.cpp
    signature_scanner.h
    signing_identity.cpp
    signing_identity.h
    signature_clear.cpp
    signature_clear.h
    signature_creator.cpp
    signature_creator.h
    timestamp_client.cpp
    timestamp_client.h
    dss_builder.cpp
    dss_builder.h
)

target_include_directories(alioth_signature PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
if(ALIOTH_OPENSSL_INCLUDE)
    target_include_directories(alioth_signature PRIVATE ${ALIOTH_OPENSSL_INCLUDE})
endif()

target_link_libraries(alioth_signature
    PUBLIC Alioth::domain
    PRIVATE pdfium Alioth::pdfium_runtime Alioth::engine Alioth::platform Alioth::objects
            ${ALIOTH_OPENSSL_LIBS}
)

add_library(Alioth::signature ALIAS alioth_signature)

# OpenSSL 是動態庫，測試與主程式的執行檔旁邊需要它。路徑往上傳，讓測試目錄能用。
set(ALIOTH_OPENSSL_RUNTIME_DIR "${OPENSSL_ROOT_DIR}/bin" CACHE INTERNAL "OpenSSL DLL 位置")
