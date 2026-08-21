#include "brecolang/gui/BrecoDecodeController.h"

#include "brecolang/render/OutformRenderer.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QMetaObject>
#include <QTemporaryFile>

namespace breco::lang {

namespace {

QString diagnosticMessages(const QVector<RuntimeDiagnostic>& diagnostics) {
    QStringList messages;
    for (const RuntimeDiagnostic& diagnostic : diagnostics) {
        messages.push_back(
            QStringLiteral("%1: %2").arg(diagnostic.code, diagnostic.message));
    }
    return messages.join(QLatin1Char('\n'));
}

}  // namespace

BrecoDecodeWorker::BrecoDecodeWorker(QObject* parent) : QObject(parent) {}

void BrecoDecodeWorker::resolve(const ResolveJob& job) {
    Q_ASSERT(thread() == QThread::currentThread());
    ResolveResponse response;
    response.tag = job.tag;
    if (!job.program || (job.control && job.control->isCancelled())) {
        response.result.status = job.control && job.control->isCancelled()
                                     ? DecodeStatus::Cancelled
                                     : DecodeStatus::Error;
        emit resolveCompleted(response);
        return;
    }

    DecodeRequest request;
    request.program = job.program;
    request.entryName = job.entryName;
    request.startOffset = job.startOffset;
    request.shapeOptions = job.shapeOptions;
    request.cancellation = job.control;
    request.inputs.resize(job.inputs.size());
    for (InputId input = 0;
         input < static_cast<InputId>(job.inputs.size()); ++input) {
        if (job.control && job.control->isCancelled()) {
            response.result.status = DecodeStatus::Cancelled;
            emit resolveCompleted(response);
            return;
        }
        const InputBindingSpec& binding = job.inputs.at(input);
        if (binding.factory) {
            request.inputs[input] = binding.factory(job.control);
        } else if (binding.hasMemoryBytes) {
            request.inputs[input] = std::make_shared<BorrowedWindowSource>(
                binding.memoryBytes, binding.path);
        } else if (!binding.path.isEmpty()) {
            QString error;
            request.inputs[input] = PagedFileSource::open(binding.path, &error);
            if (!request.inputs[input]) {
                response.result.status = DecodeStatus::Error;
                response.result.diagnostics.push_back(
                    {DiagnosticSeverity::Error, QStringLiteral("BRR0007"),
                     QStringLiteral("Could not bind input '%1': %2")
                         .arg(binding.path, error),
                     {}, {}, false});
                emit resolveCompleted(response);
                return;
            }
        }
    }

    const DecodeDocumentHandle handle{m_nextDocument++};
    response.tag.document = handle;
    auto document =
        std::make_unique<DecodeDocument>(handle, job.tag.generation);
    response.result = document->resolve(std::move(request));
    if (response.result.success()) {
        m_documents.emplace(handle.value, std::move(document));
    }
    emit resolveCompleted(response);
}

void BrecoDecodeWorker::displayPage(const DisplayPageJob& job) {
    Q_ASSERT(thread() == QThread::currentThread());
    DisplayPageResponse response;
    response.tag = job.tag;
    response.root = job.request.root;
    response.expansionPath = job.request.expansionPath;
    response.windows = job.request.sequenceWindows;
    const auto found = m_documents.find(job.tag.document.value);
    if (found == m_documents.end()) {
        response.result.status = DecodeStatus::Invalidated;
    } else if (!job.request.cancellation ||
               !job.request.cancellation->isCancelled()) {
        response.result = found->second->requestDisplayPage(job.request);
    } else {
        response.result.status = DecodeStatus::Cancelled;
    }
    emit displayPageCompleted(response);
}

void BrecoDecodeWorker::exportSpans(const ExportSpanJob& job) {
    Q_ASSERT(thread() == QThread::currentThread());
    ExportSpanResponse response;
    response.tag = job.tag;
    const auto found = m_documents.find(job.tag.document.value);
    if (found == m_documents.end()) {
        response.result.status = DecodeStatus::Invalidated;
    } else if (!job.request.cancellation ||
               !job.request.cancellation->isCancelled()) {
        response.result = found->second->requestExportSpans(job.request);
    } else {
        response.result.status = DecodeStatus::Cancelled;
    }
    emit exportSpansCompleted(response);
}

void BrecoDecodeWorker::renderOutput(const DocumentOutputJob& job) {
    Q_ASSERT(thread() == QThread::currentThread());
    DocumentOutputResponse response;
    response.tag = job.tag;
    const auto found = m_documents.find(job.tag.document.value);
    if (found == m_documents.end()) {
        response.status = DecodeStatus::Invalidated;
        response.error = QStringLiteral("Decoded document is no longer available");
        emit outputCompleted(response);
        return;
    }
    if (job.control && job.control->isCancelled()) {
        response.status = DecodeStatus::Cancelled;
        emit outputCompleted(response);
        return;
    }

    QTemporaryFile temporary(
        QDir::tempPath() + QStringLiteral("/breco-output-XXXXXX"));
    temporary.setAutoRemove(false);
    if (!temporary.open()) {
        response.error = temporary.errorString();
        emit outputCompleted(response);
        return;
    }
    DecodeDocument& document = *found->second;
    ExportSpanRequest validationRequest;
    validationRequest.document = document.handle();
    validationRequest.target =
        static_cast<InstanceLocator>(document.rootLocator());
    validationRequest.cancellation = job.control;
    const ExportSpanResult validation =
        document.requestExportSpans(validationRequest);
    if (!validation.success()) {
        const QString path = temporary.fileName();
        temporary.close();
        QFile::remove(path);
        response.status = validation.status;
        response.error = diagnosticMessages(validation.diagnostics);
        emit outputCompleted(response);
        return;
    }
    bool success = false;
    if (job.request.kind == DocumentOutputKind::Json) {
        DecodeRequest request;
        request.program = document.program();
        request.entryName = document.entryName();
        request.inputs = document.inputs();
        request.startOffset = document.startOffset();
        request.mode = DecodeMode::Streaming;
        request.output = &temporary;
        request.documentGeneration = document.generation();
        request.cancellation = job.control;
        const DecodeResult decoded = decodeBrecoProgram(request);
        success = decoded.success();
        if (!success) {
            response.status = decoded.status;
            response.error = diagnosticMessages(decoded.diagnostics);
        } else if (temporary.write("\n", 1) != 1) {
            response.error = temporary.errorString();
            success = false;
        }
    } else if (job.request.kind == DocumentOutputKind::Outform) {
        constexpr quint64 legacyOutformNodeLimit = 100000;
        if (document.ensureLegacyMaterialization(legacyOutformNodeLimit,
                                                 job.control,
                                                 &response.error)) {
            const auto tree = document.legacyTree();
            const RenderStore store(document.program(), tree, document.inputs(),
                                    document.legacyRootValue());
            const OutformRenderResult rendered = breco::lang::renderOutform(
                store, job.request.outformName, &temporary);
            success = rendered.success;
            response.error = rendered.error;
        }
    } else {
        ExportSpanRequest request;
        request.document = document.handle();
        request.target = job.request.target;
        request.cancellation = job.control;
        const ExportSpanResult spans = document.requestExportSpans(request);
        if (!spans.success()) {
            response.status = spans.status;
            response.error = diagnosticMessages(spans.diagnostics);
        } else {
            success = true;
            constexpr qsizetype chunkSize = 1024 * 1024;
            for (const ByteSpanValue& span : spans.spans.spans) {
                if (span.input >=
                        static_cast<InputId>(document.inputs().size()) ||
                    !document.inputs().at(span.input)) {
                    success = false;
                    response.error = QStringLiteral("Storage input is no longer bound");
                    break;
                }
                ByteSource* source = document.inputs().at(span.input).get();
                const quint64 base = source->absoluteOffset(0);
                if (span.offset < base) {
                    success = false;
                    response.error = QStringLiteral("Storage span precedes its input");
                    break;
                }
                quint64 logical = span.offset - base;
                quint64 remaining = span.length;
                while (remaining > 0) {
                    if (job.control && job.control->isCancelled()) {
                        success = false;
                        response.status = DecodeStatus::Cancelled;
                        break;
                    }
                    const qsizetype amount = static_cast<qsizetype>(
                        qMin<quint64>(remaining, chunkSize));
                    const ByteReadResult bytes = source->read(logical, amount);
                    if (!bytes.ok() ||
                        temporary.write(bytes.view.data(), bytes.view.length) !=
                            bytes.view.length) {
                        success = false;
                        response.error = bytes.error.isEmpty()
                                             ? temporary.errorString()
                                             : bytes.error;
                        break;
                    }
                    logical += static_cast<quint64>(amount);
                    remaining -= static_cast<quint64>(amount);
                }
                if (!success) break;
            }
        }
    }
    const QString path = temporary.fileName();
    temporary.close();
    if (success) {
        response.status = DecodeStatus::Success;
        response.temporaryPath = path;
    } else {
        QFile::remove(path);
        if (response.status == DecodeStatus::Success) {
            response.status = DecodeStatus::Error;
        }
    }
    emit outputCompleted(response);
}

void BrecoDecodeWorker::releaseDocument(DecodeDocumentHandle handle) {
    Q_ASSERT(thread() == QThread::currentThread());
    m_documents.erase(handle.value);
}

void BrecoDecodeWorker::shutdown() {
    Q_ASSERT(thread() == QThread::currentThread());
    m_documents.clear();
}

BrecoDecodeController::BrecoDecodeController(QObject* parent) : QObject(parent) {
    qRegisterMetaType<ResolveResponse>();
    qRegisterMetaType<DisplayPageResponse>();
    qRegisterMetaType<ExportSpanResponse>();
    qRegisterMetaType<DocumentOutputResponse>();
    m_worker = new BrecoDecodeWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &BrecoDecodeWorker::resolveCompleted, this,
            &BrecoDecodeController::handleResolve, Qt::QueuedConnection);
    connect(m_worker, &BrecoDecodeWorker::displayPageCompleted, this,
            &BrecoDecodeController::handleDisplayPage, Qt::QueuedConnection);
    connect(m_worker, &BrecoDecodeWorker::exportSpansCompleted, this,
            &BrecoDecodeController::handleExportSpans, Qt::QueuedConnection);
    connect(m_worker, &BrecoDecodeWorker::outputCompleted, this,
            &BrecoDecodeController::handleOutput, Qt::QueuedConnection);
    m_thread.setObjectName(QStringLiteral("BrecoLangDecodeWorker"));
    m_thread.start();
}

BrecoDecodeController::~BrecoDecodeController() {
    for (auto view = m_views.begin(); view != m_views.end(); ++view) {
        cancelControls(&view.value());
    }
    if (m_thread.isRunning()) {
        QMetaObject::invokeMethod(m_worker, &BrecoDecodeWorker::shutdown,
                                  Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
}

void BrecoDecodeController::cancelControls(ViewRequests* view) {
    for (const CancellationToken& control : std::as_const(view->controls)) {
        if (control) {
            control->cancel();
        }
    }
    view->controls.clear();
}

void BrecoDecodeController::queueRelease(DecodeDocumentHandle handle) {
    if (!handle.isValid() || !m_worker) {
        return;
    }
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, handle]() {
            worker->releaseDocument(handle);
        }, Qt::QueuedConnection);
}

void BrecoDecodeController::releaseIfUnreferenced(
    DecodeDocumentHandle handle) {
    if (!handle.isValid()) {
        return;
    }
    for (const ViewRequests& view : std::as_const(m_views)) {
        if (view.document == handle) {
            return;
        }
    }
    queueRelease(handle);
}

quint64 BrecoDecodeController::requestResolve(
    quint64 viewId, std::shared_ptr<const BrecoProgram> program,
    QString entryName, QVector<InputBindingSpec> inputs, quint64 startOffset,
    const ShapeScanOptions& options) {
    ViewRequests& view = m_views[viewId];
    cancelControls(&view);
    const DecodeDocumentHandle previous = view.document;
    view.document = {};
    releaseIfUnreferenced(previous);
    ++view.generation;
    const quint64 requestId = view.nextRequestId++;
    view.resolveRequestId = requestId;
    auto control = std::make_shared<RequestControl>(view.generation);
    view.controls.insert(requestId, control);

    ResolveJob job;
    job.tag = {viewId, {}, view.generation, requestId};
    job.program = std::move(program);
    job.entryName = std::move(entryName);
    job.inputs = std::move(inputs);
    job.startOffset = startOffset;
    job.shapeOptions = options;
    job.control = std::move(control);
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, job = std::move(job)]() {
            worker->resolve(job);
        }, Qt::QueuedConnection);
    return view.generation;
}

quint64 BrecoDecodeController::requestDisplayPage(
    quint64 viewId, DisplayPageRequest request) {
    auto found = m_views.find(viewId);
    if (found == m_views.end() || !found->document.isValid()) {
        return 0;
    }
    ViewRequests& view = found.value();
    const quint64 requestId = view.nextRequestId++;
    auto control = std::make_shared<RequestControl>(view.generation);
    view.controls.insert(requestId, control);
    request.document = view.document;
    request.cancellation = control;
    DisplayPageJob job{{viewId, view.document, view.generation, requestId},
                       std::move(request)};
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, job = std::move(job)]() {
            worker->displayPage(job);
        }, Qt::QueuedConnection);
    return requestId;
}

quint64 BrecoDecodeController::requestExportSpans(
    quint64 viewId, ExportSpanRequest request) {
    auto found = m_views.find(viewId);
    if (found == m_views.end() || !found->document.isValid()) {
        return 0;
    }
    ViewRequests& view = found.value();
    const quint64 requestId = view.nextRequestId++;
    auto control = std::make_shared<RequestControl>(view.generation);
    view.controls.insert(requestId, control);
    request.document = view.document;
    request.cancellation = control;
    ExportSpanJob job{{viewId, view.document, view.generation, requestId},
                      std::move(request)};
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, job = std::move(job)]() {
            worker->exportSpans(job);
        }, Qt::QueuedConnection);
    return requestId;
}

bool BrecoDecodeController::shareDocument(quint64 sourceViewId,
                                          quint64 targetViewId) {
    const auto source = m_views.constFind(sourceViewId);
    if (source == m_views.cend() || !source->document.isValid() ||
        m_views.contains(targetViewId)) {
        return false;
    }
    ViewRequests target;
    target.generation = source->generation;
    target.document = source->document;
    m_views.insert(targetViewId, std::move(target));
    return true;
}

bool BrecoDecodeController::renderOutputBlocking(
    quint64 viewId, const DocumentOutputRequest& request, QIODevice* output,
    QString* error) {
    auto found = m_views.find(viewId);
    if (found == m_views.end() || !found->document.isValid() ||
        output == nullptr) {
        if (error) *error = QStringLiteral("No decoded document is available");
        return false;
    }
    ViewRequests& view = found.value();
    const quint64 requestId = view.nextRequestId++;
    auto control = std::make_shared<RequestControl>(view.generation);
    view.controls.insert(requestId, control);
    DocumentOutputJob job{{viewId, view.document, view.generation, requestId},
                          request, control};
    DocumentOutputResponse response;
    QEventLoop loop;
    const QMetaObject::Connection completed = connect(
        m_worker, &BrecoDecodeWorker::outputCompleted, &loop,
        [&](const DocumentOutputResponse& candidate) {
            if (candidate.tag.viewId == viewId &&
                candidate.tag.requestId == requestId) {
                response = candidate;
                loop.quit();
            }
        }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, job = std::move(job)]() {
            worker->renderOutput(job);
        }, Qt::QueuedConnection);
    loop.exec();
    disconnect(completed);
    if (response.status != DecodeStatus::Success ||
        response.temporaryPath.isEmpty()) {
        if (error) {
            *error = response.error.isEmpty()
                         ? QStringLiteral("Document output failed")
                         : response.error;
        }
        return false;
    }
    QFile temporary(response.temporaryPath);
    bool copied = temporary.open(QIODevice::ReadOnly);
    while (copied && !temporary.atEnd()) {
        const QByteArray chunk = temporary.read(1024 * 1024);
        copied = !chunk.isEmpty() && output->write(chunk) == chunk.size();
    }
    if (!copied && error) {
        *error = temporary.errorString().isEmpty() ? output->errorString()
                                                   : temporary.errorString();
    }
    temporary.close();
    QFile::remove(response.temporaryPath);
    return copied;
}

void BrecoDecodeController::cancelView(quint64 viewId) {
    const auto found = m_views.find(viewId);
    if (found != m_views.end()) {
        cancelControls(&found.value());
    }
}

void BrecoDecodeController::closeView(quint64 viewId) {
    auto found = m_views.find(viewId);
    if (found == m_views.end()) {
        return;
    }
    cancelControls(&found.value());
    const DecodeDocumentHandle handle = found->document;
    m_views.erase(found);
    releaseIfUnreferenced(handle);
}

void BrecoDecodeController::handleResolve(const ResolveResponse& response) {
    auto found = m_views.find(response.tag.viewId);
    if (found == m_views.end() ||
        response.tag.generation != found->generation ||
        response.tag.requestId != found->resolveRequestId) {
        queueRelease(response.tag.document);
        return;
    }
    found->controls.remove(response.tag.requestId);
    if (response.result.success()) {
        found->document = response.tag.document;
    }
    emit resolveFinished(response);
}

void BrecoDecodeController::handleDisplayPage(
    const DisplayPageResponse& response) {
    auto found = m_views.find(response.tag.viewId);
    if (found == m_views.end() ||
        response.tag.generation != found->generation ||
        response.tag.document != found->document ||
        !found->controls.contains(response.tag.requestId)) {
        return;
    }
    found->controls.remove(response.tag.requestId);
    emit displayPageFinished(response);
}

void BrecoDecodeController::handleExportSpans(
    const ExportSpanResponse& response) {
    auto found = m_views.find(response.tag.viewId);
    if (found == m_views.end() ||
        response.tag.generation != found->generation ||
        response.tag.document != found->document ||
        !found->controls.contains(response.tag.requestId)) {
        return;
    }
    found->controls.remove(response.tag.requestId);
    emit exportSpansFinished(response);
}

void BrecoDecodeController::handleOutput(
    const DocumentOutputResponse& response) {
    auto found = m_views.find(response.tag.viewId);
    if (found == m_views.end() ||
        response.tag.generation != found->generation ||
        response.tag.document != found->document ||
        !found->controls.contains(response.tag.requestId)) {
        if (!response.temporaryPath.isEmpty()) {
            QFile::remove(response.temporaryPath);
        }
        return;
    }
    found->controls.remove(response.tag.requestId);
    emit documentOutputFinished(response);
}

}  // namespace breco::lang
