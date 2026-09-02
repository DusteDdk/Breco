#pragma once

#include <QByteArray>
#include <QColor>
#include <QPair>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <optional>

#include "brecolang/ir/BrecoProgram.h"
#include "brecolang/runtime/DecodedData.h"

namespace breco {

enum class VisualizationMode {
    Cartesian2D = 0,
    Cartesian3D,
    Bitmap,
};

enum class CartesianStyle {
    Line = 0,
    Dot,
    Area,
    Skin,
    Bar,
};

struct VisualizationWindow {
    quint64 start = 0;
    quint64 length = 0;
    bool truncated = false;
};

struct VisualizePoint {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    QColor color = QColor(30, 144, 255);
};

struct VisualizeBitmapPixel {
    qint64 x = 0;
    qint64 y = 0;
    QColor color;
};

struct VisualizeConfiguration {
    quint64 numBytesOnNoSelection = 1024;
    CartesianStyle style = CartesianStyle::Line;
    double tickDistance = 0.0;
};

struct VisualizationConfigurationResult {
    VisualizeConfiguration config;
    QString error;

    bool success() const { return error.isEmpty(); }
};

struct VisualizationDecodeResult {
    QVector<VisualizePoint> points;
    QVector<VisualizeBitmapPixel> bitmapPixels;
    QByteArray bitmapPackedBits;
    VisualizeConfiguration config;
    int bitmapBitsPerPixel = 1;
    bool bitmapHasPlot = false;
    bool usedBuiltinRecord = false;
    QString error;

    bool success() const { return error.isEmpty(); }
};

constexpr quint64 kDefaultVisualizationBytes = 1024;
constexpr quint64 kMaximumVisualizationBytes = 8ULL * 1024ULL * 1024ULL;

QString builtinVisualizeProgramSource();

VisualizationWindow resolveVisualizationWindow(
    std::optional<QPair<quint64, quint64>> selection,
    quint64 fallbackOffset, quint64 fileSize,
    quint64 maximumBytes = kMaximumVisualizationBytes,
    quint64 defaultBytes = kDefaultVisualizationBytes);

VisualizationConfigurationResult readVisualizationConfiguration(
    QStringView source);
VisualizationDecodeResult decodeVisualization(
    QStringView source, const QByteArray& bytes, quint64 baseOffset,
    VisualizationMode mode);

}  // namespace breco
