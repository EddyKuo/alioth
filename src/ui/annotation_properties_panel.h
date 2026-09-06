#pragma once

// 註解屬性側邊欄（PRD-ANN-009，Ctrl+' 開啟）。
//
// 這個面板的功能與 Comment Styles（PRD-ANN-029）刻意重疊一部分——後者是
// 「把一組屬性套到很多則註解」，這裡是「看見並改動眼前這一則」。分成兩個
// 入口不是重複，是因為兩件事的操作節奏差很多：改一則要即時看到結果，
// 套一組要先確認會影響哪些。
//
// 面板只負責「呈現與收集」，不直接寫檔：改動以訊號送出，由應用層包成命令
// 物件走命令匯流排（CLAUDE.md 的硬性行為要求），否則這些改動不會進復原堆疊。
//
// 沒有選取任何註解時顯示明確的空狀態，而不是一排灰掉的控制項——後者看起來
// 像壞了。

#include <QWidget>

#include <optional>

#include "domain/annotation.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;

namespace alioth::ui {

class AnnotationPropertiesPanel : public QWidget {
    Q_OBJECT

public:
    explicit AnnotationPropertiesPanel(QWidget* parent = nullptr);
    ~AnnotationPropertiesPanel() override;

    // 選取變更時由呼叫端餵入。std::nullopt 代表沒有選取。
    void setAnnotation(const std::optional<alioth::domain::Annotation>& annotation);

    [[nodiscard]] bool hasAnnotation() const noexcept { return current_.has_value(); }

protected:
    // 內容欄位在失焦時才送出改動，理由見建構式裡的說明。
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    // 使用者改了屬性。帶出完整的新狀態而不是差異：呼叫端要包成命令物件，
    // 而命令需要「改成什麼」與「原本是什麼」兩份完整快照才能正確復原。
    void annotationEdited(const alioth::domain::Annotation& updated);

private:
    void rebuildFromCurrent();
    void emitEdited();
    [[nodiscard]] alioth::domain::Annotation collect() const;

    std::optional<alioth::domain::Annotation> current_;
    // 由程式填值時要抑制訊號，否則 setValue 會被當成使用者操作而觸發一次寫入，
    // 於是每次選取都會在復原堆疊裡多出一筆什麼都沒改的命令。
    bool updating_{false};

    QStackedWidget* stack_{nullptr};
    QLabel* typeLabel_{nullptr};
    QPushButton* colorButton_{nullptr};
    QPushButton* interiorButton_{nullptr};
    QCheckBox* interiorEnabled_{nullptr};
    QDoubleSpinBox* opacity_{nullptr};
    QDoubleSpinBox* borderWidth_{nullptr};
    // 段落屬性（PRD-ANN-031）。只有 FreeText 家族用得到，因此整列在其他
    // 註解型別上隱藏而不是灰掉——灰掉的控制項會讓使用者一直在找怎麼啟用它。
    QDoubleSpinBox* lineSpacing_{nullptr};
    QDoubleSpinBox* indent_{nullptr};
    QWidget* lineSpacingLabel_{nullptr};
    QWidget* indentLabel_{nullptr};
    QComboBox* borderStyle_{nullptr};
    QLineEdit* author_{nullptr};
    QLineEdit* subject_{nullptr};
    QPlainTextEdit* contents_{nullptr};
    QCheckBox* locked_{nullptr};
    QCheckBox* hidden_{nullptr};
    QCheckBox* printable_{nullptr};
};

}  // namespace alioth::ui

// 訊號要跨 QVariant（QSignalSpy 與佇列連線都需要），因此註冊為 metatype。
// 少了它，測試拿到的是空的 QVariant，而症狀看起來像「訊號沒帶資料」。
Q_DECLARE_METATYPE(alioth::domain::Annotation)
