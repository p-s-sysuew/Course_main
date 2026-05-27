#pragma once

#include "diskbstarindex.h"
#include "types.h"
#include "course_storage.pb.h"

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct SelectResult
{
    std::string json;
    std::size_t rowCount = 0;
    bool usedIndex = false;
};

class Table
{
public:
    static void create(const std::filesystem::path& databasePath, const std::string& tableName, const std::vector<ColumnInfo>& columns);
    static void drop(const std::filesystem::path& databasePath, const std::string& tableName);

    Table(const std::filesystem::path& databasePath, const std::string& tableName);

    std::size_t insertRows(const std::vector<std::string>& columns, const std::vector<std::vector<Value> >& rows);
    std::size_t updateRows(const std::vector<UpdateAssignment>& assignments, const Expr& condition);
    std::size_t deleteRows(const Expr& condition);
    SelectResult selectRows(bool selectAll, const std::vector<SelectItem>& items, const std::optional<Expr>& condition) const;

private:
    struct StoredRow
    {
        std::streamoff offset = 0;
        Row row;
    };

    struct FreeSlot
    {
        std::streamoff offset = 0;
        std::streamoff size = 0;
    };

    std::filesystem::path databasePath_;
    std::filesystem::path tablePath_;
    std::string tableName_;
    std::vector<ColumnInfo> columns_;
    std::vector<std::unique_ptr<IndexBase> > indexes_;

    std::filesystem::path schemaPath() const;
    std::filesystem::path rowsPath() const;
    std::filesystem::path deletedOffsetsPath() const;
    std::filesystem::path indexDirectoryPath() const;
    std::filesystem::path indexPath(std::size_t columnIndex) const;
    std::filesystem::path indexMetaPath(std::size_t columnIndex) const;
    std::filesystem::path stringPoolPath() const;

    void loadSchema();
    void validateSchema() const;
    void buildIndexes();
    void persistIndexesFromRows();
    void saveIndexesToFiles() const;
    void loadIndexesFromFiles();
    void writeIndexEntry(std::vector<std::string>& lines, const Value& key, std::streamoff offset) const;
    void readIndexEntry(const std::string& line, ColumnType type, Value& key, std::streamoff& offset) const;
    void touchIndexFile(std::size_t columnIndex) const;
    std::set<std::streamoff> loadDeletedOffsets() const;
    std::vector<FreeSlot> loadFreeSlots() const;
    void saveFreeSlots(const std::vector<FreeSlot>& slots) const;
    void appendDeletedOffset(std::streamoff offset, std::streamoff size);
    bool rowOffsetIsDeleted(std::streamoff offset, const std::set<std::streamoff>& deletedOffsets) const;
    bool tryWriteRowToFreeSlot(const std::string& rowBytes, std::streamoff& offset);
    std::streamoff writeRowToStorage(const Row& row);
    std::size_t findColumnIndex(const std::string& name) const;

    void validateValueForColumn(const Value& value, const ColumnInfo& column) const;
    void validateRow(const Row& row) const;
    void validateCondition(const Expr& expr) const;
    void validateOperand(const Operand& operand) const;
    ColumnType operandType(const Operand& operand) const;

    std::vector<StoredRow> loadAllRows() const;
    Row loadRowAtOffset(std::streamoff offset) const;
    void appendRow(const Row& row);
    void rewriteRows(const std::vector<Row>& rows);

    Value resolveOperand(const Row& row, const Operand& operand) const;
    bool compareOperands(const Value& left, const Value& right, CompareOp op) const;
    bool rowMatches(const Row& row, const Expr& expr) const;

    bool tryUseIndex(const Expr& expr, std::vector<std::streamoff>& offsets) const;
    bool tryUseIndexForCompare(const Expr& expr, std::vector<std::streamoff>& offsets) const;
    bool tryUseIndexForBetween(const Expr& expr, std::vector<std::streamoff>& offsets) const;
    CompareOp reverseCompareOp(CompareOp op) const;

    std::vector<StoredRow> loadStoredRowsByOffsets(const std::vector<std::streamoff>& offsets) const;
    std::vector<Row> loadRowsByOffsets(const std::vector<std::streamoff>& offsets) const;
    std::set<std::streamoff> indexedCandidateOffsets(const Expr& condition, bool& usedIndex) const;
    std::string makeRegularJson(const std::vector<Row>& rows, bool selectAll, const std::vector<SelectItem>& items) const;
    std::string makeAggregateJson(const std::vector<Row>& rows, const std::vector<SelectItem>& items) const;
    bool itemsContainAggregates(const std::vector<SelectItem>& items) const;
    std::string defaultAggregateName(const SelectItem& item) const;
};
