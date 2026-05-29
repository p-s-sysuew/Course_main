#pragma once 

#include "lexer.h"
#include "types.h"

#include <string>

class Parser
{
public:
    Statement parseStatement(const std::string& text) const;

private:
    std::string parseIdentifier(Lexer& lexer) const;
    TableName parseTableName(Lexer& lexer) const;
    Value parseLiteral(Lexer& lexer) const;
    Operand parseOperand(Lexer& lexer) const;
    CompareOp parseCompareOp(Lexer& lexer) const;

    Expr parseWhereExpression(Lexer& lexer) const;
    Expr parseOrExpression(Lexer& lexer) const;
    Expr parseAndExpression(Lexer& lexer) const;
    Expr parsePrimaryExpression(Lexer& lexer) const;
    Expr parsePredicate(Lexer& lexer) const;

    CreateTableCommand parseCreateTable(Lexer& lexer) const;
    InsertCommand parseInsert(Lexer& lexer) const;
    UpdateCommand parseUpdate(Lexer& lexer) const;
    DeleteCommand parseDelete(Lexer& lexer) const;
    SelectCommand parseSelect(Lexer& lexer) const;
    RegisterUserCommand parseRegister(Lexer& lexer) const;

    std::vector<std::string> parseIdentifierList(Lexer& lexer) const;
    std::vector<Value> parseLiteralRow(Lexer& lexer) const;
    SelectItem parseSelectItem(Lexer& lexer) const;
};
