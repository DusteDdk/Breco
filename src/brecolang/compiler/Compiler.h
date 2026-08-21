#pragma once

#include <QString>
#include <QStringView>

#include "brecolang/ir/BrecoProgram.h"

namespace breco::lang {

CompileResult compileBrecoLang(QStringView source, const QString& sourcePath = {});

}  // namespace breco::lang
