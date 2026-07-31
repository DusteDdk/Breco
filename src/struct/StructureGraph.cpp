#include "struct/StructureGraph.h"

#include <QHash>

#include <limits>
#include <variant>

namespace breco {

namespace {

struct PodAlias {
    const char* name;
    PodKind kind;
};

constexpr PodAlias kPodAliases[] = {
    {"uint8_t", PodKind::UInt8},   {"uint_8", PodKind::UInt8},   {"uint8", PodKind::UInt8},
    {"uint16_t", PodKind::UInt16}, {"uint_16", PodKind::UInt16}, {"uint16", PodKind::UInt16},
    {"uint32_t", PodKind::UInt32}, {"uint_32", PodKind::UInt32}, {"uint32", PodKind::UInt32},
    {"uint64_t", PodKind::UInt64}, {"uint_64", PodKind::UInt64}, {"uint64", PodKind::UInt64},
    {"int8_t", PodKind::Int8},     {"int_8", PodKind::Int8},     {"int8", PodKind::Int8},
    {"int16_t", PodKind::Int16},   {"int_16", PodKind::Int16},   {"int16", PodKind::Int16},
    {"int32_t", PodKind::Int32},   {"int_32", PodKind::Int32},   {"int32", PodKind::Int32},
    {"int64_t", PodKind::Int64},   {"int_64", PodKind::Int64},   {"int64", PodKind::Int64},
};

bool isBuiltInTypeName(const QString& name) {
    for (const PodAlias& alias : kPodAliases) {
        if (name == QLatin1String(alias.name)) {
            return true;
        }
    }
    return name == QStringLiteral("asciistr") ||
           name == QStringLiteral("utf8str") ||
           name == QStringLiteral("utf16str") ||
           name == QStringLiteral("byte");
}

std::optional<int> staticTypeSize(
    const ResolvedType& type,
    const QHash<QString, std::optional<int>>& knownStructSizes) {
    if (const auto* pod = std::get_if<PodType>(&type)) {
        return podKindWidthBytes(pod->kind);
    }
    if (std::holds_alternative<ByteType>(type)) {
        return 1;
    }
    if (std::holds_alternative<StringType>(type)) {
        return std::nullopt;
    }
    if (const auto* ref = std::get_if<StructRefType>(&type)) {
        const auto found = knownStructSizes.constFind(ref->structName);
        return found == knownStructSizes.constEnd() ? std::nullopt : *found;
    }
    return std::nullopt;
}

}  // namespace

void StructureGraph::clear() {
    m_typedefs.clear();
    m_structs.clear();
    m_standaloneMembers.clear();
    m_defaultEntryName.clear();
}

bool StructureGraph::hasName(const QString& name) const {
    if (isBuiltInTypeName(name)) {
        return true;
    }
    for (const TypedefNode& node : m_typedefs) {
        if (node.name == name) {
            return true;
        }
    }
    for (const StructNode& node : m_structs) {
        if (node.name == name) {
            return true;
        }
    }
    for (const StandaloneMemberNode& node : m_standaloneMembers) {
        if (node.name == name) {
            return true;
        }
    }
    return false;
}

bool StructureGraph::resolveTypeName(const QString& name, ResolvedType* out,
                                     QString* displayName) const {
    if (out == nullptr) {
        return false;
    }
    for (const TypedefNode& node : m_typedefs) {
        if (node.name == name) {
            *out = node.type;
            if (displayName != nullptr) {
                *displayName = node.typeDisplayName;
            }
            return true;
        }
    }
    for (const PodAlias& alias : kPodAliases) {
        if (name == QLatin1String(alias.name)) {
            *out = PodType{alias.kind, Endianness::Native, {}};
            if (displayName != nullptr) {
                *displayName = name;
            }
            return true;
        }
    }
    if (name == QStringLiteral("asciistr")) {
        *out = StringType{StringEncoding::Ascii, Endianness::Native, {}};
        if (displayName != nullptr) {
            *displayName = name;
        }
        return true;
    }
    if (name == QStringLiteral("utf8str")) {
        *out = StringType{StringEncoding::Utf8, Endianness::Native, {}};
        if (displayName != nullptr) {
            *displayName = name;
        }
        return true;
    }
    if (name == QStringLiteral("utf16str")) {
        *out = StringType{StringEncoding::Utf16, Endianness::Native, {}};
        if (displayName != nullptr) {
            *displayName = name;
        }
        return true;
    }
    if (name == QStringLiteral("byte")) {
        *out = ByteType{};
        if (displayName != nullptr) {
            *displayName = name;
        }
        return true;
    }
    if (findStruct(name) != nullptr) {
        *out = StructRefType{name};
        if (displayName != nullptr) {
            *displayName = name;
        }
        return true;
    }
    return false;
}

const StructNode* StructureGraph::findStruct(const QString& name) const {
    for (const StructNode& node : m_structs) {
        if (node.name == name) {
            return &node;
        }
    }
    return nullptr;
}

const StandaloneMemberNode* StructureGraph::findStandaloneMember(const QString& name) const {
    for (const StandaloneMemberNode& node : m_standaloneMembers) {
        if (node.name == name) {
            return &node;
        }
    }
    return nullptr;
}

bool StructureGraph::addTypedef(const TypedefNode& node) {
    if (hasName(node.name)) {
        return false;
    }
    m_typedefs.push_back(node);
    return true;
}

bool StructureGraph::addStruct(const StructNode& node) {
    if (hasName(node.name)) {
        return false;
    }
    m_structs.push_back(node);
    return true;
}

bool StructureGraph::addStandaloneMember(const StandaloneMemberNode& node) {
    if (hasName(node.name)) {
        return false;
    }
    m_standaloneMembers.push_back(node);
    return true;
}

QStringList StructureGraph::entryNames() const {
    QStringList names;
    for (const StandaloneMemberNode& node : m_standaloneMembers) {
        names.push_back(node.name);
    }
    for (const StructNode& node : m_structs) {
        if (!node.name.isEmpty()) {
            names.push_back(node.name);
        }
    }
    for (const TypedefNode& node : m_typedefs) {
        if (isVisualizableEntryName(node.name) && !names.contains(node.name)) {
            names.push_back(node.name);
        }
    }
    return names;
}

bool StructureGraph::isVisualizableEntryName(const QString& name) const {
    if (findStandaloneMember(name) != nullptr || findStruct(name) != nullptr) {
        return true;
    }
    ResolvedType resolved;
    if (!resolveTypeName(name, &resolved, nullptr)) {
        return false;
    }
    return !resolved.valueless_by_exception();
}

TextRange StructureGraph::nameRangeForEntry(const QString& name) const {
    for (const TypedefNode& node : m_typedefs) {
        if (node.name == name) {
            return node.nameRange;
        }
    }
    for (const StandaloneMemberNode& node : m_standaloneMembers) {
        if (node.name == name) {
            return node.nameRange;
        }
    }
    for (const StructNode& node : m_structs) {
        if (node.name == name) {
            return node.nameRange;
        }
    }
    return {};
}

int StructureGraph::structLayoutSizeBytes(const QString& structName) const {
    return staticStructLayoutSizeBytes(structName).value_or(0);
}

std::optional<int> StructureGraph::staticStructLayoutSizeBytes(
    const QString& structName) const {
    QHash<QString, std::optional<int>> knownSizes;
    for (const StructNode& node : m_structs) {
        std::optional<int> nodeSize = 0;
        qint64 total = 0;
        for (const StructMember& member : node.members) {
            if (member.attributes.hasDynamicExtent()) {
                nodeSize = std::nullopt;
                break;
            }
            const std::optional<int> memberSize =
                staticTypeSize(member.type, knownSizes);
            if (!memberSize.has_value()) {
                nodeSize = std::nullopt;
                break;
            }
            total += *memberSize;
            if (total > std::numeric_limits<int>::max()) {
                nodeSize = std::nullopt;
                break;
            }
        }
        if (nodeSize.has_value()) {
            nodeSize = static_cast<int>(total);
        }
        knownSizes.insert(node.name, nodeSize);
        if (node.name == structName) {
            return nodeSize;
        }
    }
    return std::nullopt;
}

}  // namespace breco
