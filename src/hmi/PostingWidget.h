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

#ifndef POSTINGWIDGET_H
#define POSTINGWIDGET_H

#include <QWidget>
class NgPost;
class NntpFile;
class MainWindow;
class PostingJob;
#include "postinfo/PostInfoData.h"

#include <QFileInfoList>
#include <QMap>

class QCheckBox;
class QGroupBox;
class QPushButton;
class QTableWidget;
class QToolButton;
class QWidget;

namespace Ui {
class PostingWidget;
}

class PostingWidget : public QWidget
{
    Q_OBJECT

private:
    enum class STATE {IDLE, POSTING, STOPPING};

    Ui::PostingWidget *_ui;
    MainWindow        *_hmi;
    NgPost            *_ngPost;
    const uint         _jobNumber;
    PostingJob        *_postingJob;
    STATE              _state;
    bool               _postingFinished;

    // Post info file, per post. One discreet checkbox on the tab; everything
    // else lives in a dialog, because a posting tab is about posting.
    QCheckBox   *_postInfoCB;
    QPushButton *_postInfoButton;
    //! Model this post uses, empty when it takes the one from the config.
    QString _postInfoTemplate;
    QString _postInfoOutput;
    //! What the user typed for THIS post, never shared with the next one.
    QMap<QString, MetaValue> _postInfoMeta;

public:
    explicit PostingWidget(NgPost *ngPost, MainWindow *hmi, uint jobNumber);
    ~PostingWidget();

    void setIDLE();
    void setPosting();
    void attachResumeJob(PostingJob *job, const QFileInfoList &files, bool hasStarted);

    void init();
    void genNameAndPassword(bool genName, bool genPass, bool doPar2, bool useRarMax);
    inline uint jobNumber() const;

    void handleDropEvent(QDropEvent *e);
    void handleKeyEvent(QKeyEvent *keyEvent);

    void addPath(const QString &path, int currentNbFiles, int isDir = false);

    inline bool isPosting() const;
    inline bool isPostingFinished() const;
    inline MainWindow *hmi() const;

    void udatePostingParams();

    //! True when this post asks for a record sheet.
    bool writesPostInfoFile() const;
    //! Values the record sheet can already show while the post is prepared.
    PostInfoData _postInfoPreview() const;
    //! Applied by the auto-post tab to every post it launches.
    void setPostInfo(bool enabled,
                     const QString &templateOverride,
                     const QString &outputOverride,
                     const QMap<QString, MetaValue> &meta);
    QString postInfoTemplateOverride() const { return _postInfoTemplate; }
    QMap<QString, MetaValue> postInfoMeta() const { return _postInfoMeta; }

    void retranslate();

    void setNzbPassword(const QString &pass);
    void setPackingAuto(bool enabled, const QStringList &keys);

    void postFiles(bool updateMainParams);


public slots: // for PostingJob
    void onFilePosted(QString filePath, uint nbArticles, uint nbFailed);
    void onArchiveFileNames(QStringList paths);
    void onArticlesNumber(int nbArticles);
    void onPostingJobDone();

    void onPostFiles(); //!< for the post button but also can be used by the AutoPostWidget

private slots: // for the HMI

    void onNzbPassToggled(bool checked);
    void onPostInfoToggled(bool checked);
    void onEditPostInfo();
    void onGenNzbPassword();


    void onSelectFilesClicked();
    void onSelectFolderClicked();
    void onClearFilesClicked();
    void onCompressCB(bool checked);
    void onPar2CB(bool checked);
    void onGenCompressName();
    void onCompressPathClicked();
    void onNzbFileClicked();
    void onRarPathClicked();


private:
    void _buildPostInfoRow();
    void retranslatePostInfoTexts();
    void _buildFilesList(QFileInfoList &files, bool &hasFolder);
    bool _fileAlreadyInList(const QString &fileName, int currentNbFiles) const;


};

uint PostingWidget::jobNumber() const { return _jobNumber; }
bool PostingWidget::isPosting() const { return _postingJob != nullptr; }
bool PostingWidget::isPostingFinished() const { return _postingFinished; }

MainWindow *PostingWidget::hmi() const { return _hmi; }

#endif // POSTINGWIDGET_H
