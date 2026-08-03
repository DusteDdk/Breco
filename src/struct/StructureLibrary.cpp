#include "struct/StructureLibrary.h"

#include "struct/StructDeclarationParser.h"

#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QStandardPaths>

#include <algorithm>

namespace breco {

StructureLibrary::StructureLibrary(QString directory) {
    setDirectory(directory.isEmpty() ? defaultDirectory() : directory);
}

void StructureLibrary::setDirectory(const QString& directory) {
    m_directory = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
}

QVector<StructureLibraryFile> StructureLibrary::scan() const {
    QVector<StructureLibraryFile> files;
    QDir root(m_directory);
    if (!root.exists()) {
        return files;
    }
    QDirIterator iterator(m_directory,
                          {QStringLiteral("*.brecostruct"),
                           QStringLiteral("*.brecoscript")},
                          QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        StructureLibraryFile item;
        item.filePath = path;
        item.relativePath = root.relativeFilePath(path);
        const ParseResult parsed = parseStructDeclarationFile(item.filePath);
        if (parsed.valid) {
            item.entries = parsed.graph.entryNames();
        } else {
            item.errorMessage = parsed.errorMessage;
        }
        files.push_back(item);
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.relativePath.compare(right.relativePath, Qt::CaseInsensitive) < 0;
    });
    return files;
}

QString StructureLibrary::defaultDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("structures"));
}

bool StructureLibrary::ensureDirectory(const QString& directory, QString* errorMessage) {
    QDir dir;
    if (dir.mkpath(directory)) {
        return true;
    }
    if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("Could not create structure directory '%1'").arg(directory);
    }
    return false;
}

}  // namespace breco
