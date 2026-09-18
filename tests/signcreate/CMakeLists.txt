# 建立數位簽章的測試（WBS 6.11，PRD-SIG-004 / 005 / 006 / 007）。
#
# 憑證語料沿用 tests/signature/signature_fixture.h，因此需要 OpenSSL 與
# 上層目錄的 include path；qpdf 的探索邏輯與 tests/qa/CMakeLists.txt 各自
# 獨立一份——ALIOTH_QPDF_BIN 是 tests/qa 的目錄區域變數，不會傳到這裡。

set(OPENSSL_ROOT_DIR "C:/vcpkg/installed/x64-windows" CACHE PATH "OpenSSL 安裝位置")
find_package(OpenSSL QUIET)

if(OpenSSL_FOUND)
    set(ALIOTH_SIGNCREATE_OPENSSL_LIBS OpenSSL::Crypto)
    set(ALIOTH_SIGNCREATE_OPENSSL_INCLUDE "")
else()
    find_library(ALIOTH_SIGNCREATE_LIBCRYPTO
        NAMES libcrypto crypto
        PATHS "${OPENSSL_ROOT_DIR}/lib"
        NO_DEFAULT_PATH
    )
    if(NOT ALIOTH_SIGNCREATE_LIBCRYPTO)
        message(FATAL_ERROR "找不到 OpenSSL 的 libcrypto，簽章建立測試無法建置。")
    endif()
    set(ALIOTH_SIGNCREATE_OPENSSL_LIBS "${ALIOTH_SIGNCREATE_LIBCRYPTO}")
    set(ALIOTH_SIGNCREATE_OPENSSL_INCLUDE "${OPENSSL_ROOT_DIR}/include")
endif()

set(ALIOTH_SIGNCREATE_QPDF_BIN "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(NOT EXISTS "${ALIOTH_SIGNCREATE_QPDF_BIN}")
    find_program(ALIOTH_SIGNCREATE_QPDF_BIN NAMES qpdf)
endif()
if(NOT ALIOTH_SIGNCREATE_QPDF_BIN OR NOT EXISTS "${ALIOTH_SIGNCREATE_QPDF_BIN}")
    message(WARNING
        "找不到 qpdf，test_signature_creator 的結構檢查段落將在執行期 skip。"
        " 執行 tools/qpdf/fetch_qpdf.bat 取得後重新 configure。")
    set(ALIOTH_SIGNCREATE_QPDF_BIN "")
endif()

set(ALIOTH_TEST_LIBS ${ALIOTH_TEST_LIBS} Alioth::signature Alioth::objects)

alioth_add_test(test_signature_creator test_signature_creator.cpp)
target_include_directories(test_signature_creator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..            # pdf_fixture.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../signature   # signature_fixture.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../qa          # qpdf_check.h
    ${ALIOTH_SIGNCREATE_OPENSSL_INCLUDE}
)
target_link_libraries(test_signature_creator PRIVATE ${ALIOTH_SIGNCREATE_OPENSSL_LIBS})
if(ALIOTH_SIGNCREATE_QPDF_BIN)
    target_compile_definitions(test_signature_creator PRIVATE
        ALIOTH_QPDF_EXECUTABLE="${ALIOTH_SIGNCREATE_QPDF_BIN}")
endif()

# OpenSSL 與 PDFium 都是動態庫，執行檔旁邊需要各自的 DLL，否則退出碼是
# 0xc0000135——看起來像崩潰，實際只是找不到 DLL。
add_custom_command(TARGET test_signature_creator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll" "$<TARGET_FILE_DIR:test_signature_creator>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:test_signature_creator>"
)

# 清除所有簽章欄位（PDF-XChange 的 Clear all Signatures）。純物件層，
# 不需要 PDFium 也不需要 OpenSSL——語料用未簽署的簽章欄位範本即可。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::signature Alioth::objects)
alioth_add_test(test_signature_clear test_signature_clear.cpp)
target_include_directories(test_signature_clear PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../signature
    ${ALIOTH_SIGNCREATE_OPENSSL_INCLUDE}
)
target_link_libraries(test_signature_clear PRIVATE ${ALIOTH_SIGNCREATE_OPENSSL_LIBS})
set_property(TEST test_signature_clear APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_custom_command(TARGET test_signature_clear POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll" "$<TARGET_FILE_DIR:test_signature_clear>"
)

# 應用層的簽署入口（錯誤處理與不留半寫檔案）。密碼學本身由上面那支測試守。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app Alioth::signature Alioth::objects)
alioth_add_test(test_signing_service test_signing_service.cpp)
target_include_directories(test_signing_service PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../objects)
target_link_libraries(test_signing_service PRIVATE ${ALIOTH_SIGNCREATE_OPENSSL_LIBS})
set_property(TEST test_signing_service APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
add_custom_command(TARGET test_signing_service POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll" "$<TARGET_FILE_DIR:test_signing_service>"
)
