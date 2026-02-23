#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QDebug>

#include "text/TextSequenceAnalyzer.h"

namespace {

QByteArray makeData(int bytes, quint32 seed) {
    QByteArray out;
    out.resize(bytes);
    QRandomGenerator rng(seed);
    for (int i = 0; i < bytes; ++i) {
        out[i] = static_cast<char>(rng.generate() & 0xFFU);
    }
    return out;
}

void runCase(const QByteArray& data, breco::TextInterpretationMode mode, const char* label) {
    QElapsedTimer timer;
    timer.start();
    const breco::TextAnalysisResult result = breco::TextSequenceAnalyzer::analyze(data, mode);
    const qint64 ns = timer.nsecsElapsed();
    const double sec = static_cast<double>(ns) / 1e9;
    const double mib = static_cast<double>(data.size()) / (1024.0 * 1024.0);
    const double mibPerSec = (sec > 0.0) ? (mib / sec) : 0.0;
    qInfo().noquote() << QStringLiteral("%1: size=%2 MiB time=%3 ms throughput=%4 MiB/s sequences=%5")
                             .arg(QString::fromLatin1(label))
                             .arg(QString::number(mib, 'f', 2))
                             .arg(QString::number(sec * 1000.0, 'f', 2))
                             .arg(QString::number(mibPerSec, 'f', 2))
                             .arg(result.sequences.size());
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    constexpr int kBytes = 16 * 1024 * 1024;
    const QByteArray data = makeData(kBytes, 1337U);

    runCase(data, breco::TextInterpretationMode::Ascii, "ASCII");
    runCase(data, breco::TextInterpretationMode::Utf8, "UTF-8");
    runCase(data, breco::TextInterpretationMode::Utf16, "UTF-16");

    return 0;
}
