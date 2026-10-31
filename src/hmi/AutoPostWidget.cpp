//========================================================================
//
// Copyright (C) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
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

#include "AutoPostWidget.h"
#include "ui_AutoPostWidget.h"
#include "DependentControl.h"
#include "PostInfoDialog.h"
#include "PostingWidget.h"
#include "MainWindow.h"
#include "NgPost.h"
#include "FoldersMonitorForNewFiles.h"
#include <QFileDialog>
#include <QFrame>
#include <QDebug>
#include <QMessageBox>
#include <QKeyEvent>
#include <QMimeData>
#include <QStandardItemModel>

namespace
{

//! The rows of the password choice, in the order of AutoPostWidget.ui.
enum PassRow {
    kNoPass,
    kRandomPass,
    kDefaultPass
};

NgPost::AutoPass passChoice(QComboBox const *box)
{
    switch (box->currentIndex()) {
    case kRandomPass: return NgPost::AutoPass::Random;
    case kDefaultPass: return NgPost::AutoPass::Default;
    default: return NgPost::AutoPass::None;
    }
}

void setPassChoice(QComboBox *box, NgPost::AutoPass pass)
{
    box->setCurrentIndex(pass == NgPost::AutoPass::Random        ? kRandomPass
                             : pass == NgPost::AutoPass::Default ? kDefaultPass
                                                                 : kNoPass);
}

} // namespace

AutoPostWidget::AutoPostWidget(NgPost *ngPost, MainWindow *hmi) :
    QWidget(hmi),
    _ui(new Ui::AutoPostWidget),
    _hmi(hmi),
    _ngPost(ngPost),
    _isMonitoring(false),
    _currentPostIdx(1)
{
    _ui->setupUi(this);
    _buildPostInfoRow();
    _ui->filesList->setSignature(QString("<pre>%1</pre>").arg(_ngPost->escapeXML(_ngPost->asciiArt())));
    connect(_ui->filesList, &SignedListWidget::rightClick, this, &AutoPostWidget::onSelectFilesClicked);
}

AutoPostWidget::~AutoPostWidget()
{
    delete _ui;
}

void AutoPostWidget::init()
{
    // Its own settings (AUTO_*): neither PACK nor the defaults of a posting tab.
    NgPost::AutoPacking const &packing = _ngPost->_autoPacking;
    _ui->keepRarCB->setChecked(packing.keepRar);

    _ui->redundancySB->setRange(-1, 100);
    _ui->redundancySB->setValue(packing.par2Pct);
    refreshPar2Default();
    connect(_ngPost, &NgPost::par2DefaultsChanged, this, &AutoPostWidget::refreshPar2Default, Qt::UniqueConnection);
    // The label that carried the unit is gone with the row it lived on, so the
    // suffix carries it instead, as on a posting tab.
    _ui->redundancySB->setSuffix(QStringLiteral(" %"));

    _ui->autoDirEdit->setText(_ngPost->_inputDir);
    PostingWidget::loadObfuscationLengths(_ui->nameLengthSB, _ui->passLengthSB, _ngPost);
    _ui->nameLengthSB->setValue(static_cast<int>(packing.lengthName));
    _ui->passLengthSB->setValue(static_cast<int>(packing.lengthPass));

    _ui->filesList->setSelectionMode(QAbstractItemView::ExtendedSelection);

    connect(_ui->autoDirButton,     &QAbstractButton::clicked, this, &AutoPostWidget::onSelectAutoDirClicked);
    connect(_ui->scanAutoDirButton, &QAbstractButton::clicked, this, &AutoPostWidget::onScanAutoDirClicked);
    connect(_ui->compressCB,        &QAbstractButton::toggled, this, &AutoPostWidget::onCompressToggled);
    connect(_ui->par2CB,            &QAbstractButton::toggled, this, &AutoPostWidget::onGenPar2Toggled);
    connect(_ui->passModeCB,
            &QComboBox::currentIndexChanged,
            this,
            &AutoPostWidget::onPassChoiceChanged);
    // Kept as soon as the user picks it: a save only reads the tab in front.
    connect(_ui->passModeCB, &QComboBox::activated, this, [this] {
        _ngPost->_autoPass = passChoice(_ui->passModeCB);
    });

    _ui->compressCB->setChecked(packing.compress);
    _ui->genNameCB->setChecked(packing.genName);
    setPassChoice(_ui->passModeCB, _ngPost->autoPassChoice());
    refreshPasswordChoices();
    _ui->par2CB->setChecked(packing.doPar2);
    // setChecked() only emits toggled() when a box actually changes, so the
    // dependents are refreshed here rather than trusting a signal that may not
    // have fired. The coupling side effects of the handlers are deliberately
    // not replayed: opening the tab must not tick PAR2 on the user's behalf.
    updatePackingDependents();
    _ui->startJobsCB->setChecked(true);

    _ui->latestFilesFirstCB->setChecked(true);

    connect(_ui->postButton,   &QAbstractButton::clicked, this,    &AutoPostWidget::onGenQuickPosts);

    connect(_ui->monitorButton, &QAbstractButton::clicked, this, &AutoPostWidget::onMonitoringClicked);

    connect(_ui->delFilesCB, &QAbstractButton::toggled, this, &AutoPostWidget::onDelFilesToggled);


    _ui->addMonitoringFolderButton->setEnabled(false);
    connect(_ui->addMonitoringFolderButton, &QAbstractButton::clicked, this, &AutoPostWidget::onAddMonitoringFolder);


    _ui->extensionFilterEdit->setText(_ngPost->_monitorExtensions.isEmpty()? "" : _ngPost->_monitorExtensions.join(","));
    _ui->dirAllowedCB->setChecked(!_ngPost->_monitorIgnoreDir);
    _ui->copyNfoCB->setChecked(_ngPost->_autoIncludeNfo);

    _ui->folderBox->setStyleSheet(MainWindow::sGroupBoxStyle);
    _ui->scanBox->setStyleSheet(MainWindow::sGroupBoxStyle);
    _ui->monitorBox->setStyleSheet(MainWindow::sGroupBoxStyle);
    // Irreversible: marked like the delete buttons of the history.
    _ui->delFilesCB->setStyleSheet(QStringLiteral("QCheckBox { color: #dd4444; }"));
}


void AutoPostWidget::onGenQuickPosts()
{
    bool compress = _ui->compressCB->isChecked();

    QFileInfoList files;
    for (int i = 0 ; i < _ui->filesList->count() ; ++i)
    {
        QFileInfo fileInfo(_ui->filesList->item(i)->text());
        if (fileInfo.exists())
            files << fileInfo;
        if (!compress && fileInfo.isDir())
        {
            _ngPost->error(tr("You can't use auto posting without compression on folders... (%1)").arg(fileInfo.fileName()));
            return ;
        }
    }
    if (files.isEmpty())
    {
        QMessageBox::warning(nullptr,
                             tr("Nothing to post..."),
                             tr("There is nothing to post!\n\
Press the Scan button and remove what you don't want to post ;)\n\
(To remove files, select in the list and press DEL or BackSpace)"));
        return;
    }

    _hmi->updateServers();
    _hmi->updateParams();

    bool startPost = _ui->startJobsCB->isChecked();
    for (const QFileInfo &file : files)
    {
        // For every post, as for a monitored file: the previous post wrote its
        // tab's values (password length...) over the shared ones.
        udatePostingParams();
        // "copy nfo alongside other files": a lone .nfo that has a sibling is not
        // posted on its own, and any other file pulls in its sibling .nfo so both
        // end up in the same post (udatePostingParams() above refreshed the flag)
        if (_ngPost->_autoIsBundledNfo(file))
            continue;

        QFileInfoList postFiles{file};
        QFileInfo nfo = _ngPost->_autoSiblingNfo(file);
        if (nfo.isFile())
            postFiles << nfo;

        // This tab's packing, archive password included, before the files:
        // without a random archive name, the first file gives its own.
        PostingWidget *quickPostWidget = _hmi->addNewQuickTab();
        quickPostWidget->applyAutoPacking();
        for (const QFileInfo &postFile : postFiles)
            quickPostWidget->addPath(postFile.absoluteFilePath(), 0, postFile.isDir());
        // One choice for the whole run: every post it launches gets it.
        quickPostWidget->setPostInfo(_postInfoCB && _postInfoCB->isChecked(),
                                     _postInfoTemplate,
                                     _postInfoOutput,
                                     _postInfoMeta);

        if (startPost)
            quickPostWidget->postFiles(false);
    }
}


void AutoPostWidget::onSelectAutoDirClicked()
{
    QString path = QFileDialog::getExistingDirectory(
                this,
                tr("Select a Folder"),
                _ui->autoDirEdit->text(),
                QFileDialog::ShowDirsOnly);

    if (!path.isEmpty())
        _ui->autoDirEdit->setText(path);
}

void AutoPostWidget::onScanAutoDirClicked()
{
    QDir autoDir(_ui->autoDirEdit->text());
    if (autoDir.exists())
    {
        _ui->filesList->clear2();
        QDir::SortFlags sort = _ui->latestFilesFirstCB->isChecked() ? QDir::Time : QDir::Name;
        for (const QFileInfo &file : autoDir.entryInfoList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot, sort))
            _ui->filesList->addPathIfNotInList(file.absoluteFilePath(), 0, file.isDir());
    }
    else
        QMessageBox::warning(nullptr,
                             tr("No auto directory selected..."),
                             tr("There is no auto directory!\nPlease select one."));
}

void AutoPostWidget::onMonitoringClicked()
{
    if (_isMonitoring)
    {
        if (_ngPost->hasMonitoringPostingJobs())
        {
            int res = QMessageBox::question(this,
                                  tr("Ongoing Monitoring post"),
                                  tr("There are still ongoing or pending Monitoring Posts.\n We're going to stop all of them.\nAre you sure you want to proceed?")
                                  );
            if (res == QMessageBox::No)
                return;
            else
                _ngPost->closeAllMonitoringJobs();
        }
        _ui->filesList->clear2();
        _ngPost->_stopMonitoring();
        _ui->monitorButton->setText(tr("Monitor Folder"));
    }
    else
    {
        _currentPostIdx = 1;
        _ui->filesList->clear2();
        QString folderPath = _ui->autoDirEdit->text();
        QFileInfo fi(folderPath);
        if (!fi.exists() || !fi.isDir() || !fi.isReadable())
        {
            QMessageBox::warning(this, tr("Error accessing Auto Dir..."), tr("The auto directory must exist and be readable..."));
            return;
        }
        if (!_ui->compressCB->isChecked()) {
            QMessageBox::warning(this,
                                 tr("To be implemented..."),
                                 tr("You can't monitor a folder without compression using the GUI...\nIt's possible in command line if MONITOR_IGNORE_DIR is enabled in your configuration file."));
            return;
        }
        _ui->filesList->addItem(new QListWidgetItem(QIcon(":/icons/monitor.png"), tr("Monitoring %1").arg(folderPath)));
        _ngPost->_startMonitoring(folderPath);
        _ui->monitorButton->setText(tr("Stop Monitoring"));
    }

    _ui->autoDirEdit->setEnabled(_isMonitoring);
    _ui->autoDirButton->setEnabled(_isMonitoring);
    // The other mode as a whole, title included, so it reads as unavailable.
    _ui->scanBox->setEnabled(_isMonitoring);

    _ui->compressCB->setEnabled(_isMonitoring);
    _ui->par2CB->setEnabled(_isMonitoring);

    _isMonitoring = !_isMonitoring;
    // Frozen while monitoring, answering to their own switch otherwise.
    updatePackingDependents();

    _ui->addMonitoringFolderButton->setEnabled(_isMonitoring);

    if (_isMonitoring)
        _ui->filesList->setSelectionMode(QAbstractItemView::NoSelection);
    else
        _ui->filesList->setSelectionMode(QAbstractItemView::ExtendedSelection);
}

void AutoPostWidget::newFileToProcess(const QFileInfo &fileInfo)
{
    const QIcon icon(fileInfo.isDir() ? ":/icons/folder.png" : ":/icons/file.png");
    auto *newItem = new QListWidgetItem(icon, QString("- %1").arg(fileInfo.absoluteFilePath()));
    newItem->setData(Qt::UserRole, true);
    newItem->setForeground(_hmi->pendingColor());
    _ui->filesList->addItem(newItem);
}

void AutoPostWidget::onDelFilesToggled(bool checked)
{
    if (checked)
        QMessageBox::warning(this,
                         tr("Deleting files/folders once posted"),
                         tr("You're about to delete files from your computer once they've been posted!\nUse it at your own risk!\nIt will be irreversible..."));
    _ngPost->setDelFilesAfterPosted(checked);
}

void AutoPostWidget::onAddMonitoringFolder()
{
    QString path = QFileDialog::getExistingDirectory(
                this,
                tr("Select a Monitoring Folder to add"),
                _ui->autoDirEdit->text(),
                QFileDialog::ShowDirsOnly);

    if (!path.isEmpty())
    {
        QFileInfo newDir(path);
        if (newDir.exists() && newDir.isDir() && newDir.isReadable())
        {
            _ui->filesList->addItem(new QListWidgetItem(QIcon(":/icons/monitor.png"), tr("Monitoring %1").arg(newDir.absoluteFilePath())));
            _ngPost->addMonitoringFolder(newDir.absoluteFilePath());
        }
    }
}

void AutoPostWidget::updatePackingDependents()
{
    bool const compress = _ui->compressCB->isChecked();
    QString const needsCompress = tr("requires: %1").arg(_ui->compressCB->text());

    setDependentEnabled(_ui->genNameCB, compress,
                        tr("generate a random name for the archive"), needsCompress);
    setDependentEnabled(_ui->nameLengthSB, compress,
                        tr("length of the random archive name"), needsCompress);
    setDependentEnabled(_ui->passModeCB,
                        compress,
                        tr("password of the archives: none, a random one, or the default password "
                           "of the Compression Settings"),
                        needsCompress);
    bool const random = passChoice(_ui->passModeCB) == NgPost::AutoPass::Random;
    setDependentEnabled(_ui->passLengthSB,
                        compress && random,
                        tr("length of the random archive password"),
                        compress ? tr("requires: %1").arg(_ui->passModeCB->itemText(kRandomPass))
                                 : needsCompress);
    PostingWidget::showKeepArchivesRule(_ui->keepRarCB, _ui->compressCB, _ui->par2CB);

    setDependentEnabled(
        _ui->redundancySB,
        _ui->par2CB->isChecked(),
        tr("Using the PAR2 arguments of the configuration: %1").arg(_ngPost->par2ArgsInUse()),
        tr("requires: %1").arg(_ui->par2CB->text()));

    // Every new file of a monitored folder reads these: frozen until it stops,
    // whatever calls this meanwhile (a language change, new PAR2 defaults).
    if (_isMonitoring) {
        QWidget *const frozen[] = { _ui->genNameCB,    _ui->nameLengthSB, _ui->passModeCB,
                                    _ui->passLengthSB, _ui->keepRarCB,    _ui->redundancySB };
        // Null-safe like setDependentEnabled() above, or clang-analyzer takes
        // its guard as a hint that one of them may be missing.
        for (QWidget *widget : frozen)
            if (widget)
                widget->setEnabled(false);
    }
}

void AutoPostWidget::onCompressToggled(bool checked)
{
    updatePackingDependents();
    if (!checked && !_ui->par2CB->isChecked())
        _ui->par2CB->setChecked(true);
}

void AutoPostWidget::onGenPar2Toggled(bool checked)
{
    updatePackingDependents();
    if (!checked && !_ui->compressCB->isChecked())
        _ui->compressCB->setChecked(true);
}

#include "PostingJob.h"
void AutoPostWidget::onMonitorJobStart()
{
    updateStartedJob(static_cast<PostingJob *>(sender())->getFirstOriginalFile());
}

void AutoPostWidget::updateStartedJob(const QString &path)
{
    QString srcPath = QString("- %1").arg(path);
    int nbFiles = _ui->filesList->count();
    for (int i = _currentPostIdx; i < nbFiles; ++i)
    {
        QListWidgetItem *item = _ui->filesList->item(i);
        if (item->text() == srcPath)
        {
            item->setForeground(_hmi->sPostingColor);
            _ui->filesList->followItem(item);
            _currentPostIdx = i;
            break;
        }
    }
}

void AutoPostWidget::onSelectFilesClicked()
{
    if (!_isMonitoring)
    {
        QStringList files = QFileDialog::getOpenFileNames(
                    this,
                    tr("Select one or more files"),
                    _ngPost->_inputDir);

        int currentNbFiles = _ui->filesList->count();
        for (const QString &file : files)
            _ui->filesList->addPathIfNotInList(file, currentNbFiles);
    }
}

void AutoPostWidget::udatePostingParams()
{
    // Its own settings only (AUTO_*). It used to write the shared packing
    // flags, which the posting tabs start from and PACK was saved from: the
    // last Auto Posting run decided what a new posting tab looked like.
    NgPost::AutoPacking &packing = _ngPost->_autoPacking;
    packing.compress = _ui->compressCB->isChecked();
    packing.genName = _ui->genNameCB->isChecked();
    packing.doPar2 = _ui->par2CB->isChecked();
    packing.lengthName = static_cast<uint>(_ui->nameLengthSB->value());
    packing.lengthPass = static_cast<uint>(_ui->passLengthSB->value());
    packing.keepRar = _ui->keepRarCB->isChecked();
    packing.par2Pct = _ui->redundancySB->value();
    _ngPost->_autoPass = passChoice(_ui->passModeCB);

    QFileInfo inputDir(_ui->autoDirEdit->text());
    if (inputDir.exists() && inputDir.isDir() && inputDir.isWritable())
        _ngPost->_inputDir = inputDir.absoluteFilePath();


    _ngPost->_monitorExtensions.clear();
    if (!_ui->extensionFilterEdit->text().isEmpty())
    {
        for (QString &extension : _ui->extensionFilterEdit->text().split(","))
        {
            if (!extension.trimmed().isEmpty())
                _ngPost->_monitorExtensions << extension.trimmed();
        }
    }

    _ngPost->_monitorIgnoreDir = !_ui->dirAllowedCB->isChecked();
    _ngPost->_autoIncludeNfo = _ui->copyNfoCB->isChecked();
}

void AutoPostWidget::updateFinishedJob(const QString &path, uint nbArticles, uint nbUploaded, uint nbFailed)
{
    _handleFinishedJob(path, nbArticles, nbUploaded, nbFailed);
}

void AutoPostWidget::_handleFinishedJob(const QString &path,
                                        uint nbArticles,
                                        uint nbUploaded,
                                        uint nbFailed)
{
    QString srcPath = QString("- %1").arg(path);
    int nbFiles = _ui->filesList->count();
    for (int i = 1; i < nbFiles; ++i) {
        QListWidgetItem *item = _ui->filesList->item(i);
        if (item->text() == srcPath)
        {
            QColor color(_hmi->sDoneOKColor);
            if (nbUploaded > 0 && nbFailed == 0)
                item->setText(QString("%1 [%2 ok]").arg(srcPath).arg(nbArticles));
            else
            {
                item->setText(QString("%1 [%2 err / %3]").arg(srcPath).arg(nbFailed).arg(nbArticles));
                if (nbFailed == nbArticles)
                    color = _hmi->sDoneKOColor;
                else
                    color = _hmi->sArticlesFailedColor;
            }
            item->setData(Qt::UserRole, false);
            item->setForeground(color);
            // No scroll: the next job's start brings the list there.
            break;
        }
    }
}

bool AutoPostWidget::deleteFilesOncePosted() const { return _ui->delFilesCB->isChecked(); }

void AutoPostWidget::retranslate()
{
    _ui->retranslateUi(this);
    // retranslateUi() puts back the .ui text, which is the idle one.
    if (_isMonitoring)
        _ui->monitorButton->setText(tr("Stop Monitoring"));
    refreshPar2Default();
    // retranslateUi() has just reset the tooltips to the plain .ui text, which
    // drops what a greyed control is waiting for. Rebuild both halves.
    updatePackingDependents();
    _ui->filesList->setToolTip(QString("%1<br/><br/>%2<ul><li>%3</li><li>%4</li><li>%5</li></ul>%6").arg(
                                   tr("You can use the <b>Monitor Mode</b>")).arg(
                                   tr("or <b>Generate Posts</b> by adding files:")).arg(
                                   tr("Drag & Drop files/folders")).arg(
                                   tr("Right Click to add Files")).arg(
                                   tr("Click on the Scan Button")).arg(
                                   tr("Bare in mind you can select items in the list and press DEL to remove them")));
}

void AutoPostWidget::refreshPasswordChoices()
{
    // The default password can only be chosen while there is one. Gone, the
    // archives stay protected by a random one, never by a password nobody set.
    bool const hasDefault = !_ngPost->_rarPassFixed.isEmpty();
    auto *rows = qobject_cast<QStandardItemModel *>(_ui->passModeCB->model());
    if (QStandardItem *row = rows ? rows->item(kDefaultPass) : nullptr)
        row->setEnabled(hasDefault);
    if (!hasDefault && passChoice(_ui->passModeCB) == NgPost::AutoPass::Default)
        setPassChoice(_ui->passModeCB, NgPost::AutoPass::Random);
}

void AutoPostWidget::onPassChoiceChanged()
{
    // The controls stay greyed while monitoring; the choice itself only moves
    // there when the default password goes away.
    if (!_isMonitoring)
        updatePackingDependents();
}


void AutoPostWidget::handleKeyEvent(QKeyEvent *keyEvent)
{
    if (!_isMonitoring)
    {
        qDebug() << "[AutoPostWidget::handleKeyEvent] key event: " << keyEvent->key();
        if(keyEvent->key() == Qt::Key_Delete || keyEvent->key() == Qt::Key_Backspace)
        {
            for (QListWidgetItem *item : _ui->filesList->selectedItems())
            {
                qDebug() << "[AutoPostWidget::handleKeyEvent] remove item: " << item->text();
                _ui->filesList->removeItemWidget2(item);
                delete item;
            }
        }
    }
}


void AutoPostWidget::handleDropEvent(QDropEvent *e)
{
    if (!_isMonitoring)
    {
        int currentNbFiles = _ui->filesList->count();
        for (const QUrl &url : e->mimeData()->urls())
        {
            QString fileName = url.toLocalFile();
            _ui->filesList->addPathIfNotInList(fileName, currentNbFiles, QFileInfo(fileName).isDir());
        }
    }
}

void AutoPostWidget::_buildPostInfoRow()
{
    _postInfoCB = new QCheckBox(this);
    _postInfoCB->setObjectName(QStringLiteral("autoPostInfoCB"));
    _postInfoCB->setChecked(!_ngPost->postInfoTemplatePath().isEmpty());

    _postInfoButton = new QPushButton(this);
    _postInfoButton->setObjectName(QStringLiteral("autoPostInfoButton"));
    _postInfoButton->setEnabled(_postInfoCB->isChecked());

    connect(_postInfoCB, &QCheckBox::toggled, this, &AutoPostWidget::onPostInfoToggled);
    connect(_postInfoButton, &QAbstractButton::clicked, this, &AutoPostWidget::onEditPostInfo);

    // Like the nfo, the sheet goes with every post the folder gives, in either
    // mode: it sits on the folder line, as on line A of a posting tab.
    QFrame *sep = new QFrame(this);
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Sunken);
    _ui->autoDirLayout->addWidget(sep);
    _ui->autoDirLayout->addWidget(_postInfoCB);
    _ui->autoDirLayout->addWidget(_postInfoButton);
    _ui->autoDirLayout->setStretchFactor(_ui->autoDirEdit, 1);

    retranslatePostInfoTexts();
}

void AutoPostWidget::retranslatePostInfoTexts()
{
    if (!_postInfoCB)
        return;
    _postInfoCB->setText(tr("Create a post info file"));
    // Said plainly: this is not a per post choice here.
    _postInfoCB->setToolTip(tr("Applies to every post this auto posting launches."));
    _postInfoButton->setText(tr("Post information\342\200\246"));
    _postInfoButton->setToolTip(tr("Model and fields used by all the posts of this run."));
}

//! An auto posting run knows none of the per post names yet: every post it
//! creates gets its own nzb and archive. Only the settings shared by the whole
//! run can be shown.
PostInfoData AutoPostWidget::_postInfoPreview() const
{
    PostInfoData postData;
    postData.appVersion = QString(APP_VERSION);
    postData.groups = _ngPost->groups();
    if (!_ngPost->_genFrom && !_ngPost->_from.empty())
        postData.nzbPoster = QString::fromStdString(_ngPost->_from);
    int const redundancy = _ui->redundancySB->value();
    postData.par2Pct = !_ui->par2CB->isChecked() ? -1
        : redundancy < 0                         ? static_cast<int>(_ngPost->_par2PctDefault)
                                                 : redundancy;
    return postData;
}

void AutoPostWidget::onPostInfoToggled(bool checked) { _postInfoButton->setEnabled(checked); }

void AutoPostWidget::onEditPostInfo()
{
    PostInfoDialog dlg(_ngPost->postInfoTemplatePath(),
                       _postInfoTemplate,
                       _postInfoMeta,
                       _ngPost->sessionPostInfoTemplates(),
                       _postInfoPreview(),
                       _ngPost->postInfoOutputPattern(),
                       _postInfoOutput,
                       this);
    int const answer = dlg.exec();

    // The list of models is kept whatever the answer: opening a file, or
    // dropping one from the list, is housekeeping, not a change to this post.
    _ngPost->setSessionPostInfoTemplates(dlg.sessionTemplates());

    if (answer != QDialog::Accepted)
        return;

    QString duplicate;
    const QMap<QString, MetaValue> meta = dlg.meta(&duplicate);
    if (!duplicate.isEmpty())
    {
        _hmi->logError(tr("The post information lists '%1' twice. "
                          "Remove one of the two lines.")
                           .arg(duplicate));
        return;
    }

    _postInfoTemplate = dlg.templateOverride();
    _postInfoOutput   = dlg.outputOverride();
    _postInfoMeta     = meta;

    if (dlg.setAsDefault())
    {
        _ngPost->setPostInfoTemplate(_postInfoTemplate.isEmpty() ? _ngPost->postInfoTemplatePath()
                                                                 : _postInfoTemplate);
        _postInfoTemplate.clear();
        if (!_postInfoOutput.isEmpty())
        {
            _ngPost->setPostInfoOutput(_postInfoOutput);
            _postInfoOutput.clear();
        }
        _ngPost->saveConfig();
        _hmi->log(tr("Post info defaults saved: model %1, written to %2")
                      .arg(_ngPost->postInfoTemplatePath(), _ngPost->postInfoOutputPattern()));
    }
}

void AutoPostWidget::refreshPar2Default()
{
    PostingWidget::showPar2Default(_ui->redundancySB, _ngPost->par2DefaultPercentage());
    // The tooltip of the redundancy names the arguments a post runs with.
    updatePackingDependents();
}

void AutoPostWidget::refreshPendingColors()
{
    for (int i = 0; i < _ui->filesList->count(); ++i) {
        auto *item = _ui->filesList->item(i);
        if (item->data(Qt::UserRole).toBool())
            item->setForeground(_hmi->pendingColor());
    }
}
