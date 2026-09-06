#include "engine/save/save_testing.h"

#include <fpdf_edit.h>
#include <fpdfview.h>

namespace alioth::engine::save::support {

bool rotatePage(DocumentHandle document, int pageIndex, int quarterTurns) {
    auto doc = static_cast<FPDF_DOCUMENT>(document);
    if (!doc) return false;
    FPDF_PAGE page = FPDF_LoadPage(doc, pageIndex);
    if (!page) return false;
    const int rotation = ((FPDFPage_GetRotation(page) + quarterTurns) % 4 + 4) % 4;
    FPDFPage_SetRotation(page, rotation);
    FPDF_ClosePage(page);
    return true;
}

}  // namespace alioth::engine::save::support
