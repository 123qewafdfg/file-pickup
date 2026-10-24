#pragma once

#include <QByteArray>
#include <QString>

namespace cj {

// 本机加密存储：Windows DPAPI（当前用户作用域），
// 只用于保存登录会话这类需要重启后仍有效的数据，不存密钥、不存口令。
namespace securestore {

bool save(const QString &name, const QByteArray &plain);
QByteArray load(const QString &name);
void remove(const QString &name);
QString pathFor(const QString &name);

} // namespace securestore

} // namespace cj
