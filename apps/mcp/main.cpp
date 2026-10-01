#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QTextStream>

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QProcess bridge;
    bridge.setProcessChannelMode(QProcess::ForwardedChannels);
    bridge.setInputChannelMode(QProcess::ForwardedInputChannel);
    auto arguments = application.arguments();
    arguments.removeFirst();
    QString python = QStringLiteral(QCAE_MCP_PYTHON);
    const auto interpreter = arguments.indexOf("--python");
    if (interpreter >= 0) {
        if (interpreter + 1 == arguments.size()) {
            QTextStream(stderr) << "--python requires an interpreter path\n";
            return 2;
        }
        python = arguments.takeAt(interpreter + 1);
        arguments.removeAt(interpreter);
    }
    const QDir binaryDirectory(application.applicationDirPath());
    auto script = binaryDirectory.filePath("qcae_mcp_bridge.py");
    if (!QFileInfo::exists(script)) {
        script = binaryDirectory.filePath("../Resources/qcae_mcp_bridge.py");
    }
    arguments.prepend(script);
    bridge.start(python, arguments);
    if (!bridge.waitForStarted()) {
        QTextStream(stderr) << "Cannot start QCAE MCP bridge: " << bridge.errorString() << '\n';
        return 2;
    }
    bridge.waitForFinished(-1);
    return bridge.exitStatus() == QProcess::NormalExit ? bridge.exitCode() : 2;
}
