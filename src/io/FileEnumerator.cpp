#include "io/FileEnumerator.h"

#include <QDirIterator>
#include <QFileInfo>

namespace breco {

QVector<QString> FileEnumerator::enumerateSingleFile(const QString& path) {
    QVector<QString> result;
    QFileInfo info(path);
    if (info.exists() && info.isReadable() && info.isFile()) {
        result.push_back(info.absoluteFilePath());
    }
    return result;
}

QVector<QString> FileEnumerator::enumerateRecursive(const QString& directoryPath) {
    QVector<QString> files;
    QDirIterator it(directoryPath, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        files.push_back(it.next());
    }
    return files;
}

}  // namespace breco
