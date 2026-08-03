#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace breco {

struct StructureLibraryFile {
    QString filePath;
    QString relativePath;
    QStringList entries;
    QString errorMessage;
};

class StructureLibrary {
public:
    explicit StructureLibrary(QString directory = {});

    QString directory() const { return m_directory; }
    void setDirectory(const QString& directory);
    QVector<StructureLibraryFile> scan() const;

    static QString defaultDirectory();
    static bool ensureDirectory(const QString& directory, QString* errorMessage = nullptr);

private:
    QString m_directory;
};

}  // namespace breco
