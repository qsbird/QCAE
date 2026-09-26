#pragma once

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>

namespace qcae::transport {
inline constexpr qint64 max_frame_bytes = 1024 * 1024;
inline QString default_endpoint() {
    const auto directory =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/run";
    if (!QDir().mkpath(directory))
        return {};
    if (!QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        return {};
    return directory + "/engine.sock";
}
} // namespace qcae::transport
