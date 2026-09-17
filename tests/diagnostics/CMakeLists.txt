# 診斷程式：建置得起來，但不登錄成 CTest 測試。
#
# 這裡放的是「會壞給你看」的重現器。它們記錄的是已經定案的架構結論
# （ADR-005：PDFium 可並存、不可並行），不是產品行為，所以不該出現在
# 日常測試套件裡——不論是紅的還是被停用的。兩種都會侵蝕套件的可信度：
# 前者讓人習慣忽略紅燈，後者讓測試數比實際保護範圍多。
#
# 仍然編譯它們，是為了讓它們跟著 API 變更一起壞掉——不編譯的程式碼會
# 在幾次重構之後變成無法執行的文字，那時就再也重現不了任何東西。
#
# 手動執行（需要 Qt 與 PDFium 的 DLL 在 PATH 上，與一般測試執行檔同一個目錄）：
#   out\build\windows-x64-debug\test_parallel_search.exe

function(alioth_add_diagnostic name)
    qt_add_executable(${name} ${ARGN})
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE OFF EXCLUDE_FROM_ALL FALSE)
    target_link_libraries(${name} PRIVATE Qt6::Test Alioth::domain Alioth::text)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # 刻意沒有 add_test()。
endfunction()

alioth_add_diagnostic(test_parallel_search test_parallel_search.cpp)
