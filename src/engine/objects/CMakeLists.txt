# PDF 物件層寫入通道（ADR-002、WBS 4.2 / 4.14 / 5.1 / 5.11）。
#
# 刻意**不連結 PDFium**：這一層的存在理由正是 PDFium 的寫入 API 不讓我們控制
# 物件層。輸入是位元組與領域模型，輸出是位元組，因此可以逐位元組測到底。
#
# 連 Qt6::Core 只為了 zlib（qUncompress）：xref 串流與物件串流一律是
# FlateDecode，而專案的相依上限是三個引擎級元件，不能為了解壓再加一個。
add_library(alioth_objects STATIC
    pdf_object.cpp
    pdf_object.h
    pdf_parser.cpp
    pdf_parser.h
    pdf_source_document.cpp
    pdf_source_document.h
    incremental_appender.cpp
    incremental_appender.h
    page_object_editor.cpp
    page_object_editor.h
    annotation_object_writer.cpp
    annotation_object_writer.h
    content_stream_appender.cpp
    content_stream_appender.h
    measure_reader.cpp
    measure_reader.h
    annotation_note_writer.cpp
    annotation_note_writer.h
    annotation_reader.cpp
    annotation_reader.h
    measurement_writer.cpp
    measurement_writer.h
    space_audit.cpp
    space_audit.h
    struct_tree_reader.cpp
    struct_tree_reader.h
    reading_order.cpp
    reading_order.h
    reading_text_extractor.cpp
    reading_text_extractor.h
    struct_alt_text_writer.cpp
    struct_alt_text_writer.h
    accessibility_checker.cpp
    accessibility_checker.h
    annotation_flattener.cpp
    annotation_flattener.h
)

target_link_libraries(alioth_objects
    # CJK 內嵌字型（ADR-007）。alioth_fonts 不連結 pdfium，因此這不破壞
    # 「物件層不碰 PDFium」的立場。
    PUBLIC Alioth::fonts
    PUBLIC Alioth::domain Alioth::annotations
    PRIVATE Qt6::Core
)

add_library(Alioth::objects ALIAS alioth_objects)
