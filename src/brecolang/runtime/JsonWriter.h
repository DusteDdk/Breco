#pragma once

#include <QIODevice>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace breco::lang {

class ByteSource;

class JsonWriter {
public:
    explicit JsonWriter(QIODevice* output);

    bool beginObject();
    bool endObject();
    bool beginArray();
    bool endArray();
    bool name(QStringView name);

    bool nullValue();
    bool boolean(bool value);
    bool unsignedInteger(quint64 value);
    bool signedInteger(qint64 value);
    bool floatingPoint(double value);
    bool string(QStringView value);
    bool sourceBytesHex(ByteSource* source, quint64 offset, quint64 length);

    bool finish();
    QString errorString() const { return m_error; }
    quint64 bytesWritten() const { return m_bytesWritten; }

private:
    enum class ScopeKind { Object, Array };
    struct Scope {
        ScopeKind kind = ScopeKind::Object;
        bool first = true;
        bool waitingForValue = false;
    };

    bool beforeValue();
    bool write(QByteArrayView bytes);
    bool writeStringLiteral(QStringView value);
    void fail(QString message);

    QIODevice* m_output = nullptr;
    QVector<Scope> m_scopes;
    QString m_error;
    quint64 m_bytesWritten = 0;
    bool m_rootWritten = false;
    bool m_finished = false;
};

}  // namespace breco::lang
