#include "qcae/local_endpoint.hpp"
#include "qcae/operations.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QTextStream>
#include <QThread>
#include <optional>

namespace {
std::optional<QJsonObject> exchange(QLocalSocket& socket, const QJsonObject& request) {
    const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (socket.write(bytes) != bytes.size())
        return std::nullopt;
    socket.flush();
    QElapsedTimer timer;
    timer.start();
    QByteArray response;
    while (timer.elapsed() < 5000) {
        response += socket.readAll();
        if (response.size() > qcae::transport::max_frame_bytes)
            return std::nullopt;
        const auto newline = response.indexOf('\n');
        if (newline >= 0) {
            QJsonParseError error;
            const auto parsed = QJsonDocument::fromJson(response.left(newline), &error);
            if (error.error != QJsonParseError::NoError || !parsed.isObject())
                return std::nullopt;
            const auto object = parsed.object();
            if (!object.value("request_id").isString() ||
                object.value("request_id") != request.value("request_id") ||
                !object.value("status").isString() ||
                !response.mid(newline + 1).trimmed().isEmpty())
                return std::nullopt;
            return object;
        }
        if (socket.state() != QLocalSocket::ConnectedState)
            return std::nullopt;
        socket.waitForReadyRead(100);
    }
    return std::nullopt;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("QCAE");
    QCommandLineParser parser;
    parser.setApplicationDescription("One QCAE JSON request on stdin; JSON response on stdout");
    parser.addHelpOption();
    parser.addOption({{"s", "socket"}, "Local endpoint path", "path"});
    parser.addOption({"no-start", "Do not automatically start the sibling qcae-engine"});
    parser.addOption({"engine", "Engine executable for automatic startup", "path"});
    parser.addOption({"workspace", "SQLite workspace for auto-started engine", "path"});
    parser.process(app);
    const auto endpoint =
        parser.isSet("socket") ? parser.value("socket") : qcae::transport::default_endpoint();
    if (endpoint.isEmpty() || !QFileInfo(endpoint).isAbsolute())
        return 3;
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return 3;
    const auto bytes = input.read(qcae::transport::max_frame_bytes + 1);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (bytes.size() > qcae::transport::max_frame_bytes ||
        error.error != QJsonParseError::NoError || !document.isObject()) {
        QTextStream(stderr) << "Expected a JSON object no larger than 1 MiB on stdin\n";
        return 3;
    }
    QLocalSocket socket;
    socket.setReadBufferSize(qcae::transport::max_frame_bytes + 1);
    socket.connectToServer(endpoint);
    if (!socket.waitForConnected(200)) {
        if (parser.isSet("no-start")) {
            QTextStream(stderr) << "Engine is not available: " << socket.errorString() << '\n';
            return 3;
        }
        auto executable = parser.isSet("engine")
                              ? parser.value("engine")
                              : QCoreApplication::applicationDirPath() + "/qcae-engine";
#ifdef Q_OS_WIN
        if (!parser.isSet("engine"))
            executable += ".exe";
#endif
        QProcess launcher;
        launcher.setProgram(executable);
        QStringList engine_args{"--socket", endpoint};
        if (parser.isSet("workspace"))
            engine_args << "--workspace" << parser.value("workspace");
        launcher.setArguments(engine_args);
        launcher.setStandardInputFile(QProcess::nullDevice());
        launcher.setStandardOutputFile(QProcess::nullDevice());
        launcher.setStandardErrorFile(QProcess::nullDevice());
        if (!QFileInfo(executable).isExecutable() || !launcher.startDetached()) {
            QTextStream(stderr) << "Unable to start engine\n";
            return 3;
        }
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000 && socket.state() != QLocalSocket::ConnectedState) {
            socket.abort();
            socket.connectToServer(endpoint);
            if (!socket.waitForConnected(100))
                QThread::msleep(20);
        }
        if (socket.state() != QLocalSocket::ConnectedState)
            return 3;
    }
    const auto version = QString::fromUtf8(qcae::api_version.data(),
                                           static_cast<qsizetype>(qcae::api_version.size()));
    const auto handshake = exchange(socket,
                                    {{"api_version", version},
                                     {"request_id", "cli-handshake"},
                                     {"operation", "runtime.handshake"}});
    if (!handshake || handshake->value("status").toString() != "success" ||
        handshake->value("data").toObject().value("api_version").toString() != version) {
        QTextStream(stderr) << "Incompatible engine handshake\n";
        return 3;
    }
    const auto response = exchange(socket, document.object());
    if (!response) {
        QTextStream(stderr) << "Response unavailable; query the original operation or retry with "
                               "the same idempotency key\n";
        return 3;
    }
    QTextStream(stdout) << QJsonDocument(*response).toJson(QJsonDocument::Compact) << '\n';
    return response->value("status").toString() == "success" ? 0 : 2;
}
