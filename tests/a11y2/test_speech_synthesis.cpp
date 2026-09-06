// SAPI 語音合成包裝的冒煙測試（PRD-A11Y-006）。
//
// 這支測試不驗證「有沒有真的發出聲音」——CI 機器可能沒有音效裝置，
// 而且逐字比對語音輸出不是這裡的職責。它驗的是狀態機本身：
// 初始化、排入朗讀佇列、暫停／繼續、停止，這些呼叫要嘛成功執行，
// 要嘛在不可用時明確回報，不能崩潰或無聲地卡住。
//
// CI 機器若真的沒有安裝任何 SAPI 語音（極端但非不可能），available() 會是
// false；這裡用 QSKIP 而不是讓測試紅掉——那是環境限制，不是這支程式碼的缺陷。

#include <QtTest>

#include "platform/speech_synthesis.h"

using namespace alioth::platform;

class TestSpeechSynthesis : public QObject {
    Q_OBJECT

private slots:
    void speakEnqueuesWithoutBlocking() {
        SpeechSynthesizer speech;
        if (!speech.available()) {
            QSKIP("這台機器上沒有可用的 SAPI 語音，略過（見檔頭說明）");
        }

        QCOMPARE(static_cast<int>(speech.speak(QStringLiteral("Alioth accessibility test."))),
                 static_cast<int>(SpeechStatus::Ok));
        // speak() 是非同步排入佇列，呼叫本身必須立即回傳，不能等它念完。
        speech.stop();
        QVERIFY(!speech.isSpeaking());
    }

    void pauseResumeDoNotCrashWhenNothingIsSpeaking() {
        SpeechSynthesizer speech;
        if (!speech.available()) QSKIP("這台機器上沒有可用的 SAPI 語音，略過");
        speech.pause();
        speech.resume();
        speech.stop();
    }

    void rateAndVolumeAcceptDocumentedRange() {
        SpeechSynthesizer speech;
        if (!speech.available()) QSKIP("這台機器上沒有可用的 SAPI 語音，略過");
        speech.setRate(-10);
        speech.setRate(10);
        speech.setVolume(0);
        speech.setVolume(100);
    }
};

QTEST_APPLESS_MAIN(TestSpeechSynthesis)
#include "test_speech_synthesis.moc"
