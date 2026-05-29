#pragma once

#include <string>
#include <vector>

// Типы токенов, которые может возвращать лексический анализатор
enum class TokenType
{
    Word,     // последовательность букв (идентификатор/ключевое слово)
    Number,   // числовая константа
    String,   // строковый литерал в кавычках
    Symbol,   // одиночный символ (оператор, пунктуация и т.д.)
    End       // конец входных данных
};

// Представление одного токена
struct Token
{
    TokenType type = TokenType::End;  // тип токена
    std::string text;                 // текст токена
};
 
class Lexer
{
public:
    // Создаёт лексер и производит токенизацию переданной строки
    explicit Lexer(const std::string& text);

    // Возвращает текущий токен, не сдвигая позицию
    const Token& peek() const;
    
    // Возвращает следующий токен, не сдвигая позицию
    const Token& peekNext() const;
    
    // Возвращает текущий токен и переходит к следующему
    Token next();

    // Проверяет, совпадает ли текущий токен с указанным текстом,
    // и если да — потребляет (съедает) его
    bool consumeIf(const std::string& text);
    
    // Ожидает конкретный текст, иначе выбрасывает ошибку
    void expect(const std::string& text);
    
    // Ожидает слово с указанным текстом
    void expectWord(const std::string& text);
    
    // Проверяет, достигнут ли конец токенов
    bool isEnd() const;

private:
    std::vector<Token> tokens_;      // все распарсенные токены
    std::size_t position_ = 0;       // текущая позиция в списке токенов
};