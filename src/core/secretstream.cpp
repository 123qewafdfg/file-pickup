#include "core/secretstream.h"

#include <sodium.h>

namespace cj {

namespace {

bool ensureSodium(QString *error)
{
    static const int initialized = sodium_init();
    if (initialized < 0) {
        if (error)
            *error = QStringLiteral("libsodium 初始化失败");
        return false;
    }
    return true;
}

crypto_secretstream_xchacha20poly1305_state *streamState(QByteArray *state)
{
    return reinterpret_cast<crypto_secretstream_xchacha20poly1305_state *>(state->data());
}

} // namespace

bool secretStreamInitPush(const QByteArray &key,
                          QByteArray *state,
                          QByteArray *header,
                          QString *error)
{
    if (!state || !header || key.size() != kSecretStreamKeySize) {
        if (error)
            *error = QStringLiteral("secretstream 加密参数无效");
        return false;
    }
    if (!ensureSodium(error))
        return false;

    state->resize(static_cast<qsizetype>(crypto_secretstream_xchacha20poly1305_statebytes()));
    header->resize(kSecretStreamHeaderSize);
    if (crypto_secretstream_xchacha20poly1305_init_push(
            streamState(state),
            reinterpret_cast<unsigned char *>(header->data()),
            reinterpret_cast<const unsigned char *>(key.constData())) != 0) {
        if (error)
            *error = QStringLiteral("secretstream 加密流初始化失败");
        state->clear();
        header->clear();
        return false;
    }
    return true;
}

bool secretStreamPush(QByteArray *state,
                      const QByteArray &plain,
                      const QByteArray &aad,
                      bool finalMessage,
                      QByteArray *cipher,
                      QString *error)
{
    if (!state || !cipher
        || state->size() != static_cast<qsizetype>(crypto_secretstream_xchacha20poly1305_statebytes())) {
        if (error)
            *error = QStringLiteral("secretstream 加密流状态无效");
        return false;
    }
    cipher->resize(plain.size() + kSecretStreamOverhead);
    unsigned long long written = 0;
    const unsigned char tag = finalMessage
                                  ? crypto_secretstream_xchacha20poly1305_TAG_FINAL
                                  : crypto_secretstream_xchacha20poly1305_TAG_MESSAGE;
    if (crypto_secretstream_xchacha20poly1305_push(
            streamState(state),
            reinterpret_cast<unsigned char *>(cipher->data()),
            &written,
            reinterpret_cast<const unsigned char *>(plain.constData()),
            static_cast<unsigned long long>(plain.size()),
            aad.isEmpty() ? nullptr : reinterpret_cast<const unsigned char *>(aad.constData()),
            static_cast<unsigned long long>(aad.size()),
            tag) != 0) {
        if (error)
            *error = QStringLiteral("secretstream 分片加密失败");
        cipher->clear();
        return false;
    }
    cipher->resize(static_cast<qsizetype>(written));
    return true;
}

bool secretStreamInitPull(const QByteArray &key,
                          const QByteArray &header,
                          QByteArray *state,
                          QString *error)
{
    if (!state || key.size() != kSecretStreamKeySize || header.size() != kSecretStreamHeaderSize) {
        if (error)
            *error = QStringLiteral("secretstream 解密参数无效");
        return false;
    }
    if (!ensureSodium(error))
        return false;

    state->resize(static_cast<qsizetype>(crypto_secretstream_xchacha20poly1305_statebytes()));
    if (crypto_secretstream_xchacha20poly1305_init_pull(
            streamState(state),
            reinterpret_cast<const unsigned char *>(header.constData()),
            reinterpret_cast<const unsigned char *>(key.constData())) != 0) {
        if (error)
            *error = QStringLiteral("secretstream 解密流初始化失败");
        state->clear();
        return false;
    }
    return true;
}

bool secretStreamPull(QByteArray *state,
                      const QByteArray &cipher,
                      const QByteArray &aad,
                      QByteArray *plain,
                      bool *finalMessage,
                      QString *error)
{
    if (!state || !plain || cipher.size() < kSecretStreamOverhead
        || state->size() != static_cast<qsizetype>(crypto_secretstream_xchacha20poly1305_statebytes())) {
        if (error)
            *error = QStringLiteral("secretstream 密文或状态无效");
        return false;
    }
    plain->resize(cipher.size() - kSecretStreamOverhead);
    unsigned long long written = 0;
    unsigned char tag = 0;
    if (crypto_secretstream_xchacha20poly1305_pull(
            streamState(state),
            reinterpret_cast<unsigned char *>(plain->data()),
            &written,
            &tag,
            reinterpret_cast<const unsigned char *>(cipher.constData()),
            static_cast<unsigned long long>(cipher.size()),
            aad.isEmpty() ? nullptr : reinterpret_cast<const unsigned char *>(aad.constData()),
            static_cast<unsigned long long>(aad.size())) != 0) {
        if (error)
            *error = QStringLiteral("secretstream 完整性校验失败");
        plain->clear();
        return false;
    }
    plain->resize(static_cast<qsizetype>(written));
    if (finalMessage)
        *finalMessage = tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL;
    return true;
}

} // namespace cj
