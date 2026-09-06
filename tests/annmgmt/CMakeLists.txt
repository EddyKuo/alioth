# WP28:註解管理與其餘工具的純邏輯核心測試(PRD-ANN-003/006/007/011/012/013/020/023/029)。
#
# 全部只依賴領域層,不需要 PDFium,因此連結清單只有 Alioth::domain。
set(ALIOTH_TEST_LIBS Alioth::domain)

alioth_add_test(test_annotation_tools test_annotation_tools.cpp)
alioth_add_test(test_ink_pressure test_ink_pressure.cpp)

# XFDF 匯入/匯出需要應用層(QXmlStreamReader/Writer 走 Qt6::Core,經由 Alioth::app 帶入)。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app)
alioth_add_test(test_xfdf_io test_xfdf_io.cpp)
set_property(TEST test_xfdf_io APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 剪貼簿需要 QApplication（QClipboard 屬於 QtGui），因此走 offscreen 而非 guiless。
alioth_add_test(test_annotation_clipboard test_annotation_clipboard.cpp)
set_property(TEST test_annotation_clipboard APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# FDF（非 XML）走物件層的剖析器與序列化器，因此多連 Alioth::objects。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app Alioth::objects)
alioth_add_test(test_fdf_io test_fdf_io.cpp)
set_property(TEST test_fdf_io APPEND PROPERTY ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 圖章（PRD-ANN-006）。需要外觀串流與物件層寫入，因此連結範圍比上面兩個大。
# object_fixture.h 與 qpdf_check.h 沿用 tests/objects 與 tests/qa 的既有版本，
# 不另建一份——判定規則有兩份就會分歧。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::objects Alioth::annotations)
alioth_add_test(test_stamp_annotation test_stamp_annotation.cpp)
target_include_directories(test_stamp_annotation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../objects
    ${CMAKE_CURRENT_SOURCE_DIR}/../qa)
set(ALIOTH_STAMP_QPDF_BIN "${CMAKE_SOURCE_DIR}/third_party/qpdf/bin/qpdf.exe")
if(NOT EXISTS "${ALIOTH_STAMP_QPDF_BIN}")
    find_program(ALIOTH_STAMP_QPDF_BIN NAMES qpdf)
endif()
if(ALIOTH_STAMP_QPDF_BIN AND EXISTS "${ALIOTH_STAMP_QPDF_BIN}")
    target_compile_definitions(test_stamp_annotation PRIVATE
        ALIOTH_QPDF_EXECUTABLE="${ALIOTH_STAMP_QPDF_BIN}")
endif()

# 回覆與審閱狀態（PRD-ANN-007）。狀態由一則獨立的回覆註解承載，
# 原註解不被改寫——那正是這支測試要釘住的結構。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app Alioth::objects)
alioth_add_test(test_annotation_reply test_annotation_reply.cpp)
target_include_directories(test_annotation_reply PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../objects)
set_property(TEST test_annotation_reply APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# 攤平（PRD-ANN-013）。核心是「攤平之後 /Annots 不能還留著那則註解」——
# 留著的話同一個標記會出現兩份，而浮在上面的那一份照樣選得到、改得動。
set(ALIOTH_TEST_LIBS Alioth::domain Alioth::app Alioth::objects Alioth::annotations)
alioth_add_test(test_annotation_flatten_service test_annotation_flatten_service.cpp)
set_property(TEST test_annotation_flatten_service APPEND PROPERTY
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
