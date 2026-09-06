#include "platform/speech_synthesis.h"

#ifdef _WIN32
#include <windows.h>
#include <sapi.h>
#endif

namespace alioth::platform {

#ifdef _WIN32

struct SpeechSynthesizer::Impl {
    ISpVoice* voice{nullptr};
    // 只有這次建構真的初始化了 COM 的所有權才需要配對呼叫 CoUninitialize；
    // 執行緒已被別的元件（常見於某些 Qt 平台外掛）以不同併發模型初始化過時
    // （RPC_E_CHANGED_MODE），COM 本身可用但所有權不是我們的，配對關掉
    // 會把別人也一併關掉。
    bool ownsCom{false};

    Impl() {
        const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ownsCom = comResult == S_OK || comResult == S_FALSE;
        if (comResult == S_OK || comResult == S_FALSE || comResult == RPC_E_CHANGED_MODE) {
            ::CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                              reinterpret_cast<void**>(&voice));
        }
    }

    ~Impl() {
        if (voice != nullptr) voice->Release();
        if (ownsCom) ::CoUninitialize();
    }
};

SpeechSynthesizer::SpeechSynthesizer() : impl_(std::make_unique<Impl>()) {}
SpeechSynthesizer::~SpeechSynthesizer() = default;

bool SpeechSynthesizer::available() const noexcept { return impl_->voice != nullptr; }

SpeechStatus SpeechSynthesizer::speak(const QString& text) {
    if (impl_->voice == nullptr) return SpeechStatus::InitFailed;
    const std::wstring wide = text.toStdWString();
    // SPF_ASYNC：立即回傳，多次呼叫依序排隊念完，不互相打斷——
    // 呼叫端（app::ReadAloudController）靠這個特性把一頁拆成好幾句依序丟進來。
    const HRESULT hr = impl_->voice->Speak(wide.c_str(), SPF_ASYNC, nullptr);
    return SUCCEEDED(hr) ? SpeechStatus::Ok : SpeechStatus::SpeakFailed;
}

void SpeechSynthesizer::stop() {
    if (impl_->voice == nullptr) return;
    // 傳 nullptr 加 SPF_PURGEBEFORESPEAK：清空佇列且不念任何新內容，
    // 是 SAPI 官方文件記載的「停止朗讀」寫法。
    impl_->voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
}

void SpeechSynthesizer::pause() {
    if (impl_->voice != nullptr) impl_->voice->Pause();
}

void SpeechSynthesizer::resume() {
    if (impl_->voice != nullptr) impl_->voice->Resume();
}

bool SpeechSynthesizer::isSpeaking() const {
    if (impl_->voice == nullptr) return false;
    SPVOICESTATUS status{};
    if (FAILED(impl_->voice->GetStatus(&status, nullptr))) return false;
    return status.dwRunningState == SPRS_IS_SPEAKING;
}

void SpeechSynthesizer::setRate(int rate) {
    if (impl_->voice != nullptr) impl_->voice->SetRate(static_cast<long>(rate));
}

void SpeechSynthesizer::setVolume(int volume) {
    if (impl_->voice != nullptr) impl_->voice->SetVolume(static_cast<USHORT>(volume));
}

#else

// 尚未移植的平台（macOS 走 NSSpeechSynthesizer、Linux 走 speech-dispatcher）：
// 明確回報不可用，不是安靜地什麼都不做——那會被誤以為程式沒反應。
struct SpeechSynthesizer::Impl {};

SpeechSynthesizer::SpeechSynthesizer() : impl_(std::make_unique<Impl>()) {}
SpeechSynthesizer::~SpeechSynthesizer() = default;
bool SpeechSynthesizer::available() const noexcept { return false; }
SpeechStatus SpeechSynthesizer::speak(const QString&) { return SpeechStatus::Unsupported; }
void SpeechSynthesizer::stop() {}
void SpeechSynthesizer::pause() {}
void SpeechSynthesizer::resume() {}
bool SpeechSynthesizer::isSpeaking() const { return false; }
void SpeechSynthesizer::setRate(int) {}
void SpeechSynthesizer::setVolume(int) {}

#endif

}  // namespace alioth::platform
