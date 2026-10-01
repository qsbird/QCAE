#pragma once

#include <QHash>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <cstdint>
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
    [[nodiscard]] qsizetype pendingRequests() const {
        return pending_.size();
    }
    [[nodiscard]] QString request(const QString& operation,
                                  const QJsonObject& parameters,
                                  const QJsonObject& context,
                                  Reply callback,
                                  qsizetype max_reply_bytes = 0);
    [[nodiscard]] QString endpoint() const {
        return options_.endpoint;
    }
    [[nodiscard]] bool supportsEvents() const {
        return supports_events_;
    }
    [[nodiscard]] bool supportsResources() const {
        return supports_resources_;
    }
    [[nodiscard]] bool supportsInlineEmptyRender() const {
        return supports_inline_empty_render_;
    }
    [[nodiscard]] unsigned renderWireVersion() const {
        return render_wire_version_;
    }
    [[nodiscard]] bool supportsEventsDocumentSummary() const {
        return supports_events_document_summary_;
    }
    [[nodiscard]] bool supportsRenderModelRebase() const {
        return supports_render_model_rebase_;
    }
    [[nodiscard]] bool supportsRenderChangedRows() const {
        return supports_render_changed_rows_;
    }
    [[nodiscard]] QString engineInstanceId() const {
        return engine_instance_id_;
    }
    [[nodiscard]] quint64 eventSequence() const {
        return event_sequence_;
    }
    // Called after an explicit subscription/read baseline and authoritative resync.
    void setEventCursor(const QString& engine_instance_id, quint64 sequence);

  signals:
    void readyChanged(bool ready);
    void transportError(const QString& message);
    void eventReceived(const QJsonObject& event);
    void eventGap(const QString& reason);

  private:
    struct Pending {
        Reply callback;
        qint64 deadline_ms{};
        qsizetype max_reply_bytes{};
    };
    void connectNow();
    void launchEngine();
    void onConnected();
    void onDisconnected();
    void consume();
    void deliver(const QJsonObject& response, qsizetype frame_bytes);
    void deliverEvent(const QJsonObject& event);
    void requireEventResync(const QString& reason, quint64 sequence);
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
    bool supports_events_{false};
    bool supports_resources_{false};
    bool supports_inline_empty_render_{false};
    bool supports_events_document_summary_{false};
    bool supports_render_model_rebase_{false};
    bool supports_render_changed_rows_{false};
    unsigned render_wire_version_{2};
    bool events_need_resync_{true};
    QString engine_instance_id_;
    quint64 event_sequence_{};
    std::uint64_t transport_generation_{};
};

} // namespace qcae
