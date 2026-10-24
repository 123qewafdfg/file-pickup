#include "core/securestore.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <windows.h>
#include <wincrypt.h>

namespace cj::securestore {

namespace {

QString dirPath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(base);
    return base;
}

} // namespace

QString pathFor(const QString &name)
{
    return QDir(dirPath()).filePath(name + QStringLiteral(".dat"));
}

bool save(const QString &name, const QByteArray &plain)
{
    if (plain.isEmpty())
        return false;

    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()));
    in.cbData = static_cast<DWORD>(plain.size());

    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"cangjian", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;

    QFile file(pathFor(name));
    const bool ok = file.open(QIODevice::WriteOnly | QIODevice::Truncate);
    if (ok) {
        file.write(reinterpret_cast<const char *>(out.pbData), static_cast<qint64>(out.cbData));
        file.close();
    }
    LocalFree(out.pbData);
    return ok;
}

QByteArray load(const QString &name)
{
    QFile file(pathFor(name));
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return {};

    const QByteArray encrypted = file.readAll();
    file.close();
    if (encrypted.isEmpty())
        return {};

    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(encrypted.constData()));
    in.cbData = static_cast<DWORD>(encrypted.size());

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};

    QByteArray plain(reinterpret_cast<const char *>(out.pbData), static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return plain;
}

void remove(const QString &name)
{
    QFile::remove(pathFor(name));
}

} // namespace cj::securestore
