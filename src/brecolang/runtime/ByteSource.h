#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QString>
#include <QTemporaryFile>
#include <QtGlobal>

#include <memory>
#include <optional>

namespace breco::lang {

enum class ByteReadStatus {
    Ok,
    EndOfInput,
    Error,
};

struct ByteView {
    std::shared_ptr<const QByteArray> storage;
    qsizetype offset = 0;
    qsizetype length = 0;

    const char* data() const;
    bool isEmpty() const { return length == 0; }
};

struct ByteReadResult {
    ByteReadStatus status = ByteReadStatus::Error;
    ByteView view;
    QString error;

    bool ok() const { return status == ByteReadStatus::Ok; }
};

class ByteSource {
public:
    virtual ~ByteSource() = default;

    virtual ByteReadResult read(quint64 offset, qsizetype length) = 0;
    virtual std::optional<quint64> size() const = 0;
    virtual bool randomAccess() const = 0;
    virtual QString path() const = 0;
    virtual quint64 absoluteOffset(quint64 logicalOffset) const;
    virtual void releaseBefore(quint64 offset);

    ByteReadResult readByte(quint64 offset);
};

class BorrowedWindowSource final : public ByteSource {
public:
    explicit BorrowedWindowSource(QByteArray bytes, QString sourcePath = {},
                                  quint64 baseOffset = 0);

    ByteReadResult read(quint64 offset, qsizetype length) override;
    std::optional<quint64> size() const override;
    bool randomAccess() const override { return true; }
    QString path() const override { return m_path; }
    quint64 absoluteOffset(quint64 logicalOffset) const override;

private:
    std::shared_ptr<QByteArray> m_bytes;
    QString m_path;
    quint64 m_baseOffset = 0;
};

class PagedFileSource final : public ByteSource {
public:
    static std::shared_ptr<PagedFileSource> open(const QString& path,
                                                 QString* error = nullptr,
                                                 qsizetype pageBytes = 1024 * 1024,
                                                 int residentPages = 16);

    ByteReadResult read(quint64 offset, qsizetype length) override;
    std::optional<quint64> size() const override { return m_size; }
    bool randomAccess() const override { return true; }
    QString path() const override { return m_path; }

private:
    PagedFileSource(QString path, qsizetype pageBytes, int residentPages);
    std::shared_ptr<const QByteArray> page(quint64 pageIndex, QString* error);
    void touch(quint64 pageIndex);

    QString m_path;
    QFile m_file;
    quint64 m_size = 0;
    qsizetype m_pageBytes = 0;
    int m_residentPages = 0;
    QHash<quint64, std::shared_ptr<const QByteArray>> m_pages;
    QList<quint64> m_pageOrder;
};

class SequentialSource final : public ByteSource {
public:
    explicit SequentialSource(std::shared_ptr<QIODevice> device,
                              QString sourcePath = {});

    ByteReadResult read(quint64 offset, qsizetype length) override;
    std::optional<quint64> size() const override;
    bool randomAccess() const override { return false; }
    QString path() const override { return m_path; }
    void releaseBefore(quint64 offset) override;

private:
    ByteReadStatus fillThrough(quint64 endExclusive, QString* error);

    std::shared_ptr<QIODevice> m_device;
    std::shared_ptr<QByteArray> m_buffer;
    QString m_path;
    quint64 m_baseOffset = 0;
    bool m_eof = false;
};

class SpoolingSource final : public ByteSource {
public:
    explicit SpoolingSource(std::shared_ptr<QIODevice> device,
                            QString sourcePath = {});

    bool isOpen() const;
    ByteReadResult read(quint64 offset, qsizetype length) override;
    std::optional<quint64> size() const override;
    bool randomAccess() const override { return true; }
    QString path() const override { return m_path; }

private:
    ByteReadStatus fillThrough(quint64 endExclusive, QString* error);

    std::shared_ptr<QIODevice> m_device;
    QTemporaryFile m_spool;
    QString m_path;
    quint64 m_spooledBytes = 0;
    bool m_eof = false;
};

}  // namespace breco::lang
