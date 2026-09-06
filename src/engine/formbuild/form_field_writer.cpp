#include "engine/formbuild/form_field_writer.h"

#include "engine/fonts/cjk_font_library.h"

#include "engine/fonts/cid_font_writer.h"

#include <algorithm>
#include <variant>

#include "engine/formbuild/field_appearance.h"
#include "engine/objects/page_object_editor.h"

namespace alioth::engine::formbuild {

namespace {

using objects::makeLiteralString;
using objects::makeName;
using objects::makeNumberArray;
using objects::makeRef;
using objects::makeTextString;
using objects::PdfArray;
using objects::PdfDictionary;
using objects::PdfObject;
using objects::PdfRef;

// 欄位樹的走訪上限。/Kids 可以互相指涉，沒有上限的走訪會在惡意檔案上停不下來
// （SDD §7：不可信任輸入決定的深度一律迭代並設上限）。
constexpr int kMaxFieldNodes = 20000;

// 註解旗標 /F 的 Print 位元。缺了它，欄位在畫面上看得到但列印時消失，
// 而使用者要印出來才會發現。
constexpr std::int64_t kAnnotFlagPrint = 4;

// /DR 裡的預設字型資源名。Acrobat 產出的表單用的也是 Helv，
// 換成別的名字會讓既有文件的 /DA 字串（多半寫死 /Helv）找不到字型。
constexpr const char* kDefaultFontName = "Helv";

[[nodiscard]] FieldWriteResult writeFailure(std::string reason) {
    FieldWriteResult result;
    result.diagnostic = std::move(reason);
    return result;
}

[[nodiscard]] PdfObject colorArray(const FieldColor& color) {
    return makeNumberArray({color.r, color.g, color.b});
}

[[nodiscard]] PdfObject rectArray(const domain::RectF& rect) {
    const domain::RectF r = rect.normalized();
    return makeNumberArray({r.left, r.bottom, r.right, r.top});
}

[[nodiscard]] PdfObject fontDictionary() {
    PdfDictionary font;
    font.set("Type", makeName("Font"));
    font.set("Subtype", makeName("Type1"));
    font.set("BaseFont", makeName("Helvetica"));
    // WinAnsiEncoding：不指定編碼時 Acrobat 會用字型內建編碼，
    // 而 Helvetica 的內建編碼是 StandardEncoding，引號與破折號會畫錯字。
    font.set("Encoding", makeName("WinAnsiEncoding"));
    return PdfObject{std::move(font)};
}

}  // namespace

FormFieldWriter::FormFieldWriter(objects::IncrementalAppender& appender) : appender_(appender) {
    if (!appender_.isOpen()) {
        diagnostic_ = "附加器尚未開啟原檔";
        return;
    }
    if (!loadCatalog()) return;
    collectExistingNames();
    ready_ = true;
}

bool FormFieldWriter::loadCatalog() {
    const PdfObject* root = appender_.source().trailer().find("Root");
    if (root == nullptr || !root->isRef()) {
        diagnostic_ = "原檔的 trailer 沒有可用的 /Root";
        return false;
    }
    catalogRef_ = root->asRef();

    const PdfObject catalog = appender_.currentObject(catalogRef_.number);
    const PdfDictionary* catalogDict = catalog.asDictionary();
    if (catalogDict == nullptr) {
        diagnostic_ = "/Root 指向的不是字典";
        return false;
    }

    const PdfObject* acroForm = catalogDict->find("AcroForm");
    if (acroForm == nullptr) return true;  // 尚無表單，finish() 時新建

    PdfObject resolved = *acroForm;
    if (acroForm->isRef()) {
        acroFormObject_ = acroForm->asRef().number;
        resolved = appender_.currentObject(acroFormObject_);
    }
    const PdfDictionary* dict = resolved.asDictionary();
    if (dict == nullptr) {
        diagnostic_ = "/AcroForm 不是字典";
        return false;
    }
    acroForm_ = *dict;

    if (const PdfObject* fields = acroForm_.find("Fields")) {
        PdfObject resolvedFields = fields->isRef()
                                       ? appender_.currentObject(fields->asRef().number)
                                       : *fields;
        if (const PdfArray* array = resolvedFields.asArray()) fields_ = *array;
    }
    return true;
}

void FormFieldWriter::collectExistingNames() {
    // 顯式堆疊的走訪。欄位樹的 /Kids 來自不可信任輸入，遞迴實作會被
    // 互相指涉的節點推爆堆疊。
    std::vector<PdfObject> stack;
    for (const PdfObject& entry : fields_) stack.push_back(entry);

    int visited = 0;
    std::vector<int> seenObjects;
    while (!stack.empty() && visited < kMaxFieldNodes) {
        PdfObject current = stack.back();
        stack.pop_back();
        ++visited;

        if (current.isRef()) {
            const int number = current.asRef().number;
            if (std::find(seenObjects.begin(), seenObjects.end(), number) != seenObjects.end()) {
                continue;
            }
            seenObjects.push_back(number);
            current = appender_.currentObject(number);
        }
        const PdfDictionary* dict = current.asDictionary();
        if (dict == nullptr) continue;

        if (const PdfObject* title = dict->find("T")) {
            if (const auto* text = std::get_if<objects::PdfString>(&title->value())) {
                if (!text->bytes.empty()) fieldNames_.push_back(text->bytes);
            }
        }
        if (const PdfObject* kids = dict->find("Kids")) {
            PdfObject resolved = kids->isRef() ? appender_.currentObject(kids->asRef().number)
                                               : *kids;
            if (const PdfArray* array = resolved.asArray()) {
                for (const PdfObject& kid : *array) stack.push_back(kid);
            }
        }
    }
}

int FormFieldWriter::ensureDefaultFont() {
    if (fontObject_ != 0) return fontObject_;

    // 既有的 /DR /Font /Helv 優先沿用：另建一個同名資源會讓兩份字型物件
    // 同時存在，Acrobat 取哪一份沒有保證。
    if (const PdfObject* dr = acroForm_.find("DR")) {
        PdfObject resolvedDr = dr->isRef() ? appender_.currentObject(dr->asRef().number) : *dr;
        if (const PdfDictionary* drDict = resolvedDr.asDictionary()) {
            if (const PdfObject* fonts = drDict->find("Font")) {
                PdfObject resolvedFonts =
                    fonts->isRef() ? appender_.currentObject(fonts->asRef().number) : *fonts;
                if (const PdfDictionary* fontDict = resolvedFonts.asDictionary()) {
                    if (const PdfObject* helv = fontDict->find(kDefaultFontName);
                        helv != nullptr && helv->isRef()) {
                        fontObject_ = helv->asRef().number;
                        return fontObject_;
                    }
                }
            }
        }
    }

    fontObject_ = appender_.allocateObject();
    appender_.setObject(fontObject_, fontDictionary());
    return fontObject_;
}

PdfObject FormFieldWriter::buildBorderStyle(const FieldDefinition& definition) const {
    PdfDictionary bs;
    bs.set("Type", makeName("Border"));
    bs.set("W", PdfObject{definition.appearance.borderWidth});
    bs.set("S", makeName(definition.appearance.dashedBorder ? "D" : "S"));
    if (definition.appearance.dashedBorder) {
        bs.set("D", makeNumberArray({definition.appearance.borderWidth * 3.0}));
    }
    return PdfObject{std::move(bs)};
}

PdfObject FormFieldWriter::buildMk(const FieldDefinition& definition) const {
    PdfDictionary mk;
    if (definition.appearance.backgroundColor.has_value()) {
        mk.set("BG", colorArray(*definition.appearance.backgroundColor));
    }
    if (definition.appearance.borderColor.has_value()) {
        mk.set("BC", colorArray(*definition.appearance.borderColor));
    }
    if (!definition.appearance.caption.empty()) {
        mk.set("CA", makeTextString(definition.appearance.caption));
    }
    if (definition.type == BuildFieldType::Image) {
        // /TP 1 = 只顯示圖示。Acrobat 的「影像欄位」就是這樣實作的：
        // 一個只畫圖示的按鈕，沒有獨立的 /FT。
        mk.set("TP", PdfObject{static_cast<std::int64_t>(1)});
        PdfDictionary iconFit;
        iconFit.set("SW", makeName("A"));   // 一律縮放
        iconFit.set("S", makeName("P"));    // 等比例
        iconFit.set("A", makeNumberArray({0.5, 0.5}));  // 置中
        mk.set("IF", PdfObject{std::move(iconFit)});
    }
    return PdfObject{std::move(mk)};
}

std::string FormFieldWriter::buildDefaultAppearance(const FieldDefinition& definition) const {
    // /DA 的字級 0 代表「自動調整」。這是規格允許的寫法，
    // 但外觀串流裡不能用 0——那會畫出看不見的文字，所以兩邊的字級是分開決定的。
    std::string da = "/";
    da += kDefaultFontName;
    da += " " + objects::formatReal(definition.fontSize) + " Tf ";
    da += objects::formatReal(definition.textColor.r) + " " +
          objects::formatReal(definition.textColor.g) + " " +
          objects::formatReal(definition.textColor.b) + " rg";
    return da;
}

FieldWriteResult FormFieldWriter::addField(const FieldDefinition& definition) {
    if (!ready_) return writeFailure(diagnostic_.empty() ? "寫入器未就緒" : diagnostic_);
    if (finished_) return writeFailure("finish() 之後不可再加入欄位");

    if (const std::string problem = validate(definition); !problem.empty()) {
        return writeFailure(problem);
    }
    if (std::find(fieldNames_.begin(), fieldNames_.end(), definition.name) != fieldNames_.end()) {
        // 同名欄位在 PDF 裡代表「同一個欄位的多個 widget」，值會連動。
        // 使用者以為建了兩個獨立欄位，實際上打字會同時出現在兩格——
        // 這種錯誤在填表當下才會發現，所以必須在建立時就擋掉。
        return writeFailure("欄位名稱已存在：" + definition.name);
    }

    PdfRef pageRef{};
    if (!objects::pageRefAt(appender_, definition.pageIndex, pageRef)) {
        return writeFailure("頁碼超出範圍：" + std::to_string(definition.pageIndex));
    }

    const bool isRadio = definition.type == BuildFieldType::RadioGroup;
    const std::size_t widgetCount = isRadio ? definition.radios.size() : 1;

    FieldWriteResult result;

    // 先產生所有外觀，任何一個失敗就整個欄位不寫——寫一半的欄位比不寫更糟，
    // 檔案結構合法但欄位不可用，而且沒有任何錯誤訊息（IL-4）。
    std::vector<FieldAppearance> appearances;
    appearances.reserve(widgetCount);
    for (std::size_t i = 0; i < widgetCount; ++i) {
        FieldAppearance appearance = generateFieldAppearance(definition, i);
        if (!appearance.valid) {
            return writeFailure("外觀串流產生失敗：" + appearance.diagnostic);
        }
        appearances.push_back(std::move(appearance));
    }

    const bool needsFont =
        std::any_of(appearances.begin(), appearances.end(), [](const FieldAppearance& a) {
            return std::any_of(a.states.begin(), a.states.end(),
                               [](const FieldAppearanceState& s) { return s.needsFont; });
        });
    const int fontObject = needsFont ? ensureDefaultFont() : 0;

    // CJK 內嵌字型（ADR-007）。整個欄位的所有狀態共用同一份子集——
    // 逐狀態各嵌一份會讓一個核取方塊重複內嵌兩份相同的字型。
    std::set<char32_t> cjkCodepoints;
    for (const FieldAppearance& appearance : appearances) {
        cjkCodepoints.insert(appearance.cjkCodepoints.begin(), appearance.cjkCodepoints.end());
    }
    int cjkFontObject = 0;
    if (!cjkCodepoints.empty()) {
        auto& library = fonts::CjkFontLibrary::instance();
        const fonts::SubsetResult subset = library.subsetFor(cjkCodepoints);
        if (!subset.ok) {
            return writeFailure("CJK 字型子集化失敗：" + subset.diagnostic);
        }
        const fonts::EmbeddedFontResult embedded =
            fonts::embedSubsetFont(appender_, subset, library.baseName());
        if (!embedded.ok) {
            return writeFailure("內嵌 CJK 字型失敗：" + embedded.diagnostic);
        }
        cjkFontObject = embedded.fontObject;
    }

    const int fieldObject = appender_.allocateObject();
    std::vector<int> widgetObjects;
    widgetObjects.reserve(widgetCount);
    if (isRadio) {
        for (std::size_t i = 0; i < widgetCount; ++i) {
            widgetObjects.push_back(appender_.allocateObject());
        }
    } else {
        // 單一 widget 的欄位一律把欄位字典與 widget 字典合併成一個物件。
        // 規格允許（§12.5.6.19），而且少一層 /Parent 就少一處寫錯的機會。
        widgetObjects.push_back(fieldObject);
    }

    // 外觀串流物件。
    std::vector<std::vector<std::pair<std::string, int>>> stateObjects(widgetCount);
    for (std::size_t i = 0; i < widgetCount; ++i) {
        for (const FieldAppearanceState& state : appearances[i].states) {
            const int number = appender_.allocateObject();

            PdfDictionary dict;
            dict.set("Type", makeName("XObject"));
            dict.set("Subtype", makeName("Form"));
            dict.set("FormType", PdfObject{static_cast<std::int64_t>(1)});
            dict.set("BBox", rectArray(appearances[i].bbox));
            dict.set("Matrix", makeNumberArray({1, 0, 0, 1, 0, 0}));

            PdfDictionary resources;
            if ((state.needsFont && fontObject != 0) || cjkFontObject != 0) {
                PdfDictionary fontDict;
                if (state.needsFont && fontObject != 0) {
                    fontDict.set(kDefaultFontName, makeRef(fontObject));
                }
                if (cjkFontObject != 0) {
                    // 名稱要與 field_appearance.cpp 寫進內容串流的 /CJK 一致。
                    // 對不上的話欄位的中文**整段消失**——不是亂碼，是什麼都不畫。
                    fontDict.set("CJK", makeRef(cjkFontObject));
                }
                resources.set("Font", PdfObject{std::move(fontDict)});
            }
            // /Resources 一律寫出，即使是空的：部分檢視器在缺少這個鍵時
            // 會沿用頁面資源，那會讓外觀在不同頁面上長得不一樣。
            dict.set("Resources", PdfObject{std::move(resources)});

            appender_.setObject(number, PdfObject{objects::PdfStream{std::move(dict),
                                                                     state.content}});
            stateObjects[i].emplace_back(state.stateName, number);
            result.appearanceObjects.push_back(number);
        }
    }

    const std::int64_t flags = computeFieldFlags(definition);

    // 欄位層的鍵（/FT /T /Ff /V …）。單 widget 時直接併進 widget 字典。
    PdfDictionary field;
    field.set("FT", makeName(fieldTypeName(definition.type)));
    field.set("T", makeTextString(definition.name));
    if (!definition.alternateName.empty()) {
        field.set("TU", makeTextString(definition.alternateName));
    }
    if (!definition.mappingName.empty()) {
        field.set("TM", makeTextString(definition.mappingName));
    }
    field.set("Ff", PdfObject{flags});

    switch (definition.type) {
        case BuildFieldType::Text:
        case BuildFieldType::Date:
            field.set("V", makeTextString(definition.value));
            field.set("DV", makeTextString(definition.value));
            field.set("DA", makeLiteralString(buildDefaultAppearance(definition)));
            field.set("Q", PdfObject{static_cast<std::int64_t>(definition.alignment)});
            if (definition.maxLength > 0) {
                field.set("MaxLen", PdfObject{static_cast<std::int64_t>(definition.maxLength)});
            }
            break;

        case BuildFieldType::ComboBox:
        case BuildFieldType::ListBox: {
            field.set("V", makeTextString(definition.value));
            field.set("DV", makeTextString(definition.value));
            field.set("DA", makeLiteralString(buildDefaultAppearance(definition)));
            field.set("Q", PdfObject{static_cast<std::int64_t>(definition.alignment)});
            PdfArray options;
            options.reserve(definition.options.size());
            for (const std::string& option : definition.options) {
                options.push_back(makeTextString(option));
            }
            field.set("Opt", PdfObject{std::move(options)});
            // /I 是選取項的索引。清單方塊少了它，捲動位置與選取狀態
            // 在重新開檔後會回到第一項。
            const auto found = std::find(definition.options.begin(), definition.options.end(),
                                         definition.value);
            if (found != definition.options.end()) {
                PdfArray indices;
                indices.emplace_back(
                    static_cast<std::int64_t>(std::distance(definition.options.begin(), found)));
                field.set("I", PdfObject{std::move(indices)});
            }
            break;
        }

        case BuildFieldType::CheckBox:
            field.set("V", makeName(definition.checked ? definition.exportValue : "Off"));
            field.set("DV", makeName(definition.checked ? definition.exportValue : "Off"));
            break;

        case BuildFieldType::RadioGroup: {
            const bool anySelected =
                std::any_of(definition.radios.begin(), definition.radios.end(),
                            [&](const RadioOption& o) { return o.exportValue == definition.value; });
            field.set("V", makeName(anySelected ? definition.value : "Off"));
            field.set("DV", makeName(anySelected ? definition.value : "Off"));
            break;
        }

        case BuildFieldType::PushButton:
        case BuildFieldType::Image:
            // 按鈕沒有值。寫 /V 會讓部分檢視器把它當成可匯出的欄位。
            break;

        case BuildFieldType::Signature:
            // 未簽署的簽章欄位不寫 /V。寫成空字串會讓簽章面板顯示
            // 一個「已損毀的簽章」而不是「未簽署的欄位」。
            needsSigFlags_ = true;
            break;

        case BuildFieldType::Barcode:
            // /V 是條碼實際編碼的內容；不寫 /DA——外觀不是靠標準字型繪出的
            // 文字，/DA 在這裡沒有對應的視覺效果，寫了只會誤導以為可以
            // 調整字型或顏色來改變條碼外觀。
            field.set("V", makeTextString(definition.value));
            field.set("DV", makeTextString(definition.value));
            break;
    }

    // 零腳本執行（PRD §8.2）下，日期格式與計算式沒有 /AA JavaScript 可掛，
    // 因此以私有鍵保存。私有鍵是規格允許的（§7.12），其他檢視器會忽略它，
    // 代價是這兩項功能在別的產品上不生效——這比寫入 JavaScript 好，
    // 因為那等於要求使用者的檢視器執行我們自己都不執行的腳本。
    if (definition.type == BuildFieldType::Date && !definition.dateFormat.empty()) {
        field.set("Alioth_DateFormat", makeTextString(definition.dateFormat));
    }
    if (!definition.calculation.empty()) {
        field.set("Alioth_Calc", makeTextString(definition.calculation));
    }

    if (isRadio) {
        PdfArray kids;
        for (const int widget : widgetObjects) kids.push_back(makeRef(widget));
        field.set("Kids", PdfObject{std::move(kids)});
        appender_.setObject(fieldObject, PdfObject{std::move(field)});
    }

    for (std::size_t i = 0; i < widgetCount; ++i) {
        PdfDictionary widget = isRadio ? PdfDictionary{} : field;
        widget.set("Type", makeName("Annot"));
        widget.set("Subtype", makeName("Widget"));
        if (isRadio) widget.set("Parent", makeRef(fieldObject));
        widget.set("F", PdfObject{kAnnotFlagPrint});
        widget.set("Rect", rectArray(isRadio ? definition.radios[i].rectPt : definition.rectPt));
        widget.set("P", makeRef(pageRef.number, pageRef.generation));
        widget.set("MK", buildMk(definition));
        widget.set("BS", buildBorderStyle(definition));

        const auto& states = stateObjects[i];
        if (states.size() == 1 && states.front().first.empty()) {
            PdfDictionary ap;
            ap.set("N", makeRef(states.front().second));
            widget.set("AP", PdfObject{std::move(ap)});
        } else {
            PdfDictionary normal;
            for (const auto& [name, number] : states) normal.set(name, makeRef(number));
            PdfDictionary ap;
            ap.set("N", PdfObject{std::move(normal)});
            widget.set("AP", PdfObject{std::move(ap)});

            // /AS 決定目前畫哪一個外觀。缺了它，Acrobat 與 PDFium 都會
            // 拒絕切換狀態，核取方塊看起來像壞掉的圖片。
            std::string current = "Off";
            if (isRadio) {
                if (definition.radios[i].exportValue == definition.value) {
                    current = definition.value;
                }
            } else if (definition.checked) {
                current = definition.exportValue;
            }
            widget.set("AS", makeName(current));
        }

        appender_.setObject(widgetObjects[i], PdfObject{std::move(widget)});

        const objects::PageEditStatus status =
            objects::appendToPageArray(appender_, pageRef, "Annots", makeRef(widgetObjects[i]));
        if (!status.ok) return writeFailure("掛上 /Annots 失敗：" + status.diagnostic);
    }

    // Tab 順序：見 page_object_editor.h 的說明，只在頁面尚未指定 /Tabs 時
    // 寫入 /W（依 /Annots 裡 Widget 出現順序決定跳位順序）。這裡刻意不把
    // 失敗當成整個欄位建立失敗——跳位順序是可用性問題，不是欄位能不能用
    // 的問題，且 /Annots 已經成功掛上，回頭讓整個 addField 失敗反而會讓
    // 呼叫端誤以為欄位沒建立成功。
    objects::ensurePageTabOrder(appender_, pageRef);

    fields_.push_back(makeRef(fieldObject));
    fieldNames_.push_back(definition.name);
    written_.push_back(definition);

    result.ok = true;
    result.fieldObject = fieldObject;
    result.widgetObjects = std::move(widgetObjects);
    return result;
}

std::string FormFieldWriter::finish() {
    if (!ready_) return diagnostic_.empty() ? "寫入器未就緒" : diagnostic_;
    if (finished_) return {};
    finished_ = true;

    acroForm_.set("Fields", PdfObject{fields_});

    if (!acroForm_.has("DA")) {
        // 表單層的預設外觀。欄位自己的 /DA 會覆蓋它，但欄位缺 /DA 時
        // 沒有這個鍵，Acrobat 會拒絕顯示文字。
        acroForm_.set("DA", makeLiteralString("/Helv 0 Tf 0 g"));
    }
    if (fontObject_ != 0) {
        PdfDictionary dr;
        // 既有的 /DR 若已含其他資源就沿用，只補上字型；整份覆蓋會弄丟
        // 原檔既有欄位用到的字型，那些欄位的文字會整批消失。
        if (const PdfObject* existing = acroForm_.find("DR")) {
            PdfObject resolved =
                existing->isRef() ? appender_.currentObject(existing->asRef().number) : *existing;
            if (const PdfDictionary* dict = resolved.asDictionary()) dr = *dict;
        }
        PdfDictionary fonts;
        if (const PdfObject* existingFonts = dr.find("Font")) {
            PdfObject resolved = existingFonts->isRef()
                                     ? appender_.currentObject(existingFonts->asRef().number)
                                     : *existingFonts;
            if (const PdfDictionary* dict = resolved.asDictionary()) fonts = *dict;
        }
        fonts.set(kDefaultFontName, makeRef(fontObject_));
        dr.set("Font", PdfObject{std::move(fonts)});
        acroForm_.set("DR", PdfObject{std::move(dr)});
    }
    // 我們自產所有 /AP，因此明確寫 false。留著 true 會讓 Acrobat 重畫全部欄位，
    // 那會改動檔案並使既有簽章顯示為「簽章後有變更」，而變更並非使用者所為。
    acroForm_.set("NeedAppearances", PdfObject{false});
    if (needsSigFlags_) {
        // /SigFlags 3 = 存在簽章欄位且文件僅允許增量更新。
        acroForm_.set("SigFlags", PdfObject{static_cast<std::int64_t>(3)});
    }

    if (acroFormObject_ != 0) {
        if (!appender_.updateObject(acroFormObject_, PdfObject{acroForm_})) {
            return "無法更新 /AcroForm 物件";
        }
        return {};
    }

    PdfObject catalog = appender_.currentObject(catalogRef_.number);
    PdfDictionary* catalogDict = catalog.asDictionary();
    if (catalogDict == nullptr) return "/Root 指向的不是字典";
    catalogDict->set("AcroForm", PdfObject{acroForm_});
    if (!appender_.updateObject(catalogRef_.number, catalog)) {
        return "無法更新 catalog 物件";
    }
    return {};
}

}  // namespace alioth::engine::formbuild
