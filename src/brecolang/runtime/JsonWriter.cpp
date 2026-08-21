#include "brecolang/runtime/JsonWriter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <cmath>
#include <limits>

#include "brecolang/runtime/ByteSource.h"

namespace breco::lang {

JsonWriter::JsonWriter(QIODevice* output) : m_output(output) {
    if (m_output == nullptr || !m_output->isWritable()) {
        fail(QStringLiteral("JSON output device is not writable"));
    }
}

void JsonWriter::fail(QString message) {
    if (m_error.isEmpty()) {
        m_error = std::move(message);
    }
}

bool JsonWriter::write(QByteArrayView bytes) {
    if (!m_error.isEmpty()) {
        return false;
    }
    qsizetype written = 0;
    while (written < bytes.size()) {
        const qint64 amount = m_output->write(bytes.data() + written,
                                              bytes.size() - written);
        if (amount <= 0) {
            fail(m_output->errorString().isEmpty()
                     ? QStringLiteral("Could not write JSON output")
                     : m_output->errorString());
            return false;
        }
        written += static_cast<qsizetype>(amount);
        m_bytesWritten += static_cast<quint64>(amount);
    }
    return true;
}

bool JsonWriter::beforeValue() {
    if (!m_error.isEmpty() || m_finished) {
        return false;
    }
    if (m_scopes.isEmpty()) {
        if (m_rootWritten) {
            fail(QStringLiteral("JSON output has more than one root value"));
            return false;
        }
        m_rootWritten = true;
        return true;
    }
    Scope& scope = m_scopes.last();
    if (scope.kind == ScopeKind::Object) {
        if (!scope.waitingForValue) {
            fail(QStringLiteral("JSON object value has no member name"));
            return false;
        }
        scope.waitingForValue = false;
        return true;
    }
    if (!scope.first && !write(QByteArrayView(",", 1))) {
        return false;
    }
    scope.first = false;
    return true;
}

bool JsonWriter::writeStringLiteral(QStringView value) {
    QJsonArray wrapper;
    wrapper.push_back(QJsonValue(value.toString()));
    QByteArray encoded = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    if (encoded.size() < 2) {
        fail(QStringLiteral("Could not encode JSON string"));
        return false;
    }
    encoded.remove(0, 1);
    encoded.chop(1);
    return write(encoded);
}

bool JsonWriter::beginObject() {
    if (!beforeValue() || !write(QByteArrayView("{", 1))) {
        return false;
    }
    m_scopes.push_back({ScopeKind::Object, true, false});
    return true;
}

bool JsonWriter::endObject() {
    if (m_scopes.isEmpty() || m_scopes.last().kind != ScopeKind::Object) {
        fail(QStringLiteral("JSON object scopes are unbalanced"));
        return false;
    }
    if (m_scopes.last().waitingForValue) {
        fail(QStringLiteral("JSON object member has no value"));
        return false;
    }
    m_scopes.removeLast();
    return write(QByteArrayView("}", 1));
}

bool JsonWriter::beginArray() {
    if (!beforeValue() || !write(QByteArrayView("[", 1))) {
        return false;
    }
    m_scopes.push_back({ScopeKind::Array, true, false});
    return true;
}

bool JsonWriter::endArray() {
    if (m_scopes.isEmpty() || m_scopes.last().kind != ScopeKind::Array) {
        fail(QStringLiteral("JSON array scopes are unbalanced"));
        return false;
    }
    m_scopes.removeLast();
    return write(QByteArrayView("]", 1));
}

bool JsonWriter::name(QStringView memberName) {
    if (m_scopes.isEmpty() || m_scopes.last().kind != ScopeKind::Object) {
        fail(QStringLiteral("JSON member name is outside an object"));
        return false;
    }
    Scope& scope = m_scopes.last();
    if (scope.waitingForValue) {
        fail(QStringLiteral("Previous JSON member has no value"));
        return false;
    }
    if (!scope.first && !write(QByteArrayView(",", 1))) {
        return false;
    }
    scope.first = false;
    if (!writeStringLiteral(memberName) || !write(QByteArrayView(":", 1))) {
        return false;
    }
    scope.waitingForValue = true;
    return true;
}

bool JsonWriter::nullValue() {
    return beforeValue() && write(QByteArrayView("null", 4));
}

bool JsonWriter::boolean(bool value) {
    return beforeValue() &&
           write(value ? QByteArrayView("true", 4) : QByteArrayView("false", 5));
}

bool JsonWriter::unsignedInteger(quint64 value) {
    const QByteArray encoded = QByteArray::number(value);
    return beforeValue() && write(encoded);
}

bool JsonWriter::signedInteger(qint64 value) {
    const QByteArray encoded = QByteArray::number(value);
    return beforeValue() && write(encoded);
}

bool JsonWriter::floatingPoint(double value) {
    if (!std::isfinite(value)) {
        return nullValue();
    }
    const QByteArray encoded = QByteArray::number(value, 'g', 17);
    return beforeValue() && write(encoded);
}

bool JsonWriter::string(QStringView value) {
    return beforeValue() && writeStringLiteral(value);
}

bool JsonWriter::rawValue(QByteArrayView encoded) {
    QByteArray wrapped;
    wrapped.reserve(encoded.size() + 2);
    wrapped.push_back('[');
    wrapped.append(encoded.data(), encoded.size());
    wrapped.push_back(']');
    QJsonParseError error;
    const QJsonDocument parsed = QJsonDocument::fromJson(wrapped, &error);
    if (error.error != QJsonParseError::NoError || !parsed.isArray() ||
        parsed.array().size() != 1) {
        fail(QStringLiteral("Raw JSON fragment is not one valid value"));
        return false;
    }
    return beforeValue() && write(encoded);
}

bool JsonWriter::sourceBytesHex(ByteSource* source, quint64 offset,
                                quint64 length) {
    if (!beforeValue() || !write(QByteArrayView("\"", 1))) {
        return false;
    }
    constexpr qsizetype kChunkBytes = 64 * 1024;
    quint64 position = offset;
    quint64 remaining = length;
    while (remaining > 0) {
        const qsizetype amount = static_cast<qsizetype>(
            qMin<quint64>(remaining, static_cast<quint64>(kChunkBytes)));
        const ByteReadResult readResult = source != nullptr
                                              ? source->read(position, amount)
                                              : ByteReadResult{};
        if (!readResult.ok() || readResult.view.data() == nullptr) {
            fail(readResult.error.isEmpty()
                     ? QStringLiteral("Could not read bytes for JSON output")
                     : readResult.error);
            return false;
        }
        const QByteArray encoded =
            QByteArray(readResult.view.data(), readResult.view.length).toHex();
        if (!write(encoded)) {
            return false;
        }
        position += static_cast<quint64>(amount);
        remaining -= static_cast<quint64>(amount);
    }
    return write(QByteArrayView("\"", 1));
}

bool JsonWriter::finish() {
    if (!m_error.isEmpty()) {
        return false;
    }
    if (!m_scopes.isEmpty() || !m_rootWritten) {
        fail(QStringLiteral("JSON output is incomplete"));
        return false;
    }
    m_finished = true;
    return true;
}

}  // namespace breco::lang
