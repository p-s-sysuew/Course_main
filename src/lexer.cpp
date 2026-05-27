#include "lexer.h"
#include "utils.h"

#include <cctype>
#include <stdexcept>

Lexer::Lexer(const std::string& text)
{
    std::size_t index = 0;

    while (index < text.size())
    {
        char ch = text[index];

        if (std::isspace(static_cast<unsigned char>(ch)) != 0)
        {
            ++index;
            continue;
        }

        if (ch == '"')
        {
            std::string value;
            bool escaped = false;
            bool closed = false;
            ++index;

            while (index < text.size())
            {
                char current = text[index];
                ++index;

                if (escaped)
                {
                    if (current == 'n') value.push_back('\n');
                    else if (current == 't') value.push_back('\t');
                    else if (current == 'r') value.push_back('\r');
                    else value.push_back(current);
                    escaped = false;
                    continue;
                }

                if (current == '\\')
                {
                    escaped = true;
                    continue;
                }

                if (current == '"')
                {
                    closed = true;
                    break;
                }

                value.push_back(current);
            }

            if (!closed)
            {
                throw std::runtime_error("строковый литерал не закрыт кавычкой");
            }

            tokens_.push_back(Token{TokenType::String, value});
            continue;
        }

        if (std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_')
        {
            std::size_t start = index;
            ++index;

            while (index < text.size())
            {
                char current = text[index];
                if (std::isalnum(static_cast<unsigned char>(current)) == 0 && current != '_')
                {
                    break;
                }
                ++index;
            }

            tokens_.push_back(Token{TokenType::Word, text.substr(start, index - start)});
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(ch)) != 0 || (ch == '-' && index + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[index + 1])) != 0))
        {
            std::size_t start = index;
            ++index;

            while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0)
            {
                ++index;
            }

            tokens_.push_back(Token{TokenType::Number, text.substr(start, index - start)});
            continue;
        }

        if (index + 1 < text.size())
        {
            std::string two = text.substr(index, 2);
            if (two == "==" || two == "!=" || two == "<=" || two == ">=")
            {
                tokens_.push_back(Token{TokenType::Symbol, two});
                index += 2;
                continue;
            }
        }

        tokens_.push_back(Token{TokenType::Symbol, std::string(1, ch)});
        ++index;
    }

    tokens_.push_back(Token{TokenType::End, ""});
}

const Token& Lexer::peek() const
{
    return tokens_.at(position_);
}

const Token& Lexer::peekNext() const
{
    if (position_ + 1 >= tokens_.size())
    {
        return tokens_.back();
    }
    return tokens_.at(position_ + 1);
}

Token Lexer::next()
{
    Token token = tokens_.at(position_);
    ++position_;
    return token;
}

bool Lexer::consumeIf(const std::string& text)
{
    if (toUpper(peek().text) == toUpper(text))
    {
        ++position_;
        return true;
    }
    return false;
}

void Lexer::expect(const std::string& text)
{
    if (!consumeIf(text))
    {
        throw std::runtime_error("ожидался токен '" + text + "', но получен '" + peek().text + "'");
    }
}

void Lexer::expectWord(const std::string& text)
{
    if (peek().type != TokenType::Word || toUpper(peek().text) != toUpper(text))
    {
        throw std::runtime_error("ожидалось ключевое слово '" + text + "', но получено '" + peek().text + "'");
    }
    ++position_;
}

bool Lexer::isEnd() const
{
    return peek().type == TokenType::End;
} 
