# 文件比對、重複頁偵測與尋找取代（WBS 5.12，PRD-CMP-001 / 002、PRD-SRCH-003）。
#
# 差異演算法（token_diff / page_align / duplicate_pages）刻意不碰 PDFium：
# 它們是純資料進、純資料出，因此可以用合成的 token 序列測到邊界條件，
# 而不必為了測「插入一頁」去生一份真實 PDF。
#
# 文字來源走 Alioth::text（獨立的文件把手與專用執行緒），
# 尋找取代的寫入走 Alioth::objects（純附加，不破壞既有簽章）。
add_library(alioth_compare STATIC
    token_diff.cpp
    token_diff.h
    page_tokens.cpp
    page_tokens.h
    page_align.cpp
    page_align.h
    document_comparer.cpp
    document_comparer.h
    duplicate_pages.cpp
    duplicate_pages.h
    find_replace.cpp
    find_replace.h
    spellcheck.cpp
    spellcheck.h
    utf8_scan.h
)

target_include_directories(alioth_compare PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(alioth_compare
    PUBLIC Alioth::domain
    PRIVATE Alioth::text Alioth::objects
)

add_library(Alioth::compare ALIAS alioth_compare)
