#include "parser.h"
#include "stringpool.h"
#include "utils.h"

#include <stdexcept>

Statement Parser::parseStatement(const std::string& text) const
{
    std::string clean = trim(text);
    if (clean.empty())
    {
        throw std::runtime_error("пустая команда");
    }

    if (!clean.empty() && clean.back() == ';')
    {
        clean.pop_back();
    }

    Lexer lexer(clean);
    Token first = lexer.next();

    if (first.type != TokenType::Word)
    {
        throw std::runtime_error("команда должна начинаться с ключевого слова");
    }

    std::string command = toUpper(first.text);
    Statement statement;

    if (command == "CREATE")
    {
        Token second = lexer.next();
        std::string next = toUpper(second.text);
        if (next == "DATABASE") statement = CreateDatabaseCommand{parseIdentifier(lexer)};
        else if (next == "TABLE") statement = parseCreateTable(lexer);
        else throw std::runtime_error("неизвестная команда CREATE " + second.text);
    }
    else if (command == "DROP")
    {
        Token second = lexer.next();
        std::string next = toUpper(second.text);
        if (next == "DATABASE") statement = DropDatabaseCommand{parseIdentifier(lexer)};
        else if (next == "TABLE") statement = DropTableCommand{parseTableName(lexer)};
        else throw std::runtime_error("неизвестная команда DROP " + second.text);
    }
    else if (command == "USE")
    {
        statement = UseDatabaseCommand{parseIdentifier(lexer)};
    }
    else if (command == "INSERT")
    {
        statement = parseInsert(lexer);
    }
    else if (command == "UPDATE")
    {
        statement = parseUpdate(lexer);
    }
    else if (command == "DELETE")
    {
        statement = parseDelete(lexer);
    }
    else if (command == "SELECT")
    {
        statement = parseSelect(lexer);
    }
    else
    {
        throw std::runtime_error("неизвестная команда: " + command);
    }

    if (!lexer.isEnd())
    {
        throw std::runtime_error("лишний токен в конце команды: " + lexer.peek().text);
    }

    return statement;
}

std::string Parser::parseIdentifier(Lexer& lexer) const
{
    Token token = lexer.next();

    if (token.type != TokenType::Word)
    {
        throw std::runtime_error("ожидался идентификатор, но получено '" + token.text + "'");
    }

    if (!isValidIdentifier(token.text))
    {
        throw std::runtime_error("некорректный идентификатор: " + token.text);
    }

    return token.text;
}

TableName Parser::parseTableName(Lexer& lexer) const
{
    TableName result;
    std::string first = parseIdentifier(lexer);

    if (lexer.consumeIf("."))
    {
        result.databaseName = first;
        result.tableName = parseIdentifier(lexer);
    }
    else
    {
        result.tableName = first;
    }

    return result;
}

Value Parser::parseLiteral(Lexer& lexer) const
{
    Token token = lexer.next();

    if (token.type == TokenType::Number)
    {
        return makeInt(std::stoi(token.text));
    }

    if (token.type == TokenType::String)
    {
        return makeString(token.text);
    }

    if (token.type == TokenType::Word && toUpper(token.text) == "NULL")
    {
        return makeNull();
    }

    throw std::runtime_error("ожидалась константа, но получено '" + token.text + "'");
}

Operand Parser::parseOperand(Lexer& lexer) const
{
    Token token = lexer.peek();
    Operand result;

    if (token.type == TokenType::Number || token.type == TokenType::String || (token.type == TokenType::Word && toUpper(token.text) == "NULL"))
    {
        result.isColumn = false;
        result.literalValue = parseLiteral(lexer);
        return result;
    }

    if (token.type == TokenType::Word)
    {
        result.isColumn = true;
        result.columnName = parseIdentifier(lexer);
        return result;
    }

    throw std::runtime_error("ожидался операнд в WHERE, но получено '" + token.text + "'");
}

CompareOp Parser::parseCompareOp(Lexer& lexer) const
{
    Token token = lexer.next();

    if (token.text == "==") return CompareOp::Eq;
    if (token.text == "!=") return CompareOp::NotEq;
    if (token.text == "<") return CompareOp::Less;
    if (token.text == "<=") return CompareOp::LessOrEq;
    if (token.text == ">") return CompareOp::Greater;
    if (token.text == ">=") return CompareOp::GreaterOrEq;

    throw std::runtime_error("ожидался оператор сравнения, но получено '" + token.text + "'");
}

Expr Parser::parseWhereExpression(Lexer& lexer) const
{
    return parseOrExpression(lexer);
}

Expr Parser::parseOrExpression(Lexer& lexer) const
{
    Expr left = parseAndExpression(lexer);

    while (lexer.peek().type == TokenType::Word && toUpper(lexer.peek().text) == "OR")
    {
        lexer.next();
        Expr right = parseAndExpression(lexer);

        Expr combined;
        combined.kind = Expr::Kind::Or;
        combined.first = std::make_shared<Expr>(left);
        combined.second = std::make_shared<Expr>(right);
        left = combined;
    }

    return left;
}

Expr Parser::parseAndExpression(Lexer& lexer) const
{
    Expr left = parsePrimaryExpression(lexer);

    while (lexer.peek().type == TokenType::Word && toUpper(lexer.peek().text) == "AND")
    {
        lexer.next();
        Expr right = parsePrimaryExpression(lexer);

        Expr combined;
        combined.kind = Expr::Kind::And;
        combined.first = std::make_shared<Expr>(left);
        combined.second = std::make_shared<Expr>(right);
        left = combined;
    }

    return left;
}

Expr Parser::parsePrimaryExpression(Lexer& lexer) const
{
    if (lexer.consumeIf("("))
    {
        Expr inside = parseWhereExpression(lexer);
        lexer.expect(")");
        return inside;
    }

    return parsePredicate(lexer);
}

Expr Parser::parsePredicate(Lexer& lexer) const
{
    Operand left = parseOperand(lexer);

    if (lexer.peek().type == TokenType::Word && toUpper(lexer.peek().text) == "BETWEEN")
    {
        lexer.next();
        Expr expr;
        expr.kind = Expr::Kind::Between;
        expr.left = left;
        expr.low = parseOperand(lexer);
        lexer.expectWord("AND");
        expr.high = parseOperand(lexer);
        return expr;
    }

    if (lexer.peek().type == TokenType::Word && toUpper(lexer.peek().text) == "LIKE")
    {
        lexer.next();
        Expr expr;
        expr.kind = Expr::Kind::Like;
        expr.left = left;
        expr.pattern = parseOperand(lexer);
        return expr;
    }

    Expr expr;
    expr.kind = Expr::Kind::Compare;
    expr.left = left;
    expr.compareOp = parseCompareOp(lexer);
    expr.right = parseOperand(lexer);
    return expr;
}

CreateTableCommand Parser::parseCreateTable(Lexer& lexer) const
{
    CreateTableCommand command;
    command.tableName = parseIdentifier(lexer);
    lexer.expect("(");

    while (true)
    {
        ColumnInfo column;
        column.name = parseIdentifier(lexer);
        column.type = parseColumnType(parseIdentifier(lexer));

        while (lexer.peek().type == TokenType::Word)
        {
            std::string word = toUpper(lexer.peek().text);

            if (word == "NOT_NULL")
            {
                lexer.next();
                column.notNull = true;
            }
            else if (word == "INDEXED")
            {
                lexer.next();
                column.indexed = true;
                column.notNull = true;
            }
            else if (word == "DEFAULT")
            {
                lexer.next();
                column.hasDefault = true;
                column.defaultValue = parseLiteral(lexer);
            }
            else
            {
                break;
            }
        }

        command.columns.push_back(column);

        if (lexer.consumeIf(")"))
        {
            break;
        }

        lexer.expect(",");
    }

    return command;
}

std::vector<std::string> Parser::parseIdentifierList(Lexer& lexer) const
{
    std::vector<std::string> result;
    result.push_back(parseIdentifier(lexer));

    while (lexer.consumeIf(","))
    {
        result.push_back(parseIdentifier(lexer));
    }

    return result;
}

std::vector<Value> Parser::parseLiteralRow(Lexer& lexer) const
{
    std::vector<Value> row;
    lexer.expect("(");
    row.push_back(parseLiteral(lexer));

    while (lexer.consumeIf(","))
    {
        row.push_back(parseLiteral(lexer));
    }

    lexer.expect(")");
    return row;
}


InsertCommand Parser::parseInsert(Lexer& lexer) const
{
    InsertCommand command;
    lexer.expectWord("INTO");
    command.table = parseTableName(lexer);
    lexer.expect("(");
    command.columns = parseIdentifierList(lexer);
    lexer.expect(")");

    if (lexer.peek().type != TokenType::Word)
    {
        throw std::runtime_error("в INSERT ожидалось VALUE или VALUES");
    }

    std::string word = toUpper(lexer.next().text);
    if (word != "VALUE" && word != "VALUES")
    {
        throw std::runtime_error("в INSERT ожидалось VALUE или VALUES");
    }

    command.rows.push_back(parseLiteralRow(lexer));
    while (lexer.consumeIf(","))
    {
        command.rows.push_back(parseLiteralRow(lexer));
    }

    return command;
}

UpdateCommand Parser::parseUpdate(Lexer& lexer) const
{
    UpdateCommand command;
    command.table = parseTableName(lexer);
    lexer.expectWord("SET");

    while (true)
    {
        UpdateAssignment assignment;
        assignment.columnName = parseIdentifier(lexer);
        lexer.expect("=");
        assignment.value = parseLiteral(lexer);
        command.assignments.push_back(assignment);

        if (!lexer.consumeIf(","))
        {
            break;
        }
    }

    lexer.expectWord("WHERE");
    command.where = parseWhereExpression(lexer);
    return command;
}

DeleteCommand Parser::parseDelete(Lexer& lexer) const
{
    DeleteCommand command;
    lexer.expectWord("FROM");
    command.table = parseTableName(lexer);
    lexer.expectWord("WHERE");
    command.where = parseWhereExpression(lexer);
    return command;
}

SelectItem Parser::parseSelectItem(Lexer& lexer) const
{
    SelectItem item;

    if (lexer.peek().type == TokenType::Word)
    {
        std::string word = toUpper(lexer.peek().text);

        if (word == "COUNT" || word == "SUM" || word == "AVG")
        {
            lexer.next();
            if (word == "COUNT") item.kind = SelectItem::Kind::Count;
            if (word == "SUM") item.kind = SelectItem::Kind::Sum;
            if (word == "AVG") item.kind = SelectItem::Kind::Avg;

            lexer.expect("(");
            if (item.kind == SelectItem::Kind::Count && lexer.consumeIf("*"))
            {
                item.countStar = true;
            }
            else
            {
                item.columnName = parseIdentifier(lexer);
            }
            lexer.expect(")");
        }
        else
        {
            item.kind = SelectItem::Kind::Column;
            item.columnName = parseIdentifier(lexer);
        }
    }
    else
    {
        throw std::runtime_error("ожидался столбец или агрегатная функция в SELECT");
    }

    if (lexer.peek().type == TokenType::Word && toUpper(lexer.peek().text) == "AS")
    {
        lexer.next();
        item.alias = parseIdentifier(lexer);
    }

    return item;
}

SelectCommand Parser::parseSelect(Lexer& lexer) const
{
    SelectCommand command;

    if (lexer.consumeIf("*"))
    {
        command.selectAll = true;
    }
    else
    {
        bool hasParentheses = lexer.consumeIf("(");
        command.items.push_back(parseSelectItem(lexer));

        while (lexer.consumeIf(","))
        {
            command.items.push_back(parseSelectItem(lexer));
        }

        if (hasParentheses)
        {
            lexer.expect(")");
        }
    }

    lexer.expectWord("FROM");
    command.table = parseTableName(lexer);

    if (lexer.peek().type == TokenType::Word && toUpper(lexer.peek().text) == "WHERE")
    {
        lexer.next();
        command.where = parseWhereExpression(lexer);
    }

    return command;
}

