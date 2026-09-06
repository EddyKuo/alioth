# TrueType 子集化（ADR-007）。純位元組層，不需要 PDFium 也不需要 GUI。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::fonts)
alioth_add_test(test_truetype_subset test_truetype_subset.cpp)

# PDF 內嵌 CID 字型。輸出要通得過 qpdf --check：我們自己的序列化器認為對的
# 東西，不代表別人讀得進去。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::fonts Alioth::objects)
alioth_add_test(test_cid_font_writer test_cid_font_writer.cpp)
target_include_directories(test_cid_font_writer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(ALIOTH_FONTS_QPDF_BIN "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(NOT EXISTS "${ALIOTH_FONTS_QPDF_BIN}")
    find_program(ALIOTH_FONTS_QPDF_BIN NAMES qpdf)
endif()
if(ALIOTH_FONTS_QPDF_BIN AND EXISTS "${ALIOTH_FONTS_QPDF_BIN}")
    target_compile_definitions(test_cid_font_writer PRIVATE
        ALIOTH_QPDF_EXECUTABLE="${ALIOTH_FONTS_QPDF_BIN}")
else()
    target_compile_definitions(test_cid_font_writer PRIVATE ALIOTH_QPDF_EXECUTABLE="")
endif()

# 字型來源選擇（ADR-007）。守的是「內嵌的必須是我們隨附並驗證過的那一份」——
# 退回系統字型是可接受的降級，但不能靜默取代隨附字型。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::fonts Alioth::platform)
alioth_add_test(test_font_paths test_font_paths.cpp)
