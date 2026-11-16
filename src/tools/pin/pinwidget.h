// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#pragma once

#include <QPoint>
#include <QPointF>
#include <QString>
#include <QWidget>

class QLabel;
class QVBoxLayout;
class QGestureEvent;
class QPinchGesture;
class QGraphicsDropShadowEffect;

class PinWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PinWidget(const QPixmap& pixmap,
                       const QRect& geometry,
                       QWidget* parent = nullptr);
    ~PinWidget() override;

protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void showEvent(QShowEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    bool gestureEvent(QGestureEvent* event);
    bool scrollEvent(QWheelEvent* e);
    void pinchTriggered(QPinchGesture*);
    void closePin();
    void placeWithKWin();
    void unloadKWinScript();

    void rotateLeft();
    void rotateRight();

    void increaseOpacity();
    void decreaseOpacity();

    QPixmap m_pixmap;
    QVBoxLayout* m_layout;
    QLabel* m_label;
    QGraphicsDropShadowEffect* m_shadowEffect;
    QColor m_baseColor, m_hoverColor;

    bool m_expanding{ false };
    qreal m_scaleFactor{ 1 };
    qreal m_opacity{ 1 };
    unsigned int m_rotateFactor{ 0 };
    qreal m_currentStepScaleFactor{ 1 };
    bool m_sizeChanged{ false };
    // Where the pin should appear, in logical coordinates
    QPoint m_initialPosition;
    bool m_initialPlacementDone{ false };
    // Left button pressed, a system move starts once the mouse moves
    bool m_movePending{ false };
    QPointF m_pressPosition;
    // Name of the KWin script managing this pin, empty if there is none
    QString m_kwinScriptName;
    // File the KWin script was loaded from, removed with the script
    QString m_kwinScriptPath;
    // The KWin script is dragging the pin
    bool m_kwinMoving{ false };

private slots:
    void showContextMenu(const QPoint& pos);
    void copyToClipboard();
    void saveToFile();
};
