#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QThread>

#include <functional>
#include <memory>
#include <unordered_map>

#include "brecolang/runtime/DecodeDocument.h"

namespace breco::lang {

struct DecodeJobTag {
    quint64 viewId = 0;
    DecodeDocumentHandle document;
    quint64 generation = 0;
    quint64 requestId = 0;
};

struct InputBindingSpec {
    QString path;
    QByteArray memoryBytes;
    bool hasMemoryBytes = false;
    std::function<std::shared_ptr<ByteSource>(const CancellationToken&)> factory;
};

struct ResolveJob {
    DecodeJobTag tag;
    std::shared_ptr<const BrecoProgram> program;
    QString entryName;
    QVector<InputBindingSpec> inputs;
    quint64 startOffset = 0;
    ShapeScanOptions shapeOptions;
    CancellationToken control;
};

struct DisplayPageJob {
    DecodeJobTag tag;
    DisplayPageRequest request;
};

struct ExportSpanJob {
    DecodeJobTag tag;
    ExportSpanRequest request;
};

struct ResolveResponse {
    DecodeJobTag tag;
    DecodeResult result;
};

struct DisplayPageResponse {
    DecodeJobTag tag;
    MaterializationLocator root;
    QVector<MaterializationLocator> expansionPath;
    QVector<SequenceWindow> windows;
    DisplayPageResult result;
};

struct ExportSpanResponse {
    DecodeJobTag tag;
    ExportSpanResult result;
};

enum class DocumentOutputKind {
    Json,
    BinarySpans,
    Outform,
};

struct DocumentOutputRequest {
    DocumentOutputKind kind = DocumentOutputKind::Json;
    InstanceLocator target;
    QString outformName;
};

struct DocumentOutputJob {
    DecodeJobTag tag;
    DocumentOutputRequest request;
    CancellationToken control;
};

struct DocumentOutputResponse {
    DecodeJobTag tag;
    DecodeStatus status = DecodeStatus::Error;
    QString temporaryPath;
    QString error;
};

class BrecoDecodeWorker final : public QObject {
    Q_OBJECT

public:
    explicit BrecoDecodeWorker(QObject* parent = nullptr);

public slots:
    void resolve(const ResolveJob& job);
    void displayPage(const DisplayPageJob& job);
    void exportSpans(const ExportSpanJob& job);
    void renderOutput(const DocumentOutputJob& job);
    void releaseDocument(DecodeDocumentHandle handle);
    void shutdown();

signals:
    void resolveCompleted(const ResolveResponse& response);
    void displayPageCompleted(const DisplayPageResponse& response);
    void exportSpansCompleted(const ExportSpanResponse& response);
    void outputCompleted(const DocumentOutputResponse& response);

private:
    std::unordered_map<quint64, std::unique_ptr<DecodeDocument>> m_documents;
    quint64 m_nextDocument = 1;
};

class BrecoDecodeController final : public QObject {
    Q_OBJECT

public:
    explicit BrecoDecodeController(QObject* parent = nullptr);
    ~BrecoDecodeController() override;

    quint64 requestResolve(quint64 viewId,
                           std::shared_ptr<const BrecoProgram> program,
                           QString entryName,
                           QVector<InputBindingSpec> inputs,
                           quint64 startOffset,
                           const ShapeScanOptions& options = {});
    quint64 requestDisplayPage(quint64 viewId, DisplayPageRequest request);
    quint64 requestExportSpans(quint64 viewId, ExportSpanRequest request);
    bool shareDocument(quint64 sourceViewId, quint64 targetViewId);
    bool renderOutputBlocking(quint64 viewId,
                              const DocumentOutputRequest& request,
                              QIODevice* output, QString* error = nullptr);
    void cancelView(quint64 viewId);
    void closeView(quint64 viewId);

    QThread* workerThread() { return &m_thread; }

signals:
    void resolveFinished(const ResolveResponse& response);
    void displayPageFinished(const DisplayPageResponse& response);
    void exportSpansFinished(const ExportSpanResponse& response);
    void documentOutputFinished(const DocumentOutputResponse& response);

private:
    struct ViewRequests {
        quint64 generation = 0;
        quint64 nextRequestId = 1;
        quint64 resolveRequestId = 0;
        DecodeDocumentHandle document;
        QHash<quint64, CancellationToken> controls;
    };

    void handleResolve(const ResolveResponse& response);
    void handleDisplayPage(const DisplayPageResponse& response);
    void handleExportSpans(const ExportSpanResponse& response);
    void handleOutput(const DocumentOutputResponse& response);
    void cancelControls(ViewRequests* view);
    void queueRelease(DecodeDocumentHandle handle);
    void releaseIfUnreferenced(DecodeDocumentHandle handle);

    QThread m_thread;
    BrecoDecodeWorker* m_worker = nullptr;
    QHash<quint64, ViewRequests> m_views;
};

}  // namespace breco::lang

Q_DECLARE_METATYPE(breco::lang::DecodeJobTag)
Q_DECLARE_METATYPE(breco::lang::ResolveJob)
Q_DECLARE_METATYPE(breco::lang::DisplayPageJob)
Q_DECLARE_METATYPE(breco::lang::ExportSpanJob)
Q_DECLARE_METATYPE(breco::lang::ResolveResponse)
Q_DECLARE_METATYPE(breco::lang::DisplayPageResponse)
Q_DECLARE_METATYPE(breco::lang::ExportSpanResponse)
Q_DECLARE_METATYPE(breco::lang::DocumentOutputResponse)
