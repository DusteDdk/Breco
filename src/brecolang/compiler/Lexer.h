#pragma once

#include <QString>
#include <QStringView>
#include <QVector>

#include "brecolang/compiler/Source.h"

namespace breco::lang {

enum class TokenKind {
    End,
    Invalid,
    Identifier,
    Integer,
    Float,
    String,
    LeftBrace,
    RightBrace,
    LeftParen,
    RightParen,
    LeftBracket,
    RightBracket,
    Colon,
    Comma,
    Dot,
    DotDot,
    At,
    Semicolon,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Bang,
    Equal,
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    AndAnd,
    OrOr,
    FatArrow,
};

struct Token {
    TokenKind kind = TokenKind::Invalid;
    QString text;
    SourceSpan span;
};

struct LexResult {
    QVector<Token> tokens;
    QVector<Diagnostic> diagnostics;
};

LexResult lexBrecoLang(QStringView source, qsizetype baseOffset = 0);
QString decodeStringToken(const Token& token, QVector<Diagnostic>* diagnostics = nullptr);
bool parseUnsignedLiteral(QStringView text, quint64* value);

}  // namespace breco::lang
