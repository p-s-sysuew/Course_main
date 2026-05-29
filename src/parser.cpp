#include "parser.h"
#include "stringpool.h"
#include "utils.h"

#include <stdexcept>

// Преобразует исходный текст SQL-запроса в структуру команды
Statement Parser::parseStatement(const std::string& text) const
{
    std::string clean = trim(text); // Удаление начальных и конечных пробельных символов
    if (clean.empty())
    {
        throw std::runtime_error("Пустая команда"); // Ошибка при попытке распарсить пустую строку
    }

    if (!clean.empty() && clean.back() == ';')
    {
        clean.pop_back(); // Удаление финальной точки с запятой перед разбором
    }

    Lexer lexer(clean); // Инициализация лексера для разбиения текста на токены
    Token first = lexer.next(); // Извлечение первого (определяющего) токена команды

    if (first.type != TokenType::Word)
    {
        throw std::runtime_error("Команда должна начинаться с ключевого слова"); // Защита от некорректного старта запроса
    }

    std::string command = toUpper(first.text); // Приведение ключевого слова к верхнему регистру
    Statement statement; // Переменная для хранения результирующей структуры команды

    if (command == "CREATE")
    {
        
        Token second = lexer.next(); // Получение типа создаваемой сущности
        std::string next = toUpper(second.text); // Приведение типа сущности к верхнему регистру
        if (next == "DATABASE") statement = CreateDatabaseCommand{parseIdentifier(lexer)}; // Разбор команды создания БД
        else if (next == "TABLE") statement = parseCreateTable(lexer); // Разбор команды создания таблицы
        else throw std::runtime_error("Неизвестная команда CREATE " + second.text); // Ошибка: создание не поддерживается
    }
    else if (command == "DROP")
    {
        Token second = lexer.next(); // Получение типа удаляемой сущности
        std::string next = toUpper(second.text); // Приведение типа сущности к верхнему регистру
        if (next == "DATABASE") statement = DropDatabaseCommand{parseIdentifier(lexer)}; // Разбор команды удаления БД
        else if (next == "TABLE") statement = DropTableCommand{parseTableName(lexer)}; // Разбор команды удаления таблицы
        else throw std::runtime_error("Неизвестная команда DROP " + second.text); // Ошибка: удаление не поддерживается
    }
    else if (command == "USE")
    {
        statement = UseDatabaseCommand{parseIdentifier(lexer)}; // Разбор команды переключения контекста БД
    }
    else if (command == "INSERT")
    {
        statement = parseInsert(lexer); // Вызов метода для разбора вставки записей
    }
    else if (command == "UPDATE")
    {
        statement = parseUpdate(lexer); // Вызов метода для разбора обновления записей
    }
    else if (command == "DELETE")
    {
        statement = parseDelete(lexer); // Вызов метода для разбора удаления записей
    }
    else if (command == "SELECT")
    {
        statement = parseSelect(lexer); // Вызов метода для разбора выборки данных
    }
    else
    {
        throw std::runtime_error("Неизвестная команда: " + command); // Ошибка при передаче неподдерживаемого слова
    }

    if (!lexer.isEnd())
    {
        throw std::runtime_error("Лишний токен в конце команды: " + lexer.peek().text); // Ошибка наличия мусора после ';'
    }

    return statement; // Возврат сформированного объекта команды
}

// Валидация и извлечение имени сущности (идентификатора)
std::string Parser::parseIdentifier(Lexer& lexer) const
{
    Token token = lexer.next(); // Извлечение очередного токена из потока

    if (token.type != TokenType::Word)
    {
        throw std::runtime_error("Ожидался идентификатор, но получено '" + token.text + "'"); // Ошибка: токен не является словом
    }

    if (!isValidIdentifier(token.text))
    {
        throw std::runtime_error("Некорректный идентификатор: " + token.text); // Ошибка: имя нарушает правила синтаксиса
    }

    return token.text; // Возврат валидного имени сущности
}

// Разбор составного или простого имени таблицы (с опциональным указанием БД)
TableName Parser::parseTableName(Lexer& lexer) const
{
    TableName result; // Объект для сохранения структуры имени
    std::string first = parseIdentifier(lexer); // Считывание первого компонента имени

    if (lexer.consumeIf("."))
    {
        result.databaseName = first; // Первое слово было именем базы данных
        result.tableName = parseIdentifier(lexer); // Второе слово является именем таблицы
    }
    else
    {
        result.tableName = first; // Имя базы опущено, считываем только имя таблицы
    }

    return result; // Возврат составного имени
}

// Разбор константного литерала (число, строка или NULL)
Value Parser::parseLiteral(Lexer& lexer) const
{
    Token token = lexer.next(); // Считывание токена значения

    if (token.type == TokenType::Number)
    {
        return makeInt(std::stoi(token.text)); // Конвертация и возврат целочисленного значения
    }

    if (token.type == TokenType::String)
    {
        return makeString(token.text); // Возврат строкового значения
    }

    if (token.type == TokenType::Word && toUpper(token.text) == "NULL")
    {
        return makeNull(); // Возврат инициализированного пустого значения
    }

    throw std::runtime_error("Ожидалась константа, но получено '" + token.text + "'"); // Ошибка: передан неподходящий токен
}

// Определение типа операнда (колонка или константный литерал) для выражений фильтрации
Operand Parser::parseOperand(Lexer& lexer) const
{
    Token token = lexer.peek(); // Просмотр токена без извлечения из потока
    Operand result; // Создание объекта операнда

    if (token.type == TokenType::Number || token.type == TokenType::String || (token.type == TokenType::Word && toUpper(token.text) == "NULL"))
    {
        result.isColumn = false; // Флаг: операнд является константой
        result.literalValue = parseLiteral(lexer); // Извлечение и сохранение константы
        return result; // Возврат операнда-литерала
    }

    if (token.type == TokenType::Word)
    {
        result.isColumn = true; // Флаг: операнд является именем столбца
        result.columnName = parseIdentifier(lexer); // Разбор и сохранение имени колонки
        return result; // Возврат операнда-колонки
    }

    throw std::runtime_error("Ожидался операнд в WHERE, но получено '" + token.text + "'"); // Ошибка: неподдерживаемый тип операнда
}

// Преобразование текстового токена в перечисление операторов сравнения
CompareOp Parser::parseCompareOp(Lexer& lexer) const
{
    Token token = lexer.next(); // Чтение токена оператора

    if (token.text == "==") return CompareOp::Eq; // Соответствие оператору "равно"
    if (token.text == "!=") return CompareOp::NotEq; // Соответствие оператору "не равно"
    if (token.text == "<") return CompareOp::Less; // Соответствие оператору "меньше"
    if (token.text == "<=") return CompareOp::LessOrEq; // Соответствие оператору "меньше или равно"
    if (token.text == ">") return CompareOp::Greater; // Соответствие оператору "больше"
    if (token.text == ">=") return CompareOp::GreaterOrEq; // Соответствие оператору "больше или равно"

    throw std::runtime_error("Ожидался оператор сравнения, но получено '" + token.text + "'"); // Ошибка: недопустимый знак сравнения
}

// Точка входа для парсинга логических условий секции WHERE
Expr Parser::parseWhereExpression(Lexer& lexer) const
{
    return parseOrExpression(lexer); // Запуск разбора с наименьшего приоритета (логическое ИЛИ)
}

// Разбор цепочки условий, объединенных оператором OR
Expr Parser::parseOrExpression(Lexer& lexer) const
{
    Expr left = parseAndExpression(lexer); // Разбор левого операнда с более высоким приоритетом (И)

    while (lexer.peek().type == TokenType::Word && toUpper(lexer.peek().text) == "OR")
    {
        lexer.next(); // Извлечение токена "OR" из потока
        Expr right = parseAndExpression(lexer); // Рекурсивный разбор правого выражения

        Expr combined; // Создание узла дерева для объединения операций
        combined.kind = Expr::Kind::Or; // Установка типа операции ИЛИ
        combined.first = std::make_shared<Expr>(left); // Сохранение левой ветви дерева
        combined.second = std::make_shared<Expr>(right); // Сохранение правой ветви дерева
        left = combined; // Перенос объединенного дерева в левую часть для поддержки цепочки
    }

    return left; // Возврат корня логического поддерева OR
}

// Разбор цепочки условий, объединенных оператором AND
Expr Parser::parseAndExpression(Lexer& lexer) const
{
    Expr left = parsePrimaryExpression(lexer); // Разбор базового предиката или выражения в скобках

    while (lexer.peek().type == TokenType::Word && toUpper(lexer.peek().text) == "AND")
    {
        lexer.next(); // Извлечение токена "AND" из потока
        Expr right = parsePrimaryExpression(lexer); // Считывание следующего связанного выражения

        Expr combined; // Создание комбинированного узла дерева
        combined.kind = Expr::Kind::And; // Установка типа операции И
        combined.first = std::make_shared<Expr>(left); // Привязка левой части
        combined.second = std::make_shared<Expr>(right); // Привязка правой части
        left = combined; // Перенос узла для продолжения левоассоциативной цепочки
    }

    return left; // Возврат корня логического поддерева AND
}

// Обработка выражений высшего приоритета (условия в круглых скобках)
Expr Parser::parsePrimaryExpression(Lexer& lexer) const
{
    if (lexer.consumeIf("("))
    {
        Expr inside = parseWhereExpression(lexer); // Рекурсивный запуск разбора вложенного выражения
        lexer.expect(")"); // Проверка наличия обязательной закрывающей скобки
        return inside; // Возврат изолированного поддерева
    }

    return parsePredicate(lexer); // Если скобок нет, парсим стандартный одиночный предикат
}

// Разбор конкретного предиката сравнения, диапазона BETWEEN или шаблона LIKE
Expr Parser::parsePredicate(Lexer& lexer) const
{
    Operand left = parseOperand(lexer); // Считывание левой части выражения (переменная/константа)

    if (lexer.peek().type == TokenType::Word && toUpper(lexer.peek().text) == "BETWEEN")
    {
        lexer.next(); // Поглощение ключевого слова BETWEEN
        Expr expr; // Создание выражения диапазона
        expr.kind = Expr::Kind::Between; // Присвоение типа BETWEEN
        expr.left = left; // Привязка проверяемого операнда
        expr.low = parseOperand(lexer); // Парсинг нижней (включаемой) границы интервала
        lexer.expectWord("AND"); // Обязательный разделитель границ диапазона
        expr.high = parseOperand(lexer); // Парсинг верхней (исключаемой) границы интервала
        return expr; // Возврат сформированного условия диапазона
    }

    if (lexer.peek().type == TokenType::Word && toUpper(lexer.peek().text) == "LIKE")
    {
        lexer.next(); // Поглощение ключевого слова LIKE
        Expr expr; // Создание выражения для работы с регулярными выражениями
        expr.kind = Expr::Kind::Like; // Присвоение типа LIKE
        expr.left = left; // Запись проверяемой строки
        expr.pattern = parseOperand(lexer); // Парсинг шаблона или регулярного выражения
        return expr; // Возврат условия поиска по регулярному выражению
    }

    Expr expr; // Создание стандартного бинарного выражения сравнения
    expr.kind = Expr::Kind::Compare; // Установка типа бинарного сравнения
    expr.left = left; // Перенос левого операнда
    expr.compareOp = parseCompareOp(lexer); // Выделение знака сравнения
    expr.right = parseOperand(lexer); // Извлечение правого операнда
    return expr; // Возврат базового условия сравнения
}

// Разбор синтаксиса команды создания таблицы и спецификаций её столбцов
CreateTableCommand Parser::parseCreateTable(Lexer& lexer) const
{
    CreateTableCommand command; // Инициализация структуры команды
    command.tableName = parseIdentifier(lexer); // Извлечение имени создаваемой таблицы
    lexer.expect("("); // Проверка наличия открывающей скобки списка колонок

    while (true)
    {
        ColumnInfo column; // Создание структуры описания колонки
        column.name = parseIdentifier(lexer); // Разбор имени столбца
        column.type = parseColumnType(parseIdentifier(lexer)); // Парсинг типа данных (INT/STRING)

        while (lexer.peek().type == TokenType::Word)
        {
            std::string word = toUpper(lexer.peek().text); // Чтение модификаторов столбца

            if (word == "NOT_NULL")
            {
                lexer.next(); // Поглощение модификатора NOT_NULL
                column.notNull = true; // Установка запрета на хранение пустых значений
            }
            else if (word == "INDEXED")
            {
                lexer.next(); // Поглощение модификатора INDEXED
                column.indexed = true; // Активация автосоздания B*-tree индекса
                column.notNull = true; // Индексируемое поле неявно становится NOT_NULL
            }
            else if (word == "DEFAULT")
            {
                lexer.next(); // Поглощение ключевого слова DEFAULT
                column.hasDefault = true; // Фиксация наличия значения по умолчанию
                column.defaultValue = parseLiteral(lexer); // Парсинг константы по умолчанию
            }
            else
            {
                break; // Выход из цикла при обнаружении токена, не являющегося модификатором
            }
        }

        command.columns.push_back(column); // Регистрация описанной колонки в схеме команды

        if (lexer.consumeIf(")"))
        {
            break; // Завершение разбора таблицы, если встречена закрывающая скобка
        }

        lexer.expect(","); // Разделитель между описаниями разных столбцов
    }

    return command; // Возврат укомплектованной команды CREATE TABLE
}

// Считывание перечня идентификаторов, разделенных запятыми (например, списки полей)
std::vector<std::string> Parser::parseIdentifierList(Lexer& lexer) const
{
    std::vector<std::string> result; // Вектор для накопления имен
    result.push_back(parseIdentifier(lexer)); // Извлечение обязательного первого имени

    while (lexer.consumeIf(","))
    {
        result.push_back(parseIdentifier(lexer)); // Добавление последующих имен через запятую
    }

    return result; // Возврат сформированного списка
}

// Парсинг кортежа константных значений, обрамленного круглыми скобками (строка данных)
std::vector<Value> Parser::parseLiteralRow(Lexer& lexer) const
{
    std::vector<Value> row; // Буфер строки таблицы
    lexer.expect("("); // Ожидание начала кортежа
    row.push_back(parseLiteral(lexer)); // Извлечение первого значения

    while (lexer.consumeIf(","))
    {
        row.push_back(parseLiteral(lexer)); // Извлечение элементов строки через запятую
    }

    lexer.expect(")"); // Обязательное закрытие кортежа значений
    return row; // Возврат распарсенного ряда данных
}

// Синтаксический анализ команды вставки записей (INSERT INTO)
InsertCommand Parser::parseInsert(Lexer& lexer) const
{
    InsertCommand command; // Создание пустой структуры команды вставки
    lexer.expectWord("INTO"); // Проверка обязательного синтаксического токена INTO
    command.table = parseTableName(lexer); // Считывание целевой таблицы для вставки данных
    lexer.expect("("); // Ожидание открывающей скобки перечня полей таблицы
    command.columns = parseIdentifierList(lexer); // Чтение перечисляемых колонок
    lexer.expect(")"); // Проверка закрывающей скобки списка колонок

    if (lexer.peek().type != TokenType::Word)
    {
        throw std::runtime_error("В INSERT ожидалось VALUE или VALUES"); // Ошибка: отсутствует декларация значений
    }

    std::string word = toUpper(lexer.next().text); // Извлечение токена объявления блоков данных
    if (word != "VALUE" && word != "VALUES")
    {
        throw std::runtime_error("В INSERT ожидалось VALUE или VALUES"); // Защита от синтаксических ошибок в названии ключевого слова
    }

    command.rows.push_back(parseLiteralRow(lexer)); // Извлечение обязательного первого блока данных
    while (lexer.consumeIf(","))
    {
        command.rows.push_back(parseLiteralRow(lexer)); // Парсинг дополнительных строк при множественной вставке
    }

    return command; // Возврат укомплектованной команды INSERT
}

// Анализ команды модификации существующих записей (UPDATE)
UpdateCommand Parser::parseUpdate(Lexer& lexer) const
{
    UpdateCommand command; // Инициализация структуры команды обновления
    command.table = parseTableName(lexer); // Определение целевой таблицы
    lexer.expectWord("SET"); // Валидация обязательного ключевого слова SET

    while (true)
    {
        UpdateAssignment assignment; // Локальный объект присваивания
        assignment.columnName = parseIdentifier(lexer); // Чтение имени обновляемого поля
        lexer.expect("="); // Проверка знака присваивания
        assignment.value = parseLiteral(lexer); // Разбор нового устанавливаемого константного значения
        command.assignments.push_back(assignment); // Регистрация операции присваивания в списке

        if (!lexer.consumeIf(","))
        {
            break; // Прерывание цикла, если цепочка обновлений колонок завершена
        }
    }

    lexer.expectWord("WHERE"); // Каждая команда UPDATE обязана содержать условие фильтрации
    command.where = parseWhereExpression(lexer); // Анализ и построение дерева условий WHERE
    return command; // Возврат готовой команды модификации данных
}

// Анализ и разбор логики удаления строк (DELETE FROM)
DeleteCommand Parser::parseDelete(Lexer& lexer) const
{
    DeleteCommand command; // Инициализация структуры команды удаления
    lexer.expectWord("FROM"); // Проверка наличия обязательного слова FROM
    command.table = parseTableName(lexer); // Извлечение имени целевой таблицы
    lexer.expectWord("WHERE"); // Поля условий обязательны для выполнения DELETE
    command.where = parseWhereExpression(lexer); // Разбор предикатов фильтрации удаляемых строк
    return command; // Возврат заполненной структуры DELETE
}

// Разбор элемента выборки SELECT (конкретное поле или агрегатная функция с алиасом)
SelectItem Parser::parseSelectItem(Lexer& lexer) const
{
    SelectItem item; // Инициализация возвращаемого элемента выборки

    if (lexer.peek().type == TokenType::Word)
    {
        std::string word = toUpper(lexer.peek().text); // Чтение и нормализация токена

        if (word == "COUNT" || word == "SUM" || word == "AVG")
        {
            lexer.next(); // Извлечение имени агрегатной функции из потока
            if (word == "COUNT") item.kind = SelectItem::Kind::Count; // Выбор режима подсчета количества
            if (word == "SUM") item.kind = SelectItem::Kind::Sum; // Выбор режима подсчета суммы элементов
            if (word == "AVG") item.kind = SelectItem::Kind::Avg; // Выбор режима вычисления среднего арифметического

            lexer.expect("("); // Ожидание скобки аргумента агрегатора
            if (item.kind == SelectItem::Kind::Count && lexer.consumeIf("*"))
            {
                item.countStar = true; // Специфичный флаг для синтаксиса COUNT(*)
            }
            else
            {
                item.columnName = parseIdentifier(lexer); // Парсинг конкретной колонки-аргумента для функции
            }
            lexer.expect(")"); // Валидация закрывающей скобки функции
        }
        else
        {
            item.kind = SelectItem::Kind::Column; // Элемент является обычной колонкой, а не функцией
            item.columnName = parseIdentifier(lexer); // Чтение имени извлекаемого поля
        }
    }
    else
    {
        throw std::runtime_error("Ожидался столбец или агрегатная функция в SELECT"); // Исключение: передан недопустимый токен
    }

    if (lexer.peek().type == TokenType::Word && toUpper(lexer.peek().text) == "AS")
    {
        lexer.next(); // Поглощение опционального токена алиаса "AS"
        item.alias = parseIdentifier(lexer); // Считывание пользовательского псевдонима колонки
    }

    return item; // Возврат описания элемента проекции SELECT
}

// Построение структуры команды выборки данных (SELECT)
SelectCommand Parser::parseSelect(Lexer& lexer) const
{
    SelectCommand command; // Создание пустого объекта команды SELECT

    if (lexer.consumeIf("*"))
    {
        command.selectAll = true; // Включен флаг проекции всех доступных полей
    }
    else
    {
        bool hasParentheses = lexer.consumeIf("("); // Проверка наличия опциональной группировки полей в скобки
        command.items.push_back(parseSelectItem(lexer)); // Разбор обязательного первого элемента проекции

        while (lexer.consumeIf(","))
        {
            command.items.push_back(parseSelectItem(lexer)); // Добавление дополнительных полей и агрегатов из списка
        }

        if (hasParentheses)
        {
            lexer.expect(")"); // Контроль закрытия ранее обнаруженных скобок проекции
        }
    }

    lexer.expectWord("FROM"); // Проверка обязательного предложения FROM
    command.table = parseTableName(lexer); // Определение таблицы-источника данных

    if (lexer.peek().type == TokenType::Word && toUpper(lexer.peek().text) == "WHERE")
    {
        lexer.next(); // Перешагивание через токен "WHERE"
        command.where = parseWhereExpression(lexer); // Формирование и привязка дерева фильтрации строк
    }

    return command;
}