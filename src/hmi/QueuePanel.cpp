// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>

#include "QueuePanel.h"

#include "QueueIcons.h"
#include "QueueItemDelegate.h"
#include "QueueTableModel.h"

#include <QAction>
#include <QCheckBox>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

using namespace queue;
using queueicons::Glyph;

namespace
{

const char *kConfirmPostNowKey = "Queue/confirmPostNow";

QString commandError(const QueueCommandResult &result)
{
    static const QHash<QString, const char *> texts{
        { QStringLiteral("stale_view"),
          QT_TRANSLATE_NOOP("QueuePanel",
                            "The queue changed meanwhile: check the new order and try again.") },
        { QStringLiteral("store_failed"),
          QT_TRANSLATE_NOOP("QueuePanel", "The change could not be saved; nothing was changed.") },
        { QStringLiteral("before_active"),
          QT_TRANSLATE_NOOP("QueuePanel",
                            "Nothing goes before the post being sent: use Post now.") },
        { QStringLiteral("intent_pending"),
          QT_TRANSLATE_NOOP("QueuePanel",
                            "That place is kept for the post waiting to be posted now.") },
        { QStringLiteral("intent_exists"),
          QT_TRANSLATE_NOOP("QueuePanel", "Another post is already waiting to be posted now.") },
        { QStringLiteral("intent_switching"),
          QT_TRANSLATE_NOOP("QueuePanel", "The switch to that post is in progress.") },
        { QStringLiteral("manifest_engaged"),
          QT_TRANSLATE_NOOP(
              "QueuePanel",
              "Part of this post is already on Usenet: only Keep Archives can change.") },
        { QStringLiteral("still_blocked"),
          QT_TRANSLATE_NOOP("QueuePanel", "The problem is still there.") },
    };
    const char *text = texts.value(result.errorCode, nullptr);
    return text ? QCoreApplication::translate("QueuePanel", text)
                : QCoreApplication::translate("QueuePanel", "Refused (%1).").arg(result.errorCode);
}

QToolButton *toolButton(QAction *action, bool withText)
{
    auto *button = new QToolButton;
    button->setDefaultAction(action);
    button->setToolButtonStyle(withText ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    button->setAutoRaise(false);
    return button;
}

} // namespace

QueuePanel::QueuePanel(PostQueue *queue, QueueExecutor *executor, QWidget *parent)
    : QFrame(parent)
    , _queue(queue)
{
    setObjectName(QStringLiteral("queuePanel"));
    setFrameShape(QFrame::StyledPanel);
    _model = new QueueTableModel(queue, executor, this);
    createActions();
    auto *box = new QVBoxLayout(this);
    box->setContentsMargins(8, 6, 8, 6);
    box->setSpacing(6);
    buildHeader();
    buildToolbar();
    buildTable();
    _footer = new QLabel(this);
    box->addWidget(_footer);
    connect(_model, &QueueTableModel::moveRequested, this, &QueuePanel::moveTo);
    connect(_model, &QueueTableModel::filesDropped, this, &QueuePanel::addFilesRequested);
    connect(_model, &QAbstractItemModel::dataChanged, this, &QueuePanel::updateActions);
    connect(_model, &QAbstractItemModel::layoutChanged, this, &QueuePanel::updateActions);
    connect(_model, &QAbstractItemModel::modelReset, this, &QueuePanel::updateActions);
    connect(_model, &QueueTableModel::aboutToReset, this, [this] {
        _selectionBeforeReset = selectedPostIds();
    });
    connect(_model, &QueueTableModel::wasReset, this, [this] {
        selectPosts(_selectionBeforeReset);
    });
    connect(_queue, &PostQueue::changed, this, &QueuePanel::updateFooter);
    updateActions();
    updateFooter();
}

// --- construction --------------------------------------------------------------------------

void QueuePanel::createActions()
{
    const QColor ink = palette().color(QPalette::ButtonText);
    auto add = [this](Command c,
                      Glyph g,
                      const QColor &color,
                      const QString &text,
                      const QKeySequence &key) {
        auto *a = new QAction(queueicons::icon(g, color), text, this);
        a->setShortcut(key);
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        a->setToolTip(
            key.isEmpty()
                ? text
                : QStringLiteral("%1 (%2)").arg(text, key.toString(QKeySequence::NativeText)));
        connect(a, &QAction::triggered, this, [this, c] { trigger(c); });
        addAction(a);
        _actions.insert(c, a);
    };
    add(Command::PostNow,
        Glyph::Play,
        QColor(0x1f, 0x6f, 0xbf),
        tr("Post now"),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return));
    add(Command::PostNext,
        Glyph::Next,
        ink,
        tr("Post next"),
        QKeySequence(Qt::SHIFT | Qt::Key_Return));
    add(Command::Top, Glyph::Top, ink, tr("Move to top"), QKeySequence(Qt::ALT | Qt::Key_Home));
    add(Command::Up, Glyph::Up, ink, tr("Move up"), QKeySequence(Qt::ALT | Qt::Key_Up));
    add(Command::Down, Glyph::Down, ink, tr("Move down"), QKeySequence(Qt::ALT | Qt::Key_Down));
    add(Command::Bottom,
        Glyph::Bottom,
        ink,
        tr("Move to bottom"),
        QKeySequence(Qt::ALT | Qt::Key_End));
    add(Command::Hold, Glyph::Pause, ink, tr("Hold"), QKeySequence(Qt::Key_Space));
    add(Command::Edit, Glyph::Edit, ink, tr("Edit…"), QKeySequence(Qt::Key_F2));
    add(Command::Duplicate, Glyph::Plus, ink, tr("Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D));
    add(Command::Remove,
        Glyph::Trash,
        QColor(0xc2, 0x41, 0x0c),
        tr("Remove from queue"),
        QKeySequence(QKeySequence::Delete));
    add(Command::Resolve,
        Glyph::Warn,
        QColor(0xc2, 0x41, 0x0c),
        tr("Resolve…"),
        QKeySequence(Qt::Key_Return));
    add(Command::AddFiles, Glyph::Plus, ink, tr("Add files…"), QKeySequence());
}

void QueuePanel::buildHeader()
{
    auto *header = new QHBoxLayout;
    auto *icon = new QLabel(this);
    icon->setPixmap(
        queueicons::icon(Glyph::List, palette().color(QPalette::WindowText)).pixmap(16, 16));
    header->addWidget(icon);
    _title = new QLabel(this);
    QFont font = _title->font();
    font.setBold(true);
    _title->setFont(font);
    header->addWidget(_title);
    _message = new QLabel(this);
    _message->setWordWrap(true);
    header->addWidget(_message, 1);
    const QColor ink = palette().color(QPalette::ButtonText);
    auto addButton =
        [this, header](Glyph g, const QColor &c, const QString &tip, void (QueuePanel::*signal)()) {
        auto *button = new QToolButton(this);
        button->setIcon(queueicons::icon(g, c));
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setAutoRaise(true);
        connect(button, &QToolButton::clicked, this, signal);
        header->addWidget(button);
    };
    addButton(Glyph::Expand, ink, tr("Enlarge or reduce the queue"), &QueuePanel::expandToggled);
    addButton(Glyph::PopOut, ink, tr("Detach into a window"), &QueuePanel::detachRequested);
    addButton(Glyph::Chevron, ink, tr("Fold the queue"), &QueuePanel::collapseRequested);
    static_cast<QVBoxLayout *>(layout())->addLayout(header);
}

void QueuePanel::buildToolbar()
{
    auto *bar = new QHBoxLayout;
    bar->setSpacing(4);
    bar->addWidget(toolButton(_actions[Command::PostNow], true));
    bar->addWidget(toolButton(_actions[Command::PostNext], true));
    for (Command c : { Command::Top, Command::Up, Command::Down, Command::Bottom })
        bar->addWidget(toolButton(_actions[c], false));
    for (Command c : { Command::Hold, Command::Edit, Command::Remove })
        bar->addWidget(toolButton(_actions[c], true));
    bar->addStretch(1);
    bar->addWidget(toolButton(_actions[Command::AddFiles], true));
    static_cast<QVBoxLayout *>(layout())->addLayout(bar);
}

void QueuePanel::buildTable()
{
    _table = new QTableView(this);
    _table->setModel(_model);
    _table->setItemDelegate(new QueueItemDelegate(_model, _table));
    _table->setShowGrid(false);
    _table->verticalHeader()->hide();
    _table->verticalHeader()->setDefaultSectionSize(QFontMetrics(font()).height() + 14);
    _table->setSelectionBehavior(QAbstractItemView::SelectRows);
    _table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _table->setDragDropMode(QAbstractItemView::DragDrop);
    _table->setDefaultDropAction(Qt::MoveAction);
    _table->setDragDropOverwriteMode(false);
    _table->setDropIndicatorShown(true);
    _table->setContextMenuPolicy(Qt::CustomContextMenu);
    QHeaderView *h = _table->horizontalHeader();
    h->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    h->setHighlightSections(false);
    h->setSectionResizeMode(QHeaderView::Interactive);
    h->setSectionResizeMode(QueueTableModel::PostColumn, QHeaderView::Stretch);
    const QFontMetrics fm(font());
    _table->setColumnWidth(QueueTableModel::RankColumn,
                           fm.horizontalAdvance(QStringLiteral("0000")) + 34);
    _table->setColumnWidth(QueueTableModel::OriginColumn,
                           fm.horizontalAdvance(QStringLiteral("Onglet #00")) + 34);
    _table->setColumnWidth(QueueTableModel::StateColumn,
                           fm.horizontalAdvance(QStringLiteral("Suspendu · prêt")) + 48);
    _table->setColumnWidth(QueueTableModel::ProgressColumn,
                           fm.horizontalAdvance(QLatin1Char('x')) * 48);
    _table->setColumnWidth(QueueTableModel::SizeColumn,
                           fm.horizontalAdvance(QueueTableModel::sizeText(999LL << 30)) + 24);
    connect(_table, &QWidget::customContextMenuRequested, this, &QueuePanel::showContextMenu);
    connect(_table->selectionModel(),
            &QItemSelectionModel::selectionChanged,
            this,
            &QueuePanel::updateActions);
    connect(_table, &QAbstractItemView::doubleClicked, this, [this] {
        trigger(_actions[Command::Resolve]->isEnabled() ? Command::Resolve : Command::Edit);
    });
    static_cast<QVBoxLayout *>(layout())->addWidget(_table, 1);
}

// --- selection and state ---------------------------------------------------------------------

QList<qint64> QueuePanel::selectedPostIds() const
{
    QList<qint64> ids;
    const QModelIndexList rows = _table->selectionModel()->selectedRows();
    QList<int> sorted;
    for (const QModelIndex &idx : rows)
        sorted << idx.row();
    std::sort(sorted.begin(), sorted.end());
    for (int row : sorted)
        ids << _model->postIdAt(row);
    return ids;
}

void QueuePanel::selectPosts(const QList<qint64> &postIds)
{
    QItemSelection selection;
    for (qint64 id : postIds) {
        const int row = _model->rowOf(id);
        if (row >= 0)
            selection.select(_model->index(row, 0),
                             _model->index(row, QueueTableModel::ColumnCount - 1));
    }
    _table->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
}

QString QueuePanel::lastMessage() const
{
    return _message->text();
}

void QueuePanel::updateActions()
{
    const QList<qint64> ids = selectedPostIds();
    auto all = [this, &ids](Action a) {
        if (ids.isEmpty())
            return false;
        for (qint64 id : ids)
            if (!_queue->capabilityOf(a, id).allowed)
                return false;
        return true;
    };
    const bool single = ids.size() == 1;
    const bool held = single && _queue->find(ids.first()) && _queue->find(ids.first())->held;
    _actions[Command::PostNow]->setEnabled(single && all(Action::PostNow));
    _actions[Command::PostNext]->setEnabled(all(Action::PostNext));
    for (Command c : { Command::Top, Command::Up, Command::Down, Command::Bottom })
        _actions[c]->setEnabled(all(Action::Move));
    _actions[Command::Hold]->setText(held ? tr("Release") : tr("Hold"));
    _actions[Command::Hold]->setEnabled(all(held ? Action::Release : Action::Hold));
    _actions[Command::Edit]->setEnabled(single && all(Action::Edit));
    _actions[Command::Duplicate]->setEnabled(single && all(Action::Duplicate));
    _actions[Command::Remove]->setEnabled(all(Action::Remove));
    _actions[Command::Resolve]->setEnabled(single && all(Action::Revalidate));
}

void QueuePanel::updateFooter()
{
    int held = 0, blocked = 0;
    qint64 remaining = 0, temp = 0;
    const QList<QueueEntry> entries = _queue->snapshot();
    for (const QueueEntry &e : entries) {
        held += e.held ? 1 : 0;
        blocked += e.blocked.isBlocked() ? 1 : 0;
        remaining += e.sourceBytes;
        temp += e.retainedBytes();
    }
    _title->setText(tr("Queue (%1)").arg(entries.size()));
    _footer->setText(
        tr("Posts: %1 (%2 held, %3 blocked) · %4 left · temporary files: %5")
            .arg(entries.size())
            .arg(held)
            .arg(blocked)
            .arg(QueueTableModel::sizeText(remaining), QueueTableModel::sizeText(temp)));
    emit countsChanged(static_cast<int>(entries.size()), blocked);
}

void QueuePanel::report(const QueueCommandResult &result)
{
    _message->setText(result.ok() ? QString() : commandError(result));
}

std::shared_ptr<void> QueuePanel::holdShutdown() const
{
    return _guard ? _guard() : nullptr;
}

bool QueuePanel::confirm(const QString &title, const QString &text) const
{
    const auto hold = holdShutdown();
    return QMessageBox::question(const_cast<QueuePanel *>(this),
                                 title,
                                 text,
                                 QMessageBox::Yes | QMessageBox::Cancel,
                                 QMessageBox::Yes)
        == QMessageBox::Yes;
}

// --- commands ----------------------------------------------------------------------------------

void QueuePanel::trigger(Command command)
{
    const QList<qint64> ids = selectedPostIds();
    const QueueView &seen = _model->shownView();
    switch (command) {
    case Command::PostNow: postNow(); return;
    case Command::PostNext: report(_queue->postNext(ids, seen)); return;
    case Command::Top: report(_queue->moveToEdge(ids, Edge::Top, seen)); return;
    case Command::Bottom: report(_queue->moveToEdge(ids, Edge::Bottom, seen)); return;
    case Command::Up: moveBy(-1); return;
    case Command::Down: moveBy(+1); return;
    case Command::Hold: toggleHold(); return;
    case Command::Remove: removeSelected(); return;
    case Command::Duplicate:
        if (ids.size() == 1)
            report(_queue->duplicate(ids.first(), seen));
        return;
    case Command::Edit:
        if (ids.size() == 1 && _actions[Command::Edit]->isEnabled())
            emit editRequested(ids.first());
        return;
    case Command::Resolve:
        if (ids.size() == 1 && _actions[Command::Resolve]->isEnabled())
            emit resolveRequested(ids.first());
        return;
    case Command::AddFiles: emit addFilesRequested({}); return;
    }
}

void QueuePanel::moveTo(const QList<qint64> &ids, int targetRow)
{
    // The drop row counts the moving rows; PostQueue counts without them.
    int target = targetRow;
    for (qint64 id : ids) {
        const int row = _model->rowOf(id);
        if (row >= 0 && row < targetRow)
            --target;
    }
    report(_queue->move(ids, std::max(0, target), _model->shownView()));
}

void QueuePanel::moveBy(int delta)
{
    const QList<qint64> ids = selectedPostIds();
    if (ids.isEmpty())
        return;
    if (delta < 0) {
        moveTo(ids, std::max(0, _model->rowOf(ids.first()) - 1));
        return;
    }
    const int last = _model->rowOf(ids.last());
    if (last + 1 < _model->rowCount())
        moveTo(ids, last + 2);
}

void QueuePanel::toggleHold()
{
    const QList<qint64> ids = selectedPostIds();
    if (ids.isEmpty())
        return;
    const QueueEntry *first = _queue->find(ids.first());
    report(_queue->setHeld(ids, !(first && first->held), _model->shownView()));
}

void QueuePanel::removeSelected()
{
    const QList<qint64> ids = selectedPostIds();
    if (ids.isEmpty())
        return;
    bool ask = ids.size() > 10;
    for (qint64 id : ids)
        ask = ask || _queue->capabilityOf(Action::Remove, id).needsConfirmation;
    if (ask
        && !confirm(tr("Remove from the queue"),
                    tr("Remove the selected posts from the queue (%1)? Posts already started stay "
                       "resumable from History › Resume; archives are kept or deleted according to "
                       "Keep Archives.")
                        .arg(ids.size())))
        return;
    report(_queue->remove(ids, RemovePolicy::Default, _model->shownView()));
}

void QueuePanel::postNow()
{
    const QList<qint64> ids = selectedPostIds();
    if (ids.size() != 1)
        return;
    const QueueEntry *target = _queue->find(ids.first());
    const QueueEntry *active = _queue->activeEntry();
    QSettings settings;
    if (target && active && active->lifecycle == Lifecycle::Posting
        && settings.value(QLatin1String(kConfirmPostNowKey), true).toBool()) {
        const auto hold = holdShutdown();
        QMessageBox box(
            QMessageBox::Question,
            tr("Post now?"),
            tr("“%1” goes right after the post being sent%2. Then “%3” is set aside and comes back "
               "in "
               "2nd place: it resumes later without sending its confirmed articles again.")
                .arg(target->name,
                     target->isReady() ? QString()
                                       : tr(", which keeps sending while it is prepared"),
                     active->name),
            QMessageBox::Yes | QMessageBox::Cancel,
            this);
        box.button(QMessageBox::Yes)->setText(tr("Post now"));
        auto *dontAsk = new QCheckBox(tr("Don't ask again"), &box);
        box.setCheckBox(dontAsk);
        if (box.exec() != QMessageBox::Yes)
            return;
        if (dontAsk->isChecked())
            settings.setValue(QLatin1String(kConfirmPostNowKey), false);
    }
    QueueView seen = _model->shownView();
    report(_queue->postNow(ids.first(), seen, false));
}

void QueuePanel::showContextMenu(const QPoint &pos)
{
    if (selectedPostIds().isEmpty())
        return;
    QMenu menu(this);
    menu.addAction(_actions[Command::PostNow]);
    menu.addAction(_actions[Command::PostNext]);
    menu.addSeparator();
    menu.addAction(_actions[Command::Hold]);
    if (_actions[Command::Resolve]->isEnabled())
        menu.addAction(_actions[Command::Resolve]);
    QMenu *move = menu.addMenu(tr("Move"));
    for (Command c : { Command::Top, Command::Up, Command::Down, Command::Bottom })
        move->addAction(_actions[c]);
    menu.addSeparator();
    menu.addAction(_actions[Command::Edit]);
    menu.addAction(_actions[Command::Duplicate]);
    menu.addSeparator();
    menu.addAction(_actions[Command::Remove]);
    const auto hold = holdShutdown();
    menu.exec(_table->viewport()->mapToGlobal(pos));
}
