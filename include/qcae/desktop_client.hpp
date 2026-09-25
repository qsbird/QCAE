#pragma once

#include <QHash>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <functional>

namespace qcae {

// The desktop owns no model state. All calls, including writes, cross this IPC client.
class DesktopClient : public QObject {
    Q_OBJECT
  public:
    using Reply = std::function<void(const QJsonObject&)>;
    struct Options {
        QString endpoint;
        QString workspace;
        QString engine;
        bool auto_start{true};
        qint64 request_timeout_ms{10000};
    };

    explicit DesktopClient(Options options, QObject* parent = nullptr);
    ~DesktopClient() override;
    void start();
    [[nodiscard]] bool ready() const {
        return ready_;
    }
    [[nodiscard]] QString request(const QString& operation,
                                  const QJsonObject& parameters,
                                  const QJsonObject& context,
                                  Reply callback);
    [[nodiscard]] QString endpoint() const {
        return options_.endpoint;
    }

  signals:
    void readyChanged(bool ready);
    void transportError(const QString& message);

  private:
    struct Pending {
        Reply callback;
        qint64 deadline_ms{};
    };
    void connectNow();
    void launchEngine();
    void onConnected();
    void onDisconnected();
    void consume();
    void deliver(const QJsonObject& response);
    void postReply(Reply callback, QJsonObject response);
    void failPending(const QString& code, const QString& message);
    void checkTimeouts();
    [[nodiscard]] bool send(const QJsonObject& frame);
    [[nodiscard]] static QJsonObject
    failure(const QString& id, const QString& code, const QString& message);

    Options options_;
    QLocalSocket socket_;
    QTimer reconnect_;
    QTimer deadlines_;
    QByteArray incoming_;
    QHash<QString, Pending> pending_;
    QString handshake_id_;
    qint64 handshake_deadline_ms_{};
    bool ready_{false};
    bool launched_{false};
};

} // namespace qcae
