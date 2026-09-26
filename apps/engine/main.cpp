#include "qcae/ipc_api.hpp"
#include "qcae/nastran_codec.hpp"
#include "qcae/query.hpp"
#ifdef QCAE_HAS_SQLITE
#include "qcae/sqlite_store.hpp"
#endif
#include "qcae/local_endpoint.hpp"
#include "qcae/operations.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QTextStream>
#include <QUuid>
#include <filesystem>
#include <memory>

namespace {
struct Connection {
    QLocalSocket* socket;
    QByteArray buffer;
    bool ready{false};
};
void send(QLocalSocket* socket, const QJsonObject& response) {
    auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > qcae::transport::max_frame_bytes) {
        auto id = response.value("request_id").toString();
        if (id.toUtf8().size() > 128)
            id.clear();
        bytes = QJsonDocument(
                    qcae::ipc::failure(id, "RESOURCE_LIMIT", "Response exceeds the M0 frame limit"))
                    .toJson(QJsonDocument::Compact) +
                '\n';
    }
    if (socket->bytesToWrite() + bytes.size() > qcae::transport::max_frame_bytes * 2) {
        socket->abort();
        return;
    }
    socket->write(bytes);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("QCAE");
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "QCAE local engine; optional SQLite workspace and shared model services");
    parser.addHelpOption();
    parser.addOption({{"s", "socket"}, "Local endpoint path", "path"});
    parser.addOption({"workspace", "SQLite working recovery database", "path"});
    parser.process(app);
    const QString endpoint =
        parser.isSet("socket") ? parser.value("socket") : qcae::transport::default_endpoint();
    if (endpoint.isEmpty() || !QFileInfo(endpoint).isAbsolute() ||
        !QFileInfo(QFileInfo(endpoint).absolutePath()).isDir()) {
        QTextStream(stderr)
            << "A valid absolute socket path with an existing parent directory is required\n";
        return 2;
    }
    QLockFile lock(endpoint + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        QTextStream(stderr)
            << "Another engine owns this endpoint, or its lock cannot be acquired\n";
        return 4;
    }
#ifdef Q_OS_UNIX
    std::error_code error;
    const auto status = std::filesystem::symlink_status(endpoint.toStdString(), error);
    if (!error && std::filesystem::exists(status)) {
        if (status.type() != std::filesystem::file_type::socket) {
            QTextStream(stderr) << "Refusing to replace a non-socket endpoint\n";
            return 4;
        }
        QLocalSocket probe;
        probe.connectToServer(endpoint);
        if (probe.waitForConnected(200)) {
            QTextStream(stderr) << "An active server already owns this socket\n";
            return 4;
        }
        if (probe.error() != QLocalSocket::ConnectionRefusedError &&
            probe.error() != QLocalSocket::ServerNotFoundError) {
            QTextStream(stderr) << "Cannot safely classify an existing endpoint\n";
            return 4;
        }
        if (!QLocalServer::removeServer(endpoint))
            return 4;
    }
#endif
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(endpoint)) {
        QTextStream(stderr) << server.errorString() << '\n';
        return 4;
    }
    const qcae::NastranCodec codec;
    std::shared_ptr<qcae::IWorkspaceStore> store;
    std::unique_ptr<qcae::MemoryApplication> application;
    try {
        if (parser.isSet("workspace")) {
#ifdef QCAE_HAS_SQLITE
            store = std::make_shared<qcae::SqliteWorkspaceStore>(
                parser.value("workspace").toStdString());
#else
            throw std::runtime_error("This build does not include SQLite persistence");
#endif
        }
        application = std::make_unique<qcae::MemoryApplication>(
            qcae::Limits{}, store, [&](const qcae::ProfileRef& profile) {
                return profile == codec.definition().reference;
            });
    } catch (const std::exception& error) {
        QTextStream(stderr) << "Workspace initialization failed: " << error.what() << '\n';
        return 5;
    }
    auto& core = *application;
    qcae::SelectionService selections;
    // M0 is a single local OS-user host. Request JSON cannot choose this identity.
    const qcae::Caller caller{"local-user"};
    const auto engine_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto version = QString::fromUtf8(qcae::api_version.data(),
                                           static_cast<qsizetype>(qcae::api_version.size()));
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        while (auto* socket = server.nextPendingConnection()) {
            socket->setReadBufferSize(qcae::transport::max_frame_bytes + 1);
            auto connection = std::make_shared<Connection>(Connection{socket, {}, false});
            auto consume = [&, connection] {
                auto* client = connection->socket;
                connection->buffer += client->readAll();
                while (true) {
                    const auto newline = connection->buffer.indexOf('\n');
                    if (newline < 0) {
                        if (connection->buffer.size() > qcae::transport::max_frame_bytes) {
                            send(client,
                                 qcae::ipc::failure(
                                     {}, "FRAME_TOO_LARGE", "Request exceeds frame limit"));
                            client->disconnectFromServer();
                        }
                        return;
                    }
                    if (newline > qcae::transport::max_frame_bytes) {
                        client->abort();
                        return;
                    }
                    const auto line = connection->buffer.left(newline);
                    connection->buffer.remove(0, newline + 1);
                    QJsonParseError parse_error;
                    const auto document = QJsonDocument::fromJson(line, &parse_error);
                    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
                        send(client,
                             qcae::ipc::failure(
                                 {}, "INVALID_JSON", "Expected one JSON object per line"));
                        continue;
                    }
                    const auto request = document.object();
                    const auto id = request.value("request_id").toString();
                    if (id.isEmpty() || id.toUtf8().size() > 128) {
                        send(client,
                             qcae::ipc::failure({},
                                                "INVALID_INPUT",
                                                "request_id must contain 1..128 UTF-8 bytes"));
                        continue;
                    }
                    if (request.value("operation").toString() == "runtime.handshake") {
                        if (!request.value("api_version").isString() ||
                            request.value("api_version").toString() != version) {
                            send(client,
                                 qcae::ipc::failure(id,
                                                    "API_VERSION_UNSUPPORTED",
                                                    "Expected API version " + version));
                            client->disconnectFromServer();
                            return;
                        }
                        connection->ready = true;
                        send(client,
                             QJsonObject{
                                 {"request_id", id},
                                 {"status", "success"},
                                 {"data",
                                  QJsonObject{
                                      {"api_version", version},
                                      {"engine_instance_id", engine_id},
                                      {"pid", QString::number(QCoreApplication::applicationPid())},
                                      {"storage_mode", core.durable() ? "sqlite" : "memory"},
                                      {"durable", core.durable()}}}});
                    } else if (!connection->ready) {
                        send(client,
                             qcae::ipc::failure(
                                 id, "HANDSHAKE_REQUIRED", "Perform runtime.handshake first"));
                    } else
                        send(client,
                             qcae::ipc::dispatch(
                                 core, request, caller, &codec, &codec.definition(), &selections));
                }
            };
            QObject::connect(socket, &QLocalSocket::readyRead, &app, consume);
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            if (socket->bytesAvailable())
                consume();
        }
    });
    QTextStream(stderr) << "QCAE engine ready: " << endpoint
                        << (core.durable() ? " (SQLite; explicit recovery on restart)\n"
                                           : " (memory; data lost on exit)\n");
    return app.exec();
}
