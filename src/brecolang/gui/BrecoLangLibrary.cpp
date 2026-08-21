#include "brecolang/gui/BrecoLangLibrary.h"

#include "brecolang/compiler/Compiler.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>

namespace breco::lang {

BrecoLangLibrary::BrecoLangLibrary(QString directory) {
    setDirectory(directory.isEmpty() ? defaultDirectory() : directory);
}

void BrecoLangLibrary::setDirectory(const QString& directory) {
    m_directory = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
}

BrecoLangLibraryContents BrecoLangLibrary::scan() const {
    BrecoLangLibraryContents contents;
    const QDir root(m_directory);
    if (!root.exists()) {
        return contents;
    }

    QDirIterator schemas(m_directory, {QStringLiteral("*.breco")},
                         QDir::Files, QDirIterator::Subdirectories);
    while (schemas.hasNext()) {
        BrecoLangLibraryFile item;
        item.filePath = QFileInfo(schemas.next()).absoluteFilePath();
        item.relativePath = root.relativeFilePath(item.filePath);
        QFile file(item.filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            item.errorMessage = file.errorString();
        } else {
            const CompileResult compiled = compileBrecoLang(
                QString::fromUtf8(file.readAll()), item.filePath);
            if (compiled.success()) {
                for (const EntryDesc& entry : compiled.program->entries) {
                    item.entries.push_back(compiled.program->symbol(entry.name));
                }
            } else if (!compiled.diagnostics.isEmpty()) {
                item.errorMessage = compiled.diagnostics.first().message;
            }
        }
        contents.schemas.push_back(std::move(item));
    }

    QDirIterator older(
        m_directory,
        {QStringLiteral("*.breco") + QStringLiteral("struct"),
         QStringLiteral("*.breco") + QStringLiteral("script")},
        QDir::Files, QDirIterator::Subdirectories);
    while (older.hasNext()) {
        contents.filesNeedingMigration.push_back(
            root.relativeFilePath(older.next()));
    }

    std::sort(contents.schemas.begin(), contents.schemas.end(),
              [](const auto& left, const auto& right) {
                  return left.relativePath.compare(right.relativePath,
                                                   Qt::CaseInsensitive) < 0;
              });
    std::sort(contents.filesNeedingMigration.begin(),
              contents.filesNeedingMigration.end(),
              [](const QString& left, const QString& right) {
                  return left.compare(right, Qt::CaseInsensitive) < 0;
              });
    return contents;
}

QString BrecoLangLibrary::defaultDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("schemas"));
}

bool BrecoLangLibrary::ensureDirectory(const QString& directory,
                                       QString* errorMessage) {
    QDir dir;
    if (dir.mkpath(directory)) {
        return true;
    }
    if (errorMessage != nullptr) {
        *errorMessage =
            QStringLiteral("Could not create schema directory '%1'").arg(directory);
    }
    return false;
}

}  // namespace breco::lang
