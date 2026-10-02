#include "qcae/engine_host.hpp"
#include "qcae/event_stream.hpp"
#include "qcae/render_service.hpp"
#include "qcae/ipc_api.hpp"
#include "qcae/query.hpp"
#include "qcae/typed_host.hpp"
#ifdef QCAE_HAS_SQLITE
#include "qcae/sqlite_store.hpp"
#include "qcae/legacy_migration.hpp"
#include "qcae/records.hpp"
#endif
#include "qcae/local_endpoint.hpp"
#include "qcae/json_ledger.hpp"
#include "qcae/operations.hpp"
#include "qcae/operation_ledger.hpp"
#ifdef QCAE_HAS_SOLVER_LOCAL
#include "qcae/solver_contribution.hpp"
#endif

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QTextStream>
#include <QUuid>
#include <QTimer>
#include <map>
#include <filesystem>
#include <memory>
#include <algorithm>

namespace {
struct Connection {
    QLocalSocket* socket;
    QByteArray buffer;
    bool ready{false};
    QString id;
};
void activate_pending_observation(Connection& connection) noexcept {
    const auto pending = qcae::ledger::pending();
    if (qcae::ledger::current() || !pending)
        return;
    try {
        // Test-only observation needs the opcode before normal readAll/copy
        // accounting. Peek does not consume or validate a business command.
        // Its temporary parser/buffer is observation control overhead.
        const auto preview =
            connection.buffer + connection.socket->peek(qcae::transport::max_frame_bytes + 1);
        qsizetype start{};
        while (true) {
            const auto newline = preview.indexOf('\n', start);
            if (newline < 0)
                return;
            QJsonParseError error;
            const auto parsed =
                QJsonDocument::fromJson(preview.sliced(start, newline - start), &error);
            if (error.error == QJsonParseError::NoError && parsed.isObject() &&
                parsed.object().value("operation").toStringView() ==
                    QAnyStringView(QLatin1StringView(pending->trigger_operation().data(),
                                                     pending->trigger_operation().size()))) {
                if (qcae::ledger::activate_pending_for(pending->trigger_operation()) &&
                    (start != 0 || !connection.buffer.isEmpty())) {
                    // A split/mixed arm boundary cannot claim complete byte
                    // coverage. Preserve actual counters and the failed trace.
                    qcae::ledger::unknown(qcae::ledger::Stage::socket_receive,
                                          qcae::ledger::Metric::model_copy_bytes);
                    qcae::ledger::unknown(qcae::ledger::Stage::socket_receive,
                                          qcae::ledger::Metric::library_internal_copy_bytes);
                }
                return;
            }
            start = newline + 1;
        }
    } catch (...) {
        pending->unknown(qcae::ledger::Stage::socket_receive,
                         qcae::ledger::Metric::model_copy_bytes);
        pending->unknown(qcae::ledger::Stage::socket_receive,
                         qcae::ledger::Metric::library_internal_copy_bytes);
    }
}
qsizetype utf8_size(const QString& text,
                    qcae::ledger::Stage stage = qcae::ledger::Stage::socket_receive) {
    // Normal ASCII identifiers can be validated directly through their owned
    // UTF-16 view. Preserve Qt's conversion/error behavior for other Unicode.
    if (std::all_of(
            text.constBegin(), text.constEnd(), [](QChar value) { return value.unicode() < 0x80; }))
        return text.size();
    const auto bytes = text.toUtf8();
    qcae::ledger::add(stage, qcae::ledger::Metric::library_internal_copy_bytes, bytes.size());
    return bytes.size();
}
void send(QLocalSocket* socket,
          const QJsonObject& response,
          const std::optional<QByteArray>& retained_frame = {}) {
    const auto encode = [](const QJsonObject& object) {
        return qcae::transport::json_ledger::compact_frame(object);
    };
    auto actual_response = response; // Immutable Qt container reference only.
    auto bytes = retained_frame ? *retained_frame : encode(actual_response);
    if (bytes.size() > qcae::transport::max_frame_bytes) {
        auto id = qcae::transport::json_ledger::string(response.value("request_id"),
                                                       qcae::ledger::Stage::socket_send);
        if (utf8_size(id, qcae::ledger::Stage::socket_send) > 128)
            id.clear();
        actual_response =
            qcae::ipc::failure(id, "RESOURCE_LIMIT", "Response exceeds the M0 frame limit");
        bytes = encode(actual_response);
    }
    if (socket->bytesToWrite() + bytes.size() > qcae::transport::max_frame_bytes * 2) {
        socket->abort();
        return;
    }
    qcae::ledger::cover(qcae::ledger::Stage::socket_send);
    const auto accepted = socket->write(bytes);
    if (accepted > 0) {
        qcae::transport::json_ledger::frame(qcae::ledger::Stage::socket_send,
                                            actual_response,
                                            static_cast<std::uint64_t>(accepted));
        qcae::ledger::add(qcae::ledger::Stage::socket_send,
                          qcae::ledger::Metric::socket_bytes,
                          static_cast<std::uint64_t>(accepted));
        qcae::ledger::add(qcae::ledger::Stage::socket_send,
                          qcae::ledger::Metric::model_copy_bytes,
                          static_cast<std::uint64_t>(accepted));
    }
}
} // namespace

namespace {
int run_engine(int argc,
               char** argv,
               std::span<const qcae::ipc::EngineContribution> contributions,
               qcae::Limits limits,
               bool production_defaults) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("QCAE");
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "QCAE local engine; optional SQLite workspace and shared model services");
    parser.addHelpOption();
    parser.addOption({{"s", "socket"}, "Local endpoint path", "path"});
    parser.addOption({"workspace", "SQLite working recovery database", "path"});
    parser.addOption(
        {"migrate-from", "Read a legacy workspace into a new --workspace destination", "path"});
    if (production_defaults)
        parser.addOption(
            {"solver-config", "Trusted local solver configuration (absolute JSON path)", "path"});
    parser.process(app);
    if (parser.isSet("migrate-from") && !parser.isSet("workspace")) {
        QTextStream(stderr) << "--migrate-from requires a new --workspace destination\n";
        return 2;
    }
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
    std::shared_ptr<qcae::IWorkspaceStore> store;
    std::unique_ptr<qcae::MemoryApplication> application;
    std::unique_ptr<qcae::ipc::TypedHost> typed;
    qcae::ipc::EngineAssembly assembly;
    try {
        std::vector<qcae::ipc::EngineContribution> defaults;
        if (production_defaults) {
            if (parser.isSet("solver-config")) {
#ifdef QCAE_HAS_SOLVER_LOCAL
                const auto config = qcae::ipc::load_local_solver_configuration(
                    std::filesystem::path(parser.value("solver-config").toStdString()));
                if (!config.ok())
                    throw std::runtime_error(config.error->message);
                defaults = qcae::ipc::default_engine_contributions(*config.value);
#else
                throw std::runtime_error("This build does not include local solver support");
#endif
            } else {
                defaults = qcae::ipc::default_engine_contributions();
            }
            contributions = defaults;
        }
        assembly = qcae::ipc::assemble_engine(contributions);
        // Capture the selected ports by value: application/task callbacks can outlive assembly.
        const auto supports_profile =
            [binding = assembly.profile_codec](const qcae::ProfileRef& profile) {
                return binding && profile == binding->profile->definition().reference;
            };
        if (parser.isSet("workspace")) {
#ifdef QCAE_HAS_SQLITE
            std::optional<qcae::LoadedRows> migration;
            if (parser.isSet("migrate-from")) {
                const auto destination = parser.value("workspace");
                if (!QFileInfo(destination).isAbsolute() || QFileInfo::exists(destination) ||
                    QFileInfo(destination).isSymLink())
                    throw std::runtime_error("Migration requires a new absolute destination path");
                migration =
                    qcae::migrate_legacy_workspace(qcae::read_legacy_workspace_readonly(
                                                       parser.value("migrate-from").toStdString()),
                                                   assembly.records);
            }
            auto sqlite = std::make_shared<qcae::SqliteWorkspaceStore>(
                parser.value("workspace").toStdString());
            if (migration) {
                // The source was decoded read-only; only this fresh destination is published.
                qcae::StoreBatch batch{0, "legacy-migration", {}};
                for (const auto& row : migration->rows)
                    batch.mutations.push_back({row.key, row.value});
                sqlite->commit_rows(batch);
            }
            store = std::move(sqlite);
#else
            throw std::runtime_error("This build does not include SQLite persistence");
#endif
        }
        auto owned_rows = std::move(assembly.owned_rows);
        owned_rows.push_back(qcae::task_row_handler());
        application = std::make_unique<qcae::MemoryApplication>(
            limits, store, supports_profile, std::move(owned_rows), assembly.records);
        typed = std::make_unique<qcae::ipc::TypedHost>(application->record_application(),
                                                       supports_profile,
                                                       std::move(assembly.operations),
                                                       std::move(assembly.publisher_factory));
    } catch (const std::exception& error) {
        QTextStream(stderr) << "Workspace initialization failed: " << error.what() << '\n';
        return 5;
    }
    auto& core = *application;
    qcae::SelectionService selections;
    // M0 is a single local OS-user host. Request JSON cannot choose this identity.
    const qcae::Caller caller{"local-user"};
    const auto engine_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    qcae::ipc::ResourceStore resources;
    qcae::ipc::RenderService renders(
        core.record_application(), selections, resources, std::move(assembly.render));
    qcae::ipc::EventStream events(core.record_application(), engine_id);
    std::map<QString, std::weak_ptr<Connection>> connections;
    const auto publish_events = [&] {
        events.refresh();
        for (auto it = connections.begin(); it != connections.end();) {
            if (auto connection = it->second.lock()) {
                ++it; // send may synchronously disconnect and erase this connection.
                if (connection->ready &&
                    connection->socket->state() == QLocalSocket::ConnectedState) {
                    for (const auto& event : events.drain(connection->id)) {
                        const auto object = event.toObject();
                        send(connection->socket, object, events.encoded_frame(object));
                    }
                }
            } else
                it = connections.erase(it);
        }
    };
    QTimer event_timer;
    event_timer.setInterval(50);
    QObject::connect(&event_timer, &QTimer::timeout, &app, publish_events);
    event_timer.start();
    const auto version = QString::fromUtf8(qcae::api_version.data(),
                                           static_cast<qsizetype>(qcae::api_version.size()));
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        while (auto* socket = server.nextPendingConnection()) {
            socket->setReadBufferSize(qcae::transport::max_frame_bytes + 1);
            auto connection = std::make_shared<Connection>(
                Connection{socket, {}, false, QUuid::createUuid().toString(QUuid::WithoutBraces)});
            connections.emplace(connection->id, connection);
            auto consume = [&, connection] {
                auto* client = connection->socket;
                activate_pending_observation(*connection);
                const auto received = client->readAll();
                qcae::ledger::cover(qcae::ledger::Stage::socket_receive);
                qcae::ledger::add(qcae::ledger::Stage::socket_receive,
                                  qcae::ledger::Metric::socket_bytes,
                                  static_cast<std::uint64_t>(received.size()));
                // readAll and append each materialize the received payload. Growth may also
                // copy the existing buffer; account for that payload, not allocator capacity.
                auto copied = 2 * static_cast<std::uint64_t>(received.size());
                if (connection->buffer.size() + received.size() > connection->buffer.capacity())
                    copied += static_cast<std::uint64_t>(connection->buffer.size());
                qcae::ledger::add(qcae::ledger::Stage::socket_receive,
                                  qcae::ledger::Metric::model_copy_bytes,
                                  copied);
                qcae::transport::json_ledger::socket_read(received.size());
                connection->buffer += received;
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
                    const auto line = QByteArrayView(connection->buffer).first(newline);
                    qcae::ledger::add(qcae::ledger::Stage::socket_receive,
                                      qcae::ledger::Metric::decoded_bytes,
                                      static_cast<std::uint64_t>(line.size()));
                    qcae::transport::json_ledger::decoding(line);
                    QJsonParseError parse_error;
                    const auto document = QJsonValue::fromJson(line, &parse_error);
                    // Parse/decode owns its strings before input-prefix mutation.
                    qcae::transport::json_ledger::remove_prefix(connection->buffer, newline + 1);
                    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
                        send(client,
                             qcae::ipc::failure(
                                 {}, "INVALID_JSON", "Expected one JSON object per line"));
                        continue;
                    }
                    const auto request = document.toObject();
                    if (const auto pending = qcae::ledger::pending();
                        pending &&
                        request.value("operation").toStringView() ==
                            QAnyStringView(
                                QLatin1StringView(pending->trigger_operation().data(),
                                                  pending->trigger_operation().size())) &&
                        qcae::ledger::activate_pending_for(pending->trigger_operation())) {
                        // New bytes can arrive between peek and readAll. The
                        // command still executes normally, but its unobserved
                        // receive prefix makes this sample incomplete.
                        for (const auto metric :
                             {qcae::ledger::Metric::socket_bytes,
                              qcae::ledger::Metric::model_copy_bytes,
                              qcae::ledger::Metric::library_internal_copy_bytes})
                            qcae::ledger::unknown(qcae::ledger::Stage::socket_receive, metric);
                    }
                    qcae::transport::json_ledger::frame(qcae::ledger::Stage::socket_receive,
                                                        request,
                                                        static_cast<std::uint64_t>(newline + 1));
                    qcae::ledger::cover(qcae::ledger::Stage::command);
                    qcae::ledger::add(qcae::ledger::Stage::command,
                                      qcae::ledger::Metric::decoded_bytes,
                                      static_cast<std::uint64_t>(newline));
                    const auto id =
                        qcae::transport::json_ledger::string(request.value("request_id"));
                    if (id.isEmpty() || utf8_size(id) > 128) {
                        send(client,
                             qcae::ipc::failure({},
                                                "INVALID_INPUT",
                                                "request_id must contain 1..128 UTF-8 bytes"));
                        continue;
                    }
                    const auto operation =
                        qcae::transport::json_ledger::string(request.value("operation"));
                    if (operation == "runtime.handshake") {
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
                                      {"durable", core.durable()},
                                      {"capabilities",
                                       QJsonObject{{"resources_version", 1},
                                                   {"events_version", 1},
                                                   {"render_inline_empty_version", 1},
                                                   {"render_wire_version", 3},
                                                   {"events_document_summary_version", 1},
                                                   {"render_model_rebase_version", 1},
                                                   {"render_changed_rows_version", 1}}}}}});
                    } else if (!connection->ready) {
                        send(client,
                             qcae::ipc::failure(
                                 id, "HANDSHAKE_REQUIRED", "Perform runtime.handshake first"));
                    } else if (events.supports(operation)) {
                        events.refresh();
                        send(client, events.dispatch(request, connection->id));
                    } else if (renders.supports(operation)) {
                        send(client, renders.dispatch(request, caller));
                    } else {
                        auto response = qcae::ipc::dispatch(core,
                                                            request,
                                                            caller,
                                                            assembly.model_codec(),
                                                            assembly.profile_definition(),
                                                            &selections,
                                                            typed.get(),
                                                            true);
                        if (operation == "capabilities.list" &&
                            response.value("status").toString() == "success") {
                            auto data = response.value("data").toObject();
                            data.insert("package_contributions_version", 1);
                            data.insert("package_contributions", assembly.catalog->describe());
                            response.insert("data", data);
                        }
                        send(client, response);
                    }
                    publish_events();
                }
            };
            QObject::connect(socket, &QLocalSocket::readyRead, &app, consume);
            QObject::connect(socket, &QLocalSocket::disconnected, &app, [&, id = connection->id] {
                events.unsubscribe(id);
                connections.erase(id);
            });
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
} // namespace
int qcae_run_engine(int argc, char** argv) {
    return run_engine(argc, argv, {}, {}, true);
}
int qcae_run_engine(int argc,
                    char** argv,
                    std::span<const qcae::ipc::EngineContribution> contributions,
                    qcae::Limits limits) {
    return run_engine(argc, argv, contributions, limits, false);
}
