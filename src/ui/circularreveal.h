#pragma once

#include <QImage>
#include <QPoint>
#include <QPropertyAnimation>
#include <QQuickItemGrabResult>
#include <QQuickPaintedItem>
#include <QRect>
#include <QSharedPointer>
#include <QSize>

namespace cj {

class CircularReveal : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(int radius READ radius WRITE setRadius NOTIFY radiusChanged)
    Q_PROPERTY(bool darkToLight READ darkToLight WRITE setDarkToLight NOTIFY darkToLightChanged)
    Q_PROPERTY(int duration READ duration WRITE setDuration NOTIFY durationChanged)

public:
    explicit CircularReveal(QQuickItem *parent = nullptr);

    QQuickItem *target() const { return m_target; }
    void setTarget(QQuickItem *value);

    int radius() const { return m_radius; }
    void setRadius(int value);

    bool darkToLight() const { return m_darkToLight; }
    void setDarkToLight(bool value);

    int duration() const { return m_duration; }
    void setDuration(int value);

    void paint(QPainter *painter) override;
    Q_INVOKABLE void start(int imageWidth, int imageHeight,
                           const QPoint &center, int finalRadius,
                           const QRect &sourceRect);

signals:
    void targetChanged();
    void radiusChanged();
    void darkToLightChanged();
    void durationChanged();
    void imageChanged();
    void animationFinished();

private slots:
    void handleGrabResult();

private:
    QQuickItem *m_target = nullptr;
    int m_radius = 0;
    bool m_darkToLight = false;
    int m_duration = 1000;
    QPropertyAnimation *m_animation = nullptr;
    QImage m_source;
    QPoint m_center;
    QRect m_sourceRect;
    QSize m_captureSize;
    QSharedPointer<QQuickItemGrabResult> m_grabResult;
};

} // namespace cj
