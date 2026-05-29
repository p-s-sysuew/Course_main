#include "lexer.h"
#include "utils.h"

#include <cctype>
#include <stdexcept>

// Конструктор лексера: разбивает входную строку на вектор токенов
Lexer::Lexer(const std::string& text)
{
    std::size_t index = 0; // Инициализация текущей позиции разбора строки

    // Цикл перебора всех символов исходного текста
    while (index < text.size())
    {
        char ch = text[index]; // Получение текущего символа для анализа

        // Пропуск всех видов пробельных символов
        if (std::isspace(static_cast<unsigned char>(ch)) != 0)
        {
            ++index; // Сдвиг указателя на следующий символ
            continue; // Переход на следующую итерацию главного цикла
        }

        // Обработка строкового литерала, заключенного в двойные кавычки
        if (ch == '"')
        {
            std::string value; // Буфер для хранения содержимого строки
            bool escaped = false; // Флаг экранирования следующего символа слэшем
            bool closed = false; // Флаг успешного закрытия строкового литерала
            ++index; // Пропуск открывающей кавычки

            // Цикл посимвольного чтения внутренностей строки
            while (index < text.size())
            {
                char current = text[index]; // Чтение текущего символа внутри строки
                ++index; // Продвижение индекса вперед

                // Обработка символа, если предыдущим был экранирующий слэш
                if (escaped)
                {
                    if (current == 'n') value.push_back('\n');      // Символ переноса строки
                    else if (current == 't') value.push_back('\t'); // Символ табуляции
                    else if (current == 'r') value.push_back('\r'); // Символ возврата каретки
                    else value.push_back(current);                  // Любой другой экранированный символ
                    escaped = false; // Сброс состояния экранирования
                    continue; // Переход к следующему символу строки
                }

                // Включение режима экранирования при обнаружении обратного слэша
                if (current == '\\')
                {
                    escaped = true; // Установка флага для обработки следующего символа
                    continue; // Переход к следующему символу строки
                }

                // Выход из цикла чтения при обнаружении закрывающей кавычки
                if (current == '"')
                {
                    closed = true; // Фиксация корректного закрытия строки
                    break; // Прерывание внутреннего цикла
                }

                value.push_back(current); // Добавление обычного символа в буфер строки
            }

            // Проверка на ошибку незакрытых кавычек в конце текста
            if (!closed)
            {
                throw std::runtime_error("строковый литерал не закрыт кавычкой"); // Генерация исключения
            }

            // Добавление сформированного строкового токена в коллекцию
            tokens_.push_back(Token{TokenType::String, value});
            continue; // Переход к началу главного цикла
        }

        // Выделение слов (идентификаторов, имен и ключевых слов)
        if (std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_')
        {
            std::size_t start = index; // Сохранение стартовой позиции слова
            ++index; // Переход к следующему символу

            // Накопление символов слова, пока они являются буквами, цифрами или '_'
            while (index < text.size())
            {
                char current = text[index]; // Чтение символа для проверки
                if (std::isalnum(static_cast<unsigned char>(current)) == 0 && current != '_')
                {
                    break; // Остановка, если встречен символ не из алфавита слова
                }
                ++index; // Продвижение индекса
            }

            // Создание токена типа Word из выделенной подстроки
            tokens_.push_back(Token{TokenType::Word, text.substr(start, index - start)});
            continue; // Переход к началу главного цикла
        }

        // Выделение числовых констант (включая отрицательные числа)
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0 || (ch == '-' && index + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[index + 1])) != 0))
        {
            std::size_t start = index; // Сохранение стартовой позиции числа
            ++index; // Переход к следующему символу

            // Чтение всех последующих цифр числа
            while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0)
            {
                ++index; // Продвижение по цепочке цифр
            }

            // Добавление числового токена в общий список
            tokens_.push_back(Token{TokenType::Number, text.substr(start, index - start)});
            continue; // Переход к началу главного цикла
        }

        // Разбор составных двухсимвольных операторов сравнения
        if (index + 1 < text.size())
        {
            std::string two = text.substr(index, 2); // Извлечение пары символов для проверки
            if (two == "==" || two == "!=" || two == "<=" || two == ">=")
            {
                tokens_.push_back(Token{TokenType::Symbol, two}); // Добавление токена оператора
                index += 2; // Перешагивание через два обработанных символа
                continue; // Переход к началу главного цикла
            }
        }

        // Обработка всех остальных одиночных символов (разделители, скобки, простые операторы)
        tokens_.push_back(Token{TokenType::Symbol, std::string(1, ch)});
        ++index; // Переход к следующему символу текста
    }

    // Запись маркера окончания потока токенов
    tokens_.push_back(Token{TokenType::End, ""});
}

// Просмотр текущего токена без изменения позиции парсера
const Token& Lexer::peek() const
{
    return tokens_.at(position_); // Возврат ссылки на токен на текущей позиции
}

// Заглядывание на один токен вперед
const Token& Lexer::peekNext() const
{
    // Защита от выхода за границы вектора токенов
    if (position_ + 1 >= tokens_.size())
    {
        return tokens_.back(); // Возврат последнего токена в случае переполнения
    }
    return tokens_.at(position_ + 1); // Возврат ссылки на следующий токен
}

// Извлечение текущего токена с продвижением указателя вперед
Token Lexer::next()
{
    Token token = tokens_.at(position_); // Сохранение текущего токена во временную переменную
    ++position_; // Сдвиг внутренней позиции на один шаг вперед
    return token; // Возврат извлеченного токена
}

// Проверка совпадения текста токена (без регистра) и его опциональное поглощение
bool Lexer::consumeIf(const std::string& text)
{
    // Сравнение текста текущего токена с ожидаемым в верхнем регистре
    if (toUpper(peek().text) == toUpper(text))
    {
        ++position_; // Шаг вперед, если токен подошел
        return true; // Подтверждение успешного поглощения токена
    }
    return false; // Возврат false, если токен не совпал
}

// Обязательное поглощение символа или оператора с генерацией ошибки при несовпадении
void Lexer::expect(const std::string& text)
{
    if (!consumeIf(text))
    {
        throw std::runtime_error("ожидался токен '" + text + "', но получен '" + peek().text + "'"); // Выброс исключения
    }
}

// Обязательное поглощение ключевого слова с проверкой его типа и текста
void Lexer::expectWord(const std::string& text)
{
    // Проверка, является ли токен словом и совпадает ли его текст без учета регистра
    if (peek().type != TokenType::Word || toUpper(peek().text) != toUpper(text))
    {
        throw std::runtime_error("ожидался ввод '" + text + "', но было введено '" + peek().text + "'"); // Выброс исключения
    }
    ++position_; // Продвижение позиции после валидации слова
}

// Проверка, достигнут ли маркер конца токенов
bool Lexer::isEnd() const
{
    return peek().type == TokenType::End; // Возврат истины, если тип текущего токена — End
}