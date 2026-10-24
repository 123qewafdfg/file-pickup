// 藏笺 PC 核心自测：不依赖界面，验证密码学原语、载体格式与打包恢复。
// 用法：cangjian_tests.exe

#include "core/backend.h"
#include "core/crypto.h"
#include "core/creator.h"
#include "core/partition.h"
#include "core/payload.h"
#include "core/restorer.h"
#include "core/secretstream.h"
#include "core/tmshare.h"

#include "compress_tests.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QDir>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTextStream>

static int g_failed = 0;
static int g_passed = 0;

static void check(bool condition, const QString &label, const QString &detail = QString())
{
    QTextStream out(stdout);
    if (condition) {
        ++g_passed;
        out << "  PASS  " << label << "\n";
    } else {
        ++g_failed;
        out << "  FAIL  " << label;
        if (!detail.isEmpty())
            out << "  ->  " << detail;
        out << "\n";
    }
    out.flush();
}

static QByteArray readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

static void writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(data);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    out << "== 藏笺 PC 核心自测 ==\n";

    // ---- 1. SHA-256 ----
    check(cj::hex(cj::sha256(QByteArray())).startsWith(QStringLiteral("e3b0c44298fc1c14")),
          QStringLiteral("SHA-256 空串向量"));

    // ---- 2. CRC32 ----
    check(cj::crc32(QByteArrayLiteral("123456789")) == 0xCBF43926u,
          QStringLiteral("CRC32 IEEE 向量"));

    // ---- 3. Base64URL 往返 ----
    {
        const QByteArray raw = cj::randomBytes(37);
        const QString text = cj::base64Url(raw);
        check(!text.contains(QLatin1Char('=')) && !text.contains(QLatin1Char('+'))
                  && !text.contains(QLatin1Char('/')),
              QStringLiteral("Base64URL 无填充、无 +/"));
        check(cj::fromBase64Url(text) == raw, QStringLiteral("Base64URL 编解码往返"));
    }

    // ---- 4. AES-256-GCM 标准向量（K=0^256, IV=0^96, 空明文、空 AAD）----
    {
        const QByteArray key(32, '\0');
        const QByteArray iv(12, '\0');
        QString error;
        const QByteArray tag = cj::aesGcmEncrypt(key, iv, QByteArray(), QByteArray(), &error);
        check(cj::hex(tag) == QStringLiteral("530f8afbc74536b9a963b4f1c4cb738b"),
              QStringLiteral("AES-256-GCM 标准向量"),
              QStringLiteral("%1 / %2").arg(cj::hex(tag), error));
    }

    // ---- 5. AES-GCM 往返与篡改检测 ----
    {
        const QByteArray key = cj::randomBytes(32);
        const QByteArray iv = cj::randomBytes(12);
        const QByteArray aad = cj::dataAad(cj::randomBytes(16), 0, 1);
        QString error;
        QByteArray cipher = cj::aesGcmEncrypt(key, iv, QByteArrayLiteral("cangjian"), aad, &error);
        check(cj::aesGcmDecrypt(key, iv, cipher, aad, &error) == QByteArrayLiteral("cangjian"),
              QStringLiteral("AES-GCM 往返"));
        cipher[0] = static_cast<char>(cipher.at(0) ^ 0x01);
        check(cj::aesGcmDecrypt(key, iv, cipher, aad, nullptr).isEmpty(),
              QStringLiteral("AES-GCM 篡改被拒绝"));
    }

    // ---- 6. AAD 长度与逐字节内容（必须与安卓一致）----
    {
        const QByteArray uuid(16, '\x11');
        const QByteArray dataAad = cj::dataAad(uuid, 3, 7);
        check(dataAad.size() == 36, QStringLiteral("DATA AAD 为 36 字节"));
        const QString expected = QStringLiteral("544d5348415245313a444154")        // "TMSHARE1:DATA"
                                 + QStringLiteral("11111111111111111111111111111111") // uuid
                                 + QStringLiteral("0003")                            // partIndex=3
                                 + QStringLiteral("0007")                            // partCount=7
                                 + QStringLiteral("00000000");                       // 隐式补零
        check(cj::hex(dataAad) == expected, QStringLiteral("DATA AAD 逐字节一致"),
              cj::hex(dataAad));

        const QByteArray m = cj::metaAad(uuid);
        check(m.size() == 29, QStringLiteral("META AAD 为 29 字节"));
        check(cj::hex(m) == QStringLiteral("544d5348415245313a4d455441")
                                + QStringLiteral("11111111111111111111111111111111"),
              QStringLiteral("META AAD 逐字节一致"),
              cj::hex(m));
    }

    // ---- libsodium secretstream：连续消息、FINAL 与篡改检测 ----
    {
        const QByteArray key = cj::randomBytes(cj::kSecretStreamKeySize);
        QByteArray pushState;
        QByteArray header;
        QString error;
        const bool initialized = cj::secretStreamInitPush(key, &pushState, &header, &error);
        check(initialized && header.size() == cj::kSecretStreamHeaderSize,
              QStringLiteral("secretstream 加密流初始化"), error);
        QByteArray firstCipher;
        QByteArray finalCipher;
        if (initialized) {
            check(cj::secretStreamPush(&pushState, QByteArrayLiteral("first"),
                                       QByteArrayLiteral("aad-0"), false, &firstCipher, &error),
                  QStringLiteral("secretstream 普通分片加密"), error);
            check(cj::secretStreamPush(&pushState, QByteArrayLiteral("final"),
                                       QByteArrayLiteral("aad-1"), true, &finalCipher, &error),
                  QStringLiteral("secretstream 最终分片加密"), error);
        }
        QByteArray pullState;
        bool finalTag = false;
        check(cj::secretStreamInitPull(key, header, &pullState, &error),
              QStringLiteral("secretstream 解密流初始化"), error);
        QByteArray plain;
        check(cj::secretStreamPull(&pullState, firstCipher, QByteArrayLiteral("aad-0"),
                                   &plain, &finalTag, &error)
                  && plain == QByteArrayLiteral("first") && !finalTag,
              QStringLiteral("secretstream 普通分片解密"), error);
        check(cj::secretStreamPull(&pullState, finalCipher, QByteArrayLiteral("aad-1"),
                                   &plain, &finalTag, &error)
                  && plain == QByteArrayLiteral("final") && finalTag,
              QStringLiteral("secretstream FINAL 被识别"), error);

        QByteArray tampered = firstCipher;
        if (!tampered.isEmpty())
            tampered[0] = static_cast<char>(tampered.at(0) ^ 1);
        check(cj::secretStreamInitPull(key, header, &pullState, &error)
                  && !cj::secretStreamPull(&pullState, tampered, QByteArrayLiteral("aad-0"),
                                           &plain, &finalTag, nullptr),
              QStringLiteral("secretstream 篡改被拒绝"));
    }

    // ---- 小飞机分享接口兼容加密：与 feijipan-api 固定向量一致 ----
    {
        QString error;
        check(cj::feijiiEncryptParameter(QStringLiteral("1700000000000"), &error)
                  == QStringLiteral("fff653ea6768a22eab8cea3d83a8693c"),
              QStringLiteral("小飞机 AES-ECB 参数向量"), error);
    }

    // ---- 7. 索引构造 / 解析 ----
    {
        cj::CarrierIndex index;
        index.uuid = cj::randomBytes(16);
        index.partIndex = 2;
        index.partCount = 5;
        index.cipherLen = 1234;
        index.metaLen = 96;
        index.imageLen = cj::whiteQqCarrierPng().size();
        index.iv = cj::randomBytes(12);
        index.metaIv = cj::randomBytes(12);
        index.cipherHash = cj::sha256(QByteArrayLiteral("x"));
        const QByteArray raw = cj::buildIndex(index);
        check(raw.size() == cj::kIndexSize, QStringLiteral("索引长度为 112 字节"));
        check(raw.left(8) == QByteArrayLiteral("TMSHARE1"), QStringLiteral("索引标记正确"));

        QTemporaryDir dir;
        const QString path = QDir(dir.path()).filePath(QStringLiteral("index.bin"));
        {
            QFile file(path);
            file.open(QIODevice::WriteOnly);
            file.write(cj::whiteQqCarrierPng());
            file.write(QByteArray(1234, 'c'));
            file.write(QByteArray(96, 'm'));
            file.write(raw);
        }
        cj::CarrierIndex parsed;
        QString error;
        check(cj::readCarrierIndex(path, &parsed, &error), QStringLiteral("索引可被解析"), error);
        check(parsed.partIndex == 2 && parsed.partCount == 5 && parsed.cipherLen == 1234
                  && parsed.metaLen == 96,
              QStringLiteral("索引字段一致"));
    }

    // ---- 8. 分片规划 ----
    {
        const QList<qint64> qqSmall = cj::qq::plan(1024);
        check(qqSmall.size() == 1 && qqSmall.first() == 1024,
              QStringLiteral("QQ 小内容单片"));
        const QList<qint64> qqBig = cj::qq::plan(cj::qq::kAppendBytes + 1000);
        check(qqBig.size() == 2 && qqBig.first() == cj::qq::kPayloadBytesPerImage,
              QStringLiteral("QQ 超限拆片"));
        const QList<qint64> qqMax = cj::qq::plan(cj::qq::kBytesPerPost);
        check(qqMax.size() == cj::qq::kImagesPerPost,
              QStringLiteral("QQ 单次支持 18 张载体"));
        check(cj::qq::plan(cj::qq::kBytesPerPost + 1).size() == cj::qq::kImagesPerPost + 1,
              QStringLiteral("QQ 超过 18 张继续规划下一批"));

        const QByteArray png = cj::whiteQqCarrierPng();
        const QList<qint64> bili = cj::bili::plan(200 * 1024, 96, png.size());
        check(bili.size() == 1 && bili.first() == 200 * 1024,
              QStringLiteral("B站单分片"));
        const QList<qint64> biliTooBig = cj::bili::plan(cj::bili::kMaxPayload + 1, 96, png.size());
        check(biliTooBig.isEmpty(), QStringLiteral("B站超过 100 MiB 被拒绝"));
    }

    // ---- 9. 载体生成 → 恢复 往返（QQ 规则）----
    {
        QTemporaryDir dir;
        const QString payloadPath = QDir(dir.path()).filePath(QStringLiteral("payload.bin"));
        QByteArray payload;
        payload.resize(3 * 1024 * 1024 + 12345);
        for (int i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>((i * 31 + 7) & 0xFF);
        writeFile(payloadPath, payload);

        cj::CreatedResource resource;
        QString error;
        const bool created = cj::generateCarriers(payloadPath,
                                                  QStringLiteral("payload.bin"),
                                                  QStringLiteral("application/octet-stream"),
                                                  cj::whiteQqCarrierPng(),
                                                  QDir(dir.path()).filePath(QStringLiteral("out")),
                                                  true,
                                                  &resource,
                                                  &error);
        check(created, QStringLiteral("载体生成成功"), error);
        if (created) {
            check(resource.parts.size() == 1, QStringLiteral("3 MiB 内容使用 1 张载体"));
            check(resource.fileKey.size() == 32 && resource.uuid.size() == 16,
                  QStringLiteral("密钥与 uuid 长度正确"));
            const QByteArray fileBytes = readAll(resource.parts.first().path);
            check(cj::hex(cj::sha256(fileBytes)) == resource.parts.first().remoteSha256Hex,
                  QStringLiteral("载体 SHA-256 与记录一致"));

            cj::ResourceRecord record;
            record.protocolVersion = 1;
            record.fileKey = resource.fileKey;
            record.encryptedMetadata = resource.encryptedMetadata;
            record.originalSha256Hex = resource.originalSha256Hex;

            cj::RestoredResult restored;
            const bool ok = cj::restoreFromCarriers(
                {resource.parts.first().path},
                record,
                QDir(dir.path()).filePath(QStringLiteral("restore")),
                &restored,
                &error);
            check(ok, QStringLiteral("载体恢复成功"), error);
            if (ok) {
                check(readAll(restored.path) == payload, QStringLiteral("恢复内容与原文件一致"));
                check(restored.name == QStringLiteral("payload.bin"),
                      QStringLiteral("恢复文件名一致"));
            }
        }
    }

    // ---- JPEG 首图载体：索引格式与恢复逻辑不依赖 PNG ----
    {
        QTemporaryDir dir;
        QImage image(640, 360, QImage::Format_RGB32);
        image.fill(QColor(32, 96, 160));
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        buffer.open(QIODevice::WriteOnly);
        const bool encoded = image.save(&buffer, "JPEG", 95);
        buffer.close();
        check(encoded && cj::cleanCarrierImageLength(jpeg) == jpeg.size(),
              QStringLiteral("JPEG 首图载体校验通过"));

        const QByteArray payload("jpeg-cover-roundtrip");
        const QString payloadPath = QDir(dir.path()).filePath(QStringLiteral("cover.bin"));
        writeFile(payloadPath, payload);
        cj::CreatedResource resource;
        QString error;
        const bool created = cj::generateCarriers(payloadPath,
                                                  QStringLiteral("cover.bin"),
                                                  QStringLiteral("application/octet-stream"),
                                                  jpeg,
                                                  QDir(dir.path()).filePath(QStringLiteral("jpeg-carrier")),
                                                  true,
                                                  &resource,
                                                  &error);
        check(created, QStringLiteral("JPEG 首图载体生成成功"), error);
        if (created) {
            check(resource.parts.first().path.endsWith(QStringLiteral(".jpg")),
                  QStringLiteral("JPEG 载体扩展名正确"));
            cj::ResourceRecord record;
            record.fileKey = resource.fileKey;
            record.encryptedMetadata = resource.encryptedMetadata;
            record.originalSha256Hex = resource.originalSha256Hex;
            cj::RestoredResult restored;
            const bool restoredOk = cj::restoreFromCarriers(
                {resource.parts.first().path}, record,
                QDir(dir.path()).filePath(QStringLiteral("jpeg-restore")), &restored, &error);
            check(restoredOk && readAll(restored.path) == payload,
                  QStringLiteral("JPEG 首图载体恢复成功"), error);
        }
    }

    // 201 MiB 不会成为一个巨型 AES-GCM 缓冲区：QQ 规划始终限制到约 29 MiB/片。
    {
        const qint64 total = 201LL * 1024 * 1024;
        const QList<qint64> parts = cj::qq::plan(total);
        qint64 planned = 0;
        bool bounded = !parts.isEmpty();
        for (qint64 part : parts) {
            planned += part;
            bounded = bounded && part <= cj::qq::kAppendBytes - 128;
        }
        check(planned == total && bounded && parts.size() > 1,
              QStringLiteral("大载荷按 29 MiB 上限分片"));
    }

    // ---- 10. 多文件打包往返 + B站多分片往返 ----
    {
        QTemporaryDir dir;
        const QString payloadPath = QDir(dir.path()).filePath(QStringLiteral("cloud.bin"));
        QByteArray payload(2 * 1024 * 1024 + 321, Qt::Uninitialized);
        for (int i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>((i * 17 + 29) & 0xFF);
        writeFile(payloadPath, payload);

        cj::QqCarrierPlan plan;
        QString error;
        const bool prepared = cj::prepareQqCarrierPlan(
            payloadPath,
            QStringLiteral("cloud.bin"),
            QStringLiteral("application/octet-stream"),
            QByteArray(),
            QDir(dir.path()).filePath(QStringLiteral("cloud-parts")),
            &plan,
            &error,
            false,
            true);
        check(prepared, QStringLiteral("小飞机 secretstream 计划生成"), error);
        if (prepared) {
            plan.plainSizes = {1024 * 1024, payload.size() - 1024 * 1024};
            cj::CarrierPart first;
            cj::CarrierPart second;
            const bool firstOk = cj::generateQqCarrierPart(&plan, 0, &first, &error);
            const bool secondOk = firstOk
                                  && cj::generateQqCarrierPart(&plan, 1, &second, &error);
            check(firstOk && secondOk, QStringLiteral("小飞机双分片依次生成"), error);
            if (firstOk && secondOk) {
                cj::CarrierIndex firstIndex;
                check(cj::readCarrierIndex(first.path, &firstIndex, &error)
                          && firstIndex.imageLen == 0
                          && (firstIndex.flags & cj::kCarrierFlagSecretStream) != 0,
                      QStringLiteral("小飞机分片标记为无图片 secretstream"), error);

                cj::ResourceRecord record;
                record.protocolVersion = 1;
                record.fileKey = plan.resource.fileKey;
                record.encryptedMetadata = plan.resource.encryptedMetadata;
                record.originalSha256Hex = plan.resource.originalSha256Hex;
                cj::RestoredResult restored;
                const bool restoredOk = cj::restoreFromCarriers(
                    {second.path, first.path},
                    record,
                    QDir(dir.path()).filePath(QStringLiteral("cloud-restore")),
                    &restored,
                    &error);
                check(restoredOk, QStringLiteral("小飞机 secretstream 双分片恢复"), error);
                if (restoredOk)
                    check(readAll(restored.path) == payload,
                          QStringLiteral("小飞机恢复内容与原文件一致"));
            }
        }
    }

    // ---- 11. 多文件打包往返 + B站多分片往返 ----
    {
        QTemporaryDir dir;
        const QString a = QDir(dir.path()).filePath(QStringLiteral("a.txt"));
        const QString b = QDir(dir.path()).filePath(QStringLiteral("b.bin"));
        writeFile(a, QByteArray(300000, 'A'));
        writeFile(b, QByteArray(500000, 'B'));

        const QString bundlePath = QDir(dir.path()).filePath(QStringLiteral("hidden-files.zip"));
        QString error;
        check(cj::bundle::pack({{QStringLiteral("a.txt"), a, 300000},
                                {QStringLiteral("b.bin"), b, 500000}},
                               bundlePath,
                               &error),
              QStringLiteral("多文件打包成功"),
              error);

        QList<cj::BundleItem> unpacked;
        check(cj::bundle::unpack(bundlePath,
                                 QDir(dir.path()).filePath(QStringLiteral("unpacked")),
                                 &unpacked,
                                 &error),
              QStringLiteral("多文件解包成功"),
              error);
        check(unpacked.size() == 2, QStringLiteral("解包条目数为 2"));
        check(readAll(unpacked.first().path).size() == 300000,
              QStringLiteral("解包内容大小一致"));

        const QString big = QDir(dir.path()).filePath(QStringLiteral("big.bin"));
        writeFile(big, QByteArray(6 * 1024 * 1024, 'Z'));

        cj::CreatedResource resource;
        const bool created = cj::generateCarriers(big,
                                                  QStringLiteral("big.bin"),
                                                  QStringLiteral("application/octet-stream"),
                                                  cj::whiteQqCarrierPng(),
                                                  QDir(dir.path()).filePath(QStringLiteral("bili")),
                                                  false,
                                                  &resource,
                                                  &error);
        check(created, QStringLiteral("B站规则载体生成"), error);
        if (created) {
            check(resource.parts.size() >= 2, QStringLiteral("6 MiB 内容被拆成多片"));

            cj::ResourceRecord record;
            record.protocolVersion = 1;
            record.fileKey = resource.fileKey;
            record.encryptedMetadata = resource.encryptedMetadata;
            record.originalSha256Hex = resource.originalSha256Hex;

            QStringList paths;
            for (auto it = resource.parts.crbegin(); it != resource.parts.crend(); ++it)
                paths << it->path; // 故意倒序，验证按索引排序

            cj::RestoredResult restored;
            const bool ok = cj::restoreFromCarriers(
                paths,
                record,
                QDir(dir.path()).filePath(QStringLiteral("bili-restore")),
                &restored,
                &error);
            check(ok, QStringLiteral("多分片恢复成功"), error);
            if (ok)
                check(readAll(restored.path) == readAll(big), QStringLiteral("多分片恢复内容一致"));
        }
    }

    // ---- 11. 单文件智能压缩（bundle 标记）往返 ----
    {
        QTemporaryDir dir;
        const QString doc = QDir(dir.path()).filePath(QStringLiteral("报告.txt"));
        QByteArray text;
        for (int i = 0; i < 40000; ++i)
            text += QStringLiteral("第 %1 行 藏笺智能压缩 abcdefg\n").arg(i).toUtf8();
        writeFile(doc, text);

        QString error;
        const QString zipped = QDir(dir.path()).filePath(QStringLiteral("smart.zip"));
        check(cj::bundle::pack({{QStringLiteral("报告.txt"), doc, text.size()}}, zipped, &error),
              QStringLiteral("单文件打包成功"),
              error);
        check(QFileInfo(zipped).size() < text.size(),
              QStringLiteral("单文件打包确实变小"));

        cj::CreatedResource resource;
        const bool created = cj::generateCarriers(zipped,
                                                  QStringLiteral("报告.txt"),
                                                  QStringLiteral("application/zip"),
                                                  cj::whiteQqCarrierPng(),
                                                  QDir(dir.path()).filePath(QStringLiteral("smart-out")),
                                                  true,
                                                  &resource,
                                                  &error,
                                                  true);
        check(created, QStringLiteral("单文件智能压缩载体生成"), error);
        if (created) {
            cj::ResourceRecord record;
            record.protocolVersion = 1;
            record.fileKey = resource.fileKey;
            record.encryptedMetadata = resource.encryptedMetadata;
            record.originalSha256Hex = resource.originalSha256Hex;

            cj::RestoredResult restored;
            const bool ok = cj::restoreFromCarriers(
                {resource.parts.first().path},
                record,
                QDir(dir.path()).filePath(QStringLiteral("smart-restore")),
                &restored,
                &error);
            check(ok, QStringLiteral("单文件智能压缩恢复成功"), error);
            if (ok) {
                check(restored.children.size() == 1,
                      QStringLiteral("恢复时自动解包出 1 个文件"));
                const QDir restoreDir(QDir(dir.path()).filePath(QStringLiteral("smart-restore")));
                const QStringList leftovers =
                    restoreDir.entryList(QStringList{QStringLiteral("*.zip")}, QDir::Files);
                check(leftovers.isEmpty(), QStringLiteral("解包后中间 zip 已物理删除"));
                if (restored.children.size() == 1) {
                    check(restored.children.first().name == QStringLiteral("报告.txt"),
                          QStringLiteral("解包后文件名保持原样"));
                    check(readAll(restored.children.first().path) == text,
                          QStringLiteral("解包后内容与原文件一致"));
                }
            }
        }
    }

    g_failed += runCompressionTests(out, &g_passed);

    // ---- 网络：B站 CDN 走 aria2 下载（CJ_NET_TEST=1 时才跑）----
    if (qEnvironmentVariableIsSet("CJ_NET_TEST")) {
        out << "\n== 网络下载 ==\n";
        const QTemporaryDir dir;
        const QString path = QDir(dir.path()).filePath(QStringLiteral("part-0.png"));
        cj::Backend backend;
        bool done = false;
        bool ok = false;
        QString error;
        QEventLoop loop;
        QObject::connect(&backend, &cj::Backend::progressChanged, &loop, [] {});
        backend.downloadFile(
            QStringLiteral("https://i0.hdslb.com/bfs/new_dyn/"
                           "065c71d7c8b7adf75084432b4d9ced083707036669839672.png"),
            path,
            [&](bool success, const QString &err) {
                ok = success;
                error = err;
                done = true;
                loop.quit();
            });
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        loop.exec();
        check(done && ok, QStringLiteral("B站图片下载完成"), error);
        const QByteArray data = readAll(path);
        check(data.size() == 4320661,
              QStringLiteral("下载长度与云端一致 4320661"),
              QString::number(data.size()));
        check(cj::hex(cj::sha256(data))
                  == QStringLiteral("d880060ba91d1c86f114dbd8a8760f71cc8b21df100c0cfefb6f7e9413ace9a4"),
              QStringLiteral("下载内容 SHA-256 一致"));
    }

    out << "\n== 结果：" << g_passed << " 通过 / " << g_failed << " 失败 ==\n";
    out.flush();
    return g_failed == 0 ? 0 : 1;
}
