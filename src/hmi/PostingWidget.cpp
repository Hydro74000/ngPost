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
#include <QKeyEvent>
#include <QClipboard>
#include <QMimeData>



PostingWidget::PostingWidget(NgPost *ngPost, MainWindow *hmi, uint jobNumber) :
    QWidget(hmi),
    _ui(new Ui::PostingWidget),
    _hmi(hmi),
    _ngPost(ngPost),
    _jobNumber(jobNumber),
    _postingJob(nullptr),
    _state(STATE::IDLE),
    _postingFinished(false),
    _postInfoCB(nullptr),
    _postInfoButton(nullptr),
    _postInfoTemplate(),
    _postInfoMeta()
{
    _ui->setupUi(this);
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
    delete _ui;
}

void PostingWidget::onFilePosted(QString filePath, uint nbArticles, uint nbFailed)
{
    int nbFiles = _ui->filesList->count();
    for (int i = 0 ; i < nbFiles ; ++i)
    {
        QListWidgetItem *item = _ui->filesList->item(i);
        if (item->text() == filePath)
        {
            QColor color(_hmi->sDoneOKColor);
            if (nbFailed == 0)
                item->setText(QString("%1 [%2 ok]").arg(filePath).arg(nbArticles));
            else
            {
                item->setText(QString("%1 [%2 err / %3]").arg(filePath).arg(nbFailed).arg(nbArticles));
                if (nbFailed == nbArticles)
                    color = _hmi->sDoneKOColor;
                else
                    color = _hmi->sArticlesFailedColor;
            }
            item->setForeground(color);
            break;
        }
    }
}

void PostingWidget::onArchiveFileNames(QStringList paths)
{
    _ui->filesList->clear();
    for (const QString & path : paths)
        _ui->filesList->addPath(path);
}

void PostingWidget::onArticlesNumber(int nbArticles)
{
    Q_UNUSED(nbArticles);
    _hmi->setJobLabel(static_cast<int>(_jobNumber));
}

void PostingWidget::onPostingJobDone()
{
    // we could arrive here twice: from PostingJob::postingFinished or PostingJob::noMoreConnection
    // This could happen especially when we exceed the number of connections allowed by a provider
    if (!_postingJob)
        return;

    if (_postingJob->nbArticlesTotal() > 0)
    {
        if (_postingJob->nbArticlesFailed() > 0)
            _hmi->updateJobTab(this, _hmi->sDoneKOColor, QIcon(_hmi->sDoneKOIcon));
        else
            _hmi->updateJobTab(this, _hmi->sDoneOKColor, QIcon(_hmi->sDoneOKIcon));
    }
    else
        _hmi->clearJobTab(this);

    disconnect(_postingJob);
    _postingJob = nullptr; //!< we don't own it, NgPost will delete it
    _postingFinished = true;
    setIDLE();
}

void PostingWidget::onPostFiles()
{
    postFiles(true);
}

void PostingWidget::postFiles(bool updateMainParams)
{
    if (_state == STATE::IDLE)
    {
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

        // check if the nzb file name already exist
        QString nzbPath = _ngPost->nzbPath();
        if (!nzbPath.endsWith(".nzb"))
            nzbPath += ".nzb";
        QFileInfo fiNzb(nzbPath);
        if (fiNzb.exists())
        {
            int overwrite = QMessageBox::question(nullptr,
                                                  tr("Overwrite existing nzb file?"),
                                                  tr("The nzb file '%1' already exists.\nWould you like to overwrite it ?").arg(nzbPath),
                                                  QMessageBox::Yes,
                                                  QMessageBox::No);
            if (overwrite == QMessageBox::No)
                return;
        }

        _postingFinished = false;
        _state = STATE::POSTING;
        PostingJobOptions options = _ngPost->_baseJobOptions();
        options.nzbFilePath       = nzbPath;
        options.files             = files;
        // in the GUI the list holds exactly what the user dropped, folders included
        options.inputPaths.reserve(files.size());
        for (QFileInfo const &file : files)
            options.inputPaths << file.absoluteFilePath();
        // the GUI already asked about overwriting, and never deletes the sources
        options.overwriteNzb      = true;
        options.delFilesAfterPost = false;
        // per post, deliberately not copied into the NgPost globals
        // The fields belong to the post info feature as a whole: with the box
        // unticked there is no sheet, and nothing to publish in the nzb either.
        // Leaving them in would publish through a box the user just turned off.
        options.writePostInfoFile = writesPostInfoFile();
        if (options.writePostInfoFile)
        {
            options.meta             = _postInfoMeta;
            options.postInfoTemplate = _postInfoTemplate;
            options.postInfoOutput   = _postInfoOutput;
        }

        _postingJob = new PostingJob(_ngPost, options, this);

        bool hasStarted = _ngPost->startPostingJob(_postingJob);
        if (_ngPost->lastPostingStartCanceled())
        {
            _postingJob = nullptr;
            _postingFinished = false;
            setIDLE();
            return;
        }

        QString buttonTxt;
        QColor  tabColor;
        QString tabIcon;
        if (hasStarted)
        {
            buttonTxt = tr("Stop Posting");
            tabColor  = _hmi->sPostingColor;
            tabIcon   = _hmi->sPostingIcon;
        }
        else
        {
            buttonTxt = tr("Cancel Posting");
            tabColor  = _hmi->sPendingColor;
            tabIcon   = _hmi->sPendingIcon;
        }
        _ui->postButton->setText(buttonTxt);
        _hmi->updateJobTab(this, tabColor, QIcon(tabIcon), _postingJob->nzbName());
    }
    else  if (_state == STATE::POSTING)
    {
        _state = STATE::STOPPING;
        emit _postingJob->stopPosting();
    }
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
    _ui->nzbPassEdit->setText(_ngPost->randomPass(static_cast<uint>(_ui->passLengthSB->value())));
}

void PostingWidget::onSelectFilesClicked()
{
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
    _ui->filesList->clear2();
    // The post information describes the post that was there; leaving it would
    // hand the title of one post to the next one queued in this tab.
    _postInfoMeta.clear();
    _postInfoTemplate.clear();
    _postInfoOutput.clear();
    _ui->nzbFileEdit->clear();
    _ui->compressNameEdit->clear();
    if (_hmi->hasAutoCompress())
    {
        onGenCompressName();
        onGenNzbPassword();
    }
    else
        _ui->compressNameEdit->clear();

    _hmi->clearJobTab(this);
}

void PostingWidget::onCompressCB(bool checked)
{
    // Everything here only means something while ngPost is the one building the
    // archive, so it follows the box. Each greyed control now says what it is
    // waiting for: a greyed box with no explanation was reported as a broken one.
    QString const needsCompress = tr("requires: %1").arg(_ui->compressCB->text());

    setDependentEnabled(_ui->compressNameEdit, checked,
                        tr("archive name (file name obfuscation)"), needsCompress);
    setDependentEnabled(_ui->nameLengthSB, checked,
                        tr("length of the archive name drawn by the dice button (config LENGTH_NAME)"),
                        needsCompress);
    setDependentEnabled(_ui->genCompressName, checked,
                        tr("generate random archive name"), needsCompress);
    setDependentEnabled(_ui->keepRarCB, checked,
                        tr("by default archives and par2 files are deleted uppon post success but you can choose to keep them"),
                        needsCompress);
    setDependentEnabled(_ui->nzbPassCB, checked,
                        tr("This should be the password of the archive you're posting"), needsCompress);
    setDependentEnabled(_ui->par2CB, checked,
                        tr("generate the par2 (the compress option must be selected)"), needsCompress);

    // The password field and the redundancy have a switch of their own on top
    // of this one, so they answer to both.
    onNzbPassToggled(_ui->nzbPassCB->isChecked());
    onPar2CB(_ui->par2CB->isChecked());
}

void PostingWidget::onPar2CB(bool checked)
{
    // Same rule: the redundancy percentage answers to PAR2, which itself
    // answers to the compression of this post.
    bool const compress = _ui->compressCB->isChecked();
    QString const needs = compress ? tr("requires: %1").arg(_ui->par2CB->text())
                                   : tr("requires: %1").arg(_ui->compressCB->text());

    setDependentEnabled(_ui->redundancySB, checked && compress,
                        tr("Using PAR2_ARGS from config file: %1").arg(_ngPost->_par2Args),
                        needs);
}

void PostingWidget::onGenCompressName()
{
    _ui->compressNameEdit->setText(_ngPost->randomPass(static_cast<uint>(_ui->nameLengthSB->value())));
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

    _ui->redundancySB->setRange(0, 100);
    _ui->redundancySB->setValue(static_cast<int>(_ngPost->_par2Pct));
    // The suffix travels inside the spin box, so the number stops being an
    // unlabelled one without costing a widget and its layout spacing.
    _ui->redundancySB->setSuffix(QStringLiteral(" %"));

    if (!_ngPost->_rarPassFixed.isEmpty())
    {
        _ui->nzbPassCB->setChecked(true);
        _ui->nzbPassEdit->setText(_ngPost->_rarPassFixed);
    }

    _ui->nameLengthSB->setRange(5, 50);
    _ui->nameLengthSB->setValue(static_cast<int>(_ngPost->_lengthName));
    _ui->passLengthSB->setRange(5, 50);
    _ui->passLengthSB->setValue(static_cast<int>(_ngPost->_lengthPass));

    _ui->copyNfoWithNzbCB->setChecked(_ngPost->_copyNfoWithNzb);

    _ui->filesList->setSelectionMode(QAbstractItemView::ExtendedSelection);

    connect(_ui->selectFilesButton, &QAbstractButton::clicked, this, &PostingWidget::onSelectFilesClicked);
    connect(_ui->selectFolderButton,&QAbstractButton::clicked, this, &PostingWidget::onSelectFolderClicked);
    connect(_ui->clearFilesButton,  &QAbstractButton::clicked, this, &PostingWidget::onClearFilesClicked);
    connect(_ui->filesList,         &SignedListWidget::empty,  this, &PostingWidget::onClearFilesClicked, Qt::QueuedConnection);

    connect(_ui->compressCB,        &QAbstractButton::toggled, this, &PostingWidget::onCompressCB);
    connect(_ui->genCompressName,   &QAbstractButton::clicked, this, &PostingWidget::onGenCompressName);

    connect(_ui->nzbFileButton,     &QAbstractButton::clicked, this, &PostingWidget::onNzbFileClicked);


    onCompressCB(_ngPost->_doCompress);
    onPar2CB(_ui->par2CB->isChecked());
    if (_ngPost->_doCompress)
        _ui->compressCB->setChecked(true);
    if (_ngPost->_genName)
        onGenCompressName();
    if (_ngPost->_genPass)
    {
        _ui->nzbPassCB->setChecked(true);
        onGenNzbPassword();
    }
    if (_ngPost->_doPar2)
        _ui->par2CB->setChecked(true);

    QString fixedPass = _hmi->fixedArchivePassword();
    if (!fixedPass.isEmpty())
        _ui->nzbPassEdit->setText(fixedPass);
}

void PostingWidget::genNameAndPassword(bool genName, bool genPass, bool doPar2)
{
    _ui->compressCB->setChecked(_ngPost->_doCompress);
    if (genName)
        onGenCompressName();
    if (genPass && _ngPost->_rarPassFixed.isEmpty())
    {
        _ui->nzbPassCB->setChecked(true);        
        onGenNzbPassword();
    }
    if (doPar2)
        _ui->par2CB->setChecked(true);

    _ui->keepRarCB->setChecked(_ngPost->_keepRarDefault);
}



void PostingWidget::udatePostingParams()
{
    if (!_ui->nzbFileEdit->text().isEmpty())
    {
        QFileInfo nzb(_ui->nzbFileEdit->text());
        if (!nzb.absolutePath().isEmpty())
            _ngPost->_nzbPath = nzb.absolutePath();
        _ngPost->setNzbName(nzb);
    }

    // fetch compression settings. The compression paths and the volume size are
    // NOT read here any more: they are configuration, they live in the
    // Compression settings dialog, and every tab used to overwrite the same
    // NgPost members with its own copy so the last active tab silently won.
    _ngPost->_doCompress = _ui->compressCB->isChecked();
    _ngPost->_rarName    = _ui->compressNameEdit->text();
    if (_ui->nzbPassCB->isChecked())
        _ngPost->_rarPass = _ui->nzbPassEdit->text().toLocal8Bit();
    else
        _ngPost->_rarPass = QString();
    _ngPost->_lengthName = static_cast<uint>(_ui->nameLengthSB->value());
    _ngPost->_lengthPass = static_cast<uint>(_ui->passLengthSB->value());
    // fetch par2 settings
    _ngPost->_doPar2  = _ui->par2CB->isChecked();
    _ngPost->_par2Pct = static_cast<uint>(_ui->redundancySB->value());

    _ngPost->_keepRar = _ui->keepRarCB->isChecked();

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
    PostInfoData data;
    data.appVersion = QString(APP_VERSION);

    if (!_ui->nzbFileEdit->text().isEmpty())
    {
        QFileInfo const nzb(_ui->nzbFileEdit->text());
        data.nzbPath     = nzb.absoluteFilePath();
        data.nzbDir      = nzb.absolutePath();
        data.nzbName     = nzb.completeBaseName();
        data.nzbFileName = nzb.fileName();
    }

    data.rarName = _ui->compressNameEdit->text();
    if (_ui->nzbPassCB->isChecked())
        data.rarPass = _ui->nzbPassEdit->text();

    data.groups = _ngPost->groups();
    // Left empty when ngPost draws a poster at random: showing one sample
    // would name an address the post is not going to use.
    if (!_ngPost->_genFrom && !_ngPost->_from.empty())
        data.nzbPoster = QString::fromStdString(_ngPost->_from);

    data.par2Pct = _ui->par2CB->isChecked() ? _ui->redundancySB->value() : -1;

    QFileInfoList files;
    bool          hasFolder = false;
    const_cast<PostingWidget *>(this)->_buildFilesList(files, hasFolder);
    if (!files.isEmpty())
    {
        data.sourcePath   = files.first().absoluteFilePath();
        data.originalName = files.first().fileName();
        data.originalPath = files.first().absolutePath();
    }
    return data;
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
    // code built widgets are not touched by retranslateUi()
    retranslatePostInfoTexts();
    // The tooltips of the dependent controls carry both their help text and,
    // while they are greyed, what they are waiting for. retranslateUi() has just
    // reset them to the plain .ui text, so let the handlers rebuild both halves
    // in the new language instead of setting them here and losing the state.
    onCompressCB(_ui->compressCB->isChecked());
    onPar2CB(_ui->par2CB->isChecked());
    _ui->filesList->setToolTip(QString("%1<ul><li>%2</li><li>%3</li><li>%4</li></ul>%5").arg(
                                   tr("You can add files or folder by:")).arg(
                                   tr("Drag & Drop files/folders")).arg(
                                   tr("Right Click to add Files")).arg(
                                   tr("Click on Select Files/Folder buttons")).arg(
                                   tr("Bare in mind you can select items in the list and press DEL to remove them")));
}

void PostingWidget::setNzbPassword(const QString &pass)
{
    _ui->nzbPassCB->setChecked(true);
    _ui->nzbPassEdit->setText(pass);
}

void PostingWidget::setPackingAuto(bool enabled, const QStringList &keys)
{
    bool compress = false, genName = false, genPass = false, doPar2 = false;
    if (enabled)
    {
        for (auto it = keys.cbegin(), itEnd = keys.cend(); it != itEnd; ++it)
        {
            QString keyWord = (*it).toLower();
            if (keyWord == NgPost::optionName(NgPost::Opt::COMPRESS))
                compress = true;
            else if (keyWord == NgPost::optionName(NgPost::Opt::GEN_NAME))
                genName = true;
            else if (keyWord == NgPost::optionName(NgPost::Opt::GEN_PASS))
                genPass = true;
            else if (keyWord == NgPost::optionName(NgPost::Opt::GEN_PAR2))
                doPar2 = true;
        }
    }
    _ui->nzbPassCB->setChecked(genPass);
    _ui->compressCB->setChecked(compress);
    _ui->par2CB->setChecked(doPar2);


    if (compress)
    {
        if (_ui->nzbPassEdit->text().isEmpty())
        {
            if (_hmi->useFixedPassword())
                _ui->nzbPassEdit->setText(_ngPost->_rarPassFixed);
            else if (genPass)
                onGenNzbPassword();
        }
        if (genName && _ui->compressNameEdit->text().isEmpty())
            onGenCompressName();
    }
}

void PostingWidget::addPath(const QString &path, int currentNbFiles, int isDir)
{
    if (_ui->filesList->addPathIfNotInList(path, currentNbFiles, isDir))
    {
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

void PostingWidget::setIDLE()
{
    _ui->postButton->setText(tr("Post Files"));
    _state = STATE::IDLE;
}

void PostingWidget::setPosting()
{
    _hmi->updateJobTab(this, _hmi->sPostingColor, QIcon(_hmi->sPostingIcon), _postingJob->nzbName());
    _ui->postButton->setText(tr("Stop Posting"));
    _state = STATE::POSTING;
}

void PostingWidget::attachResumeJob(PostingJob *job, const QFileInfoList &files, bool hasStarted)
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

    if (hasStarted) {
        _ui->postButton->setText(tr("Stop Posting"));
        _hmi->updateJobTab(this, _hmi->sPostingColor, QIcon(_hmi->sPostingIcon), job->nzbName());
    } else {
        _ui->postButton->setText(tr("Cancel Posting"));
        _hmi->updateJobTab(this, _hmi->sPendingColor, QIcon(_hmi->sPendingIcon), job->nzbName());
    }
}
