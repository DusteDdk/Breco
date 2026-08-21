#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QStringList>

#include <iostream>
#include <algorithm>
#include <memory>
#include <optional>

#include "brecolang/compiler/Compiler.h"
#include "brecolang/render/OutformRenderer.h"
#include "brecolang/runtime/Interpreter.h"

namespace {

struct Options {
    QString schemaPath;
    QHash<QString, QString> inputs;
    QString entryName;
    QString outformName;
    QString outputPath;
    quint64 offset = 0;
    bool help = false;
};

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  brecodump --schema FILE --input NAME=FILE [--input NAME=FILE ...]\n"
        << "            [--entry NAME] [--offset BYTES] [--outform NAME]\n"
        << "            [--output FILE]\n\n"
        << "Compiles a BrecoLang 0.1 schema and decodes its selected entry. If\n"
        << "--entry is omitted, the schema's default entry is used. JSON is written\n"
        << "incrementally unless --outform selects a declared text or binary renderer.\n"
        << "Use '-' for one input stream or for output. File output is atomically\n"
        << "replaced only after decoding and rendering succeed.\n";
}

bool parseUnsigned(const QString& text, quint64* value) {
    bool ok = false;
    const quint64 parsed = text.toULongLong(&ok, 0);
    if (!ok || value == nullptr) {
        return false;
    }
    *value = parsed;
    return true;
}

std::optional<Options> parseOptions(const QStringList& args, QString* error) {
    Options options;
    for (int index = 1; index < args.size(); ++index) {
        const QString option = args.at(index);
        if (option == QStringLiteral("-h") || option == QStringLiteral("--help")) {
            options.help = true;
            return options;
        }
        const auto valueFor = [&](QString* value) -> bool {
            if (index + 1 >= args.size()) {
                if (error != nullptr) {
                    *error = QStringLiteral("Missing value for %1").arg(option);
                }
                return false;
            }
            *value = args.at(++index);
            return true;
        };

        QString value;
        if (option == QStringLiteral("--schema")) {
            if (!valueFor(&options.schemaPath)) return std::nullopt;
        } else if (option == QStringLiteral("--input")) {
            if (!valueFor(&value)) return std::nullopt;
            const qsizetype separator = value.indexOf(QLatin1Char('='));
            const QString name = value.left(separator).trimmed();
            const QString path = separator >= 0 ? value.mid(separator + 1) : QString();
            if (separator <= 0 || name.isEmpty() || path.isEmpty()) {
                if (error != nullptr) {
                    *error = QStringLiteral("--input must use NAME=FILE syntax");
                }
                return std::nullopt;
            }
            if (options.inputs.contains(name)) {
                if (error != nullptr) {
                    *error = QStringLiteral("Input '%1' was bound more than once")
                                 .arg(name);
                }
                return std::nullopt;
            }
            options.inputs.insert(name, path);
        } else if (option == QStringLiteral("--entry")) {
            if (!valueFor(&options.entryName) || options.entryName.isEmpty()) {
                if (error != nullptr) {
                    *error = QStringLiteral("Entry name cannot be empty");
                }
                return std::nullopt;
            }
        } else if (option == QStringLiteral("--outform")) {
            if (!valueFor(&options.outformName) || options.outformName.isEmpty()) {
                if (error != nullptr) {
                    *error = QStringLiteral("Outform name cannot be empty");
                }
                return std::nullopt;
            }
        } else if (option == QStringLiteral("--output")) {
            if (!valueFor(&options.outputPath)) return std::nullopt;
        } else if (option == QStringLiteral("--offset")) {
            if (!valueFor(&value) || !parseUnsigned(value, &options.offset)) {
                if (error != nullptr) {
                    *error = QStringLiteral("Invalid byte offset: %1").arg(value);
                }
                return std::nullopt;
            }
        } else {
            if (error != nullptr) {
                *error = QStringLiteral("Unknown argument: %1").arg(option);
            }
            return std::nullopt;
        }
    }
    if (options.schemaPath.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("Missing required --schema FILE");
        }
        return std::nullopt;
    }
    if (options.schemaPath == QStringLiteral("-") &&
        std::any_of(options.inputs.cbegin(), options.inputs.cend(),
                    [](const QString& path) { return path == QStringLiteral("-"); })) {
        if (error != nullptr) {
            *error = QStringLiteral("Schema and binary input cannot both use stdin");
        }
        return std::nullopt;
    }
    int stdinInputs = 0;
    for (const QString& path : options.inputs) {
        if (path == QStringLiteral("-")) ++stdinInputs;
    }
    if (stdinInputs > 1) {
        if (error != nullptr) {
            *error = QStringLiteral("Only one declared input may use stdin");
        }
        return std::nullopt;
    }
    return options;
}

std::optional<QByteArray> readSchema(const QString& path, QString* error) {
    QFile file;
    if (path == QStringLiteral("-")) {
        if (!file.open(stdin, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) {
            if (error != nullptr) *error = file.errorString();
            return std::nullopt;
        }
    } else {
        file.setFileName(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error != nullptr) {
                *error = QStringLiteral("Could not open schema '%1': %2")
                             .arg(path, file.errorString());
            }
            return std::nullopt;
        }
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (error != nullptr) *error = file.errorString();
        return std::nullopt;
    }
    return bytes;
}

QString compileDiagnostics(const QVector<breco::lang::Diagnostic>& diagnostics) {
    QStringList messages;
    for (const breco::lang::Diagnostic& diagnostic : diagnostics) {
        messages.push_back(QStringLiteral("%1: %2 (source offset %3)")
                               .arg(diagnostic.code, diagnostic.message)
                               .arg(diagnostic.span.start));
    }
    return messages.join(QLatin1Char('\n'));
}

QString runtimeDiagnostics(
    const QVector<breco::lang::RuntimeDiagnostic>& diagnostics) {
    QStringList messages;
    for (const breco::lang::RuntimeDiagnostic& diagnostic : diagnostics) {
        messages.push_back(
            QStringLiteral("%1: %2").arg(diagnostic.code, diagnostic.message));
    }
    return messages.join(QLatin1Char('\n'));
}

QStringList entryNames(const breco::lang::BrecoProgram& program) {
    QStringList names;
    for (const breco::lang::EntryDesc& entry : program.entries) {
        names.push_back(program.symbol(entry.name));
    }
    return names;
}

QStringList inputNames(const breco::lang::BrecoProgram& program) {
    QStringList names;
    for (const breco::lang::InputDesc& input : program.inputs) {
        names.push_back(program.symbol(input.name));
    }
    return names;
}

QStringList outformNames(const breco::lang::BrecoProgram& program) {
    QStringList names;
    for (const breco::lang::OutformDesc& outform : program.outforms) {
        names.push_back(program.symbol(outform.name));
    }
    return names;
}

class OutputDestination {
public:
    bool open(const QString& path, QString* error) {
        if (path.isEmpty() || path == QStringLiteral("-")) {
            if (!m_stdout.open(stdout, QIODevice::WriteOnly,
                               QFileDevice::DontCloseHandle)) {
                if (error != nullptr) *error = m_stdout.errorString();
                return false;
            }
            m_device = &m_stdout;
            return true;
        }
        m_file = std::make_unique<QSaveFile>(path);
        m_file->setDirectWriteFallback(false);
        if (!m_file->open(QIODevice::WriteOnly)) {
            if (error != nullptr) {
                *error = QStringLiteral("Could not stage output '%1': %2")
                             .arg(path, m_file->errorString());
            }
            m_file.reset();
            return false;
        }
        m_device = m_file.get();
        return true;
    }

    QIODevice* device() const { return m_device; }

    bool commit(QString* error) {
        if (m_file) {
            if (!m_file->commit()) {
                if (error != nullptr) *error = m_file->errorString();
                return false;
            }
            return true;
        }
        if (!m_stdout.flush()) {
            if (error != nullptr) *error = m_stdout.errorString();
            return false;
        }
        return true;
    }

    void cancel() {
        if (m_file && m_file->isOpen()) m_file->cancelWriting();
    }

private:
    QFile m_stdout;
    std::unique_ptr<QSaveFile> m_file;
    QIODevice* m_device = nullptr;
};

int run(const Options& options) {
    QString error;
    const std::optional<QByteArray> schema = readSchema(options.schemaPath, &error);
    if (!schema.has_value()) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }
    const breco::lang::CompileResult compiled = breco::lang::compileBrecoLang(
        QString::fromUtf8(*schema),
        options.schemaPath == QStringLiteral("-")
            ? QStringLiteral("<stdin>")
            : QFileInfo(options.schemaPath).absoluteFilePath());
    if (!compiled.success()) {
        std::cerr << compileDiagnostics(compiled.diagnostics).toStdString() << '\n';
        return 2;
    }

    const auto& program = *compiled.program;
    const QStringList entries = entryNames(program);
    const QStringList inputs = inputNames(program);
    const QStringList outforms = outformNames(program);
    QString entryName = options.entryName;
    if (entryName.isEmpty() && program.defaultEntry != breco::lang::kInvalidId) {
        entryName = program.symbol(program.defaultEntry);
    }
    if (!entries.contains(entryName)) {
        std::cerr << (entryName.isEmpty() ? "No entry was selected"
                                          : "Unknown entry '" +
                                                entryName.toStdString() + "'")
                  << ". Available entries: "
                  << entries.join(QStringLiteral(", ")).toStdString() << '\n';
        return 2;
    }
    if (!options.outformName.isEmpty() &&
        !outforms.contains(options.outformName)) {
        std::cerr << "Unknown outform '" << options.outformName.toStdString()
                  << "'. Available outforms: "
                  << outforms.join(QStringLiteral(", ")).toStdString() << '\n';
        return 2;
    }

    QVector<std::shared_ptr<breco::lang::ByteSource>> sources(program.inputs.size());
    std::shared_ptr<QFile> stdinDevice;
    for (auto binding = options.inputs.cbegin(); binding != options.inputs.cend();
         ++binding) {
        const qsizetype input = inputs.indexOf(binding.key());
        if (input < 0) {
            std::cerr << "Unknown input '" << binding.key().toStdString()
                      << "'. Available inputs: "
                      << inputs.join(QStringLiteral(", ")).toStdString() << '\n';
            return 2;
        }
        if (binding.value() == QStringLiteral("-")) {
            stdinDevice = std::make_shared<QFile>();
            if (!stdinDevice->open(stdin, QIODevice::ReadOnly,
                                   QFileDevice::DontCloseHandle)) {
                std::cerr << stdinDevice->errorString().toStdString() << '\n';
                return 1;
            }
            sources[static_cast<breco::lang::InputId>(input)] =
                std::make_shared<breco::lang::SpoolingSource>(stdinDevice,
                                                              QStringLiteral("<stdin>"));
        } else {
            QString sourceError;
            sources[static_cast<breco::lang::InputId>(input)] =
                breco::lang::PagedFileSource::open(binding.value(), &sourceError);
            if (!sources[static_cast<breco::lang::InputId>(input)]) {
                std::cerr << "Could not bind input '" << binding.key().toStdString()
                          << "': " << sourceError.toStdString() << '\n';
                return 1;
            }
        }
    }

    OutputDestination output;
    if (!output.open(options.outputPath, &error)) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }

    breco::lang::DecodeRequest request;
    request.program = compiled.program;
    request.entryName = entryName;
    request.inputs = sources;
    request.startOffset = options.offset;
    request.mode = options.outformName.isEmpty()
                       ? breco::lang::DecodeMode::Streaming
                       : breco::lang::DecodeMode::Tree;
    request.output = options.outformName.isEmpty() ? output.device() : nullptr;
    const breco::lang::DecodeResult decoded =
        breco::lang::decodeBrecoProgram(request);
    if (!decoded.success()) {
        const QString messages = runtimeDiagnostics(decoded.diagnostics);
        std::cerr << (messages.isEmpty() ? "Decode failed" : messages.toStdString())
                  << '\n';
        std::cerr << "Declared inputs: "
                  << inputs.join(QStringLiteral(", ")).toStdString() << '\n';
        output.cancel();
        return 1;
    }

    if (options.outformName.isEmpty()) {
        if (output.device()->write("\n", 1) != 1) {
            std::cerr << output.device()->errorString().toStdString() << '\n';
            output.cancel();
            return 1;
        }
    } else {
        const breco::lang::RenderStore store(compiled.program, decoded.tree, sources,
                                              decoded.rootValue);
        const breco::lang::OutformRenderResult rendered =
            breco::lang::renderOutform(store, options.outformName,
                                       output.device());
        if (!rendered.success) {
            std::cerr << rendered.error.toStdString() << '\n';
            output.cancel();
            return 1;
        }
    }
    if (!output.commit(&error)) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("brecodump"));

    QString error;
    const std::optional<Options> options =
        parseOptions(QCoreApplication::arguments(), &error);
    if (!options.has_value()) {
        std::cerr << error.toStdString() << "\n\n";
        printUsage();
        return 2;
    }
    if (options->help) {
        printUsage();
        return 0;
    }
    return run(*options);
}
