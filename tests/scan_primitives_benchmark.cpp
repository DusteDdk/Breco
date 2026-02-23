#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QDebug>

#include "scan/MatchUtils.h"
#include "scan/ShiftTransform.h"

namespace {

QByteArray makeAsciiData(int bytes, quint32 seed) {
    QByteArray out;
    out.resize(bytes);
    QRandomGenerator rng(seed);
    for (int i = 0; i < bytes; ++i) {
        const int v = static_cast<int>(rng.generate() % 52U);
        out[i] = (v < 26) ? static_cast<char>('A' + v) : static_cast<char>('a' + (v - 26));
    }
    return out;
}

QByteArray makeBinaryData(int bytes, quint32 seed) {
    QByteArray out;
    out.resize(bytes);
    QRandomGenerator rng(seed);
    for (int i = 0; i < bytes; ++i) {
        out[i] = static_cast<char>(rng.generate() & 0xFFU);
    }
    return out;
}

void benchmarkMatchUtils(const QByteArray& haystack, const QByteArray& needle,
                         breco::TextInterpretationMode mode, bool ignoreCase,
                         const char* label) {
    QElapsedTimer timer;
    timer.start();

    int matches = 0;
    int from = 0;
    while (true) {
        const int pos = breco::MatchUtils::indexOf(haystack, needle, from, mode, ignoreCase);
        if (pos < 0) {
            break;
        }
        ++matches;
        from = pos + 1;
    }

    const qint64 ns = timer.nsecsElapsed();
    const double sec = static_cast<double>(ns) / 1e9;
    const double mib = static_cast<double>(haystack.size()) / (1024.0 * 1024.0);
    const double mibPerSec = (sec > 0.0) ? (mib / sec) : 0.0;

    qInfo().noquote() << QStringLiteral("%1: scan=%2 MiB time=%3 ms throughput=%4 MiB/s matches=%5")
                             .arg(QString::fromLatin1(label))
                             .arg(QString::number(mib, 'f', 2))
                             .arg(QString::number(sec * 1000.0, 'f', 2))
                             .arg(QString::number(mibPerSec, 'f', 2))
                             .arg(matches);
}

void benchmarkShiftTransform(const QByteArray& raw, const breco::ShiftSettings& shift,
                             const char* label) {
    QElapsedTimer timer;
    timer.start();
    const QByteArray out = breco::ShiftTransform::transformWindow(
        raw, 0, 0, static_cast<quint64>(raw.size()), static_cast<quint64>(raw.size()), shift);
    const qint64 ns = timer.nsecsElapsed();

    quint64 checksum = 0;
    for (const char ch : out) {
        checksum += static_cast<unsigned char>(ch);
    }

    const double sec = static_cast<double>(ns) / 1e9;
    const double mib = static_cast<double>(raw.size()) / (1024.0 * 1024.0);
    const double mibPerSec = (sec > 0.0) ? (mib / sec) : 0.0;

    qInfo().noquote() << QStringLiteral("%1: size=%2 MiB time=%3 ms throughput=%4 MiB/s checksum=%5")
                             .arg(QString::fromLatin1(label))
                             .arg(QString::number(mib, 'f', 2))
                             .arg(QString::number(sec * 1000.0, 'f', 2))
                             .arg(QString::number(mibPerSec, 'f', 2))
                             .arg(checksum);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    constexpr int kMatchBytes = 32 * 1024 * 1024;
    QByteArray haystack = makeAsciiData(kMatchBytes, 2026U);
    const QByteArray needle = QByteArrayLiteral("AbCdEf");

    for (int pos = 8192; pos + needle.size() < haystack.size(); pos += 131072) {
        for (int i = 0; i < needle.size(); ++i) {
            haystack[pos + i] = needle.at(i);
        }
    }

    benchmarkMatchUtils(haystack, needle, breco::TextInterpretationMode::Ascii, false,
                        "MatchUtils exact");
    benchmarkMatchUtils(haystack, QByteArrayLiteral("abcdef"), breco::TextInterpretationMode::Ascii,
                        true, "MatchUtils ignore-case");

    constexpr int kShiftBytes = 24 * 1024 * 1024;
    const QByteArray raw = makeBinaryData(kShiftBytes, 9001U);

    benchmarkShiftTransform(raw, breco::ShiftSettings{1, breco::ShiftUnit::Bytes},
                            "ShiftTransform byte+1");
    benchmarkShiftTransform(raw, breco::ShiftSettings{-3, breco::ShiftUnit::Bits},
                            "ShiftTransform bit-3");

    return 0;
}
