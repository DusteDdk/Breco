#pragma once

#include <QWidget>

#include <memory>

#include "struct/StructExport.h"
#include "struct/StructureGraph.h"
#include "struct/VisualizedNode.h"

QT_BEGIN_NAMESPACE
class QPoint;
class QPushButton;
class QMenu;
class QTreeView;
class QVBoxLayout;
namespace Ui {
class StructDataView;
}
QT_END_NAMESPACE

namespace breco {

class StructVisualizedTreeModel;
class StructDataViewPanelTestAccess;

class StructDataViewPanel : public QWidget {
    Q_OBJECT

public:
    explicit StructDataViewPanel(QWidget* parent = nullptr);
    ~StructDataViewPanel() override;

    QTreeView* structDataTreeView() const;
    QPushButton* expandCollapseAllButton() const;
    QVBoxLayout* structDataViewLayout() const;

    void setVisualization(const VisualizedNode& root);
    void clearVisualization();
    void setSourceEndianness(Endianness endianness);
    void setOutforms(const QVector<OutformNode>& outforms);
    void copySelectedScalarValuesToClipboard() const;

    static bool saveBytesToFile(const QString& filePath,
                                const QByteArray& bytes);

signals:
    void sourceLocationActivated(const QString& filePath,
                                 quint64 absoluteOffset, quint64 byteLength);
    void declarationLocationActivated(int start, int end);

private:
    friend class StructDataViewPanelTestAccess;
    void toggleExpandCollapseAll();
    void showTreeContextMenu(const QPoint& pos);
    void addScopeMenu(QMenu* parent, const QString& label,
                      const QVector<const VisualizedNode*>& nodes,
                      bool writeSingleJsonObject, bool enabled);
    QVector<const VisualizedNode*> selectedTreeNodes() const;
    QByteArray jsonForNodes(const QVector<const VisualizedNode*>& nodes,
                            bool writeSingleObject) const;
    void saveJson(const QVector<const VisualizedNode*>& nodes,
                  bool writeSingleObject);
    void saveBinary(const QVector<const VisualizedNode*>& nodes,
                    StructBinaryExportMode mode);
    void addOutformMenu(QMenu* parent,
                        const QVector<const VisualizedNode*>& nodes);
    void saveUsingOutform(const OutformNode& outform,
                          const QVector<const VisualizedNode*>& nodes);
    void copyJsonToClipboard(const QVector<const VisualizedNode*>& nodes,
                             bool writeSingleObject) const;
    void copyScalarToClipboard(const VisualizedNode& node,
                               StructScalarFormat format) const;
    void copyPrefixedScalarToClipboard(const VisualizedNode& node,
                                       StructScalarFormat format) const;

    std::unique_ptr<Ui::StructDataView> m_ui;
    StructVisualizedTreeModel* m_model = nullptr;
    bool m_allExpanded = false;
    Endianness m_sourceEndianness = Endianness::Little;
    QVector<OutformNode> m_outforms;
    QString m_outformSavePathForTests;
};

}  // namespace breco
