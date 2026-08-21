#include "brecolang/runtime/ByteSource.h"

#include <QFileInfo>

#include <algorithm>
#include <limits>

namespace breco::lang {

namespace {

bool validRange(quint64 offset, qsizetype length, quint64 size) {
    if (length < 0) {
        return false;
    }
    const quint64 amount = static_cast<quint64>(length);
    return offset <= size && amount <= size - offset;
}

ByteReadResult endOfInput() {
    return {ByteReadStatus::EndOfInput, {}, {}};
}

ByteReadResult readError(QString message) {
    return {ByteReadStatus::Error, {}, std::move(message)};
}

}  // namespace

const char* ByteView::data() const {
    if (!storage || offset < 0 || length < 0 ||
        offset > storage->size() || length > storage->size() - offset) {
        return nullptr;
    }
    return storage->constData() + offset;
}

quint64 ByteSource::absoluteOffset(quint64 logicalOffset) const {
    assertThreadAffinity();
    return logicalOffset;
}

void ByteSource::releaseBefore(quint64) { assertThreadAffinity(); }

void ByteSource::assertThreadAffinity() const {
    Q_ASSERT(m_ownerThread == QThread::currentThreadId());
}

ByteSourceIdentity ByteSource::identity() const {
    assertThreadAffinity();
    return {path(), size(), 0, 0};
}

ByteReadResult ByteSource::readByte(quint64 offset) {
    assertThreadAffinity();
    return read(offset, 1);
}

BorrowedWindowSource::BorrowedWindowSource(QByteArray bytes, QString sourcePath,
                                           quint64 baseOffset)
    : m_bytes(std::make_shared<QByteArray>(std::move(bytes))),
      m_path(std::move(sourcePath)),
      m_baseOffset(baseOffset) {}

ByteReadResult BorrowedWindowSource::read(quint64 offset, qsizetype length) {
    assertThreadAffinity();
    const quint64 total = static_cast<quint64>(m_bytes->size());
    if (!validRange(offset, length, total)) {
        return endOfInput();
    }
    return {ByteReadStatus::Ok,
            {m_bytes, static_cast<qsizetype>(offset), length}, {}};
}

std::optional<quint64> BorrowedWindowSource::size() const {
    assertThreadAffinity();
    return static_cast<quint64>(m_bytes->size());
}

quint64 BorrowedWindowSource::absoluteOffset(quint64 logicalOffset) const {
    assertThreadAffinity();
    if (logicalOffset > std::numeric_limits<quint64>::max() - m_baseOffset) {
        return std::numeric_limits<quint64>::max();
    }
    return m_baseOffset + logicalOffset;
}

ByteSourceIdentity BorrowedWindowSource::identity() const {
    assertThreadAffinity();
    return {m_path, static_cast<quint64>(m_bytes->size()), 0,
            static_cast<quint64>(reinterpret_cast<quintptr>(m_bytes.get()))};
}

PagedFileSource::PagedFileSource(QString path, qsizetype pageBytes,
                                 int residentPages)
    : m_path(std::move(path)), m_file(m_path),
      m_pageBytes(qMax<qsizetype>(4096, pageBytes)),
      m_residentPages(qMax(1, residentPages)) {}

std::shared_ptr<PagedFileSource> PagedFileSource::open(
    const QString& path, QString* error, qsizetype pageBytes, int residentPages) {
    auto source = std::shared_ptr<PagedFileSource>(
        new PagedFileSource(QFileInfo(path).absoluteFilePath(), pageBytes,
                            residentPages));
    if (!source->m_file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = source->m_file.errorString();
        }
        return {};
    }
    const qint64 fileSize = source->m_file.size();
    if (fileSize < 0) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not determine input size");
        }
        return {};
    }
    source->m_size = static_cast<quint64>(fileSize);
    return source;
}

std::optional<quint64> PagedFileSource::size() const {
    assertThreadAffinity();
    return m_size;
}

ByteSourceIdentity PagedFileSource::identity() const {
    assertThreadAffinity();
    const QFileInfo info(m_path);
    return {m_path,
            info.exists() && info.size() >= 0
                ? std::optional<quint64>(static_cast<quint64>(info.size()))
                : std::nullopt,
            info.lastModified().toMSecsSinceEpoch(), 0};
}

void PagedFileSource::touch(quint64 pageIndex) {
    m_pageOrder.removeAll(pageIndex);
    m_pageOrder.push_back(pageIndex);
    while (m_pageOrder.size() > m_residentPages) {
        const quint64 evicted = m_pageOrder.takeFirst();
        m_pages.remove(evicted);
    }
}

std::shared_ptr<const QByteArray> PagedFileSource::page(quint64 pageIndex,
                                                       QString* error) {
    const auto found = m_pages.constFind(pageIndex);
    if (found != m_pages.constEnd()) {
        const auto result = *found;
        touch(pageIndex);
        return result;
    }
    const quint64 start = pageIndex * static_cast<quint64>(m_pageBytes);
    if (start >= m_size) {
        return {};
    }
    if (!m_file.seek(static_cast<qint64>(start))) {
        if (error != nullptr) {
            *error = m_file.errorString();
        }
        return {};
    }
    const quint64 remaining = m_size - start;
    const qsizetype amount = static_cast<qsizetype>(
        qMin<quint64>(remaining, static_cast<quint64>(m_pageBytes)));
    auto bytes = std::make_shared<QByteArray>(m_file.read(amount));
    if (bytes->size() != amount) {
        if (error != nullptr) {
            *error = m_file.errorString().isEmpty()
                         ? QStringLiteral("Short read from input")
                         : m_file.errorString();
        }
        return {};
    }
    m_pages.insert(pageIndex, bytes);
    touch(pageIndex);
    return bytes;
}

ByteReadResult PagedFileSource::read(quint64 offset, qsizetype length) {
    assertThreadAffinity();
    if (!validRange(offset, length, m_size)) {
        return endOfInput();
    }
    if (length == 0) {
        auto empty = std::make_shared<QByteArray>();
        return {ByteReadStatus::Ok, {empty, 0, 0}, {}};
    }
    const quint64 firstPage = offset / static_cast<quint64>(m_pageBytes);
    const quint64 lastByte = offset + static_cast<quint64>(length) - 1;
    const quint64 lastPage = lastByte / static_cast<quint64>(m_pageBytes);
    QString error;
    if (firstPage == lastPage) {
        const auto bytes = page(firstPage, &error);
        if (!bytes) {
            return readError(error);
        }
        const qsizetype inPage = static_cast<qsizetype>(
            offset % static_cast<quint64>(m_pageBytes));
        return {ByteReadStatus::Ok, {bytes, inPage, length}, {}};
    }

    auto combined = std::make_shared<QByteArray>();
    combined->reserve(length);
    quint64 current = offset;
    qsizetype remaining = length;
    while (remaining > 0) {
        const quint64 index = current / static_cast<quint64>(m_pageBytes);
        const auto bytes = page(index, &error);
        if (!bytes) {
            return readError(error);
        }
        const qsizetype inPage = static_cast<qsizetype>(
            current % static_cast<quint64>(m_pageBytes));
        const qsizetype take = qMin(remaining, bytes->size() - inPage);
        combined->append(bytes->constData() + inPage, take);
        current += static_cast<quint64>(take);
        remaining -= take;
    }
    return {ByteReadStatus::Ok, {combined, 0, combined->size()}, {}};
}

SequentialSource::SequentialSource(std::shared_ptr<QIODevice> device,
                                   QString sourcePath)
    : m_device(std::move(device)), m_buffer(std::make_shared<QByteArray>()),
      m_path(std::move(sourcePath)) {}

ByteReadStatus SequentialSource::fillThrough(quint64 endExclusive,
                                             QString* error) {
    if (!m_device || !m_device->isOpen()) {
        if (error != nullptr) {
            *error = QStringLiteral("Sequential input is not open");
        }
        return ByteReadStatus::Error;
    }
    while (m_baseOffset + static_cast<quint64>(m_buffer->size()) < endExclusive &&
           !m_eof) {
        if (m_buffer.use_count() > 1) {
            m_buffer = std::make_shared<QByteArray>(*m_buffer);
        }
        const quint64 missing =
            endExclusive - (m_baseOffset + static_cast<quint64>(m_buffer->size()));
        const qint64 requested = static_cast<qint64>(
            qMin<quint64>(qMax<quint64>(missing, 64 * 1024),
                          static_cast<quint64>(std::numeric_limits<int>::max())));
        const QByteArray chunk = m_device->read(requested);
        if (chunk.isEmpty()) {
            if (m_device->atEnd()) {
                m_eof = true;
                break;
            }
            if (error != nullptr) {
                *error = m_device->errorString();
            }
            return ByteReadStatus::Error;
        }
        m_buffer->append(chunk);
    }
    return m_baseOffset + static_cast<quint64>(m_buffer->size()) >= endExclusive
               ? ByteReadStatus::Ok
               : ByteReadStatus::EndOfInput;
}

ByteReadResult SequentialSource::read(quint64 offset, qsizetype length) {
    assertThreadAffinity();
    if (length < 0 || offset < m_baseOffset ||
        offset > std::numeric_limits<quint64>::max() -
                                   static_cast<quint64>(length)) {
        return readError(QStringLiteral("Invalid sequential read range"));
    }
    QString error;
    const ByteReadStatus status =
        fillThrough(offset + static_cast<quint64>(length), &error);
    if (status != ByteReadStatus::Ok) {
        return {status, {}, error};
    }
    return {ByteReadStatus::Ok,
            {m_buffer, static_cast<qsizetype>(offset - m_baseOffset), length}, {}};
}

std::optional<quint64> SequentialSource::size() const {
    assertThreadAffinity();
    if (m_eof) {
        return m_baseOffset + static_cast<quint64>(m_buffer->size());
    }
    if (m_device && !m_device->isSequential()) {
        const qint64 amount = m_device->size();
        if (amount >= 0) {
            return static_cast<quint64>(amount);
        }
    }
    return std::nullopt;
}

void SequentialSource::releaseBefore(quint64 offset) {
    assertThreadAffinity();
    const quint64 end = m_baseOffset + static_cast<quint64>(m_buffer->size());
    const quint64 clamped = qBound(m_baseOffset, offset, end);
    const quint64 amount = clamped - m_baseOffset;
    if (amount == 0) {
        return;
    }
    if (m_buffer.use_count() > 1) {
        m_buffer = std::make_shared<QByteArray>(*m_buffer);
    }
    m_buffer->remove(0, static_cast<qsizetype>(amount));
    m_baseOffset = clamped;
}

ByteSourceIdentity SequentialSource::identity() const {
    assertThreadAffinity();
    return {m_path, std::nullopt, 0,
            static_cast<quint64>(reinterpret_cast<quintptr>(m_device.get()))};
}

SpoolingSource::SpoolingSource(std::shared_ptr<QIODevice> device,
                               QString sourcePath)
    : m_device(std::move(device)), m_path(std::move(sourcePath)) {
    m_spool.open();
}

bool SpoolingSource::isOpen() const {
    assertThreadAffinity();
    return m_device && m_device->isOpen() && m_spool.isOpen();
}

ByteReadStatus SpoolingSource::fillThrough(quint64 endExclusive,
                                          QString* error) {
    if (!isOpen()) {
        if (error != nullptr) {
            *error = QStringLiteral("Spooling input is not open");
        }
        return ByteReadStatus::Error;
    }
    while (m_spooledBytes < endExclusive && !m_eof) {
        const quint64 missing = endExclusive - m_spooledBytes;
        const qint64 requested = static_cast<qint64>(
            qMin<quint64>(qMax<quint64>(missing, 64 * 1024),
                          static_cast<quint64>(std::numeric_limits<int>::max())));
        const QByteArray chunk = m_device->read(requested);
        if (chunk.isEmpty()) {
            if (m_device->atEnd()) {
                m_eof = true;
                break;
            }
            if (error != nullptr) {
                *error = m_device->errorString();
            }
            return ByteReadStatus::Error;
        }
        if (!m_spool.seek(static_cast<qint64>(m_spooledBytes)) ||
            m_spool.write(chunk) != chunk.size()) {
            if (error != nullptr) {
                *error = m_spool.errorString();
            }
            return ByteReadStatus::Error;
        }
        m_spooledBytes += static_cast<quint64>(chunk.size());
    }
    return m_spooledBytes >= endExclusive ? ByteReadStatus::Ok
                                          : ByteReadStatus::EndOfInput;
}

ByteReadResult SpoolingSource::read(quint64 offset, qsizetype length) {
    assertThreadAffinity();
    if (length < 0 || offset > std::numeric_limits<quint64>::max() -
                                   static_cast<quint64>(length)) {
        return readError(QStringLiteral("Invalid spooled read range"));
    }
    QString error;
    const ByteReadStatus status =
        fillThrough(offset + static_cast<quint64>(length), &error);
    if (status != ByteReadStatus::Ok) {
        return {status, {}, error};
    }
    if (!m_spool.seek(static_cast<qint64>(offset))) {
        return readError(m_spool.errorString());
    }
    auto bytes = std::make_shared<QByteArray>(m_spool.read(length));
    if (bytes->size() != length) {
        return readError(m_spool.errorString().isEmpty()
                             ? QStringLiteral("Short read from spool")
                             : m_spool.errorString());
    }
    return {ByteReadStatus::Ok, {bytes, 0, length}, {}};
}

std::optional<quint64> SpoolingSource::size() const {
    assertThreadAffinity();
    if (m_eof) {
        return m_spooledBytes;
    }
    if (m_device && !m_device->isSequential()) {
        const qint64 amount = m_device->size();
        if (amount >= 0) {
            return static_cast<quint64>(amount);
        }
    }
    return std::nullopt;
}

ByteSourceIdentity SpoolingSource::identity() const {
    assertThreadAffinity();
    return {m_path, std::nullopt, 0,
            static_cast<quint64>(reinterpret_cast<quintptr>(m_device.get()))};
}

}  // namespace breco::lang
