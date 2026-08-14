// =============================================================================
// src/frontend/lexer/lexer.h
// =============================================================================
// Hand-written scanner for a subset of JavaScript.
//
// Why hand-written?
//   - JS has nasty lexer quirks (automatic semicolon insertion, regex vs
//     division ambiguity, template literals, etc.) that are painful in a
//     table-driven lexer.
//   - We need line/column tracking for diagnostics.
//   - We need to peek ahead at multiple tokens for the parser.
//
// Output: a stream of Token structs. The lexer does NOT do automatic
// semicolon insertion; the parser handles that. The lexer just produces
// kSemicolon-or-not and the parser decides whether to insert one.
//
// Wait - actually we DO need to do ASI in the lexer for some cases (e.g.
// end of line before `}` or EOF). The standard approach is:
//   - The lexer produces a stream of tokens AND a flag "had_newline_before".
//   - The parser uses this flag to decide whether to insert a semicolon.
// We follow that approach.

#ifndef V12_FRONTEND_LEXER_LEXER_H_
#define V12_FRONTEND_LEXER_LEXER_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/arena.h"
#include "base/macros.h"
#include "frontend/lexer/tokens.h"

namespace v12 {

struct Token {
    TokenKind kind = TokenKind::kEof;
    const char* start = nullptr;   // points into source buffer
    uint32_t length = 0;
    uint32_t line = 1;
    uint32_t column = 1;
    bool had_line_terminator_before = false;

    // For kNumber: the parsed value.
    double number_value = 0.0;
    // For kString: the de-quoted, de-escaped value (heap-allocated in arena).
    std::string_view string_value;
    // For kIdentifier: a view into the source buffer (no allocation).
    std::string_view identifier_value;

    bool Is(TokenKind k) const { return kind == k; }
    bool IsKeyword() const { return v12::IsKeyword(kind); }
};

class Lexer {
public:
    Lexer(Arena* arena, std::string_view source);

    // Scan the next token.
    Token Next();

    // Peek at the next token without consuming it.
    const Token& Peek() const;

    // Peek at the token after the next.
    const Token& Peek2() const;

    // Rewind to a previously scanned token. The lexer must have saved it.
    // This is used by the parser for backtracking during ASI.
    void Rewind(const Token& tok);

    // Current position (line/column) for diagnostics.
    uint32_t line() const { return line_; }
    uint32_t column() const { return column_; }

    // Source access for error messages.
    std::string_view source() const { return source_; }

    // Error state. If has_error() is true, the lexer will keep producing
    // kError tokens until EOF.
    bool has_error() const { return has_error_; }
    const std::string& error_message() const { return error_message_; }

private:
    char PeekChar() const {
        return pos_ < source_.size() ? source_[pos_] : '\0';
    }
    char PeekChar2() const {
        return pos_ + 1 < source_.size() ? source_[pos_ + 1] : '\0';
    }
    char AdvanceChar() {
        char c = source_[pos_++];
        if (c == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return c;
    }

    void SkipWhitespaceAndComments();
    void SkipLineComment();
    void SkipBlockComment();

    Token ScanToken();
    Token ScanIdentifierOrKeyword();
    Token ScanNumber();
    Token ScanString(char quote);
    Token ScanPunctuation();

    void RecordError(std::string msg);
    Token MakeError(std::string msg);

    Token MakeToken(TokenKind kind, const char* start, uint32_t length) {
        Token t;
        t.kind = kind;
        t.start = start;
        t.length = length;
        t.line = token_line_;
        t.column = token_column_;
        t.had_line_terminator_before = had_line_terminator_before_;
        return t;
    }

    Arena* arena_;
    std::string_view source_;
    size_t pos_ = 0;
    uint32_t line_ = 1;
    uint32_t column_ = 1;

    // Per-token bookkeeping
    uint32_t token_line_ = 1;
    uint32_t token_column_ = 1;
    bool had_line_terminator_before_ = false;

    // Lookahead buffer. We need at most 2 tokens of lookahead.
    // Mutable because Peek()/Peek2() are const but lazily populate the buffer.
    mutable Token lookahead_[2];
    mutable int lookahead_count_ = 0;
    mutable int lookahead_pos_ = 0;

    bool has_error_ = false;
    std::string error_message_;
};

}  // namespace v12

#endif  // V12_FRONTEND_LEXER_LEXER_H_
