#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

#include "struct/StructTypes.h"

namespace breco {

struct StructMember {
    QString name;
    ResolvedType type;
    QString typeDisplayName;
    FieldAttributes attributes;
    TextRange nameRange;
};

struct StructNode {
    QString name;
    QVector<StructMember> members;
    QVector<ComparisonExpression> assertions;
    TextRange nameRange;
    TextRange bodyRange;
};

struct TypedefNode {
    QString name;
    ResolvedType type;
    QString typeDisplayName;
    TextRange nameRange;
};

struct StandaloneMemberNode {
    QString name;
    ResolvedType type;
    QString typeDisplayName;
    FieldAttributes attributes;
    TextRange nameRange;
};

enum class OutformMode {
    Text,
    Binary,
};

struct OutformNode {
    QString name;
    OutformMode mode = OutformMode::Text;
    QString templateText;
    TextRange nameRange;
    QString sourceFilePath;
};

class StructureGraph {
public:
    void clear();

    bool addTypedef(const TypedefNode& node);
    bool addStruct(const StructNode& node);
    bool addStandaloneMember(const StandaloneMemberNode& node);
    bool addOutform(const OutformNode& node);

    const QVector<TypedefNode>& typedefs() const { return m_typedefs; }
    const QVector<StructNode>& structs() const { return m_structs; }
    const QVector<StandaloneMemberNode>& standaloneMembers() const { return m_standaloneMembers; }
    const QVector<OutformNode>& outforms() const { return m_outforms; }
    const QString& defaultEntryName() const { return m_defaultEntryName; }
    void setDefaultEntryName(const QString& name) { m_defaultEntryName = name; }
    bool addExternalRole(const QString& role);
    const QStringList& externalRoles() const { return m_externalRoles; }

    bool hasName(const QString& name) const;
    bool resolveTypeName(const QString& name, ResolvedType* out, QString* displayName) const;
    const StructNode* findStruct(const QString& name) const;
    const StandaloneMemberNode* findStandaloneMember(const QString& name) const;
    const OutformNode* findOutform(const QString& name) const;

    QStringList entryNames() const;
    TextRange nameRangeForEntry(const QString& name) const;
    bool isVisualizableEntryName(const QString& name) const;
    bool entryHasEffectiveScanConstraint(const QString& name) const;

    int structLayoutSizeBytes(const QString& structName) const;
    std::optional<int> staticStructLayoutSizeBytes(const QString& structName) const;

private:
    QVector<TypedefNode> m_typedefs;
    QVector<StructNode> m_structs;
    QVector<StandaloneMemberNode> m_standaloneMembers;
    QVector<OutformNode> m_outforms;
    QString m_defaultEntryName;
    QStringList m_externalRoles;
};

}  // namespace breco
