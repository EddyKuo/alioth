# 引擎轉接層：全專案唯一可以連結 PDFium 的目標。
# 若有其他 target 出現 target_link_libraries(... pdfium)，那是分層違規。
# PDFium 全域初始化。獨立成 target，讓各個 PDFium 子系統共用同一份引用計數，
# 而不是各自初始化一次。
add_library(alioth_pdfium_runtime STATIC
    pdfium_library.cpp
    pdfium_library.h
    pdfium_lock.cpp
    pdfium_lock.h
)
target_include_directories(alioth_pdfium_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(alioth_pdfium_runtime PRIVATE pdfium)
add_library(Alioth::pdfium_runtime ALIAS alioth_pdfium_runtime)

add_library(alioth_engine STATIC
    file_source.cpp
    file_source.h
    pdfium_engine.cpp
    pdfium_engine.h
    pixel_buffer.cpp
    pixel_buffer.h
    tile_cache.cpp
    tile_cache.h
    cancellation.h
)

target_link_libraries(alioth_engine
    PUBLIC Alioth::domain
    PRIVATE pdfium Alioth::pdfium_runtime Alioth::platform
)

add_library(Alioth::engine ALIAS alioth_engine)

# 引擎子系統。各自獨立成 target，讓不同工作包可以並行推進而不互相踩到。
add_subdirectory(annotations)
add_subdirectory(pages)
add_subdirectory(objects)
add_subdirectory(forms)
add_subdirectory(signature)
add_subdirectory(redaction)
add_subdirectory(formbuild)
add_subdirectory(compare)
add_subdirectory(bookmarks)
add_subdirectory(layers)
add_subdirectory(pageops)
add_subdirectory(enhance)
add_subdirectory(labels)
add_subdirectory(attachments)
add_subdirectory(shellthumb)
add_subdirectory(create)
add_subdirectory(text)
add_subdirectory(save)
add_subdirectory(iosec)

# PDFium 是動態庫，執行期需要 pdfium.dll 在旁邊。
set(ALIOTH_PDFIUM_RUNTIME "${PDFium_DIR}/bin/pdfium.dll" CACHE INTERNAL "")

add_subdirectory(fonts)
