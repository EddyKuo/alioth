// 安全性原則建立與套用（PRD-SEC-003）。
//
// 「建立」（CRUD）可行；「套用」在目前技術堆疊下只有完全開放的原則能真正
// 生效，理由見 app/security_policy.h 檔頭與
// exceptions/EXC_20260906_RD_SA_set_password_unsupported.md。

#include <QtTest>

#include "app/security_policy.h"

using namespace alioth::app;
using alioth::engine::save::PermissionFlags;

class TestSecurityPolicy : public QObject {
    Q_OBJECT

private slots:
    void addAndFind() {
        SecurityPolicyStore store;
        SecurityPolicy policy;
        policy.name = "internal-review";
        policy.description = "內部審閱用途";
        QVERIFY(store.add(policy));

        const SecurityPolicy* found = store.find("internal-review");
        QVERIFY(found != nullptr);
        QCOMPARE(QString::fromStdString(found->description), QStringLiteral("內部審閱用途"));
    }

    void duplicateNameRejected() {
        SecurityPolicyStore store;
        SecurityPolicy policy;
        policy.name = "dup";
        QVERIFY(store.add(policy));
        QVERIFY(!store.add(policy));
        QCOMPARE(store.size(), std::size_t{1});
    }

    void emptyNameRejected() {
        SecurityPolicyStore store;
        SecurityPolicy policy;
        policy.name = "";
        QVERIFY(!store.add(policy));
    }

    void removeWorks() {
        SecurityPolicyStore store;
        SecurityPolicy policy;
        policy.name = "temp";
        QVERIFY(store.add(policy));
        QVERIFY(store.remove("temp"));
        QVERIFY(store.find("temp") == nullptr);
        QVERIFY(!store.remove("temp"));  // 已經不存在，第二次移除應失敗
    }

    void applyingFullyOpenPolicySucceeds() {
        SecurityPolicy policy;
        policy.name = "open";
        policy.requiresPassword = false;
        policy.permissions = PermissionFlags{};  // 預設全部允許

        const ApplyPolicyResult result = applySecurityPolicy(policy);
        QVERIFY(result.ok);
        QVERIFY(!result.blocked);
    }

    void applyingPasswordPolicyIsBlockedNotSilentlyIgnored() {
        SecurityPolicy policy;
        policy.name = "confidential";
        policy.requiresPassword = true;

        const ApplyPolicyResult result = applySecurityPolicy(policy);
        QVERIFY(!result.ok);
        QVERIFY(result.blocked);
        QVERIFY(!result.message.empty());
    }

    void applyingRestrictedPermissionPolicyIsBlocked() {
        SecurityPolicy policy;
        policy.name = "no-print";
        policy.permissions.print = false;

        const ApplyPolicyResult result = applySecurityPolicy(policy);
        QVERIFY(!result.ok);
        QVERIFY(result.blocked);
    }
};

QTEST_MAIN(TestSecurityPolicy)
#include "test_security_policy.moc"
