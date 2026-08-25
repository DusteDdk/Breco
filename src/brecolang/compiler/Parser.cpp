#include "brecolang/compiler/Parser.h"

#include "brecolang/compiler/Lexer.h"

#include <QHash>

#include <algorithm>
#include <utility>

namespace breco::lang {
namespace {

SourceSpan spanning(SourceSpan first, SourceSpan last) {
    const qsizetype start = qMin(first.start, last.start);
    const qsizetype end = qMax(first.end(), last.end());
    return {start, end - start};
}

bool isKeyword(const Token& token, QStringView keyword) {
    return token.kind == TokenKind::Identifier && token.text == keyword;
}

class ExpressionParser {
public:
    ExpressionParser(const QVector<Token>& tokens, qsizetype* position,
                     SyntaxFile* syntax, QVector<Diagnostic>* diagnostics)
        : m_tokens(tokens),
          m_position(position),
          m_syntax(syntax),
          m_diagnostics(diagnostics) {}

    SyntaxExpressionId parseExpression(int minimumPrecedence = 1) {
        SyntaxExpressionId left = parseUnary();
        while (left != kInvalidSyntaxExpression) {
            const auto binary = binaryOperator(peek().kind);
            if (!binary.has_value() || binary->second < minimumPrecedence) {
                break;
            }
            const Token op = advance();
            const int precedence = binary->second;
            SyntaxExpressionId right = parseExpression(precedence + 1);
            if (right == kInvalidSyntaxExpression) {
                report(QStringLiteral("BP0202"),
                       QStringLiteral("Expected expression after operator"), op.span);
                break;
            }
            SyntaxExpression expression;
            expression.kind = SyntaxExpressionKind::Binary;
            expression.binaryOperator = binary->first;
            expression.operands = {left, right};
            expression.span = spanning(m_syntax->expressions.at(left).span,
                                       m_syntax->expressions.at(right).span);
            left = append(std::move(expression));
        }
        return left;
    }

private:
    const Token& peek(qsizetype distance = 0) const {
        const qsizetype index = *m_position + distance;
        return m_tokens.at(qBound<qsizetype>(0, index, m_tokens.size() - 1));
    }
    const Token& advance() {
        const Token& token = peek();
        if (token.kind != TokenKind::End) {
            ++*m_position;
        }
        return token;
    }
    bool match(TokenKind kind) {
        if (peek().kind != kind) {
            return false;
        }
        advance();
        return true;
    }

    SyntaxExpressionId append(SyntaxExpression expression) {
        const SyntaxExpressionId id =
            static_cast<SyntaxExpressionId>(m_syntax->expressions.size());
        m_syntax->expressions.push_back(std::move(expression));
        return id;
    }

    void report(const QString& code, const QString& message, SourceSpan span) {
        m_diagnostics->push_back(
            {DiagnosticSeverity::Error, code, message, span, {}});
    }

    SyntaxExpressionId parseUnary() {
        if (peek().kind == TokenKind::Minus || peek().kind == TokenKind::Bang) {
            const Token op = advance();
            const SyntaxExpressionId operand = parseUnary();
            if (operand == kInvalidSyntaxExpression) {
                report(QStringLiteral("BP0201"),
                       QStringLiteral("Expected expression after unary operator"),
                       op.span);
                return kInvalidSyntaxExpression;
            }
            SyntaxExpression expression;
            expression.kind = SyntaxExpressionKind::Unary;
            expression.unaryOperator = op.kind == TokenKind::Minus
                                           ? SyntaxUnaryOperator::Negate
                                           : SyntaxUnaryOperator::LogicalNot;
            expression.operands = {operand};
            expression.span =
                spanning(op.span, m_syntax->expressions.at(operand).span);
            return append(std::move(expression));
        }
        return parsePostfix();
    }

    SyntaxExpressionId parsePostfix() {
        SyntaxExpressionId base = parsePrimary();
        while (base != kInvalidSyntaxExpression) {
            if (match(TokenKind::Dot)) {
                const bool metadata = match(TokenKind::At);
                if (peek().kind != TokenKind::Identifier) {
                    report(QStringLiteral("BP0203"),
                           QStringLiteral("Expected member name after '.'"),
                           peek().span);
                    return base;
                }
                const Token name = advance();
                SyntaxExpression expression;
                expression.kind = metadata ? SyntaxExpressionKind::MetadataMember
                                           : SyntaxExpressionKind::Member;
                expression.text = name.text;
                expression.operands = {base};
                expression.span = spanning(m_syntax->expressions.at(base).span,
                                           name.span);
                base = append(std::move(expression));
                continue;
            }
            if (match(TokenKind::LeftParen)) {
                SyntaxExpression expression;
                expression.kind = SyntaxExpressionKind::Call;
                expression.operands.push_back(base);
                if (peek().kind != TokenKind::RightParen) {
                    do {
                        const SyntaxExpressionId argument = parseExpression();
                        if (argument == kInvalidSyntaxExpression) {
                            break;
                        }
                        expression.operands.push_back(argument);
                    } while (match(TokenKind::Comma));
                }
                const Token close = peek();
                if (!match(TokenKind::RightParen)) {
                    report(QStringLiteral("BP0204"),
                           QStringLiteral("Expected ')' after arguments"),
                           peek().span);
                }
                expression.span = spanning(m_syntax->expressions.at(base).span,
                                           close.span);
                base = append(std::move(expression));
                continue;
            }
            break;
        }
        return base;
    }

    SyntaxExpressionId parsePrimary() {
        const Token token = peek();
        if (token.kind == TokenKind::Integer) {
            advance();
            SyntaxExpression expression;
            expression.kind = SyntaxExpressionKind::UnsignedInteger;
            expression.span = token.span;
            if (!parseUnsignedLiteral(token.text, &expression.unsignedValue)) {
                report(QStringLiteral("BP0205"),
                       QStringLiteral("Invalid or overflowing integer literal '%1'")
                           .arg(token.text),
                       token.span);
            }
            return append(std::move(expression));
        }
        if (token.kind == TokenKind::Float) {
            advance();
            SyntaxExpression expression;
            expression.kind = SyntaxExpressionKind::FloatingPoint;
            expression.span = token.span;
            QString normalized = token.text;
            normalized.remove(QLatin1Char('_'));
            bool ok = false;
            expression.floatingValue = normalized.toDouble(&ok);
            if (!ok) {
                report(QStringLiteral("BP0206"),
                       QStringLiteral("Invalid floating-point literal '%1'")
                           .arg(token.text),
                       token.span);
            }
            return append(std::move(expression));
        }
        if (token.kind == TokenKind::String) {
            advance();
            return parseString(token);
        }
        if (token.kind == TokenKind::Identifier) {
            if (token.text == QStringLiteral("true") ||
                token.text == QStringLiteral("false")) {
                advance();
                SyntaxExpression expression;
                expression.kind = SyntaxExpressionKind::Boolean;
                expression.booleanValue = token.text == QStringLiteral("true");
                expression.span = token.span;
                return append(std::move(expression));
            }
            if (token.text == QStringLiteral("bytes") &&
                peek(1).kind == TokenKind::LeftBracket) {
                advance();
                advance();
                SyntaxExpression expression;
                expression.kind = SyntaxExpressionKind::ByteArray;
                expression.span = token.span;
                if (peek().kind != TokenKind::RightBracket) {
                    do {
                        const SyntaxExpressionId byte = parseExpression();
                        if (byte == kInvalidSyntaxExpression) {
                            break;
                        }
                        expression.operands.push_back(byte);
                    } while (match(TokenKind::Comma));
                }
                const Token close = peek();
                if (!match(TokenKind::RightBracket)) {
                    report(QStringLiteral("BP0207"),
                           QStringLiteral("Expected ']' after byte array"),
                           peek().span);
                }
                expression.span = spanning(token.span, close.span);
                return append(std::move(expression));
            }
            advance();
            SyntaxExpression expression;
            expression.kind = SyntaxExpressionKind::Identifier;
            expression.text = token.text;
            expression.span = token.span;
            return append(std::move(expression));
        }
        if (match(TokenKind::LeftParen)) {
            const SyntaxExpressionId expression = parseExpression();
            if (!match(TokenKind::RightParen)) {
                report(QStringLiteral("BP0208"),
                       QStringLiteral("Expected ')' after expression"), peek().span);
            }
            return expression;
        }
        return kInvalidSyntaxExpression;
    }

    SyntaxExpressionId parseString(const Token& token) {
        QVector<QString> parts;
        QVector<SyntaxExpressionId> expressions;
        qsizetype partStart = 0;
        qsizetype position = 0;
        while (position + 1 < token.text.size()) {
            if (token.text.at(position) == QLatin1Char('\\')) {
                position += 2;
                continue;
            }
            if (token.text.at(position) != QLatin1Char('$') ||
                token.text.at(position + 1) != QLatin1Char('{')) {
                ++position;
                continue;
            }
            const qsizetype expressionStart = position + 2;
            qsizetype cursor = expressionStart;
            int braceDepth = 1;
            bool inString = false;
            while (cursor < token.text.size() && braceDepth > 0) {
                const QChar ch = token.text.at(cursor);
                if (ch == QLatin1Char('\\')) {
                    cursor += 2;
                    continue;
                }
                if (ch == QLatin1Char('"')) {
                    inString = !inString;
                } else if (!inString && ch == QLatin1Char('{')) {
                    ++braceDepth;
                } else if (!inString && ch == QLatin1Char('}')) {
                    --braceDepth;
                }
                ++cursor;
            }
            if (braceDepth != 0) {
                report(QStringLiteral("BP0209"),
                       QStringLiteral("Unclosed string interpolation"), token.span);
                break;
            }

            Token partToken{TokenKind::String,
                            token.text.mid(partStart, position - partStart),
                            {token.span.start + 1 + partStart,
                             position - partStart}};
            parts.push_back(decodeStringToken(partToken, m_diagnostics));

            const qsizetype expressionLength = cursor - expressionStart - 1;
            const QStringView expressionText =
                QStringView(token.text).mid(expressionStart, expressionLength);
            LexResult nested =
                lexBrecoLang(expressionText,
                             token.span.start + 1 + expressionStart);
            *m_diagnostics += nested.diagnostics;
            qsizetype nestedPosition = 0;
            ExpressionParser nestedParser(nested.tokens, &nestedPosition, m_syntax,
                                          m_diagnostics);
            const SyntaxExpressionId nestedExpression =
                nestedParser.parseExpression();
            if (nestedExpression == kInvalidSyntaxExpression ||
                nested.tokens.at(nestedPosition).kind != TokenKind::End) {
                report(QStringLiteral("BP0210"),
                       QStringLiteral("Invalid interpolation expression"),
                       {token.span.start + 1 + expressionStart, expressionLength});
            } else {
                expressions.push_back(nestedExpression);
            }
            position = cursor;
            partStart = cursor;
        }

        if (expressions.isEmpty()) {
            SyntaxExpression expression;
            expression.kind = SyntaxExpressionKind::String;
            expression.span = token.span;
            expression.text = decodeStringToken(token, m_diagnostics);
            return append(std::move(expression));
        }

        Token tailToken{TokenKind::String, token.text.mid(partStart),
                        {token.span.start + 1 + partStart,
                         token.text.size() - partStart}};
        parts.push_back(decodeStringToken(tailToken, m_diagnostics));
        SyntaxExpression expression;
        expression.kind = SyntaxExpressionKind::InterpolatedString;
        expression.span = token.span;
        expression.operands = std::move(expressions);
        expression.textParts = std::move(parts);
        return append(std::move(expression));
    }

    static std::optional<QPair<SyntaxBinaryOperator, int>> binaryOperator(
        TokenKind kind) {
        switch (kind) {
            case TokenKind::OrOr:
                return QPair{SyntaxBinaryOperator::LogicalOr, 2};
            case TokenKind::AndAnd:
                return QPair{SyntaxBinaryOperator::LogicalAnd, 3};
            case TokenKind::EqualEqual:
                return QPair{SyntaxBinaryOperator::Equal, 4};
            case TokenKind::BangEqual:
                return QPair{SyntaxBinaryOperator::NotEqual, 4};
            case TokenKind::Less:
                return QPair{SyntaxBinaryOperator::Less, 5};
            case TokenKind::LessEqual:
                return QPair{SyntaxBinaryOperator::LessEqual, 5};
            case TokenKind::Greater:
                return QPair{SyntaxBinaryOperator::Greater, 5};
            case TokenKind::GreaterEqual:
                return QPair{SyntaxBinaryOperator::GreaterEqual, 5};
            case TokenKind::Plus:
                return QPair{SyntaxBinaryOperator::Add, 6};
            case TokenKind::Minus:
                return QPair{SyntaxBinaryOperator::Subtract, 6};
            case TokenKind::Star:
                return QPair{SyntaxBinaryOperator::Multiply, 7};
            case TokenKind::Slash:
                return QPair{SyntaxBinaryOperator::Divide, 7};
            case TokenKind::Percent:
                return QPair{SyntaxBinaryOperator::Remainder, 7};
            default: return std::nullopt;
        }
    }

    const QVector<Token>& m_tokens;
    qsizetype* m_position = nullptr;
    SyntaxFile* m_syntax = nullptr;
    QVector<Diagnostic>* m_diagnostics = nullptr;
};

class Parser {
public:
    Parser(QStringView source, QString sourcePath)
        : m_source(source), m_lexed(lexBrecoLang(source)) {
        m_result.syntax.sourcePath = std::move(sourcePath);
        m_result.diagnostics = m_lexed.diagnostics;
    }

    ParseSyntaxResult run() {
        parseLanguageHeader();
        while (!atEnd()) {
            const qsizetype before = m_position;
            if (!parseTopLevel()) {
                report(QStringLiteral("BP0002"),
                       QStringLiteral("Expected a top-level declaration"), peek().span);
                synchronizeTopLevel();
            }
            if (m_position == before) {
                advance();
            }
        }
        return std::move(m_result);
    }

private:
    const Token& peek(qsizetype distance = 0) const {
        const qsizetype index = m_position + distance;
        return m_lexed.tokens.at(
            qBound<qsizetype>(0, index, m_lexed.tokens.size() - 1));
    }
    const Token& previous() const {
        return m_lexed.tokens.at(qMax<qsizetype>(0, m_position - 1));
    }
    bool atEnd() const { return peek().kind == TokenKind::End; }
    const Token& advance() {
        const Token& token = peek();
        if (!atEnd()) {
            ++m_position;
        }
        return token;
    }
    bool check(TokenKind kind) const { return peek().kind == kind; }
    bool match(TokenKind kind) {
        if (!check(kind)) {
            return false;
        }
        advance();
        return true;
    }
    bool checkKeyword(QStringView keyword) const {
        return isKeyword(peek(), keyword);
    }
    bool matchKeyword(QStringView keyword) {
        if (!checkKeyword(keyword)) {
            return false;
        }
        advance();
        return true;
    }

    void report(const QString& code, const QString& message, SourceSpan span,
                DiagnosticSeverity severity = DiagnosticSeverity::Error) {
        m_result.diagnostics.push_back({severity, code, message, span, {}});
    }

    Token expect(TokenKind kind, const QString& message) {
        if (check(kind)) {
            return advance();
        }
        report(QStringLiteral("BP0001"), message, peek().span);
        return Token{kind, {}, peek().span};
    }

    Token expectIdentifier(const QString& message) {
        return expect(TokenKind::Identifier, message);
    }

    bool expectKeyword(QStringView keyword, const QString& message) {
        if (matchKeyword(keyword)) {
            return true;
        }
        report(QStringLiteral("BP0001"), message, peek().span);
        return false;
    }

    void consumeOptionalSemicolon() { match(TokenKind::Semicolon); }

    SyntaxExpressionId parseExpression() {
        ExpressionParser parser(m_lexed.tokens, &m_position, &m_result.syntax,
                                &m_result.diagnostics);
        const SyntaxExpressionId expression = parser.parseExpression();
        if (expression == kInvalidSyntaxExpression) {
            report(QStringLiteral("BP0200"), QStringLiteral("Expected expression"),
                   peek().span);
        }
        return expression;
    }

    SyntaxStatementId appendStatement(SyntaxStatement statement) {
        const SyntaxStatementId id =
            static_cast<SyntaxStatementId>(m_result.syntax.statements.size());
        m_result.syntax.statements.push_back(std::move(statement));
        return id;
    }

    void parseLanguageHeader() {
        if (!matchKeyword(u"language")) {
            report(QStringLiteral("BP0100"),
                   QStringLiteral("File must begin with 'language breco 0.1'"),
                   peek().span);
            return;
        }
        const Token name = expectIdentifier(QStringLiteral("Expected language name"));
        Token version = peek();
        if (version.kind == TokenKind::Float || version.kind == TokenKind::Integer) {
            advance();
        } else {
            version = expect(TokenKind::Float,
                             QStringLiteral("Expected language version '0.1'"));
        }
        m_result.syntax.languageName = name.text;
        m_result.syntax.languageVersion = version.text;
        if (name.text != QStringLiteral("breco")) {
            report(QStringLiteral("BP0101"),
                   QStringLiteral("Unsupported language '%1'").arg(name.text),
                   name.span);
        }
        if (version.text != QStringLiteral("0.1")) {
            report(QStringLiteral("BP0102"),
                   QStringLiteral("Unsupported BrecoLang version '%1'")
                       .arg(version.text),
                   version.span);
        }
        consumeOptionalSemicolon();
    }

    bool parseTopLevel() {
        if (matchKeyword(u"inputs")) {
            parseInputs();
            return true;
        }
        if (matchKeyword(u"limits")) {
            parseLimits();
            return true;
        }
        if (matchKeyword(u"const")) {
            parseConstant();
            return true;
        }
        if (matchKeyword(u"enum")) {
            parseEnum();
            return true;
        }
        if (matchKeyword(u"record")) {
            parseRecord();
            return true;
        }
        if (matchKeyword(u"entry")) {
            parseEntry();
            return true;
        }
        if (matchKeyword(u"default")) {
            parseDefaultEntry();
            return true;
        }
        if (matchKeyword(u"outform")) {
            parseOutform();
            return true;
        }
        return false;
    }

    void parseInputs() {
        if (!match(TokenKind::LeftBrace)) {
            report(QStringLiteral("BP0300"),
                   QStringLiteral("Expected '{' after inputs"), peek().span);
            return;
        }
        while (!atEnd() && !check(TokenKind::RightBrace)) {
            if (!matchKeyword(u"input")) {
                report(QStringLiteral("BP0301"),
                       QStringLiteral("Expected input declaration"), peek().span);
                synchronizeBlock();
                continue;
            }
            SyntaxInput input;
            const Token name =
                expectIdentifier(QStringLiteral("Expected input name"));
            input.name = name.text;
            input.nameSpan = name.span;
            if (check(TokenKind::String)) {
                input.label = decodeStringToken(advance(), &m_result.diagnostics);
            }
            if (!match(TokenKind::LeftBrace)) {
                report(QStringLiteral("BP0302"),
                       QStringLiteral("Expected '{' after input declaration"),
                       peek().span);
                synchronizeBlock();
                continue;
            }
            while (!atEnd() && !check(TokenKind::RightBrace)) {
                if (matchKeyword(u"default")) {
                    input.isDefault = true;
                } else if (matchKeyword(u"description")) {
                    const Token description =
                        expect(TokenKind::String,
                               QStringLiteral("Expected input description string"));
                    input.description =
                        decodeStringToken(description, &m_result.diagnostics);
                } else {
                    report(QStringLiteral("BP0303"),
                           QStringLiteral("Expected 'default' or 'description'"),
                           peek().span);
                    advance();
                }
                consumeOptionalSemicolon();
            }
            expect(TokenKind::RightBrace,
                   QStringLiteral("Expected '}' after input declaration"));
            m_result.syntax.inputs.push_back(std::move(input));
        }
        expect(TokenKind::RightBrace, QStringLiteral("Expected '}' after inputs"));
    }

    void parseLimits() {
        if (!match(TokenKind::LeftBrace)) {
            report(QStringLiteral("BP0310"),
                   QStringLiteral("Expected '{' after limits"), peek().span);
            return;
        }
        while (!atEnd() && !check(TokenKind::RightBrace)) {
            SyntaxLimit limit;
            const Token name = expectIdentifier(QStringLiteral("Expected limit name"));
            limit.name = name.text;
            limit.nameSpan = name.span;
            limit.value = parseExpression();
            consumeOptionalSemicolon();
            m_result.syntax.limits.push_back(std::move(limit));
        }
        expect(TokenKind::RightBrace, QStringLiteral("Expected '}' after limits"));
    }

    void parseConstant() {
        SyntaxConstant constant;
        const Token name = expectIdentifier(QStringLiteral("Expected constant name"));
        constant.name = name.text;
        constant.nameSpan = name.span;
        if (match(TokenKind::Colon)) {
            constant.type = parseType();
            constant.hasExplicitType = true;
        }
        expect(TokenKind::Equal, QStringLiteral("Expected '=' in constant declaration"));
        constant.value = parseExpression();
        consumeOptionalSemicolon();
        m_result.syntax.constants.push_back(std::move(constant));
    }

    void parseEnum() {
        SyntaxEnum declaration;
        const Token name = expectIdentifier(QStringLiteral("Expected enum name"));
        declaration.name = name.text;
        declaration.nameSpan = name.span;
        expect(TokenKind::Colon, QStringLiteral("Expected ':' after enum name"));
        declaration.underlyingType = parseType(false);
        if (!match(TokenKind::LeftBrace)) {
            report(QStringLiteral("BP0320"),
                   QStringLiteral("Expected '{' before enum members"), peek().span);
            return;
        }
        while (!atEnd() && !check(TokenKind::RightBrace)) {
            SyntaxEnumMember member;
            const Token memberName =
                expectIdentifier(QStringLiteral("Expected enum member name"));
            member.name = memberName.text;
            member.nameSpan = memberName.span;
            expect(TokenKind::Equal,
                   QStringLiteral("Expected '=' after enum member name"));
            member.value = parseExpression();
            consumeOptionalSemicolon();
            match(TokenKind::Comma);
            declaration.members.push_back(std::move(member));
        }
        expect(TokenKind::RightBrace, QStringLiteral("Expected '}' after enum"));
        consumeOptionalSemicolon();
        m_result.syntax.enums.push_back(std::move(declaration));
    }

    void parseRecord() {
        SyntaxRecord record;
        const Token name = expectIdentifier(QStringLiteral("Expected record name"));
        record.name = name.text;
        record.nameSpan = name.span;
        if (match(TokenKind::LeftParen)) {
            if (!check(TokenKind::RightParen)) {
                do {
                    SyntaxParameter parameter;
                    const Token parameterName = expectIdentifier(
                        QStringLiteral("Expected parameter name"));
                    parameter.name = parameterName.text;
                    parameter.nameSpan = parameterName.span;
                    expect(TokenKind::Colon,
                           QStringLiteral("Expected ':' after parameter name"));
                    parameter.type = parseType(false);
                    record.parameters.push_back(std::move(parameter));
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RightParen,
                   QStringLiteral("Expected ')' after record parameters"));
        }
        record.statements = parseStatementBlock(false);
        consumeOptionalSemicolon();
        m_result.syntax.records.push_back(std::move(record));
    }

    void parseEntry() {
        SyntaxEntry entry;
        const Token name = expectIdentifier(QStringLiteral("Expected entry name"));
        entry.name = name.text;
        entry.nameSpan = name.span;
        expectKeyword(u"from", QStringLiteral("Expected 'from' after entry name"));
        const Token input = expectIdentifier(QStringLiteral("Expected entry input name"));
        entry.inputName = input.text;
        entry.inputSpan = input.span;
        entry.statements = parseStatementBlock(false);
        consumeOptionalSemicolon();
        m_result.syntax.entries.push_back(std::move(entry));
    }

    void parseDefaultEntry() {
        expectKeyword(u"entry", QStringLiteral("Expected 'entry' after 'default'"));
        const Token name = expectIdentifier(QStringLiteral("Expected default entry name"));
        if (!m_result.syntax.defaultEntry.isEmpty()) {
            report(QStringLiteral("BP0330"),
                   QStringLiteral("Default entry may be declared only once"), name.span);
        } else {
            m_result.syntax.defaultEntry = name.text;
            m_result.syntax.defaultEntrySpan = name.span;
        }
        consumeOptionalSemicolon();
    }

    void parseOutform() {
        SyntaxOutform outform;
        const Token name = expectIdentifier(QStringLiteral("Expected outform name"));
        outform.name = name.text;
        outform.nameSpan = name.span;
        expect(TokenKind::LeftParen,
               QStringLiteral("Expected '(' after outform name"));
        const Token parameter =
            expectIdentifier(QStringLiteral("Expected outform parameter name"));
        outform.parameterName = parameter.text;
        outform.parameterSpan = parameter.span;
        expect(TokenKind::Colon,
               QStringLiteral("Expected ':' after outform parameter"));
        outform.targetType = parseType(false);
        expect(TokenKind::RightParen,
               QStringLiteral("Expected ')' after outform target type"));
        const Token mode =
            expectIdentifier(QStringLiteral("Expected outform mode 'text' or 'binary'"));
        if (mode.text == QStringLiteral("text")) {
            outform.mode = OutformMode::Text;
        } else if (mode.text == QStringLiteral("binary")) {
            outform.mode = OutformMode::Binary;
        } else {
            report(QStringLiteral("BP0340"),
                   QStringLiteral("Unknown outform mode '%1'").arg(mode.text), mode.span);
        }
        outform.statements = parseStatementBlock(true);
        consumeOptionalSemicolon();
        m_result.syntax.outforms.push_back(std::move(outform));
    }

    SyntaxType parseType(bool allowArguments = true) {
        SyntaxType type;
        const Token name = expectIdentifier(QStringLiteral("Expected type name"));
        type.name = name.text;
        type.span = name.span;
        if (allowArguments && match(TokenKind::LeftParen)) {
            if (!check(TokenKind::RightParen)) {
                do {
                    type.arguments.push_back(parseExpression());
                } while (match(TokenKind::Comma));
            }
            const Token close = expect(
                TokenKind::RightParen,
                QStringLiteral("Expected ')' after type arguments"));
            type.span = spanning(type.span, close.span);
        }
        return type;
    }

    QVector<SyntaxStatementId> parseStatementBlock(bool outform) {
        QVector<SyntaxStatementId> statements;
        if (!match(TokenKind::LeftBrace)) {
            report(QStringLiteral("BP0400"),
                   QStringLiteral("Expected '{' before block"), peek().span);
            return statements;
        }
        while (!atEnd() && !check(TokenKind::RightBrace)) {
            const qsizetype before = m_position;
            const SyntaxStatementId statement = parseStatement(outform);
            if (statement != std::numeric_limits<SyntaxStatementId>::max()) {
                statements.push_back(statement);
            } else {
                report(QStringLiteral("BP0401"),
                       outform ? QStringLiteral("Expected an outform statement")
                               : QStringLiteral("Expected a record statement"),
                       peek().span);
                synchronizeStatement();
            }
            if (m_position == before) {
                advance();
            }
        }
        expect(TokenKind::RightBrace, QStringLiteral("Expected '}' after block"));
        return statements;
    }

    SyntaxStatementId parseStatement(bool outform) {
        if (outform) {
            return parseOutformStatement();
        }
        if (matchKeyword(u"identify")) {
            SyntaxStatement statement;
            statement.kind = SyntaxStatementKind::Identify;
            const SourceSpan start = previous().span;
            statement.statements = parseStatementBlock(false);
            statement.span = spanning(start, previous().span);
            return appendStatement(std::move(statement));
        }
        if (matchKeyword(u"commit")) {
            return appendSimple(SyntaxStatementKind::Commit, previous().span);
        }
        if (matchKeyword(u"require")) {
            return parseValidation(SyntaxStatementKind::Require, previous().span);
        }
        if (matchKeyword(u"check")) {
            return parseValidation(SyntaxStatementKind::Check, previous().span);
        }
        if (matchKeyword(u"match")) {
            return parseValidation(SyntaxStatementKind::Match, previous().span);
        }
        if (matchKeyword(u"computed")) {
            return parseComputed(previous().span);
        }
        if (matchKeyword(u"preserve")) {
            return parseRawOrPreserve(SyntaxStatementKind::Preserve,
                                      previous().span);
        }
        if (matchKeyword(u"raw")) {
            return parseRawOrPreserve(SyntaxStatementKind::Raw, previous().span);
        }
        if (matchKeyword(u"continue")) {
            return parseLoopControl(SyntaxStatementKind::Continue, previous().span);
        }
        if (matchKeyword(u"break")) {
            return parseLoopControl(SyntaxStatementKind::Break, previous().span);
        }
        if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
            return parseNamedField();
        }
        return std::numeric_limits<SyntaxStatementId>::max();
    }

    SyntaxStatementId appendSimple(SyntaxStatementKind kind, SourceSpan start) {
        consumeOptionalSemicolon();
        SyntaxStatement statement;
        statement.kind = kind;
        statement.span = spanning(start, previous().span);
        return appendStatement(std::move(statement));
    }

    SyntaxStatementId parseValidation(SyntaxStatementKind kind, SourceSpan start) {
        SyntaxStatement statement;
        statement.kind = kind;
        statement.expression = parseExpression();
        if (matchKeyword(u"else")) {
            const Token message = expect(
                TokenKind::String,
                QStringLiteral("Expected diagnostic message after 'else'"));
            statement.message = decodeStringToken(message, &m_result.diagnostics);
        } else if (kind != SyntaxStatementKind::Check) {
            report(QStringLiteral("BP0410"),
                   QStringLiteral("Expected 'else' and a diagnostic message"),
                   peek().span);
        }
        consumeOptionalSemicolon();
        statement.span = spanning(start, previous().span);
        return appendStatement(std::move(statement));
    }

    SyntaxStatementId parseComputed(SourceSpan start) {
        SyntaxStatement statement;
        statement.kind = SyntaxStatementKind::ComputedField;
        const Token name =
            expectIdentifier(QStringLiteral("Expected computed field name"));
        statement.name = name.text;
        statement.nameSpan = name.span;
        expect(TokenKind::Colon,
               QStringLiteral("Expected ':' after computed field name"));
        statement.type = parseType(false);
        expect(TokenKind::Equal,
               QStringLiteral("Expected '=' before computed expression"));
        statement.expression = parseExpression();
        consumeOptionalSemicolon();
        statement.span = spanning(start, previous().span);
        return appendStatement(std::move(statement));
    }

    SyntaxStatementId parseRawOrPreserve(SyntaxStatementKind kind,
                                         SourceSpan start) {
        SyntaxStatement statement;
        statement.kind = kind;
        expectKeyword(u"remaining", QStringLiteral("Expected 'remaining'"));
        expectKeyword(u"as", QStringLiteral("Expected 'as' after 'remaining'"));
        const Token name = expectIdentifier(QStringLiteral("Expected field name"));
        statement.name = name.text;
        statement.nameSpan = name.span;
        consumeOptionalSemicolon();
        statement.span = spanning(start, previous().span);
        return appendStatement(std::move(statement));
    }

    SyntaxStatementId parseLoopControl(SyntaxStatementKind kind,
                                       SourceSpan start) {
        SyntaxStatement statement;
        statement.kind = kind;
        if (matchKeyword(u"when")) {
            statement.condition = parseExpression();
        }
        consumeOptionalSemicolon();
        statement.span = spanning(start, previous().span);
        return appendStatement(std::move(statement));
    }

    SyntaxStatementId parseNamedField() {
        SyntaxStatement statement;
        const Token name = advance();
        advance();
        statement.name = name.text;
        statement.nameSpan = name.span;

        if (matchKeyword(u"ref")) {
            statement.kind = SyntaxStatementKind::Reference;
            statement.type = parseType();
            parseReferenceModifiers(&statement);
        } else if (matchKeyword(u"bitfield")) {
            statement.kind = SyntaxStatementKind::BitfieldField;
            statement.type = parseType(false);
            parseBitfieldMembers(&statement);
        } else if (matchKeyword(u"region")) {
            statement.kind = SyntaxStatementKind::Region;
            statement.expression = parseBytesCall();
            statement.statements = parseStatementBlock(false);
        } else if (matchKeyword(u"repeat")) {
            statement.kind = SyntaxStatementKind::Repeat;
            if (!check(TokenKind::LeftBrace)) {
                statement.expression = parseExpression();
            }
            statement.statements = parseStatementBlock(false);
            if (matchKeyword(u"until")) {
                statement.condition = parseExpression();
            }
        } else if (matchKeyword(u"while")) {
            statement.kind = SyntaxStatementKind::While;
            statement.condition = parseExpression();
            if (matchKeyword(u"with")) {
                statement.initializers = parseInitializerBlock();
            }
            statement.statements = parseStatementBlock(false);
        } else if (matchKeyword(u"many")) {
            statement.kind = SyntaxStatementKind::Many;
            statement.type = parseType();
        } else if (matchKeyword(u"select")) {
            statement.kind = SyntaxStatementKind::Select;
            if (!check(TokenKind::LeftBrace)) {
                statement.expression = parseExpression();
            }
            parseSelectCases(&statement);
        } else if (matchKeyword(u"one_of")) {
            statement.kind = SyntaxStatementKind::OneOf;
            parseAlternatives(&statement);
        } else if (matchKeyword(u"recover")) {
            statement.kind = SyntaxStatementKind::Recover;
            statement.type = parseType();
            parseRecoveryOptions(&statement);
        } else {
            statement.kind = SyntaxStatementKind::Field;
            statement.type = parseType();
            parseFieldModifiers(&statement);
        }

        consumeOptionalSemicolon();
        statement.span = spanning(name.span, previous().span);
        return appendStatement(std::move(statement));
    }

    void parseBitfieldMembers(SyntaxStatement* statement) {
        if (!match(TokenKind::LeftBrace)) {
            report(QStringLiteral("BP0420"),
                   QStringLiteral("Expected '{' before bitfield members"),
                   peek().span);
            return;
        }
        while (!atEnd() && !check(TokenKind::RightBrace)) {
            SyntaxBitMember member;
            const Token name =
                expectIdentifier(QStringLiteral("Expected bitfield member name"));
            member.name = name.text;
            member.span = name.span;
            expect(TokenKind::Colon,
                   QStringLiteral("Expected ':' after bitfield member name"));
            if (matchKeyword(u"bit")) {
                member.highBit = parseExpression();
                member.lowBit = member.highBit;
            } else if (matchKeyword(u"bits")) {
                member.highBit = parseExpression();
                if (!match(TokenKind::DotDot)) {
                    report(QStringLiteral("BP0421"),
                           QStringLiteral("Expected '..' in bit range"), peek().span);
                }
                member.lowBit = parseExpression();
            } else {
                report(QStringLiteral("BP0422"),
                       QStringLiteral("Expected 'bit' or 'bits'"), peek().span);
                synchronizeStatement();
            }
            consumeOptionalSemicolon();
            statement->bitMembers.push_back(std::move(member));
        }
        expect(TokenKind::RightBrace,
               QStringLiteral("Expected '}' after bitfield members"));
    }

    SyntaxExpressionId parseBytesCall() {
        const Token bytes = expectIdentifier(QStringLiteral("Expected bytes(size)"));
        if (bytes.text != QStringLiteral("bytes")) {
            report(QStringLiteral("BP0430"),
                   QStringLiteral("Region size must use bytes(size)"), bytes.span);
        }
        expect(TokenKind::LeftParen, QStringLiteral("Expected '(' after bytes"));
        const SyntaxExpressionId expression = parseExpression();
        expect(TokenKind::RightParen,
               QStringLiteral("Expected ')' after region size"));
        return expression;
    }

    QVector<SyntaxStatementId> parseInitializerBlock() {
        QVector<SyntaxStatementId> initializers;
        if (!match(TokenKind::LeftBrace)) {
            report(QStringLiteral("BP0431"),
                   QStringLiteral("Expected '{' after 'with'"), peek().span);
            return initializers;
        }
        while (!atEnd() && !check(TokenKind::RightBrace)) {
            SyntaxStatement initializer;
            initializer.kind = SyntaxStatementKind::Let;
            const Token name =
                expectIdentifier(QStringLiteral("Expected loop binding name"));
            initializer.name = name.text;
            initializer.nameSpan = name.span;
            expect(TokenKind::Colon,
                   QStringLiteral("Expected ':' after loop binding name"));
            initializer.type = parseType(false);
            expect(TokenKind::Equal,
                   QStringLiteral("Expected '=' in loop binding"));
            initializer.expression = parseExpression();
            consumeOptionalSemicolon();
            initializer.span = spanning(name.span, previous().span);
            initializers.push_back(appendStatement(std::move(initializer)));
        }
        expect(TokenKind::RightBrace,
               QStringLiteral("Expected '}' after loop bindings"));
        return initializers;
    }

    void parseSelectCases(SyntaxStatement* statement) {
        if (!match(TokenKind::LeftBrace)) {
            report(QStringLiteral("BP0440"),
                   QStringLiteral("Expected '{' before select cases"), peek().span);
            return;
        }
        while (!atEnd() && !check(TokenKind::RightBrace)) {
            SyntaxSelectCase selectCase;
            const SourceSpan start = peek().span;
            if (matchKeyword(u"default") || matchKeyword(u"else")) {
                selectCase.isDefault = true;
            } else if (matchKeyword(u"when")) {
                selectCase.isConditional = true;
                selectCase.expression = parseExpression();
            } else {
                selectCase.expression = parseExpression();
            }
            expect(TokenKind::FatArrow,
                   QStringLiteral("Expected '=>' after select case"));
            if (check(TokenKind::LeftBrace)) {
                selectCase.statements = parseStatementBlock(false);
            } else if (check(TokenKind::Identifier) &&
                       !checkKeyword(u"raw") && !checkKeyword(u"preserve")) {
                selectCase.isType = true;
                selectCase.type = parseType();
            } else {
                const SyntaxStatementId child = parseStatement(false);
                if (child != std::numeric_limits<SyntaxStatementId>::max()) {
                    selectCase.statements.push_back(child);
                } else {
                    report(QStringLiteral("BP0441"),
                           QStringLiteral("Expected select branch statement"),
                           peek().span);
                    synchronizeStatement();
                }
            }
            selectCase.span = spanning(start, previous().span);
            statement->selectCases.push_back(std::move(selectCase));
        }
        expect(TokenKind::RightBrace,
               QStringLiteral("Expected '}' after select cases"));
    }

    void parseAlternatives(SyntaxStatement* statement) {
        if (!match(TokenKind::LeftBrace)) {
            report(QStringLiteral("BP0450"),
                   QStringLiteral("Expected '{' after one_of"), peek().span);
            return;
        }
        while (!atEnd() && !check(TokenKind::RightBrace)) {
            const Token as = peek();
            if (!matchKeyword(u"as")) {
                report(QStringLiteral("BP0451"),
                       QStringLiteral("Expected 'as' before alternative type"),
                       peek().span);
                synchronizeStatement();
                continue;
            }
            SyntaxAlternative alternative;
            alternative.type = parseType();
            alternative.span = spanning(as.span, alternative.type.span);
            statement->alternatives.push_back(std::move(alternative));
            match(TokenKind::Comma);
        }
        expect(TokenKind::RightBrace,
               QStringLiteral("Expected '}' after one_of alternatives"));
    }

    void parseRecoveryOptions(SyntaxStatement* statement) {
        if (!match(TokenKind::LeftBrace)) {
            report(QStringLiteral("BP0460"),
                   QStringLiteral("Expected '{' after recover item type"),
                   peek().span);
            return;
        }
        while (!atEnd() && !check(TokenKind::RightBrace)) {
            if (matchKeyword(u"sync")) {
                parseSyncPatterns(statement);
            } else if (matchKeyword(u"step")) {
                statement->condition = parseExpression();
                expectKeyword(u"byte", QStringLiteral("Expected 'byte' after step"));
            } else if (matchKeyword(u"max_probe")) {
                statement->secondaryExpression = parseExpression();
            } else if (matchKeyword(u"gaps")) {
                expectKeyword(u"as", QStringLiteral("Expected 'as' after gaps"));
                statement->gapsName =
                    expectIdentifier(QStringLiteral("Expected gap node name")).text;
            } else {
                report(QStringLiteral("BP0461"),
                       QStringLiteral("Expected recover option"), peek().span);
                synchronizeStatement();
            }
            consumeOptionalSemicolon();
        }
        expect(TokenKind::RightBrace,
               QStringLiteral("Expected '}' after recover options"));
    }

    void parseSyncPatterns(SyntaxStatement* statement) {
        if (matchKeyword(u"one_of")) {
            if (!match(TokenKind::LeftBrace)) {
                report(QStringLiteral("BP0462"),
                       QStringLiteral("Expected '{' after sync one_of"),
                       peek().span);
                return;
            }
            while (!atEnd() && !check(TokenKind::RightBrace)) {
                parseOneSyncPattern(statement);
                match(TokenKind::Comma);
            }
            expect(TokenKind::RightBrace,
                   QStringLiteral("Expected '}' after sync patterns"));
        } else {
            parseOneSyncPattern(statement);
        }
    }

    void parseOneSyncPattern(SyntaxStatement* statement) {
        const SourceSpan start = peek().span;
        if (!matchKeyword(u"bytes") || !match(TokenKind::LeftBracket)) {
            report(QStringLiteral("BP0463"),
                   QStringLiteral("Expected bytes [...] sync pattern"), peek().span);
            synchronizeStatement();
            return;
        }
        SyntaxBytePattern pattern;
        pattern.span = start;
        if (!check(TokenKind::RightBracket)) {
            do {
                pattern.bytes.push_back(parseExpression());
            } while (match(TokenKind::Comma));
        }
        const Token close = expect(
            TokenKind::RightBracket,
            QStringLiteral("Expected ']' after sync byte pattern"));
        pattern.span = spanning(start, close.span);
        statement->syncPatterns.push_back(std::move(pattern));
    }

    void parseFieldModifiers(SyntaxStatement* statement) {
        for (;;) {
            if (matchKeyword(u"when")) {
                statement->condition = parseExpression();
            } else if (matchKeyword(u"from")) {
                const Token input =
                    expectIdentifier(QStringLiteral("Expected source input name"));
                statement->sourceInput = input.text;
                statement->sourceInputSpan = input.span;
                expectKeyword(u"at", QStringLiteral("Expected 'at' after input name"));
                statement->expression = parseExpression();
            } else if (matchKeyword(u"within")) {
                statement->secondaryExpression = parseBytesCall();
            } else {
                break;
            }
        }
    }

    void parseReferenceModifiers(SyntaxStatement* statement) {
        for (;;) {
            if (matchKeyword(u"when")) {
                statement->condition = parseExpression();
            } else if (matchKeyword(u"from")) {
                const Token input =
                    expectIdentifier(QStringLiteral("Expected reference input name"));
                statement->sourceInput = input.text;
                statement->sourceInputSpan = input.span;
                expectKeyword(u"at",
                              QStringLiteral("Expected 'at' after input name"));
                statement->expression = parseExpression();
            } else if (matchKeyword(u"within")) {
                statement->secondaryExpression = parseBytesCall();
            } else if (matchKeyword(u"key")) {
                statement->referenceKeys.push_back(parseExpression());
            } else if (matchKeyword(u"follow")) {
                if (statement->referenceStrength !=
                    SyntaxReferenceStrength::Invalid) {
                    report(QStringLiteral("BP0452"),
                           QStringLiteral("Reference strength may be declared only once"),
                           previous().span);
                }
                statement->referenceStrength = SyntaxReferenceStrength::Follow;
            } else if (matchKeyword(u"weak")) {
                if (statement->referenceStrength !=
                    SyntaxReferenceStrength::Invalid) {
                    report(QStringLiteral("BP0452"),
                           QStringLiteral("Reference strength may be declared only once"),
                           previous().span);
                }
                statement->referenceStrength = SyntaxReferenceStrength::Weak;
            } else if (matchKeyword(u"cover")) {
                const Token coverage = expectIdentifier(
                    QStringLiteral("Expected 'decoded' or 'region' after cover"));
                if (coverage.text == QStringLiteral("decoded")) {
                    statement->referenceCoverage =
                        SyntaxReferenceCoverage::DecodedStorage;
                } else if (coverage.text == QStringLiteral("region")) {
                    statement->referenceCoverage =
                        SyntaxReferenceCoverage::WholeRegion;
                } else {
                    report(QStringLiteral("BP0450"),
                           QStringLiteral("Reference coverage must be 'decoded' or 'region'"),
                           coverage.span);
                }
            } else if (matchKeyword(u"rewrite")) {
                parseReferenceRewrites(statement);
            } else {
                break;
            }
        }
    }

    void parseReferenceRewrites(SyntaxStatement* statement) {
        if (!match(TokenKind::LeftBrace)) {
            report(QStringLiteral("BP0451"),
                   QStringLiteral("Expected '{' before reference rewrite rules"),
                   peek().span);
            return;
        }
        while (!atEnd() && !check(TokenKind::RightBrace)) {
            SyntaxReferenceRewrite rewrite;
            const Token first = expectIdentifier(
                QStringLiteral("Expected rewrite target field"));
            rewrite.targetPath.push_back(first.text);
            SourceSpan end = first.span;
            while (match(TokenKind::Dot)) {
                const Token member = expectIdentifier(
                    QStringLiteral("Expected field name after '.'"));
                rewrite.targetPath.push_back(member.text);
                end = member.span;
            }
            expect(TokenKind::Equal,
                   QStringLiteral("Expected '=' after rewrite target"));
            rewrite.expression = parseExpression();
            consumeOptionalSemicolon();
            if (rewrite.expression != kInvalidSyntaxExpression) {
                end = m_result.syntax.expressions.at(rewrite.expression).span;
            }
            rewrite.span = spanning(first.span, end);
            statement->referenceRewrites.push_back(std::move(rewrite));
        }
        expect(TokenKind::RightBrace,
               QStringLiteral("Expected '}' after reference rewrite rules"));
    }

    SyntaxStatementId parseOutformStatement() {
        if (matchKeyword(u"emit")) {
            const SourceSpan start = previous().span;
            SyntaxStatement statement;
            statement.kind = SyntaxStatementKind::Emit;
            statement.expression = parseExpression();
            consumeOptionalSemicolon();
            statement.span = spanning(start, previous().span);
            return appendStatement(std::move(statement));
        }
        if (matchKeyword(u"let")) {
            const SourceSpan start = previous().span;
            SyntaxStatement statement;
            statement.kind = SyntaxStatementKind::Let;
            const Token name =
                expectIdentifier(QStringLiteral("Expected local name after let"));
            statement.name = name.text;
            statement.nameSpan = name.span;
            if (match(TokenKind::Colon)) {
                statement.type = parseType(false);
            }
            expect(TokenKind::Equal, QStringLiteral("Expected '=' after local name"));
            statement.expression = parseExpression();
            consumeOptionalSemicolon();
            statement.span = spanning(start, previous().span);
            return appendStatement(std::move(statement));
        }
        if (matchKeyword(u"if")) {
            const SourceSpan start = previous().span;
            SyntaxStatement statement;
            statement.kind = SyntaxStatementKind::If;
            statement.condition = parseExpression();
            statement.statements = parseStatementBlock(true);
            if (matchKeyword(u"else")) {
                if (checkKeyword(u"if")) {
                    const SyntaxStatementId nested = parseOutformStatement();
                    if (nested != std::numeric_limits<SyntaxStatementId>::max()) {
                        statement.elseStatements.push_back(nested);
                    }
                } else {
                    statement.elseStatements = parseStatementBlock(true);
                }
            }
            statement.span = spanning(start, previous().span);
            return appendStatement(std::move(statement));
        }
        if (matchKeyword(u"for")) {
            const SourceSpan start = previous().span;
            SyntaxStatement statement;
            statement.kind = SyntaxStatementKind::For;
            const Token item =
                expectIdentifier(QStringLiteral("Expected loop item name"));
            statement.name = item.text;
            statement.nameSpan = item.span;
            if (match(TokenKind::Comma)) {
                const Token index =
                    expectIdentifier(QStringLiteral("Expected loop index name"));
                statement.secondaryName = index.text;
                statement.secondaryNameSpan = index.span;
            }
            expectKeyword(u"in", QStringLiteral("Expected 'in' in for loop"));
            statement.expression = parseExpression();
            statement.statements = parseStatementBlock(true);
            statement.span = spanning(start, previous().span);
            return appendStatement(std::move(statement));
        }
        return std::numeric_limits<SyntaxStatementId>::max();
    }

    void synchronizeTopLevel() {
        if (!atEnd()) {
            advance();
        }
        while (!atEnd()) {
            if (checkKeyword(u"inputs") || checkKeyword(u"limits") ||
                checkKeyword(u"const") || checkKeyword(u"enum") ||
                checkKeyword(u"record") || checkKeyword(u"entry") ||
                checkKeyword(u"default") || checkKeyword(u"outform")) {
                return;
            }
            advance();
        }
    }

    void synchronizeBlock() {
        if (!atEnd()) {
            advance();
        }
        while (!atEnd() && !check(TokenKind::RightBrace) &&
               !checkKeyword(u"input")) {
            advance();
        }
    }

    void synchronizeStatement() {
        if (!atEnd()) {
            advance();
        }
        while (!atEnd() && !check(TokenKind::RightBrace)) {
            if (checkKeyword(u"identify") || checkKeyword(u"commit") ||
                checkKeyword(u"require") || checkKeyword(u"check") ||
                checkKeyword(u"match") || checkKeyword(u"computed") ||
                checkKeyword(u"preserve") || checkKeyword(u"raw") ||
                checkKeyword(u"continue") || checkKeyword(u"break") ||
                checkKeyword(u"emit") || checkKeyword(u"let") ||
                checkKeyword(u"if") || checkKeyword(u"for") ||
                (check(TokenKind::Identifier) &&
                 peek(1).kind == TokenKind::Colon)) {
                return;
            }
            advance();
        }
    }

    QStringView m_source;
    LexResult m_lexed;
    qsizetype m_position = 0;
    ParseSyntaxResult m_result;
};

}  // namespace

ParseSyntaxResult parseBrecoLang(QStringView source, const QString& sourcePath) {
    return Parser(source, sourcePath).run();
}

}  // namespace breco::lang
