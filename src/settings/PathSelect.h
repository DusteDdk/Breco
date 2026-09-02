#pragma once

#include <QString>

class QWidget;

namespace breco {

enum class PathSelectActivity : int {
    OpenMainFile = 1,
    OpenScanDir,
    SaveBinaryRange,
    SaveQueuedEditsAs,
    SaveDecodedJson,
    SaveDecodedBinary,
    SaveOutform,
    OpenBrecoLangSchema,
    BindBrecoLangInput,
    OpenSchemaLibraryDir,
    SaveEmbeddedImage,
};

enum class PathSelectKind {
    OpenFile,
    OpenDirectory,
    SaveFile,
};

QString lastResortDirectory();
QString resolveStartPath(const QString& storedPath);
QString resolveStartPath(const QString& storedPath,
                         const QString& lastResort);
QString lastSelectedPathFor(PathSelectActivity activity);
void setLastSelectedPathFor(PathSelectActivity activity,
                            const QString& path);

QString suggestedQueuedEditsSaveAsPath(const QString& sourcePath);
QString suggestedQueuedEditsSaveAsPath(const QString& sourcePath,
                                       const QString& lastResort);

QString promptForPath(QWidget* parent, PathSelectActivity activity,
                      PathSelectKind kind, const QString& caption,
                      const QString& filter = {},
                      const QString& initialPath = {});

}  // namespace breco
