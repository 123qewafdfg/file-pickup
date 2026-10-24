#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QTextStream>
#include <QTimer>
#include <QtQml/qqmlextensionplugin.h>

#include "app/appcontroller.h"
#include "ui/circularreveal.h"

// FluentUI 以静态库方式接入：必须显式导入它的 QML 插件
Q_IMPORT_QML_PLUGIN(FluentUIPlugin)

namespace {
void shutdownMark(const QByteArray &stage)
{
    const QString path = qEnvironmentVariable("CJ_SHUTDOWN_LOG");
    if (path.isEmpty())
        return;
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        file.write(stage);
        file.write("\r\n");
    }
}
} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Cangjian"));
    app.setApplicationName(QStringLiteral("藏笺"));
    app.setApplicationDisplayName(QStringLiteral("文件取件"));
    app.setApplicationVersion(QStringLiteral("0.2.0"));

    QQuickStyle::setStyle(QStringLiteral("Basic"));

    int exitCode = 0;
    {
    cj::AppController controller;

    // 无界面自检：CJ_MAKE_CHECK=<文件路径> 时跳过界面，直接跑一次「压缩 → 生成载体」并输出结果。
    // 平台强制为哔哩哔哩，避免真的往 QQ 频道发帖。
    const QString makeCheck = qEnvironmentVariable("CJ_MAKE_CHECK");
    if (!makeCheck.isEmpty()) {
        controller.setPlatform(QStringLiteral("哔哩哔哩"));
        controller.addUrls(makeCheck.split(QLatin1Char(';'), Qt::SkipEmptyParts));
        QObject::connect(&controller, &cj::AppController::busyChanged, &app, [&controller]() {
            if (controller.busy() || controller.compressBusy())
                return;
            QTextStream out(stdout);
            const QVariantList files = controller.files();
            for (const QVariant &entry : files) {
                const QVariantMap item = entry.toMap();
                out << "FILE=" << item.value(QStringLiteral("name")).toString() << " | "
                    << item.value(QStringLiteral("meta")).toString() << "\n";
            }
            out << "TOTAL=" << controller.filesTotalText()
                << " bytes=" << controller.filesTotalBytes() << "\n";
            out << "COMPRESS_INFO=" << controller.compressInfo() << "\n";
            out << "CARRIERS=" << controller.carriers().size() << "\n";
            out.flush();
            QCoreApplication::exit(0);
        });
        QTimer::singleShot(0, &app, [&controller]() { controller.startMake(); });
        QTimer::singleShot(120000, &app, []() {
            QTextStream(stdout) << "TIMEOUT\n";
            QCoreApplication::exit(2);
        });
        return app.exec();
    }

    // 无界面自检：CJ_EXTRACT_DIR_CHECK=1（或直接给路径）验证提取目录与时间戳子目录
    const QString extractCheck = qEnvironmentVariable("CJ_EXTRACT_DIR_CHECK");
    if (!extractCheck.isEmpty()) {
        if (extractCheck != QStringLiteral("1"))
            controller.setExtractDir(extractCheck);
        const QString first = controller.newExtractSessionDir();
        const QString second = controller.newExtractSessionDir();
        QTextStream out(stdout);
        out << "EXTRACT_ROOT=" << controller.extractDir() << "\n";
        out << "SESSION1=" << first << "\n";
        out << "SESSION2=" << second << "\n";
        out << "EXISTS=" << (QDir(first).exists() ? 1 : 0) << (QDir(second).exists() ? 1 : 0)
            << " distinct=" << (first != second ? 1 : 0) << "\n";
        out.flush();
        QDir(first).removeRecursively();
        QDir(second).removeRecursively();
        return 0;
    }

    // 调试用：CJ_PRELOAD_FILES=a;b;c 在启动后直接把这些文件放进列表（仅用于界面自查）
    const QString preload = qEnvironmentVariable("CJ_PRELOAD_FILES");
    if (!preload.isEmpty()) {
        const QStringList paths = preload.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        QTimer::singleShot(0, &controller, [&controller, paths]() { controller.addUrls(paths); });
    }

    qmlRegisterSingletonInstance("Cangjian", 1, 0, "App", &controller);
    qmlRegisterType<cj::CircularReveal>("Cangjian", 1, 0, "CircularReveal");

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    engine.loadFromModule("Cangjian", "Main");

    // 联调用：CJ_EXIT_MS=3000 表示 3 秒后自动退出（用于验证退出路径是否干净）
    const int exitMs = qEnvironmentVariableIntValue("CJ_EXIT_MS");
    if (exitMs > 0)
        QTimer::singleShot(exitMs, &app, []() { QCoreApplication::quit(); });

    exitCode = app.exec();
    shutdownMark("event-loop-returned");
    }
    shutdownMark("qml-and-controller-destroyed");
    return exitCode;
}
