// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// The queue manager drawer (plan_queue.md §4): toolbar, table, context
// menu, keyboard shortcuts, drag and drop, footer. Every command goes to
// PostQueue with the view the user saw; the panel decides nothing itself.
// Dialogs it opens hold the automatic shutdown (AGENTS.md §3.1).
//
//========================================================================

#ifndef HMI_QUEUEPANEL_H
#define HMI_QUEUEPANEL_H

#include "queue/PostQueue.h"
#include "queue/QueueExecutor.h"

#include <QFrame>
#include <QHash>

#include <functional>
#include <memory>

class QAction;
class QLabel;
class QTableView;
class QToolButton;
class QueueTableModel;

class QueuePanel : public QFrame
{
    Q_OBJECT

public:
    enum class Command {
        PostNow,
        PostNext,
        Top,
        Up,
        Down,
        Bottom,
        Hold,
        Edit,
        Duplicate,
        Remove,
        Resolve,
        AddFiles
    };

    QueuePanel(queue::PostQueue *queue, queue::QueueExecutor *executor, QWidget *parent = nullptr);

    //! Called around every dialog: keeps "shut down when done" from firing.
    void setDialogGuard(std::function<std::shared_ptr<void>()> guard) { _guard = std::move(guard); }
    QueueTableModel *model() const { return _model; }
    QTableView *table() const { return _table; }
    QAction *action(Command command) const { return _actions.value(command); }
    //! Selects the rows of these posts (tests, "show in queue").
    void selectPosts(const QList<qint64> &postIds);
    QList<qint64> selectedPostIds() const;
    QString lastMessage() const;

public slots:
    void trigger(QueuePanel::Command command);

signals:
    //! Empty list: let the user pick files.
    void addFilesRequested(const QStringList &paths);
    void editRequested(qint64 postId);
    void resolveRequested(qint64 postId);
    void expandToggled();
    void detachRequested();
    void collapseRequested();
    void countsChanged(int total, int blocked);

private:
    void buildHeader();
    void buildToolbar();
    void buildTable();
    void createActions();
    void updateActions();
    void updateFooter();
    void showContextMenu(const QPoint &pos);
    void report(const queue::QueueCommandResult &result);
    std::shared_ptr<void> holdShutdown() const;

    void postNow();
    void moveBy(int delta);
    void moveTo(const QList<qint64> &ids, int targetRow);
    void toggleHold();
    void removeSelected();
    bool confirm(const QString &title, const QString &text) const;

    queue::PostQueue *_queue;
    QueueTableModel *_model = nullptr;
    QTableView *_table = nullptr;
    QLabel *_title = nullptr;
    QLabel *_footer = nullptr;
    QLabel *_message = nullptr;
    QHash<Command, QAction *> _actions;
    std::function<std::shared_ptr<void>()> _guard;
    QList<qint64> _selectionBeforeReset;
};

inline size_t qHash(QueuePanel::Command command, size_t seed = 0) noexcept
{
    return ::qHash(static_cast<int>(command), seed);
}

#endif // HMI_QUEUEPANEL_H
