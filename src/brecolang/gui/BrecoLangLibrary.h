#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace breco::lang {

struct BrecoLangLibraryFile {
    QString filePath;
    QString relativePath;
    QStringList entries;
    QString errorMessage;
};

struct BrecoLangLibraryContents {
    QVector<BrecoLangLibraryFile> schemas;
    QStringList filesNeedingMigration;
};

class BrecoLangLibrary {
public:
    explicit BrecoLangLibrary(QString directory = {});

    QString directory() const { return m_directory; }
    void setDirectory(const QString& directory);
    BrecoLangLibraryContents scan() const;

    static QString defaultDirectory();
    static bool ensureDirectory(const QString& directory,
                                QString* errorMessage = nullptr);

private:
    QString m_directory;
};

}  // namespace breco::lang
