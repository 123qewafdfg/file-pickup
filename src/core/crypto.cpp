#include "core/crypto.h"

#include <QCryptographicHash>
#include <QFile>
#include <QRandomGenerator>

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace cj {

namespace {

QString cngError(const char *where, NTSTATUS status)
{
    return QStringLiteral("%1 失败（NTSTATUS 0x%2）")
        .arg(QString::fromLatin1(where))
        .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
}

struct AesGcmContext {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;

    AesGcmContext() = default;
    AesGcmContext(const AesGcmContext &) = delete;
    AesGcmContext &operator=(const AesGcmContext &) = delete;

    ~AesGcmContext()
    {
        if (key)
            BCryptDestroyKey(key);
        if (alg)
            BCryptCloseAlgorithmProvider(alg, 0);
    }

    bool init(const QByteArray &keyBytes, QString *error)
    {
        NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
        if (st != STATUS_SUCCESS) {
            if (error)
                *error = cngError("BCryptOpenAlgorithmProvider", st);
            return false;
        }
        st = BCryptSetProperty(alg,
                               BCRYPT_CHAINING_MODE,
                               reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_GCM)),
                               static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)),
                               0);
        if (st != STATUS_SUCCESS) {
            if (error)
                *error = cngError("BCryptSetProperty(GCM)", st);
            return false;
        }
        st = BCryptGenerateSymmetricKey(alg,
                                        &key,
                                        nullptr,
                                        0,
                                        reinterpret_cast<PUCHAR>(const_cast<char *>(keyBytes.constData())),
                                        static_cast<ULONG>(keyBytes.size()),
                                        0);
        if (st != STATUS_SUCCESS) {
            if (error)
                *error = cngError("BCryptGenerateSymmetricKey", st);
            return false;
        }
        return true;
    }
};

// 与安卓一致的 12 字节 nonce / 16 字节 tag
constexpr int kIvSize = 12;
constexpr int kTagSize = 16;

QByteArray aesEcbCrypt(const QByteArray &keyBytes,
                       const QByteArray &input,
                       bool encrypt,
                       QString *error)
{
    if (keyBytes.size() != 16 && keyBytes.size() != 24 && keyBytes.size() != 32) {
        if (error)
            *error = QStringLiteral("AES-ECB 密钥长度无效");
        return {};
    }
    if (!encrypt && (input.isEmpty() || input.size() % 16 != 0)) {
        if (error)
            *error = QStringLiteral("AES-ECB 密文长度无效");
        return {};
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (st == STATUS_SUCCESS) {
        st = BCryptSetProperty(alg,
                               BCRYPT_CHAINING_MODE,
                               reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(BCRYPT_CHAIN_MODE_ECB)),
                               static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_ECB)),
                               0);
    }
    if (st == STATUS_SUCCESS) {
        st = BCryptGenerateSymmetricKey(
            alg,
            &key,
            nullptr,
            0,
            reinterpret_cast<PUCHAR>(const_cast<char *>(keyBytes.constData())),
            static_cast<ULONG>(keyBytes.size()),
            0);
    }
    if (st != STATUS_SUCCESS) {
        if (key)
            BCryptDestroyKey(key);
        if (alg)
            BCryptCloseAlgorithmProvider(alg, 0);
        if (error)
            *error = cngError("AES-ECB 初始化", st);
        return {};
    }

    ULONG required = 0;
    PUCHAR source = input.isEmpty()
                        ? nullptr
                        : reinterpret_cast<PUCHAR>(const_cast<char *>(input.constData()));
    const ULONG sourceSize = static_cast<ULONG>(input.size());
    const ULONG flags = BCRYPT_BLOCK_PADDING;
    st = encrypt ? BCryptEncrypt(key, source, sourceSize, nullptr, nullptr, 0,
                                 nullptr, 0, &required, flags)
                 : BCryptDecrypt(key, source, sourceSize, nullptr, nullptr, 0,
                                 nullptr, 0, &required, flags);
    QByteArray output;
    if (st == STATUS_SUCCESS) {
        output.resize(static_cast<qsizetype>(required));
        ULONG written = 0;
        st = encrypt ? BCryptEncrypt(key, source, sourceSize, nullptr, nullptr, 0,
                                     reinterpret_cast<PUCHAR>(output.data()), required, &written, flags)
                     : BCryptDecrypt(key, source, sourceSize, nullptr, nullptr, 0,
                                     reinterpret_cast<PUCHAR>(output.data()), required, &written, flags);
        if (st == STATUS_SUCCESS)
            output.resize(static_cast<qsizetype>(written));
    }

    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != STATUS_SUCCESS) {
        if (error)
            *error = cngError(encrypt ? "AES-ECB 加密" : "AES-ECB 解密", st);
        return {};
    }
    return output;
}

} // namespace

QByteArray randomBytes(int count)
{
    QByteArray out(count, Qt::Uninitialized);
    if (count <= 0)
        return out;
    QRandomGenerator *rng = QRandomGenerator::system();
    // fillRange 需要 quint32 目标，这里逐 4 字节填充后再截断
    int filled = 0;
    while (filled < count) {
        quint32 word = rng->generate();
        const char *src = reinterpret_cast<const char *>(&word);
        for (int i = 0; i < 4 && filled < count; ++i)
            out[filled++] = src[i];
    }
    return out;
}

QByteArray sha256(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QByteArray sha256File(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法读取文件：%1").arg(path);
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        if (error)
            *error = QStringLiteral("读取文件失败：%1").arg(path);
        return {};
    }
    return hash.result();
}

QByteArray aesGcmEncrypt(const QByteArray &key,
                         const QByteArray &iv,
                         const QByteArray &plain,
                         const QByteArray &aad,
                         QString *error)
{
    if (key.size() != 32) {
        if (error)
            *error = QStringLiteral("AES-256 需要 32 字节密钥");
        return {};
    }
    if (iv.size() != kIvSize) {
        if (error)
            *error = QStringLiteral("GCM nonce 必须为 12 字节");
        return {};
    }

    AesGcmContext ctx;
    if (!ctx.init(key, error))
        return {};

    // 本机 bcrypt 在 pbTag == nullptr（把标签追加到输出尾部）时会崩，
    // 因此统一用独立 tag 缓冲区，再由调用方拼接，语义与安卓一致。
    QByteArray out(plain.size(), Qt::Uninitialized);
    QByteArray tag(kTagSize, Qt::Uninitialized);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char *>(iv.constData()));
    info.cbNonce = static_cast<ULONG>(iv.size());
    info.pbAuthData = aad.isEmpty() ? nullptr
                                    : reinterpret_cast<PUCHAR>(const_cast<char *>(aad.constData()));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
    info.cbTag = kTagSize;

    ULONG written = 0;
    static char emptyProbe = 0;
    PUCHAR input = plain.isEmpty()
        ? reinterpret_cast<PUCHAR>(&emptyProbe)
        : reinterpret_cast<PUCHAR>(const_cast<char *>(plain.constData()));
    static char emptyOut = 0;
    PUCHAR output = plain.isEmpty() ? reinterpret_cast<PUCHAR>(&emptyOut)
                                    : reinterpret_cast<PUCHAR>(out.data());
    NTSTATUS st = BCryptEncrypt(ctx.key,
                                input,
                                static_cast<ULONG>(plain.size()),
                                &info,
                                nullptr,
                                0,
                                output,
                                static_cast<ULONG>(plain.size()),
                                &written,
                                0);
    if (st != STATUS_SUCCESS) {
        if (error)
            *error = cngError("BCryptEncrypt", st);
        return {};
    }
    out.resize(static_cast<int>(written));
    return out + tag;
}

QByteArray aesGcmDecrypt(const QByteArray &key,
                         const QByteArray &iv,
                         const QByteArray &cipherWithTag,
                         const QByteArray &aad,
                         QString *error)
{
    if (key.size() != 32) {
        if (error)
            *error = QStringLiteral("AES-256 需要 32 字节密钥");
        return {};
    }
    if (iv.size() != kIvSize) {
        if (error)
            *error = QStringLiteral("GCM nonce 必须为 12 字节");
        return {};
    }
    if (cipherWithTag.size() < kTagSize) {
        if (error)
            *error = QStringLiteral("密文长度不足");
        return {};
    }

    AesGcmContext ctx;
    if (!ctx.init(key, error))
        return {};

    // 与加密对称：密文尾部 16 字节是独立 tag 缓冲区，需拆出来交给 CNG
    const QByteArray cipher = cipherWithTag.left(cipherWithTag.size() - kTagSize);
    QByteArray tag = cipherWithTag.right(kTagSize);

    QByteArray out(cipher.size(), Qt::Uninitialized);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char *>(iv.constData()));
    info.cbNonce = static_cast<ULONG>(iv.size());
    info.pbAuthData = aad.isEmpty() ? nullptr
                                    : reinterpret_cast<PUCHAR>(const_cast<char *>(aad.constData()));
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
    info.cbTag = kTagSize;

    ULONG written = 0;
    static char emptyProbe = 0;
    static char emptyOut = 0;
    PUCHAR input = cipher.isEmpty()
        ? reinterpret_cast<PUCHAR>(&emptyProbe)
        : reinterpret_cast<PUCHAR>(const_cast<char *>(cipher.constData()));
    PUCHAR output = cipher.isEmpty() ? reinterpret_cast<PUCHAR>(&emptyOut)
                                     : reinterpret_cast<PUCHAR>(out.data());
    NTSTATUS st = BCryptDecrypt(ctx.key,
                                input,
                                static_cast<ULONG>(cipher.size()),
                                &info,
                                nullptr,
                                0,
                                output,
                                static_cast<ULONG>(cipher.size()),
                                &written,
                                0);
    if (st != STATUS_SUCCESS) {
        if (error)
            *error = QStringLiteral("解密失败：密钥或完整性校验不通过");
        return {};
    }
    out.resize(static_cast<int>(written));
    return out;
}

QByteArray aesEcbEncryptPkcs7(const QByteArray &key,
                              const QByteArray &plain,
                              QString *error)
{
    return aesEcbCrypt(key, plain, true, error);
}

QByteArray aesEcbDecryptPkcs7(const QByteArray &key,
                              const QByteArray &cipher,
                              QString *error)
{
    return aesEcbCrypt(key, cipher, false, error);
}

namespace {
std::array<quint32, 256> makeCrcTable()
{
    std::array<quint32, 256> table{};
    for (quint32 i = 0; i < 256; ++i) {
        quint32 c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}
} // namespace

quint32 crc32Update(quint32 seed, const char *data, qsizetype size)
{
    static const std::array<quint32, 256> table = makeCrcTable();
    quint32 c = seed ^ 0xFFFFFFFFu;
    for (qsizetype i = 0; i < size; ++i)
        c = table[(c ^ static_cast<quint8>(data[i])) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

quint32 crc32(const QByteArray &data)
{
    return crc32Update(0, data.constData(), data.size());
}

QString base64Url(const QByteArray &data)
{
    return QString::fromLatin1(
        data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray fromBase64Url(const QString &text)
{
    return QByteArray::fromBase64(text.toLatin1(),
                                  QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QString hex(const QByteArray &data)
{
    return QString::fromLatin1(data.toHex());
}

QByteArray fromHex(const QString &text)
{
    return QByteArray::fromHex(text.toLatin1());
}

} // namespace cj
