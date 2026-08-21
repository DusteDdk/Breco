#include "brecolang/runtime/DecodeTarget.h"

namespace breco::lang {

ResolvedDecodeTarget resolveDecodeTarget(
    const std::shared_ptr<const BrecoProgram>& program,
    DecodeTargetKind kind, QStringView name, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return ResolvedDecodeTarget{};
    };
    if (!program) {
        return fail(QStringLiteral("No compiled BrecoLang program is loaded"));
    }
    const QString wanted = name.toString();
    if (wanted.isEmpty()) {
        return fail(QStringLiteral("No decode target is selected"));
    }

    if (kind == DecodeTargetKind::Entry) {
        for (const EntryDesc& entry : program->entries) {
            if (program->symbol(entry.name) == wanted) {
                if (entry.input >= static_cast<InputId>(program->inputs.size())) {
                    return fail(QStringLiteral("The selected entry has no valid primary input"));
                }
                return {program, wanted, entry.input};
            }
        }
        return fail(QStringLiteral("Unknown entry '%1'").arg(wanted));
    }

    const RecordDesc* record = nullptr;
    for (const RecordDesc& candidate : program->records) {
        if (program->symbol(candidate.name) == wanted) {
            record = &candidate;
            break;
        }
    }
    if (record == nullptr) {
        return fail(QStringLiteral("Unknown record '%1'").arg(wanted));
    }
    if (record->parameters.count != 0) {
        return fail(QStringLiteral("Record '%1' requires parameters").arg(wanted));
    }

    for (const EntryDesc& entry : program->entries) {
        if (entry.name == record->name && entry.resultType == record->type) {
            if (entry.input >= static_cast<InputId>(program->inputs.size())) {
                return fail(QStringLiteral("The selected record has no valid primary input"));
            }
            return {program, wanted, entry.input};
        }
    }

    InputId input = kInvalidId;
    if (program->inputs.size() == 1) {
        input = 0;
    } else {
        for (InputId candidate = 0;
             candidate < static_cast<InputId>(program->inputs.size());
             ++candidate) {
            if (program->inputs.at(candidate).isDefault) {
                input = candidate;
                break;
            }
        }
    }
    if (input == kInvalidId) {
        return fail(QStringLiteral(
            "Selecting a record requires one input or a declared default input"));
    }

    auto adapted = std::make_shared<BrecoProgram>(*program);
    EntryDesc entry;
    entry.name = record->name;
    entry.resultType = record->type;
    entry.input = input;
    entry.statements = record->statements;
    entry.slotCount = record->slotCount;
    entry.extent = record->extent;
    adapted->entries.push_back(entry);
    return {std::move(adapted), wanted, input};
}

}  // namespace breco::lang
