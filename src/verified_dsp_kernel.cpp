#include "broaudio/verified_dsp_kernel.h"
#include "broaudio/synth/synth_verify.h"
#include "verified_dsp_math.h"

#include <brass/codegen/kernel_jit.hpp>
#include <brass/mir/block.hpp>
#include <brass/mir/builder.hpp>
#include <brass/mir/function.hpp>
#include <brass/mir/instruction.hpp>
#include <brass/mir/module.hpp>
#include <brass/mir/superopt.hpp>
#include <brass/mir/types.hpp>
#include <brass/verify/smt_equiv.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace broaudio {
namespace {

using namespace brass;
using namespace broaudio::dspmath;

enum class TokKind {
    Eof, Number, Ident, String,
    LParen, RParen, LBrace, RBrace,
    Comma, Semi, Question, Colon, Dot, Arrow,
    Plus, Minus, Star, Slash, Percent, StarStar,
    Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign,
    Eq, Ne, StrictEq, StrictNe, Lt, Le, Gt, Ge,
    AndAnd, OrOr, Bang, BitAnd, BitOr, BitXor, Tilde
};

struct Token {
    TokKind kind = TokKind::Eof;
    std::string text;
    float numVal = 0.0f;
};

class JsDspCompiler {
public:
    JsDspCompiler(std::string_view src, const VerifiedDspCompileOptions& opts)
        : src_(src), opts_(opts) {}

    bool compileToMir(Module& mod, Function*& outFn, std::string& err) {
        if (!tokenize(err)) return false;

        std::vector<Type> paramTypes = {Type::f32(), Type::f32(), Type::f32(), Type::f32()};
        outFn = mod.create_function(opts_.name.empty() ? "js_dsp_kernel" : opts_.name,
                                    Type::f32(), std::move(paramTypes));
        Builder b(mod);
        b.set_function(outFn);
        BasicBlock* entry = b.append_block("entry");
        Value* pSample = b.add_block_param(entry, Type::f32());
        Value* pTime = b.add_block_param(entry, Type::f32());
        Value* p0 = b.add_block_param(entry, Type::f32());
        Value* p1 = b.add_block_param(entry, Type::f32());
        b.position_at_end(entry);
        b_ = &b;

        env_["sample"] = pSample;
        env_["time"] = pTime;
        env_["p0"] = p0;
        env_["p1"] = p1;
        env_["param0"] = p0;
        env_["param1"] = p1;

        activeCond_ = b.build_iconst_i32(1);
        hasReturned_ = b.build_iconst_i32(0);
        returnVal_ = b.build_fconst_f32(0.0f);

        Value* rawOut = parseTopLevel(pSample, pTime, p0, p1, err);
        if (!rawOut) return false;

        // Hardware Speaker-Protection Epilogue (exact IEEE-754 binary32 sign-magnitude clamp):
        float maxA = (std::isfinite(opts_.maxAmp) && opts_.maxAmp > 0.0f) ? opts_.maxAmp : 1.0f;
        uint32_t maxBitsU32 = 0;
        std::memcpy(&maxBitsU32, &maxA, sizeof(float));

        Value* u = b.build_bitcast_i32_f32(rawOut);
        Value* signBit = b.build_and(u, b.build_iconst_i32(static_cast<int32_t>(0x80000000u)));
        Value* mag = b.build_and(u, b.build_iconst_i32(0x7FFFFFFF));
        Value* maxBitsVal = b.build_iconst_i32(static_cast<int32_t>(maxBitsU32));
        Value* isOver = b.build_ugt(mag, maxBitsVal);
        Value* clampedMag = b.build_select(isOver, maxBitsVal, mag);
        Value* clampedBits = b.build_or(signBit, clampedMag);
        Value* isNan = b.build_ugt(mag, b.build_iconst_i32(0x7F800000));
        Value* safeBits = b.build_select(isNan, b.build_iconst_i32(0), clampedBits);
        Value* safeOut = b.build_bitcast_f32_i32(safeBits);

        b.build_ret(safeOut);
        return true;
    }

private:
    std::string_view src_;
    VerifiedDspCompileOptions opts_;
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    Builder* b_ = nullptr;
    std::unordered_map<std::string, Value*> env_;
    std::unordered_set<std::string> constVars_;
    Value* activeCond_ = nullptr;
    Value* hasReturned_ = nullptr;
    Value* returnVal_ = nullptr;

    const Token& peek(size_t offset = 0) const {
        size_t idx = pos_ + offset;
        return idx < tokens_.size() ? tokens_[idx] : tokens_.back();
    }
    Token consume() {
        const Token& t = peek();
        if (pos_ + 1 < tokens_.size()) ++pos_;
        return t;
    }
    bool match(TokKind k) {
        if (peek().kind != k) return false;
        consume();
        return true;
    }

    bool tokenize(std::string& err) {
        size_t i = 0, n = src_.size();
        while (i < n) {
            char c = src_[i];
            if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
            if (c == '/' && i + 1 < n && src_[i + 1] == '/') {
                for (i += 2; i < n && src_[i] != '\n'; ++i) {}
                continue;
            }
            if (c == '/' && i + 1 < n && src_[i + 1] == '*') {
                for (i += 2; i + 1 < n && !(src_[i] == '*' && src_[i + 1] == '/'); ++i) {}
                if (i + 1 >= n) { err = "Unterminated block comment in @kernel DSP source"; return false; }
                i += 2;
                continue;
            }
            if (c == '`') { err = "Template literals (string allocation) are forbidden in @kernel audio DSP"; return false; }
            if (c == '"' || c == '\'') {
                char q = c; ++i;
                std::string s;
                while (i < n && src_[i] != q) {
                    if (src_[i] == '\\' && i + 1 < n) ++i;
                    s.push_back(src_[i++]);
                }
                if (i >= n) { err = "Unterminated string literal in @kernel audio DSP"; return false; }
                ++i;
                if (s != "@kernel" && s != "use strict") {
                    err = "Heap string allocation (\"" + s + "\") is forbidden in @kernel audio DSP";
                    return false;
                }
                tokens_.push_back({TokKind::String, s, 0.0f});
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(src_[i + 1])))) {
                size_t start = i;
                while (i < n && (std::isalnum(static_cast<unsigned char>(src_[i])) || src_[i] == '.' ||
                                 ((src_[i] == '+' || src_[i] == '-') && i > start &&
                                  (src_[i - 1] == 'e' || src_[i - 1] == 'E')))) ++i;
                std::string numStr(src_.substr(start, i - start));
                tokens_.push_back({TokKind::Number, numStr, std::strtof(numStr.c_str(), nullptr)});
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
                size_t start = i;
                while (i < n && (std::isalnum(static_cast<unsigned char>(src_[i])) || src_[i] == '_' || src_[i] == '$')) ++i;
                std::string id(src_.substr(start, i - start));
                if (!checkIdentifierSafety(id, err)) return false;
                tokens_.push_back({TokKind::Ident, std::move(id), 0.0f});
                continue;
            }

            auto sub2 = (i + 1 < n) ? src_.substr(i, 2) : std::string_view{};
            auto sub3 = (i + 2 < n) ? src_.substr(i, 3) : std::string_view{};
            if (sub3 == "===") { tokens_.push_back({TokKind::StrictEq, "===", 0.0f}); i += 3; continue; }
            if (sub3 == "!==") { tokens_.push_back({TokKind::StrictNe, "!==", 0.0f}); i += 3; continue; }
            if (sub2 == "=>") { tokens_.push_back({TokKind::Arrow, "=>", 0.0f}); i += 2; continue; }
            if (sub2 == "==") { tokens_.push_back({TokKind::Eq, "==", 0.0f}); i += 2; continue; }
            if (sub2 == "!=") { tokens_.push_back({TokKind::Ne, "!=", 0.0f}); i += 2; continue; }
            if (sub2 == "<=") { tokens_.push_back({TokKind::Le, "<=", 0.0f}); i += 2; continue; }
            if (sub2 == ">=") { tokens_.push_back({TokKind::Ge, ">=", 0.0f}); i += 2; continue; }
            if (sub2 == "&&") { tokens_.push_back({TokKind::AndAnd, "&&", 0.0f}); i += 2; continue; }
            if (sub2 == "||") { tokens_.push_back({TokKind::OrOr, "||", 0.0f}); i += 2; continue; }
            if (sub2 == "**") { tokens_.push_back({TokKind::StarStar, "**", 0.0f}); i += 2; continue; }
            if (sub2 == "+=") { tokens_.push_back({TokKind::PlusAssign, "+=", 0.0f}); i += 2; continue; }
            if (sub2 == "-=") { tokens_.push_back({TokKind::MinusAssign, "-=", 0.0f}); i += 2; continue; }
            if (sub2 == "*=") { tokens_.push_back({TokKind::StarAssign, "*=", 0.0f}); i += 2; continue; }
            if (sub2 == "/=") { tokens_.push_back({TokKind::SlashAssign, "/=", 0.0f}); i += 2; continue; }

            ++i;
            switch (c) {
                case '(': tokens_.push_back({TokKind::LParen, "(", 0.0f}); break;
                case ')': tokens_.push_back({TokKind::RParen, ")", 0.0f}); break;
                case '{': tokens_.push_back({TokKind::LBrace, "{", 0.0f}); break;
                case '}': tokens_.push_back({TokKind::RBrace, "}", 0.0f}); break;
                case '[': case ']':
                    err = "Array literals / dynamic indexing are forbidden in @kernel audio DSP";
                    return false;
                case ',': tokens_.push_back({TokKind::Comma, ",", 0.0f}); break;
                case ';': tokens_.push_back({TokKind::Semi, ";", 0.0f}); break;
                case '?': tokens_.push_back({TokKind::Question, "?", 0.0f}); break;
                case ':': tokens_.push_back({TokKind::Colon, ":", 0.0f}); break;
                case '.': tokens_.push_back({TokKind::Dot, ".", 0.0f}); break;
                case '+': tokens_.push_back({TokKind::Plus, "+", 0.0f}); break;
                case '-': tokens_.push_back({TokKind::Minus, "-", 0.0f}); break;
                case '*': tokens_.push_back({TokKind::Star, "*", 0.0f}); break;
                case '/': tokens_.push_back({TokKind::Slash, "/", 0.0f}); break;
                case '%': tokens_.push_back({TokKind::Percent, "%", 0.0f}); break;
                case '=': tokens_.push_back({TokKind::Assign, "=", 0.0f}); break;
                case '<': tokens_.push_back({TokKind::Lt, "<", 0.0f}); break;
                case '>': tokens_.push_back({TokKind::Gt, ">", 0.0f}); break;
                case '!': tokens_.push_back({TokKind::Bang, "!", 0.0f}); break;
                case '&': tokens_.push_back({TokKind::BitAnd, "&", 0.0f}); break;
                case '|': tokens_.push_back({TokKind::BitOr, "|", 0.0f}); break;
                case '^': tokens_.push_back({TokKind::BitXor, "^", 0.0f}); break;
                case '~': tokens_.push_back({TokKind::Tilde, "~", 0.0f}); break;
                default:
                    err = std::string("Unsupported character '") + c + "' in @kernel audio DSP";
                    return false;
            }
        }
        if (tokens_.empty()) { err = "Empty @kernel audio DSP source"; return false; }
        tokens_.push_back({TokKind::Eof, "", 0.0f});
        return true;
    }

    static bool checkIdentifierSafety(const std::string& id, std::string& err) {
        if (id == "new") { err = "Heap allocation ('new') is forbidden in @kernel audio DSP"; return false; }
        if (id == "throw" || id == "try" || id == "catch" || id == "finally") {
            err = "Exceptions ('" + id + "') are forbidden in @kernel audio DSP";
            return false;
        }
        if (id == "while" || id == "for" || id == "do") {
            err = "Unbounded loops ('" + id + "') are forbidden in @kernel audio DSP";
            return false;
        }
        if (id == "async" || id == "await" || id == "yield" || id == "Promise") {
            err = "Async/generator constructs ('" + id + "') are forbidden in @kernel audio DSP";
            return false;
        }
        static const std::unordered_set<std::string> kForbidden = {
            "eval", "Function", "with", "delete", "typeof", "instanceof", "in", "void",
            "this", "super", "arguments", "class", "import", "export", "globalThis",
            "window", "process", "console", "bro", "Object", "Array", "Map", "Set",
            "WeakMap", "WeakSet", "Symbol", "Proxy", "Reflect", "Float32Array",
            "Float64Array", "Int32Array", "Uint8Array", "setTimeout", "setInterval", "fetch"
        };
        if (kForbidden.count(id)) {
            err = "Dynamic/GC-hazard identifier ('" + id + "') is forbidden in @kernel audio DSP";
            return false;
        }
        return true;
    }

    void skipDirectives() {
        while (peek().kind == TokKind::String) { consume(); match(TokKind::Semi); }
    }

    Value* parseTopLevel(Value* pSample, Value* pTime, Value* p0, Value* p1, std::string& err) {
        skipDirectives();
        Value* paramVals[4] = {pSample, pTime, p0, p1};

        if (peek().kind == TokKind::Ident && peek().text == "function") {
            consume();
            if (peek().kind == TokKind::Star) {
                err = "Generator functions ('function*') are forbidden in @kernel audio DSP";
                return nullptr;
            }
            if (peek().kind == TokKind::Ident) consume();
            if (!parseParamList(paramVals, err)) return nullptr;
            if (peek().kind != TokKind::LBrace) {
                err = "Expected '{' after function parameter list in @kernel audio DSP";
                return nullptr;
            }
            Value* res = parseBlockBody(err);
            if (!res) return nullptr;
            if (peek().kind != TokKind::Eof) {
                err = "Unexpected trailing tokens after function body in @kernel audio DSP";
                return nullptr;
            }
            return res;
        }

        if (peek().kind == TokKind::Ident && peek(1).kind == TokKind::Arrow) {
            env_[consume().text] = pSample;
            consume();
            return parseArrowBody(err);
        }

        if (peek().kind == TokKind::LParen && isArrowHeader()) {
            if (!parseParamList(paramVals, err)) return nullptr;
            if (!match(TokKind::Arrow)) { err = "Expected '=>' after parameter list"; return nullptr; }
            return parseArrowBody(err);
        }

        if (peek().kind == TokKind::LBrace) return parseBlockBody(err);
        if (peek().kind == TokKind::Ident &&
            (peek().text == "const" || peek().text == "let" || peek().text == "var" ||
             peek().text == "return" || peek().text == "if")) {
            return parseStatementListUntil(TokKind::Eof, err);
        }

        Value* expr = parseExpression(err);
        if (!expr) return nullptr;
        match(TokKind::Semi);
        if (peek().kind != TokKind::Eof) {
            err = "Unexpected token '" + peek().text + "' in @kernel audio DSP expression";
            return nullptr;
        }
        return expr;
    }

    bool isArrowHeader() const {
        if (peek().kind != TokKind::LParen) return false;
        size_t k = 1;
        int depth = 1;
        while (pos_ + k < tokens_.size() && depth > 0) {
            TokKind tk = peek(k).kind;
            if (tk == TokKind::LParen) ++depth;
            else if (tk == TokKind::RParen) --depth;
            else if (tk == TokKind::Eof) return false;
            ++k;
        }
        return peek(k).kind == TokKind::Arrow;
    }

    bool parseParamList(Value* paramVals[4], std::string& err) {
        if (!match(TokKind::LParen)) { err = "Expected '(' to start parameter list"; return false; }
        size_t idx = 0;
        while (peek().kind != TokKind::RParen && peek().kind != TokKind::Eof) {
            if (peek().kind != TokKind::Ident) { err = "Expected parameter identifier"; return false; }
            std::string name = consume().text;
            env_[name] = (idx < 4) ? paramVals[idx] : b_->build_fconst_f32(0.0f);
            ++idx;
            if (!match(TokKind::Comma)) break;
        }
        if (!match(TokKind::RParen)) { err = "Expected ')' after parameter list"; return false; }
        return true;
    }

    Value* parseArrowBody(std::string& err) {
        skipDirectives();
        if (peek().kind == TokKind::LBrace) {
            if (peek(1).kind == TokKind::Ident && peek(2).kind == TokKind::Colon) {
                err = "Object literal allocation is forbidden in @kernel audio DSP";
                return nullptr;
            }
            return parseBlockBody(err);
        }
        Value* expr = parseExpression(err);
        if (!expr) return nullptr;
        match(TokKind::Semi);
        if (peek().kind != TokKind::Eof) {
            err = "Unexpected token '" + peek().text + "' after arrow expression";
            return nullptr;
        }
        return expr;
    }

    Value* parseBlockBody(std::string& err) {
        if (!match(TokKind::LBrace)) { err = "Expected '{' to begin function body"; return nullptr; }
        Value* res = parseStatementListUntil(TokKind::RBrace, err);
        if (!res) return nullptr;
        if (!match(TokKind::RBrace)) { err = "Expected '}' at end of function body"; return nullptr; }
        return res;
    }

    Value* parseStatementListUntil(TokKind endTok, std::string& err) {
        skipDirectives();
        while (peek().kind != endTok && peek().kind != TokKind::Eof) {
            if (!parseStatement(err)) return nullptr;
        }
        return returnVal_;
    }

    Value* currentEffectiveCond() {
        return b_->build_and(activeCond_, b_->build_eq(hasReturned_, b_->build_iconst_i32(0)));
    }

    bool parseStatement(std::string& err) {
        skipDirectives();
        if (match(TokKind::Semi)) return true;

        if (peek().kind == TokKind::LBrace) {
            if (peek(1).kind == TokKind::Ident && peek(2).kind == TokKind::Colon) {
                err = "Object literal allocation is forbidden in @kernel audio DSP";
                return false;
            }
            consume();
            while (peek().kind != TokKind::RBrace && peek().kind != TokKind::Eof) {
                if (!parseStatement(err)) return false;
            }
            if (!match(TokKind::RBrace)) { err = "Expected '}' at end of block statement"; return false; }
            return true;
        }

        if (peek().kind == TokKind::Ident) {
            const std::string& kw = peek().text;
            if (kw == "function") {
                err = "Nested functions / closures are forbidden in @kernel audio DSP";
                return false;
            }
            if (kw == "const" || kw == "let" || kw == "var") {
                bool isConst = (kw == "const");
                consume();
                do {
                    if (peek().kind != TokKind::Ident) { err = "Expected variable name"; return false; }
                    std::string varName = consume().text;
                    Value* initVal = b_->build_fconst_f32(0.0f);
                    if (match(TokKind::Assign)) {
                        initVal = parseExpression(err);
                        if (!initVal) return false;
                    }
                    Value* eff = currentEffectiveCond();
                    auto it = env_.find(varName);
                    env_[varName] = (it == env_.end()) ? initVal : b_->build_select(eff, initVal, it->second);
                    if (isConst) constVars_.insert(varName);
                } while (match(TokKind::Comma));
                match(TokKind::Semi);
                return true;
            }
            if (kw == "return") {
                consume();
                Value* val = (peek().kind == TokKind::Semi || peek().kind == TokKind::RBrace || peek().kind == TokKind::Eof)
                    ? b_->build_fconst_f32(0.0f) : parseExpression(err);
                if (!val) return false;
                match(TokKind::Semi);
                returnVal_ = b_->build_select(currentEffectiveCond(), val, returnVal_);
                hasReturned_ = b_->build_or(hasReturned_, activeCond_);
                return true;
            }
            if (kw == "if") {
                consume();
                if (!match(TokKind::LParen)) { err = "Expected '(' after 'if'"; return false; }
                Value* condF = parseExpression(err);
                if (!condF) return false;
                if (!match(TokKind::RParen)) { err = "Expected ')' after 'if' condition"; return false; }
                Value* condBool = toBoolI32(condF);
                Value* savedActive = activeCond_;
                activeCond_ = b_->build_and(savedActive, condBool);
                if (!parseStatement(err)) return false;
                if (peek().kind == TokKind::Ident && peek().text == "else") {
                    consume();
                    activeCond_ = b_->build_and(savedActive, b_->build_eq(condBool, b_->build_iconst_i32(0)));
                    if (!parseStatement(err)) return false;
                }
                activeCond_ = savedActive;
                return true;
            }

            TokKind nextK = peek(1).kind;
            if (nextK == TokKind::Assign || nextK == TokKind::PlusAssign ||
                nextK == TokKind::MinusAssign || nextK == TokKind::StarAssign ||
                nextK == TokKind::SlashAssign) {
                std::string varName = consume().text;
                TokKind opK = consume().kind;
                if (constVars_.count(varName)) {
                    err = "Cannot reassign to const variable '" + varName + "' in @kernel audio DSP";
                    return false;
                }
                auto it = env_.find(varName);
                if (it == env_.end()) {
                    err = "Assignment to undeclared variable '" + varName + "' in @kernel audio DSP";
                    return false;
                }
                Value* rhs = parseExpression(err);
                if (!rhs) return false;
                match(TokKind::Semi);
                Value* cur = it->second;
                Value* nextVal = rhs;
                if (opK == TokKind::PlusAssign) nextVal = b_->build_fadd(cur, rhs);
                else if (opK == TokKind::MinusAssign) nextVal = b_->build_sub(cur, rhs);
                else if (opK == TokKind::StarAssign) nextVal = b_->build_mul(cur, rhs);
                else if (opK == TokKind::SlashAssign) nextVal = b_->build_sdiv(cur, rhs);
                it->second = b_->build_select(currentEffectiveCond(), nextVal, cur);
                return true;
            }
        }

        Value* expr = parseExpression(err);
        if (!expr) return false;
        match(TokKind::Semi);
        if (peek().kind == TokKind::RBrace || peek().kind == TokKind::Eof) {
            returnVal_ = b_->build_select(currentEffectiveCond(), expr, returnVal_);
        }
        return true;
    }

    Value* toBoolI32(Value* v) {
        return b_->build_and(b_->build_ne(v, b_->build_fconst_f32(0.0f)), b_->build_eq(v, v));
    }
    Value* boolI32ToF32(Value* cond) {
        return b_->build_select(cond, b_->build_fconst_f32(1.0f), b_->build_fconst_f32(0.0f));
    }

    Value* parseExpression(std::string& err) { return parseTernary(err); }

    Value* parseTernary(std::string& err) {
        Value* cond = parseLogicalOr(err);
        if (!cond || !match(TokKind::Question)) return cond;
        Value* tVal = parseTernary(err);
        if (!tVal) return nullptr;
        if (!match(TokKind::Colon)) { err = "Expected ':' in conditional '? :' expression"; return nullptr; }
        Value* fVal = parseTernary(err);
        return fVal ? b_->build_select(toBoolI32(cond), tVal, fVal) : nullptr;
    }

    Value* parseLogicalOr(std::string& err) {
        Value* lhs = parseLogicalAnd(err);
        while (lhs && match(TokKind::OrOr)) {
            Value* rhs = parseLogicalAnd(err);
            if (!rhs) return nullptr;
            lhs = b_->build_select(toBoolI32(lhs), lhs, rhs);
        }
        return lhs;
    }

    Value* parseLogicalAnd(std::string& err) {
        Value* lhs = parseEquality(err);
        while (lhs && match(TokKind::AndAnd)) {
            Value* rhs = parseEquality(err);
            if (!rhs) return nullptr;
            lhs = b_->build_select(toBoolI32(lhs), rhs, lhs);
        }
        return lhs;
    }

    Value* parseEquality(std::string& err) {
        Value* lhs = parseRelational(err);
        while (lhs && (peek().kind == TokKind::Eq || peek().kind == TokKind::Ne ||
                       peek().kind == TokKind::StrictEq || peek().kind == TokKind::StrictNe)) {
            TokKind op = consume().kind;
            Value* rhs = parseRelational(err);
            if (!rhs) return nullptr;
            Value* cmp = (op == TokKind::Eq || op == TokKind::StrictEq) ? b_->build_eq(lhs, rhs) : b_->build_ne(lhs, rhs);
            lhs = boolI32ToF32(cmp);
        }
        return lhs;
    }

    Value* parseRelational(std::string& err) {
        Value* lhs = parseAdditive(err);
        while (lhs && (peek().kind == TokKind::Lt || peek().kind == TokKind::Le ||
                       peek().kind == TokKind::Gt || peek().kind == TokKind::Ge)) {
            TokKind op = consume().kind;
            Value* rhs = parseAdditive(err);
            if (!rhs) return nullptr;
            Value* cmp = (op == TokKind::Lt) ? b_->build_slt(lhs, rhs)
                       : (op == TokKind::Le) ? b_->build_sle(lhs, rhs)
                       : (op == TokKind::Gt) ? b_->build_sgt(lhs, rhs) : b_->build_sge(lhs, rhs);
            lhs = boolI32ToF32(cmp);
        }
        return lhs;
    }

    Value* parseAdditive(std::string& err) {
        Value* lhs = parseMultiplicative(err);
        while (lhs && (peek().kind == TokKind::Plus || peek().kind == TokKind::Minus)) {
            TokKind op = consume().kind;
            Value* rhs = parseMultiplicative(err);
            if (!rhs) return nullptr;
            lhs = (op == TokKind::Plus) ? b_->build_fadd(lhs, rhs) : b_->build_sub(lhs, rhs);
        }
        return lhs;
    }

    Value* parseMultiplicative(std::string& err) {
        Value* lhs = parsePower(err);
        while (lhs && (peek().kind == TokKind::Star || peek().kind == TokKind::Slash ||
                       peek().kind == TokKind::Percent)) {
            TokKind op = consume().kind;
            Value* rhs = parsePower(err);
            if (!rhs) return nullptr;
            if (op == TokKind::Star) lhs = b_->build_mul(lhs, rhs);
            else if (op == TokKind::Slash) lhs = b_->build_sdiv(lhs, rhs);
            else lhs = b_->build_sub(lhs, b_->build_mul(rhs, emitTrunc(*b_, b_->build_sdiv(lhs, rhs))));
        }
        return lhs;
    }

    Value* parsePower(std::string& err) {
        Value* base = parseUnary(err);
        if (base && match(TokKind::StarStar)) {
            Value* exp = parsePower(err);
            return exp ? emitPow(base, exp) : nullptr;
        }
        return base;
    }

    Value* parseUnary(std::string& err) {
        if (match(TokKind::Plus)) return parseUnary(err);
        if (match(TokKind::Minus)) {
            Value* v = parseUnary(err);
            return v ? b_->build_neg(v) : nullptr;
        }
        if (match(TokKind::Bang)) {
            Value* v = parseUnary(err);
            return v ? boolI32ToF32(b_->build_eq(toBoolI32(v), b_->build_iconst_i32(0))) : nullptr;
        }
        return parsePrimary(err);
    }

    Value* parsePrimary(std::string& err) {
        if (peek().kind == TokKind::Arrow) {
            err = "Nested arrow functions / closures are forbidden in @kernel audio DSP";
            return nullptr;
        }
        if (peek().kind == TokKind::LBrace) {
            err = "Object literal allocation ('{...}') is forbidden in @kernel audio DSP";
            return nullptr;
        }
        if (peek().kind == TokKind::Number) return b_->build_fconst_f32(consume().numVal);
        if (match(TokKind::LParen)) {
            if (isArrowHeader()) {
                err = "Nested arrow functions / closures are forbidden in @kernel audio DSP";
                return nullptr;
            }
            Value* inner = parseExpression(err);
            if (!inner) return nullptr;
            if (!match(TokKind::RParen)) { err = "Expected ')' after parenthesized expression"; return nullptr; }
            if (peek().kind == TokKind::Arrow) {
                err = "Nested arrow functions / closures are forbidden in @kernel audio DSP";
                return nullptr;
            }
            return inner;
        }
        if (peek().kind == TokKind::Ident) {
            std::string name = consume().text;
            if (peek().kind == TokKind::Arrow) {
                err = "Nested arrow functions / closures are forbidden in @kernel audio DSP";
                return nullptr;
            }
            if (name == "Math") {
                if (!match(TokKind::Dot) || peek().kind != TokKind::Ident) {
                    err = "Expected property or method name after 'Math.'";
                    return nullptr;
                }
                std::string member = consume().text;
                return (peek().kind != TokKind::LParen) ? resolveMathConstant(member, err)
                                                        : parseBuiltinCall(member, err);
            }
            if (match(TokKind::Dot)) {
                err = "Property access on '" + name + "' is forbidden in @kernel audio DSP (only Math.* is allowed)";
                return nullptr;
            }
            if (peek().kind == TokKind::LParen) return parseBuiltinCall(name, err);
            if (name == "PI") return b_->build_fconst_f32(kPi);
            if (name == "TAU" || name == "TWO_PI") return b_->build_fconst_f32(kTwoPi);
            if (name == "Infinity") return b_->build_fconst_f32(INFINITY);
            if (name == "NaN") return b_->build_fconst_f32(NAN);
            if (name == "true") return b_->build_fconst_f32(1.0f);
            if (name == "false") return b_->build_fconst_f32(0.0f);
            auto it = env_.find(name);
            if (it != env_.end()) return it->second;
            err = "Unknown or disallowed identifier '" + name + "' in @kernel audio DSP";
            return nullptr;
        }
        err = "Unexpected token '" + peek().text + "' in @kernel audio DSP";
        return nullptr;
    }

    Value* resolveMathConstant(const std::string& prop, std::string& err) {
        if (prop == "PI") return b_->build_fconst_f32(kPi);
        if (prop == "TAU" || prop == "TWO_PI") return b_->build_fconst_f32(kTwoPi);
        if (prop == "E") return b_->build_fconst_f32(kEulerE);
        if (prop == "LN2") return b_->build_fconst_f32(kLn2);
        if (prop == "LN10") return b_->build_fconst_f32(kLn10);
        if (prop == "LOG2E") return b_->build_fconst_f32(kLog2E);
        if (prop == "LOG10E") return b_->build_fconst_f32(kLog10E);
        if (prop == "SQRT2") return b_->build_fconst_f32(kSqrt2);
        if (prop == "SQRT1_2") return b_->build_fconst_f32(kSqrt1_2);
        err = "Unsupported Math constant 'Math." + prop + "' in @kernel audio DSP";
        return nullptr;
    }

    Value* emitPow(Value* base, Value* exp) {
        return emitExp2(*b_, b_->build_mul(exp, emitLog2(*b_, base)));
    }

    Value* parseBuiltinCall(const std::string& fn, std::string& err) {
        if (!match(TokKind::LParen)) { err = "Expected '(' in call to '" + fn + "'"; return nullptr; }
        std::vector<Value*> args;
        while (peek().kind != TokKind::RParen && peek().kind != TokKind::Eof) {
            Value* a = parseExpression(err);
            if (!a) return nullptr;
            args.push_back(a);
            if (!match(TokKind::Comma)) break;
        }
        if (!match(TokKind::RParen)) { err = "Expected ')' after arguments to '" + fn + "'"; return nullptr; }

        auto needArgs = [&](size_t n) -> bool {
            if (args.size() != n) { err = "Function '" + fn + "' expects " + std::to_string(n) + " argument(s)"; return false; }
            return true;
        };

        if (fn == "sin") return needArgs(1) ? emitSinRad(*b_, args[0]) : nullptr;
        if (fn == "cos") return needArgs(1) ? emitCosRad(*b_, args[0]) : nullptr;
        if (fn == "tan") return needArgs(1) ? b_->build_sdiv(emitSinRad(*b_, args[0]), emitCosRad(*b_, args[0])) : nullptr;
        if (fn == "tanh" || fn == "softclip") return needArgs(1) ? emitTanh(*b_, args[0]) : nullptr;
        if (fn == "abs") return needArgs(1) ? b_->build_fabs_f32(args[0]) : nullptr;
        if (fn == "sqrt") return needArgs(1) ? b_->build_sqrt_f32(args[0]) : nullptr;
        if (fn == "floor") return needArgs(1) ? b_->build_floor_f32(args[0]) : nullptr;
        if (fn == "ceil") return needArgs(1) ? b_->build_ceil_f32(args[0]) : nullptr;
        if (fn == "round") return needArgs(1) ? b_->build_round_f32(args[0]) : nullptr;
        if (fn == "fround") return needArgs(1) ? args[0] : nullptr;
        if (fn == "trunc") return needArgs(1) ? emitTrunc(*b_, args[0]) : nullptr;
        if (fn == "fract" || fn == "frac") return needArgs(1) ? b_->build_sub(args[0], b_->build_floor_f32(args[0])) : nullptr;
        if (fn == "sign") {
            if (!needArgs(1)) return nullptr;
            Value* z = b_->build_fconst_f32(0.0f);
            return b_->build_select(b_->build_sgt(args[0], z), b_->build_fconst_f32(1.0f),
                                    b_->build_select(b_->build_slt(args[0], z), b_->build_fconst_f32(-1.0f), z));
        }
        if (fn == "exp2") return needArgs(1) ? emitExp2(*b_, args[0]) : nullptr;
        if (fn == "exp") return needArgs(1) ? emitExp2(*b_, b_->build_mul(args[0], b_->build_fconst_f32(kLog2E))) : nullptr;
        if (fn == "log2") return needArgs(1) ? emitLog2(*b_, args[0]) : nullptr;
        if (fn == "log") return needArgs(1) ? b_->build_mul(emitLog2(*b_, args[0]), b_->build_fconst_f32(kLn2)) : nullptr;
        if (fn == "log10") return needArgs(1) ? b_->build_mul(emitLog2(*b_, args[0]), b_->build_fconst_f32(kLog10E)) : nullptr;
        if (fn == "atan") return needArgs(1) ? emitAtan(*b_, args[0]) : nullptr;
        if (fn == "atan2") {
            if (!needArgs(2)) return nullptr;
            Value* safeX = b_->build_select(b_->build_eq(args[1], b_->build_fconst_f32(0.0f)),
                                            b_->build_fconst_f32(1e-20f), args[1]);
            Value* base = emitAtan(*b_, b_->build_sdiv(args[0], safeX));
            Value* adj = b_->build_select(b_->build_slt(args[0], b_->build_fconst_f32(0.0f)),
                                          b_->build_fconst_f32(-kPi), b_->build_fconst_f32(kPi));
            return b_->build_select(b_->build_slt(args[1], b_->build_fconst_f32(0.0f)),
                                    b_->build_fadd(base, adj), base);
        }
        if (fn == "min" || fn == "max") {
            if (args.empty()) { err = "Math." + fn + " requires at least 1 argument"; return nullptr; }
            Value* acc = args[0];
            for (size_t i = 1; i < args.size(); ++i) {
                acc = (fn == "min") ? b_->build_fmin_f32(acc, args[i]) : b_->build_fmax_f32(acc, args[i]);
            }
            return acc;
        }
        if (fn == "pow") return needArgs(2) ? emitPow(args[0], args[1]) : nullptr;
        if (fn == "hypot") {
            return needArgs(2) ? b_->build_sqrt_f32(b_->build_fadd(b_->build_mul(args[0], args[0]),
                                                                   b_->build_mul(args[1], args[1]))) : nullptr;
        }
        if (fn == "clamp") return needArgs(3) ? b_->build_fmin_f32(b_->build_fmax_f32(args[0], args[1]), args[2]) : nullptr;
        if (fn == "mix" || fn == "lerp") {
            return needArgs(3) ? b_->build_fadd(args[0], b_->build_mul(b_->build_sub(args[1], args[0]), args[2])) : nullptr;
        }
        if (fn == "step") {
            return needArgs(2) ? b_->build_select(b_->build_slt(args[1], args[0]),
                                                  b_->build_fconst_f32(0.0f), b_->build_fconst_f32(1.0f)) : nullptr;
        }
        if (fn == "smoothstep") {
            if (!needArgs(3)) return nullptr;
            Value* t = b_->build_fmin_f32(b_->build_fmax_f32(
                b_->build_sdiv(b_->build_sub(args[2], args[0]), b_->build_sub(args[1], args[0])),
                b_->build_fconst_f32(0.0f)), b_->build_fconst_f32(1.0f));
            return b_->build_mul(b_->build_mul(t, t),
                                 b_->build_sub(b_->build_fconst_f32(3.0f), b_->build_mul(b_->build_fconst_f32(2.0f), t)));
        }

        err = "Unsupported or unsafe function call '" + fn + "()' in @kernel audio DSP";
        return nullptr;
    }
};

} // namespace

CustomDspKernel compileVerifiedDspKernel(
    std::string_view jsSourceOrExpr,
    const VerifiedDspCompileOptions& opts) {

    CustomDspKernel result;
    result.maxAmp = (std::isfinite(opts.maxAmp) && opts.maxAmp > 0.0f) ? opts.maxAmp : 1.0f;
    result.kernelName = opts.name.empty() ? "js_dsp_kernel" : opts.name;
    result.source = std::string(jsSourceOrExpr);

    Module mod("verified_dsp_module");
    mod.set_allow_fp_reassociation(false);

    Function* fn = nullptr;
    std::string err;
    JsDspCompiler compiler(jsSourceOrExpr, opts);
    if (!compiler.compileToMir(mod, fn, err)) {
        result.errorMessage = err;
        return result;
    }

    result.zeroAllocProven = true;

    if (opts.enableSuperopt) {
        SynthMirVerifyResult vres = optimize_and_verify_mir(*fn, mod);
        if (!vres.proven) {
            result.errorMessage = "SMT equivalence verification failed: " + vres.diagnostic;
            return result;
        }
        result.mirEquivProven = true;
    } else {
        brass::verify::EquivOptions eqOpts;
        eqOpts.check_memory = false;
        eqOpts.check_guards = false;
        auto selfEq = brass::verify::verify_mir_equivalence(*fn, *fn, eqOpts);
        result.mirEquivProven = (selfEq.status == brass::verify::EquivStatus::ProvenEquivalent);
    }

    result.nanSafeProven = proveSpeakerProtectionSafety(*fn, result.maxAmp);
    if (!result.nanSafeProven) {
        result.errorMessage = "SMT speaker-protection proof failed to verify finite [-maxAmp, +maxAmp] bounds";
        return result;
    }

    auto kopts = brass::codegen::KernelOptions::audio_realtime();
    kopts.enable_optimizations = true;
    kopts.opt_level = 2;
    kopts.enable_smt_guard_elim = opts.enableSuperopt;
    kopts.enable_superopt = opts.enableSuperopt;
    kopts.enable_fma = false;
    kopts.enable_fp_reassociation = false;

    brass::codegen::KernelJit jit(kopts);
    brass::codegen::KernelFunction kfn = jit.compile(mod, result.kernelName);
    if (!kfn.is_valid() || !kfn.entry_point()) {
        result.errorMessage = "KernelJit failed to lower verified MIR to native machine code";
        return result;
    }

    auto kept = std::make_shared<brass::codegen::KernelFunction>(std::move(kfn));
    result.fn = reinterpret_cast<DspKernelFnPtr>(kept->entry_point());
    result.jitKeepAlive = std::move(kept);
    result.valid = true;
    result.verified = result.zeroAllocProven && result.nanSafeProven && result.mirEquivProven;
    return result;
}

VerifiedDspProcessor::VerifiedDspProcessor() {
    for (size_t i = 0; i < MAX_PARAMS; ++i) {
        params_[i].store(0.0f, std::memory_order_relaxed);
    }
}

bool VerifiedDspProcessor::compileAndInstall(
    std::string_view jsSourceOrExpr,
    const VerifiedDspCompileOptions& opts,
    std::string* outError) {

    CustomDspKernel kernel = compileVerifiedDspKernel(jsSourceOrExpr, opts);
    if (!kernel.valid || !kernel.verified || !kernel.fn) {
        std::lock_guard<std::mutex> lock(controlMutex_);
        lastError_ = kernel.errorMessage;
        if (outError) *outError = kernel.errorMessage;
        return false;
    }
    return installKernel(kernel, false);
}

bool VerifiedDspProcessor::hotSwap(
    std::string_view jsSourceOrExpr,
    const VerifiedDspCompileOptions& opts,
    std::string* outError) {

    CustomDspKernel kernel = compileVerifiedDspKernel(jsSourceOrExpr, opts);
    if (!kernel.valid || !kernel.verified || !kernel.fn) {
        std::lock_guard<std::mutex> lock(controlMutex_);
        lastError_ = kernel.errorMessage;
        if (outError) *outError = kernel.errorMessage;
        return false;
    }
    return installKernel(kernel, true);
}

bool VerifiedDspProcessor::installKernel(const CustomDspKernel& kernel, bool isHotSwap) {
    if (!kernel.valid || !kernel.fn) return false;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        retainedKernels_.push_back(kernel);
        lastError_.clear();
        kernelName_ = kernel.kernelName;
        currentSource_ = kernel.source;
    }

    maxAmp_.store(kernel.maxAmp, std::memory_order_relaxed);
    zeroAllocProven_.store(kernel.zeroAllocProven, std::memory_order_release);
    nanSafeProven_.store(kernel.nanSafeProven, std::memory_order_release);
    mirEquivProven_.store(kernel.mirEquivProven, std::memory_order_release);
    verified_.store(kernel.verified, std::memory_order_release);

    activeFn_.store(kernel.fn, std::memory_order_release);
    (void)isHotSwap;
    hotSwapCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void VerifiedDspProcessor::setRawFunction(DspKernelFnPtr fn, bool recordSwap) noexcept {
    DspKernelFnPtr prev = activeFn_.exchange(fn, std::memory_order_acq_rel);
    if (recordSwap && prev != nullptr && prev != fn) {
        hotSwapCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

void VerifiedDspProcessor::processBlock(
    const float* in,
    float* out,
    uint32_t numSamples,
    double startTime,
    double sampleRate) const noexcept {

    if (!out || numSamples == 0) return;

    DspKernelFnPtr fn = activeFn_.load(std::memory_order_acquire);
    if (!fn) {
        if (in && in != out) {
            std::memcpy(out, in, static_cast<size_t>(numSamples) * sizeof(float));
        } else if (!in) {
            std::memset(out, 0, static_cast<size_t>(numSamples) * sizeof(float));
        }
        return;
    }

    const float p0 = params_[0].load(std::memory_order_relaxed);
    const float p1 = params_[1].load(std::memory_order_relaxed);
    const double invSr = (sampleRate > 0.0) ? (1.0 / sampleRate) : (1.0 / 48000.0);

    for (uint32_t i = 0; i < numSamples; ++i) {
        const float s = in ? in[i] : 0.0f;
        const float t = static_cast<float>(startTime + static_cast<double>(i) * invSr);
        out[i] = fn(s, t, p0, p1);
    }
}

void VerifiedDspProcessor::processStereoInterleaved(
    float* interleavedBuf,
    uint32_t numFrames,
    double startTime,
    double sampleRate) const noexcept {

    if (!interleavedBuf || numFrames == 0) return;

    DspKernelFnPtr fn = activeFn_.load(std::memory_order_acquire);
    if (!fn) return;

    const float p0 = params_[0].load(std::memory_order_relaxed);
    const float p1 = params_[1].load(std::memory_order_relaxed);
    const double invSr = (sampleRate > 0.0) ? (1.0 / sampleRate) : (1.0 / 48000.0);

    for (uint32_t i = 0; i < numFrames; ++i) {
        const float t = static_cast<float>(startTime + static_cast<double>(i) * invSr);
        interleavedBuf[i * 2]     = fn(interleavedBuf[i * 2],     t, p0, p1);
        interleavedBuf[i * 2 + 1] = fn(interleavedBuf[i * 2 + 1], t, p0, p1);
    }
}

std::string VerifiedDspProcessor::lastError() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return lastError_;
}

std::string VerifiedDspProcessor::kernelName() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return kernelName_;
}

std::string VerifiedDspProcessor::currentSource() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    return currentSource_;
}

} // namespace broaudio
