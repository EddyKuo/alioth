#pragma once

// 鎖定／隱藏／列印旗標的操作面(PRD-ANN-012)。
//
// domain/annotation.h 已經定義了 /F 旗標本身(AnnotationFlag)與逐位元的
// 存取子(hasFlag、operator|)。這裡只補上「以使用者操作為單位」的語意
// 封裝——例如「切換鎖定」而不是要呼叫端自己記得 Locked 對應哪個位元、
// LockedContents 是否也要一起處理。刻意不改 domain/annotation.h 本體,
// 只在既有型別上疊一層純函數,降低與其他正在改該檔的協作衝突面。

#include "domain/annotation.h"

namespace alioth::domain {

[[nodiscard]] inline bool isLocked(const Annotation& annotation) noexcept {
    return hasFlag(annotation.flags, AnnotationFlag::Locked);
}

[[nodiscard]] inline bool isHidden(const Annotation& annotation) noexcept {
    return hasFlag(annotation.flags, AnnotationFlag::Hidden);
}

[[nodiscard]] inline bool isPrintable(const Annotation& annotation) noexcept {
    return hasFlag(annotation.flags, AnnotationFlag::Print);
}

// 鎖定同時鎖住位置與內容(/Locked + /LockedContents):PRD-ANN-012 的「鎖定」
// 是使用者可見的單一開關,不細分「只鎖位置」與「只鎖內容」兩檔——那個
// 區分在 Acrobat 裡也很少被用到,細分只會讓面板多兩個使用者搞不懂差異
// 的選項。
[[nodiscard]] inline Annotation setLocked(Annotation annotation, bool locked) noexcept {
    if (locked) {
        annotation.flags |= AnnotationFlag::Locked;
        annotation.flags |= AnnotationFlag::LockedContents;
    } else {
        annotation.flags = static_cast<AnnotationFlag>(
            static_cast<std::uint32_t>(annotation.flags) &
            ~static_cast<std::uint32_t>(AnnotationFlag::Locked | AnnotationFlag::LockedContents));
    }
    return annotation;
}

[[nodiscard]] inline Annotation setHidden(Annotation annotation, bool hidden) noexcept {
    if (hidden) {
        annotation.flags |= AnnotationFlag::Hidden;
        // 隱藏的註解不該同時被要求印出來——那會產生「螢幕上看不到但印出來
        // 卻有」的使用者無法理解的落差。
        annotation.flags = static_cast<AnnotationFlag>(
            static_cast<std::uint32_t>(annotation.flags) &
            ~static_cast<std::uint32_t>(AnnotationFlag::Print));
    } else {
        annotation.flags = static_cast<AnnotationFlag>(
            static_cast<std::uint32_t>(annotation.flags) & ~static_cast<std::uint32_t>(AnnotationFlag::Hidden));
    }
    return annotation;
}

[[nodiscard]] inline Annotation setPrintable(Annotation annotation, bool printable) noexcept {
    if (printable) {
        annotation.flags |= AnnotationFlag::Print;
    } else {
        annotation.flags = static_cast<AnnotationFlag>(
            static_cast<std::uint32_t>(annotation.flags) & ~static_cast<std::uint32_t>(AnnotationFlag::Print));
    }
    return annotation;
}

}  // namespace alioth::domain
