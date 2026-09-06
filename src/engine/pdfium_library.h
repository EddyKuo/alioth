#pragma once

// PDFium 全域初始化的唯一入口。
//
// FPDF_InitLibraryWithConfig 在一個行程裡只能生效一次，而引擎與文字擷取是兩個
// 獨立的子系統、各自持有自己的專用執行緒。若兩邊各自初始化，第二次呼叫的行為
// 是未定義的——症狀通常不是當場崩潰，而是之後某個看似無關的地方拿到壞掉的狀態。
//
// 初始化一次就不再銷毀：PDFium 不保證 FPDF_DestroyLibrary 之後可以再 init，
// 而子系統的生命週期是交錯的。詳見 .cpp 的說明——那裡記著這個 bug 的實際症狀。

namespace alioth::engine {

class PdfiumRuntime {
public:
    // 在持有本物件期間，PDFium 保證已初始化。放在每條 PDFium 專用執行緒的
    // 生命週期上，而不是散在各個呼叫點。
    PdfiumRuntime();
    ~PdfiumRuntime();

    PdfiumRuntime(const PdfiumRuntime&) = delete;
    PdfiumRuntime& operator=(const PdfiumRuntime&) = delete;
};

}  // namespace alioth::engine
