# 應用層：控制器、命令匯流排、復原堆疊。可以用 Qt，但不得呼叫 PDFium。
add_library(alioth_app STATIC
    accessibility_service.cpp
    accessibility_service.h
    annotation_service.cpp
    annotation_service.h
    attachment_service.cpp
    attachment_service.h
    annotation_clipboard.cpp
    annotation_clipboard.h
    annotation_family_builders.cpp
    annotation_family_builders.h
    comment_summary_service.cpp
    comment_summary_service.h
    measurement_service.cpp
    measurement_service.h
    command_stack.cpp
    command_stack.h
    document_controller.cpp
    document_controller.h
    document_email.cpp
    document_email.h
    external_tools.cpp
    external_tools.h
    form_build_service.cpp
    form_build_service.h
    form_controller.cpp
    form_controller.h
    form_email.cpp
    form_email.h
    csv_form_fill.cpp
    csv_form_fill.h
    handwritten_signature.cpp
    handwritten_signature.h
    history_store.cpp
    history_store.h
    navigation_service.cpp
    navigation_service.h
    view_history.cpp
    view_history.h
    redaction_service.cpp
    redaction_service.h
    annotation_flatten_service.cpp
    annotation_flatten_service.h
    security_policy.cpp
    security_policy.h
    selection_controller.cpp
    selection_controller.h
    page_operations_service.cpp
    page_operations_service.h
    read_aloud_controller.cpp
    read_aloud_controller.h
    signature_controller.cpp
    signature_controller.h
    session.cpp
    session.h
    settings.cpp
    settings_profile.cpp
    settings_profile.h
    settings.h
    stamp_service.cpp
    stamp_service.h
    fdf_io.cpp
    fdf_io.h
    xfdf_io.cpp
    xfdf_io.h
    batch_runner.cpp
    batch_runner.h
)

target_link_libraries(alioth_app
    # Alioth::text 是 PUBLIC：selection_controller.h 的成員型別來自它。
    # Alioth::annotations 只在 .cpp 用到，維持 PRIVATE，避免 PDFium 的相依往上滲。
    PUBLIC Alioth::uisystem Alioth::domain Alioth::engine Alioth::platform Alioth::text Alioth::save Qt6::Core Qt6::Gui
    # Alioth::enhance 與 Alioth::pages 只被 batch_runner.cpp 用到（PRD-MAC-001
    # 批次處理需要色彩轉換／條碼／頁面旋轉這三個既有服務），維持 PRIVATE。
    PRIVATE Alioth::annotations Alioth::bookmarks Alioth::forms Alioth::formbuild Alioth::layers Alioth::objects Alioth::pageops Alioth::print Alioth::signature Alioth::enhance Alioth::pages
    # Alioth::attachments 只被 attachment_service.cpp 用到（PRD-ANN-015 加入附件）。
    Alioth::attachments
    # Alioth::create 只被 page_operations_service.cpp 用到（PRD-PAGE-001 的
    # 「從文字插入」把文字排版成 PDF，再走與「從檔案插入」相同的插入路徑）。
    Alioth::create
)

add_library(Alioth::app ALIAS alioth_app)

add_subdirectory(print)
add_subdirectory(touch)
