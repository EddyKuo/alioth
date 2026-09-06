#include "engine/formbuild/field_tree.h"

#include <algorithm>
#include <iterator>

namespace alioth::engine::formbuild {

namespace {

[[nodiscard]] std::vector<std::string> splitSegments(const std::string& name) {
    std::vector<std::string> segments;
    std::string current;
    for (const char c : name) {
        if (c == '.') {
            // 空片段代表名稱裡有連續的點。validate() 已經擋掉這種名稱，
            // 但這條路徑也吃得到既有文件的欄位名，因此這裡再擋一次：
            // 空片段會產生一個沒有名字、也點不開的樹節點。
            if (!current.empty()) segments.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) segments.push_back(current);
    return segments;
}

[[nodiscard]] FieldTreeNode* findChild(std::vector<FieldTreeNode>& nodes,
                                       const std::string& segment) {
    for (FieldTreeNode& node : nodes) {
        if (node.segment == segment) return &node;
    }
    return nullptr;
}

}  // namespace

std::int32_t FieldTreeNode::fieldCount() const {
    std::int32_t count = isField ? 1 : 0;
    for (const FieldTreeNode& child : children) count += child.fieldCount();
    return count;
}

std::vector<FieldTreeNode> buildFieldTree(const std::vector<FieldSummary>& fields) {
    std::vector<FieldTreeNode> roots;

    for (const FieldSummary& field : fields) {
        const std::vector<std::string> segments = splitSegments(field.name);
        if (segments.empty()) continue;

        std::vector<FieldTreeNode>* level = &roots;
        std::string prefix;
        FieldTreeNode* node = nullptr;

        for (const std::string& segment : segments) {
            prefix = prefix.empty() ? segment : prefix + "." + segment;
            node = findChild(*level, segment);
            if (node == nullptr) {
                FieldTreeNode created;
                created.segment = segment;
                created.fullName = prefix;
                level->push_back(std::move(created));
                node = &level->back();
            }
            level = &node->children;
        }

        // 同名欄位會走到同一個節點。後來的覆蓋先前的，與 PDF 的語意一致：
        // 同名就是同一個欄位，只是有多個 widget。
        node->isField = true;
        node->type = field.type;
        node->pageIndex = field.pageIndex;
        node->rectPt = field.rectPt;
        node->widgetCount += field.widgetCount;
        node->readOnly = field.readOnly;
        node->required = field.required;
    }

    return roots;
}

std::vector<FieldSummary> summarize(const std::vector<FieldDefinition>& definitions) {
    std::vector<FieldSummary> summaries;
    summaries.reserve(definitions.size());
    for (const FieldDefinition& definition : definitions) {
        FieldSummary summary;
        summary.name = definition.name;
        summary.type = definition.type;
        summary.pageIndex = definition.pageIndex;
        summary.readOnly = definition.readOnly;
        summary.required = definition.required;
        if (definition.type == BuildFieldType::RadioGroup) {
            summary.widgetCount = static_cast<std::int32_t>(definition.radios.size());
            // 單選群組沒有單一矩形，面板需要一個能「跳到這個欄位」的目標，
            // 因此取所有按鈕的聯集。取第一顆會讓面板跳到群組的一角。
            domain::RectF united{};
            for (const RadioOption& option : definition.radios) {
                united = united.united(option.rectPt.normalized());
            }
            summary.rectPt = united;
        } else {
            summary.widgetCount = 1;
            summary.rectPt = definition.rectPt.normalized();
        }
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

std::vector<FieldSummary> fieldsOnPage(const std::vector<FieldSummary>& fields,
                                       std::int32_t pageIndex) {
    std::vector<FieldSummary> filtered;
    std::copy_if(fields.begin(), fields.end(), std::back_inserter(filtered),
                 [&](const FieldSummary& f) { return f.pageIndex == pageIndex; });
    return filtered;
}

}  // namespace alioth::engine::formbuild
