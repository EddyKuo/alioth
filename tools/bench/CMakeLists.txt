# 效能基準工具。console 子系統，輸出 JSON 供 CI 比對基準線。
add_executable(alioth_bench main.cpp)

# Alioth::text 是全文搜尋（PRD §8.1 ≤ 2 秒 / 500 頁），
# Alioth::platform 是記憶體量測——作業系統呼叫只能出現在平台層，
# 本工具透過 platform/process_metrics.h 取用，不自行 include <psapi.h>。
target_link_libraries(alioth_bench PRIVATE
    Alioth::domain Alioth::engine Alioth::text Alioth::platform)

# 工程圖語料產生器與 tests/qa 共用同一份程式碼：基準工具量的語料與
# 渲染回歸比對的語料若是兩份實作，兩邊的結論就無法互相印證。
target_include_directories(alioth_bench PRIVATE ${CMAKE_SOURCE_DIR}/tools/corpus)

set_target_properties(alioth_bench PROPERTIES WIN32_EXECUTABLE OFF)

add_custom_command(TARGET alioth_bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PDFium_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:alioth_bench>"
)
