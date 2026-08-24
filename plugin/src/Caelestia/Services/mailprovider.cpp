#include "mailprovider.hpp"
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

namespace caelestia::services {

MailWorker::MailWorker(const QString& host, quint16 port, const QString& email, const QString& accessToken)
    : m_host(host)
    , m_port(port)
    , m_email(email)
    , m_accessToken(accessToken) {}

MailWorker::~MailWorker() {
    stop();
}

void MailWorker::start() {
    m_running = true;
    m_socket = new QSslSocket(this);

    connect(m_socket, &QSslSocket::readyRead, this, [this]() {
        while (m_socket->canReadLine()) {
            QString line = QString::fromUtf8(m_socket->readLine()).trimmed();
            processImapLine(line);
        }
    });

    connect(m_socket, &QSslSocket::connected, this, [this]() {
        emit connectionChanged(true);
        QByteArray sasl = "user=" + m_email.toUtf8() + "\x01" + "auth=Bearer " + m_accessToken.toUtf8() + "\x01\x01";
        m_socket->write("A1 AUTHENTICATE XOAUTH2 " + sasl.toBase64() + "\r\n");
    });

    connect(m_socket, &QSslSocket::disconnected, this, [this]() {
        emit connectionChanged(false);
    });

    m_socket->connectToHostEncrypted(m_host, m_port);
}

void MailWorker::processImapLine(const QString& line) {
    qDebug() << "[IMAP]:" << line;

    if (line.startsWith("A1 OK")) {
        m_socket->write("A2 SELECT INBOX\r\n");
    } else if (line.startsWith("A2 OK")) {
        // Initial INBOX select done, enter IDLE
        reEnterIdle();
    } else if (line.startsWith("A1 NO") || line.startsWith("A1 BAD")) {
        emit authFailed();
        stop();
    } else if (line.contains("EXISTS")) {
        // Matches lines like: "* 142 EXISTS"
        static QRegularExpression existsRegex(R"(\*\s+(\d+)\s+EXISTS)");
        auto match = existsRegex.match(line);
        if (match.hasMatch()) {
            int currentCount = match.captured(1).toInt();
            if (m_lastKnownCount != -1 && currentCount > m_lastKnownCount) {
                // Terminate IDLE to fetch headers for new email
                if (m_inIdle) {
                    m_socket->write("DONE\r\n");
                    m_inIdle = false;
                }
                fetchLastMessage(currentCount);
            }
            m_lastKnownCount = currentCount;
        }
    } else if (m_fetching) {
        if (line.startsWith("From:", Qt::CaseInsensitive)) {
            m_curFrom = line.mid(5).trimmed();
        } else if (line.startsWith("Subject:", Qt::CaseInsensitive)) {
            m_curSubject = line.mid(8).trimmed();
        } else if (line.startsWith("A3 OK")) {
            // Fetch finished -> emit and return to IDLE
            m_fetching = false;
            emit newMailDetails(m_curFrom.isEmpty() ? "Unknown Sender" : m_curFrom,
                m_curSubject.isEmpty() ? "No Subject" : m_curSubject, m_curSnippet);
            reEnterIdle();
        }
    }
}

void MailWorker::fetchLastMessage(int index) {
    m_fetching = true;
    m_curFrom.clear();
    m_curSubject.clear();
    m_curSnippet.clear();
    // PEEK keeps the unread state untouched
    m_socket->write(QString("A3 FETCH %1 (BODY.PEEK[HEADER.FIELDS (FROM SUBJECT)] BODY.PEEK[TEXT]<0.200>)\r\n")
            .arg(index)
            .toUtf8());
}

void MailWorker::reEnterIdle() {
    m_inIdle = true;
    m_socket->write("A4 IDLE\r\n");
}

void MailWorker::stop() {
    m_running = false;
    if (m_socket) {
        if (m_socket->isOpen()) {
            if (m_inIdle) {
                m_socket->write("DONE\r\n");
            }
            m_socket->disconnectFromHost();
        }
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

// --- MailProvider Implementation ---
MailProvider::MailProvider(QObject* parent)
    : Service(parent) {}

MailProvider::~MailProvider() {
    stop();
}

void MailProvider::start() {
    if (!m_email.isEmpty()) {
        QString storedRefreshToken = loadRefreshToken();
        if (!storedRefreshToken.isEmpty()) {
            m_oauth->setRefreshToken(storedRefreshToken);
            m_oauth->refreshAccessToken();
        } else {
            emit authRequired();
        }
    }
}

void MailProvider::stop() {
    if (m_workerThread.isRunning()) {
        m_workerThread.quit();
        m_workerThread.wait();
    }
}

void MailProvider::init(const QString& email, const QString& clientId, const QString& clientSecret) {
    m_email = email;
    m_clientId = clientId;
    m_clientSecret = clientSecret;

    setupOAuth();
    start();
}

void MailProvider::setupOAuth() {
    if (m_oauth)
        return;

    m_oauth = new QOAuth2AuthorizationCodeFlow(this);
    auto* replyHandler = new QOAuthHttpServerReplyHandler(12345, this);
    m_oauth->setReplyHandler(replyHandler);

    m_oauth->setAuthorizationUrl(QUrl("https://accounts.google.com/o/oauth2/v2/auth"));
    m_oauth->setAccessTokenUrl(QUrl("https://oauth2.googleapis.com/token"));
    m_oauth->setClientIdentifier(m_clientId);
    m_oauth->setClientIdentifierSharedKey(m_clientSecret);
    m_oauth->setScope("https://mail.google.com/");

    connect(m_oauth, &QOAuth2AuthorizationCodeFlow::authorizeWithBrowser, [](const QUrl& url) {
        QDesktopServices::openUrl(url);
    });

    connect(m_oauth, &QOAuth2AuthorizationCodeFlow::granted, this, [this]() {
        if (!m_oauth->refreshToken().isEmpty()) {
            saveRefreshToken(m_oauth->refreshToken());
        }
        startWorker(m_oauth->token());
    });
}

void MailProvider::startLoginFlow() {
    if (m_oauth) {
        m_oauth->grant();
    }
}

void MailProvider::startWorker(const QString& accessToken) {
    if (m_workerThread.isRunning()) {
        m_workerThread.quit();
        m_workerThread.wait();
    }

    m_worker = new MailWorker("imap.gmail.com", 993, m_email, accessToken);
    m_worker->moveToThread(&m_workerThread);

    connect(&m_workerThread, &QThread::started, m_worker, &MailWorker::start);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &MailWorker::newMailDetails, this, &MailProvider::newMailReceived);
    connect(m_worker, &MailWorker::authFailed, this, [this]() {
        m_oauth->refreshAccessToken();
    });
    connect(m_worker, &MailWorker::connectionChanged, this, [this](bool conn) {
        m_connected = conn;
        emit connectionChanged(conn);
    });

    m_workerThread.start();
}

QString MailProvider::loadRefreshToken() {
    QString path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/mail_refresh_token";
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        return QString::fromUtf8(file.readAll()).trimmed();
    }
    return QString();
}

void MailProvider::saveRefreshToken(const QString& token) {
    QString dirPath = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dirPath);
    QFile file(dirPath + "/mail_refresh_token");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(token.toUtf8());
    }
}

} // namespace caelestia::services
