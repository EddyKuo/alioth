# 增量儲存器與自動儲存（WBS 5.1 / 5.3）。
# 屬於引擎轉接層，因此可以連結 PDFium；原子更名與落盤同步一律轉給平台層。
add_library(alioth_save STATIC
    incremental_saver.cpp
    incremental_saver.h
    autosave.cpp
    autosave.h
    save_testing.cpp
    save_testing.h
    optimize_saver.cpp
    optimize_saver.h
    security_saver.cpp
    security_saver.h
)

target_include_directories(alioth_save PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(alioth_save
    PUBLIC Alioth::domain
    PRIVATE pdfium Alioth::pdfium_runtime Alioth::platform Alioth::objects Qt6::Core
)

add_library(Alioth::save ALIAS alioth_save)
