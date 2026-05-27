#pragma once

#include <string>
#include <vector>

enum class TokenType
{
    Word,
    Number,
    String,
    Symbol,
    End
};

struct Token
{
    TokenType type = TokenType::End;
    std::string text;
};
 
class Lexer
{
public:
    explicit Lexer(const std::string& text);

    const Token& peek() const;
    const Token& peekNext() const;
    Token next();

    bool consumeIf(const std::string& text);
    void expect(const std::string& text);
    void expectWord(const std::string& text);
    bool isEnd() const;

private:
    std::vector<Token> tokens_;
    std::size_t position_ = 0;
};
