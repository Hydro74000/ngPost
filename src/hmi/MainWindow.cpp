//========================================================================
//
// Copyright (C) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3..
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>
//
//========================================================================

#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "PostingWidget.h"
#include "AutoPostWidget.h"
#include "NgPost.h"
#include "VpnSettingsDialog.h"
#include "history/PostHistoryService.h"
#include "history/PostHistoryStore.h"
#include "nntp/NntpServerParams.h"
#include "nntp/NntpArticle.h"
#include "vpn/VpnManager.h"

#include <QCoreApplication>
#include <QAbstractScrollArea>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QLineEdit>
#include <QProgressBar>
#include <QProgressDialog>
#include <QHBoxLayout>
#include <QPainter>
#include <QScrollArea>
#include <QShortcut>
#include <QKeySequence>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextStream>
#include <QVBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QSizePolicy>
#include <QStatusBar>
#include <QtCharts/QAbstractAxis>
#include <QtCharts/QBarCategoryAxis>
#include <QtCharts/QBarSeries>
#include <QtCharts/QBarSet>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QValueAxis>
#include <QDate>
#include <QDateEdit>
#include <QDesktopServices>
#include "utils/UpdateChecker.h"

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
QT_CHARTS_USE_NAMESPACE
#endif


const QColor  MainWindow::sPostingColor = QColor(255,162, 0); // gold (#FFA200)
const QString MainWindow::sPostingIcon  = ":/icons/uploading.png";
const QColor  MainWindow::sPendingColor = Qt::darkBlue;
const QString MainWindow::sPendingIcon  = ":/icons/pending.png";
const QColor  MainWindow::sDoneOKColor  = Qt::darkGreen;
const QString MainWindow::sDoneOKIcon   = ":/icons/ok.png";
const QColor  MainWindow::sDoneKOColor  = Qt::darkRed;
const QString MainWindow::sDoneKOIcon   = ":/icons/ko.png";
const QColor  MainWindow::sArticlesFailedColor  = Qt::darkYellow;


const QList<const char *> MainWindow::sServerListHeaders = {
    QT_TRANSLATE_NOOP("MainWindow", "on"),
    QT_TRANSLATE_NOOP("MainWindow", "Host (name or IP)"),
    QT_TRANSLATE_NOOP("MainWindow", "Port"),
    QT_TRANSLATE_NOOP("MainWindow", "SSL"),
    QT_TRANSLATE_NOOP("MainWindow", "Use VPN"),
    QT_TRANSLATE_NOOP("MainWindow", "Connections"),
    QT_TRANSLATE_NOOP("MainWindow", "Username"),
    QT_TRANSLATE_NOOP("MainWindow", "Password"),
    "" // for the delete button
};
const QVector<int> MainWindow::sServerListSizes   = {30, 200, 50, 30, 60, 100, 150, 150, sDeleteColumnWidth};

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    _ui(new Ui::MainWindow),
    _ngPost(nullptr),
    _state(STATE::IDLE),
    _quickJobTab(nullptr),
    _autoPostTab(nullptr)
{
    setAcceptDrops(true);

    _ui->setupUi(this);

    _ui->serverBox->setStyleSheet(sGroupBoxStyle);
    _ui->fileBox->setStyleSheet(sGroupBoxStyle);
    _ui->postingBox->setStyleSheet(sGroupBoxStyle);
    _ui->logBox->setStyleSheet(sGroupBoxStyle);

    _ui->hSplitter->setStretchFactor(0, 1);
    _ui->hSplitter->setStretchFactor(1, 1);

    _ui->vSplitter->setStretchFactor(0, 1);
    _ui->vSplitter->setStretchFactor(1, 3);
    _ui->vSplitter->setCollapsible(1, false);

    _ui->postSplitter->setStretchFactor(0, 3);
    _ui->postSplitter->setStretchFactor(1, 1);
    _ui->postSplitter->setCollapsible(0, false);


    _ui->progressBar->setRange(0, 100);
    updateProgressBar(0, 0, "");

    QSize screenSize = qApp->screens()[0]->size();
    resize(screenSize * 0.8);
    setWindowIcon(QIcon(":/icons/ngPost.png"));
    setGeometry((screenSize.width() - width())/2,  (screenSize.height() - height())/2, width(), height());

    connect(_ui->clearLogButton, &QAbstractButton::clicked, _ui->logBrowser, &QTextEdit::clear);
    connect(_ui->debugBox,       &QAbstractButton::toggled, this,            &MainWindow::onDebugToggled);
    connect(_ui->pauseButton,    &QAbstractButton::clicked, this,            &MainWindow::onPauseClicked);

#if QT_VERSION < QT_VERSION_CHECK(5, 12, 0)
    connect(_ui->debugSB, static_cast<void(QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, &MainWindow::onDebugValue);
#else
    connect(_ui->debugSB,   qOverload<int>(&QSpinBox::valueChanged),   this,    &MainWindow::onDebugValue);
#endif
}

MainWindow::~MainWindow()
{
    delete _ui;
}

void MainWindow::init(NgPost *ngPost)
{
    _ngPost = ngPost;

    _quickJobTab = new PostingWidget(ngPost, this, 1);
    _autoPostTab = new AutoPostWidget(ngPost, this);

    _ui->debugBox->setChecked(_ngPost->debugMode());
    _ui->debugSB->setEnabled(_ngPost->debugMode());

    QTabBar *tabBar = _ui->postTabWidget->tabBar();
    tabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    tabBar->setElideMode(Qt::TextElideMode::ElideNone);
    tabBar->setIconSize({18, 18});

    _ui->postTabWidget->clear();
    _ui->postTabWidget->setUsesScrollButtons(true);
    _ui->postTabWidget->setStyleSheet(sTabWidgetStyle);
    _ui->postTabWidget->addTab(_quickJobTab, QIcon(":/icons/quick.png"), _ngPost->quickJobName());
    tabBar->setTabToolTip(0, tr("Default %1").arg(_ngPost->quickJobName()));
    _ui->postTabWidget->addTab(_autoPostTab, QIcon(":/icons/auto.png"), _ngPost->folderMonitoringName());
    tabBar->setTabToolTip(1, _ngPost->folderMonitoringName());
    _ui->postTabWidget->addTab(_buildHistoryTab(), QIcon(":/icons/monitor.png"), tr("History"));
    tabBar->setTabToolTip(2, tr("Post history, statistics and resume center"));
    _ui->postTabWidget->addTab(new QWidget(_ui->postTabWidget), QIcon(":/icons/plus.png"), tr("New"));
    tabBar->setTabToolTip(3, QString("Create a new %1").arg(_ngPost->quickJobName()));

//    connect(_ui->postTabWidget,           &QTabWidget::currentChanged, this, &MainWindow::onJobTabClicked);
    connect(tabBar, &QTabBar::tabBarClicked,              this, &MainWindow::onJobTabClicked);
    connect(tabBar, &QWidget::customContextMenuRequested, this, &MainWindow::onTabContextMenu);
    connect(tabBar, &QTabBar::tabCloseRequested,          this, &MainWindow::onCloseJob);
    _ui->postTabWidget->setTabsClosable(true);
    _ui->postTabWidget->installEventFilter(this);
//    _ui->postTabWidget->setCurrentIndex(1);

    setJobLabel(1);


    for (const QString &lang : _ngPost->languages())
        _ui->langCB->addItem(QIcon(QString(":/icons/flag_%1.png").arg(lang.toUpper())), lang.toUpper(), lang);
    _ui->langCB->setCurrentText(_ngPost->_lang.toUpper());
    connect(_ui->langCB, &QComboBox::currentTextChanged, this, &MainWindow::onLangChanged);

    _initServerBox();
    _initPostingBox();
    _quickJobTab->init();
    _autoPostTab->init();

    _ui->goCmdButton->hide();
//    connect(_ui->goCmdButton, &QAbstractButton::clicked, _ngPost, &NgPost::onGoCMD, Qt::QueuedConnection);

    updateProgressBar(0, 0);
}

#ifdef NGPOST_TESTING
QWidget *MainWindow::buildHistoryTabForTest()
{
    return _buildHistoryTab();
}
#endif


void MainWindow::updateProgressBar(uint nbArticlesTotal, uint nbArticlesUploaded, const QString &avgSpeed
                                   #ifdef __COMPUTE_IMMEDIATE_SPEED__
                                       , const QString &immediateSpeed
                                   #endif
                                   )
{
//    qDebug() << "[MainWindow::updateProgressBar] _nbArticlesUploaded: " << nbArticlesUploaded;
    _ui->progressBar->setValue(static_cast<int>(nbArticlesUploaded));

#ifdef __COMPUTE_IMMEDIATE_SPEED__
    _ui->uploadLbl->setText(QString("%5 (%1 / %2) %3: %4").arg(
                                nbArticlesUploaded).arg(
                                nbArticlesTotal).arg(
                                tr("avg speed")).arg(
                                avgSpeed).arg(
                                immediateSpeed));
#else
    _ui->uploadLbl->setText(QString("(%1 / %2) %3: %4").arg(
                                nbArticlesUploaded).arg(
                                nbArticlesTotal).arg(
                                tr("avg speed")).arg(
                                avgSpeed));
#endif
}


void MainWindow::log(const QString &aMsg, bool newline) const
{
    if (newline)
        _ui->logBrowser->append(aMsg);
    else
    {
        _ui->logBrowser->insertPlainText(aMsg);
        _ui->logBrowser->moveCursor(QTextCursor::End);
    }
}

void MainWindow::logError(const QString &error) const
{
    _ui->logBrowser->append(QString("<font color='red'>%1</font><br/>\n").arg(error));
}

bool MainWindow::useFixedPassword() const
{
    return _ui->rarPassCB->isChecked();
}

bool MainWindow::hasAutoCompress() const
{
    return _ui->autoCompressCB->isChecked();
}

#include <QKeyEvent>
bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress && obj == _ui->postTabWidget)
    {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        qDebug() << "[MainWindow] getting key event: " << keyEvent->key();
        int currentTabIdx = _ui->postTabWidget->currentIndex();
        if (currentTabIdx == 1)
            static_cast<AutoPostWidget*>(_ui->postTabWidget->currentWidget())->handleKeyEvent(keyEvent);
        else if (PostingWidget *postWidget = _getPostWidget(currentTabIdx))
            postWidget->handleKeyEvent(keyEvent);
    }
    return QObject::eventFilter(obj, event);
}

#include <QMimeData>
void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    int currentTabIdx = _ui->postTabWidget->currentIndex();
    if (currentTabIdx == 1)
        _autoPostTab->handleDropEvent(e);
    else if (PostingWidget *postWidget = qobject_cast<PostingWidget*>(_ui->postTabWidget->currentWidget()))
        postWidget->handleDropEvent(e);
}


void MainWindow::closeEvent(QCloseEvent *event)
{
    if (_ngPost->hasPostingJobs())
    {
        int res = QMessageBox::question(this,
                                        tr("close while still posting?"),
                                        tr("ngPost is currently posting.\nAre you sure you want to quit?"),
                                        QMessageBox::Yes,
                                        QMessageBox::No);
        if (res == QMessageBox::Yes)
        {
            _ngPost->closeAllPostingJobs();
            event->accept();
        }
        else
        {
            event->ignore();
            return;
        }
    }
    else
        event->accept();

    // Phase 3 — tear down the VPN here, while the event loop is still
    // alive (waitForFinished needs it). If we let ~VpnManager handle it
    // in the destructor chain, the event loop is already gone and the
    // helper script doesn't get the EOF signal in time, leaving an
    // orphan tunnel that gets detected as "stale state" on next launch.
    if (VpnManager *vpn = _ngPost->vpnManager()) {
        if (vpn->state() == VpnManager::State::Connected
            || vpn->state() == VpnManager::State::Starting) {
            vpn->stop();
            // Spin the event loop briefly so the helper's stdin closure is
            // processed and its trap can run cleanup.
            QElapsedTimer t; t.start();
            while (vpn->state() != VpnManager::State::Disabled
                   && t.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            }
        }
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    if(event)
    {
        QStringList serverTableHeader;
        QTabBar *tabBar = _ui->postTabWidget->tabBar();
        int lastTabIdx = tabBar->count() - 1;
        switch(event->type()) {
        // this event is send if a translator is loaded
        case QEvent::LanguageChange:
            qDebug() << "MainWindow::changeEvent";
            _ui->retranslateUi(this);
#ifdef __COMPUTE_IMMEDIATE_SPEED__
            _ui->uploadLbl->setToolTip(tr("Immediate speed (avg on %1 sec) - (nb Articles uploaded / total number of Articles) - avg speed").arg(NgPost::immediateSpeedDuration()));
#endif
            _ui->shutdownCB->setToolTip(tr("Shutdown computer when all the current Posts are done (with command: %1)").arg(
                                            _ngPost->_shutdownCmd));

            _ui->serverBox->setTitle(tr("Servers"));
            _ui->fileBox->setTitle(tr("Files"));
            _ui->postingBox->setTitle(tr("Parameters"));
            _ui->logBox->setTitle(tr("Posting Log"));

            tabBar->setTabText(0, _ngPost->quickJobName());
            tabBar->setTabToolTip(0, tr("Default %1").arg(_ngPost->quickJobName()));
            tabBar->setTabText(1, _ngPost->folderMonitoringName());
            tabBar->setTabToolTip(1, _ngPost->folderMonitoringName());
            tabBar->setTabText(2, tr("History"));
            tabBar->setTabToolTip(2, tr("Post history, statistics and resume center"));
            for (int i = 3 ; i < lastTabIdx; ++i)
                if (_getPostWidget(i))
                    tabBar->setTabText(i, _ngPost->quickJobName());
            tabBar->setTabText(lastTabIdx, tr("New"));
            tabBar->setTabToolTip(lastTabIdx, QString("Create a new %1").arg(_ngPost->quickJobName()));

            setJobLabel(_ui->postTabWidget->currentIndex());

            for (const char *header : sServerListHeaders)
                serverTableHeader << tr(header);
            _ui->serversTable->setHorizontalHeaderLabels(serverTableHeader);


            _quickJobTab->retranslate();
            _autoPostTab->retranslate();
            for (int i = 2 ; i < _ui->postTabWidget->count() - 1; ++i)
                if (PostingWidget *postWidget = _getPostWidget(i))
                    postWidget->retranslate();
            _retranslateHistoryTab();
            break;

            // this event is send, if the system, language changes
        default:
            break;
        }
    }
    QMainWindow::changeEvent(event);
}



#include "CheckBoxCenterWidget.h"
void MainWindow::onAddServer()
{
    _addServer(nullptr);
}

void MainWindow::onDelServer()
{
    QObject *delButton = sender();
    int row = _serverRow(delButton);
    if (row < _ui->serversTable->rowCount())
        _ui->serversTable->removeRow(row);

    NntpServerParams *serverParam = static_cast<NntpServerParams*>(delButton->property("server").value<void*>());
    if (serverParam)
    {
        _ngPost->removeNntpServer(serverParam);
        delete serverParam;
    }
}

void MainWindow::onObfucateToggled(bool checked)
{
    bool enabled = !checked;
    _ui->fromEdit->setEnabled(enabled);
    _ui->genPoster->setEnabled(enabled);
    _ui->saveFromCB->setEnabled(enabled);
    _ui->uniqueFromCB->setEnabled(enabled);
}

void MainWindow::onTabContextMenu(const QPoint &point)
{
//    qDebug() << "MainWindow::onTabContextMenu: " << point;
    if (point.isNull())
        return;

//    QTabBar *tabBar = _ui->postTabWidget->tabBar();
//    int tabIndex = tabBar->tabAt(point);
//    PostingWidget *currentPostWidget = _getPostWidget(tabIndex);
    QMenu menu(tr("Quick Tabs Menu"), this);
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    QAction *action = menu.addAction(QIcon(":/icons/clear.png"), tr("Close All finished Tabs"), this, &MainWindow::onCloseAllFinishedQuickTabs);
#else
    QAction *action = menu.addAction(QIcon(":/icons/clear.png"), tr("Close All finished Tabs"), this, SLOT(onCloseAllFinishedQuickTabs));
#endif
    action->setEnabled(hasFinishedPosts());
    menu.exec(QCursor::pos());
}

bool MainWindow::hasFinishedPosts() const
{
    for (int idx = 2 ; idx < _ui->postTabWidget->count() - 2 ; ++idx)
    {
        PostingWidget *postWidget = _getPostWidget(idx);
        if (postWidget && postWidget->isPostingFinished())
            return true;
    }
    return false;
}

void MainWindow::onCloseAllFinishedQuickTabs()
{
    // go backwards as we may delete the current tab ;)
    for (int idx = _ui->postTabWidget->count() - 2 ; idx > 1  ; --idx)
    {
        PostingWidget *postWidget = _getPostWidget(idx);
        if (postWidget && postWidget->isPostingFinished())
            onCloseJob(idx);
    }
}

void MainWindow::onSetProgressBarRange(int nbArticles)
{
    qDebug() << "MainWindow::onSetProgressBarRange: " << nbArticles;
    _ui->progressBar->setRange(0, nbArticles);
}

void MainWindow::onNewVersionAvailable(const QString &tag, const QString &notes, const QUrl &releasePage)
{
    UpdateChecker *uc = _ngPost->updateChecker();
    if (!uc)
        return;

    QString notesPreview = notes.left(800);
    if (notes.size() > 800)
        notesPreview += "...";
    notesPreview.replace('<', "&lt;").replace('>', "&gt;").replace('\n', "<br/>");

    QString body = tr("<h3>New version available: <b>ngPost %1</b></h3>"
                      "<p>Current: v%2</p>"
                      "<p><a href=\"%3\">View release on GitHub</a></p>")
                       .arg(tag, NgPost::sVersion, releasePage.toString());
    if (!notesPreview.isEmpty())
        body += "<hr/><div style='font-size:small'>" + notesPreview + "</div>";

    QMessageBox box(this);
    box.setWindowTitle(tr("New version available"));
    box.setTextFormat(Qt::RichText);
    box.setTextInteractionFlags(Qt::TextBrowserInteraction);
    box.setText(body);
    QPushButton *install = box.addButton(tr("Install and Restart"), QMessageBox::AcceptRole);
    box.addButton(tr("Later"), QMessageBox::RejectRole);
    box.setDefaultButton(install);
    box.exec();
    if (box.clickedButton() != install)
        return;

    auto *progress = new QProgressDialog(tr("Downloading update..."), tr("Cancel"), 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setMinimumDuration(0);
    progress->setValue(0);

    connect(uc, &UpdateChecker::downloadProgress, progress,
            [progress](qint64 received, qint64 total) {
                if (total > 0)
                {
                    progress->setMaximum(static_cast<int>(total));
                    progress->setValue(static_cast<int>(received));
                }
            });
    connect(uc, &UpdateChecker::installStarting, progress, &QProgressDialog::close);
    connect(uc, &UpdateChecker::downloadFailed, this,
            [this, progress](const QString &msg) {
                progress->close();
                QMessageBox::warning(this, tr("Update failed"), msg);
            });
    connect(progress, &QProgressDialog::canceled, uc, [uc]() {
        // Nothing wires this to abort the QNetworkReply for now; close the dialog is enough.
        Q_UNUSED(uc);
    });

    uc->startDownloadAndInstall();
}


void MainWindow::_initServerBox()
{
    _ui->serversTable->verticalHeader()->hide();
    _ui->serversTable->setColumnCount(sServerListHeaders.size());

    int width = 2, col = 0;
    for (int size : sServerListSizes)
    {
        _ui->serversTable->setColumnWidth(col++, size);
        width += size;
    }
//    _ui->serversTable->setMaximumWidth(width);

    connect(_ui->addServerButton,   &QAbstractButton::clicked, this, &MainWindow::onAddServer);

    for (NntpServerParams *srv : _ngPost->_nntpServers)
        _addServer(srv);
}


void MainWindow::_initPostingBox()
{
    connect(_ui->shutdownCB,        &QAbstractButton::toggled, this, &MainWindow::onShutdownToggled);
    connect(_ui->saveButton,        &QAbstractButton::clicked, this, &MainWindow::onSaveConfig);

    connect(_ui->genPoster,         &QAbstractButton::clicked, this, &MainWindow::onGenPoster);
    connect(_ui->obfuscateMsgIdCB,  &QAbstractButton::toggled, this, &MainWindow::onObfucateToggled);
    connect(_ui->uniqueFromCB,      &QAbstractButton::toggled, this, &MainWindow::onUniqueFromToggled);
    connect(_ui->rarPassCB,         &QAbstractButton::toggled, this, &MainWindow::onRarPassToggled);
    connect(_ui->genPass,           &QAbstractButton::clicked, this, &MainWindow::onArchivePass);
    connect(_ui->autoCompressCB,    &QAbstractButton::toggled, this, &MainWindow::onAutoCompressToggled);
    connect(_ui->rarPassEdit,       &QLineEdit::textChanged,   this, &MainWindow::onRarPassUpdated);

    _ui->fromEdit->setText(_ngPost->xml2txt(_ngPost->_from.c_str()));
    _ui->groupsEdit->setText(_ngPost->groups());
    _ui->uniqueFromCB->setChecked(_ngPost->_genFrom);
    _ui->saveFromCB->setChecked(_ngPost->_saveFrom);

    _ui->rarLengthSB->setValue(static_cast<int>(_ngPost->_lengthPass));
    if (_ngPost->_rarPassFixed.isEmpty())
    {
        _ui->rarPassCB->setChecked(false);
        onRarPassToggled(false);
    }
    else
    {
	// Issue #48 we should set the text first!
        _ui->rarPassEdit->setText(_ngPost->_rarPassFixed);
        _ui->rarPassCB->setChecked(true);
    }

    _ui->autoCompressCB->setChecked(_ngPost->_packAuto);
    _ui->autoCloseCB->setChecked(_ngPost->_autoCloseTabs);
    _ui->checkForUpdatesCB->setChecked(_ngPost->_checkForUpdates);

    connect(_ui->vpnSettingsBtn, &QAbstractButton::clicked, this, &MainWindow::onVpnSettingsClicked);
    if (VpnManager *vpn = _ngPost->vpnManager()) {
        connect(vpn, &VpnManager::stateChanged, this, &MainWindow::onVpnStateChanged);
        // Phase 3: surface VPN-required-but-unavailable as a popup so the
        // user knows why their job didn't start.
        connect(vpn, &VpnManager::vpnRequiredButUnavailable,
                this, [this](VpnManager::JobBlockReason, QString const &detail) {
            QMessageBox::warning(this, tr("VPN required"),
                tr("This job needs the VPN but it cannot be started:\n\n%1\n\n"
                   "The job stays in the queue.").arg(detail));
        });
        onVpnStateChanged(vpn->state());
    }

    _ui->obfuscateMsgIdCB->setChecked(_ngPost->_obfuscateArticles);
    _ui->obfuscateFileNameCB->setChecked(_ngPost->_obfuscateFileName);
    _ui->keepNfoExtensionCB->setChecked(_ngPost->_keepNfoExtension);

    _ui->articleSizeEdit->setText(QString::number(_ngPost->articleSize()));
    _ui->articleSizeEdit->setValidator(new QIntValidator(100000, 10000000, _ui->articleSizeEdit));

    _ui->nbRetrySB->setRange(0, 15);
    _ui->nbRetrySB->setValue(NntpArticle::nbMaxTrySending());

    _ui->threadSB->setRange(0, 50);
    _ui->threadSB->setValue(_ngPost->_nbThreads);

    _ui->nzbPathEdit->setText(_ngPost->_nzbPath);
    connect(_ui->nzbPathButton, &QAbstractButton::clicked, this, &MainWindow::onNzbPathClicked);
}

void MainWindow::updateServers()
{
    qDeleteAll(_ngPost->_nntpServers);
    _ngPost->_nntpServers.clear();

    int nbRows = _ui->serversTable->rowCount();
    for (int row = 0 ; row < nbRows; ++row)
    {
        int col = 0;
        bool isEnabled =  static_cast<CheckBoxCenterWidget*>(_ui->serversTable->cellWidget(row, col++))->isChecked();

        QLineEdit *hostEdit = static_cast<QLineEdit*>(_ui->serversTable->cellWidget(row, col++));
        if (hostEdit->text().isEmpty())
            continue;

        QLineEdit *portEdit = static_cast<QLineEdit*>(_ui->serversTable->cellWidget(row, col++));
        CheckBoxCenterWidget *sslCb = static_cast<CheckBoxCenterWidget*>(_ui->serversTable->cellWidget(row, col++));
        CheckBoxCenterWidget *vpnCb = static_cast<CheckBoxCenterWidget*>(_ui->serversTable->cellWidget(row, col++));
        QLineEdit *nbConsEdit = static_cast<QLineEdit*>(_ui->serversTable->cellWidget(row, col++));
        QLineEdit *userEdit = static_cast<QLineEdit*>(_ui->serversTable->cellWidget(row, col++));
        QLineEdit *passEdit = static_cast<QLineEdit*>(_ui->serversTable->cellWidget(row, col++));

        NntpServerParams *srvParams = new NntpServerParams(hostEdit->text(), portEdit->text().toUShort());
        srvParams->useSSL = sslCb->isChecked();
        srvParams->useVpn = vpnCb->isChecked();
        srvParams->nbCons = nbConsEdit->text().toInt();
        srvParams->enabled = isEnabled;
        QString user = userEdit->text();
        if (!user.isEmpty())
        {
            srvParams->auth = true;
            srvParams->user = user.toStdString();
            srvParams->pass = passEdit->text().toStdString();
        }

        _ngPost->_nntpServers << srvParams;
    }
}

void MainWindow::updateParams()
{
    QString from = _ui->fromEdit->text();
    if (!from.isEmpty())
    {
        QRegularExpression email("\\w+@\\w+\\.\\w+");
        if (!email.match(from).hasMatch())
            from += QString("@%1.com").arg(_ngPost->aticleSignature().c_str());
        _ngPost->_from   = _ngPost->escapeXML(from).toStdString();
    }
    _ngPost->_genFrom  = _ui->uniqueFromCB->isChecked();
    _ngPost->_saveFrom = _ui->saveFromCB->isChecked();

    if (_ui->rarPassCB->isChecked())
    {
        _ngPost->_rarPassFixed = _ui->rarPassEdit->text();
        _ngPost->_rarPass      = _ngPost->_rarPassFixed;
    }

    _ngPost->enableAutoPacking(_ui->autoCompressCB->isChecked());
    _ngPost->_autoCloseTabs   = _ui->autoCloseCB->isChecked();
    _ngPost->_checkForUpdates = _ui->checkForUpdatesCB->isChecked();

    _ngPost->updateGroups(_ui->groupsEdit->toPlainText());

    _ngPost->_obfuscateArticles = _ui->obfuscateMsgIdCB->isChecked();
    _ngPost->_obfuscateFileName = _ui->obfuscateFileNameCB->isChecked();
    _ngPost->_keepNfoExtension  = _ui->keepNfoExtensionCB->isChecked();

    bool ok = false;
    uint articleSize = _ui->articleSizeEdit->text().toUInt(&ok);
    if (ok)
        NgPost::sArticleSize = articleSize;

    NntpArticle::setNbMaxRetry(static_cast<ushort>(_ui->nbRetrySB->value()));

    _ngPost->_nbThreads = _ui->threadSB->value();
    if (_ngPost->_nbThreads < 1)
        _ngPost->_nbThreads = 1;

    QFileInfo nzbPath(_ui->nzbPathEdit->text());
    if (nzbPath.exists() && nzbPath.isDir() && nzbPath.isWritable())
        _ngPost->_nzbPath = nzbPath.absoluteFilePath();
}

void MainWindow::updateAutoPostingParams()
{
    updateServers();
    updateParams();
    _autoPostTab->udatePostingParams();
}

void MainWindow::updateConfigFromUi()
{
    if (!_ngPost)
        return;

    updateParams();

    if (!_quickJobTab || !_autoPostTab)
        return;

    int currentTabIdx = _ui->postTabWidget->currentIndex();
    if (currentTabIdx == 0)
        _quickJobTab->udatePostingParams();
    else if (currentTabIdx == 1)
        _autoPostTab->udatePostingParams();
    else
    {
        PostingWidget *postWidget = _getPostWidget(currentTabIdx);
        if (postWidget)
            postWidget->udatePostingParams();
    }

    const bool signalsWereBlocked = _ui->autoCompressCB->blockSignals(true);
    _ui->autoCompressCB->setChecked(_ngPost->_packAuto);
    _ui->autoCompressCB->blockSignals(signalsWereBlocked);
}

QString MainWindow::fixedArchivePassword() const
{
    if (_ui->rarPassCB->isChecked())
    {
        QString pass = _ui->rarPassEdit->text();
        if (!pass.isEmpty())
            return pass;
    }
    return QString();
}

namespace {

static QString histHumanSize(qint64 bytes)
{
    if (bytes < 1024LL)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024LL * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
}

static bool isDarkMode()
{
    return qApp->palette().color(QPalette::Window).lightness() < 128;
}

static QColor statusColor(const QString &status)
{
    const bool dark = isDarkMode();
    if (status == QStringLiteral("success"))
        return dark ? QColor(0x4C, 0xFF, 0x4C) : QColor(Qt::darkGreen);
    if (status == QStringLiteral("partial"))
        return QColor(0xFF, 0xA2, 0x00);
    if (status == QStringLiteral("failed"))
        return dark ? QColor(0xFF, 0x60, 0x60) : QColor(Qt::darkRed);
    if (status == QStringLiteral("cancelled"))
        return dark ? QColor(0xAA, 0xAA, 0xAA) : QColor(Qt::gray);
    if (status == QStringLiteral("resumable"))
        return dark ? QColor(0x66, 0xAA, 0xFF) : QColor(Qt::darkBlue);
    if (status == QStringLiteral("partial_resumable"))
        return QColor(0xFF, 0xA2, 0x00);
    if (status == QStringLiteral("posting"))
        return QColor(0xFF, 0xA2, 0x00);
    if (status == QStringLiteral("unknown"))
        return dark ? QColor(0xFF, 0x66, 0xFF) : QColor(Qt::darkMagenta);
    return qApp->palette().color(QPalette::WindowText);
}

} // namespace

void MainWindow::_retranslateHistoryTab()
{
    if (!_innerHistoryTabs)
        return;

    // Inner tab labels
    _innerHistoryTabs->setTabText(0, tr("History"));
    _innerHistoryTabs->setTabText(1, tr("Stats"));
    _innerHistoryTabs->setTabText(2, tr("Resume"));

    // Banner
    if (_bannerLabel && _bannerLabel->isVisible()) {
        // Re-emit the count from the current text is fragile; just retranslate the format.
        // The actual count will be refreshed next time _buildHistoryTab triggers the banner.
    }
    if (_bannerResumeBtn)
        _bannerResumeBtn->setText(tr("Open Resume \342\206\222"));

    // History filter row 1
    if (_histSearchLabel)  _histSearchLabel->setText(tr("Search:"));
    if (_histStatusLabel)  _histStatusLabel->setText(tr("Status:"));
    if (_histRefreshBtn)   _histRefreshBtn->setText(tr("Refresh"));
    if (_histExportCsvBtn) _histExportCsvBtn->setText(tr("Export CSV\342\200\246"));
    if (_historySearchEdit)
        _historySearchEdit->setPlaceholderText(tr("Search name, NZB, archive\342\200\246"));
    if (_historyStatusFilter) {
        const int cur = _historyStatusFilter->currentIndex();
        _historyStatusFilter->setItemText(0, tr("All statuses"));
        _historyStatusFilter->setCurrentIndex(cur);
    }
    if (_historyPassFilter)  _historyPassFilter->setText(tr("Has password"));

    // History filter row 2
    if (_histFromLabel)  _histFromLabel->setText(tr("From:"));
    if (_histToLabel)    _histToLabel->setText(tr("To:"));
    if (_histGroupLabel) _histGroupLabel->setText(tr("Group:"));
    if (_historyGroupEdit)
        _historyGroupEdit->setPlaceholderText(tr("Group filter\342\200\246"));
    if (_historyErrorsFilter) _historyErrorsFilter->setText(tr("Has errors"));
    if (_histClearBtn) {
        _histClearBtn->setText(tr("Clear"));
        _histClearBtn->setToolTip(tr("Clear all filters"));
    }
    if (_historyDateFrom) _historyDateFrom->setSpecialValueText(tr("Any date"));
    if (_historyDateTo)   _historyDateTo->setSpecialValueText(tr("Any date"));

    // History table headers
    if (_historyTable)
        _historyTable->setHorizontalHeaderLabels({
            tr("Date"), tr("Name"), tr("Status"), tr("Size"), tr("Speed"),
            tr("Files"), tr("Articles"), tr("Failed"), tr("Groups"), tr("Password"), tr("NZB path")});

    // History detail panel
    if (_historyDetailInfo && _historyDetailInfo->text().contains("Select a post"))
        _historyDetailInfo->setText(tr("<i>Select a post to see its details.</i>"));
    if (_histRegenNzbBtn)  _histRegenNzbBtn->setText(tr("Regenerate NZB\342\200\246"));
    if (_histCopyPassBtn)  _histCopyPassBtn->setText(tr("Copy password"));
    if (_histPurgePassBtn) _histPurgePassBtn->setText(tr("Purge password"));
    if (_histOpenNzbBtn)   _histOpenNzbBtn->setText(tr("Open NZB location"));
    if (_histDeleteBtn)    _histDeleteBtn->setText(tr("Delete entry"));

    // Stats filter bar
    if (_statsPeriodLabel) _statsPeriodLabel->setText(tr("Period:"));
    if (_statsGroupLabel)  _statsGroupLabel->setText(tr("Group:"));
    if (_statsRefreshBtn)  _statsRefreshBtn->setText(tr("Refresh"));
    if (_statsPeriodFilter) {
        const int cur = _statsPeriodFilter->currentIndex();
        _statsPeriodFilter->setItemText(0, tr("Last 7 days"));
        _statsPeriodFilter->setItemText(1, tr("Last 30 days"));
        _statsPeriodFilter->setItemText(2, tr("Last 90 days"));
        _statsPeriodFilter->setItemText(3, tr("This year"));
        _statsPeriodFilter->setItemText(4, tr("All time"));
        _statsPeriodFilter->setCurrentIndex(cur);
    }
    if (_statsGroupFilter && _statsGroupFilter->count() > 0)
        _statsGroupFilter->setItemText(0, tr("All groups"));

    // Stats inner tabs
    if (_statsInnerTabs) {
        _statsInnerTabs->setTabText(0, tr("Timeline"));
        _statsInnerTabs->setTabText(1, tr("By group"));
        _statsInnerTabs->setTabText(2, tr("Top posts"));
    }
    if (_statsTimelineChart) _statsTimelineChart->setTitle(tr("Volume and failures per day"));
    if (_statsGroupChart)    _statsGroupChart->setTitle(tr("Posts by newsgroup"));
    if (_statsTopTable)
        _statsTopTable->setHorizontalHeaderLabels({
            tr("Name"), tr("Date"), tr("Size"), tr("Status"), tr("Groups")});

    // Resume tab
    if (_resumeDetailInfo && _resumeDetailInfo->text().contains("Select a post"))
        _resumeDetailInfo->setText(tr("<i>Select a post to see resume details.</i>"));
    if (_resumeTable)
        _resumeTable->setHorizontalHeaderLabels({
            tr("Name"), tr("Status"), tr("Posted"), tr("To repost"), tr("Unknown"), tr("Reason")});
    if (_resumeResumeBtn) {
        _resumeResumeBtn->setText(tr("Resume"));
        _resumeResumeBtn->setToolTip(tr("Resume posting the selected post(s) from where they stopped"));
    }
    if (_resumeAbandonBtn) {
        _resumeAbandonBtn->setText(tr("Abandon"));
        _resumeAbandonBtn->setToolTip(tr("Mark selected post(s) as abandoned (keep history entry)"));
    }
    if (_resumePurgeBtn) {
        _resumePurgeBtn->setText(tr("Purge resume data"));
        _resumePurgeBtn->setToolTip(tr("Delete the technical resume data for selected post(s)"));
    }
    if (_resumeIgnoreBtn) {
        _resumeIgnoreBtn->setText(tr("Ignore (session)"));
        _resumeIgnoreBtn->setToolTip(tr("Hide selected post(s) from this view for this session"));
    }
    if (_resumeDeleteBtn) {
        _resumeDeleteBtn->setText(tr("Delete entry"));
        _resumeDeleteBtn->setToolTip(tr("Permanently delete selected post(s) from the history database"));
    }
    if (_resumeRefreshBtn) _resumeRefreshBtn->setText(tr("Refresh"));
}

QWidget *MainWindow::_buildHistoryTab()
{
    QWidget *root = new QWidget(_ui->postTabWidget);
    QVBoxLayout *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(4, 4, 4, 4);

    // Banner: shown when resumable posts exist
    _bannerLabel = new QLabel(root);
    _bannerLabel->setVisible(false);
    _bannerLabel->setStyleSheet(
        "QLabel { border: 2px solid #FFA200; color: palette(windowText); "
        "padding: 6px; border-radius: 4px; }");
    _bannerResumeBtn = new QPushButton(tr("Open Resume \342\206\222"), root);
    _bannerResumeBtn->setFlat(true);
    _bannerResumeBtn->setCursor(Qt::PointingHandCursor);
    _bannerResumeBtn->setVisible(false);
    QHBoxLayout *bannerRow = new QHBoxLayout();
    bannerRow->addWidget(_bannerLabel, 1);
    bannerRow->addWidget(_bannerResumeBtn);
    rootLayout->addLayout(bannerRow);

    _innerHistoryTabs = new QTabWidget(root);
    rootLayout->addWidget(_innerHistoryTabs);

    // ===== Tab 0: History =====
    QWidget *histTab = new QWidget(_innerHistoryTabs);
    QVBoxLayout *histLayout = new QVBoxLayout(histTab);
    histLayout->setContentsMargins(4, 4, 4, 4);

    // Filter row 1: search / status / password
    QHBoxLayout *filterRow1 = new QHBoxLayout();
    _historySearchEdit = new QLineEdit(histTab);
    _historySearchEdit->setPlaceholderText(tr("Search name, NZB, archive\342\200\246"));
    _historyStatusFilter = new QComboBox(histTab);
    _historyStatusFilter->addItems({tr("All statuses"),
                                    QStringLiteral("success"),
                                    QStringLiteral("partial"),
                                    QStringLiteral("failed"),
                                    QStringLiteral("cancelled"),
                                    QStringLiteral("resumable"),
                                    QStringLiteral("posting"),
                                    QStringLiteral("unknown")});
    _historyPassFilter = new QCheckBox(tr("Has password"), histTab);
    _histRefreshBtn   = new QPushButton(tr("Refresh"), histTab);
    _histExportCsvBtn = new QPushButton(tr("Export CSV\342\200\246"), histTab);
    _histSearchLabel = new QLabel(tr("Search:"), histTab);
    filterRow1->addWidget(_histSearchLabel);
    filterRow1->addWidget(_historySearchEdit, 1);
    _histStatusLabel = new QLabel(tr("Status:"), histTab);
    filterRow1->addWidget(_histStatusLabel);
    filterRow1->addWidget(_historyStatusFilter);
    filterRow1->addWidget(_historyPassFilter);
    filterRow1->addWidget(_histRefreshBtn);
    filterRow1->addWidget(_histExportCsvBtn);
    histLayout->addLayout(filterRow1);

    // Filter row 2: date range / group / errors
    QHBoxLayout *filterRow2 = new QHBoxLayout();
    _historyDateFrom = new QDateEdit(histTab);
    _historyDateFrom->setCalendarPopup(true);
    _historyDateFrom->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    _historyDateFrom->setSpecialValueText(tr("Any date"));
    _historyDateFrom->setMinimumDate(QDate(2000, 1, 1));
    _historyDateFrom->setDate(_historyDateFrom->minimumDate());

    _historyDateTo = new QDateEdit(histTab);
    _historyDateTo->setCalendarPopup(true);
    _historyDateTo->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    _historyDateTo->setSpecialValueText(tr("Any date"));
    _historyDateTo->setMinimumDate(QDate(2000, 1, 1));
    _historyDateTo->setDate(_historyDateTo->minimumDate());

    _historyGroupEdit = new QLineEdit(histTab);
    _historyGroupEdit->setPlaceholderText(tr("Group filter\342\200\246"));
    _historyGroupEdit->setMaximumWidth(200);

    _historyErrorsFilter = new QCheckBox(tr("Has errors"), histTab);

    _histClearBtn = new QPushButton(tr("Clear"), histTab);
    _histClearBtn->setToolTip(tr("Clear all filters"));

    _histFromLabel  = new QLabel(tr("From:"), histTab);
    _histToLabel    = new QLabel(tr("To:"), histTab);
    _histGroupLabel = new QLabel(tr("Group:"), histTab);
    filterRow2->addWidget(_histFromLabel);
    filterRow2->addWidget(_historyDateFrom);
    filterRow2->addWidget(_histToLabel);
    filterRow2->addWidget(_historyDateTo);
    filterRow2->addWidget(_histGroupLabel);
    filterRow2->addWidget(_historyGroupEdit);
    filterRow2->addWidget(_historyErrorsFilter);
    filterRow2->addWidget(_histClearBtn);
    filterRow2->addStretch();
    histLayout->addLayout(filterRow2);

    // Splitter: table / detail panel
    QSplitter *histSplitter = new QSplitter(Qt::Vertical, histTab);
    histLayout->addWidget(histSplitter);

    // History table — 11 columns
    _historyTable = new QTableWidget(histSplitter);
    _historyTable->setColumnCount(11);
    _historyTable->setHorizontalHeaderLabels({
        tr("Date"), tr("Name"), tr("Status"), tr("Size"), tr("Speed"),
        tr("Files"), tr("Articles"), tr("Failed"), tr("Groups"), tr("Password"), tr("NZB path")});
    _historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _historyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _historyTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _historyTable->setSortingEnabled(true);
    _historyTable->horizontalHeader()->setStretchLastSection(false);
    _historyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    _historyTable->verticalHeader()->hide();
    _historyTable->setContextMenuPolicy(Qt::CustomContextMenu);
    histSplitter->addWidget(_historyTable);

    // Detail panel
    QWidget *detailPanel = new QWidget(histSplitter);
    QVBoxLayout *detailLayout = new QVBoxLayout(detailPanel);
    detailLayout->setContentsMargins(4, 4, 4, 4);
    QScrollArea *detailScroll = new QScrollArea(detailPanel);
    detailScroll->setObjectName(QStringLiteral("historyDetailScroll"));
    detailScroll->setWidgetResizable(true);
    detailScroll->setFrameShape(QFrame::NoFrame);
    detailScroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    detailScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    detailScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    _historyDetailInfo = new QLabel(
        tr("<i>Select a post to see its details.</i>"), detailScroll);
    _historyDetailInfo->setObjectName(QStringLiteral("historyDetailInfo"));
    _historyDetailInfo->setWordWrap(true);
    _historyDetailInfo->setTextFormat(Qt::RichText);
    _historyDetailInfo->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    _historyDetailInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _historyDetailInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    _historyDetailInfo->setMinimumSize(0, 0);
    detailScroll->setWidget(_historyDetailInfo);
    detailLayout->addWidget(detailScroll, 1);

    QHBoxLayout *detailActions = new QHBoxLayout();
    _histRegenNzbBtn  = new QPushButton(tr("Regenerate NZB\342\200\246"), detailPanel);
    _histCopyPassBtn  = new QPushButton(tr("Copy password"), detailPanel);
    _histPurgePassBtn = new QPushButton(tr("Purge password"), detailPanel);
    _histOpenNzbBtn   = new QPushButton(tr("Open NZB location"), detailPanel);
    _histDeleteBtn    = new QPushButton(tr("Delete entry"), detailPanel);
    _histDeleteBtn->setStyleSheet("QPushButton { color: #dd4444; }");
    for (QPushButton *btn : {_histRegenNzbBtn, _histCopyPassBtn, _histPurgePassBtn,
                             _histOpenNzbBtn, _histDeleteBtn})
        btn->setEnabled(false);
    detailActions->addWidget(_histRegenNzbBtn);
    detailActions->addWidget(_histCopyPassBtn);
    detailActions->addWidget(_histPurgePassBtn);
    detailActions->addWidget(_histOpenNzbBtn);
    detailActions->addStretch();
    detailActions->addWidget(_histDeleteBtn);
    detailLayout->addLayout(detailActions);
    histSplitter->addWidget(detailPanel);
    histSplitter->setStretchFactor(0, 3);
    histSplitter->setStretchFactor(1, 1);

    _innerHistoryTabs->addTab(histTab, tr("History"));

    // ===== Tab 1: Stats =====
    QWidget *statsTab = new QWidget(_innerHistoryTabs);
    QVBoxLayout *statsLayout = new QVBoxLayout(statsTab);
    statsLayout->setContentsMargins(4, 4, 4, 4);

    // Stats filter bar
    QHBoxLayout *statsFilterBar = new QHBoxLayout();
    _statsPeriodFilter = new QComboBox(statsTab);
    _statsPeriodFilter->addItems({tr("Last 7 days"), tr("Last 30 days"), tr("Last 90 days"),
                                  tr("This year"), tr("All time")});
    _statsPeriodFilter->setCurrentIndex(1);
    _statsGroupFilter = new QComboBox(statsTab);
    _statsGroupFilter->addItem(tr("All groups"));
    _statsRefreshBtn = new QPushButton(tr("Refresh"), statsTab);
    _statsPeriodLabel = new QLabel(tr("Period:"), statsTab);
    _statsGroupLabel  = new QLabel(tr("Group:"), statsTab);
    statsFilterBar->addWidget(_statsPeriodLabel);
    statsFilterBar->addWidget(_statsPeriodFilter);
    statsFilterBar->addWidget(_statsGroupLabel);
    statsFilterBar->addWidget(_statsGroupFilter);
    statsFilterBar->addWidget(_statsRefreshBtn);
    statsFilterBar->addStretch();
    statsLayout->addLayout(statsFilterBar);

    // Stats inner tabs: Timeline / By group / Top posts
    _statsInnerTabs = new QTabWidget(statsTab);
    statsLayout->addWidget(_statsInnerTabs, 1);

    const QChart::ChartTheme chartTheme = isDarkMode()
                                              ? QChart::ChartThemeDark
                                              : QChart::ChartThemeLight;

    _statsTimelineChart = new QChart();
    _statsTimelineChart->setTheme(chartTheme);
    _statsTimelineChart->setTitle(tr("Volume and failures per day"));
    _statsTimelineChart->legend()->setVisible(true);
    QChartView *timelineView = new QChartView(_statsTimelineChart, statsTab);
    timelineView->setRenderHint(QPainter::Antialiasing);
    _statsInnerTabs->addTab(timelineView, tr("Timeline"));

    _statsGroupChart = new QChart();
    _statsGroupChart->setTheme(chartTheme);
    _statsGroupChart->setTitle(tr("Posts by newsgroup"));
    _statsGroupChart->legend()->setVisible(false);
    QChartView *groupView = new QChartView(_statsGroupChart, statsTab);
    groupView->setRenderHint(QPainter::Antialiasing);
    _statsInnerTabs->addTab(groupView, tr("By group"));

    _statsTopTable = new QTableWidget(statsTab);
    _statsTopTable->setColumnCount(5);
    _statsTopTable->setHorizontalHeaderLabels({
        tr("Name"), tr("Date"), tr("Size"), tr("Status"), tr("Groups")});
    _statsTopTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _statsTopTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _statsTopTable->setSelectionMode(QAbstractItemView::SingleSelection);
    _statsTopTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    _statsTopTable->verticalHeader()->hide();
    _statsInnerTabs->addTab(_statsTopTable, tr("Top posts"));

    _innerHistoryTabs->addTab(statsTab, tr("Stats"));

    // ===== Tab 2: Resume =====
    QWidget *resumeTab = new QWidget(_innerHistoryTabs);
    QVBoxLayout *resumeLayout = new QVBoxLayout(resumeTab);
    resumeLayout->setContentsMargins(4, 4, 4, 4);

    _resumeDetailInfo = new QLabel(tr("<i>Select a post to see resume details.</i>"), resumeTab);
    _resumeDetailInfo->setWordWrap(true);
    _resumeDetailInfo->setTextFormat(Qt::RichText);
    resumeLayout->addWidget(_resumeDetailInfo);

    _resumeTable = new QTableWidget(resumeTab);
    _resumeTable->setColumnCount(6);
    _resumeTable->setHorizontalHeaderLabels({
        tr("Name"), tr("Status"), tr("Posted"), tr("To repost"), tr("Unknown"), tr("Reason")});
    _resumeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _resumeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _resumeTable->setSelectionMode(QAbstractItemView::MultiSelection);
    _resumeTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    _resumeTable->verticalHeader()->hide();
    resumeLayout->addWidget(_resumeTable, 1);

    QHBoxLayout *resumeActions = new QHBoxLayout();
    _resumeResumeBtn  = new QPushButton(tr("Resume"), resumeTab);
    _resumeAbandonBtn = new QPushButton(tr("Abandon"), resumeTab);
    _resumePurgeBtn   = new QPushButton(tr("Purge resume data"), resumeTab);
    _resumeIgnoreBtn  = new QPushButton(tr("Ignore (session)"), resumeTab);
    _resumeDeleteBtn  = new QPushButton(tr("Delete entry"), resumeTab);
    _resumeRefreshBtn = new QPushButton(tr("Refresh"), resumeTab);
    _resumeResumeBtn->setToolTip(tr("Resume posting the selected post(s) from where they stopped"));
    _resumeAbandonBtn->setToolTip(tr("Mark selected post(s) as abandoned (keep history entry)"));
    _resumePurgeBtn->setToolTip(tr("Delete the technical resume data for selected post(s)"));
    _resumeIgnoreBtn->setToolTip(tr("Hide selected post(s) from this view for this session"));
    _resumeDeleteBtn->setToolTip(tr("Permanently delete selected post(s) from the history database"));
    _resumeDeleteBtn->setStyleSheet("QPushButton { color: #dd4444; }");
    for (QPushButton *btn : {_resumeResumeBtn, _resumeAbandonBtn, _resumePurgeBtn,
                             _resumeIgnoreBtn, _resumeDeleteBtn})
        btn->setEnabled(false);
    resumeActions->addWidget(_resumeResumeBtn);
    resumeActions->addWidget(_resumeAbandonBtn);
    resumeActions->addWidget(_resumePurgeBtn);
    resumeActions->addWidget(_resumeIgnoreBtn);
    resumeActions->addStretch();
    resumeActions->addWidget(_resumeDeleteBtn);
    resumeActions->addWidget(_resumeRefreshBtn);
    resumeLayout->addLayout(resumeActions);

    _innerHistoryTabs->addTab(resumeTab, tr("Resume"));

    // ===== Connect signals =====
    connect(_histRefreshBtn,      &QPushButton::clicked,  this, &MainWindow::_onHistoryRefresh);
    connect(_resumeRefreshBtn,    &QPushButton::clicked,  this, &MainWindow::_onHistoryRefresh);
    connect(_histExportCsvBtn,    &QPushButton::clicked,  this, &MainWindow::_onHistoryExportCsv);
    connect(_historySearchEdit,   &QLineEdit::returnPressed, this, &MainWindow::_onHistoryRefresh);
    connect(_historyStatusFilter, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::_onHistoryRefresh);
    connect(_historyPassFilter,   &QCheckBox::toggled,    this, &MainWindow::_onHistoryRefresh);
    connect(_historyErrorsFilter, &QCheckBox::toggled,    this, &MainWindow::_onHistoryRefresh);
    connect(_historyDateFrom,     &QDateEdit::dateChanged, this, &MainWindow::_onHistoryRefresh);
    connect(_historyDateTo,       &QDateEdit::dateChanged, this, &MainWindow::_onHistoryRefresh);
    connect(_historyGroupEdit,    &QLineEdit::returnPressed, this, &MainWindow::_onHistoryRefresh);
    connect(_histClearBtn,        &QPushButton::clicked,  this, [this]() {
        _historySearchEdit->clear();
        _historyStatusFilter->setCurrentIndex(0);
        _historyPassFilter->setChecked(false);
        _historyErrorsFilter->setChecked(false);
        _historyDateFrom->setDate(_historyDateFrom->minimumDate());
        _historyDateTo->setDate(_historyDateTo->minimumDate());
        _historyGroupEdit->clear();
        _onHistoryRefresh();
    });
    connect(_historyTable,        &QTableWidget::currentCellChanged,
            this, [this](int row, int, int, int) { _onHistoryRowSelected(row); });
    connect(_histRegenNzbBtn,     &QPushButton::clicked,  this, &MainWindow::_onHistoryRegenNzb);
    connect(_histCopyPassBtn,     &QPushButton::clicked,  this, &MainWindow::_onHistoryCopyPassword);
    connect(_histPurgePassBtn,    &QPushButton::clicked,  this, &MainWindow::_onHistoryPurgePassword);
    connect(_histDeleteBtn,       &QPushButton::clicked,  this, &MainWindow::_onHistoryDeleteEntry);
    // Del key triggers the delete button, but only while the history table has
    // focus (so it never fires from the resume tab or a text field).
    auto *histDeleteShortcut = new QShortcut(QKeySequence::Delete, _historyTable);
    histDeleteShortcut->setContext(Qt::WidgetShortcut);
    connect(histDeleteShortcut,   &QShortcut::activated,  this, &MainWindow::_onHistoryDeleteEntry);
    connect(_histOpenNzbBtn,      &QPushButton::clicked,  this, &MainWindow::_onHistoryOpenNzb);
    connect(_historyTable,        &QTableWidget::customContextMenuRequested,
            this, &MainWindow::_onHistoryContextMenu);
    connect(_statsRefreshBtn,     &QPushButton::clicked,  this, &MainWindow::_onStatsRefresh);
    connect(_statsPeriodFilter,   qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::_onStatsRefresh);
    connect(_statsGroupFilter,    qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::_onStatsRefresh);
    connect(_resumeTable,         &QTableWidget::itemSelectionChanged,
            this, &MainWindow::_onResumeSelectionChanged);
    connect(_resumeResumeBtn,     &QPushButton::clicked,  this, &MainWindow::_onResumePost);
    connect(_resumeAbandonBtn,    &QPushButton::clicked,  this, &MainWindow::_onResumeAbandon);
    connect(_resumePurgeBtn,      &QPushButton::clicked,  this, &MainWindow::_onResumePurge);
    connect(_resumeIgnoreBtn,     &QPushButton::clicked,  this, &MainWindow::_onResumeIgnore);
    connect(_resumeDeleteBtn,     &QPushButton::clicked,  this, &MainWindow::_onResumeDeleteEntries);
    connect(_bannerResumeBtn,     &QPushButton::clicked,  this, [this]() {
        _innerHistoryTabs->setCurrentIndex(2);
    });

    // ===== Initial data load =====
    _refreshHistoryViews();

    // Banner: show if resumable posts exist.
    if (_ngPost && _ngPost->historyService()) {
        _ngPost->historyService()->requestHistorySnapshot(PostHistoryStore::ListFilter(),
                                                          _ignoredResumeIds,
                                                          this,
                                                          [this](const PostHistoryService::HistorySnapshot &snapshot) {
            if (!snapshot.error.isEmpty() || snapshot.resumeRows.isEmpty())
                return;
            _bannerLabel->setText(tr("%1 post(s) can be resumed.").arg(snapshot.resumeRows.size()));
            _bannerLabel->setVisible(true);
            _bannerResumeBtn->setVisible(true);
            statusBar()->showMessage(
                tr("%1 post(s) can be resumed. Open the Resume tab to review them.")
                    .arg(snapshot.resumeRows.size()), 10000);
        });
    }

    return root;
}

void MainWindow::_refreshHistoryViews()
{
    PostHistoryService *history = _ngPost ? _ngPost->historyService() : nullptr;
    if (!history)
        return;

    PostHistoryStore::ListFilter filter;
    if (_historyStatusFilter && _historyStatusFilter->currentIndex() > 0)
        filter.status = _historyStatusFilter->currentText();
    if (_historySearchEdit)
        filter.search = _historySearchEdit->text().trimmed();
    filter.onlyWithPassword = _historyPassFilter && _historyPassFilter->isChecked();
    filter.onlyWithErrors   = _historyErrorsFilter && _historyErrorsFilter->isChecked();
    if (_historyGroupEdit)
        filter.group = _historyGroupEdit->text().trimmed();
    if (_historyDateFrom && _historyDateFrom->date() > _historyDateFrom->minimumDate())
        filter.dateFrom = _historyDateFrom->date().toString(Qt::ISODate);
    if (_historyDateTo && _historyDateTo->date() > _historyDateTo->minimumDate())
        filter.dateTo = _historyDateTo->date().toString(Qt::ISODate);

    const QSet<qint64> ignoredResumeIds = _ignoredResumeIds;
    history->requestHistorySnapshot(filter, ignoredResumeIds, this, [this](const PostHistoryService::HistorySnapshot &snapshot) {
        if (!snapshot.error.isEmpty()) {
            statusBar()->showMessage(tr("History refresh failed: %1").arg(snapshot.error), 6000);
            return;
        }

        if (!_historyTable || !_resumeTable)
            return;

        _historyTable->setSortingEnabled(false);
        const QList<PostHistoryStore::PostSummary> posts = snapshot.posts;
        _historyTable->setRowCount(posts.size());
        for (int row = 0; row < posts.size(); ++row) {
            const PostHistoryStore::PostSummary &p = posts.at(row);
            const QStringList values = {
                p.createdAt,
                p.nzbName,
                p.status,
                histHumanSize(p.sizeBytes),
                p.avgSpeed,
                QString::number(p.nbFiles),
                QString::number(p.nbArticles),
                p.nbFailedArticles > 0 ? QString::number(p.nbFailedArticles) : QString(),
                p.groups,
                p.hasPassword ? QStringLiteral("***") : QString(),
                p.nzbPath
            };
            for (int col = 0; col < values.size(); ++col) {
                auto *item = new QTableWidgetItem(values.at(col));
                if (col == 2)
                    item->setForeground(statusColor(p.status));
                item->setData(Qt::UserRole, p.id);
                _historyTable->setItem(row, col, item);
            }
        }
        _historyTable->resizeColumnsToContents();
        _historyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        _historyTable->setSortingEnabled(true);

        _resumeTable->setRowCount(0);
        for (const PostHistoryService::ResumeRow &resume : snapshot.resumeRows) {
            const int row = _resumeTable->rowCount();
            _resumeTable->setRowCount(row + 1);
            const QStringList values = {
                resume.name, resume.state,
                QString::number(resume.postedArticles),
                QString::number(resume.pendingArticles + resume.failedArticles),
                QString::number(resume.unknownArticles), resume.reason
            };
            for (int col = 0; col < values.size(); ++col) {
                auto *item = new QTableWidgetItem(values.at(col));
                if (col == 1)
                    item->setForeground(statusColor(resume.status));
                item->setData(Qt::UserRole, resume.postId);
                _resumeTable->setItem(row, col, item);
            }
        }
        _resumeTable->resizeColumnsToContents();
        _resumeTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);

        // Reset selection state only after a successful snapshot.
        _selectedHistoryId = 0;
        for (QPushButton *btn : {_histRegenNzbBtn, _histCopyPassBtn, _histPurgePassBtn,
                                 _histOpenNzbBtn, _histDeleteBtn})
            if (btn) btn->setEnabled(false);
        for (QPushButton *btn : {_resumeResumeBtn, _resumeAbandonBtn, _resumePurgeBtn,
                                 _resumeIgnoreBtn, _resumeDeleteBtn})
            if (btn) btn->setEnabled(false);
        if (_historyDetailInfo)
            _historyDetailInfo->setText(tr("<i>Select a post to see its details.</i>"));
        if (_resumeDetailInfo)
            _resumeDetailInfo->setText(tr("<i>Select a post to see resume details.</i>"));
    });

    _onStatsRefresh();
}

PostingWidget *MainWindow::addNewQuickTab(int lastTabIdx, const QFileInfoList &files)
{
    if (!lastTabIdx)
        lastTabIdx = _ui->postTabWidget->count() -1;
    PostingWidget *newPostingWidget = new PostingWidget(_ngPost, this, static_cast<uint>(lastTabIdx));
    newPostingWidget->init();
    // Tab layout: 0=quick (#1), 1=folder, 2=history, 3="+" — so a tab inserted
    // at lastTabIdx becomes the (lastTabIdx-1)-th quick post for display.
    QString tabName = QString("%1 #%2").arg(_ngPost->quickJobName()).arg(lastTabIdx - 1);
    _ui->postTabWidget->insertTab(lastTabIdx,
                                  newPostingWidget ,
                                  QIcon(":/icons/quick.png"),
                                  tabName);
    _ui->postTabWidget->setTabToolTip(lastTabIdx, tabName);

    for (const QFileInfo &file : files)
        newPostingWidget->addPath(file.absoluteFilePath(), 0, file.isDir());

    return newPostingWidget;
}

bool MainWindow::_startResumePost(qint64 postId, bool askConfirmation)
{
    if (!postId || !_ngPost)
        return false;

    if (askConfirmation) {
        const int res = QMessageBox::question(
            this,
            tr("Resume post"),
            tr("Resume posting for this post?\nOnly missing or failed articles will be re-sent."),
            QMessageBox::Yes,
            QMessageBox::No);
        if (res != QMessageBox::Yes)
            return false;
    }

    const int lastTabIdx = _ui->postTabWidget->count() - 1;
    PostingWidget *widget = addNewQuickTab(lastTabIdx);
    _ui->postTabWidget->setCurrentWidget(widget);

    QString err;
    if (!_ngPost->resumePostGui(postId, widget, &err)) {
        closeTab(widget);
        QMessageBox::warning(this,
                             tr("Resume failed"),
                             err.isEmpty()
                                 ? tr("This post could not be resumed.")
                                 : tr("This post could not be resumed:\n%1").arg(err));
        return false;
    }

    _refreshHistoryViews();
    return true;
}

void MainWindow::setTab(QWidget *postWidget)
{
    int nbJob = _ui->postTabWidget->count() -1;
    for (int i = 0 ; i < nbJob ; ++i)
    {
        if (_ui->postTabWidget->widget(i) == postWidget)
        {
            _ui->postTabWidget->setCurrentIndex(i);
            break;
        }
    }
}

void MainWindow::clearJobTab(QWidget *postWidget)
{
    int nbJob = _ui->postTabWidget->count() -1;
    for (int i = 0 ; i < nbJob ; ++i)
    {
        if (_ui->postTabWidget->widget(i) == postWidget)
        {
            QTabBar *bar = _ui->postTabWidget->tabBar();
            bar->setTabToolTip(i, "");
            bar->setTabTextColor(i, Qt::black);
            bar->setTabIcon(i, QIcon(":/icons/quick.png"));
        }
    }
}

void MainWindow::updateJobTab(QWidget *postWidget, const QColor &color, const QIcon &icon, const QString &tooltip)
{
    int nbJob = _ui->postTabWidget->count() -1;
    for (int i = 0 ; i < nbJob ; ++i)
    {
        if (_ui->postTabWidget->widget(i) == postWidget)
        {
            QTabBar *bar = _ui->postTabWidget->tabBar();
            if (!tooltip.isEmpty())
                bar->setTabToolTip(i, tooltip);
            bar->setTabTextColor(i, color);
            bar->setTabIcon(i, icon);
            break;
        }
    }
}

void MainWindow::setJobLabel(int jobNumber)
{
    _ui->jobLabel->setText(QString("<b><u>Post #%1</u></b>").arg(jobNumber != 1 ? QString::number(jobNumber) : "Auto"));
}


void MainWindow::_addServer(NntpServerParams *serverParam)
{
    int nbRows = _ui->serversTable->rowCount(), col = 0;
    _ui->serversTable->setRowCount(nbRows+1);

    // Object names on the dynamically-created server row widgets make
    // findChild<>() lookups deterministic for GUI tests. The naming
    // convention is "server<Role>_<row>" so a test can target row N
    // unambiguously.
    const QString suffix = QStringLiteral("_%1").arg(nbRows);

    auto *enabledCb = new CheckBoxCenterWidget(_ui->serversTable,
                                               serverParam ? serverParam->enabled : true);
    enabledCb->setObjectName(QStringLiteral("serverEnabledCb") + suffix);
    _ui->serversTable->setCellWidget(nbRows, col++, enabledCb);

    QLineEdit *hostEdit = new QLineEdit(_ui->serversTable);
    hostEdit->setObjectName(QStringLiteral("serverHostEdit") + suffix);
    if (serverParam)
        hostEdit->setText(serverParam->host);
    hostEdit->setFrame(false);
    _ui->serversTable->setCellWidget(nbRows, col++, hostEdit);

    QLineEdit *portEdit = new QLineEdit(_ui->serversTable);
    portEdit->setObjectName(QStringLiteral("serverPortEdit") + suffix);
    portEdit->setFrame(false);
    portEdit->setValidator(new QIntValidator(1, 99999, portEdit));
    portEdit->setText(QString::number(serverParam ? serverParam->port : sDefaultServerPort));
    portEdit->setAlignment(Qt::AlignCenter);
    _ui->serversTable->setCellWidget(nbRows, col++, portEdit);

    auto *sslCb = new CheckBoxCenterWidget(_ui->serversTable,
                                           serverParam ? serverParam->useSSL : sDefaultServerSSL);
    sslCb->setObjectName(QStringLiteral("serverSslCb") + suffix);
    _ui->serversTable->setCellWidget(nbRows, col++, sslCb);

    // Phase 3: per-server "Use VPN" — when checked, the server's NNTP
    // sockets are bound to the VPN tun (and the VPN is auto-started for
    // any job that uses this server).
    // Phase 5d: persist immediately on toggle so the user doesn't have to
    // hit "Save Config" after each tweak.
    CheckBoxCenterWidget *vpnCb = new CheckBoxCenterWidget(
        _ui->serversTable, serverParam ? serverParam->useVpn : false);
    vpnCb->setObjectName(QStringLiteral("serverUseVpnCb") + suffix);
    _ui->serversTable->setCellWidget(nbRows, col++, vpnCb);
    connect(vpnCb, &CheckBoxCenterWidget::toggled,
            this, &MainWindow::_onUseVpnToggled);

    QLineEdit *nbConsEdit = new QLineEdit(_ui->serversTable);
    nbConsEdit->setObjectName(QStringLiteral("serverNbConsEdit") + suffix);
    nbConsEdit->setFrame(false);
    nbConsEdit->setValidator(new QIntValidator(1, 99999, nbConsEdit));
    nbConsEdit->setText(QString::number(serverParam ? serverParam->nbCons : sDefaultConnections));
    nbConsEdit->setAlignment(Qt::AlignCenter);
    _ui->serversTable->setCellWidget(nbRows, col++, nbConsEdit);


    QLineEdit *userEdit = new QLineEdit(_ui->serversTable);
    userEdit->setObjectName(QStringLiteral("serverUserEdit") + suffix);
    if (serverParam)
        userEdit->setText(serverParam->user.c_str());
    userEdit->setFrame(false);
    _ui->serversTable->setCellWidget(nbRows, col++, userEdit);

    QLineEdit *passEdit = new QLineEdit(_ui->serversTable);
    passEdit->setObjectName(QStringLiteral("serverPassEdit") + suffix);
    passEdit->setEchoMode(QLineEdit::EchoMode::PasswordEchoOnEdit);
    if (serverParam)
        passEdit->setText(serverParam->pass.c_str());
    passEdit->setFrame(false);
    _ui->serversTable->setCellWidget(nbRows, col++, passEdit);

    QPushButton *delButton = new QPushButton(_ui->serversTable);
    delButton->setObjectName(QStringLiteral("serverDelButton") + suffix);
    delButton->setProperty("server", QVariant::fromValue(static_cast<void*>(serverParam)));
    delButton->setIcon(QIcon(":/icons/clear.png"));
    delButton->setMaximumWidth(sDeleteColumnWidth);
    connect(delButton, &QAbstractButton::clicked, this, &MainWindow::onDelServer);
    _ui->serversTable->setCellWidget(nbRows, col++, delButton);
}

int MainWindow::_serverRow(QObject *delButton)
{
    int nbRows = _ui->serversTable->rowCount(), delCol =_ui->serversTable->columnCount()-1;
    for (int row = 0 ; row < nbRows; ++row)
    {
        if (_ui->serversTable->cellWidget(row, delCol) == delButton)
            return row;
    }
    return nbRows;
}

PostingWidget *MainWindow::_getPostWidget(int tabIndex) const
{
    if(tabIndex > 1 && tabIndex < _ui->postTabWidget->count() - 1)
        return qobject_cast<PostingWidget*>(_ui->postTabWidget->widget(tabIndex));
    else
        return nullptr;
}

int MainWindow::_getPostWidgetIndex(PostingWidget *postWidget) const
{
    int nbJob = _ui->postTabWidget->count() -1;
    for (int i = 2; i < nbJob ; ++i)
    {
        if (_ui->postTabWidget->widget(i) == postWidget)
            return i;
    }
    return 0;
}



void MainWindow::onGenPoster()
{
    _ui->fromEdit->setText(_ngPost->randomFrom());
}

void MainWindow::onUniqueFromToggled(bool checked)
{
    bool enabled = !checked;
    _ui->genPoster->setEnabled(enabled);
    _ui->fromEdit->setEnabled(enabled);
    _ui->saveFromCB->setEnabled(enabled);
}

void MainWindow::onRarPassToggled(bool checked)
{
    _ui->rarPassEdit->setEnabled(checked);
    _ui->rarLengthSB->setEnabled(checked);
    _ui->genPass->setEnabled(checked);
    if (checked)
        onRarPassUpdated(_ui->rarPassEdit->text());
}

void MainWindow::onRarPassUpdated(const QString &fixedPass)
{
    _ngPost->_rarPassFixed = fixedPass;
//    if (!_quickJobTab->isPosting())
        _quickJobTab->setNzbPassword(fixedPass);
    PostingWidget *currentQuickPost = _getPostWidget(_ui->postTabWidget->currentIndex());
    if (currentQuickPost) //&& !currentQuickPost->isPosting())
        currentQuickPost->setNzbPassword(fixedPass);
}

void MainWindow::onArchivePass()
{
    _ui->rarPassEdit->setText(_ngPost->randomPass(static_cast<uint>(_ui->rarLengthSB->value())));
}

void MainWindow::onAutoCompressToggled(bool checked)
{
    _ngPost->enableAutoPacking(checked);
    _autoPostTab->setPackingAuto(checked, _ngPost->_packAutoKeywords);
    if (!_quickJobTab->isPosting())
        _quickJobTab->setPackingAuto(checked, _ngPost->_packAutoKeywords);
    PostingWidget *currentQuickPost = _getPostWidget(_ui->postTabWidget->currentIndex());
    if (currentQuickPost && !currentQuickPost->isPosting())
        currentQuickPost->setPackingAuto(checked, _ngPost->_packAutoKeywords);
}

void MainWindow::onDebugToggled(bool checked)
{
#ifdef __DEBUG__
    qDebug() << "Debug mode: " << checked;
#endif
    if (checked)
    {
        if (!_ui->debugSB->value())
            _ui->debugSB->setValue(1);
        _ngPost->setDebug(static_cast<ushort>(_ui->debugSB->value()));
    }
    else
        _ngPost->setDebug(0);
    _ui->debugSB->setEnabled(checked);
}

void MainWindow::onDebugValue(int value)
{
    _ngPost->setDebug(static_cast<ushort>(value));
}


void MainWindow::_onUseVpnToggled(bool checked)
{
    // Pull the current state of every "Use VPN" checkbox in the table back
    // into the in-memory NntpServerParams list, then flush to disk. We can't
    // target a single server here because the sender CheckBoxCenterWidget
    // doesn't carry its row index — `updateServers()` is cheap enough that
    // doing the full sync is simpler than introducing per-row bookkeeping.
    Q_UNUSED(checked);
    updateServers();
    _ngPost->saveConfig();
}

void MainWindow::onSaveConfig()
{
    updateServers();
    _ngPost->saveConfig();
}

void MainWindow::onJobTabClicked(int index)
{
    int nbJob = _ui->postTabWidget->count() -1;
    qDebug() << "Click on tab: " << index << ", count: " << nbJob;
    if (index == nbJob) // click on the last tab
        addNewQuickTab(nbJob);
}

void MainWindow::onCloseJob(int index)
{
    int nbJob = _ui->postTabWidget->count() -1;
    qDebug() << "onCloseJob on tab: " << index << ", count: " << nbJob;
    if (index > 1 && index < nbJob )
    {
        PostingWidget *postWidget = _getPostWidget(index);
        if (!postWidget)
            return;
        if (postWidget->isPosting())
        {
            QMessageBox::warning(this,
                                 tr("Quick Post is working.."),
                                 tr("The Quick post is currentling uploading.\n Please Stop it before closing it.."));
        }
        else
        {
            _ui->postTabWidget->removeTab(index);
            delete postWidget;

            if (index == nbJob - 1)
                _ui->postTabWidget->setCurrentIndex(_ui->postTabWidget->count() - 2);
        }
    }
}

void MainWindow::closeTab(PostingWidget *postWidget)
{
    int index = _getPostWidgetIndex(postWidget);
    if (index)
    {
        int nbJob = _ui->postTabWidget->count() -1;
        _ui->postTabWidget->removeTab(index);
        delete postWidget;

        if (index == nbJob - 1)
            _ui->postTabWidget->setCurrentIndex(_ui->postTabWidget->count() - 2);
    }
}


void MainWindow::toBeImplemented()
{
    QMessageBox::information(nullptr, "To be implemented...", "To be implemented...", QMessageBox::Ok);
}

#include <QFileDialog>
void MainWindow::onNzbPathClicked()
{
    QString path = QFileDialog::getExistingDirectory(
                this,
                tr("Select a Folder"),
                _ui->nzbPathEdit->text(),
                QFileDialog::ShowDirsOnly);

    if (!path.isEmpty())
        _ui->nzbPathEdit->setText(path);
}

void MainWindow::onLangChanged(const QString &lang)
{
    qDebug() << "Changing lang to " << lang;
    _ngPost->changeLanguage(lang.toLower());
}

void MainWindow::onShutdownToggled(bool checked)
{
    if (checked)
    {
        int res = QMessageBox::question(this,
                                        tr("Automatic Shutdown?"),
                                        QString("%1\n%2").arg(
                                            tr("You're about to schedule the shutdown of the computer once all the current Postings will be finished")).arg(
                                            tr("Are you sure you want to switch off the computer?")),
                                        QMessageBox::Yes,
                                        QMessageBox::No);
        if (res == QMessageBox::Yes)
            _ngPost->_doShutdownWhenDone = checked;
        else
            _ui->shutdownCB->setChecked(false);
    }
    else
        _ngPost->_doShutdownWhenDone = false;
}

void MainWindow::setPauseIcon(bool pause)
{
    if (pause)
        _ui->pauseButton->setIcon(QIcon(":/icons/pause.png"));
    else
        _ui->pauseButton->setIcon(QIcon(":/icons/play.png"));
}

void MainWindow::onPauseClicked()
{
    if (_ngPost->isPosting())
    {
        if (_ngPost->isPaused())
            _ngPost->resume();
        else
            _ngPost->pause();
    }
}

// ============================================================
// History tab slots
// ============================================================

void MainWindow::_onHistoryRefresh()
{
    _refreshHistoryViews();
}

void MainWindow::_onHistoryRowSelected(int row)
{
    if (!_historyTable || row < 0) {
        _selectedHistoryId = 0;
        if (_historyDetailInfo)
            _historyDetailInfo->setText(tr("<i>Select a post to see its details.</i>"));
        for (QPushButton *btn : {_histRegenNzbBtn, _histCopyPassBtn, _histPurgePassBtn,
                                 _histOpenNzbBtn, _histDeleteBtn})
            if (btn) btn->setEnabled(false);
        return;
    }

    QTableWidgetItem *item = _historyTable->item(row, 0);
    if (!item) return;
    _selectedHistoryId = item->data(Qt::UserRole).toLongLong();

    PostHistoryService *history = _ngPost ? _ngPost->historyService() : nullptr;
    if (!history || !_selectedHistoryId) return;
    const qint64 requestedId = _selectedHistoryId;
    history->requestPostDetails(requestedId, this, [this, requestedId](bool ok,
                                                                       const PostHistoryStore::PostDetails &details,
                                                                       const QString &err) {
        if (requestedId != _selectedHistoryId)
            return;
        if (!ok) {
            if (_historyDetailInfo)
                _historyDetailInfo->setText(tr("<i>Could not load details: %1</i>").arg(err));
            return;
        }
        _showHistoryDetails(details);
    });
}

void MainWindow::_showHistoryDetails(const PostHistoryStore::PostDetails &details)
{
    const PostHistoryStore::PostSummary &s = details.post;
    QString html = QStringLiteral(
        "<table cellspacing='4'>"
        "<tr><td><b>%1</b></td><td>%2</td>"
        "<td>&nbsp;&nbsp;<b>%3</b></td><td>%4</td></tr>"
        "<tr><td><b>%5</b></td><td>%6</td>"
        "<td>&nbsp;&nbsp;<b>%7</b></td><td>%8</td></tr>"
        "<tr><td><b>%9</b></td><td>%10</td>"
        "<td>&nbsp;&nbsp;<b>%11</b></td><td>%12</td></tr>"
        "<tr><td><b>%13</b></td><td>%14</td>"
        "<td>&nbsp;&nbsp;<b>%15</b></td><td>%16</td></tr>"
        "<tr><td><b>%17</b></td><td colspan='3'>%18</td></tr>"
        "<tr><td><b>%19</b></td><td colspan='3'>%20</td></tr>"
        "</table>")
        .arg(tr("Name:"), s.nzbName,
             tr("Status:"),
             QStringLiteral("<span style='color:%1'>%2</span>")
                 .arg(statusColor(s.status).name(), s.status))
        .arg(tr("Created:"), s.createdAt,
             tr("Finished:"), s.finishedAt.isEmpty() ? tr("\342\200\224") : s.finishedAt)
        .arg(tr("Size:"), histHumanSize(s.sizeBytes),
             tr("Speed:"), s.avgSpeed.isEmpty() ? tr("\342\200\224") : s.avgSpeed)
        .arg(tr("Files:"), QString::number(s.nbFiles),
             tr("Articles:"), QString("%1 / %2 failed")
                 .arg(s.nbArticles).arg(s.nbFailedArticles))
        .arg(tr("Groups:"), s.groups.isEmpty() ? tr("\342\200\224") : s.groups)
        .arg(tr("Archive:"), details.rarName.isEmpty() ? tr("\342\200\224") : details.rarName);

    if (!details.nzbPath.isEmpty())
        html += QStringLiteral("<table cellspacing='4'><tr><td><b>%1</b></td><td>%2</td></tr></table>")
                    .arg(tr("NZB path:"), details.nzbPath);

    if (s.hasPassword) {
        const QString passInfo = s.passwordStored
            ? tr("stored (%1 \342\200\224 hidden)").arg(details.passwordOrigin)
            : tr("present but not stored (%1)").arg(details.passwordOrigin);
        html += QStringLiteral("<table cellspacing='4'><tr><td><b>%1</b></td><td>%2</td></tr></table>")
                    .arg(tr("Password:"), passInfo);
    }

    if (!details.files.isEmpty()) {
        html += QStringLiteral("<br/><b>%1</b><br/><table cellspacing='2'>"
                               "<tr><th align='left'>%2</th><th>%3</th><th>%4</th></tr>")
                    .arg(tr("Files:"), tr("Name"), tr("Size"), tr("Status"));
        for (const PostHistoryStore::FileSummary &f : details.files) {
            html += QStringLiteral("<tr><td>%1</td><td align='right'>%2</td>"
                                   "<td align='center'><span style='color:%3'>%4</span></td></tr>")
                        .arg(f.postedName.isEmpty() ? f.originalPath : f.postedName,
                             histHumanSize(f.sizeBytes),
                             statusColor(f.status).name(),
                             f.status);
        }
        html += QStringLiteral("</table>");
    }

    if (_historyDetailInfo)
        _historyDetailInfo->setText(html);

    if (_histRegenNzbBtn)  _histRegenNzbBtn->setEnabled(true);
    if (_histDeleteBtn)    _histDeleteBtn->setEnabled(true);
    if (_histCopyPassBtn)  _histCopyPassBtn->setEnabled(s.hasPassword && s.passwordStored);
    if (_histPurgePassBtn) _histPurgePassBtn->setEnabled(s.hasPassword && s.passwordStored);
    if (_histOpenNzbBtn)   _histOpenNzbBtn->setEnabled(!details.nzbPath.isEmpty());
}

void MainWindow::_onHistoryRegenNzb()
{
    if (!_selectedHistoryId || !_ngPost) return;

    PostHistoryService *history = _ngPost->historyService();
    if (!history) return;

    PostHistoryStore::PostDetails details;
    QString err;
    if (!history->loadPostDetails(_selectedHistoryId, &details, &err)) return;

    bool includePassword = false;
    if (details.post.hasPassword && details.post.passwordStored) {
        const int res = QMessageBox::question(
            this, tr("Include password?"),
            tr("This post has a stored password.\n"
               "Include the password in the regenerated NZB?"),
            QMessageBox::Yes | QMessageBox::No);
        includePassword = (res == QMessageBox::Yes);
    }

    const QString outPath = QFileDialog::getSaveFileName(
        this,
        tr("Save regenerated NZB"),
        _ngPost->_nzbPath,
        tr("NZB files (*.nzb)"));
    if (outPath.isEmpty()) return;

    if (_ngPost->regenerateNzbGui(_selectedHistoryId, outPath, includePassword))
        QMessageBox::information(this, tr("NZB regenerated"),
                                 tr("NZB written to:\n%1").arg(outPath));
    else
        QMessageBox::warning(this, tr("NZB regeneration failed"),
                             tr("Could not regenerate the NZB.\n"
                                "Check that the post has complete article history."));
}

void MainWindow::_onHistoryExportCsv()
{
    PostHistoryService *history = _ngPost ? _ngPost->historyService() : nullptr;
    if (!history) return;

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export history as CSV"), QString(), tr("CSV files (*.csv)"));
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export failed"),
                             tr("Cannot write file:\n%1").arg(path));
        return;
    }
    QTextStream stream(&f);
    QString err;
    if (history->exportCsv(stream, false, &err))
        QMessageBox::information(this, tr("Export done"),
                                 tr("History exported to:\n%1").arg(path));
    else
        QMessageBox::warning(this, tr("Export failed"), err);
}

void MainWindow::_onHistoryCopyPassword()
{
    if (!_selectedHistoryId || !_ngPost) return;
    PostHistoryService *history = _ngPost->historyService();
    if (!history) return;

    PostHistoryStore::PostDetails details;
    QString err;
    if (!history->loadPostDetails(_selectedHistoryId, &details, &err)) return;
    if (details.rarPass.isEmpty()) return;

    QApplication::clipboard()->setText(details.rarPass);
    statusBar()->showMessage(tr("Password copied to clipboard."), 4000);
}

void MainWindow::_onHistoryPurgePassword()
{
    if (!_selectedHistoryId || !_ngPost) return;
    const int res = QMessageBox::question(
        this, tr("Purge password"),
        tr("Remove the stored password for this post?\n"
           "The history entry is kept; only the password value is erased."),
        QMessageBox::Yes, QMessageBox::No);
    if (res != QMessageBox::Yes) return;

    PostHistoryService *history = _ngPost->historyService();
    QString err;
    if (!history || !history->purgePassword(_selectedHistoryId, &err))
        QMessageBox::warning(this, tr("Error"), err.isEmpty()
                             ? tr("Could not purge password.") : err);
    else
        _refreshHistoryViews();
}

void MainWindow::_onHistoryDeleteEntry()
{
    if (!_historyTable || !_ngPost) return;

    // Collect the post id of every selected row (multi-selection), falling back
    // to the current row if the selection model happens to be empty.
    QList<qint64> postIds;
    for (const QModelIndex &idx : _historyTable->selectionModel()->selectedRows()) {
        if (QTableWidgetItem *item = _historyTable->item(idx.row(), 0))
            postIds << item->data(Qt::UserRole).toLongLong();
    }
    if (postIds.isEmpty() && _selectedHistoryId)
        postIds << _selectedHistoryId;
    if (postIds.isEmpty()) return;

    const int res = QMessageBox::question(
        this, tr("Delete history entries"),
        tr("Permanently delete %1 selected post(s) from the history?\n"
           "This also removes all associated file and article records.")
            .arg(postIds.size()),
        QMessageBox::Yes, QMessageBox::No);
    if (res != QMessageBox::Yes) return;

    PostHistoryService *history = _ngPost->historyService();
    if (!history) return;

    QString firstError;
    int failures = 0;
    for (const qint64 postId : postIds) {
        QString err;
        if (!history->deletePost(postId, &err)) {
            ++failures;
            if (firstError.isEmpty())
                firstError = err;
        }
    }
    if (failures)
        QMessageBox::warning(this, tr("Error"),
                             firstError.isEmpty()
                                 ? tr("Could not delete %1 of %2 selected entries.")
                                       .arg(failures).arg(postIds.size())
                                 : firstError);
    _refreshHistoryViews();
}

void MainWindow::_onHistoryOpenNzb()
{
    if (!_selectedHistoryId || !_ngPost) return;
    PostHistoryService *history = _ngPost->historyService();
    if (!history) return;
    PostHistoryStore::PostDetails details;
    QString err;
    if (!history->loadPostDetails(_selectedHistoryId, &details, &err)) return;
    if (details.nzbPath.isEmpty()) {
        QMessageBox::information(this, tr("No NZB path"),
                                 tr("No NZB path stored for this post."));
        return;
    }
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QFileInfo(details.nzbPath).absolutePath()));
}

void MainWindow::_onHistoryResumePost()
{
    if (!_selectedHistoryId)
        return;
    _startResumePost(_selectedHistoryId, true);
}

// ============================================================
// Stats tab slots
// ============================================================

void MainWindow::_onStatsRefresh()
{
    PostHistoryService *history = _ngPost ? _ngPost->historyService() : nullptr;
    if (!history) return;

    // Compute date range from period combo
    QString dateFrom, dateTo;
    if (_statsPeriodFilter) {
        const QDate today = QDate::currentDate();
        switch (_statsPeriodFilter->currentIndex()) {
        case 0: dateFrom = today.addDays(-6).toString(Qt::ISODate); break;
        case 1: dateFrom = today.addDays(-29).toString(Qt::ISODate); break;
        case 2: dateFrom = today.addDays(-89).toString(Qt::ISODate); break;
        case 3: dateFrom = QDate(today.year(), 1, 1).toString(Qt::ISODate); break;
        default: break; // All time
        }
        dateTo = today.toString(Qt::ISODate);
    }

    const QString groupFilter = (_statsGroupFilter && _statsGroupFilter->currentIndex() > 0)
                                    ? _statsGroupFilter->currentText() : QString();

    const QString previousGroup = (_statsGroupFilter && _statsGroupFilter->currentIndex() > 0)
                                      ? _statsGroupFilter->currentText() : QString();

    history->requestStatsSnapshot(dateFrom, dateTo, groupFilter, this, [this, previousGroup](const PostHistoryService::StatsSnapshot &snapshot) {
    if (!snapshot.error.isEmpty()) {
        statusBar()->showMessage(tr("History stats refresh failed: %1").arg(snapshot.error), 6000);
        return;
    }

    if (_statsGroupFilter) {
        _statsGroupFilter->blockSignals(true);
        _statsGroupFilter->clear();
        _statsGroupFilter->addItem(tr("All groups"));
        for (const QString &g : snapshot.groups)
            _statsGroupFilter->addItem(g);
        if (!previousGroup.isEmpty()) {
            const int idx = _statsGroupFilter->findText(previousGroup);
            if (idx >= 0)
                _statsGroupFilter->setCurrentIndex(idx);
        }
        _statsGroupFilter->blockSignals(false);
    }

    if (_statsTimelineChart) {
        _statsTimelineChart->removeAllSeries();
        for (QAbstractAxis *ax : _statsTimelineChart->axes())
            _statsTimelineChart->removeAxis(ax);

        auto *volSet  = new QBarSet(tr("Volume (MB)"), _statsTimelineChart);
        auto *failSet = new QBarSet(tr("Failed"), _statsTimelineChart);
        volSet->setColor(QColor(Qt::darkGreen));
        failSet->setColor(QColor(Qt::darkRed));

        QStringList labels;
        for (const PostHistoryStore::DayStats &d : snapshot.days) {
            *volSet  << (d.totalBytes / (1024.0 * 1024.0));
            *failSet << d.nbFailed;
            labels   << d.date.mid(5); // MM-DD
        }

        auto *series = new QBarSeries(_statsTimelineChart);
        series->append(volSet);
        series->append(failSet);
        _statsTimelineChart->addSeries(series);

        auto *axisX = new QBarCategoryAxis(_statsTimelineChart);
        axisX->append(labels);
        _statsTimelineChart->addAxis(axisX, Qt::AlignBottom);
        series->attachAxis(axisX);

        auto *axisY = new QValueAxis(_statsTimelineChart);
        axisY->setLabelFormat("%.0f");
        _statsTimelineChart->addAxis(axisY, Qt::AlignLeft);
        series->attachAxis(axisY);
    }

    if (_statsGroupChart) {
        _statsGroupChart->removeAllSeries();
        for (QAbstractAxis *ax : _statsGroupChart->axes())
            _statsGroupChart->removeAxis(ax);

        auto *set = new QBarSet(tr("Posts"), _statsGroupChart);
        set->setColor(QColor(Qt::darkBlue));
        QStringList labels;
        for (const PostHistoryStore::GroupStats &g : snapshot.groupStats) {
            *set << g.nbPosts;
            labels << g.group;
        }

        auto *series = new QBarSeries(_statsGroupChart);
        series->append(set);
        _statsGroupChart->addSeries(series);

        auto *axisX = new QBarCategoryAxis(_statsGroupChart);
        axisX->append(labels);
        _statsGroupChart->addAxis(axisX, Qt::AlignBottom);
        series->attachAxis(axisX);

        auto *axisY = new QValueAxis(_statsGroupChart);
        axisY->setLabelFormat("%d");
        _statsGroupChart->addAxis(axisY, Qt::AlignLeft);
        series->attachAxis(axisY);
    }

    if (_statsTopTable) {
        _statsTopTable->setRowCount(0);
        const QList<PostHistoryStore::PostSummary> tops = snapshot.topPosts;
        _statsTopTable->setRowCount(tops.size());
        for (int row = 0; row < tops.size(); ++row) {
            const PostHistoryStore::PostSummary &p = tops.at(row);
            const QStringList values = {
                p.nzbName, p.createdAt, histHumanSize(p.sizeBytes), p.status, p.groups
            };
            for (int col = 0; col < values.size(); ++col) {
                auto *item = new QTableWidgetItem(values.at(col));
                if (col == 3) item->setForeground(statusColor(p.status));
                _statsTopTable->setItem(row, col, item);
            }
        }
        _statsTopTable->resizeColumnsToContents();
        _statsTopTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    }
    });
}

// ============================================================
// Resume tab slots
// ============================================================

void MainWindow::_onResumeSelectionChanged()
{
    if (!_resumeTable) return;
    const QModelIndexList selected = _resumeTable->selectionModel()->selectedRows();
    const bool hasSelection = !selected.isEmpty();
    for (QPushButton *btn : {_resumeResumeBtn, _resumeAbandonBtn, _resumePurgeBtn,
                             _resumeIgnoreBtn, _resumeDeleteBtn})
        if (btn) btn->setEnabled(hasSelection);

    if (!_resumeDetailInfo) return;
    if (!hasSelection) {
        _resumeDetailInfo->setText(tr("<i>Select a post to see resume details.</i>"));
        return;
    }

    const int firstRow = selected.first().row();
    QTableWidgetItem *item = _resumeTable->item(firstRow, 0);
    if (!item) return;
    const qint64 postId = item->data(Qt::UserRole).toLongLong();
    if (!postId || !_ngPost) return;
    PostHistoryService *history = _ngPost->historyService();
    if (!history) return;

    PostHistoryService::ResumeRow dec;
    QString err;
    history->checkResume(postId, &dec, &err);
    const QString stateStr =
        dec.state == QStringLiteral("resumable")
            ? tr("<span style='color:darkgreen'>Resumable</span>")
            : dec.state == QStringLiteral("partial_resumable")
                ? tr("<span style='color:#FFA200'>Partially resumable</span>")
                : tr("<span style='color:darkred'>Not resumable</span>");
    _resumeDetailInfo->setText(
        QStringLiteral("<b>%1</b> \342\200\224 %2 | "
                       "Posted: %3 | Pending: %4 | Failed: %5 | Unknown: %6%7")
            .arg(item->text(), stateStr)
            .arg(dec.postedArticles).arg(dec.pendingArticles)
            .arg(dec.failedArticles).arg(dec.unknownArticles)
            .arg(dec.reason.isEmpty()
                     ? QString()
                     : QStringLiteral("<br/>%1").arg(dec.reason)));
}

void MainWindow::_onResumePost()
{
    if (!_resumeTable || !_ngPost) return;
    const QModelIndexList selected = _resumeTable->selectionModel()->selectedRows();
    if (selected.isEmpty()) return;
    const int res = QMessageBox::question(
        this, tr("Resume post(s)"),
        tr("Resume posting for %1 selected post(s)?\n"
           "Only missing or failed articles will be re-sent.").arg(selected.size()),
        QMessageBox::Yes, QMessageBox::No);
    if (res != QMessageBox::Yes) return;
    int failed = 0;
    for (const QModelIndex &idx : selected) {
        QTableWidgetItem *item = _resumeTable->item(idx.row(), 0);
        if (item && !_startResumePost(item->data(Qt::UserRole).toLongLong(), false))
            ++failed;
    }
    if (failed)
        QMessageBox::warning(this, tr("Resume failed"),
                             tr("%1 post(s) could not be resumed.\n"
                                "Source files may be missing.").arg(failed));
    _refreshHistoryViews();
}

void MainWindow::_onResumeAbandon()
{
    if (!_resumeTable || !_ngPost) return;
    const QModelIndexList selected = _resumeTable->selectionModel()->selectedRows();
    if (selected.isEmpty()) return;
    const int res = QMessageBox::question(
        this, tr("Abandon post(s)"),
        tr("Mark %1 selected post(s) as abandoned?\n"
           "The history entries are kept but the posts will no longer be offered for resume.")
            .arg(selected.size()),
        QMessageBox::Yes, QMessageBox::No);
    if (res != QMessageBox::Yes) return;
    PostHistoryService *history = _ngPost->historyService();
    if (!history) return;
    for (const QModelIndex &idx : selected) {
        QTableWidgetItem *item = _resumeTable->item(idx.row(), 0);
        if (item) {
            QString err;
            history->setPostAbandoned(item->data(Qt::UserRole).toLongLong(), &err);
        }
    }
    _refreshHistoryViews();
}

void MainWindow::_onResumePurge()
{
    if (!_resumeTable || !_ngPost) return;
    const QModelIndexList selected = _resumeTable->selectionModel()->selectedRows();
    if (selected.isEmpty()) return;
    const int res = QMessageBox::question(
        this, tr("Purge resume data"),
        tr("Delete the technical resume data for %1 selected post(s)?\n"
           "The posts will no longer be resumable but the history entries are kept.")
            .arg(selected.size()),
        QMessageBox::Yes, QMessageBox::No);
    if (res != QMessageBox::Yes) return;
    PostHistoryService *history = _ngPost->historyService();
    if (!history) return;
    for (const QModelIndex &idx : selected) {
        QTableWidgetItem *item = _resumeTable->item(idx.row(), 0);
        if (item) {
            QString err;
            history->purgeResumeData(item->data(Qt::UserRole).toLongLong(), &err);
        }
    }
    _refreshHistoryViews();
}

void MainWindow::_onResumeIgnore()
{
    if (!_resumeTable) return;
    for (const QModelIndex &idx : _resumeTable->selectionModel()->selectedRows()) {
        QTableWidgetItem *item = _resumeTable->item(idx.row(), 0);
        if (item)
            _ignoredResumeIds.insert(item->data(Qt::UserRole).toLongLong());
    }
    _refreshHistoryViews();
}

void MainWindow::_onResumeDeleteEntries()
{
    if (!_resumeTable || !_ngPost) return;
    const QModelIndexList selected = _resumeTable->selectionModel()->selectedRows();
    if (selected.isEmpty()) return;
    const int res = QMessageBox::question(
        this, tr("Delete history entries"),
        tr("Permanently delete %1 selected post(s) from the history database?\n"
           "This also removes all associated file and article records.")
            .arg(selected.size()),
        QMessageBox::Yes, QMessageBox::No);
    if (res != QMessageBox::Yes) return;
    PostHistoryService *history = _ngPost->historyService();
    if (!history) return;
    for (const QModelIndex &idx : selected) {
        QTableWidgetItem *item = _resumeTable->item(idx.row(), 0);
        if (item) {
            QString err;
            history->deletePost(item->data(Qt::UserRole).toLongLong(), &err);
        }
    }
    _refreshHistoryViews();
}

// ============================================================
// History table context menu (reveal password)
// ============================================================

void MainWindow::_onHistoryContextMenu(const QPoint &pos)
{
    if (!_historyTable || !_ngPost) return;
    QTableWidgetItem *clicked = _historyTable->itemAt(pos);
    if (!clicked) return;

    const int row = clicked->row();
    QTableWidgetItem *idItem = _historyTable->item(row, 0);
    if (!idItem) return;
    _historyTable->setCurrentCell(row, clicked->column());
    _onHistoryRowSelected(row);
    const qint64 postId = idItem->data(Qt::UserRole).toLongLong();
    if (!postId) return;

    PostHistoryService *history = _ngPost->historyService();
    if (!history) return;

    PostHistoryStore::PostDetails details;
    QString err;
    if (!history->loadPostDetails(postId, &details, &err)) return;

    QMenu menu(this);
    QAction *resumeAct = nullptr;
    PostHistoryService::ResumeRow dec;
    if (history->checkResume(postId, &dec, &err))
        resumeAct = menu.addAction(tr("Resume"));

    QAction *revealAct = nullptr;
    QAction *copyAct = nullptr;
    if (details.post.hasPassword && details.post.passwordStored) {
        if (!menu.actions().isEmpty())
            menu.addSeparator();
        revealAct = menu.addAction(tr("Reveal password"));
        copyAct   = menu.addAction(tr("Copy password"));
    }

    if (menu.actions().isEmpty())
        return;

    const QAction *chosen = menu.exec(_historyTable->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == resumeAct) {
        _startResumePost(postId, true);
    } else if (chosen == revealAct) {
        QMessageBox box(this);
        box.setWindowTitle(tr("Archive password"));
        box.setText(tr("Password for <b>%1</b>:").arg(details.post.nzbName));
        box.setInformativeText(details.rarPass);
        box.setIcon(QMessageBox::Information);
        box.setStandardButtons(QMessageBox::Ok);
        box.exec();
    } else if (chosen == copyAct) {
        QApplication::clipboard()->setText(details.rarPass);
        statusBar()->showMessage(tr("Password copied to clipboard."), 4000);
    }
}

void MainWindow::onVpnSettingsClicked()
{
    if (!_ngPost->vpnManager())
        return;
    VpnSettingsDialog dlg(_ngPost->vpnManager(), this);
    dlg.exec();
}

void MainWindow::onVpnStateChanged(VpnManager::State newState)
{
    QString text;
    switch (newState) {
    case VpnManager::State::Disabled:  text = tr("VPN: disabled");                 break;
    case VpnManager::State::Starting:  text = tr("VPN: starting...");              break;
    case VpnManager::State::Connected:
        text = tr("VPN: connected (%1)").arg(_ngPost->vpnManager()->tunInterface());
        break;
    case VpnManager::State::Stopping:  text = tr("VPN: stopping...");              break;
    case VpnManager::State::Failed:    text = tr("VPN: failed");                   break;
    }
    _ui->vpnStateLbl->setText(text);
}


const QString MainWindow::sGroupBoxStyle =  "\
        QGroupBox {\
        font: bold; \
        border: 1px solid palette(mid);\
        border-radius: 6px;\
        margin-top: 6px;\
        }\
        QGroupBox::title {\
        subcontrol-origin:  margin;\
        left: 7px;\
        padding: 0 5px 0 5px;\
        }";

const QString MainWindow::sTabWidgetStyle = "\
        QTabWidget::pane {\
            border-top: 2px solid palette(mid);\
        }\
        QTabWidget::tab-bar {\
            left: 5px;\
        }\
        QTabBar::tab {\
            background: palette(button);\
            color: palette(buttonText);\
            border: 2px solid palette(mid);\
            border-bottom-color: palette(window);\
            border-top-left-radius: 4px;\
            border-top-right-radius: 4px;\
            min-width: 8ex;\
            padding: 2px;\
        }\
        QTabBar::tab:selected, QTabBar::tab:hover {\
            background: palette(window);\
            color: palette(windowText);\
        }\
        QTabBar::tab:selected {\
            border-color: palette(dark);\
            border-bottom-color: palette(window);\
            margin-left: -4px;\
            margin-right: -4px;\
        }\
        QTabBar::tab:!selected {\
            margin-top: 2px;\
        }\
        QTabBar::tab:first:selected { margin-left: 0; }\
        QTabBar::tab:last:selected  { margin-right: 0; }\
        QTabBar::tab:only-one       { margin: 0; }\
        QTabBar::scroller { width: 40px; }\
        QTabBar QToolButton::right-arrow { image: url(:/icons/right.png); }\
        QTabBar QToolButton::left-arrow  { image: url(:/icons/left.png);  }\
";
