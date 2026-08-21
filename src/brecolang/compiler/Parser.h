#pragma once

#include <QString>
#include <QStringView>

#include "brecolang/compiler/Syntax.h"

namespace breco::lang {

ParseSyntaxResult parseBrecoLang(QStringView source, const QString& sourcePath = {});

}  // namespace breco::lang
