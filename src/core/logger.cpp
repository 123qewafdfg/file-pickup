#include "core/logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace cj::logger {

namespace {
QString logFile()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(root);
    return QDir(root).filePath(QStringLiteral("pipeline.log"));
}
} // namespace

void event(const QString &stage, const QString &detail)
{
    QFile file(logFile());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    QString line = QStringLiteral("[%1] %2").arg(stamp, stage);
    if (!detail.isEmpty())
        line += QStringLiteral(" · ") + detail;
    line += QLatin1Char('\n');
    file.write(line.toUtf8());
    file.flush();
}

QString path()
{
    return logFile();
}

void reset()
{
    QFile file(logFile());
    QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Append;
    if (file.exists() && file.size() > 4 * 1024 * 1024)
        mode = QIODevice::WriteOnly | QIODevice::Truncate;
    if (file.open(mode)) {
        const QString stamp = QDateTime::currentDateTime().toString(
            QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
        file.write(QStringLiteral("\n=== 文件取件 PC 发放日志 · %1 ===\n").arg(stamp).toUtf8());
    }
}

} // namespace cj::logger
