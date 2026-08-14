// =============================================================================
// src/frontend/lexer/lexer.cc
// =============================================================================

#include "frontend/lexer/lexer.h"

#include <cstring>

namespace v12 {

namespace {

struct KeywordEntry {
    const char* name;
    TokenKind kind;
};

// Sorted by length then alphabetical for fast lookup.
constexpr KeywordEntry kKeywords[] = {
    {"as",        TokenKind::kAs},
    {"do",        TokenKind::kDo},
    {"if",        TokenKind::kIf},
    {"in",        TokenKind::kIn},
    {"of",        TokenKind::kOf},
    {"var",       TokenKind::kVar},
    {"let",       TokenKind::kLet},
    {"new",       TokenKind::kNew},
    {"for",       TokenKind::kFor},
    {"try",       TokenKind::kTry},
    {"case",      TokenKind::kCase},
    {"else",      TokenKind::kElse},
    {"enum",      TokenKind::kIdentifier},  // reserved, treat as ident
    {"from",      TokenKind::kFrom},
    {"get",       TokenKind::kGet},
    {"set",       TokenKind::kSet},
    {"true",      TokenKind::kTrue},
    {"await",     TokenKind::kAwait},
    {"break",     TokenKind::kBreak},
    {"catch",     TokenKind::kCatch},
    {"class",     TokenKind::kClass},
    {"const",     TokenKind::kConst},
    {"false",     TokenKind::kFalse},
    {"null",      TokenKind::kNull},
    {"super",     TokenKind::kSuper},
    {"this",      TokenKind::kThis},
    {"throw",     TokenKind::kThrow},
    {"yield",     TokenKind::kYield},
    {"async",     TokenKind::kAsync},
    {"await",     TokenKind::kAwait},
    {"default",   TokenKind::kDefault},
    {"delete",    TokenKind::kDelete},
    {"export",    TokenKind::kExport},
    {"import",    TokenKind::kImport},
    {"return",    TokenKind::kReturn},
    {"static",    TokenKind::kStatic},
    {"switch",    TokenKind::kSwitch},
    {"typeof",    TokenKind::kTypeof},
    {"void",      TokenKind::kVoid},
    {"while",     TokenKind::kWhile},
    {"continue",  TokenKind::kContinue},
    {"debugger",  TokenKind::kIdentifier},  // reserved, treat as ident
    {"extends",   TokenKind::kExtends},
    {"finally",   TokenKind::kFinally},
    {"function",  TokenKind::kFunction},
    {"instanceof",TokenKind::kInstanceof},
    {"undefined", TokenKind::kUndefined},
};

TokenKind LookupKeyword(std::string_view word) {
    for (const auto& kw : kKeywords) {
        size_t kw_len = std::strlen(kw.name);
        if (kw_len == word.size() &&
            std::memcmp(kw.name, word.data(), kw_len) == 0) {
            return kw.kind;
        }
    }
    return TokenKind::kIdentifier;
}

bool IsIdentifierStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '$' ||
           static_cast<unsigned char>(c) >= 0x80;  // Unicode (simplified)
}

bool IsIdentifierPart(char c) {
    return IsIdentifierStart(c) || (c >= '0' && c <= '9');
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHexDigit(char c) {
    return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool IsBinaryDigit(char c) { return c == '0' || c == '1'; }
bool IsOctalDigit(char c) { return c >= '0' && c <= '7'; }

bool IsLineTerminator(char c) { return c == '\n' || c == '\r'; }
bool IsWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' ||
           c == '\n' || c == '\r' ||
           static_cast<unsigned char>(c) >= 0x80;  // simplified Unicode WS
}

}  // namespace

Lexer::Lexer(Arena* arena, std::string_view source)
    : arena_(arena), source_(source) {
    // Prime the lookahead buffer lazily.
}

void Lexer::SkipWhitespaceAndComments() {
    had_line_terminator_before_ = false;
    while (pos_ < source_.size()) {
        char c = PeekChar();
        if (IsWhitespace(c)) {
            if (IsLineTerminator(c)) {
                had_line_terminator_before_ = true;
            }
            AdvanceChar();
        } else if (c == '/' && PeekChar2() == '/') {
            SkipLineComment();
        } else if (c == '/' && PeekChar2() == '*') {
            SkipBlockComment();
        } else {
            break;
        }
    }
}

void Lexer::SkipLineComment() {
    // Assumes we're at "//"
    AdvanceChar(); AdvanceChar();
    while (pos_ < source_.size() && !IsLineTerminator(PeekChar())) {
        AdvanceChar();
    }
}

void Lexer::SkipBlockComment() {
    // Assumes we're at "/*"
    AdvanceChar(); AdvanceChar();
    while (pos_ < source_.size()) {
        char c = AdvanceChar();
        if (c == '*' && PeekChar() == '/') {
            AdvanceChar();
            return;
        }
        if (IsLineTerminator(c)) {
            had_line_terminator_before_ = true;
        }
    }
    RecordError("unterminated block comment");
}

Token Lexer::ScanIdentifierOrKeyword() {
    const char* start = source_.data() + pos_;
    token_line_ = line_;
    token_column_ = column_;
    while (pos_ < source_.size() && IsIdentifierPart(PeekChar())) {
        AdvanceChar();
    }
    size_t length = (source_.data() + pos_) - start;
    TokenKind kind = LookupKeyword(std::string_view(start, length));
    Token t = MakeToken(kind, start, static_cast<uint32_t>(length));
    if (kind == TokenKind::kIdentifier) {
        t.identifier_value = std::string_view(start, length);
    }
    return t;
}

Token Lexer::ScanNumber() {
    const char* start = source_.data() + pos_;
    token_line_ = line_;
    token_column_ = column_;

    double value = 0.0;

    // Handle 0x, 0b, 0o prefixes
    if (PeekChar() == '0') {
        char next = PeekChar2();
        if (next == 'x' || next == 'X') {
            AdvanceChar(); AdvanceChar();
            if (!IsHexDigit(PeekChar())) {
                return MakeError("expected hex digit after 0x");
            }
            uint64_t v = 0;
            while (pos_ < source_.size() && IsHexDigit(PeekChar())) {
                char c = AdvanceChar();
                int digit = (c >= '0' && c <= '9') ? c - '0' :
                            (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                            c - 'A' + 10;
                v = v * 16 + digit;
            }
            value = static_cast<double>(v);
            Token t = MakeToken(TokenKind::kNumber, start,
                                static_cast<uint32_t>((source_.data() + pos_) - start));
            t.number_value = value;
            return t;
        }
        if (next == 'b' || next == 'B') {
            AdvanceChar(); AdvanceChar();
            if (!IsBinaryDigit(PeekChar())) {
                return MakeError("expected binary digit after 0b");
            }
            uint64_t v = 0;
            while (pos_ < source_.size() && IsBinaryDigit(PeekChar())) {
                v = v * 2 + (AdvanceChar() - '0');
            }
            value = static_cast<double>(v);
            Token t = MakeToken(TokenKind::kNumber, start,
                                static_cast<uint32_t>((source_.data() + pos_) - start));
            t.number_value = value;
            return t;
        }
        if (next == 'o' || next == 'O') {
            AdvanceChar(); AdvanceChar();
            if (!IsOctalDigit(PeekChar())) {
                return MakeError("expected octal digit after 0o");
            }
            uint64_t v = 0;
            while (pos_ < source_.size() && IsOctalDigit(PeekChar())) {
                v = v * 8 + (AdvanceChar() - '0');
            }
            value = static_cast<double>(v);
            Token t = MakeToken(TokenKind::kNumber, start,
                                static_cast<uint32_t>((source_.data() + pos_) - start));
            t.number_value = value;
            return t;
        }
    }

    // Decimal: integer part
    while (pos_ < source_.size() && IsDigit(PeekChar())) {
        value = value * 10.0 + (AdvanceChar() - '0');
    }

    // Fractional part
    if (PeekChar() == '.' && pos_ + 1 < source_.size() && IsDigit(source_[pos_ + 1])) {
        AdvanceChar();  // consume '.'
        double frac = 0.1;
        while (pos_ < source_.size() && IsDigit(PeekChar())) {
            value += (AdvanceChar() - '0') * frac;
            frac *= 0.1;
        }
    }

    // Exponent
    if (PeekChar() == 'e' || PeekChar() == 'E') {
        size_t saved = pos_;
        AdvanceChar();
        bool neg = false;
        if (PeekChar() == '+' || PeekChar() == '-') {
            neg = (AdvanceChar() == '-');
        }
        if (!IsDigit(PeekChar())) {
            // Not a valid exponent; back up and treat 'e' as start of identifier.
            pos_ = saved;
        } else {
            int exp = 0;
            while (pos_ < source_.size() && IsDigit(PeekChar())) {
                exp = exp * 10 + (AdvanceChar() - '0');
            }
            double mul = 1.0;
            double base = 10.0;
            while (exp > 0) {
                if (exp & 1) mul *= base;
                base *= base;
                exp >>= 1;
            }
            value = neg ? value / mul : value * mul;
        }
    }

    Token t = MakeToken(TokenKind::kNumber, start,
                        static_cast<uint32_t>((source_.data() + pos_) - start));
    t.number_value = value;
    return t;
}

Token Lexer::ScanString(char quote) {
    const char* start = source_.data() + pos_;
    token_line_ = line_;
    token_column_ = column_;
    AdvanceChar();  // consume opening quote

    std::string buf;
    bool has_escape = false;
    while (pos_ < source_.size() && PeekChar() != quote) {
        char c = PeekChar();
        if (c == '\n' || c == '\r') {
            return MakeError("unterminated string literal");
        }
        if (c == '\\') {
            has_escape = true;
            AdvanceChar();
            char esc = AdvanceChar();
            switch (esc) {
                case 'n': buf.push_back('\n'); break;
                case 't': buf.push_back('\t'); break;
                case 'r': buf.push_back('\r'); break;
                case 'b': buf.push_back('\b'); break;
                case 'f': buf.push_back('\f'); break;
                case 'v': buf.push_back('\v'); break;
                case '0': buf.push_back('\0'); break;
                case '\'': buf.push_back('\''); break;
                case '"': buf.push_back('"'); break;
                case '\\': buf.push_back('\\'); break;
                case 'x': {
                    if (pos_ + 2 > source_.size()) return MakeError("bad \\x escape");
                    int hi = PeekChar();
                    int lo = PeekChar2();
                    if (!IsHexDigit(hi) || !IsHexDigit(lo)) return MakeError("bad \\x escape");
                    auto hexval = [](char c) {
                        return (c >= '0' && c <= '9') ? c - '0' :
                               (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                               c - 'A' + 10;
                    };
                    buf.push_back(static_cast<char>(hexval(hi) * 16 + hexval(lo)));
                    AdvanceChar(); AdvanceChar();
                    break;
                }
                case 'u': {
                    if (pos_ + 4 > source_.size()) return MakeError("bad \\u escape");
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = PeekChar();
                        if (!IsHexDigit(h)) return MakeError("bad \\u escape");
                        auto hexval = [](char c) {
                            return (c >= '0' && c <= '9') ? c - '0' :
                                   (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                                   c - 'A' + 10;
                        };
                        cp = cp * 16 + hexval(h);
                        AdvanceChar();
                    }
                    if (cp < 0x80) {
                        buf.push_back(static_cast<char>(cp));
                    } else if (cp < 0x800) {
                        buf.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        buf.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        buf.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        buf.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        buf.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default:
                    buf.push_back(esc);
                    break;
            }
        } else {
            buf.push_back(c);
            AdvanceChar();
        }
    }
    if (pos_ >= source_.size()) {
        return MakeError("unterminated string literal");
    }
    AdvanceChar();  // consume closing quote

    Token t = MakeToken(TokenKind::kString, start,
                        static_cast<uint32_t>((source_.data() + pos_) - start));
    if (has_escape) {
        // Allocate in the arena so the string survives.
        char* buf_storage = arena_->NewArray<char>(buf.size() + 1);
        std::memcpy(buf_storage, buf.data(), buf.size());
        buf_storage[buf.size()] = '\0';
        t.string_value = std::string_view(buf_storage, buf.size());
    } else {
        // The string content is just the bytes between the quotes.
        t.string_value = std::string_view(start + 1,
                                          (source_.data() + pos_ - 1) - (start + 1));
    }
    return t;
}

Token Lexer::ScanPunctuation() {
    const char* start = source_.data() + pos_;
    token_line_ = line_;
    token_column_ = column_;
    char c = AdvanceChar();

    auto make = [&](TokenKind k, uint32_t len) {
        return MakeToken(k, start, len);
    };

    switch (c) {
        case '(': return make(TokenKind::kLParen, 1);
        case ')': return make(TokenKind::kRParen, 1);
        case '[': return make(TokenKind::kLBracket, 1);
        case ']': return make(TokenKind::kRBracket, 1);
        case '{': return make(TokenKind::kLBrace, 1);
        case '}': return make(TokenKind::kRBrace, 1);
        case ',': return make(TokenKind::kComma, 1);
        case ';': return make(TokenKind::kSemicolon, 1);
        case ':': return make(TokenKind::kColon, 1);
        case '~': return make(TokenKind::kBitNot, 1);
        case '?':
            if (PeekChar() == '?') {
                AdvanceChar();
                if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kNullishAssign, 3); }
                return make(TokenKind::kNullCoalesce, 2);
            }
            if (PeekChar() == '.') {
                // ?.  but only if the next char is not a digit
                if (!IsDigit(PeekChar2())) {
                    AdvanceChar();
                    return make(TokenKind::kOptChain, 2);
                }
            }
            return make(TokenKind::kQuestion, 1);
        case '.':
            if (PeekChar() == '.' && PeekChar2() == '.') {
                AdvanceChar(); AdvanceChar();
                return make(TokenKind::kSpread, 3);
            }
            if (IsDigit(PeekChar())) {
                // Should not happen - the number scanner should have caught this.
                pos_--;
                return ScanNumber();
            }
            return make(TokenKind::kDot, 1);
        case '+':
            if (PeekChar() == '+') { AdvanceChar(); return make(TokenKind::kInc, 2); }
            if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kAddAssign, 2); }
            return make(TokenKind::kPlus, 1);
        case '-':
            if (PeekChar() == '-') { AdvanceChar(); return make(TokenKind::kDec, 2); }
            if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kSubAssign, 2); }
            return make(TokenKind::kMinus, 1);
        case '*':
            if (PeekChar() == '*') {
                AdvanceChar();
                if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kExpAssign, 3); }
                return make(TokenKind::kExp, 2);
            }
            if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kMulAssign, 2); }
            return make(TokenKind::kStar, 1);
        case '/':
            if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kDivAssign, 2); }
            return make(TokenKind::kSlash, 1);
        case '%':
            if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kModAssign, 2); }
            return make(TokenKind::kPercent, 1);
        case '&':
            if (PeekChar() == '&') {
                AdvanceChar();
                if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kAndAssign, 3); }
                return make(TokenKind::kAnd, 2);
            }
            if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kBitAndAssign, 2); }
            return make(TokenKind::kBitAnd, 1);
        case '|':
            if (PeekChar() == '|') {
                AdvanceChar();
                if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kOrAssign, 3); }
                return make(TokenKind::kOr, 2);
            }
            if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kBitOrAssign, 2); }
            return make(TokenKind::kBitOr, 1);
        case '^':
            if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kBitXorAssign, 2); }
            return make(TokenKind::kBitXor, 1);
        case '<':
            if (PeekChar() == '<') {
                AdvanceChar();
                if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kShlAssign, 3); }
                return make(TokenKind::kShl, 2);
            }
            if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kLe, 2); }
            return make(TokenKind::kLt, 1);
        case '>':
            if (PeekChar() == '>') {
                AdvanceChar();
                if (PeekChar() == '>') {
                    AdvanceChar();
                    if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kUshrAssign, 4); }
                    return make(TokenKind::kUshr, 3);
                }
                if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kShrAssign, 3); }
                return make(TokenKind::kShr, 2);
            }
            if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kGe, 2); }
            return make(TokenKind::kGt, 1);
        case '=':
            if (PeekChar() == '=') {
                AdvanceChar();
                if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kEqEq, 3); }
                return make(TokenKind::kEq, 2);
            }
            if (PeekChar() == '>') { AdvanceChar(); return make(TokenKind::kArrow, 2); }
            return make(TokenKind::kAssign, 1);
        case '!':
            if (PeekChar() == '=') {
                AdvanceChar();
                if (PeekChar() == '=') { AdvanceChar(); return make(TokenKind::kNotEqEq, 3); }
                return make(TokenKind::kNotEq, 2);
            }
            return make(TokenKind::kNot, 1);
        default:
            return MakeError(std::string("unexpected character '") + c + "'");
    }
}

Token Lexer::ScanToken() {
    SkipWhitespaceAndComments();
    if (pos_ >= source_.size()) {
        Token t = MakeToken(TokenKind::kEof, source_.data() + source_.size(), 0);
        t.had_line_terminator_before = had_line_terminator_before_;
        return t;
    }
    char c = PeekChar();
    if (IsIdentifierStart(c)) return ScanIdentifierOrKeyword();
    if (IsDigit(c) || (c == '.' && pos_ + 1 < source_.size() && IsDigit(source_[pos_ + 1]))) {
        return ScanNumber();
    }
    if (c == '"' || c == '\'') return ScanString(c);
    return ScanPunctuation();
}

Token Lexer::Next() {
    if (lookahead_count_ > 0) {
        Token t = lookahead_[lookahead_pos_ % 2];
        lookahead_pos_ = (lookahead_pos_ + 1) % 2;
        --lookahead_count_;
        return t;
    }
    return ScanToken();
}

const Token& Lexer::Peek() const {
    if (lookahead_count_ == 0) {
        lookahead_[lookahead_pos_ % 2] = const_cast<Lexer*>(this)->ScanToken();
        ++lookahead_count_;
    }
    return lookahead_[lookahead_pos_ % 2];
}

const Token& Lexer::Peek2() const {
    while (lookahead_count_ < 2) {
        lookahead_[(lookahead_pos_ + lookahead_count_) % 2] = const_cast<Lexer*>(this)->ScanToken();
        ++lookahead_count_;
    }
    return lookahead_[(lookahead_pos_ + 1) % 2];
}

void Lexer::Rewind(const Token& tok) {
    // Only allow rewinding if we have room in the lookahead buffer.
    // In practice the parser only rewinds one token.
    lookahead_pos_ = (lookahead_pos_ - 1 + 2) % 2;
    lookahead_[lookahead_pos_] = tok;
    ++lookahead_count_;
}

void Lexer::RecordError(std::string msg) {
    if (!has_error_) {
        has_error_ = true;
        error_message_ = std::move(msg);
    }
}

Token Lexer::MakeError(std::string msg) {
    RecordError(std::move(msg));
    Token t = MakeToken(TokenKind::kError, source_.data() + pos_, 0);
    return t;
}

}  // namespace v12
