#include "document.h"

namespace alioth::domain {

const char* describe(DocumentError error) noexcept {
    switch (error) {
        case DocumentError::None:               return "成功";
        case DocumentError::FileNotFound:       return "找不到檔案";
        case DocumentError::NotAPdf:            return "不是有效的 PDF 檔案";
        case DocumentError::PasswordRequired:   return "此文件需要密碼";
        case DocumentError::WrongPassword:      return "密碼錯誤";
        case DocumentError::CorruptXref:        return "交叉參照表損壞，已嘗試重建";
        case DocumentError::UnsupportedFeature: return "文件使用了不支援的功能";
        case DocumentError::OutOfMemory:        return "記憶體不足";
        case DocumentError::Unknown:            return "未知錯誤";
    }
    return "未知錯誤";
}

}  // namespace alioth::domain
