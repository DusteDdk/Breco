#pragma once

#include <QString>
#include <QStringView>

#include <memory>

#include "brecolang/ir/BrecoProgram.h"

namespace breco::lang {

enum class DecodeTargetKind : quint8 {
    Entry,
    Record,
};

struct ResolvedDecodeTarget {
    std::shared_ptr<const BrecoProgram> program;
    QString entryName;
    InputId primaryInput = kInvalidId;

    bool isValid() const {
        return program != nullptr && !entryName.isEmpty() &&
               primaryInput != kInvalidId;
    }
};

ResolvedDecodeTarget resolveDecodeTarget(
    const std::shared_ptr<const BrecoProgram>& program,
    DecodeTargetKind kind, QStringView name, QString* error = nullptr);

}  // namespace breco::lang
