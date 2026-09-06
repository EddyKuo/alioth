# 平台層：作業系統差異的唯一收斂點。
# 專案目前只出 Windows，但其他四層不得出現 #ifdef _WIN32——移植成本全靠這條規則壓住。
add_library(alioth_platform STATIC
    font_paths.cpp
    font_paths.h
    cloud_folders.cpp
    cloud_folders.h
    paths.cpp
    shared_file.cpp
    shared_file.h
    paths.h
    atomic_file.cpp
    atomic_file.h
    process_metrics.cpp
    process_metrics.h
    external_change.cpp
    external_change.h
    email_compose.cpp
    email_compose.h
    external_process.cpp
    external_process.h
    speech_synthesis.cpp
    speech_synthesis.h
)

target_link_libraries(alioth_platform PUBLIC Alioth::domain Qt6::Core)

# SAPI（朗讀，PRD-A11Y-006）是 COM 元件，只在 Windows 建置時才需要這幾個
# 系統匯入庫；#ifdef _WIN32 已經把介面收斂成同一份標頭，這裡只需要多連結。
if(WIN32)
    target_link_libraries(alioth_platform PRIVATE ole32 oleaut32 sapi)
endif()

add_library(Alioth::platform ALIAS alioth_platform)
