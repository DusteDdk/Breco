#pragma once

#include <QVector>

namespace breco {

class FileEnumerator {
public:
    static QVector<QString> enumerateSingleFile(const QString& path);
    static QVector<QString> enumerateRecursive(const QString& directoryPath);
};

}  // namespace breco
