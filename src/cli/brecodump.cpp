#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QStringList>

#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

#include "io/OpenFilePool.h"
#include "io/ShiftedWindowLoader.h"
#include "model/ResultTypes.h"
#include "struct/StructDeclarationParser.h"
#include "struct/StructExport.h"
#include "struct/StructVisualizer.h"
#include "struct/VisualizedNode.h"

namespace {

struct Options {
    QString structDeclarationPath;
    QString inputPath;
    QString outputPath;
    QString entryName;
    quint64 offset = 0;
    int bitshift = 0;
    int repeat = 1;
    bool help = false;
};

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  brecodump [-s STRUCT_DECLARATION_FILE] -i BINARY_FILE_NAME "
           "[-e ENTRY_NAME=last] [-ofs BYTE_OFFSET=0] [-bs BITSHIFT=0] "
           "[-r REPEATNUM=1] "
           "[-o OUTPUT_FILE=stdout] [-h|--help]\n\n"
        << "Purpose:\n"
        << "  Dump binary data through breco's Struct Declaration parser and visualizer.\n"
        << "  The declaration uses the same language as the Struct Declaration text area.\n\n"
        << "Behavior:\n"
        << "  With -s, the declaration is read from that file. Without -s, the\n"
        << "  declaration is read from stdin so agents can pipe declarations directly.\n"
        << "  -e selects a visualizable entry by name. Without -e, the last visualizable\n"
        << "  declaration in source order is decoded. Decoding starts at the selected\n"
        << "  byte offset and uses the requested repeat count.\n\n"
        << "Output format:\n"
        << "  Pretty JSON. metadata contains the input parameters and source info.\n"
        << "  The other top-level key is the decoded struct or field name. Each node\n"
        << "  contains focused data such as value, type, rawBytesHex, bytesMissing,\n"
        << "  error, and nested struct/field names. Fields retain declaration order;\n"
        << "  valid appears directly after value for /cond fields and /assert nodes.\n"
        << "  Endianness is\n"
        << "  emitted only when explicitly decorated. Struct members are nested in a\n"
        << "  value object, and repeated values are nested in a value array.\n";
}

bool parseUnsigned(const QString& text, quint64* value) {
    if (value == nullptr) {
        return false;
    }
    bool ok = false;
    const quint64 parsed = text.toULongLong(&ok, 0);
    if (!ok) {
        return false;
    }
    *value = parsed;
    return true;
}

bool parseSignedInt(const QString& text, int* value) {
    if (value == nullptr) {
        return false;
    }
    bool ok = false;
    const qlonglong parsed = text.toLongLong(&ok, 0);
    if (!ok || parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

bool parsePositiveInt(const QString& text, int* value) {
    int parsed = 0;
    if (!parseSignedInt(text, &parsed) || parsed < 1) {
        return false;
    }
    *value = parsed;
    return true;
}

std::optional<Options> parseOptions(const QStringList& args, QString* error) {
    Options options;
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        if (arg == QStringLiteral("-h") || arg == QStringLiteral("--help")) {
            options.help = true;
            return options;
        }

        const auto requireValue = [&](QString* value) -> bool {
            if (i + 1 >= args.size()) {
                if (error != nullptr) {
                    *error = QStringLiteral("Missing value for %1").arg(arg);
                }
                return false;
            }
            *value = args.at(++i);
            return true;
        };

        QString value;
        if (arg == QStringLiteral("-s")) {
            if (!requireValue(&options.structDeclarationPath)) {
                return std::nullopt;
            }
        } else if (arg == QStringLiteral("-i")) {
            if (!requireValue(&options.inputPath)) {
                return std::nullopt;
            }
        } else if (arg == QStringLiteral("-e")) {
            if (!requireValue(&options.entryName)) {
                return std::nullopt;
            }
            if (options.entryName.isEmpty()) {
                if (error != nullptr) {
                    *error = QStringLiteral("Entry name cannot be empty");
                }
                return std::nullopt;
            }
        } else if (arg == QStringLiteral("-ofs")) {
            if (!requireValue(&value) || !parseUnsigned(value, &options.offset)) {
                if (error != nullptr) {
                    *error = QStringLiteral("Invalid byte offset: %1").arg(value);
                }
                return std::nullopt;
            }
        } else if (arg == QStringLiteral("-bs")) {
            if (!requireValue(&value) || !parseSignedInt(value, &options.bitshift)) {
                if (error != nullptr) {
                    *error = QStringLiteral("Invalid bitshift: %1").arg(value);
                }
                return std::nullopt;
            }
        } else if (arg == QStringLiteral("-r")) {
            if (!requireValue(&value) || !parsePositiveInt(value, &options.repeat)) {
                if (error != nullptr) {
                    *error = QStringLiteral("Invalid repeat count: %1").arg(value);
                }
                return std::nullopt;
            }
        } else if (arg == QStringLiteral("-o")) {
            if (!requireValue(&options.outputPath)) {
                return std::nullopt;
            }
        } else {
            if (error != nullptr) {
                *error = QStringLiteral("Unknown argument: %1").arg(arg);
            }
            return std::nullopt;
        }
    }

    if (options.inputPath.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("Missing required -i BINARY_FILE_NAME");
        }
        return std::nullopt;
    }
    return options;
}

std::optional<QByteArray> readAllFile(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not open '%1': %2")
                         .arg(path, file.errorString());
        }
        return std::nullopt;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not read '%1': %2")
                         .arg(path, file.errorString());
        }
        return std::nullopt;
    }
    return bytes;
}

QByteArray readAllStdin() {
    const std::string input((std::istreambuf_iterator<char>(std::cin)),
                            std::istreambuf_iterator<char>());
    return QByteArray(input.data(), static_cast<qsizetype>(input.size()));
}

bool writeOutput(const QByteArray& bytes, const QString& outputPath, QString* error) {
    if (outputPath.isEmpty() || outputPath == QStringLiteral("-") ||
        outputPath == QStringLiteral("stdout")) {
        std::cout.write(bytes.constData(), bytes.size());
        std::cout << '\n';
        return std::cout.good();
    }

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not open output '%1': %2")
                         .arg(outputPath, output.errorString());
        }
        return false;
    }
    if (output.write(bytes) != bytes.size()) {
        if (error != nullptr) {
            *error = QStringLiteral("Could not write output '%1': %2")
                         .arg(outputPath, output.errorString());
        }
        return false;
    }
    output.write("\n", 1);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("brecodump"));

    QString error;
    const std::optional<Options> maybeOptions =
        parseOptions(QCoreApplication::arguments(), &error);
    if (!maybeOptions.has_value()) {
        std::cerr << error.toStdString() << "\n\n";
        printUsage();
        return 2;
    }

    const Options options = *maybeOptions;
    if (options.help) {
        printUsage();
        return 0;
    }

    const QFileInfo inputInfo(options.inputPath);
    if (!inputInfo.exists() || !inputInfo.isFile() || inputInfo.size() < 0) {
        std::cerr << "Input file is not a readable regular file: "
                  << options.inputPath.toStdString() << '\n';
        return 2;
    }

    const quint64 fileSize = static_cast<quint64>(inputInfo.size());
    if (options.offset > fileSize) {
        std::cerr << "Byte offset is past end of input\n";
        return 2;
    }

    breco::OpenFilePool filePool;
    breco::ShiftedWindowLoader loader(&filePool);
    breco::ShiftSettings shift;
    shift.amount = options.bitshift;
    shift.unit = breco::ShiftUnit::Bits;
    const quint64 windowSize = fileSize - options.offset;
    const std::optional<QByteArray> bytes =
        loader.loadTransformedWindow(inputInfo.absoluteFilePath(), fileSize,
                                     options.offset, windowSize, shift);
    if (!bytes.has_value()) {
        std::cerr << "Could not read input bytes\n";
        return 1;
    }

    QJsonObject metadata;
    metadata.insert(QStringLiteral("tool"), QStringLiteral("brecodump"));
    metadata.insert(QStringLiteral("input"), inputInfo.absoluteFilePath());
    metadata.insert(QStringLiteral("offset"), QString::number(options.offset));
    metadata.insert(QStringLiteral("bitshift"), options.bitshift);
    metadata.insert(QStringLiteral("repeat"), options.repeat);
    metadata.insert(QStringLiteral("byteCount"), bytes->size());

    std::optional<QByteArray> declarationBytes;
    if (options.structDeclarationPath.isEmpty()) {
        metadata.insert(QStringLiteral("declarationSource"), QStringLiteral("stdin"));
        declarationBytes = readAllStdin();
    } else {
        const QFileInfo declarationInfo(options.structDeclarationPath);
        metadata.insert(QStringLiteral("declarationSource"),
                        declarationInfo.absoluteFilePath());
        declarationBytes = readAllFile(options.structDeclarationPath, &error);
    }
    if (!declarationBytes.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }

    const breco::ParseResult parsed =
        breco::parseStructDeclaration(QString::fromUtf8(*declarationBytes));
    if (!parsed.valid) {
        std::cerr << "Invalid struct declaration";
        if (!parsed.errorMessage.isEmpty()) {
            std::cerr << ": " << parsed.errorMessage.toStdString();
        }
        std::cerr << '\n';
        return 2;
    }

    const QStringList entryNames = parsed.graph.entryNames();
    if (entryNames.isEmpty()) {
        std::cerr << "Struct declaration has no visualizable entries\n";
        return 2;
    }

    QString entryName = options.entryName;
    if (entryName.isEmpty()) {
        entryName = entryNames.first();
        int latestSourcePosition =
            parsed.graph.nameRangeForEntry(entryName).start;
        for (const QString& candidate : entryNames) {
            const int sourcePosition =
                parsed.graph.nameRangeForEntry(candidate).start;
            if (sourcePosition > latestSourcePosition) {
                entryName = candidate;
                latestSourcePosition = sourcePosition;
            }
        }
    } else if (!parsed.graph.isVisualizableEntryName(entryName)) {
        std::cerr << "Unknown entry '" << entryName.toStdString()
                  << "'. Available entries: "
                  << entryNames.join(QStringLiteral(", ")).toStdString() << '\n';
        return 2;
    }
    metadata.insert(QStringLiteral("entrypoint"), entryName);

    const breco::VisualizedNode visualization =
        breco::visualize(parsed.graph, entryName, *bytes, 0, options.repeat);
    const bool singleDecodedNode =
        visualization.children.size() == 1 && options.repeat == 1;
    const QByteArray json =
        serializeDump(metadata, entryName,
                      singleDecodedNode ? visualization.children.first()
                                        : visualization);
    if (!writeOutput(json, options.outputPath, &error)) {
        if (!error.isEmpty()) {
            std::cerr << error.toStdString() << '\n';
        }
        return 1;
    }
    return 0;
}
