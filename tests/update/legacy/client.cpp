#include "UpdateChecker.h"
#include "NgPost.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <cstring>

class Reply : public QNetworkReply
{
public:
    QByteArray bytes;
    Reply(QObject *parent, QByteArray body)
        : QNetworkReply(parent)
        , bytes(std::move(body))
    {
        open(QIODevice::ReadOnly);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        QTimer::singleShot(0, this, [this] {
            emit readyRead();
            emit finished();
        });
    }
    void abort() override { }
    qint64 bytesAvailable() const override
    {
        return bytes.size() + QNetworkReply::bytesAvailable();
    }
    qint64 readData(char *data, qint64 maximum) override
    {
        auto count = qMin(maximum, qint64(bytes.size()));
        memcpy(data, bytes.constData(), size_t(count));
        bytes.remove(0, count);
        return count;
    }
};
class Network : public QNetworkAccessManager
{
public:
    QJsonObject files;
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        QFile file(files.value(request.url().toString()).toString());
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Unmapped legacy update request");
        return new Reply(this, file.readAll());
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QFile scenario(qEnvironmentVariable("NGPOST_UPDATE_SCENARIO"));
    if (!scenario.open(QIODevice::ReadOnly))
        return 2;
    Network network;
    network.files = QJsonDocument::fromJson(scenario.readAll()).object();
    NgPost coordinator;
    UpdateChecker checker(&coordinator, &network);
    QObject::connect(&checker,
                     &UpdateChecker::newVersionAvailable,
                     &checker,
                     &UpdateChecker::startDownloadAndInstall);
    QObject::connect(&checker, &UpdateChecker::downloadFailed, &app, [&app] { app.exit(3); });
    QTimer::singleShot(30000, &app, [&app] { app.exit(4); });
    checker.checkLatestRelease();
    return app.exec();
}
