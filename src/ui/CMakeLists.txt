# 呈現層：純 Qt，不得直接呼叫 PDFium，也不得連結 pdfium。
add_library(alioth_ui STATIC
    compare_dialog.cpp
    compare_dialog.h
    document_properties_dialog.cpp
    document_properties_dialog.h
    external_tool_dialogs.cpp
    external_tool_dialogs.h
    main_window.cpp
    main_window.h
    page_view.cpp
    page_view.h
    signature_panel.cpp
    signature_panel.h
    history_panel.cpp
    history_panel.h
    destinations_panel.cpp
    destinations_panel.h
    links_panel.cpp
    links_panel.h
    shortcuts_page.cpp
    shortcuts_page.h
    fields_panel.cpp
    fields_panel.h
    annotation_properties_panel.cpp
    annotation_properties_panel.h
    attachments_panel.cpp
    attachments_panel.h
    preferences_dialog.cpp
    preferences_dialog.h
    page_labels_dialog.cpp
    page_labels_dialog.h
    ribbon_customize_dialog.cpp
    ribbon_customize_dialog.h
    sign_dialog.cpp
    sign_dialog.h
    stamp_dialog.cpp
    stamp_dialog.h
    sticky_note_popup.cpp
    sticky_note_popup.h
    thumbnail_delegate.cpp
    thumbnail_delegate.h
    layers_panel.cpp
    layers_panel.h
    ruler_widget.cpp
    ruler_widget.h
    loupe_widget.cpp
    loupe_widget.h
    pan_zoom_panel.cpp
    pan_zoom_panel.h
    tags_panel.cpp
    tags_panel.h
    order_panel.cpp
    order_panel.h
    inspector_panel.cpp
    inspector_panel.h
    a11y_audit.cpp
    a11y_audit.h
)

# Alioth::objects 是 PUBLIC：Tags 面板的標頭直接用 StructTree 當參數型別
# （PRD-A11Y-001）。alioth_objects 刻意不連結 pdfium，因此這不違反
# 「呈現層不得連結 pdfium」——依賴的是純位元組的物件層。
target_link_libraries(alioth_ui PUBLIC Alioth::app Alioth::ribbon Alioth::print Alioth::objects
    Qt6::Widgets Qt6::PrintSupport
    # Fields 面板的樹狀模型來自 formbuild（PRD-FORM-021），只在 .cpp 用到。
    PRIVATE Alioth::formbuild Alioth::attachments Alioth::uisystem Alioth::touch
    # 匯出頁面為影像與純文字（PRD-IO-007），只在 main_window.cpp 用到。
    Alioth::iosec Alioth::text
    # 頁面標籤編輯（PRD-PAGE-013）走物件層的增量附加，不經 PDFium。
    Alioth::labels
    # Fit Visible 用頁面編輯器的內容邊界偵測（PRD-ZOOM-003），與裁切至白邊同一個判準。
    Alioth::pages
    # 文件比較（PRD-CMP-001）只在 main_window.cpp 用到。
    Alioth::compare
    # 建立新文件（空白／文字／Markdown／影像）的三條路徑，只在 main_window.cpp 用到。
    Alioth::create)
add_library(Alioth::ui ALIAS alioth_ui)

add_subdirectory(ribbon)
