#include "brecolang/compiler/Lexer.h"

#include <QChar>

#include <limits>

namespace breco::lang {
namespace {

bool isIdentifierStart(QChar ch) {
    return ch == QLatin1Char('_') || ch.isLetter();
}

bool isIdentifierPart(QChar ch) {
    return ch == QLatin1Char('_') || ch.isLetterOrNumber();
}

class Lexer {
public:
    Lexer(QStringView source, qsizetype baseOffset)
        : m_source(source), m_baseOffset(baseOffset) {}

    LexResult run() {
        while (!atEnd()) {
            skipTrivia();
            if (atEnd()) {
                break;
            }
            scanToken();
        }
        m_result.tokens.push_back(
            Token{TokenKind::End, {}, {m_baseOffset + m_position, 0}});
        return std::move(m_result);
    }

private:
    bool atEnd() const { return m_position >= m_source.size(); }
    QChar peek(qsizetype distance = 0) const {
        const qsizetype index = m_position + distance;
        return index >= 0 && index < m_source.size() ? m_source.at(index) : QChar();
    }
    QChar advance() { return atEnd() ? QChar() : m_source.at(m_position++); }

    bool match(QChar expected) {
        if (peek() != expected) {
            return false;
        }
        ++m_position;
        return true;
    }

    void add(TokenKind kind, qsizetype start, QString text = {}) {
        if (text.isNull()) {
            text = m_source.mid(start, m_position - start).toString();
        }
        m_result.tokens.push_back(
            Token{kind, std::move(text), {m_baseOffset + start, m_position - start}});
    }

    void error(const QString& code, const QString& message, qsizetype start,
               qsizetype length) {
        m_result.diagnostics.push_back(
            Diagnostic{DiagnosticSeverity::Error, code, message,
                       {m_baseOffset + start, length}, {}});
    }

    void skipTrivia() {
        for (;;) {
            while (peek().isSpace()) {
                ++m_position;
            }
            if (peek() == QLatin1Char('/') && peek(1) == QLatin1Char('/')) {
                m_position += 2;
                while (!atEnd() && peek() != QLatin1Char('\n')) {
                    ++m_position;
                }
                continue;
            }
            if (peek() == QLatin1Char('/') && peek(1) == QLatin1Char('*')) {
                const qsizetype start = m_position;
                m_position += 2;
                bool closed = false;
                while (!atEnd()) {
                    if (peek() == QLatin1Char('*') && peek(1) == QLatin1Char('/')) {
                        m_position += 2;
                        closed = true;
                        break;
                    }
                    ++m_position;
                }
                if (!closed) {
                    error(QStringLiteral("BL0002"),
                          QStringLiteral("Unterminated block comment"), start,
                          m_position - start);
                }
                continue;
            }
            break;
        }
    }

    void scanToken() {
        const qsizetype start = m_position;
        const QChar ch = advance();
        if (isIdentifierStart(ch)) {
            while (isIdentifierPart(peek())) {
                ++m_position;
            }
            add(TokenKind::Identifier, start);
            return;
        }
        if (ch.isDigit()) {
            scanNumber(start);
            return;
        }
        if (ch == QLatin1Char('"')) {
            scanString(start);
            return;
        }

        switch (ch.unicode()) {
            case '{': add(TokenKind::LeftBrace, start); return;
            case '}': add(TokenKind::RightBrace, start); return;
            case '(': add(TokenKind::LeftParen, start); return;
            case ')': add(TokenKind::RightParen, start); return;
            case '[': add(TokenKind::LeftBracket, start); return;
            case ']': add(TokenKind::RightBracket, start); return;
            case ':': add(TokenKind::Colon, start); return;
            case ',': add(TokenKind::Comma, start); return;
            case '@': add(TokenKind::At, start); return;
            case ';': add(TokenKind::Semicolon, start); return;
            case '+': add(TokenKind::Plus, start); return;
            case '-': add(TokenKind::Minus, start); return;
            case '*': add(TokenKind::Star, start); return;
            case '/': add(TokenKind::Slash, start); return;
            case '%': add(TokenKind::Percent, start); return;
            case '.':
                add(match(QLatin1Char('.')) ? TokenKind::DotDot : TokenKind::Dot,
                    start);
                return;
            case '!':
                add(match(QLatin1Char('=')) ? TokenKind::BangEqual : TokenKind::Bang,
                    start);
                return;
            case '=':
                if (match(QLatin1Char('>'))) {
                    add(TokenKind::FatArrow, start);
                } else {
                    add(match(QLatin1Char('=')) ? TokenKind::EqualEqual
                                                : TokenKind::Equal,
                        start);
                }
                return;
            case '<':
                add(match(QLatin1Char('=')) ? TokenKind::LessEqual : TokenKind::Less,
                    start);
                return;
            case '>':
                add(match(QLatin1Char('=')) ? TokenKind::GreaterEqual
                                            : TokenKind::Greater,
                    start);
                return;
            case '&':
                if (match(QLatin1Char('&'))) {
                    add(TokenKind::AndAnd, start);
                } else {
                    invalidCharacter(start, ch);
                }
                return;
            case '|':
                if (match(QLatin1Char('|'))) {
                    add(TokenKind::OrOr, start);
                } else {
                    invalidCharacter(start, ch);
                }
                return;
            default: invalidCharacter(start, ch); return;
        }
    }

    void invalidCharacter(qsizetype start, QChar ch) {
        error(QStringLiteral("BL0001"),
              QStringLiteral("Unexpected character '%1'").arg(ch), start, 1);
        add(TokenKind::Invalid, start);
    }

    void scanNumber(qsizetype start) {
        bool isFloat = false;
        if (m_source.mid(start, 2).compare(QStringView(u"0x"), Qt::CaseInsensitive) == 0) {
            ++m_position;
            while (peek().isDigit() ||
                   (peek().toLower() >= QLatin1Char('a') &&
                    peek().toLower() <= QLatin1Char('f')) ||
                   peek() == QLatin1Char('_')) {
                ++m_position;
            }
        } else {
            while (peek().isDigit() || peek() == QLatin1Char('_')) {
                ++m_position;
            }
            if (peek() == QLatin1Char('.') && peek(1) != QLatin1Char('.') &&
                peek(1).isDigit()) {
                isFloat = true;
                ++m_position;
                while (peek().isDigit() || peek() == QLatin1Char('_')) {
                    ++m_position;
                }
            }
            if (peek().toLower() == QLatin1Char('e')) {
                isFloat = true;
                ++m_position;
                if (peek() == QLatin1Char('+') || peek() == QLatin1Char('-')) {
                    ++m_position;
                }
                while (peek().isDigit() || peek() == QLatin1Char('_')) {
                    ++m_position;
                }
            }
        }
        while (peek().isLetter()) {
            ++m_position;
        }
        add(isFloat ? TokenKind::Float : TokenKind::Integer, start);
    }

    void scanString(qsizetype start) {
        const qsizetype contentStart = m_position;
        bool closed = false;
        while (!atEnd()) {
            const QChar ch = advance();
            if (ch == QLatin1Char('\\') && !atEnd()) {
                ++m_position;
                continue;
            }
            if (ch == QLatin1Char('"')) {
                closed = true;
                break;
            }
        }
        if (!closed) {
            error(QStringLiteral("BL0003"), QStringLiteral("Unterminated string literal"),
                  start, m_position - start);
            const QString raw = m_source.mid(contentStart).toString();
            add(TokenKind::String, start, raw);
            return;
        }
        const qsizetype contentLength = m_position - contentStart - 1;
        add(TokenKind::String, start,
            m_source.mid(contentStart, contentLength).toString());
    }

    QStringView m_source;
    qsizetype m_baseOffset = 0;
    qsizetype m_position = 0;
    LexResult m_result;
};

}  // namespace

LexResult lexBrecoLang(QStringView source, qsizetype baseOffset) {
    return Lexer(source, baseOffset).run();
}

QString decodeStringToken(const Token& token, QVector<Diagnostic>* diagnostics) {
    QString result;
    result.reserve(token.text.size());
    for (qsizetype i = 0; i < token.text.size(); ++i) {
        QChar ch = token.text.at(i);
        if (ch != QLatin1Char('\\')) {
            result += ch;
            continue;
        }
        if (++i >= token.text.size()) {
            if (diagnostics != nullptr) {
                diagnostics->push_back(
                    {DiagnosticSeverity::Error, QStringLiteral("BL0004"),
                     QStringLiteral("Incomplete string escape"), token.span, {}});
            }
            break;
        }
        ch = token.text.at(i);
        switch (ch.unicode()) {
            case 'n': result += QLatin1Char('\n'); break;
            case 'r': result += QLatin1Char('\r'); break;
            case 't': result += QLatin1Char('\t'); break;
            case '0': result += QChar(u'\0'); break;
            case '"': result += QLatin1Char('"'); break;
            case '\\': result += QLatin1Char('\\'); break;
            case '$': result += QLatin1Char('$'); break;
            default:
                if (diagnostics != nullptr) {
                    diagnostics->push_back(
                        {DiagnosticSeverity::Warning, QStringLiteral("BL1001"),
                         QStringLiteral("Unknown escape sequence '\\%1'").arg(ch),
                         token.span, {}});
                }
                result += ch;
                break;
        }
    }
    return result;
}

bool parseUnsignedLiteral(QStringView text, quint64* value) {
    if (value == nullptr) {
        return false;
    }
    QString normalized = text.toString();
    normalized.remove(QLatin1Char('_'));

    quint64 multiplier = 1;
    const struct Suffix {
        const char* text;
        quint64 multiplier;
    } suffixes[] = {{"KiB", 1024ULL},
                    {"MiB", 1024ULL * 1024ULL},
                    {"GiB", 1024ULL * 1024ULL * 1024ULL},
                    {"KB", 1000ULL},
                    {"MB", 1000ULL * 1000ULL},
                    {"GB", 1000ULL * 1000ULL * 1000ULL}};
    for (const Suffix& suffix : suffixes) {
        const QString suffixText = QLatin1String(suffix.text);
        if (normalized.endsWith(suffixText, Qt::CaseInsensitive)) {
            normalized.chop(suffixText.size());
            multiplier = suffix.multiplier;
            break;
        }
    }

    bool ok = false;
    const int base = normalized.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
                         ? 16
                         : 10;
    const quint64 parsed = normalized.toULongLong(&ok, base);
    if (!ok || parsed > std::numeric_limits<quint64>::max() / multiplier) {
        return false;
    }
    *value = parsed * multiplier;
    return true;
}

}  // namespace breco::lang
