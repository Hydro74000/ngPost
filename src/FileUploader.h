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

#ifndef FILEUPLOADER_H
#define FILEUPLOADER_H
#include <QFile>
#include <QFileInfo>
#include <QUrl>
class QNetworkAccessManager;
class QNetworkReply;

class FileUploader : public QObject
{
    Q_OBJECT

private:
    QNetworkAccessManager &_netMgr;
    QNetworkReply *_reply;
    QFileInfo _nzbFilePath;
    QFile _nzbFile;
    QUrl _nzbUrl;

public:
    FileUploader(QNetworkAccessManager &netMgr, const QString &nzbFilePath);
    ~FileUploader();

    void startUpload(const QUrl &serverUrl);

    //! Gives up any transfer in flight and closes the nzb, synchronously.
    //! deleteLater() alone is not enough before running a post command: the
    //! file stays open until the event loop deletes us, and on Windows a hook
    //! that moves the nzb would fail.
    void release();

signals:
    void readyToDie();
    void error(const QString &msg);
    void log(const QString &msg, bool newline = true);

private slots:
    void onUploadFinished();

private:
    inline QString url() const;
};

QString FileUploader::url() const
{
    if (_nzbUrl.isEmpty())
        return QString();
    else
        return _nzbUrl.toString(QUrl::RemovePassword | QUrl::RemovePath);
}

#endif // FILEUPLOADER_H
