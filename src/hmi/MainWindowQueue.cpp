// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// MainWindow side of the queue manager drawer (plan_queue.md §4.1, §4.2):
// a third pane of the vertical splitter, a "Queue (N)" button and a blocked
// badge at the left of the progress bar, enlarge / detach / fold.
//
// J0: until the engine is wired (lot C3), the drawer exists only with the
// development variable NGPOST_QUEUE_PREVIEW and runs on a simulation.
//
//========================================================================

#include "MainWindow.h"
#include "ui_MainWindow.h"

#include "NgPost.h"
#include "QueueIcons.h"
#include "QueuePanel.h"
#include "queue/QueuePreview.h"

#include <QEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QToolButton>

#include <memory>
#include <numeric>
#include <optional>

namespace
{

//! Re-docks the detached drawer when its window is closed.
class RedockOnClose : public QObject
{
public:
    RedockOnClose(QObject *parent, std::function<void()> redock)
        : QObject(parent)
        , _redock(std::move(redock))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Close) {
            event->ignore();
            _redock();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> _redock;
};

} // namespace

void MainWindow::_initQueuePanel()
{
    if (!queue::QueuePreview::requested())
        return;
    _queuePreview = new queue::QueuePreview(this);
    _queuePanel = new QueuePanel(_queuePreview->queue(), _queuePreview->executor(), this);
    _queuePanel->setDialogGuard([this]() -> std::shared_ptr<void> {
        if (!_ngPost)
            return nullptr;
        using Hold = decltype(std::declval<NgPost &>().holdShutdown());
        return std::make_shared<Hold>(_ngPost->holdShutdown());
    });
    _ui->vSplitter->addWidget(_queuePanel);
    _ui->vSplitter->setStretchFactor(2, 2);

    _queueToggle = new QToolButton(this);
    _queueToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _queueToggle->setIcon(
        queueicons::icon(queueicons::Glyph::Chevron, palette().color(QPalette::ButtonText), 12));
    _queueToggle->setToolTip(tr("Show or fold the queue (Ctrl+Shift+Q)"));
    _queueToggle->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q));
    QFont bold = _queueToggle->font();
    bold.setBold(true);
    _queueToggle->setFont(bold);
    connect(_queueToggle, &QToolButton::clicked, this, [this] {
        _setQueuePanelShown(!_queuePanel->isVisible());
    });

    _queueBlockedBadge = new QToolButton(this);
    _queueBlockedBadge->setAutoRaise(true);
    _queueBlockedBadge->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _queueBlockedBadge->setIcon(
        queueicons::icon(queueicons::Glyph::Warn, QColor(0xc2, 0x41, 0x0c), 14));
    _queueBlockedBadge->setToolTip(tr("Blocked posts: show the queue"));
    connect(_queueBlockedBadge, &QToolButton::clicked, this, [this] { _setQueuePanelShown(true); });
    if (auto *bar = qobject_cast<QHBoxLayout *>(_ui->uploadFrame->layout())) {
        bar->insertWidget(0, _queueBlockedBadge);
        bar->insertWidget(0, _queueToggle);
    }

    connect(_queuePanel, &QueuePanel::countsChanged, this, &MainWindow::_onQueueCounts);
    connect(_queuePanel, &QueuePanel::collapseRequested, this, [this] {
        _setQueuePanelShown(false);
    });
    connect(_queuePanel, &QueuePanel::expandToggled, this, &MainWindow::_toggleQueueExpanded);
    connect(_queuePanel, &QueuePanel::detachRequested, this, &MainWindow::_detachQueuePanel);
    connect(_queuePanel, &QueuePanel::addFilesRequested, this, [this](const QStringList &dropped) {
        QStringList paths = dropped;
        if (paths.isEmpty()) {
            const auto hold = _ngPost ? std::make_optional(_ngPost->holdShutdown()) : std::nullopt;
            paths = QFileDialog::getOpenFileNames(this, tr("Add files to the queue"));
        }
        _queuePreview->addPaths(paths);
    });
    _queuePreview->addDemoPosts();
    _onQueueCounts(static_cast<int>(_queuePreview->queue()->snapshot().size()), 0);
}

void MainWindow::_onQueueCounts(int total, int blocked)
{
    if (!_queueToggle)
        return;
    _queueToggle->setText(tr("Queue (%1)").arg(total));
    _queueBlockedBadge->setText(QString::number(blocked));
    _queueBlockedBadge->setVisible(blocked > 0);
}

void MainWindow::_setQueuePanelShown(bool shown)
{
    if (!_queuePanel)
        return;
    if (_queuePanel->isWindow()) {
        _queuePanel->raise();
        _queuePanel->activateWindow();
        return;
    }
    _queuePanel->setVisible(shown);
    _queueToggle->setIcon(
        queueicons::icon(queueicons::Glyph::Chevron, palette().color(QPalette::ButtonText), 12));
}

void MainWindow::_toggleQueueExpanded()
{
    if (!_queuePanel || _queuePanel->isWindow())
        return;
    if (_queueSizesBeforeExpand.isEmpty()) {
        _queueSizesBeforeExpand = _ui->vSplitter->sizes();
        const int total = std::accumulate(_queueSizesBeforeExpand.cbegin(),
                                          _queueSizesBeforeExpand.cend(),
                                          0);
        _ui->vSplitter->setSizes({ 0, total / 6, total - total / 6 });
    } else {
        _ui->vSplitter->setSizes(_queueSizesBeforeExpand);
        _queueSizesBeforeExpand.clear();
    }
}

void MainWindow::_detachQueuePanel()
{
    if (!_queuePanel || _queuePanel->isWindow())
        return;
    _queueSizesBeforeExpand.clear();
    _queuePanel->setParent(nullptr, Qt::Window);
    _queuePanel->setWindowTitle(tr("ngPost queue"));
    _queuePanel->installEventFilter(
        new RedockOnClose(_queuePanel, [this] { _redockQueuePanel(); }));
    _queuePanel->resize(width() * 3 / 4, height() / 2);
    _queuePanel->show();
}

void MainWindow::_redockQueuePanel()
{
    if (!_queuePanel || !_queuePanel->isWindow())
        return;
    for (QObject *child : _queuePanel->children())
        if (dynamic_cast<RedockOnClose *>(child)) {
            _queuePanel->removeEventFilter(child);
            child->deleteLater();
        }
    _ui->vSplitter->addWidget(_queuePanel); // reparents and turns it back into a child widget
    _queuePanel->show();
}
