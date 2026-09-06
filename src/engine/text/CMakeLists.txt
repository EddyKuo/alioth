# 文字擷取與全文搜尋（WBS 2.8 / 2.9）。
#
# 獨立成 target 是為了讓文字子系統能與渲染子系統並行推進；
# 它同樣屬於引擎轉接層，因此也是少數可以連結 PDFium 的 target 之一。
add_library(alioth_text STATIC
    text_extractor.cpp
    text_extractor.h
    text_search.cpp
    text_search.h
    text_index.cpp
    text_index.h
    parallel_search.cpp
    parallel_search.h
    text_encoding.h
    bounded_regex.cpp
    bounded_regex.h
    multi_document_search.cpp
    multi_document_search.h
)

target_include_directories(alioth_text PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(alioth_text
    PUBLIC Alioth::domain
    PRIVATE pdfium Alioth::pdfium_runtime Alioth::engine
)

add_library(Alioth::text ALIAS alioth_text)
