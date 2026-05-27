#include "table.h"
#include "jsonhelper.h"
#include "storage.h"
#include "stringpool.h"
#include "utils.h"
#include "securefile.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <set>
#include <stdexcept>

// Создание пути схемы
std::filesystem::path Table::schemaPath() const
{
    return tablePath_ / "schema.pb";
}

// Создание пути rows
std::filesystem::path Table::rowsPath() const
{
    return tablePath_ / "rows.dat";
}

// Кладбище
std::filesystem::path Table::deletedOffsetsPath() const
{
    return tablePath_ / "deleted.pb";
}

// Папка с индексами
std::filesystem::path Table::indexDirectoryPath() const
{
    return tablePath_ / "indexes";
}

// Создание индекс-файла
std::filesystem::path Table::indexPath(std::size_t columnIndex) const
{
    return indexDirectoryPath() / (columns_[columnIndex].name + ".tree.pb");
}

// meta путь
std::filesystem::path Table::indexMetaPath(std::size_t columnIndex) const
{
    return indexDirectoryPath() / (columns_[columnIndex].name + ".tree.meta.pb");
}

// Путь к пулу строк
std::filesystem::path Table::stringPoolPath() const
{
    return tablePath_ / "stringpool.pb";
}

// Создание таблицы и файлов
void Table::create(const std::filesystem::path& databasePath, const std::string& tableName, const std::vector<ColumnInfo>& columns)
{
    if (!isValidIdentifier(tableName))
    {
        throw std::runtime_error("некорректное имя таблицы: " + tableName);
    }

    if (columns.empty())
    {
        throw std::runtime_error("таблица должна содержать хотя бы один столбец");
    }

    std::filesystem::path tablePath = databasePath / tableName;
    if (std::filesystem::exists(tablePath))
    {
        throw std::runtime_error("таблица уже существует: " + tableName);
    }

    ensureDirectoryExists(tablePath);

    std::set<std::string> names;
    for (std::size_t index = 0; index < columns.size(); ++index)
    {
        const ColumnInfo& column = columns[index];

        if (!isValidIdentifier(column.name))
        {
            throw std::runtime_error("некорректное имя столбца: " + column.name);
        }

        if (!names.insert(column.name).second)
        {
            throw std::runtime_error("повтор имени столбца: " + column.name);
        }

        if (column.indexed && !column.notNull)
        {
            throw std::runtime_error("INDEXED-столбец автоматически должен быть NOT_NULL: " + column.name);
        }

        if (column.hasDefault && column.defaultValue.type != ValueType::Null && !valueHasColumnType(column.defaultValue, column.type))
        {
            throw std::runtime_error("DEFAULT имеет неверный тип для столбца " + column.name);
        }

    }

    std::vector<std::string> schemaRecords;
    schemaRecords.push_back(schemaToProtoBytes(columns));
    writeSecureRecords(tablePath / "schema.pb", schemaRecords);

    createEmptySecureFile(tablePath / "rows.dat");
    createEmptySecureFile(tablePath / "deleted.pb");
    createEmptySecureFile(tablePath / "stringpool.pb");
    ensureDirectoryExists(tablePath / "indexes");

    for (std::size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        if (columns[columnIndex].indexed)
        {
            DiskBStarIndex emptyIndex(
                (tablePath / "indexes") / (columns[columnIndex].name + ".tree.pb"),
                (tablePath / "indexes") / (columns[columnIndex].name + ".tree.meta.pb"),
                columns[columnIndex].type
            );
            emptyIndex.clear();
        }
    }
}

// Удаление таблицы
void Table::drop(const std::filesystem::path& databasePath, const std::string& tableName)
{
    std::filesystem::path tablePath = databasePath / tableName;

    if (!std::filesystem::exists(tablePath))
    {
        throw std::runtime_error("таблица не существует: " + tableName);
    }

    std::filesystem::remove_all(tablePath);
}


// Открывает таблицу, читает схему, строит индексы 
Table::Table(const std::filesystem::path& databasePath, const std::string& tableName)
    : databasePath_(databasePath), tablePath_(databasePath / tableName), tableName_(tableName)
{
    if (!std::filesystem::exists(tablePath_))
    {
        throw std::runtime_error("таблица не существует: " + tableName_);
    }

    loadSchema();
    validateSchema();
    buildIndexes();
}

// Читает столбцы из схемы
void Table::loadSchema()
{
    columns_.clear();

    std::vector<std::string> records = readSecureRecords(schemaPath());
    if (records.empty())
    {
        throw std::runtime_error("не удалось открыть schema.pb для таблицы " + tableName_);
    }

    columns_ = schemaFromProtoBytes(records[0]);
}



// Валидация схемы
void Table::validateSchema() const
{
    if (columns_.empty())
    {
        throw std::runtime_error("у таблицы пустая схема: " + tableName_);
    }

    std::set<std::string> names;

    for (std::size_t index = 0; index < columns_.size(); ++index)
    {
        const ColumnInfo& column = columns_[index];

        if (!isValidIdentifier(column.name))
        {
            throw std::runtime_error("некорректное имя столбца в схеме: " + column.name);
        }

        if (!names.insert(column.name).second)
        {
            throw std::runtime_error("повтор имени столбца в схеме: " + column.name);
        }

        if (column.indexed && !column.notNull)
        {
            throw std::runtime_error("INDEXED-столбец должен быть NOT_NULL: " + column.name);
        }
    }
}

// Создание индексов
void Table::buildIndexes()
{
    indexes_.clear();
    indexes_.resize(columns_.size());

    bool allIndexFilesExist = true;
    for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
    {
        if (columns_[columnIndex].indexed)
        {
            if (!secureFileExists(indexPath(columnIndex)) || !secureFileExists(indexMetaPath(columnIndex)))
            {
                allIndexFilesExist = false;
            }
        }
    }

    for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
    {
        if (!columns_[columnIndex].indexed)
        {
            continue;
        }

        indexes_[columnIndex] = std::unique_ptr<IndexBase>(new DiskBStarIndex(
            indexPath(columnIndex),
            indexMetaPath(columnIndex),
            columns_[columnIndex].type
        ));
    }

    if (!allIndexFilesExist)
    {
        persistIndexesFromRows();
    }
}

// Запись структуры дерева
void Table::persistIndexesFromRows()
{
    ensureDirectoryExists(indexDirectoryPath());

    std::vector<std::unique_ptr<IndexBase> > rebuiltIndexes;
    rebuiltIndexes.resize(columns_.size());

    for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
    {
        if (!columns_[columnIndex].indexed)
        {
            continue;
        }

        rebuiltIndexes[columnIndex] = std::unique_ptr<IndexBase>(new DiskBStarIndex(
            indexPath(columnIndex),
            indexMetaPath(columnIndex),
            columns_[columnIndex].type
        ));
        rebuiltIndexes[columnIndex]->clear();
    }

    std::set<std::streamoff> deletedOffsets = loadDeletedOffsets();
    std::vector<std::pair<std::streamoff, std::string> > rawRows = readSecureRecordsWithOffsets(rowsPath());

    for (std::size_t rowIndex = 0; rowIndex < rawRows.size(); ++rowIndex)
    {
        std::streamoff offset = rawRows[rowIndex].first;
        if (rowOffsetIsDeleted(offset, deletedOffsets))
        {
            continue;
        }

        Row row = rowFromProtoBytes(rawRows[rowIndex].second, columns_);

        for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
        {
            if (!columns_[columnIndex].indexed)
            {
                continue;
            }

            if (row[columnIndex].type == ValueType::Null)
            {
                throw std::runtime_error("INDEXED-столбец содержит NULL: " + columns_[columnIndex].name);
            }

            if (!rebuiltIndexes[columnIndex]->insert(row[columnIndex], offset))
            {
                throw std::runtime_error("повтор значения INDEXED-столбца: " + columns_[columnIndex].name);
            }
        }
    }

    indexes_ = std::move(rebuiltIndexes);
}


// [OLD] Сохранение деревьев
void Table::saveIndexesToFiles() const
{
}

// [OLD] Загрузка B*-индекса из файла
void Table::loadIndexesFromFiles()
{
}

// [OLD] Линейный хелпер
void Table::writeIndexEntry(std::vector<std::string>& lines, const Value& key, std::streamoff offset) const
{
    lines.push_back(indexEntryToProtoBytes(key, offset));
}

// [OLD] Чтение одной ProtoIndexEntry
void Table::readIndexEntry(const std::string& line, ColumnType type, Value& key, std::streamoff& offset) const
{
    indexEntryFromProtoBytes(line, type, key, offset);
}


// Явное обращение к файлу B*-дерева
void Table::touchIndexFile(std::size_t columnIndex) const
{
    std::vector<std::string> ignored = readSecureRecords(indexMetaPath(columnIndex));
    (void)ignored;
}

// Нахождение индекса столбца по его имени
std::size_t Table::findColumnIndex(const std::string& name) const
{
    for (std::size_t index = 0; index < columns_.size(); ++index)
    {
        if (columns_[index].name == name)
        {
            return index;
        }
    }

    throw std::runtime_error("неизвестный столбец '" + name + "' в таблице " + tableName_);
}

// Проверка notnull и совпадения типа в столбце
void Table::validateValueForColumn(const Value& value, const ColumnInfo& column) const
{
    if (value.type == ValueType::Null)
    {
        if (column.notNull)
        {
            throw std::runtime_error("столбец '" + column.name + "' не может быть NULL");
        }
        return;
    }

    if (!valueHasColumnType(value, column.type))
    {
        throw std::runtime_error("неверный тип значения для столбца '" + column.name + "'");
    }
}

// Валидация ряда
void Table::validateRow(const Row& row) const
{
    if (row.size() != columns_.size())
    {
        throw std::runtime_error("внутренняя ошибка: размер строки не совпадает со схемой");
    }

    for (std::size_t index = 0; index < columns_.size(); ++index)
    {
        validateValueForColumn(row[index], columns_[index]);
    }
}

// Существует ли операнд-колонка
void Table::validateOperand(const Operand& operand) const
{
    if (operand.isColumn)
    {
        findColumnIndex(operand.columnName);
    }
}

// Тип операнда
ColumnType Table::operandType(const Operand& operand) const
{
    if (operand.isColumn)
    {
        return columns_[findColumnIndex(operand.columnName)].type;
    }

    if (operand.literalValue.type == ValueType::Int)
    {
        return ColumnType::Int;
    }

    if (operand.literalValue.type == ValueType::String)
    {
        return ColumnType::String;
    }

    return ColumnType::String;
}

// Валидация условия
void Table::validateCondition(const Expr& expr) const
{
    if (expr.kind == Expr::Kind::And || expr.kind == Expr::Kind::Or)
    {
        validateCondition(*expr.first);
        validateCondition(*expr.second);
        return;
    }

    if (expr.kind == Expr::Kind::Compare)
    {
        validateOperand(expr.left);
        validateOperand(expr.right);

        if (expr.left.isColumn && expr.right.isColumn)
        {
            ColumnType leftType = operandType(expr.left);
            ColumnType rightType = operandType(expr.right);
            if (leftType != rightType)
            {
                throw std::runtime_error("в WHERE сравниваются столбцы разных типов");
            }
        }
        else if (expr.left.isColumn || expr.right.isColumn)
        {
            Operand columnOperand = expr.left.isColumn ? expr.left : expr.right;
            Operand literalOperand = expr.left.isColumn ? expr.right : expr.left;
            if (literalOperand.literalValue.type != ValueType::Null && !valueHasColumnType(literalOperand.literalValue, columns_[findColumnIndex(columnOperand.columnName)].type))
            {
                throw std::runtime_error("в WHERE константа имеет неверный тип");
            }
        }
        return;
    }

    if (expr.kind == Expr::Kind::Between)
    {
        validateOperand(expr.left);
        validateOperand(expr.low);
        validateOperand(expr.high);
        return;
    }

    if (expr.kind == Expr::Kind::Like)
    {
        validateOperand(expr.left);
        validateOperand(expr.pattern);
        return;
    }
}


// Чтение всех tombstone'ов
std::vector<Table::FreeSlot> Table::loadFreeSlots() const
{
    std::vector<FreeSlot> slots;
    std::vector<std::string> records = readSecureRecords(deletedOffsetsPath());

    for (std::size_t index = 0; index < records.size(); ++index)
    {
        ProtoDeletedOffset proto;
        if (!proto.ParseFromString(records[index]))
        {
            throw std::runtime_error("не удалось прочитать free-list запись из deleted.pb");
        }

        FreeSlot slot;
        slot.offset = static_cast<std::streamoff>(proto.offset());
        slot.size = static_cast<std::streamoff>(proto.size());

        slots.push_back(slot);
    }

    return slots;
}

// Перезапись deleted.pb
void Table::saveFreeSlots(const std::vector<FreeSlot>& slots) const
{
    std::vector<std::string> records;

    for (std::size_t index = 0; index < slots.size(); ++index)
    {
        ProtoDeletedOffset proto;
        proto.set_offset(static_cast<long long>(slots[index].offset));
        proto.set_size(static_cast<long long>(slots[index].size));

        std::string bytes;
        if (!proto.SerializeToString(&bytes))
        {
            throw std::runtime_error("не удалось сериализовать free-list запись deleted.pb");
        }

        records.push_back(bytes);
    }

    writeSecureRecords(deletedOffsetsPath(), records);
}

// Получение offset'ов tombstone'ов
std::set<std::streamoff> Table::loadDeletedOffsets() const
{
    std::set<std::streamoff> deletedOffsets;
    std::vector<FreeSlot> slots = loadFreeSlots();

    for (std::size_t index = 0; index < slots.size(); ++index)
    {
        deletedOffsets.insert(slots[index].offset);
    }

    return deletedOffsets;
}

// Новый tombstone
void Table::appendDeletedOffset(std::streamoff offset, std::streamoff size)
{
    std::vector<FreeSlot> slots = loadFreeSlots();

    for (std::size_t index = 0; index < slots.size(); ++index)
    {
        if (slots[index].offset == offset)
        {
            return;
        }
    }

    FreeSlot slot;
    slot.offset = offset;
    slot.size = size;
    slots.push_back(slot);
    saveFreeSlots(slots);
}

// Offset относится к tombstone'ам?
bool Table::rowOffsetIsDeleted(std::streamoff offset, const std::set<std::streamoff>& deletedOffsets) const
{
    return deletedOffsets.find(offset) != deletedOffsets.end();
}

// Использование tombstone
bool Table::tryWriteRowToFreeSlot(const std::string& rowBytes, std::streamoff& offset)
{
    std::vector<FreeSlot> slots = loadFreeSlots();

    for (std::size_t index = 0; index < slots.size(); ++index)
    {
        if (slots[index].size <= 0)
        {
            continue;
        }

        if (overwriteSecureRecordInSlot(rowsPath(), rowBytes, slots[index].offset, slots[index].size))
        {
            offset = slots[index].offset;
            slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(index));
            saveFreeSlots(slots);
            return true;
        }
    }

    return false;
}

// Физическая запись строки (tombstone or end?)
std::streamoff Table::writeRowToStorage(const Row& row)
{
    const std::string rowBytes = rowToProtoBytes(row);

    std::streamoff offset = 0;
    if (tryWriteRowToFreeSlot(rowBytes, offset))
    {
        return offset;
    }

    appendSecureRecord(rowsPath(), rowBytes, &offset);
    return offset;
}

// Чтение рядов и offset из файла rows
std::vector<Table::StoredRow> Table::loadAllRows() const
{
    std::vector<StoredRow> rows;
    std::vector<std::pair<std::streamoff, std::string> > lines = readSecureRecordsWithOffsets(rowsPath());
    const std::set<std::streamoff> deletedOffsets = loadDeletedOffsets();

    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        if (rowOffsetIsDeleted(lines[index].first, deletedOffsets))
        {
            continue;
        }

        StoredRow item;
        item.offset = lines[index].first;
        item.row = rowFromProtoBytes(lines[index].second, columns_);
        rows.push_back(item);
    }

    return rows;
}

// Чтение ряда по offset
Row Table::loadRowAtOffset(std::streamoff offset) const
{
    std::string rowBytes = readSecureRecordAtOffset(rowsPath(), offset);
    return rowFromProtoBytes(rowBytes, columns_);
}


// Добавление строки и обновление индексов
void Table::appendRow(const Row& row)
{
    validateRow(row);

    for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
    {
        if (!columns_[columnIndex].indexed)
        {
            continue;
        }

        if (row[columnIndex].type == ValueType::Null)
        {
            throw std::runtime_error("INDEXED-столбец не может быть NULL: " + columns_[columnIndex].name);
        }

        if (indexes_[columnIndex]->contains(row[columnIndex]))
        {
            throw std::runtime_error("повтор значения для INDEXED-столбца: " + columns_[columnIndex].name);
        }
    }

    std::streamoff offset = writeRowToStorage(row);

    for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
    {
        if (columns_[columnIndex].indexed)
        {
            indexes_[columnIndex]->insert(row[columnIndex], offset);
        }
    }

    saveIndexesToFiles();
}

// [OLD] Переписывание rows.dat и перестройка индексов
void Table::rewriteRows(const std::vector<Row>& rows)
{
    std::set<int> intValues;
    std::set<std::string> stringValues;

    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
    {
        validateRow(rows[rowIndex]);
    }

    for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
    {
        if (!columns_[columnIndex].indexed)
        {
            continue;
        }

        intValues.clear();
        stringValues.clear();

        for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
        {
            const Value& value = rows[rowIndex][columnIndex];

            if (value.type == ValueType::Null)
            {
                throw std::runtime_error("INDEXED-столбец не может быть NULL: " + columns_[columnIndex].name);
            }

            if (value.type == ValueType::Int)
            {
                if (!intValues.insert(value.intValue).second)
                {
                    throw std::runtime_error("повтор значения для INDEXED-столбца: " + columns_[columnIndex].name);
                }
            }
            else
            {
                if (!stringValues.insert(*value.stringValue).second)
                {
                    throw std::runtime_error("повтор значения для INDEXED-столбца: " + columns_[columnIndex].name);
                }
            }
        }
    }

    std::vector<std::string> lines;
    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
    {
        lines.push_back(rowToProtoBytes(rows[rowIndex]));
    }

    writeSecureRecords(rowsPath(), lines);

    persistIndexesFromRows();
    buildIndexes();
}


// Получение значения столбца или константы
Value Table::resolveOperand(const Row& row, const Operand& operand) const
{
    if (operand.isColumn)
    {
        return row[findColumnIndex(operand.columnName)];
    }

    return operand.literalValue;
}

// Сравнение значений по оператору
bool Table::compareOperands(const Value& left, const Value& right, CompareOp op) const
{
    if (left.type == ValueType::Null || right.type == ValueType::Null)
    {
        return false;
    }

    int cmp = compareValues(left, right);

    if (op == CompareOp::Eq) return cmp == 0;
    if (op == CompareOp::NotEq) return cmp != 0;
    if (op == CompareOp::Less) return cmp < 0;
    if (op == CompareOp::LessOrEq) return cmp <= 0;
    if (op == CompareOp::Greater) return cmp > 0;
    if (op == CompareOp::GreaterOrEq) return cmp >= 0;

    return false;
}

// Проверяет строку по where
bool Table::rowMatches(const Row& row, const Expr& expr) const
{
    if (expr.kind == Expr::Kind::And)
    {
        return rowMatches(row, *expr.first) && rowMatches(row, *expr.second);
    }

    if (expr.kind == Expr::Kind::Or)
    {
        return rowMatches(row, *expr.first) || rowMatches(row, *expr.second);
    }

    if (expr.kind == Expr::Kind::Compare)
    {
        Value left = resolveOperand(row, expr.left);
        Value right = resolveOperand(row, expr.right);
        return compareOperands(left, right, expr.compareOp);
    }

    if (expr.kind == Expr::Kind::Between)
    {
        Value value = resolveOperand(row, expr.left);
        Value low = resolveOperand(row, expr.low);
        Value high = resolveOperand(row, expr.high);

        if (value.type == ValueType::Null || low.type == ValueType::Null || high.type == ValueType::Null)
        {
            return false;
        }

        return compareValues(value, low) >= 0 && compareValues(value, high) < 0;
    }

    if (expr.kind == Expr::Kind::Like)
    {
        Value value = resolveOperand(row, expr.left);
        Value pattern = resolveOperand(row, expr.pattern);

        if (value.type != ValueType::String || pattern.type != ValueType::String)
        {
            return false;
        }

        return std::regex_match(*value.stringValue, std::regex(*pattern.stringValue));
    }

    return false;
}


// Переворот оператора
CompareOp Table::reverseCompareOp(CompareOp op) const
{
    if (op == CompareOp::Less) return CompareOp::Greater;
    if (op == CompareOp::LessOrEq) return CompareOp::GreaterOrEq;
    if (op == CompareOp::Greater) return CompareOp::Less;
    if (op == CompareOp::GreaterOrEq) return CompareOp::LessOrEq;
    return op;
}

// Можно ли использовать индексный доступ при сравнении
bool Table::tryUseIndexForCompare(const Expr& expr, std::vector<std::streamoff>& offsets) const
{
    if (expr.kind != Expr::Kind::Compare)
    {
        return false;
    }

    Operand columnOperand = expr.left;
    Operand literalOperand = expr.right;
    CompareOp op = expr.compareOp;

    if (!columnOperand.isColumn && literalOperand.isColumn)
    {
        columnOperand = expr.right;
        literalOperand = expr.left;
        op = reverseCompareOp(op);
    }

    if (!columnOperand.isColumn || literalOperand.isColumn)
    {
        return false;
    }

    std::size_t columnIndex = findColumnIndex(columnOperand.columnName);
    if (!columns_[columnIndex].indexed)
    {
        return false;
    }

    if (literalOperand.literalValue.type == ValueType::Null)
    {
        return false;
    }

    if (!valueHasColumnType(literalOperand.literalValue, columns_[columnIndex].type))
    {
        return false;
    }

    touchIndexFile(columnIndex);

    if (op == CompareOp::Eq)
    {
        std::optional<std::streamoff> found = indexes_[columnIndex]->find(literalOperand.literalValue);
        if (found.has_value())
        {
            offsets.push_back(found.value());
        }
        return true;
    }

    if (op == CompareOp::Less)
    {
        offsets = indexes_[columnIndex]->lessThan(literalOperand.literalValue, false);
        return true;
    }

    if (op == CompareOp::LessOrEq)
    {
        offsets = indexes_[columnIndex]->lessThan(literalOperand.literalValue, true);
        return true;
    }

    if (op == CompareOp::Greater)
    {
        offsets = indexes_[columnIndex]->greaterThan(literalOperand.literalValue, false);
        return true;
    }

    if (op == CompareOp::GreaterOrEq)
    {
        offsets = indexes_[columnIndex]->greaterThan(literalOperand.literalValue, true);
        return true;
    }

    return false;
}

// Можно ли использовать индексный доступ для between
bool Table::tryUseIndexForBetween(const Expr& expr, std::vector<std::streamoff>& offsets) const
{
    if (expr.kind != Expr::Kind::Between)
    {
        return false;
    }

    if (!expr.left.isColumn || expr.low.isColumn || expr.high.isColumn)
    {
        return false;
    }

    std::size_t columnIndex = findColumnIndex(expr.left.columnName);
    if (!columns_[columnIndex].indexed)
    {
        return false;
    }

    touchIndexFile(columnIndex);
    offsets = indexes_[columnIndex]->between(expr.low.literalValue, expr.high.literalValue);
    return true;
}

// Можно ли использовать индекс
bool Table::tryUseIndex(const Expr& expr, std::vector<std::streamoff>& offsets) const
{
    if (tryUseIndexForCompare(expr, offsets))
    {
        return true;
    }

    if (tryUseIndexForBetween(expr, offsets))
    {
        return true;
    }

    if (expr.kind == Expr::Kind::And)
    {
        if (tryUseIndex(*expr.first, offsets))
        {
            return true;
        }

        if (tryUseIndex(*expr.second, offsets))
        {
            return true;
        }
    }

    return false;
}

// Получение offset-кандидатов
std::set<std::streamoff> Table::indexedCandidateOffsets(const Expr& condition, bool& usedIndex) const
{
    std::set<std::streamoff> candidates;
    std::vector<std::streamoff> offsets;
    usedIndex = false;

    if (tryUseIndex(condition, offsets))
    {
        usedIndex = true;
        for (std::size_t index = 0; index < offsets.size(); ++index)
        {
            candidates.insert(offsets[index]);
        }
    }

    return candidates;
}

// Загрузка Row + Offset
std::vector<Table::StoredRow> Table::loadStoredRowsByOffsets(const std::vector<std::streamoff>& offsets) const
{
    std::vector<StoredRow> rows;
    for (std::size_t index = 0; index < offsets.size(); ++index)
    {
        StoredRow item;
        item.offset = offsets[index];
        item.row = loadRowAtOffset(offsets[index]);
        rows.push_back(item);
    }
    return rows;
}

// Загрузка строк, найденных индексом
std::vector<Row> Table::loadRowsByOffsets(const std::vector<std::streamoff>& offsets) const
{
    std::vector<Row> rows;

    for (std::size_t index = 0; index < offsets.size(); ++index)
    {
        rows.push_back(loadRowAtOffset(offsets[index]));
    }

    return rows;
}

// INSERT ряда в таблицу
std::size_t Table::insertRows(const std::vector<std::string>& columns, const std::vector<std::vector<Value> >& rows)
{
    if (columns.empty())
    {
        throw std::runtime_error("INSERT должен содержать хотя бы один столбец");
    }

    std::set<std::string> usedColumns;
    std::vector<std::size_t> targetIndexes;

    for (std::size_t index = 0; index < columns.size(); ++index)
    {
        if (!usedColumns.insert(columns[index]).second)
        {
            throw std::runtime_error("столбец указан дважды в INSERT: " + columns[index]);
        }
        targetIndexes.push_back(findColumnIndex(columns[index]));
    }

    std::vector<Row> preparedRows;

    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
    {
        if (rows[rowIndex].size() != columns.size())
        {
            throw std::runtime_error("количество значений в INSERT не совпадает с количеством столбцов");
        }

        Row row(columns_.size(), makeNull());

        for (std::size_t valueIndex = 0; valueIndex < rows[rowIndex].size(); ++valueIndex)
        {
            row[targetIndexes[valueIndex]] = rows[rowIndex][valueIndex];
        }

        for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
        {
            if (row[columnIndex].type == ValueType::Null && columns_[columnIndex].hasDefault)
            {
                row[columnIndex] = columns_[columnIndex].defaultValue;
            }
        }

        validateRow(row);
        preparedRows.push_back(row);
    }

    for (std::size_t rowIndex = 0; rowIndex < preparedRows.size(); ++rowIndex)
    {
        appendRow(preparedRows[rowIndex]);
    }

    return preparedRows.size();
}

// UPDATE рядов по where
std::size_t Table::updateRows(const std::vector<UpdateAssignment>& assignments, const Expr& condition)
{
    validateCondition(condition);

    for (std::size_t assignmentIndex = 0; assignmentIndex < assignments.size(); ++assignmentIndex)
    {
        std::size_t columnIndex = findColumnIndex(assignments[assignmentIndex].columnName);
        validateValueForColumn(assignments[assignmentIndex].value, columns_[columnIndex]);
    }

    bool usedIndex = false;
    std::set<std::streamoff> candidates = indexedCandidateOffsets(condition, usedIndex);
    std::vector<StoredRow> stored = loadAllRows();

    struct PendingUpdate
    {
        std::streamoff oldOffset = 0;
        Row oldRow;
        Row newRow;
    };

    std::vector<PendingUpdate> pending;

    for (std::size_t rowIndex = 0; rowIndex < stored.size(); ++rowIndex)
    {
        bool mayMatch = true;
        if (usedIndex)
        {
            mayMatch = candidates.find(stored[rowIndex].offset) != candidates.end();
        }

        if (mayMatch && rowMatches(stored[rowIndex].row, condition))
        {
            Row newRow = stored[rowIndex].row;
            for (std::size_t assignmentIndex = 0; assignmentIndex < assignments.size(); ++assignmentIndex)
            {
                std::size_t columnIndex = findColumnIndex(assignments[assignmentIndex].columnName);
                newRow[columnIndex] = assignments[assignmentIndex].value;
            }

            validateRow(newRow);

            PendingUpdate item;
            item.oldOffset = stored[rowIndex].offset;
            item.oldRow = stored[rowIndex].row;
            item.newRow = newRow;
            pending.push_back(item);
        }
    }

    for (std::size_t updateIndex = 0; updateIndex < pending.size(); ++updateIndex)
    {
        for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
        {
            if (!columns_[columnIndex].indexed)
            {
                continue;
            }

            const Value& oldValue = pending[updateIndex].oldRow[columnIndex];
            const Value& newValue = pending[updateIndex].newRow[columnIndex];

            bool changed = false;
            if (oldValue.type != newValue.type)
            {
                changed = true;
            }
            else if (oldValue.type != ValueType::Null)
            {
                changed = compareValues(oldValue, newValue) != 0;
            }

            if (changed && indexes_[columnIndex]->contains(newValue))
            {
                throw std::runtime_error("UPDATE нарушает уникальность INDEXED-столбца: " + columns_[columnIndex].name);
            }
        }
    }

    for (std::size_t updateIndex = 0; updateIndex < pending.size(); ++updateIndex)
    {
        std::streamoff newOffset = writeRowToStorage(pending[updateIndex].newRow);
        std::streamoff oldSize = secureRecordTotalSizeAtOffset(rowsPath(), pending[updateIndex].oldOffset);
        appendDeletedOffset(pending[updateIndex].oldOffset, oldSize);

        for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
        {
            if (!columns_[columnIndex].indexed)
            {
                continue;
            }

            indexes_[columnIndex]->erase(pending[updateIndex].oldRow[columnIndex]);
            indexes_[columnIndex]->insert(pending[updateIndex].newRow[columnIndex], newOffset);
        }
    }

    if (!pending.empty())
    {
        saveIndexesToFiles();
    }

    return pending.size();
}

// DELETE рядов по where
std::size_t Table::deleteRows(const Expr& condition)
{
    validateCondition(condition);

    bool usedIndex = false;
    std::set<std::streamoff> candidates = indexedCandidateOffsets(condition, usedIndex);
    std::vector<StoredRow> stored = loadAllRows();
    std::size_t deleted = 0;

    for (std::size_t rowIndex = 0; rowIndex < stored.size(); ++rowIndex)
    {
        bool mayMatch = true;
        if (usedIndex)
        {
            mayMatch = candidates.find(stored[rowIndex].offset) != candidates.end();
        }

        if (mayMatch && rowMatches(stored[rowIndex].row, condition))
        {
            std::streamoff oldSize = secureRecordTotalSizeAtOffset(rowsPath(), stored[rowIndex].offset);
            appendDeletedOffset(stored[rowIndex].offset, oldSize);

            for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
            {
                if (columns_[columnIndex].indexed)
                {
                    indexes_[columnIndex]->erase(stored[rowIndex].row[columnIndex]);
                }
            }

            ++deleted;
        }
    }

    if (deleted > 0)
    {
        saveIndexesToFiles();
    }

    return deleted;
}

// Есть ли в SELECT агрегаты
bool Table::itemsContainAggregates(const std::vector<SelectItem>& items) const
{
    for (std::size_t index = 0; index < items.size(); ++index)
    {
        if (items[index].kind != SelectItem::Kind::Column)
        {
            return true;
        }
    }
    return false;
}

// Генерирует имя для агрегата
std::string Table::defaultAggregateName(const SelectItem& item) const
{
    if (item.alias.has_value())
    {
        return item.alias.value();
    }

    if (item.kind == SelectItem::Kind::Count)
    {
        if (item.countStar) return "COUNT(*)";
        return "COUNT(" + item.columnName + ")";
    }

    if (item.kind == SelectItem::Kind::Sum)
    {
        return "SUM(" + item.columnName + ")";
    }

    if (item.kind == SelectItem::Kind::Avg)
    {
        return "AVG(" + item.columnName + ")";
    }

    return item.columnName;
}

// Считает по агрегату и формирует json
std::string Table::makeAggregateJson(const std::vector<Row>& rows, const std::vector<SelectItem>& items) const
{
    std::map<std::string, Value> object;

    for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex)
    {
        const SelectItem& item = items[itemIndex];
        std::string name = defaultAggregateName(item);

        if (item.kind == SelectItem::Kind::Count)
        {
            if (item.countStar)
            {
                object[name] = makeInt(static_cast<int>(rows.size()));
            }
            else
            {
                std::size_t columnIndex = findColumnIndex(item.columnName);
                int count = 0;
                for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
                {
                    if (rows[rowIndex][columnIndex].type != ValueType::Null)
                    {
                        ++count;
                    }
                }
                object[name] = makeInt(count);
            }
        }
        else if (item.kind == SelectItem::Kind::Sum || item.kind == SelectItem::Kind::Avg)
        {
            std::size_t columnIndex = findColumnIndex(item.columnName);
            if (columns_[columnIndex].type != ColumnType::Int)
            {
                throw std::runtime_error("SUM и AVG поддерживаются только для INT-столбцов");
            }

            int sum = 0;
            int count = 0;
            for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
            {
                if (rows[rowIndex][columnIndex].type == ValueType::Int)
                {
                    sum += rows[rowIndex][columnIndex].intValue;
                    ++count;
                }
            }

            if (item.kind == SelectItem::Kind::Sum)
            {
                object[name] = makeInt(sum);
            }
            else
            {
                if (count == 0) object[name] = makeNull();
                else object[name] = makeInt(sum / count);
            }
        }
        else
        {
            throw std::runtime_error("нельзя смешивать обычные столбцы и агрегаты в этой простой реализации");
        }
    }

    std::vector<std::map<std::string, Value> > objects;
    objects.push_back(object);
    return objectsToJson(objects);
}

// Формирует обычный json
std::string Table::makeRegularJson(const std::vector<Row>& rows, bool selectAll, const std::vector<SelectItem>& items) const
{
    std::vector<std::map<std::string, Value> > objects;

    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
    {
        std::map<std::string, Value> object;

        if (selectAll)
        {
            for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
            {
                object[columns_[columnIndex].name] = rows[rowIndex][columnIndex];
            }
        }
        else
        {
            for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex)
            {
                const SelectItem& item = items[itemIndex];
                if (item.kind != SelectItem::Kind::Column)
                {
                    throw std::runtime_error("агрегаты нельзя выводить как обычные строки");
                }

                std::size_t columnIndex = findColumnIndex(item.columnName);
                std::string outputName = item.alias.has_value() ? item.alias.value() : item.columnName;
                object[outputName] = rows[rowIndex][columnIndex];
            }
        }

        objects.push_back(object);
    }

    return objectsToJson(objects);
}

// SELECT рядов с индексами/полным проходом
SelectResult Table::selectRows(bool selectAll, const std::vector<SelectItem>& items, const std::optional<Expr>& condition) const
{
    std::vector<Row> rows;
    bool usedIndex = false;

    if (!selectAll && items.empty())
    {
        throw std::runtime_error("SELECT должен содержать * или список столбцов");
    }

    if (!selectAll)
    {
        for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex)
        {
            if (items[itemIndex].kind != SelectItem::Kind::Count || !items[itemIndex].countStar)
            {
                if (!items[itemIndex].columnName.empty())
                {
                    findColumnIndex(items[itemIndex].columnName);
                }
            }
        }
    }

    if (condition.has_value())
    {
        validateCondition(condition.value());

        std::vector<std::streamoff> offsets;
        if (tryUseIndex(condition.value(), offsets))
        {
            usedIndex = true;
            rows = loadRowsByOffsets(offsets);

            std::vector<Row> filtered;
            for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
            {
                if (rowMatches(rows[rowIndex], condition.value()))
                {
                    filtered.push_back(rows[rowIndex]);
                }
            }
            rows = filtered;
        }
        else
        {
            std::vector<StoredRow> stored = loadAllRows();
            for (std::size_t rowIndex = 0; rowIndex < stored.size(); ++rowIndex)
            {
                if (rowMatches(stored[rowIndex].row, condition.value()))
                {
                    rows.push_back(stored[rowIndex].row);
                }
            }
        }
    }
    else
    {
        std::vector<StoredRow> stored = loadAllRows();
        for (std::size_t rowIndex = 0; rowIndex < stored.size(); ++rowIndex)
        {
            rows.push_back(stored[rowIndex].row);
        }
    }

    SelectResult result;
    result.rowCount = rows.size();
    result.usedIndex = usedIndex;

    if (!selectAll && itemsContainAggregates(items))
    {
        result.json = makeAggregateJson(rows, items);
    }
    else
    {
        result.json = makeRegularJson(rows, selectAll, items);
    }

    return result;
}
