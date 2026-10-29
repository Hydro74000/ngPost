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

#include "PostingWidget.h"
#include "ui_PostingWidget.h"
#include "CheckBoxCenterWidget.h"
#include "DependentControl.h"
#include "PostInfoDialog.h"
#include "MainWindow.h"
#include "NgPost.h"
#include "PostingJob.h"
#include "history/PostHistoryService.h"
#include "nntp/NntpFile.h"

#include <QCheckBox>
#include <QDebug>
#include <QGroupBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QFrame>
#include <QMessageBox>
#include <QFileDialog>
#include <QDir>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMimeData>
#include <QStatusBar>
#include <QTimer>
#include <QToolTip>

#include <utility>

namespace
{

//! Set on a file once posted: files finish out of order, and a name may well
//! contain what the "[n ok]" suffix says.
constexpr int kFilePostedRole = Qt::UserRole;

//! The switches the PACK keywords ask of a post, all off while PACK is.
struct PackingChoice
{
    bool compress = false;
    bool genName = false;
    bool genPass = false;
    bool doPar2 = false;
};

PackingChoice packingChoice(bool enabled, const QStringList &keys)
{
    PackingChoice choice;
    if (!enabled)
        return choice;
    for (const QString &key : keys) {
        QString const keyWord = key.toLower();
        if (keyWord == NgPost::optionName(NgPost::Opt::COMPRESS))
            choice.compress = true;
        else if (keyWord == NgPost::optionName(NgPost::Opt::GEN_NAME))
            choice.genName = true;
        else if (keyWord == NgPost::optionName(NgPost::Opt::GEN_PASS))
            choice.genPass = true;
        else if (keyWord == NgPost::optionName(NgPost::Opt::GEN_PAR2))
            choice.doPar2 = true;
    }
    return choice;
}

} // namespace

PostingWidget::PostingWidget(NgPost *ngPost, MainWindow *hmi, uint jobNumber)
    : QWidget(hmi)
    , _ui(new Ui::PostingWidget)
    , _hmi(hmi)
    , _ngPost(ngPost)
    , _jobNumber(jobNumber)
    , _postingJob(nullptr)
    , _state(STATE::IDLE)
    , _postingFinished(false)
    , _postInfoCB(nullptr)
    , _postInfoButton(nullptr)
{
    _ui->setupUi(this);
    _setupCopyActions();
    _ui->postButton->setText(tr("Start Quick Post #%1").arg(displayNumber()));
    connect(_ui->filesList->model(), &QAbstractItemModel::rowsInserted, this, &PostingWidget::submissionEligibilityChanged);
    connect(_ui->filesList->model(), &QAbstractItemModel::rowsRemoved, this, &PostingWidget::submissionEligibilityChanged);
    connect(_ui->filesList->model(), &QAbstractItemModel::modelReset, this, &PostingWidget::submissionEligibilityChanged);
    // Emitted on every change of the post's state as well as of the list.
    connect(this,
            &PostingWidget::submissionEligibilityChanged,
            this,
            &PostingWidget::_refreshFileControls);
    _buildPostInfoRow();
    connect(_ui->postButton, &QAbstractButton::clicked, this, &PostingWidget::onPostFiles);
    connect(_ui->nzbPassCB,  &QAbstractButton::toggled, this, &PostingWidget::onNzbPassToggled);
    connect(_ui->genPass,    &QAbstractButton::clicked, this, &PostingWidget::onGenNzbPassword);
    connect(_ui->par2CB,     &QAbstractButton::toggled, this, &PostingWidget::onPar2CB);

    _ui->filesList->setSignature(QString("<pre>%1</pre>").arg(_ngPost->escapeXML(_ngPost->asciiArt())));
    connect(_ui->filesList, &SignedListWidget::rightClick, this, &PostingWidget::onSelectFilesClicked);
}

PostingWidget::~PostingWidget()
{
    // The file list and its model are children, deleted by ~QWidget() after
    // this body has run. The model announces modelReset as it goes, and Qt
    // only drops this object's connections in ~QObject(), later still: the
    // signal would reach a PostingWidget that no longer exists (UBSan).
    disconnect(_ui->filesList->model(), nullptr, this, nullptr);
    delete _ui;
}

void PostingWidget::onFilePosted(const QString &filePath, uint nbArticles, uint nbFailed)
{
    _handleFilePosted(filePath, nbArticles, nbFailed);
}

void PostingWidget::_handleFilePosted(const QString &filePath, uint nbArticles, uint nbFailed)
{
    int const nbFiles = _ui->filesList->count();
    // Every file above the one in progress is posted already.
    for (int i = qMax(0, _ui->filesList->followedRow()); i < nbFiles; ++i) {
        QListWidgetItem *item = _ui->filesList->item(i);
        if (item && item->text() == filePath) {
            QColor color(_hmi->sDoneOKColor);
            if (nbFailed == 0)
                item->setText(QString("%1 [%2 ok]").arg(filePath).arg(nbArticles));
            else {
                item->setText(QString("%1 [%2 err / %3]").arg(filePath).arg(nbFailed).arg(nbArticles));
                if (nbFailed == nbArticles)
                    color = _hmi->sDoneKOColor;
                else
                    color = _hmi->sArticlesFailedColor;
            }
            item->setForeground(color);
            item->setData(kFilePostedRole, true);
            if (!_highlightFirstPendingFile())
                _ui->filesList->followItem(item);
            break;
        }
    }
}

void PostingWidget::onArchiveFileNames(const QStringList &paths)
{
    _setArchiveFilesAndHighlight(paths);
}

void PostingWidget::_setArchiveFilesAndHighlight(const QStringList &paths)
{
    _ui->filesList->clear();
    for (const QString &path : paths)
        _ui->filesList->addPath(path);
    _highlightFirstPendingFile();
}

void PostingWidget::onArticlesNumber(int nbArticles)
{
    Q_UNUSED(nbArticles);
    _hmi->refreshJobLabel();
}

void PostingWidget::onPostingJobDone()
{
    _handlePostingJobDone();
}

void PostingWidget::_handlePostingJobDone()
{
    // Both postingFinished and noMoreConnection can notify the same completion.
    if (!_postingJob)
        return;

    // Detached before the tab is decorated: restoring the sources refreshes
    // every tab, and one still holding a job that is no longer the active one
    // gets the queued icon back. We don't own it, NgPost will delete it.
    PostingJob *job = std::exchange(_postingJob, nullptr);
    disconnect(job);
    _postingFinished = true;

    if (job->nbArticlesTotal() > 0) {
        if (job->nbArticlesFailed() > 0)
            _hmi->updateJobTab(this, _hmi->sDoneKOColor, QIcon(_hmi->sDoneKOIcon));
        else
            _hmi->updateJobTab(this, _hmi->sDoneOKColor, QIcon(_hmi->sDoneOKIcon));
    } else
        _hmi->clearJobTab(this);

    _armResubmission(job);
    _clearActivePostingHighlights();
    setIDLE();
}

bool PostingWidget::canSubmit() const
{
    return acceptsFiles() && _ui->filesList->count() > 0;
}

QString PostingWidget::nzbFolder() const
{
    QString const nzb = _ui->nzbFileEdit->text();
    return nzb.isEmpty() ? QString() : QFileInfo(nzb).absolutePath();
}

void PostingWidget::setNzbFolder(const QString &folder)
{
    QString const nzb = _ui->nzbFileEdit->text();
    if (!nzb.isEmpty())
        _ui->nzbFileEdit->setText(QDir(folder).filePath(QFileInfo(nzb).fileName()));
}

bool PostingWidget::_isSubmitted() const
{
    return _postingJob || _state != STATE::IDLE;
}

QString PostingWidget::_archivePassword() const
{
    if (!_ui->compressCB->isChecked() || !_ui->nzbPassCB->isChecked())
        return QString();
    return _ui->nzbPassEdit->text();
}

bool PostingWidget::acceptsFiles() const
{
    // The job took a copy of the list when it was submitted: a file added
    // afterwards would never be posted, yet be shown as part of the post.
    return !_isSubmitted() && (!_postingFinished || _resubmittable);
}

void PostingWidget::_refreshFileControls()
{
    bool const accepts = acceptsFiles();
    _ui->selectFilesButton->setEnabled(accepts);
    _ui->selectFolderButton->setEnabled(accepts);
    _ui->filesList->setEditable(accepts);
    // A finished tab is emptied to serve again: the default one cannot close.
    _ui->clearFilesButton->setEnabled(!_isSubmitted());
    _refreshPostButton();
}

void PostingWidget::_refreshPostButton()
{
    // Submitted, the button stops the post. Idle, it starts one only where files
    // are taken: a post that ran to its end is not posted again from this tab.
    bool const usable = _isSubmitted() ? !(_postingJob && _postingJob->cancelRequested())
                                       : acceptsFiles();
    _ui->postButton->setEnabled(usable && !_ngPost->_cancelingAll);
}

bool PostingWidget::isBlank() const
{
    return _state == STATE::IDLE && !_postingJob && !_postingFinished
        && _ui->filesList->count() == 0;
}

void PostingWidget::_armResubmission(const PostingJob *job)
{
    // A fresh submission is safe before any transfer started, and after a stop:
    // the post then starts over from scratch. A history resume keeps its resume
    // semantics; the History tab is where it is resumed again.
    bool const stopped = job->cancelRequested();
    _resubmittable = !job->isResumeFromHistory() && (stopped || !job->startedAtWall().isValid());
    if (!_resubmittable)
        return;
    if (stopped) {
        _stoppedAttempt = { job->historyPostId(),
                            QFileInfo(job->nzbFilePath()).absoluteFilePath(),
                            job->archiveFolder() };
        // Ready to post again, not done: no OK/KO mark for what was cut short.
        _hmi->clearJobTab(this);
    }
    if (job->inputPaths().isEmpty())
        return;
    // Packing may have replaced the list with temporary archives. Restore the
    // user's sources without regenerating the NZB name, password or metadata.
    _ui->filesList->clear2();
    for (const QString &path : job->inputPaths())
        _ui->filesList->addPath(path, QFileInfo(path).isDir());
}

void PostingWidget::_supersedeStoppedAttempt()
{
    // Posting a stopped post again from scratch replaces it. Its archives were
    // kept for a resume and would make the new compression refuse the same
    // folder; they are ngPost's own output, created by that job alone, and the
    // sources they came from are untouched. Its history row is closed, since
    // the NZB it may have written is about to be overwritten.
    bool const replaceable = _stoppedAttemptIsReplaceable();
    StoppedAttempt const stopped = std::exchange(_stoppedAttempt, {});
    if (!replaceable)
        return;
    if (!stopped.archiveFolder.isEmpty() && QFileInfo::exists(stopped.archiveFolder)
        && !QDir(stopped.archiveFolder).removeRecursively())
        _hmi->logError(
            tr("Could not remove the archives of the stopped post: %1").arg(stopped.archiveFolder));
    if (stopped.historyPostId && _ngPost->historyService())
        _ngPost->historyService()->setPostAbandoned(stopped.historyPostId);
}

QFileInfoList PostingWidget::previewFiles() const
{
    QFileInfoList files;
    for (int i = 0; i < _ui->filesList->count(); ++i)
        files << QFileInfo(_ui->filesList->item(i)->text());
    return files;
}

void PostingWidget::onPostFiles()
{
    postFiles(true);
}

void PostingWidget::postFiles(bool updateMainParams)
{
    if (_state == STATE::IDLE)
    {
        // Greyed button or not, a post that ran to its end is not posted again.
        if (!acceptsFiles())
            return;
        if (_ui->filesList->count() == 0)
        {
            _hmi->logError(tr("There are no selected files to post..."));
            return;
        }

        QFileInfoList files;
        bool hasFolder = false;
        _buildFilesList(files, hasFolder);
        if (files.isEmpty())
        {
            _hmi->logError(tr("There are no existing files to post..."));
            return;
        }

        if (hasFolder && !_ui->compressCB->isChecked())
        {
            _hmi->logError(tr("You can't post folders without using compression..."));
            return;
        }


        if (updateMainParams)
        {
            _hmi->updateServers();
            _hmi->updateParams();
        }
        udatePostingParams();
        // A confirmation runs a nested event loop; an Auto Posting event may
        // update the shared globals while it is open. Keep this tab's values.
        PostingJobOptions options = _ngPost->_baseJobOptions();

        // check if the nzb file name already exist
        QString nzbPath = nzbFolder().isEmpty() ? _ngPost->nzbPath()
                                                : QDir(nzbFolder()).filePath(_ngPost->_nzbName);
        if (!nzbPath.endsWith(".nzb"))
            nzbPath += ".nzb";
        if (!_confirmNzbOverwrite(nzbPath))
            return;
        // Before the job exists: an active one may compress at once.
        _supersedeStoppedAttempt();

        _postingFinished = false;
        _resubmittable = false;
        _state = STATE::POSTING;
        emit submissionEligibilityChanged();
        _fillJobOptions(options, nzbPath, files);

        _postingJob = new PostingJob(_ngPost, options, this);

        bool hasStarted = _ngPost->startPostingJob(_postingJob);
        if (_ngPost->lastPostingStartCanceled())
        {
            _postingJob = nullptr;
            _postingFinished = false;
            setIDLE();
            return;
        }

        _ui->postButton->setText(hasStarted ? tr("Stop Posting") : tr("Cancel Posting"));
        _hmi->updateJobTab(this,
                           hasStarted ? _hmi->sPostingColor : _hmi->pendingColor(),
                           hasStarted ? QIcon(_hmi->sPostingIcon) : _hmi->pendingIcon(),
                           _postingJob->nzbName());
    }
    else  if (_state == STATE::POSTING)
    {
        _state = STATE::STOPPING;
        emit _postingJob->stopPosting();
        emit submissionEligibilityChanged();
    }
}


void PostingWidget::_fillJobOptions(PostingJobOptions &options,
                                    const QString &nzbPath,
                                    const QFileInfoList &files) const
{
    options.nzbFilePath = nzbPath;
    options.files = files;
    // in the GUI the list holds exactly what the user dropped, folders included
    options.inputPaths.reserve(files.size());
    for (QFileInfo const &file : files)
        options.inputPaths << file.absoluteFilePath();
    // the GUI already asked about overwriting, and never deletes the sources
    options.overwriteNzb = true;
    options.delFilesAfterPost = false;
    options.doCompress = _ui->compressCB->isChecked();
    options.doPar2 = _ui->par2CB->isChecked();
    options.rarName = _ui->compressNameEdit->text();
    options.rarPass = _archivePassword();
    options.passwordGenerated = !options.rarPass.isEmpty() && options.rarPass == _drawnPassword;
    options.lengthName = static_cast<uint>(_ui->nameLengthSB->value());
    int const redundancy = _ui->redundancySB->value();
    options.par2Pct = redundancy < 0 ? _ngPost->_par2PctDefault : static_cast<uint>(redundancy);
    options.keepRar = _ui->keepRarCB->isChecked();
    // per post, deliberately not copied into the NgPost globals
    // The fields belong to the post info feature as a whole: with the box
    // unticked there is no sheet, and nothing to publish in the nzb either.
    // Leaving them in would publish through a box the user just turned off.
    options.writePostInfoFile = writesPostInfoFile();
    if (options.writePostInfoFile) {
        options.meta = _postInfoMeta;
        options.postInfoTemplate = _postInfoTemplate;
        options.postInfoOutput = _postInfoOutput;
    }
}

bool PostingWidget::_stoppedAttemptIsReplaceable() const
{
    // Only while it is still the post this tab stopped. The History tab may
    // have resumed it since, or be resuming it from these very archives: its
    // NZB, archives and history row then belong to that resume.
    qint64 const id = _stoppedAttempt.historyPostId;
    if (!id)
        return true;
    QList<PostingJob *> jobs(_ngPost->_pendingJobs.cbegin(), _ngPost->_pendingJobs.cend());
    jobs << _ngPost->_activeJob;
    for (PostingJob const *job : jobs)
        if (job && job->historyPostId() == id)
            return false;
    PostHistoryService::ResumeRow row;
    if (auto *history = _ngPost->historyService())
        history->checkResume(id, &row); // fills the row even when not resumable
    return row.status != QLatin1String("success") && row.status != QLatin1String("posting");
}

bool PostingWidget::_isStoppedAttemptNzb(const QString &nzbPath) const
{
    // Written by the stopped post of this very tab, or already there when the
    // user agreed to replace it for that post: asking again is noise.
    return !_stoppedAttempt.nzbPath.isEmpty()
        && QFileInfo(nzbPath).absoluteFilePath() == _stoppedAttempt.nzbPath
        && _stoppedAttemptIsReplaceable();
}

bool PostingWidget::_confirmNzbOverwrite(const QString &nzbPath)
{
    const quint64 generation = _ngPost->_postingCancelGeneration;
    if (QFileInfo::exists(nzbPath) && !_isStoppedAttemptNzb(nzbPath)
        && QMessageBox::question(
               nullptr,
               tr("Overwrite existing nzb file?"),
               tr("The nzb file '%1' already exists.\nWould you like to overwrite it ?")
                   .arg(nzbPath),
               QMessageBox::Yes,
               QMessageBox::No)
            != QMessageBox::Yes)
        return false;
    return generation == _ngPost->_postingCancelGeneration;
}

void PostingWidget::onNzbPassToggled(bool checked)
{
    // Two switches command these: the password box itself, and the compression
    // of this post above it. The greyed tooltip names whichever is missing.
    bool const compress = _ui->compressCB->isChecked();
    bool const on       = checked && compress;
    QString const needs = compress ? tr("requires: %1").arg(_ui->nzbPassCB->text())
                                   : tr("requires: %1").arg(_ui->compressCB->text());

    setDependentEnabled(_ui->nzbPassEdit, on,
                        tr("password used in your archive that would also be added in the header of the nzb file"),
                        needs);
    setDependentEnabled(_ui->passLengthSB, on,
                        tr("length of the password drawn by the dice button (config LENGTH_PASS)"), needs);
    setDependentEnabled(_ui->genPass, on, tr("generate random password"), needs);
}

void PostingWidget::onGenNzbPassword()
{
    _drawnPassword = _ngPost->randomPass(static_cast<uint>(_ui->passLengthSB->value()));
    _ui->nzbPassEdit->setText(_drawnPassword);
}

void PostingWidget::onSelectFilesClicked()
{
    // The right click on the list lands here too, button greyed or not.
    if (!acceptsFiles())
        return;
    const auto shutdownHold = _ngPost->holdShutdown();
    QStringList files = QFileDialog::getOpenFileNames(
                this,
                tr("Select one or more files to Post"),
                _ngPost->_inputDir);

    int currentNbFiles = _ui->filesList->count();
    for (const QString &file : files)
        addPath(file, currentNbFiles);
}

void PostingWidget::onSelectFolderClicked()
{
    const auto shutdownHold = _ngPost->holdShutdown();
    QString folder = QFileDialog::getExistingDirectory(
                this,
                tr("Select a Folder"),
                _ngPost->_inputDir,
                QFileDialog::ShowDirsOnly);

    if (!folder.isEmpty())
        addPath(folder, _ui->filesList->count(), true);
}

void PostingWidget::onClearFilesClicked()
{
    // Also reached when the list is emptied: during a post this would wipe its
    // NZB name, its post information and its tab mark.
    if (_isSubmitted())
        return;
    _postingFinished = false;
    _ui->filesList->clear2();
    // The post information describes the post that was there; leaving it would
    // hand the title of one post to the next one queued in this tab.
    _postInfoMeta.clear();
    _postInfoTemplate.clear();
    _postInfoOutput.clear();
    _ui->nzbFileEdit->clear();
    _ui->compressNameEdit->clear();
    // A new post: the stopped one stays resumable from the History tab.
    _stoppedAttempt = {};
    if (_hmi->hasAutoCompress())
    {
        // Drawn again for it, as PACK asks. NgPost's own flags would not do:
        // every post writes its choices into them (udatePostingParams).
        PackingChoice const pack = packingChoice(true, _ngPost->_packAutoKeywords);
        if (pack.genName)
            onGenCompressName();
        _renewArchivePassword(pack.genPass);
    }

    _hmi->clearJobTab(this);
}

void PostingWidget::_renewArchivePassword(bool genPass)
{
    // The default password of the configuration is never replaced by a draw.
    if (_hmi->useFixedPassword())
        _ui->nzbPassEdit->setText(_ngPost->_rarPassFixed);
    else if (genPass)
        onGenNzbPassword();
}

void PostingWidget::_restoreConfiguredOptions()
{
    // What the last post changed goes back to the configuration, as when the
    // tab opens. The packing switches come from the PACK keywords, for the
    // reason above; a default password goes with every compressed post.
    loadObfuscationLengths(_ui->nameLengthSB, _ui->passLengthSB, _ngPost);
    _ui->keepRarCB->setChecked(_ngPost->_keepRarDefault);
    _ui->redundancySB->setValue(-1);
    PackingChoice const pack = packingChoice(_hmi->hasAutoCompress(), _ngPost->_packAutoKeywords);
    _ui->compressCB->setChecked(pack.compress);
    _ui->par2CB->setChecked(pack.doPar2);
    _ui->nzbPassCB->setChecked(pack.genPass || _hmi->useFixedPassword());
    // The last post's password stays with it: the emptying draws a new one.
    _ui->nzbPassEdit->setText(_hmi->fixedArchivePassword());
    _postInfoCB->setChecked(!_ngPost->postInfoTemplatePath().isEmpty());
}

void PostingWidget::resetForNextPost()
{
    // Auto close reaches us through NgPost's end of job slot, connected to
    // PostingJob::postingFinished before ours and so served first: the job is
    // over, but this tab has not heard it yet. Closing it there simply deletes
    // it; emptying it has to end the job here first. onPostingJobDone is
    // already written to be called twice, so the queued one that follows finds
    // nothing left to do.
    if (_postingJob)
        onPostingJobDone();

    // Anything else than an idle tab is not ours to empty: what is listed
    // belongs to the post using it.
    if (_state != STATE::IDLE)
        return;

    // The configuration first, so that the same emptying as the Clear button,
    // decorations included, draws the next archive name and password with it.
    _restoreConfiguredOptions();
    onClearFilesClicked();
    _postingFinished = false;
}

void PostingWidget::onCompressCB(bool checked)
{
    // The archive settings only mean something while ngPost is the one building
    // the archive, so they follow the box. Each greyed control now says what it
    // is waiting for: a greyed box with no explanation was reported as a broken
    // one. PAR2 is not among them: it can protect the files as they are
    // (Request #102).
    QString const needsCompress = tr("requires: %1").arg(_ui->compressCB->text());

    setDependentEnabled(_ui->compressNameEdit, checked,
                        tr("archive name (file name obfuscation)"), needsCompress);
    setDependentEnabled(_ui->nameLengthSB, checked,
                        tr("length of the archive name drawn by the dice button (config LENGTH_NAME)"),
                        needsCompress);
    setDependentEnabled(_ui->genCompressName,
                        checked,
                        tr("generate random archive name"),
                        needsCompress);
    setDependentEnabled(_ui->nzbPassCB,
                        checked,
                        tr("This should be the password of the archive you're posting"),
                        needsCompress);

    // The password field has a switch of its own on top of this one, so it
    // answers to both. Keep Archives answers to this box or to PAR2.
    onNzbPassToggled(_ui->nzbPassCB->isChecked());
    onPar2CB(_ui->par2CB->isChecked());
}

void PostingWidget::onPar2CB(bool checked)
{
    setDependentEnabled(
        _ui->redundancySB,
        checked,
        tr("Using the PAR2 arguments of the configuration: %1").arg(_ngPost->par2ArgsInUse()),
        tr("requires: %1").arg(_ui->par2CB->text()));

    showKeepArchivesRule(_ui->keepRarCB, _ui->compressCB, _ui->par2CB);
}

void PostingWidget::showKeepArchivesRule(QCheckBox *keepRar,
                                         const QCheckBox *compress,
                                         const QCheckBox *par2)
{
    // Archives or PAR2 volumes alone: whatever a post generates is deleted after
    // it unless kept.
    setDependentEnabled(keepRar,
                        compress->isChecked() || par2->isChecked(),
                        tr("by default archives and par2 files are deleted uppon post success but "
                           "you can choose to keep them"),
                        tr("requires: %1 or %2").arg(compress->text(), par2->text()));
}

void PostingWidget::onGenCompressName()
{
    _ui->compressNameEdit->setText(_ngPost->randomName(static_cast<uint>(_ui->nameLengthSB->value())));
}

void PostingWidget::onNzbFileClicked()
{
    QString path = QFileDialog::getSaveFileName(
                this,
                tr("Create nzb file"),
                _ngPost->_nzbPath,
                "*.nzb"
                );

    if (!path.isEmpty())
        _ui->nzbFileEdit->setText(path);
}

void PostingWidget::handleKeyEvent(QKeyEvent *keyEvent)
{
    qDebug() << "[PostingWidget::handleKeyEvent] key event: " << keyEvent->key();
    // Delete and paste are the only keys handled here.
    if (!acceptsFiles())
        return;

    if(keyEvent->key() == Qt::Key_Delete || keyEvent->key() == Qt::Key_Backspace)
    {
        for (QListWidgetItem *item : _ui->filesList->selectedItems())
        {
            qDebug() << "[PostingWidget::handleKeyEvent] remove item: " << item->text();
            _ui->filesList->removeItemWidget2(item);
            delete item;
        }
    }
    else if (keyEvent->matches(QKeySequence::Paste))
    {
        const QClipboard *clipboard = QApplication::clipboard();
        const QMimeData *mimeData = clipboard->mimeData();
        if (mimeData->hasImage()) {
            qDebug() << "[PostingWidget::handleKeyEvent] try to paste image...";
        } else if (mimeData->hasHtml()) {
            qDebug() << "[PostingWidget::handleKeyEvent] try to paste html: ";
        } else if (mimeData->hasText()) {
            QString txt = mimeData->text();
            qDebug() << "[PostingWidget::handleKeyEvent] paste text: " << txt;
            int currentNbFiles = _ui->filesList->count();
            for (const QString &path : txt.split(QRegularExpression("\n|\r|\r\n")))
            {
                QFileInfo fileInfo(path);
                if (!fileInfo.exists())
                    qDebug() << "[PostingWidget::handleKeyEvent] NOT A FILE: " << path;
                else
                    addPath(path, currentNbFiles, fileInfo.isDir());
//                        else if (fileInfo.isDir())
//                        {
//                            QDir dir(fileInfo.absoluteFilePath());
//                            for (const QFileInfo &subFile : dir.entryInfoList(QDir::Files, QDir::Name))
//                            {
//                                if (subFile.isReadable())
//                                    _addFile(subFile.absoluteFilePath(), currentNbFiles);
//                            }
//                        }
            }

        } else if (mimeData->hasUrls()) {
            qDebug() << "[PostingWidget::handleKeyEvent] paste urls...";

        } else {
            qDebug() << "[PostingWidget::handleKeyEvent] unknown type...";
        }
    }

}


void PostingWidget::handleDropEvent(QDropEvent *e)
{
    int currentNbFiles = _ui->filesList->count();
    for (const QUrl &url : e->mimeData()->urls())
    {
        QString fileName = url.toLocalFile();
        addPath(fileName, currentNbFiles, QFileInfo(fileName).isDir());
    }
}

void PostingWidget::_buildFilesList(QFileInfoList &files, bool &hasFolder)
{
    for (int i = 0 ; i < _ui->filesList->count() ; ++i)
    {
        QFileInfo fileInfo(_ui->filesList->item(i)->text());
        if (fileInfo.exists())
        {
            files << fileInfo;
            if (fileInfo.isDir())
                hasFolder = true;
        }
    }
}

void PostingWidget::init()
{
    _ui->nzbPassCB->setChecked(false);
    onNzbPassToggled(false);

    _ui->keepRarCB->setChecked(_ngPost->_keepRarDefault);

    _ui->redundancySB->setRange(-1, 100);
    _ui->redundancySB->setValue(-1);
    refreshPar2Default();
    connect(_ngPost, &NgPost::par2DefaultsChanged, this, &PostingWidget::refreshPar2Default, Qt::UniqueConnection);
    // The suffix travels inside the spin box, so the number stops being an
    // unlabelled one without costing a widget and its layout spacing.
    _ui->redundancySB->setSuffix(QStringLiteral(" %"));

    if (!_ngPost->_rarPassFixed.isEmpty())
    {
        _ui->nzbPassCB->setChecked(true);
        _ui->nzbPassEdit->setText(_ngPost->_rarPassFixed);
    }

    loadObfuscationLengths(_ui->nameLengthSB, _ui->passLengthSB, _ngPost);

    _ui->copyNfoWithNzbCB->setChecked(_ngPost->_copyNfoWithNzb);

    _ui->filesList->setSelectionMode(QAbstractItemView::ExtendedSelection);

    connect(_ui->selectFilesButton, &QAbstractButton::clicked, this, &PostingWidget::onSelectFilesClicked);
    connect(_ui->selectFolderButton,&QAbstractButton::clicked, this, &PostingWidget::onSelectFolderClicked);
    connect(_ui->clearFilesButton,  &QAbstractButton::clicked, this, &PostingWidget::onClearFilesClicked);
    connect(_ui->filesList,         &SignedListWidget::empty,  this, &PostingWidget::onClearFilesClicked, Qt::QueuedConnection);

    connect(_ui->compressCB,        &QAbstractButton::toggled, this, &PostingWidget::onCompressCB);
    connect(_ui->genCompressName,   &QAbstractButton::clicked, this, &PostingWidget::onGenCompressName);

    connect(_ui->nzbFileButton,     &QAbstractButton::clicked, this, &PostingWidget::onNzbFileClicked);


    // A new post starts from the defaults the user configured, as Remove All
    // does, never from what the last post or the Auto Posting tab left behind.
    PackingChoice const pack = packingChoice(_hmi->hasAutoCompress(), _ngPost->_packAutoKeywords);
    onCompressCB(pack.compress);
    onPar2CB(_ui->par2CB->isChecked());
    _ui->compressCB->setChecked(pack.compress);
    if (pack.genName)
        onGenCompressName();
    if (pack.genPass)
        _ui->nzbPassCB->setChecked(true);
    _renewArchivePassword(pack.genPass);
    _ui->par2CB->setChecked(pack.doPar2);
}

void PostingWidget::applyAutoPacking()
{
    NgPost::AutoPacking const &packing = _ngPost->_autoPacking;
    _ui->compressCB->setChecked(packing.compress);
    _ui->par2CB->setChecked(packing.doPar2);
    _ui->keepRarCB->setChecked(packing.keepRar);
    _ui->nameLengthSB->setValue(static_cast<int>(packing.lengthName));
    _ui->passLengthSB->setValue(static_cast<int>(packing.lengthPass));
    setPar2PercentageOverride(packing.par2Pct);
    _ui->compressNameEdit->clear();
    if (packing.compress && packing.genName)
        onGenCompressName();
    QString const pass = packing.compress ? _ngPost->autoArchivePassword() : QString();
    setNzbPassword(pass);
    if (_ngPost->autoPassChoice() == NgPost::AutoPass::Random)
        _drawnPassword = pass;
}



void PostingWidget::udatePostingParams()
{
    // Only the name: the folder of this post's nzb is not the configured one.
    // Written into NgPost it would be saved as NZB_PATH, and taken by the
    // other tabs and the Auto Posting.
    if (!_ui->nzbFileEdit->text().isEmpty())
        _ngPost->setNzbName(QFileInfo(_ui->nzbFileEdit->text()));

    // The packing of this post is not written here any more: it goes with its
    // job options (_fillJobOptions). Written into NgPost, it became what the
    // next new tab started with, what the Auto Posting used, and PACK on disk.

    _ngPost->_copyNfoWithNzb = _ui->copyNfoWithNzbCB->isChecked();
}

void PostingWidget::_buildPostInfoRow()
{
    _postInfoCB = new QCheckBox(this);
    _postInfoCB->setObjectName(QStringLiteral("postInfoCB"));
    // On by default when a model is configured: the user asked for sheets once,
    // in the configuration, and should not have to ask again for every post.
    _postInfoCB->setChecked(!_ngPost->postInfoTemplatePath().isEmpty());

    _postInfoButton = new QPushButton(this);
    _postInfoButton->setObjectName(QStringLiteral("postInfoButton"));
    _postInfoButton->setEnabled(_postInfoCB->isChecked());

    connect(_postInfoCB, &QCheckBox::toggled, this, &PostingWidget::onPostInfoToggled);
    connect(_postInfoButton, &QAbstractButton::clicked, this, &PostingWidget::onEditPostInfo);

    // The sheet is one of the things this post produces, like the nzb and the
    // copied nfo, so it belongs on that line rather than on a row of its own
    // under everything else.
    QFrame *sep = new QFrame(this);
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Sunken);
    _ui->horizontalLayout_9->addWidget(sep);
    _ui->horizontalLayout_9->addWidget(_postInfoCB);
    _ui->horizontalLayout_9->addWidget(_postInfoButton);

    // No trailing stretch: the spare width goes to the nzb path, which is the
    // only field on the line long enough to need it. A stretch here would eat
    // it instead and leave the path showing "...do.nzb".
    _ui->horizontalLayout_9->setStretchFactor(_ui->nzbFileLayout, 1);

    retranslatePostInfoTexts();
}

void PostingWidget::retranslatePostInfoTexts()
{
    if (!_postInfoCB)
        return;
    _postInfoCB->setText(tr("Create a post info file"));
    _postInfoCB->setToolTip(tr("Write a small text file describing this post next to the nzb.\n"
                               "Some Usenet indexes ask for one."));
    _postInfoButton->setText(tr("Post information\342\200\246"));
    _postInfoButton->setToolTip(tr("Choose the model and fill in your own fields, for this post."));
}

void PostingWidget::onPostInfoToggled(bool checked)
{
    _postInfoButton->setEnabled(checked);
}

void PostingWidget::onEditPostInfo()
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
        // Same rule as --meta on the command line: refuse rather than keep one
        // of the two values without saying which.
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
        // Asked to become what is offered from now on: model and destination
        // go in the configuration and stop being overrides for this post.
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

//! What the sheet already knows while the post is only being prepared. Read
//! straight from the tab rather than through udatePostingParams(), which
//! writes into the NgPost globals: previewing a layout must change nothing.
PostInfoData PostingWidget::_postInfoPreview() const
{
    PostInfoData postData;
    postData.appVersion = QString(APP_VERSION);

    if (!_ui->nzbFileEdit->text().isEmpty())
    {
        QFileInfo const nzb(_ui->nzbFileEdit->text());
        postData.nzbPath = nzb.absoluteFilePath();
        postData.nzbDir = nzb.absolutePath();
        postData.nzbName = nzb.completeBaseName();
        postData.nzbFileName = nzb.fileName();
    }

    postData.rarName = _ui->compressNameEdit->text();
    postData.rarPass = _archivePassword();

    postData.groups = _ngPost->groups();
    // Left empty when ngPost draws a poster at random: showing one sample
    // would name an address the post is not going to use.
    if (!_ngPost->_genFrom && !_ngPost->_from.empty())
        postData.nzbPoster = QString::fromStdString(_ngPost->_from);

    postData.par2Pct = _ui->par2CB->isChecked()
        ? (_ui->redundancySB->value() < 0 ? int(_ngPost->_par2PctDefault)
                                          : _ui->redundancySB->value())
        : -1;

    QFileInfoList files;
    bool          hasFolder = false;
    const_cast<PostingWidget *>(this)->_buildFilesList(files, hasFolder);
    if (!files.isEmpty())
    {
        postData.sourcePath = files.first().absoluteFilePath();
        postData.originalName = files.first().fileName();
        postData.originalPath = files.first().absolutePath();
    }
    return postData;
}

void PostingWidget::setPostInfo(bool enabled,
                                const QString &templateOverride,
                                const QString &outputOverride,
                                const QMap<QString, MetaValue> &meta)
{
    if (_postInfoCB)
        _postInfoCB->setChecked(enabled);
    _postInfoTemplate = templateOverride;
    _postInfoOutput   = outputOverride;
    _postInfoMeta     = meta;
}

bool PostingWidget::writesPostInfoFile() const
{
    return _postInfoCB && _postInfoCB->isChecked();
}

void PostingWidget::retranslate()
{
    _ui->retranslateUi(this);
    refreshPostingState();
    refreshPar2Default();
    retranslatePostInfoTexts();
    _copyCompressNameAction->setText(tr("Copy archive name to clipboard"));
    _copyPassAction->setText(tr("Copy password to clipboard"));
    onCompressCB(_ui->compressCB->isChecked());
    onPar2CB(_ui->par2CB->isChecked());
    _ui->filesList->setToolTip(
        QString("%1<ul><li>%2</li><li>%3</li><li>%4</li></ul>%5")
            .arg(tr("You can add files or folder by:"),
                 tr("Drag & Drop files/folders"),
                 tr("Right Click to add Files"),
                 tr("Click on Select Files/Folder buttons"),
                 tr("Bare in mind you can select items in the list and press DEL to remove them")));
}

void PostingWidget::setNzbPassword(const QString &pass)
{
    // No password is an unticked box: ticked over an empty field would be a lie.
    _ui->nzbPassCB->setChecked(!pass.isEmpty());
    _ui->nzbPassEdit->setText(pass);
}

void PostingWidget::setPackingAuto(bool enabled, const QStringList &keys)
{
    PackingChoice const pack = packingChoice(enabled, keys);
    _ui->nzbPassCB->setChecked(pack.genPass);
    _ui->compressCB->setChecked(pack.compress);
    _ui->par2CB->setChecked(pack.doPar2);


    if (pack.compress) {
        if (_ui->nzbPassEdit->text().isEmpty())
            _renewArchivePassword(pack.genPass);
        if (pack.genName && _ui->compressNameEdit->text().isEmpty())
            onGenCompressName();
    }
}

void PostingWidget::addPath(const QString &path, int currentNbFiles, int isDir)
{
    // Every way of adding comes here: drop, paste, file and folder pickers.
    if (!acceptsFiles())
        return;
    if (_ui->filesList->addPathIfNotInList(path, currentNbFiles, isDir))
    {
        _postingFinished = false;
        emit submissionEligibilityChanged();
        QFileInfo fileInfo(path);
        if (_ui->nzbFileEdit->text().isEmpty())
        {
            _ngPost->setNzbName(fileInfo);
            _ui->nzbFileEdit->setText(QString("%1.nzb").arg(_ngPost->nzbPath()));
        }
        if (_ui->compressNameEdit->text().isEmpty())
            _ui->compressNameEdit->setText(_ngPost->_nzbName);
    }
}

bool PostingWidget::_fileAlreadyInList(const QString &fileName, int currentNbFiles) const
{
    for (int i = 0 ; i < currentNbFiles ; ++i)
    {
        if (_ui->filesList->item(i)->text() == fileName)
            return true;
    }
    return false;
}

QColor PostingWidget::postingTextColor() const
{
    const bool submitted = _postingJob && !_postingFinished;
    const bool stopping = submitted && _postingJob->cancelRequested();
    const bool active = submitted && _ngPost->_activeJob == _postingJob;
    const bool paused = submitted && !stopping
        && (_ngPost->_queuePaused || _postingJob->isPaused());
    const QPalette theme = _hmi->palette();
    const bool dark = theme.color(QPalette::Window).lightness() < 128;
    if (paused)
        return dark ? QColor(Qt::yellow) : QColor(160, 110, 0);
    if (active && !stopping)
        return dark ? QColor(0x4c, 0xff, 0x4c) : QColor(Qt::darkGreen);
    return dark ? QColor(Qt::white) : theme.color(QPalette::WindowText);
}

void PostingWidget::refreshPostingState()
{
    const bool submitted = _postingJob && !_postingFinished;
    const bool active = submitted && _ngPost->_activeJob == _postingJob;
    _ui->postButton->setText(!submitted   ? tr("Start Quick Post #%1").arg(displayNumber())
                                 : active ? tr("Stop Posting")
                                          : tr("Cancel Posting"));
    _refreshPostButton();
    const QIcon icon = (submitted && !active) ? _hmi->pendingIcon() : QIcon();
    _hmi->updateJobTab(this, postingTextColor(), icon);
}

void PostingWidget::setIDLE()
{
    _ui->postButton->setText(tr("Start Quick Post #%1").arg(displayNumber()));
    _state = STATE::IDLE;
    emit submissionEligibilityChanged();
}

void PostingWidget::setPosting()
{
    _activatePosting();
}

void PostingWidget::_activatePosting()
{
    const QString nzbName = _postingJob ? _postingJob->nzbName() : QString();
    _hmi->updateJobTab(this, _hmi->sPostingColor, QIcon(_hmi->sPostingIcon), nzbName);
    _ui->postButton->setText(tr("Stop Posting"));
    _state = STATE::POSTING;
    _highlightFirstPendingFile();
    emit submissionEligibilityChanged();
}

void PostingWidget::attachResumeJob(PostingJob *job, const QFileInfoList &files, bool hasStarted)
{
    _attachResumeJob(job, files, hasStarted);
}

void PostingWidget::_attachResumeJob(PostingJob *job, const QFileInfoList &files, bool hasStarted)
{
    if (!job)
        return;

    _ui->filesList->clear2();
    for (const QFileInfo &file : files)
        _ui->filesList->addPath(file.absoluteFilePath(), file.isDir());

    _postingJob = job;
    _postingFinished = false;
    _state = STATE::POSTING;
    _ui->nzbFileEdit->setText(job->nzbFilePath());

    _ui->postButton->setText(hasStarted ? tr("Stop Posting") : tr("Cancel Posting"));
    _hmi->updateJobTab(this,
                       hasStarted ? _hmi->sPostingColor : _hmi->pendingColor(),
                       hasStarted ? QIcon(_hmi->sPostingIcon) : _hmi->pendingIcon(),
                       job->nzbName());
    if (hasStarted)
        _highlightFirstPendingFile();
    emit submissionEligibilityChanged();
}

bool PostingWidget::_highlightFirstPendingFile()
{
    int const nbFiles = _ui->filesList->count();
    for (int i = qMax(0, _ui->filesList->followedRow()); i < nbFiles; ++i) {
        QListWidgetItem *item = _ui->filesList->item(i);
        if (item && !item->data(kFilePostedRole).toBool()) {
            item->setForeground(_hmi->sPostingColor);
            _ui->filesList->followItem(item);
            return true;
        }
    }
    return false;
}

void PostingWidget::_clearActivePostingHighlights()
{
    _ui->filesList->followItem(nullptr);
    int const nbFiles = _ui->filesList->count();
    const QColor normalColor = postingTextColor();
    for (int i = 0; i < nbFiles; ++i) {
        QListWidgetItem *item = _ui->filesList->item(i);
        if (item && item->foreground().color() == _hmi->sPostingColor)
            item->setForeground(normalColor);
    }
}


void PostingWidget::showPar2Default(QSpinBox *box, uint percentage)
{
    box->setSpecialValueText(tr("Global (%1 %)").arg(percentage));
    box->setMaximumWidth(QWIDGETSIZE_MAX);
    box->setMinimumWidth(box->sizeHint().width());
}

void PostingWidget::loadObfuscationLengths(QSpinBox *nameLength,
                                           QSpinBox *passLength,
                                           const NgPost *ngPost)
{
    nameLength->setRange(5, 50);
    nameLength->setValue(static_cast<int>(ngPost->_lengthNameDefault));
    passLength->setRange(5, 50);
    passLength->setValue(static_cast<int>(ngPost->_lengthPassDefault));
}

void PostingWidget::refreshPar2Default()
{
    showPar2Default(_ui->redundancySB, _ngPost->par2DefaultPercentage());
    // The tooltip of the redundancy names the arguments a post runs with.
    onPar2CB(_ui->par2CB->isChecked());
}

void PostingWidget::setPar2PercentageOverride(int percentage)
{
    _ui->redundancySB->setValue(percentage);
}
bool PostingWidget::hasPar2PercentageOverride() const
{
    return _ui->redundancySB->value() >= 0;
}

static const char *sActionLineEditStyle =
    "QLineEdit QToolButton { border: none; background: transparent; padding: 0px; margin: 0px; }"
    "QLineEdit QToolButton:hover { border: none; background: rgba(128, 128, 128, 40); "
    "border-radius: 3px; }"
    "QLineEdit QToolButton:pressed { border: none; background: rgba(128, 128, 128, 80); "
    "border-radius: 3px; }";

void PostingWidget::_setupCopyActions()
{
    _ui->compressNameEdit->setStyleSheet(sActionLineEditStyle);
    _ui->nzbPassEdit->setStyleSheet(sActionLineEditStyle);

    const QIcon copyIcon(QStringLiteral(":/icons/copy.png"));

    _copyCompressNameAction = _ui->compressNameEdit->addAction(copyIcon,
                                                               QLineEdit::TrailingPosition);
    _copyCompressNameAction->setText(tr("Copy archive name to clipboard"));

    _copyPassAction = _ui->nzbPassEdit->addAction(copyIcon, QLineEdit::TrailingPosition);
    _copyPassAction->setText(tr("Copy password to clipboard"));

    for (auto *action : { _copyCompressNameAction, _copyPassAction }) {
        auto *resetTimer = new QTimer(action);
        resetTimer->setSingleShot(true);
        connect(resetTimer, &QTimer::timeout, action, [action, copyIcon] {
            action->setIcon(copyIcon);
        });
    }

    for (auto *tb : _ui->compressNameEdit->findChildren<QToolButton *>()) {
        tb->setCursor(Qt::PointingHandCursor);
        tb->setAutoRaise(true);
    }
    for (auto *tb : _ui->nzbPassEdit->findChildren<QToolButton *>()) {
        tb->setCursor(Qt::PointingHandCursor);
        tb->setAutoRaise(true);
    }

    connect(_copyCompressNameAction, &QAction::triggered, this, [this]() {
        _copyToClipboard(_copyCompressNameAction, _ui->compressNameEdit);
    });
    connect(_copyPassAction, &QAction::triggered, this, [this]() {
        _copyToClipboard(_copyPassAction, _ui->nzbPassEdit);
    });
}

void PostingWidget::_copyToClipboard(QAction *action, QLineEdit *edit)
{
    if (!edit || !action)
        return;

    const QString text = edit->text();
    if (text.isEmpty()) {
        QToolTip::showText(QCursor::pos(), tr("Nothing to copy"), edit, {}, 1500);
        return;
    }

    QApplication::clipboard()->setText(text);

    QToolTip::showText(QCursor::pos(), tr("Copied!"), edit, {}, 1500);

    action->setIcon(QIcon(QStringLiteral(":/icons/ok.png")));
    // Restart the action's feedback timer; repeated clicks always restore the copy icon.
    action->findChild<QTimer *>()->start(1200);
}
