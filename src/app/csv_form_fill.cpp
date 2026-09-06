#include "app/csv_form_fill.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
#include <memory>
#include <utility>

#include "domain/csv.h"
#include "engine/forms/form_document.h"

namespace alioth::app {

namespace {

using engine::forms::FormDataFormat;
using engine::forms::FormDataImport;
using engine::forms::FormDocument;
using engine::forms::FormFieldInfo;
using engine::forms::FormFillResult;

[[nodiscard]] std::string xmlEscapeFieldText(const std::string& text) {
    // 與 engine/forms/form_data.cpp 的 xmlEscape 邏輯相同，但那份是該檔案
    // 匿名命名空間裡的私有函式；這裡的用途（把任意欄位值包進 <value>）
    // 夠單純，重寫一份比為了共用十行程式碼而把它升級成公開 API 更省事。
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

// 資料列序號補零至 4 位，讓輸出目錄裡的檔案依檔名排序就等於依原始 CSV 順序。
[[nodiscard]] std::string padRowNumber(std::int32_t row) {
    std::string digits = std::to_string(row);
    while (digits.size() < 4) digits.insert(digits.begin(), '0');
    return digits;
}

[[nodiscard]] std::string buildOutputPath(const CsvFormFillOptions& options, std::int32_t row) {
    std::string name = options.outputNamePattern;
    const std::string token = "{row}";
    const std::size_t pos = name.find(token);
    if (pos != std::string::npos) name.replace(pos, token.size(), padRowNumber(row));

    std::string dir = options.outputDirectory;
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last != '/' && last != '\\') dir.push_back('/');
    return dir + name;
}

struct MatchedColumn {
    std::size_t csvColumnIndex{0};
    std::string fieldName;
};

// 把一列的值組成 XFDF 文字，只含表頭驗證通過的欄位。
[[nodiscard]] std::string buildRowXfdf(const std::vector<MatchedColumn>& matched,
                                       const std::vector<std::string>& row) {
    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n  <fields>\n";
    for (const MatchedColumn& column : matched) {
        if (column.csvColumnIndex >= row.size()) continue;  // 呼叫端已先擋過欄數不符
        out += "    <field name=\"" + xmlEscapeFieldText(column.fieldName) + "\">\n";
        out += "      <value>" + xmlEscapeFieldText(row[column.csvColumnIndex]) + "</value>\n";
        out += "    </field>\n";
    }
    out += "  </fields>\n</xfdf>\n";
    return out;
}

// 把工作排回呼叫端所在的執行緒（qApp 的執行緒親和性）。
//
// FormDocument 的每個回呼都在它自己的專用執行緒上被呼叫（SDD §3.2）。
// 這裡的流程需要在收到回呼後建立/銷毀下一個 FormDocument；若直接在該回呼
// 裡做，~FormDocument() 的 thread.join() 會對自己所在的執行緒自我 join，
// 導致 std::terminate。排回 qApp 的執行緒（也就是呼叫 runCsvFormFill 的
// 那條執行緒）之後才動 document_，就不會有這個問題——與 form_controller.cpp
// 的跨執行緒編組是同一個理由。
void postToCallerThread(std::function<void()> fn) {
    QMetaObject::invokeMethod(qApp, std::move(fn), Qt::QueuedConnection);
}

// 驅動整批流程的狀態機。以 shared_ptr + enable_shared_from_this 讓遞迴的
// callback 鏈能安全延續自己的生命週期。
class Runner : public std::enable_shared_from_this<Runner> {
public:
    Runner(CsvFormFillOptions opts, std::function<void(CsvFormFillReport)> onDone)
        : options_(std::move(opts)), onDone_(std::move(onDone)) {}

    void start() {
        const std::vector<std::vector<std::string>> parsed =
            domain::parseCsvDocument(options_.csvText);
        if (parsed.empty()) {
            finishAborted("CSV 內容為空，至少需要一列表頭");
            return;
        }
        header_ = parsed.front();
        dataRows_.assign(parsed.begin() + 1, parsed.end());
        if (header_.empty()) {
            finishAborted("CSV 表頭為空");
            return;
        }

        openForFieldDiscovery();
    }

private:
    void openForFieldDiscovery() {
        document_ = std::make_unique<FormDocument>();
        auto self = shared_from_this();
        document_->open(options_.templatePath, options_.password,
                        [self](domain::DocumentError error) {
                            postToCallerThread([self, error] {
                                if (error != domain::DocumentError::None) {
                                    self->finishAborted(
                                        "無法開啟範本文件（開啟失敗，可能是密碼錯誤或檔案損毀）");
                                    return;
                                }
                                self->document_->allFields(
                                    [self](std::vector<FormFieldInfo> fields) {
                                        postToCallerThread([self, fields = std::move(fields)] {
                                            self->onFieldsDiscovered(fields);
                                        });
                                    });
                            });
                        });
    }

    void onFieldsDiscovered(const std::vector<FormFieldInfo>& fields) {
        std::vector<std::string> templateNames;
        templateNames.reserve(fields.size());
        for (const FormFieldInfo& field : fields) {
            if (std::find(templateNames.begin(), templateNames.end(), field.name) ==
                templateNames.end()) {
                templateNames.push_back(field.name);
            }
        }

        for (std::size_t i = 0; i < header_.size(); ++i) {
            const std::string& columnName = header_[i];
            if (std::find(templateNames.begin(), templateNames.end(), columnName) !=
                templateNames.end()) {
                matched_.push_back(MatchedColumn{i, columnName});
            } else {
                report_.ignoredColumns.push_back(columnName);
            }
        }

        auto self = shared_from_this();
        document_->close([self] {
            postToCallerThread([self] {
                if (self->matched_.empty()) {
                    self->finishAborted("CSV 表頭沒有任何一欄能對應到範本裡的表單欄位，"
                                        "可能貼錯範本或欄位名稱不一致");
                    return;
                }
                self->processNextRow();
            });
        });
    }

    void processNextRow() {
        if (rowCursor_ >= dataRows_.size()) {
            finishCompleted();
            return;
        }

        const std::int32_t rowNumber = static_cast<std::int32_t>(rowCursor_) + 1;
        const std::vector<std::string>& row = dataRows_[rowCursor_];

        if (row.size() != header_.size()) {
            const std::string message = "第 " + std::to_string(rowNumber) + " 列的欄位數（" +
                                        std::to_string(row.size()) + "）與表頭（" +
                                        std::to_string(header_.size()) + "）不一致";
            if (options_.mismatchPolicy == CsvMismatchPolicy::AbortBatch) {
                finishAborted(message);
                return;
            }
            report_.rows.push_back(CsvFormFillRowResult{rowNumber, false, {}, message});
            ++rowCursor_;
            processNextRow();
            return;
        }

        // 先徹底釋放上一列的文件把手再開新的：這一步現在保證發生在呼叫端
        // 執行緒上（見 postToCallerThread），不會再對正在關閉的 FormDocument
        // 自我 join。兩份獨立把手短暫並存本身也不違反 PDFium 非執行緒安全的
        // 前提（SDD §1.1 已驗證兩條執行緒各自開同一份檔案沒問題），但批次
        // 流程本來就是嚴格序列，沒有理由留著額外的重疊視窗。
        document_.reset();
        document_ = std::make_unique<FormDocument>();
        auto self = shared_from_this();
        document_->open(options_.templatePath, options_.password,
                        [self, rowNumber](domain::DocumentError error) {
                            postToCallerThread([self, rowNumber, error] {
                                if (error != domain::DocumentError::None) {
                                    self->recordRowFailureAndAdvance(
                                        rowNumber, "無法重新開啟範本文件填寫此列");
                                    return;
                                }
                                self->fillRow(rowNumber);
                            });
                        });
    }

    void fillRow(std::int32_t rowNumber) {
        const std::vector<std::string>& row = dataRows_[static_cast<std::size_t>(rowNumber) - 1];
        const std::string xfdf = buildRowXfdf(matched_, row);

        auto self = shared_from_this();
        document_->importData(xfdf, FormDataFormat::Xfdf,
                              [self, rowNumber](FormDataImport result) {
                                  postToCallerThread([self, rowNumber, result = std::move(result)] {
                                      if (!result.ok) {
                                          self->recordRowFailureAndAdvance(
                                              rowNumber, "XFDF 匯入失敗：" + result.error);
                                          return;
                                      }
                                      self->saveRow(rowNumber);
                                  });
                              });
    }

    void saveRow(std::int32_t rowNumber) {
        const std::string outputPath = buildOutputPath(options_, rowNumber);
        auto self = shared_from_this();
        document_->saveCopy(outputPath, [self, rowNumber, outputPath](FormFillResult result) {
            postToCallerThread([self, rowNumber, outputPath, result] {
                if (!result.ok) {
                    self->recordRowFailureAndAdvance(rowNumber, "存檔失敗：" + result.error);
                    return;
                }
                self->document_->close([self, rowNumber, outputPath] {
                    postToCallerThread([self, rowNumber, outputPath] {
                        self->report_.rows.push_back(
                            CsvFormFillRowResult{rowNumber, true, outputPath, {}});
                        ++self->rowCursor_;
                        self->processNextRow();
                    });
                });
            });
        });
    }

    void recordRowFailureAndAdvance(std::int32_t rowNumber, std::string error) {
        report_.rows.push_back(CsvFormFillRowResult{rowNumber, false, {}, std::move(error)});
        auto self = shared_from_this();
        if (document_) {
            document_->close([self] {
                postToCallerThread([self] {
                    ++self->rowCursor_;
                    self->processNextRow();
                });
            });
        } else {
            ++rowCursor_;
            processNextRow();
        }
    }

    void finishAborted(std::string reason) {
        report_.aborted = true;
        report_.abortReason = std::move(reason);
        if (document_) {
            auto self = shared_from_this();
            document_->close([self] { postToCallerThread([self] { self->deliver(); }); });
        } else {
            deliver();
        }
    }

    void finishCompleted() {
        if (document_) {
            auto self = shared_from_this();
            document_->close([self] { postToCallerThread([self] { self->deliver(); }); });
        } else {
            deliver();
        }
    }

    void deliver() {
        // **先明確銷毀文件把手，再回呼。**
        //
        // 「讓它跟著 Runner 一起被解構」聽起來沒問題，但 Runner 的最後一個
        // 參照是握在「投遞出去的函式物件」手上的，而那個函式物件被銷毀的
        // 時機與執行緒取決於交錯順序——有可能落在工作執行緒上。
        // ~FormDocument 會 join 自己的工作執行緒，在那條執行緒上就是自我 join，
        // 結果是 std::terminate。
        //
        // 症狀是 ctest -j 下大約每九十次崩潰一次，而且不是斷言失敗——
        // 測試輸出在中途整個斷掉，沒有 FAIL 也沒有 Totals。單獨跑永遠不會發生。
        //
        // deliver() 一定跑在呼叫端執行緒（見 postToCallerThread），而且此刻
        // 工作執行緒已經完成 close，因此在這裡 join 是確定安全的。
        document_.reset();
        if (onDone_) onDone_(std::move(report_));
    }

    CsvFormFillOptions options_;
    std::function<void(CsvFormFillReport)> onDone_;

    std::vector<std::string> header_;
    std::vector<std::vector<std::string>> dataRows_;
    std::vector<MatchedColumn> matched_;
    std::size_t rowCursor_{0};

    std::unique_ptr<FormDocument> document_;
    CsvFormFillReport report_;
};

}  // namespace

std::int32_t CsvFormFillReport::succeededCount() const noexcept {
    return static_cast<std::int32_t>(
        std::count_if(rows.begin(), rows.end(), [](const CsvFormFillRowResult& r) { return r.ok; }));
}

std::int32_t CsvFormFillReport::failedCount() const noexcept {
    return static_cast<std::int32_t>(
        std::count_if(rows.begin(), rows.end(), [](const CsvFormFillRowResult& r) { return !r.ok; }));
}

void runCsvFormFill(const CsvFormFillOptions& options,
                    std::function<void(CsvFormFillReport)> onDone) {
    auto runner = std::make_shared<Runner>(options, std::move(onDone));
    runner->start();
}

}  // namespace alioth::app
