// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The queue manager drawer: rows follow the queue, actions follow the
// capabilities of the selection, toolbar / keyboard / drag and drop all go
// through PostQueue with the view that was displayed (plan_queue.md §4).
//
//========================================================================

#include "QueueTestHelpers.h"
#include "hmi/QueuePanel.h"
#include "hmi/QueueTableModel.h"
#include "queue/FakeQueueExecutor.h"
#include "queue/InMemoryQueueStore.h"

#include <QAction>
#include <QLabel>
#include <QMimeData>
#include <QTableView>
#include <QtTest>

#include <memory>

using namespace queue;
using queuetest::makeOptions;
using queuetest::settle;

class tst_QueuePanel : public QObject
{
    Q_OBJECT

private:
    struct Fixture
    {
        InMemoryQueueStore store;
        PostQueue queue{ &store, QStringLiteral("session") };
        FakeQueueExecutor executor;
        std::unique_ptr<QueuePanel> panel;

        Fixture() { panel = std::make_unique<QueuePanel>(&queue, &executor); }
        qint64 add(const QString &name, bool compress = false)
        {
            const qint64 id = queue.submit(makeOptions(name, compress), Origin::Auto)
                                  .postIds.value(0);
            settle();
            return id;
        }
        QueueTableModel *model() const { return panel->model(); }
        QString cell(int row, int column) const
        {
            return model()->index(row, column).data().toString();
        }
        void select(const QList<qint64> &ids)
        {
            settle();
            panel->selectPosts(ids);
        }
        bool enabled(QueuePanel::Command c) const { return panel->action(c)->isEnabled(); }
    };

private slots:
    void rowsFollowTheQueue()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("alpha"));
        const qint64 b = f.add(QStringLiteral("beta"), true);
        QCOMPARE(f.model()->rowCount(), 2);
        QCOMPARE(f.cell(0, QueueTableModel::PostColumn), QStringLiteral("alpha"));
        QCOMPARE(f.cell(1, QueueTableModel::StateColumn), QStringLiteral("Queued"));
        QVERIFY(f.queue.engineActivate(b));
        settle();
        QCOMPARE(f.model()->postIdAt(0), b);
        QCOMPARE(f.cell(0, QueueTableModel::StateColumn), QStringLiteral("Posting"));
        QCOMPARE(f.cell(1, QueueTableModel::RankColumn), QStringLiteral("2"));
        QCOMPARE(f.model()->postIdAt(1), a);
    }

    void actionsFollowTheSelectedEntries()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        QVERIFY(f.queue.engineActivate(a));
        f.select({ a });
        QVERIFY(!f.enabled(QueuePanel::Command::Up));
        QVERIFY(!f.enabled(QueuePanel::Command::PostNow));
        QVERIFY(f.enabled(QueuePanel::Command::Hold));
        QVERIFY(f.enabled(QueuePanel::Command::Remove));
        f.select({ b });
        QVERIFY(f.enabled(QueuePanel::Command::PostNow));
        QVERIFY(f.enabled(QueuePanel::Command::Edit));
        f.select({ a, b });
        QVERIFY(!f.enabled(QueuePanel::Command::PostNow)); // single target only
        QVERIFY(!f.enabled(QueuePanel::Command::Down));    // the active cannot move
    }

    void toolbarCommandsGoThroughTheQueue()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        const qint64 c = f.add(QStringLiteral("c"));
        QVERIFY(f.queue.engineActivate(a));
        f.select({ c });
        f.panel->trigger(QueuePanel::Command::PostNext);
        settle();
        QCOMPARE(f.queue.order(), (QList<qint64>{ a, c, b }));
        QCOMPARE(f.panel->selectedPostIds(), QList<qint64>{ c }); // selection follows the post
        f.panel->trigger(QueuePanel::Command::Bottom);
        settle();
        QCOMPARE(f.queue.order(), (QList<qint64>{ a, b, c }));
        f.panel->trigger(QueuePanel::Command::Up);
        settle();
        QCOMPARE(f.queue.order(), (QList<qint64>{ a, c, b }));
        f.panel->trigger(QueuePanel::Command::Up); // never before the active entry
        settle();
        QCOMPARE(f.queue.order(), (QList<qint64>{ a, c, b }));
        QVERIFY(!f.panel->lastMessage().isEmpty());
    }

    void dropRowsBecomeQueuePositions()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        const qint64 c = f.add(QStringLiteral("c"));
        const qint64 d = f.add(QStringLiteral("d"));
        auto drop = [&f](int sourceRow, int targetRow) {
            std::unique_ptr<QMimeData> mime(
                f.model()->mimeData({ f.model()->index(sourceRow, 0) }));
            f.model()->dropMimeData(mime.get(), Qt::MoveAction, targetRow, 0, {});
            settle();
        };
        drop(3, 1); // d dropped between a and b
        QCOMPARE(f.queue.order(), (QList<qint64>{ a, d, b, c }));
        drop(0, 3); // a dropped between b and c
        QCOMPARE(f.queue.order(), (QList<qint64>{ d, b, a, c }));
    }

    void aStaleViewIsReportedNotApplied()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        f.select({ a });
        QVERIFY(f.queue.move({ b }, 0, f.queue.view()).ok()); // changed behind the view's back
        f.panel->trigger(QueuePanel::Command::Bottom);        // before the view refreshed
        QVERIFY(f.panel->lastMessage().contains(QStringLiteral("changed meanwhile")));
        QCOMPARE(f.queue.order(), (QList<qint64>{ b, a }));
    }

    void holdTogglesAndRemoveNeedsNoDialogForPlainEntries()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        f.add(QStringLiteral("b"));
        f.select({ a });
        QCOMPARE(f.panel->action(QueuePanel::Command::Hold)->text(), QStringLiteral("Hold"));
        f.panel->trigger(QueuePanel::Command::Hold);
        f.select({ a });
        QVERIFY(f.queue.find(a)->held);
        QCOMPARE(f.panel->action(QueuePanel::Command::Hold)->text(), QStringLiteral("Release"));
        QCOMPARE(f.cell(0, QueueTableModel::StateColumn), QStringLiteral("Held"));
        f.panel->trigger(QueuePanel::Command::Remove);
        settle();
        QVERIFY(!f.queue.find(a));
        QCOMPARE(f.model()->rowCount(), 1);
    }

    void footerCountsTheQueue()
    {
        Fixture f;
        f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        QVERIFY(f.queue.engineBlock(b, { QStringLiteral("resources"), {} }, Lifecycle::Queued));
        settle();
        const QString footer = f.panel->findChildren<QLabel *>().last()->text();
        QVERIFY2(footer.startsWith(QStringLiteral("Posts: 2 (0 held, 1 blocked)")),
                 qPrintable(footer));
        QCOMPARE(f.cell(1, QueueTableModel::ProgressColumn),
                 QStringLiteral("Not enough temporary space"));
    }

    void livePreparationProgressIsShown()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        QVERIFY(f.queue.engineBeginPreparation(a, { QStringLiteral("s"), 1, 0, 0, QString() }));
        emit f.executor.stepProgress(a, PrepStep::Par2, 42);
        settle();
        QCOMPARE(f.cell(0, QueueTableModel::ProgressColumn), QStringLiteral("PAR2 42 %"));
        QCOMPARE(f.model()->index(0, 0).data(QueueTableModel::PercentRole).toInt(), 42);
    }

    //! Renders the real drawer for a visual review; runs only on demand.
    void renderForReview()
    {
        const QString path = qEnvironmentVariable("NGPOST_QUEUE_SHOT");
        if (path.isEmpty())
            QSKIP("set NGPOST_QUEUE_SHOT=<file.png> to render the drawer");
        Fixture f;
        const qint64 a = f.add(QStringLiteral("Linux_Mint_22_Cinnamon_x64"));
        const qint64 b = f.add(QStringLiteral("Debian_13_Netinst"), true);
        const qint64 c = f.add(QStringLiteral("Archlinux-2026.11.01-x86_64"), true);
        f.add(QStringLiteral("Fedora-Workstation-Live-43"), true);
        const qint64 e = f.add(QStringLiteral("Ubuntu-26.04-Desktop-amd64"), true);
        const qint64 g = f.add(QStringLiteral("FreeBSD_15_Release_amd64"));
        QVERIFY(f.queue.engineActivate(a));
        QVERIFY(f.queue.engineBeginPreparation(b, { QStringLiteral("s1"), 1, 0, 0, QString() }));
        QVERIFY(
            f.queue.engineRecordPrepared(c, { QStringLiteral("s2"), 1, 0, 1200000000, QString() }));
        QVERIFY(
            f.queue.engineBlock(e, { QStringLiteral("password_required"), {} }, Lifecycle::Queued));
        QVERIFY(f.queue.engineInterrupt(g, true, true, true));
        emit f.executor.progress(a, 64, 71700000);
        emit f.executor.stepProgress(b, PrepStep::Compress, 48);
        settle();
        f.select({ f.model()->postIdAt(3) });
        f.panel->resize(1500, 420);
        f.panel->show();
        settle();
        QVERIFY(f.panel->grab().save(path));
    }
};

QTEST_MAIN(tst_QueuePanel)
#include "tst_QueuePanel.moc"
