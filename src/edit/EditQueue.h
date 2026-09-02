#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <optional>

namespace breco {

struct QueuedEdit {
    QString filePath;
    quint64 offset = 0;
    QByteArray originalBytes;
    QByteArray newBytes;

    quint64 endOffset() const {
        return offset + static_cast<quint64>(newBytes.size());
    }
    bool matchesOriginal() const { return originalBytes == newBytes; }
};

class EditQueue {
public:
    const QVector<QueuedEdit>& edits() const { return m_edits; }
    int size() const { return m_edits.size(); }
    bool isEmpty() const { return m_edits.isEmpty(); }
    const QueuedEdit& at(int index) const { return m_edits.at(index); }
    QueuedEdit& at(int index) { return m_edits[index]; }

    int add(QueuedEdit edit);
    void removeAt(int index);
    void removeIndices(QVector<int> indices);
    void setNewBytes(int index, QByteArray newBytes);
    void remapFilePath(const QString& from, const QString& to);
    QVector<QString> implicatedFiles() const;
    QVector<QueuedEdit> mergedForApply() const;

    static std::optional<quint64> parseUnsigned(const QString& text, int defaultBase = 10);
    static std::optional<qint64> parseSigned(const QString& text, int defaultBase = 10);
    static std::optional<QByteArray> packInteger(const QString& text, int byteWidth,
                                                 bool signedValue, bool littleEndian,
                                                 int defaultBase = 10);
    static QByteArray packUnsigned(quint64 value, int byteWidth, bool littleEndian);
    static QString bytesToHex(const QByteArray& bytes);
    static QByteArray hexToBytes(const QString& hex);

private:
    static int detectBase(QString* text, int defaultBase);
    QVector<QueuedEdit> m_edits;
};

std::optional<QByteArray> packTypedInteger(const QString& text, int byteWidth, bool signedValue,
                                           bool littleEndian);

}  // namespace breco
