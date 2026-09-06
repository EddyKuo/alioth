// 外觀串流遇到非 ASCII 字元時必須明確失敗（見 field_appearance.h 的說明）。
//
// 這支測試釘住一個修過的缺陷：foldToWinAnsi 舊版會靜默丟棄非 ASCII 字元，
// 只畫出剩下的 ASCII 部分（甚至整段空白），/V 仍然保有完整的值。
// 症狀是「CJK 表單欄位的 /V 有值但畫面空白」，使用者不會發現。
// 修正後遇到非 ASCII 字元必須讓整個欄位的外觀產生失敗，而不是產生殘缺的內容。

#include <QtTest>

#include "engine/fonts/cjk_font_library.h"
#include "engine/formbuild/field_appearance.h"

using namespace alioth;
using namespace alioth::engine::formbuild;

namespace {

FieldDefinition asciiTextField(std::string value) {
    FieldDefinition definition;
    definition.type = BuildFieldType::Text;
    definition.name = "field.text";
    definition.rectPt = domain::RectF{0.0, 0.0, 200.0, 24.0};
    definition.value = std::move(value);
    return definition;
}

}  // namespace

class TestFieldAppearanceAscii : public QObject {
    Q_OBJECT

private slots:
    void asciiValueSucceeds() {
        const FieldAppearance result = generateFieldAppearance(asciiTextField("Hello, World!"));
        QVERIFY2(result.valid, result.diagnostic.c_str());
        QVERIFY(!result.states.empty());
        QVERIFY(result.states.front().content.find("Hello, World!") != std::string::npos);
    }

    void emptyValueSucceeds() {
        const FieldAppearance result = generateFieldAppearance(asciiTextField(""));
        QVERIFY2(result.valid, result.diagnostic.c_str());
    }

    // ADR-007 之後，CJK 走內嵌的思源黑體子集而不是被拒絕。
    //
    // 這支測試原本要守的東西**一個字都沒有放寬**：畫不出來就要整個失敗，
    // 絕不輸出只剩 ASCII 的殘缺內容。變的只是判準——從「是不是 ASCII」
    // 變成「畫不畫得出來」。
    void cjkValueRendersWithTheEmbeddedFont() {
        const FieldAppearance result =
            generateFieldAppearance(asciiTextField("\xE5\xA7\x93\xE5\x90\x8D"));  // 姓名

        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QVERIFY(!result.valid);
            QVERIFY2(!result.diagnostic.empty(), "失敗時必須帶原因（IL-4）");
            // 不得殘留任何外觀狀態——失敗就是失敗，不能一半有效一半空白。
            QVERIFY(result.states.empty());
            return;
        }

        QVERIFY2(result.valid, result.diagnostic.c_str());
        QVERIFY(result.needsCjkFont);
        QCOMPARE(result.cjkCodepoints.size(), std::size_t(2));
        QVERIFY(!result.states.empty());
        // 內容串流必須切到 /CJK。留在 /Helv 會畫出亂碼，而那看起來像
        // 「字型沒裝好」而不是程式的錯。
        QVERIFY2(result.states.front().content.find("/CJK") != std::string::npos,
                 "欄位內容串流沒有切換到 CJK 字型");
    }

    void mixedAsciiAndCjkValueSwitchesFonts() {
        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QSKIP("沒有 CJK 字型，跳過");
        }
        const FieldAppearance result = generateFieldAppearance(
            asciiTextField(std::string("Name: ") + "\xE5\xA7\x93\xE5\x90\x8D"));
        QVERIFY2(result.valid, result.diagnostic.c_str());
        const std::string& content = result.states.front().content;
        // 拉丁與 CJK 的編碼方式不同，兩個字型都要出現。
        QVERIFY(content.find("/Helv") != std::string::npos);
        QVERIFY(content.find("/CJK") != std::string::npos);
        // 拉丁部分仍然是可讀的字面值。
        QVERIFY(content.find("Name: ") != std::string::npos);
    }

    void comboBoxWithCjkValueRenders() {
        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QSKIP("沒有 CJK 字型，跳過");
        }
        // ComboBox 的外觀只畫目前選取的值，不逐一畫出每個選項（那是 ListBox
        // 的行為），所以這裡驗證的是 .value 而不是 .options。
        FieldDefinition combo;
        combo.type = BuildFieldType::ComboBox;
        combo.name = "field.combo";
        combo.rectPt = domain::RectF{0.0, 0.0, 200.0, 24.0};
        combo.options = {"A", "B"};
        combo.value = "\xE4\xB8\xAD\xE6\x96\x87";

        const FieldAppearance result = generateFieldAppearance(combo);
        QVERIFY2(result.valid, result.diagnostic.c_str());
        QVERIFY(result.needsCjkFont);
    }

    void listBoxOptionWithCjkRenders() {
        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QSKIP("沒有 CJK 字型，跳過");
        }
        FieldDefinition list;
        list.type = BuildFieldType::ListBox;
        list.name = "field.list";
        list.rectPt = domain::RectF{0.0, 0.0, 200.0, 100.0};
        list.options = {"Alpha", "\xE4\xB8\xAD\xE6\x96\x87"};

        const FieldAppearance result = generateFieldAppearance(list);
        QVERIFY2(result.valid, result.diagnostic.c_str());
        QVERIFY(result.needsCjkFont);
    }

    void pushButtonCaptionWithCjkRenders() {
        if (!alioth::engine::fonts::CjkFontLibrary::instance().available()) {
            QSKIP("沒有 CJK 字型，跳過");
        }
        FieldDefinition button;
        button.type = BuildFieldType::PushButton;
        button.name = "field.button";
        button.rectPt = domain::RectF{0.0, 0.0, 100.0, 24.0};
        button.appearance.caption = "\xE7\xA2\xBA\xE5\xAE\x9A";  // "確定"

        const FieldAppearance result = generateFieldAppearance(button);
        QVERIFY2(result.valid, result.diagnostic.c_str());
        QVERIFY(result.needsCjkFont);
    }

    void controlCharactersStillFailExplicitly() {
        // 放寬的只有「非 ASCII」。控制字元仍然畫不出來，仍然要整個失敗，
        // 而不是畫一半。
        const FieldAppearance result =
            generateFieldAppearance(asciiTextField(std::string("a\x01") + "b"));
        QVERIFY(!result.valid);
        QVERIFY(!result.diagnostic.empty());
        QVERIFY(result.states.empty());
    }

    void pushButtonWithoutCaptionStillSucceeds() {
        FieldDefinition button;
        button.type = BuildFieldType::PushButton;
        button.name = "field.button";
        button.rectPt = domain::RectF{0.0, 0.0, 100.0, 24.0};

        const FieldAppearance result = generateFieldAppearance(button);
        QVERIFY2(result.valid, result.diagnostic.c_str());
    }

    void foldAcceptsWhatCanBeDrawnAndRejectsWhatCannot() {
        const AsciiFold ascii = foldToWinAnsi("plain text\n123");
        QVERIFY(ascii.ok);
        QCOMPARE(QString::fromStdString(ascii.text), QStringLiteral("plain text\n123"));

        // 有字型時中文通過，沒字型時不通過——判準是「畫不畫得出來」，
        // 不再是「是不是 ASCII」。
        const AsciiFold cjk = foldToWinAnsi("\xE4\xB8\xAD");
        QCOMPARE(cjk.ok, alioth::engine::fonts::CjkFontLibrary::instance().available());
        if (!cjk.ok) QVERIFY(cjk.text.empty());

        // 控制字元永遠不通過，而且失敗時不得回傳半截文字。
        const AsciiFold control = foldToWinAnsi(std::string("a\x01") + "b");
        QVERIFY(!control.ok);
        QVERIFY(control.text.empty());
    }
};

QTEST_MAIN(TestFieldAppearanceAscii)
#include "test_field_appearance_ascii.moc"
