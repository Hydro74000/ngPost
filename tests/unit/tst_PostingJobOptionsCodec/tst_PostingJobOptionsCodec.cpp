// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The options of a queued post survive a restoration exactly, secrets never
// reach the JSON, and every persisted field has an edit class
// (plan_queue.md §3.7, §5.3).
//
//========================================================================

#include "PostingJobOptionsCodec.h"
#include "QueueTestHelpers.h"

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QJsonArray>

#include <functional>
#include <QtTest>

using queue::FieldClass;

class tst_PostingJobOptionsCodec : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir _dir;

    QString makeFile(const QString &name, const QByteArray &content = QByteArrayLiteral("data"))
    {
        QFile f(_dir.filePath(name));
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write(content);
        return f.fileName();
    }

    PersistedQueueOptions fullOptions()
    {
        PersistedQueueOptions o;
        PostingJobOptions &j = o.job;
        j.nzbFilePath = _dir.filePath(QStringLiteral("out/post.nzb"));
        j.files << QFileInfo(makeFile(QStringLiteral("a.bin"), "aaaa"))
                << QFileInfo(makeFile(QStringLiteral("b.bin"), "bbbbbbbb"));
        j.inputPaths = { _dir.filePath(QStringLiteral("a.bin")),
                         _dir.filePath(QStringLiteral("b.bin")) };
        j.grpList = { QStringLiteral("alt.binaries.a"), QStringLiteral("alt.binaries.b") };
        j.from = "poster@example.com";
        j.articleSizeBytes = 500000;
        j.obfuscateArticles = true;
        j.obfuscateFileName = true;
        j.rarArgs = QStringLiteral("-ep1 -m0 -x.DS_Store");
        j.rarSize = 50;
        j.useRarMax = true;
        j.rarMax = 42;
        j.par2Pct = 12;
        j.par2Arguments = QStringLiteral("-s1M -r12%");
        j.doCompress = true;
        j.doPar2 = true;
        j.rarName = QStringLiteral("m9Zx4Lk8Qp");
        j.rarPass = QStringLiteral("s3cr3t-archive");
        j.passwordGenerated = true;
        j.lengthName = 21;
        j.keepRar = true;
        j.delFilesAfterPost = true;
        j.overwriteNzb = false;
        j.meta.insert(QStringLiteral("title"), MetaValue(QStringLiteral("T"), MetaScope::Nzb));
        j.meta.insert(QStringLiteral("note"), MetaValue(QStringLiteral("N"), MetaScope::Local));
        j.writePostInfoFile = false;
        j.postInfoTemplate = QStringLiteral("tpl.txt");
        j.postInfoOutput = QStringLiteral("/out/info");
        j.declaredPassword = QStringLiteral("declared-secret");
        o.passwordOrigin = PasswordOrigin::Generated;
        o.passwordLength = 13;
        o.archiverFamily = QStringLiteral("rar");
        o.par2Family = QStringLiteral("parpar");
        return o;
    }

private slots:
    void initTestCase() { QVERIFY(_dir.isValid()); }

    void roundTripKeepsEveryPersistedField()
    {
        const PersistedQueueOptions in = fullOptions();
        const auto encoded = optionscodec::encode(in);
        QVERIFY2(encoded.ok(), qPrintable(encoded.error));
        const auto decoded = optionscodec::decode(encoded.json);
        QVERIFY2(decoded.ok(), qPrintable(decoded.error));
        const PersistedQueueOptions &out = decoded.options;
        QVERIFY(optionscodec::changedClasses(in, out).isEmpty()
                == false); // secrets differ, see below
        PersistedQueueOptions inWithoutSecrets = in;
        inWithoutSecrets.job.rarPass.clear();
        inWithoutSecrets.job.declaredPassword.clear();
        inWithoutSecrets.sources = out.sources; // filled by encode() from the files, checked below
        QVERIFY(optionscodec::changedClasses(inWithoutSecrets, out).isEmpty());
        QCOMPARE(optionscodec::encode(out).json, encoded.json); // encoding is stable
        QCOMPARE(out.sources.size(), 2);
        QCOMPARE(out.sources.at(0).sizeBytes, qint64(4));
        QCOMPARE(out.sources.at(1).sizeBytes, qint64(8));
        QCOMPARE(out.job.files.size(), 2);
        QCOMPARE(out.job.files.at(1).absoluteFilePath(), _dir.filePath(QStringLiteral("b.bin")));
        QCOMPARE(out.job.from, std::string("poster@example.com"));
        QCOMPARE(out.job.rarTool, QStringLiteral("rar"));
        QCOMPARE(out.job.par2Tool, par2::Tool::ParPar);
        QCOMPARE(out.passwordOrigin, PasswordOrigin::Generated);
        QCOMPARE(out.passwordLength, 13);
        QCOMPARE(out.job.meta.value(QStringLiteral("title")).scope, MetaScope::Nzb);
        QVERIFY(out.omittedFields.isEmpty());
    }

    void secretsAndRuntimeFieldsAreNeverWritten()
    {
        PersistedQueueOptions in = fullOptions();
        in.job.tmpPath = QStringLiteral("/tmp/should-not-persist");
        in.job.rarPath = QStringLiteral("/usr/bin/should-not-persist");
        in.job.par2Path = QStringLiteral("/usr/bin/par2-should-not-persist");
        in.job.resumeHistoryPostId = 77;
        in.job.originalDidCompress = true;
        const auto encoded = optionscodec::encode(in);
        QVERIFY(encoded.ok());
        const QByteArray text = QJsonDocument(encoded.json).toJson();
        QVERIFY(!text.contains("s3cr3t-archive"));
        QVERIFY(!text.contains("declared-secret"));
        QVERIFY(!text.contains("should-not-persist"));
        for (const QString &key : encoded.json.keys())
            QVERIFY2(!key.startsWith(QLatin1String("resume"))
                         && !key.startsWith(QLatin1String("original")),
                     qPrintable(key));
    }

    void archiverArgumentsAllowList_data()
    {
        QTest::addColumn<QString>("family");
        QTest::addColumn<QString>("args");
        QTest::addColumn<bool>("persistable");
        QTest::newRow("rar defaults") << "rar" << "-ep1 -m0 -x.DS_Store" << true;
        QTest::newRow("7z defaults") << "7zip" << "-mx0 -mhe=on" << true;
        QTest::newRow("empty") << "rar" << "" << true;
        QTest::newRow("rar volumes") << "rar" << "-m3 -v50m -rr5p" << true;
        QTest::newRow("rar header password") << "rar" << "-ep1 -hpsecret" << false;
        QTest::newRow("rar password") << "rar" << "-psecret -m0" << false;
        QTest::newRow("rar quoted password") << "rar" << "-m0 \"-pmy key\"" << false;
        QTest::newRow("7z password") << "7zip" << "-mx0 -psecret" << false;
        QTest::newRow("unknown switch") << "rar" << "-ep1 -zfoo" << false;
        QTest::newRow("bare value") << "7zip" << "-mx0 secret" << false;
        QTest::newRow("no family") << "" << "-m0" << false;
    }

    void archiverArgumentsAllowList()
    {
        QFETCH(QString, family);
        QFETCH(QString, args);
        QFETCH(bool, persistable);
        QCOMPARE(optionscodec::archiverArgsArePersistable(family, args), persistable);
    }

    void unsafeArchiverArgumentsAreOmittedAndFlagged()
    {
        PersistedQueueOptions in = fullOptions();
        in.job.rarArgs = QStringLiteral("-ep1 -hpTopSecret");
        const auto encoded = optionscodec::encode(in);
        QVERIFY(encoded.ok());
        QVERIFY(!encoded.json.contains(QStringLiteral("rarArgs")));
        QVERIFY(encoded.omittedFields.contains(QStringLiteral("rarArgs")));
        QVERIFY(!QJsonDocument(encoded.json).toJson().contains("TopSecret"));
        const auto decoded = optionscodec::decode(encoded.json);
        QVERIFY(decoded.ok());
        QVERIFY(decoded.options.job.rarArgs.isEmpty());
        QVERIFY(decoded.options.omittedFields.contains(QStringLiteral("rarArgs")));
    }

    void toolFamiliesMustBeResolved()
    {
        PersistedQueueOptions in = fullOptions();
        in.archiverFamily.clear();
        in.job.rarTool.clear();
        QCOMPARE(optionscodec::encode(in).error, QStringLiteral("archiver_unresolved"));
        in.job.rarTool = QStringLiteral("7zip");
        QVERIFY(optionscodec::encode(in).ok()); // the configured family is the resolved one
        in.par2Family.clear();
        in.job.par2Tool = par2::Tool::Auto;
        QCOMPARE(optionscodec::encode(in).error, QStringLiteral("par2_unresolved"));
    }

    void rejectsUnsupportedOrInvalidJson_data()
    {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::addColumn<QString>("error");
        QTest::newRow("version") << "v" << QJsonValue(2) << "unsupported_version";
        QTest::newRow("missing groups")
            << "groups" << QJsonValue(QJsonValue::Undefined) << "missing_field:groups";
        QTest::newRow("empty groups")
            << "groups" << QJsonValue(QJsonArray()) << "invalid_field:groups";
        QTest::newRow("rarSize type")
            << "rarSize" << QJsonValue(QStringLiteral("x")) << "invalid_field:rarSize";
        QTest::newRow("negative par2") << "par2Pct" << QJsonValue(-1) << "invalid_field:par2Pct";
        QTest::newRow("no source")
            << "sources" << QJsonValue(QJsonArray()) << "invalid_field:sources";
        QTest::newRow("origin") << "passwordOrigin" << QJsonValue(QStringLiteral("guessed"))
                                << "invalid_field:passwordOrigin";
        QTest::newRow("article size")
            << "articleSizeBytes" << QJsonValue(0) << "invalid_field:articleSizeBytes";
    }

    void rejectsUnsupportedOrInvalidJson()
    {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        QFETCH(QString, error);
        QJsonObject json = optionscodec::encode(fullOptions()).json;
        if (value.isUndefined())
            json.remove(field);
        else
            json.insert(field, value);
        QCOMPARE(optionscodec::decode(json).error, error);
    }

    void everyPersistedFieldHasAnEditClass()
    {
        const QStringList fields = optionscodec::persistedFields();
        QVERIFY(fields.size() >= 30);
        for (const QString &field : fields) {
            bool known = false;
            optionscodec::fieldClass(field, &known);
            QVERIFY2(known, qPrintable(field));
        }
        bool known = true;
        optionscodec::fieldClass(QStringLiteral("noSuchField"), &known);
        QVERIFY(!known);
    }

    void changedClassesFollowTheEditedField()
    {
        const PersistedQueueOptions base = queuetest::makeOptions(QStringLiteral("x"), true, true);
        auto changed = [&base](const std::function<void(PersistedQueueOptions &)> &edit) {
            PersistedQueueOptions after = base;
            edit(after);
            return optionscodec::changedClasses(base, after);
        };
        QVERIFY(changed([](PersistedQueueOptions &) {}).isEmpty());
        QCOMPARE(changed([](PersistedQueueOptions &o) { o.job.grpList << QStringLiteral("g2"); }),
                 QSet<FieldClass>{ FieldClass::TransferGeometry });
        QCOMPARE(changed([](PersistedQueueOptions &o) { o.job.obfuscateArticles = true; }),
                 QSet<FieldClass>{ FieldClass::TransferGeometry });
        QCOMPARE(changed(
                     [](PersistedQueueOptions &o) { o.job.nzbFilePath += QStringLiteral(".x"); }),
                 QSet<FieldClass>{ FieldClass::Outputs });
        QCOMPARE(changed([](PersistedQueueOptions &o) { o.job.keepRar = true; }),
                 QSet<FieldClass>{ FieldClass::Operation });
        QCOMPARE(changed([](PersistedQueueOptions &o) { o.job.rarName = QStringLiteral("n"); }),
                 QSet<FieldClass>{ FieldClass::ArtifactRecipe });
        QCOMPARE(changed([](PersistedQueueOptions &o) { o.job.rarPass = QStringLiteral("p"); }),
                 QSet<FieldClass>{ FieldClass::ArtifactRecipe });
        QCOMPARE(changed([](PersistedQueueOptions &o) {
            o.job.declaredPassword = QStringLiteral("d");
        }),
                 QSet<FieldClass>{ FieldClass::Outputs });
    }
};

QTEST_GUILESS_MAIN(tst_PostingJobOptionsCodec)
#include "tst_PostingJobOptionsCodec.moc"
