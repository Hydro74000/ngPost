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

#include "FileUploader.h"
#include <QHttpMultiPart>
#include <QNetworkAccessManager>
#include <QNetworkReply>
FileUploader::FileUploader(QNetworkAccessManager &netMgr, const QString &nzbFilePath)
    : QObject()
    , _netMgr(netMgr)
    , _reply(nullptr)
    , _nzbFilePath(nzbFilePath)
    , _nzbFile(nzbFilePath)
    , _nzbUrl()
    , _responseBytes(0)
{}

FileUploader::~FileUploader()
{
    if (_nzbFile.isOpen()) {
#ifdef __DEBUG__
        qDebug() << "Deleting FileUploader for " << _nzbFile.fileName();
#endif
        _nzbFile.close();
    }

    if (_reply)
        delete _reply;
}

void FileUploader::release()
{
    // abort() is allowed to emit finished() synchronously. Detach and clear
    // the member first so onUploadFinished()/a nested release() cannot re-enter
    // with the same reply and leave this frame dereferencing a null pointer.
    QNetworkReply *reply = _reply;
    _reply = nullptr;
    if (reply) {
        QObject::disconnect(reply, nullptr, this, nullptr);
        if (reply->isRunning())
            reply->abort();
        reply->deleteLater();
    }
    if (_nzbFile.isOpen())
        _nzbFile.close();
}

void FileUploader::startUpload(const QUrl &serverUrl)
{
    // Keep the sanitized endpoint available for every diagnostic, including
    // an unsupported scheme that never creates a QNetworkReply.
    _nzbUrl = serverUrl;
    _responseBytes = 0;
    if (_nzbFile.open(QIODevice::ReadOnly)) {
        QString protocol = serverUrl.scheme(); // always lowercase
        if (protocol == "ftp") {
            _nzbUrl = QUrl(QString("%1/%2").arg(serverUrl.url()).arg(_nzbFilePath.fileName()));
#ifdef __DEBUG__
            qDebug() << "FileUploader FTP url: " << url();
#endif

            _reply = _netMgr.put(QNetworkRequest(_nzbUrl), &_nzbFile);
        } else if (protocol.startsWith("http")) {
#ifdef __DEBUG__
            qDebug() << "FileUploader POST on url: " << url();
#endif
            QHttpMultiPart *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
            QString fileKey("file"), fileName = QFileInfo(_nzbFilePath).fileName();
            fileName.replace('"', '\'');
            QHttpPart filePart;
            filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                               QString("form-data; name=\"%1\"; filename=\"%2\"")
                                   .arg(fileKey)
                                   .arg(fileName));
            filePart.setBodyDevice(&_nzbFile);
            multiPart->append(filePart);

            QNetworkRequest req(_nzbUrl);
            req.setRawHeader("User-Agent", "ngPost C++ app");

            _reply = _netMgr.post(req, multiPart);

            multiPart->setParent(_reply); // multiPart deleted on the destruction of reply
        } else {
            emit error(tr("Error uploading nzb to %1: Protocol not supported").arg(url()));
            emit readyToDie();
        }
        if (_reply) {
            QObject::connect(_reply,
                             &QIODevice::readyRead,
                             this,
                             &FileUploader::onReplyReadyRead);
            QObject::connect(_reply,
                             &QNetworkReply::finished,
                             this,
                             &FileUploader::onUploadFinished);
        }
    } else {
        emit error(tr("Error uploading file: can't open file %1").arg(_nzbFile.fileName()));
        emit readyToDie();
    }
}

void FileUploader::onReplyReadyRead()
{
    if (_reply)
        _responseBytes += _reply->readAll().size();
}

void FileUploader::onUploadFinished()
{
    // A queued finished() may already have been posted when release()
    // disconnects a timed-out upload.
    if (!_reply)
        return;
    onReplyReadyRead(); // drain bytes delivered with finished()
    qDebug() << "FileUploader reply received (bytes): " << _responseBytes;
    if (_reply->error())
        emit error(tr("Error uploading nzb to %1: %2").arg(url()).arg(_reply->errorString()));
    else
        emit log(tr("nzb %1 uploaded to %2\n").arg(_nzbFilePath.fileName()).arg(url()));

    emit readyToDie();
}
