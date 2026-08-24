#pragma once

#include "service.hpp"
#include <QOAuth2AuthorizationCodeFlow>
#include <QOAuthHttpServerReplyHandler>
#include <QObject>
#include <QSslSocket>
#include <QString>
#include <QThread>
#include <QtQml/qqmlregistration.h>

namespace caelestia::services {

class MailWorker : public QObject {
    Q_OBJECT

public:
    explicit MailWorker(const QString& host, quint16 port, const QString& email, const QString& accessToken);
    ~MailWorker() override;

public slots:
    void start();
    void stop();

signals:
    void newMailDetails(const QString& sender, const QString& subject, const QString& snippet);
    void authFailed();
    void connectionChanged(bool connected);

private:
    void processImapLine(const QString& line);
    void fetchLastMessage(int index);
    void reEnterIdle();

    QString m_host;
    quint16 m_port;
    QString m_email;
    QString m_accessToken;
    QSslSocket* m_socket = nullptr;
    bool m_running = false;
    bool m_inIdle = false;
    int m_lastKnownCount = -1;

    // Buffer state for parsing FETCH responses
    bool m_fetching = false;
    QString m_curFrom;
    QString m_curSubject;
    QString m_curSnippet;
};

class MailProvider : public Service {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool connected READ connected NOTIFY connectionChanged FINAL)

public:
    explicit MailProvider(QObject* parent = nullptr);
    ~MailProvider() override;

    void start() override;
    void stop() override;

    Q_INVOKABLE void init(const QString& email, const QString& clientId, const QString& clientSecret);
    Q_INVOKABLE void startLoginFlow();

    [[nodiscard]] bool connected() const { return m_connected; }

signals:
    void newMailReceived(const QString& sender, const QString& subject, const QString& snippet);
    void authRequired();
    void connectionChanged(bool connected);

private:
    void setupOAuth();
    void startWorker(const QString& accessToken);
    QString loadRefreshToken();
    void saveRefreshToken(const QString& token);

    QString m_email;
    QString m_clientId;
    QString m_clientSecret;
    bool m_connected = false;

    QOAuth2AuthorizationCodeFlow* m_oauth = nullptr;
    QThread m_workerThread;
    MailWorker* m_worker = nullptr;
};

} // namespace caelestia::services
