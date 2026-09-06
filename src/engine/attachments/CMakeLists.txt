# 附件的讀取（PRD-ANN-015）。
#
# 不連結 PDFium：FPDFAttachment_* 只看得到文件層的 /Names /EmbeddedFiles，
# 讀不到檔案附件註解攜帶的那一份，也拿不到 /Params 的日期。兩種附件都要列出，
# 否則使用者會在另一種存在時以為文件沒有附件。
add_library(alioth_attachments STATIC
    attachment_reader.cpp
    attachment_reader.h
    attachment_writer.cpp
    media_annotations.cpp
    media_annotations.h
    attachment_writer.h
)

target_include_directories(alioth_attachments PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(alioth_attachments
    PUBLIC Alioth::domain Alioth::objects
)

add_library(Alioth::attachments ALIAS alioth_attachments)
