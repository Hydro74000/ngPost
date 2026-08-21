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

#ifndef AUTOPOSTWIDGET_H
#define AUTOPOSTWIDGET_H

#include "postinfo/PostInfoData.h"

#include <QMap>
#include <QWidget>

class QCheckBox;
class QPushButton;
class NgPost;
class MainWindow;
class QFileInfo;


namespace Ui {
class AutoPostWidget;
}

class AutoPostWidget : public QWidget
{
    Q_OBJECT
private:
    Ui::AutoPostWidget *_ui;

    //! Post info file for the whole auto-post run: one choice, applied to
    //! every post it launches.
    QCheckBox   *_postInfoCB = nullptr;
    QPushButton *_postInfoButton = nullptr;
    QString _postInfoTemplate;
    QMap<QString, MetaValue> _postInfoMeta;

    void _buildPostInfoRow();
    void retranslatePostInfoTexts();
    MainWindow         *_hmi;
    NgPost             *_ngPost;
    bool                _isMonitoring;
    int                 _currentPostIdx;

public:
    explicit AutoPostWidget(NgPost *ngPost, MainWindow *hmi);
    ~AutoPostWidget();

    void init();

    void handleKeyEvent(QKeyEvent *keyEvent);
    void handleDropEvent(QDropEvent *e);


    void udatePostingParams();

    void newFileToProcess(const QFileInfo &fileInfo);
    void updateFinishedJob(const QString &path, uint nbArticles, uint nbUploaded, uint nbFailed);

    bool deleteFilesOncePosted() const;

    void retranslate();

    void setPackingAuto(bool enabled, const QStringList &keys);

public slots:
    void onMonitorJobStart();

private slots:
    void onPostInfoToggled(bool checked);
    void onEditPostInfo();
    void onSelectFilesClicked();
    void onGenQuickPosts();
    void onCompressPathClicked();
    void onRarPathClicked();
    void onSelectAutoDirClicked();
    void onScanAutoDirClicked();
    void onMonitoringClicked();
    void onDelFilesToggled(bool checked);
    void onAddMonitoringFolder();
    void onCompressToggled(bool checked);
    void onGenPar2Toggled(bool checked);
};

#endif // AUTOPOSTWIDGET_H
