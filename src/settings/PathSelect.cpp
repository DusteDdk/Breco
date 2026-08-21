#include "settings/PathSelect.h"

#include "settings/AppSettings.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStringList>

#include <cstdio>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace breco {

namespace {

QString activityKey(PathSelectActivity activity) {
    switch (activity) {
        case PathSelectActivity::OpenMainFile:
            return QStringLiteral("OpenMainFile");
        case PathSelectActivity::OpenScanDir:
            return QStringLiteral("OpenScanDir");
        case PathSelectActivity::SaveBinaryRange:
            return QStringLiteral("SaveBinaryRange");
        case PathSelectActivity::SaveQueuedEditsAs:
            return QStringLiteral("SaveQueuedEditsAs");
        case PathSelectActivity::SaveDecodedJson:
            return QStringLiteral("SaveDecodedJson");
        case PathSelectActivity::SaveDecodedBinary:
            return QStringLiteral("SaveDecodedBinary");
        case PathSelectActivity::SaveOutform:
            return QStringLiteral("SaveOutform");
        case PathSelectActivity::OpenBrecoLangSchema:
            return QStringLiteral("OpenBrecoLangSchema");
        case PathSelectActivity::BindBrecoLangInput:
            return QStringLiteral("BindBrecoLangInput");
        case PathSelectActivity::OpenSchemaLibraryDir:
            return QStringLiteral("OpenSchemaLibraryDir");
        case PathSelectActivity::SaveEmbeddedImage:
            return QStringLiteral("SaveEmbeddedImage");
    }
    return QString::number(static_cast<int>(activity));
}

bool hasInteractiveTerminal() {
#ifdef Q_OS_WIN
    const auto isTerminal = [](FILE* stream) {
        const int descriptor = _fileno(stream);
        return descriptor >= 0 && _isatty(descriptor) != 0;
    };
    if (isTerminal(stdin) || isTerminal(stdout) || isTerminal(stderr) ||
        GetConsoleWindow() != nullptr) {
        return true;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS) != FALSE) {
        FreeConsole();
        return true;
    }
    return false;
#elif defined(Q_OS_UNIX)
    return isatty(STDIN_FILENO) != 0 ||
           isatty(STDOUT_FILENO) != 0 ||
           isatty(STDERR_FILENO) != 0;
#else
    return false;
#endif
}

QString resolveStartPathImpl(const QString& storedPath,
                             const QString& lastResort) {
    if (storedPath.trimmed().isEmpty()) {
        return lastResort;
    }

    const QFileInfo storedInfo(storedPath);
    if (storedInfo.exists()) {
        return storedPath;
    }

    const QString originalAbsolutePath =
        QDir::cleanPath(storedInfo.absoluteFilePath());
    const bool originalDirectlyUnderRoot =
        QFileInfo(originalAbsolutePath).absolutePath() == QDir::rootPath();
    QString candidate = QFileInfo(originalAbsolutePath).absolutePath();

    while (!candidate.isEmpty()) {
        const QFileInfo candidateInfo(candidate);
        if (candidateInfo.exists() && candidateInfo.isDir()) {
#ifdef Q_OS_UNIX
            if (QDir::cleanPath(candidate) == QDir::rootPath() &&
                !originalDirectlyUnderRoot) {
                return lastResort;
            }
#endif
            return candidateInfo.absoluteFilePath();
        }

        const QString parent = candidateInfo.absolutePath();
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }
    return lastResort;
}

QString editedFileName(const QString& sourcePath) {
    const QString fileName = QFileInfo(sourcePath).fileName();
    if (fileName.isEmpty()) {
        return QString();
    }
    const int extensionSeparator = fileName.lastIndexOf(QLatin1Char('.'));
    if (extensionSeparator <= 0) {
        return fileName + QStringLiteral(".edited");
    }
    return fileName.left(extensionSeparator) + QStringLiteral(".edited") +
           fileName.mid(extensionSeparator);
}

}  // namespace

QString lastResortDirectory() {
    static const bool interactiveTerminal = hasInteractiveTerminal();
    return interactiveTerminal ? QDir::currentPath() : QDir::homePath();
}

QString resolveStartPath(const QString& storedPath) {
    return resolveStartPathImpl(storedPath, lastResortDirectory());
}

QString resolveStartPath(const QString& storedPath,
                         const QString& lastResort) {
    return resolveStartPathImpl(storedPath, lastResort);
}

QString lastSelectedPathFor(PathSelectActivity activity) {
    return resolveStartPath(
        AppSettings::lastSelectedPath(activityKey(activity)));
}

void setLastSelectedPathFor(PathSelectActivity activity,
                            const QString& path) {
    AppSettings::setLastSelectedPath(activityKey(activity), path);
}

QString suggestedQueuedEditsSaveAsPath(const QString& sourcePath) {
    return suggestedQueuedEditsSaveAsPath(sourcePath,
                                          lastResortDirectory());
}

QString suggestedQueuedEditsSaveAsPath(const QString& sourcePath,
                                       const QString& lastResort) {
    const QFileInfo sourceInfo(sourcePath);
    const QString directory = sourceInfo.exists()
                                  ? sourceInfo.absolutePath()
                                  : resolveStartPath(sourcePath, lastResort);
    const QString suggestion = editedFileName(sourcePath);
    return suggestion.isEmpty() ? directory
                                : QDir(directory).filePath(suggestion);
}

QString promptForPath(QWidget* parent, PathSelectActivity activity,
                      PathSelectKind kind, const QString& caption,
                      const QString& filter, const QString& initialPath) {
    const QString startPath =
        initialPath.isEmpty() ? lastSelectedPathFor(activity) : initialPath;
    QString selectedPath;

    switch (kind) {
        case PathSelectKind::OpenFile:
            selectedPath = QFileDialog::getOpenFileName(
                parent, caption, startPath, filter);
            break;
        case PathSelectKind::SaveFile:
            selectedPath = QFileDialog::getSaveFileName(
                parent, caption, startPath, filter);
            break;
        case PathSelectKind::OpenDirectory: {
            QFileDialog dialog(parent, caption, startPath, filter);
            dialog.setAcceptMode(QFileDialog::AcceptOpen);
            dialog.setFileMode(QFileDialog::Directory);
            dialog.setOption(QFileDialog::ShowDirsOnly, false);
            if (dialog.exec() != QDialog::Accepted) {
                return QString();
            }
            const QStringList selectedFiles = dialog.selectedFiles();
            if (!selectedFiles.isEmpty()) {
                selectedPath = selectedFiles.constFirst();
                const QFileInfo selectedInfo(selectedPath);
                if (selectedInfo.isFile()) {
                    selectedPath = selectedInfo.absolutePath();
                }
            }
            break;
        }
    }

    if (!selectedPath.isEmpty()) {
        setLastSelectedPathFor(activity, selectedPath);
    }
    return selectedPath;
}

}  // namespace breco
