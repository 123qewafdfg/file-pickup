#pragma once

#include <QByteArray>
#include <QString>

namespace cj {

constexpr int kSecretStreamKeySize = 32;
constexpr int kSecretStreamHeaderSize = 24;
constexpr int kSecretStreamOverhead = 17;

bool secretStreamInitPush(const QByteArray &key,
                          QByteArray *state,
                          QByteArray *header,
                          QString *error = nullptr);

bool secretStreamPush(QByteArray *state,
                      const QByteArray &plain,
                      const QByteArray &aad,
                      bool finalMessage,
                      QByteArray *cipher,
                      QString *error = nullptr);

bool secretStreamInitPull(const QByteArray &key,
                          const QByteArray &header,
                          QByteArray *state,
                          QString *error = nullptr);

bool secretStreamPull(QByteArray *state,
                      const QByteArray &cipher,
                      const QByteArray &aad,
                      QByteArray *plain,
                      bool *finalMessage,
                      QString *error = nullptr);

} // namespace cj
