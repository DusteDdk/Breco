#pragma once

#include <QString>
#include <QVector>

#include <memory>

#include "brecolang/ir/BrecoProgram.h"

namespace breco::lang {

// Immutable description copied into scan workers. The primary input is replaced
// with each scan target; every other declared input is opened from inputPaths.
struct ProbeScanPlan {
    std::shared_ptr<const BrecoProgram> program;
    QString entryName;
    InputId primaryInput = kInvalidId;
    QVector<QString> inputPaths;
};

}  // namespace breco::lang
