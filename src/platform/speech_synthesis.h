#pragma once

// 朗讀（Read Out Loud，PRD-A11Y-006）的語音合成後端。
//
// Windows 走 SAPI（Speech API，隨作業系統／Windows SDK 提供的 COM 元件）。
// 這不是新增第三方相依：PRD §4.1 鎖定的三個引擎級元件（Qt6 / PDFium /
// OpenSSL）指的是需要另外取得、另外維護升版節奏的外部函式庫；SAPI 與
// atomic_file.cpp 用的 Win32 檔案 API、shared_file.h 用的 FILE_SHARE_DELETE
// 是同一類東西——作業系統內建的能力，只是這裡走的是 COM 而不是扁平 API。
//
// 語音合成是平台層收斂作業系統差異的典型案例，因此本檔遵循 atomic_file.cpp
// 的既有慣例：#ifdef _WIN32 允許出現在這裡（也只允許出現在這裡），
// 其他四層一律看不到 Win32／COM 型別。macOS 要支援時走 NSSpeechSynthesizer、
// Linux 走 speech-dispatcher，都會是另一個分支，不影響呼叫端的介面。
//
// 朗讀內容一律來自 engine::objects::extractReadingText（結構樹順序），
// 本檔完全不理解 PDF 或文字擷取，只負責「把一段字串念出來」。

#include <QString>

#include <memory>

namespace alioth::platform {

enum class SpeechStatus {
    Ok,
    Unsupported,   // 這個平台目前沒有實作（例如尚未移植的 macOS / Linux）
    InitFailed,    // COM 元件建立失敗（缺少已安裝的語音引擎等）
    SpeakFailed,
};

class SpeechSynthesizer {
public:
    SpeechSynthesizer();
    ~SpeechSynthesizer();

    SpeechSynthesizer(const SpeechSynthesizer&) = delete;
    SpeechSynthesizer& operator=(const SpeechSynthesizer&) = delete;

    // false 代表這個平台／這台機器上朗讀不可用；呼叫端必須明確告知使用者，
    // 不能安靜地什麼都不做（那會被誤以為是程式沒反應）。
    [[nodiscard]] bool available() const noexcept;

    // 非同步排入朗讀佇列，立即回傳。多次呼叫會依序念完，不會互相打斷——
    // 這樣呼叫端可以把一頁拆成好幾句依序丟進來，而不必自己維護一個計時器
    // 去等上一句念完才丟下一句。
    [[nodiscard]] SpeechStatus speak(const QString& text);

    // 清空佇列並停止目前正在念的句子（用於「停止朗讀」或換頁）。
    void stop();
    void pause();
    void resume();

    // 佇列裡還有沒有念完的內容（含正在念的這一句）。
    [[nodiscard]] bool isSpeaking() const;

    // SAPI 的語速範圍是 -10（最慢）到 10（最快），音量是 0-100。
    void setRate(int rate);
    void setVolume(int volume);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alioth::platform
