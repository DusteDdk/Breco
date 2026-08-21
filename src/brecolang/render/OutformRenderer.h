#pragma once

#include <QIODevice>
#include <QString>
#include <QStringView>
#include <QtGlobal>

#include "brecolang/render/RenderStore.h"

namespace breco::lang {

struct OutformRenderResult {
    bool success = false;
    QString error;
    quint64 bytesWritten = 0;
};

OutformRenderResult renderOutform(const RenderStore& store,
                                  QStringView outformName, QIODevice* output);

}  // namespace breco::lang
