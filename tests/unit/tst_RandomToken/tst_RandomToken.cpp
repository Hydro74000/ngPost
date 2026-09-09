#include <QtTest>
#include "../../../src/utils/RandomToken.h"

class TestRandomToken : public QObject {
    Q_OBJECT
private slots:
    void tokens() {
        QVERIFY(RandomToken::secret(0).isEmpty());
        const QRegularExpression alphabet(QStringLiteral("^[A-Za-z0-9]+$"));
        for (uint length : {1u, 13u, 62u, 1024u}) {
            const QString password = RandomToken::secret(length);
            QCOMPARE(password.size(), qsizetype(length));
            QVERIFY(alphabet.match(password).hasMatch());
            QCOMPARE(RandomToken::publicName(length).size(), password.size());
        }
    }
};
QTEST_APPLESS_MAIN(TestRandomToken)
#include "tst_RandomToken.moc"
