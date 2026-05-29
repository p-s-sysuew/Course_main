#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Столбец таблицы
enum class ColumnType
{
    Int,
    String
};

// Тип значения в ячейке
enum class ValueType
{
    Null,
    Int,
    String
};

// The значение
struct Value
{
    ValueType type = ValueType::Null;
    int intValue = 0;
    std::shared_ptr<std::string> stringValue;
};

// The столбец
struct ColumnInfo
{
    std::string name;
    ColumnType type = ColumnType::Int;
    bool notNull = false;
    bool indexed = false;
    bool hasDefault = false;
    Value defaultValue;
};

// Имя таблицы
struct TableName
{
    std::optional<std::string> databaseName;
    std::string tableName;
};

// Операторы сравнения в WHERE
enum class CompareOp
{
    Eq,
    NotEq,
    Less,
    LessOrEq,
    Greater,
    GreaterOrEq
};

// Операнд WHERE
struct Operand
{
    bool isColumn = false;
    std::string columnName;
    Value literalValue;
};

// Структура для WHERE в виде дерева
struct Expr
{
    enum class Kind
    {
        Empty,
        Compare,
        Between,
        Like,
        And,
        Or
    };

    Kind kind = Kind::Empty;

    Operand left;
    Operand right;
    Operand low;
    Operand high;
    Operand pattern;
    CompareOp compareOp = CompareOp::Eq;

    std::shared_ptr<Expr> first;
    std::shared_ptr<Expr> second;
};

// Структура для SELECT
struct SelectItem
{
    enum class Kind
    {
        Column,
        Count,
        Sum,
        Avg
    };

    Kind kind = Kind::Column;
    std::string columnName;
    bool countStar = false;
    std::optional<std::string> alias;
};

// Команды
struct CreateDatabaseCommand { std::string databaseName; };
struct DropDatabaseCommand { std::string databaseName; };
struct UseDatabaseCommand { std::string databaseName; };
struct CreateTableCommand { std::string tableName; std::vector<ColumnInfo> columns; };
struct DropTableCommand { TableName table; };
struct InsertCommand { TableName table; std::vector<std::string> columns; std::vector<std::vector<Value> > rows; };
struct UpdateAssignment { std::string columnName; Value value; };
struct UpdateCommand { TableName table; std::vector<UpdateAssignment> assignments; Expr where; };
struct DeleteCommand { TableName table; Expr where; };
struct SelectCommand { TableName table; bool selectAll = false; std::vector<SelectItem> items; std::optional<Expr> where; };
struct RegisterUserCommand { std::string username; std::string password; std::string role; };

// Общий тип команды
using Statement = std::variant<
    CreateDatabaseCommand,
    DropDatabaseCommand,
    UseDatabaseCommand,
    CreateTableCommand,
    DropTableCommand,
    InsertCommand,
    UpdateCommand,
    DeleteCommand,
    SelectCommand,
    RegisterUserCommand
>;

using Row = std::vector<Value>;
