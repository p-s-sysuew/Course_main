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

// Формирование пути к файлу описания схемы таблицы
std::filesystem::path Table::schemaPath() const
{
    return tablePath_ / "schema.pb"; // Файл схемы в формате Protobuf
}

// Формирование пути к двоичному файлу с данными строк таблицы
std::filesystem::path Table::rowsPath() const
{
    return tablePath_ / "rows.dat"; // Файл непосредственного хранения записей
}

// Формирование пути к файлу смещений удаленных записей
std::filesystem::path Table::deletedOffsetsPath() const
{
    return tablePath_ / "deleted.pb"; // Хранилище адресов для повторного использования пространства
}

// Получение пути к директории хранения индексов таблицы
std::filesystem::path Table::indexDirectoryPath() const
{
    return tablePath_ / "indexes"; // Папка со всеми индексными деревьями
}

// Путь к конкретному файлу данных B*-дерева для указанного столбца
std::filesystem::path Table::indexPath(std::size_t columnIndex) const
{
    return indexDirectoryPath() / (columns_[columnIndex].name + ".tree.pb"); // Файл узлов дерева
}

// Путь к конфигурационному файлу метаданных индекса конкретного столбца
std::filesystem::path Table::indexMetaPath(std::size_t columnIndex) const
{
    return indexDirectoryPath() / (columns_[columnIndex].name + ".tree.meta.pb"); // Файл заголовка и корня дерева
}

// Формирование пути к файлу пула уникальных строк таблицы
std::filesystem::path Table::stringPoolPath() const
{
    return tablePath_ / "stringpool.pb"; // Файл для оптимизации хранения повторяющихся строк
}

// Статический метод инициализации структуры новой таблицы на диске
void Table::create(const std::filesystem::path& databasePath, const std::string& tableName, const std::vector<ColumnInfo>& columns)
{
    // Валидация имени таблицы на допустимые символы
    if (!isValidIdentifier(tableName))
    {
        throw std::runtime_error("Некорректное имя таблицы: " + tableName); // Исключение при запрещенных знаках
    }

    // Запрет на создание пустой таблицы без метаданных
    if (columns.empty())
    {
        throw std::runtime_error("Таблица должна содержать хотя бы один столбец"); // Ошибка пустой схемы
    }

    std::filesystem::path tablePath = databasePath / tableName; // Вычисление полной целевой папки таблицы
    // Защита от случайной перезаписи уже существующей таблицы
    if (std::filesystem::exists(tablePath))
    {
        throw std::runtime_error("Таблица уже существует: " + tableName); // Ошибка дублирования сущности
    }

    ensureDirectoryExists(tablePath); // Физическое создание директории таблицы на диске

    std::set<std::string> names; // Контейнер для отслеживания уникальности имен колонок
    // Перебор и проверка спецификаций каждого столбца
    for (std::size_t index = 0; index < columns.size(); ++index)
    {
        const ColumnInfo& column = columns[index]; // Ссылка на текущее описание столбца

        // Контроль корректности названия поля
        if (!isValidIdentifier(column.name))
        {
            throw std::runtime_error("некорректное имя столбца: " + column.name); // Ошибка формата имени
        }

        // Проверка на отсутствие дубликатов полей в рамках одной таблицы
        if (!names.insert(column.name).second)
        {
            throw std::runtime_error("Повтор имени столбца: " + column.name); // Ошибка повторения имени
        }

        // Логическое требование: индексируемые поля не могут принимать NULL
        if (column.indexed && !column.notNull)
        {
            throw std::runtime_error("INDEXED-столбец автоматически должен быть NOT_NULL: " + column.name); // Конфликт флагов
        }

        // Валидация соответствия типа дефолтного значения типу самого столбца
        if (column.hasDefault && column.defaultValue.type != ValueType::Null && !valueHasColumnType(column.defaultValue, column.type))
        {
            throw std::runtime_error("DEFAULT имеет неверный тип для столбца " + column.name); // Несовпадение типов данных
        }
    }

    std::vector<std::string> schemaRecords; // Временный буфер под сериализованную схему
    schemaRecords.push_back(schemaToProtoBytes(columns)); // Упаковка массива колонок в формат Protobuf
    writeSecureRecords(tablePath / "schema.pb", schemaRecords); // Безопасное сохранение схемы в файл

    createEmptySecureFile(tablePath / "rows.dat"); // Инициализация пустого зашифрованного файла записей
    createEmptySecureFile(tablePath / "deleted.pb"); // Инициализация пустого файла учета удалений
    createEmptySecureFile(tablePath / "stringpool.pb"); // Инициализация пустого файла пула строк
    ensureDirectoryExists(tablePath / "indexes"); // Создание пустой системной директории под индексы

    // Первоначальное создание файлов пустых B*-деревьев для проиндексированных полей
    for (std::size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
    {
        if (columns[columnIndex].indexed)
        {
            // Создание и очистка объекта дискового B*-индекса
            DiskBStarIndex emptyIndex(
                (tablePath / "indexes") / (columns[columnIndex].name + ".tree.pb"),
                (tablePath / "indexes") / (columns[columnIndex].name + ".tree.meta.pb"),
                columns[columnIndex].type
            );
            emptyIndex.clear(); // Запись базовой структуры пустого корневого узла дерева
        }
    }
}

// Удаление каталога таблицы и всех ассоциированных с ней файлов
void Table::drop(const std::filesystem::path& databasePath, const std::string& tableName)
{
    std::filesystem::path tablePath = databasePath / tableName; // Вычисление папки удаления

    // Защита от удаления отсутствующего ресурса
    if (!std::filesystem::exists(tablePath))
    {
        throw std::runtime_error("Таблица не существует: " + tableName); // Ошибка удаления
    }

    std::filesystem::remove_all(tablePath); // Полное рекурсивное удаление папки с диска
}

// Конструктор: монтирует объект таблицы и считывает её текущее состояние
Table::Table(const std::filesystem::path& databasePath, const std::string& tableName)
    : databasePath_(databasePath), tablePath_(databasePath / tableName), tableName_(tableName)
{
    // Валидация физического присутствия таблицы перед инициализацией
    if (!std::filesystem::exists(tablePath_))
    {
        throw std::runtime_error("Таблица не существует: " + tableName_); // Ошибка инициализации объекта
    }

    loadSchema(); // Выгрузка колонок из файла схемы
    validateSchema(); // Комплексная проверка целостности прочитанной схемы
    buildIndexes(); // Загрузка существующих или полная реконструкция поврежденных индексов
}

// Загрузка метаданных схемы таблицы из бинарного файла
void Table::loadSchema()
{
    columns_.clear(); // Сброс старого содержимого вектора колонок

    std::vector<std::string> records = readSecureRecords(schemaPath()); // Чтение блоков данных схемы
    if (records.empty())
    {
        throw std::runtime_error("Не удалось открыть schema.pb для таблицы " + tableName_); // Ошибка структуры файла
    }

    columns_ = schemaFromProtoBytes(records[0]); // Десериализация структуры колонок из байтового потока Protobuf
}

// Проверка структуры схемы данных на логические несоответствия
void Table::validateSchema() const
{
    // Схема обязана содержать описания колонок
    if (columns_.empty())
    {
        throw std::runtime_error("У таблицы пустая схема: " + tableName_); // Ошибка: таблица не имеет полей
    }

    std::set<std::string> names; // Локальный набор контроля уникальности имен

    // Валидация каждого поля в считанном массиве
    for (std::size_t index = 0; index < columns_.size(); ++index)
    {
        const ColumnInfo& column = columns_[index]; // Доступ к текущему описанию поля

        if (!isValidIdentifier(column.name))
        {
            throw std::runtime_error("Некорректное имя столбца в схеме: " + column.name); // Критическое имя в файле схемы
        }

        if (!names.insert(column.name).second)
        {
            throw std::runtime_error("Повтор имени столбца в схеме: " + column.name); // Нарушение уникальности полей
        }

        if (column.indexed && !column.notNull)
        {
            throw std::runtime_error("INDEXED-столбец должен быть NOT_NULL: " + column.name); // Нарушение ограничений целостности
        }
    }
}

// Инициализация индексных менеджеров для таблицы
void Table::buildIndexes()
{
    indexes_.clear(); // Сброс старой коллекции индексов
    indexes_.resize(columns_.size()); // Резервирование ячеек под каждый столбец схемы

    bool allIndexFilesExist = true; // Триггер проверки целостности индексных файлов
    // Сканирование наличия необходимых файлов деревьев на диске
    for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
    {
        if (columns_[columnIndex].indexed)
        {
            // Проверка одновременного наличия файла данных и файла метаданных дерева
            if (!secureFileExists(indexPath(columnIndex)) || !secureFileExists(indexMetaPath(columnIndex)))
            {
                allIndexFilesExist = false; // Хотя бы один индексный компонент утерян
            }
        }
    }

    // Создание объектов управления B*-деревьями в памяти
    for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
    {
        if (!columns_[columnIndex].indexed)
        {
            continue; // Игнорирование неиндексируемых полей таблицы
        }

        // Аллокация умного указателя на экземпляр дискового B*-дерева
        indexes_[columnIndex] = std::unique_ptr<IndexBase>(new DiskBStarIndex(
            indexPath(columnIndex),
            indexMetaPath(columnIndex),
            columns_[columnIndex].type
        ));
    }

    // Если файлы индексов повреждены или отсутствуют — запускаем полное перестроение
    if (!allIndexFilesExist)
    {
        persistIndexesFromRows(); // Реконструкция структуры индексов путем сканирования всей таблицы
    }
}

// Полное перестроение B*-деревьев на основе последовательного чтения файла строк
void Table::persistIndexesFromRows()
{
    ensureDirectoryExists(indexDirectoryPath()); // Гарантирование наличия папки индексов

    std::vector<std::unique_ptr<IndexBase> > rebuiltIndexes; // Временный массив для перестраиваемых индексов
    rebuiltIndexes.resize(columns_.size()); // Размещение по размеру схемы

    // Подготовка чистых пустых файлов деревьев для индексируемых полей
    for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
    {
        if (!columns_[columnIndex].indexed)
        {
            continue; // Пропуск полей без индекса
        }

        // Инициализация объекта пустого индекса
        rebuiltIndexes[columnIndex] = std::unique_ptr<IndexBase>(new DiskBStarIndex(
            indexPath(columnIndex),
            indexMetaPath(columnIndex),
            columns_[columnIndex].type
        ));
        rebuiltIndexes[columnIndex]->clear(); // Запись чистого корневого узла на диск
    }

    std::set<std::streamoff> deletedOffsets = loadDeletedOffsets(); // Загрузка позиций удаленных строк
    std::vector<std::pair<std::streamoff, std::string> > rawRows = readSecureRecordsWithOffsets(rowsPath()); // Чтение всех сырых строк с их смещениями

    // Наполнение деревьев актуальными данными из живых строк
    for (std::size_t rowIndex = 0; rowIndex < rawRows.size(); ++rowIndex)
    {
        std::streamoff offset = rawRows[rowIndex].first; // Смещение текущей записи в файле таблицы
        // Пропуск записей, которые помечены как удаленные
        if (rowOffsetIsDeleted(offset, deletedOffsets))
        {
            continue; // Переход к следующей строке файла
        }

        Row row = rowFromProtoBytes(rawRows[rowIndex].second, columns_); // Десериализация сырых байт в типизированную строку данных

        // Извлечение ключей и их интеграция в соответствующие деревья
        for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
        {
            if (!columns_[columnIndex].indexed)
            {
                continue; // Пропуск столбцов, не требующих индексации
            }

            // Индексируемое поле обязано содержать данные
            if (row[columnIndex].type == ValueType::Null)
            {
                throw std::runtime_error("INDEXED-столбец содержит NULL: " + columns_[columnIndex].name); // Нарушение констреинта UNIQUE/NOT_NULL
            }

            // Вставка ключа и файлового смещения строки в B*-дерево с контролем уникальности
            if (!rebuiltIndexes[columnIndex]->insert(row[columnIndex], offset))
            {
                throw std::runtime_error("Повтор значения INDEXED-столбца: " + columns_[columnIndex].name); // Нарушение уникальности ключа индекса
            }
        }
    }

    indexes_ = std::move(rebuiltIndexes); // Атомарное обновление рабочих индексов таблицы восстановленными объектами
}

// [Устарело] Заглушка старого метода сохранения структуры деревьев
void Table::saveIndexesToFiles() const
{
}

// [Устарело] Заглушка старого метода загрузки B*-индекса из файла
void Table::loadIndexesFromFiles()
{
}

// [Устарело] Вспомогательный метод записи сериализованного элемента индекса во временный буфер строк
void Table::writeIndexEntry(std::vector<std::string>& lines, const Value& key, std::streamoff offset) const
{
    lines.push_back(indexEntryToProtoBytes(key, offset)); // Упаковка пары Ключ-Смещение в формат Protobuf
}

// [Устарело] Восстановление параметров ключа и смещения из сериализованной строки
void Table::readIndexEntry(const std::string& line, ColumnType type, Value& key, std::streamoff& offset) const
{
    indexEntryFromProtoBytes(line, type, key, offset); // Десериализация одной записи индекса
}

// Принудительное считывание заголовка метаданных индекса для верификации доступа к файлу
void Table::touchIndexFile(std::size_t columnIndex) const
{
    std::vector<std::string> ignored = readSecureRecords(indexMetaPath(columnIndex)); // Холостой вызов чтения защищенного файла
    (void)ignored; // Подавление предупреждения компилятора о неиспользуемой переменной
}

// Поиск порядкового индекса столбца в схеме таблицы по его строковому имени
std::size_t Table::findColumnIndex(const std::string& name) const
{
    for (std::size_t index = 0; index < columns_.size(); ++index)
    {
        if (columns_[index].name == name)
        {
            return index; // Возврат позиции найденного столбца
        }
    }

    throw std::runtime_error("Неизвестный столбец '" + name + "' в таблице " + tableName_); // Ошибка: поле отсутствует в схеме
}

// Проверка ячейки данных на ограничения NOT NULL и совместимость типов данных
void Table::validateValueForColumn(const Value& value, const ColumnInfo& column) const
{
    // Обработка ситуации, когда значение пусто (NULL)
    if (value.type == ValueType::Null)
    {
        if (column.notNull)
        {
            throw std::runtime_error("Столбец '" + column.name + "' не может быть NULL"); // Запрет на запись NULL
        }
        return; // Корректно, если поле nullable
    }

    // Проверка физической совместимости типа константы и типа поля
    if (!valueHasColumnType(value, column.type))
    {
        throw std::runtime_error("Неверный тип значения для столбца '" + column.name + "'"); // Несовпадение типов
    }
}

// Комплексная валидация сформированной строки данных перед записью на диск
void Table::validateRow(const Row& row) const
{
    // Контроль соответствия количества переданных значений количеству полей таблицы
    if (row.size() != columns_.size())
    {
        throw std::runtime_error("Внутренняя ошибка: размер строки не совпадает со схемой"); // Критическая ошибка структуры данных
    }

    // Поэлементная валидация каждой ячейки строки
    for (std::size_t index = 0; index < columns_.size(); ++index)
    {
        validateValueForColumn(row[index], columns_[index]); // Вызов ограничений для ячейки
    }
}

// Проверка существования столбца, указанного внутри операнда логического выражения
void Table::validateOperand(const Operand& operand) const
{
    if (operand.isColumn)
    {
        findColumnIndex(operand.columnName); // Вызов поиска для подтверждения наличия поля в таблице
    }
}

// Определение результирующего типа данных операнда (для колонок и литералов)
ColumnType Table::operandType(const Operand& operand) const
{
    // Если операнд — колонка, возвращаем тип этой колонки из схемы
    if (operand.isColumn)
    {
        return columns_[findColumnIndex(operand.columnName)].type;
    }

    // Определение типа для целочисленного литерала
    if (operand.literalValue.type == ValueType::Int)
    {
        return ColumnType::Int;
    }

    // Определение типа для строкового литерала
    if (operand.literalValue.type == ValueType::String)
    {
        return ColumnType::String;
    }

    return ColumnType::String; // Тип по умолчанию для неопределенных текстовых констант
}

// Рекурсивный семантический анализ дерева условий выражения WHERE
void Table::validateCondition(const Expr& expr) const
{
    // Проверка логических связок AND / OR (рекурсивный спуск в ветви дерева)
    if (expr.kind == Expr::Kind::And || expr.kind == Expr::Kind::Or)
    {
        validateCondition(*expr.first); // Анализ левого поддерева условий
        validateCondition(*expr.second); // Анализ правого поддерева условий
        return;
    }

    // Валидация стандартного бинарного предиката сравнения (например, id == 5)
    if (expr.kind == Expr::Kind::Compare)
    {
        validateOperand(expr.left); // Валидация левой части операции
        validateOperand(expr.right); // Валидация правой части операции

        // Случай А: Сравнение колонки с другой колонкой
        if (expr.left.isColumn && expr.right.isColumn)
        {
            ColumnType leftType = operandType(expr.left); // Извлечение типа левого поля
            ColumnType rightType = operandType(expr.right); // Извлечение типа правого поля
            if (leftType != rightType)
            {
                throw std::runtime_error("В WHERE сравниваются столбцы разных типов"); // Ошибка типизации логики
            }
        }
        // Случай Б: Сравнение колонки с константным литералом
        else if (expr.left.isColumn || expr.right.isColumn)
        {
            Operand columnOperand = expr.left.isColumn ? expr.left : expr.right; // Выделение операнда-столбца
            Operand literalOperand = expr.left.isColumn ? expr.right : expr.left; // Выделение операнда-константы
            // Валидация соответствия типа константы типу поля, с которым она сравнивается
            if (literalOperand.literalValue.type != ValueType::Null && !valueHasColumnType(literalOperand.literalValue, columns_[findColumnIndex(columnOperand.columnName)].type))
            {
                throw std::runtime_error("В WHERE константа имеет неверный тип"); // Ошибка типа константы
            }
        }
        return;
    }

    // Валидация условий проверки диапазона BETWEEN
    if (expr.kind == Expr::Kind::Between)
    {
        validateOperand(expr.left); // Проверка целевого операнда
        validateOperand(expr.low); // Проверка нижней границы интервала
        validateOperand(expr.high); // Проверка верхней границы интервала
        return;
    }

    // Валидация условий поиска по шаблону/регулярному выражению LIKE
    if (expr.kind == Expr::Kind::Like)
    {
        validateOperand(expr.left); // Проверка целевой текстовой колонки
        validateOperand(expr.pattern); // Проверка операнда маски регулярного выражения
        return;
    }
}

// Чтение всех tombstone'ов (свободных слотов удаленных записей) из файла
std::vector<Table::FreeSlot> Table::loadFreeSlots() const
{
    std::vector<FreeSlot> slots; // Вектор для накопления прочитанных свободных слотов
    std::vector<std::string> records = readSecureRecords(deletedOffsetsPath()); // Чтение зашифрованных блоков из файла удалений

    // Цикл десериализации каждой записи о свободном месте
    for (std::size_t index = 0; index < records.size(); ++index)
    {
        ProtoDeletedOffset proto; // Объект Protobuf для декодирования структуры
        if (!proto.ParseFromString(records[index]))
        {
            throw std::runtime_error("Не удалось прочитать free-list запись из deleted.pb"); // Ошибка повреждения файла удалений
        }

        FreeSlot slot; // Локальная структура свободного слота
        slot.offset = static_cast<std::streamoff>(proto.offset()); // Извлечение файлового смещения слота
        slot.size = static_cast<std::streamoff>(proto.size()); // Извлечение доступного размера слота

        slots.push_back(slot); // Сохранение слота в результирующий вектор
    }

    return slots; // Возврат списка свободных мест для повторного использования
}

// Перезапись файла метаданных deleted.pb актуальным списком свободных слотов
void Table::saveFreeSlots(const std::vector<FreeSlot>& slots) const
{
    std::vector<std::string> records; // Вектор сериализованных байтовых строк

    // Перевод каждого слота в формат Protobuf и упаковка в байты
    for (std::size_t index = 0; index < slots.size(); ++index)
    {
        ProtoDeletedOffset proto; // Создание Protobuf сообщения
        proto.set_offset(static_cast<long long>(slots[index].offset)); // Запись смещения в структуру данных
        proto.set_size(static_cast<long long>(slots[index].size)); // Запись размера слота в структуру данных

        std::string bytes; // Строковый буфер для сериализации
        if (!proto.SerializeToString(&bytes))
        {
            throw std::runtime_error("Не удалось сериализовать free-list запись deleted.pb"); // Исключение при сбое кодирования
        }

        records.push_back(bytes); // Накопление байтовых строк для сохранения
    }

    writeSecureRecords(deletedOffsetsPath(), records); // Физическая защищенная запись обновленного списка слотов на диск
}

// Получение упорядоченного множества (set) смещений всех удаленных строк
std::set<std::streamoff> Table::loadDeletedOffsets() const
{
    std::set<std::streamoff> deletedOffsets; // Создание множества смещений для быстрого поиска
    std::vector<FreeSlot> slots = loadFreeSlots(); // Чтение всех свободных слотов с диска

    // Извлечение только адресной компоненты (offset) из каждого слота
    for (std::size_t index = 0; index < slots.size(); ++index)
    {
        deletedOffsets.insert(slots[index].offset); // Добавление адреса в поисковое множество
    }

    return deletedOffsets; // Возврат набора адресов удаленных записей
}

// Регистрация нового tombstone (пометка региона данных как удаленного)
void Table::appendDeletedOffset(std::streamoff offset, std::streamoff size)
{
    std::vector<FreeSlot> slots = loadFreeSlots(); // Чтение текущей карты свободных мест

    // Проверка, не был ли данный адрес уже зарегистрирован ранее
    for (std::size_t index = 0; index < slots.size(); ++index)
    {
        if (slots[index].offset == offset)
        {
            return; // Выход из метода, если дубликат адреса найден
        }
    }

    FreeSlot slot; // Формирование новой структуры свободного места
    slot.offset = offset; // Привязка адреса освободившегося пространства
    slot.size = size; // Привязка длины освободившегося пространства
    slots.push_back(slot); // Добавление в общий список
    saveFreeSlots(slots); // Фиксация измененного списка свободных слотов на диске
}

// Проверка: относится ли переданный offset к ранее удаленной строке
bool Table::rowOffsetIsDeleted(std::streamoff offset, const std::set<std::streamoff>& deletedOffsets) const
{
    return deletedOffsets.find(offset) != deletedOffsets.end(); // Поиск совпадения в кэшированном множестве
}

// Попытка утилизации и записи новой строки в первый подходящий по размеру свободный слот
bool Table::tryWriteRowToFreeSlot(const std::string& rowBytes, std::streamoff& offset)
{
    std::vector<FreeSlot> slots = loadFreeSlots(); // Получение карты доступных пустых участков файла

    // Поиск слота, способного вместить бинарные данные новой записи
    for (std::size_t index = 0; index < slots.size(); ++index)
    {
        if (slots[index].size <= 0)
        {
            continue; // Пропуск некорректных или пустых слотов
        }

        // Попытка безопасной перезаписи данных внутри выбранного участка файла
        if (overwriteSecureRecordInSlot(rowsPath(), rowBytes, slots[index].offset, slots[index].size))
        {
            offset = slots[index].offset; // Фиксация адреса вставки для вызывающей функции
            slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(index)); // Удаление занятого слота из списка свободных
            saveFreeSlots(slots); // Сохранение обновленной карты слотов на диск
            return true; // Индикация успешного повторного использования места
        }
    }

    return false; // Подходящих свободных мест не найдено, требуется запись в конец файла
}

// Физическое сохранение сериализованной строки в файл (в свободный слот или в хвост)
std::streamoff Table::writeRowToStorage(const Row& row)
{
    const std::string rowBytes = rowToProtoBytes(row); // Конвертация полей строки в байтовый поток Protobuf

    std::streamoff offset = 0; // Инициализация переменной адреса записи
    if (tryWriteRowToFreeSlot(rowBytes, offset))
    {
        return offset; // Возврат адреса, если удалось повторно использовать старое место
    }

    appendSecureRecord(rowsPath(), rowBytes, &offset); // Физическая дозапись новой строки в конец файла rows.dat
    return offset; // Возврат смещения новой записи в файле
}

// Чтение всех живых рядов и их смещений с фильтрацией удаленных элементов
std::vector<Table::StoredRow> Table::loadAllRows() const
{
    std::vector<StoredRow> rows; // Инициализация вектора результатов выборки
    std::vector<std::pair<std::streamoff, std::string> > lines = readSecureRecordsWithOffsets(rowsPath()); // Извлечение всех сырых записей со смещениями
    const std::set<std::streamoff> deletedOffsets = loadDeletedOffsets(); // Загрузка черного списка удаленных адресов

    // Фильтрация и десериализация записей таблицы
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        if (rowOffsetIsDeleted(lines[index].first, deletedOffsets))
        {
            continue; // Игнорирование записи, если её адрес помечен как удаленный
        }

        StoredRow item; // Создание объекта валидной строки СУБД
        item.offset = lines[index].first; // Перенос физического адреса строки
        item.row = rowFromProtoBytes(lines[index].second, columns_); // Восстановление полей строки по схеме таблицы
        rows.push_back(item); // Добавление строки в итоговую выборку
    }

    return rows; // Возврат массива всех неудаленных данных
}

// Прямое чтение и декодирование одной строки по её точному смещению в файле
Row Table::loadRowAtOffset(std::streamoff offset) const
{
    std::string rowBytes = readSecureRecordAtOffset(rowsPath(), offset); // Чтение сырых байт с указанной позиции файла
    return rowFromProtoBytes(rowBytes, columns_); // Десериализация и возврат типизированной строки данных
}

// Добавление новой строки в таблицу с валидацией ограничений и обновлением индексов
void Table::appendRow(const Row& row)
{
    validateRow(row); // Проверка соответствия типов данных строки схеме таблицы

    // Проверка уникальности и ограничений целостности для индексируемых полей
    for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
    {
        if (!columns_[columnIndex].indexed)
        {
            continue; // Пропуск полей, не имеющих B*-дерева
        }

        if (row[columnIndex].type == ValueType::Null)
        {
            throw std::runtime_error("INDEXED-столбец не может быть NULL: " + columns_[columnIndex].name); // Запрет на NULL в индексах
        }

        if (indexes_[columnIndex]->contains(row[columnIndex]))
        {
            throw std::runtime_error("Повтор значения для INDEXED-столбца: " + columns_[columnIndex].name); // Контроль уникальности ключа
        }
    }

    std::streamoff offset = writeRowToStorage(row); // Сохранение строки на диск и получение её физического смещения

    // Синхронная вставка ключей новой записи во все B*-деревья таблицы
    for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
    {
        if (columns_[columnIndex].indexed)
        {
            indexes_[columnIndex]->insert(row[columnIndex], offset); // Добавление пары Ключ-Смещение в индекс
        }
    }

    saveIndexesToFiles(); // Фиксация измененных B*-деревьев в файлы на диске
}

// [Устарело] Полная перезапись файла данных rows.dat с тотальной перестройкой индексов
void Table::rewriteRows(const std::vector<Row>& rows)
{
    std::set<int> intValues; // Хелпер для уникальности целочисленных ключей
    std::set<std::string> stringValues; // Хелпер для уникальности строковых ключей

    // Предварительная проверка всей пачки строк на соответствие типам схемы
    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
    {
        validateRow(rows[rowIndex]); // Построчная валидация типов
    }

    // Проверка уникальности данных во всех индексируемых колонках переданного пакета
    for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
    {
        if (!columns_[columnIndex].indexed)
        {
            continue; // Игнорирование колонок без индексов
        }

        intValues.clear(); // Очистка буфера уникальности чисел для новой колонки
        stringValues.clear(); // Очистка буфера уникальности строк для новой колонки

        // Построчный контроль ограничений уникальности
        for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
        {
            const Value& value = rows[rowIndex][columnIndex]; // Текущая проверяемая ячейка

            if (value.type == ValueType::Null)
            {
                throw std::runtime_error("INDEXED-столбец не может быть NULL: " + columns_[columnIndex].name); // Запрет на NULL-значения
            }

            if (value.type == ValueType::Int)
            {
                if (!intValues.insert(value.intValue).second)
                {
                    throw std::runtime_error("Повтор значения для INDEXED-столбца: " + columns_[columnIndex].name); // Обнаружен дубликат числа
                }
            }
            else
            {
                if (!stringValues.insert(*value.stringValue).second)
                {
                    throw std::runtime_error("Повтор значения для INDEXED-столбца: " + columns_[columnIndex].name); // Обнаружен дубликат строки
                }
            }
        }
    }

    std::vector<std::string> lines; // Буфер пакета сериализованных данных
    // Перевод всех переданных объектов строк в формат Protobuf-байтов
    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
    {
        lines.push_back(rowToProtoBytes(rows[rowIndex])); // Сериализация строки таблицы
    }

    writeSecureRecords(rowsPath(), lines); // Физическое затирание файла rows.dat новыми записями

    persistIndexesFromRows(); // Полный сброс и генерация новых B*-деревьев на основе только что записанных строк
    buildIndexes(); // Повторная инициализация менеджеров индексов в оперативной памяти
}

// Извлечение реального значения: чтение ячейки из переданного ряда или возврат готовой константы
Value Table::resolveOperand(const Row& row, const Operand& operand) const
{
    if (operand.isColumn)
    {
        return row[findColumnIndex(operand.columnName)]; // Поиск и возврат значения ячейки из структуры строки
    }

    return operand.literalValue; // Возврат неизменяемого константного значения
}

// Сравнение двух сущностей Value на основе переданного арифметического оператора сравнения
bool Table::compareOperands(const Value& left, const Value& right, CompareOp op) const
{
    // Любые операции сравнения (кроме явных проверок на NULL) с неопределенным значением ложны
    if (left.type == ValueType::Null || right.type == ValueType::Null)
    {
        return false; // Логика обработки неопределенности в SQL
    }

    int cmp = compareValues(left, right); // Вызов низкоуровневой функции сравнения (-1, 0, 1)

    if (op == CompareOp::Eq) return cmp == 0; // Оператор "равно"
    if (op == CompareOp::NotEq) return cmp != 0; // Оператор "не равно"
    if (op == CompareOp::Less) return cmp < 0; // Оператор "меньше"
    if (op == CompareOp::LessOrEq) return cmp <= 0; // Оператор "меньше или равно"
    if (op == CompareOp::Greater) return cmp > 0; // Оператор "больше"
    if (op == CompareOp::GreaterOrEq) return cmp >= 0; // Оператор "больше или равно"

    return false; // Защитный возврат ложного значения при неизвестном операторе
}

// Вычисление соответствия строки логическому дереву выражений секции WHERE
bool Table::rowMatches(const Row& row, const Expr& expr) const
{
    // Обработка логического И (AND): оба условия обязаны выполняться
    if (expr.kind == Expr::Kind::And)
    {
        return rowMatches(row, *expr.first) && rowMatches(row, *expr.second); // Рекурсивная проверка левой и правой ветви
    }

    // Обработка логического ИЛИ (OR): достаточно выполнения одного из условий
    if (expr.kind == Expr::Kind::Or)
    {
        return rowMatches(row, *expr.first) || rowMatches(row, *expr.second); // Рекурсивная проверка ветвей выражения
    }

    // Вычисление базового арифметического сравнения (например, цена > 100)
    if (expr.kind == Expr::Kind::Compare)
    {
        Value left = resolveOperand(row, expr.left); // Разрешение левого аргумента сравнения
        Value right = resolveOperand(row, expr.right); // Разрешение правого аргумента сравнения
        return compareOperands(left, right, expr.compareOp); // Вычисление результата операции
    }

    // Вычисление оператора попадания в диапазон BETWEEN (включая нижнюю, исключая верхнюю границу)
    if (expr.kind == Expr::Kind::Between)
    {
        Value value = resolveOperand(row, expr.left); // Извлечение проверяемой переменной
        Value low = resolveOperand(row, expr.low); // Извлечение нижней границы интервала
        Value high = resolveOperand(row, expr.high); // Извлечение верхней границы интервала

        // Если хотя бы один элемент диапазона неопределен — условие не выполняется
        if (value.type == ValueType::Null || low.type == ValueType::Null || high.type == ValueType::Null)
        {
            return false; // Возврат false при наличии NULL
        }

        return compareValues(value, low) >= 0 && compareValues(value, high) < 0; // Проверка вхождения в границы [low, high)
    }

    // Вычисление оператора LIKE с помощью сопоставления текста регулярным выражениям C++
    if (expr.kind == Expr::Kind::Like)
    {
        Value value = resolveOperand(row, expr.left); // Чтение текстового значения из проверяемой строки
        Value pattern = resolveOperand(row, expr.pattern); // Чтение маски поиска (регулярного выражения)

        // Проверка применимости: оба операнда обязаны быть строго строкового типа
        if (value.type != ValueType::String || pattern.type != ValueType::String)
        {
            return false; // Логическая ошибка типов ведет к невыполнению условия
        }

        return std::regex_match(*value.stringValue, std::regex(*pattern.stringValue)); // Выполнение проверки соответствия регулярному выражению
    }

    return false; // Страховочный сброс, если тип выражения не распознан
}

// Инверсия операторов знака сравнения при зеркальном изменении порядка операндов
CompareOp Table::reverseCompareOp(CompareOp op) const
{
    if (op == CompareOp::Less) return CompareOp::Greater; // Меньше преобразуется в Больше
    if (op == CompareOp::LessOrEq) return CompareOp::GreaterOrEq; // Меньше или равно преобразуется в Больше или равно
    if (op == CompareOp::Greater) return CompareOp::Less; // Больше преобразуется в Меньше
    if (op == CompareOp::GreaterOrEq) return CompareOp::LessOrEq; // Больше или равно преобразуется в Меньше или равно
    return op; // Равно и Не равно остаются неизменными при перестановке мест
}

// Попытка оптимизации поиска по бинарному сравнению с использованием B*-дерева
bool Table::tryUseIndexForCompare(const Expr& expr, std::vector<std::streamoff>& offsets) const
{
    if (expr.kind != Expr::Kind::Compare)
    {
        return false; // Отказ, если выражение не является операцией бинарного сравнения
    }

    Operand columnOperand = expr.left; // Проекция левой части выражения
    Operand literalOperand = expr.right; // Проекция правой части выражения
    CompareOp op = expr.compareOp; // Извлечение знака сравнения

    // Зеркалирование структуры выражения, если колонка находится в правой части (например, 5 == id)
    if (!columnOperand.isColumn && literalOperand.isColumn)
    {
        columnOperand = expr.right; // Перенос колонки влево
        literalOperand = expr.left; // Перенос константы вправо
        op = reverseCompareOp(op); // Корректировка знака сравнения на противоположный
    }

    // Если структура выражения не соответствует шаблону (Колонка Сравнение Константа) — индекс неприменим
    if (!columnOperand.isColumn || literalOperand.isColumn)
    {
        return false; // Отказ в оптимизации
    }

    std::size_t columnIndex = findColumnIndex(columnOperand.columnName); // Поиск физического индекса поля в схеме таблицы
    if (!columns_[columnIndex].indexed)
    {
        return false; // Отказ, если для данного столбца не было создано B*-дерево
    }

    if (literalOperand.literalValue.type == ValueType::Null)
    {
        return false; // Индекс неприменим для проверок с неопределенными значениями (NULL)
    }

    if (!valueHasColumnType(literalOperand.literalValue, columns_[columnIndex].type))
    {
        return false; // Отказ при несовпадении типов константы поиска и самого проиндексированного поля
    }

    touchIndexFile(columnIndex); // Проверка доступности и актуальности индексного файла на диске

    // Сценарий точечной выборки по точному совпадению (Оператор ==)
    if (op == CompareOp::Eq)
    {
        std::optional<std::streamoff> found = indexes_[columnIndex]->find(literalOperand.literalValue); // Поиск смещения в B*-дереве
        if (found.has_value())
        {
            offsets.push_back(found.value()); // Накопление найденного адреса строки таблицы
        }
        return true; // Индекс успешно отработал точечный запрос
    }

    // Сценарий диапазонного сканирования левой части дерева (Оператор <)
    if (op == CompareOp::Less)
    {
        offsets = indexes_[columnIndex]->lessThan(literalOperand.literalValue, false); // Извлечение адресов строк строго меньше ключа
        return true; // Индекс отработал диапазон
    }

    // Сценарий диапазонного сканирования левой части дерева включая ключ (Оператор <=)
    if (op == CompareOp::LessOrEq)
    {
        offsets = indexes_[columnIndex]->lessThan(literalOperand.literalValue, true); // Извлечение адресов строк меньше или равных ключу
        return true; // Индекс отработал диапазон
    }

    // Сценарий диапазонного сканирования правой части дерева (Оператор >)
    if (op == CompareOp::Greater)
    {
        offsets = indexes_[columnIndex]->greaterThan(literalOperand.literalValue, false); // Извлечение адресов строк строго больше ключа
        return true; // Индекс отработал диапазон
    }

    // Сценарий диапазонного сканирования правой части дерева включая ключ (Оператор >=)
    if (op == CompareOp::GreaterOrEq)
    {
        offsets = indexes_[columnIndex]->greaterThan(literalOperand.literalValue, true); // Извлечение адресов строк больше или равных ключу
        return true; // Индекс отработал диапазон
    }

    return false; // Защитный возврат false для нереализованных операторов сравнения
}

// Попытка оптимизации диапазонного поиска BETWEEN с использованием преимуществ B*-дерева
bool Table::tryUseIndexForBetween(const Expr& expr, std::vector<std::streamoff>& offsets) const
{
    if (expr.kind != Expr::Kind::Between)
    {
        return false; // Отказ, если тип логического выражения не BETWEEN
    }

    // Для работы индекса проверяемый операнд должен быть колонкой, а границы — чистыми константами
    if (!expr.left.isColumn || expr.low.isColumn || expr.high.isColumn)
    {
        return false; // Отказ в оптимизации из-за неверной структуры аргументов
    }

    std::size_t columnIndex = findColumnIndex(expr.left.columnName); // Идентификация столбца в схеме
    if (!columns_[columnIndex].indexed)
    {
        return false; // Отказ, если поле не имеет построенного дискового индекса
    }

    touchIndexFile(columnIndex); // Валидация дескриптора индексного файла на диске
    offsets = indexes_[columnIndex]->between(expr.low.literalValue, expr.high.literalValue); // Эффективное извлечение диапазона адресов из B*-дерева
    return true; // Индекс успешно вернул адреса строк для диапазона BETWEEN
}

// Универсальный диспетчер подсистемы индексации: проверяет возможность ускорения выборки
bool Table::tryUseIndex(const Expr& expr, std::vector<std::streamoff>& offsets) const
{
    // Попытка применить индекс для ускорения бинарного сравнения
    if (tryUseIndexForCompare(expr, offsets))
    {
        return true; // Оптимизация успешно применена
    }

    // Попытка применить индекс для ускорения диапазонного поиска BETWEEN
    if (tryUseIndexForBetween(expr, offsets))
    {
        return true; // Оптимизация успешно применена
    }

    // Рекурсивный анализ сложного составного условия, объединенного через логическое И (AND)
    if (expr.kind == Expr::Kind::And)
    {
        // Попытка извлечь выгоду и применить индекс к левой части составного условия
        if (tryUseIndex(*expr.first, offsets))
        {
            return true; // Левая ветвь успешно оптимизирована индексом
        }

        // Попытка извлечь выгоду и применить индекс к правой части составного условия
        if (tryUseIndex(*expr.second, offsets))
        {
            return true; // Правая ветвь успешно оптимизирована индексом
        }
    }

    return false; // Индексируемых полей в выражении не обнаружено, будет задействовано полное сканирование таблицы (Full Table Scan)
}


// Получение множества смещений-кандидатов на основе переданного логического условия
std::set<std::streamoff> Table::indexedCandidateOffsets(const Expr& condition, bool& usedIndex) const
{
    std::set<std::streamoff> candidates; // Результирующее уникальное множество смещений
    std::vector<std::streamoff> offsets; // Временный вектор для адресов из B*-дерева
    usedIndex = false; // Изначально сбрасываем флаг использования индекса

    // Попытка извлечь адреса строк по индексу
    if (tryUseIndex(condition, offsets))
    {
        usedIndex = true; // Фиксация факта успешного применения оптимизации
        // Перенос всех найденных адресов во множество для быстрого O(1) поиска
        for (std::size_t index = 0; index < offsets.size(); ++index)
        {
            candidates.insert(offsets[index]); // Вставка смещения во множество
        }
    }

    return candidates; // Возврат сформированных кандидатов на выборку
}

// Загрузка структуры StoredRow (строка + её смещение) по переданному массиву адресов
std::vector<Table::StoredRow> Table::loadStoredRowsByOffsets(const std::vector<std::streamoff>& offsets) const
{
    std::vector<StoredRow> rows; // Буфер для прочитанных строк с метаданными
    // Последовательное точечное чтение строк с диска по адресам
    for (std::size_t index = 0; index < offsets.size(); ++index)
    {
        StoredRow item; // Создание контейнера строки
        item.offset = offsets[index]; // Сохранение физического смещения
        item.row = loadRowAtOffset(offsets[index]); // Чтение и десериализация полей строки
        rows.push_back(item); // Накопление в результирующем векторе
    }
    return rows; // Возврат упакованных строк
}

// Загрузка чистых объектов данных Row по массиву смещений, полученных из индекса
std::vector<Row> Table::loadRowsByOffsets(const std::vector<std::streamoff>& offsets) const
{
    std::vector<Row> rows; // Создание вектора для хранения строк данных

    // Прямой посимвольный обход адресов и чтение записей
    for (std::size_t index = 0; index < offsets.size(); ++index)
    {
        rows.push_back(loadRowAtOffset(offsets[index])); // Чтение строки по смещению и запись в вектор
    }

    return rows; // Возврат массива прочитанных строк
}

// Вставка (INSERT) пакета новых строк с сопоставлением колонок и подстановкой DEFAULT
std::size_t Table::insertRows(const std::vector<std::string>& columns, const std::vector<std::vector<Value> >& rows)
{
    // Запрос на добавление обязан содержать перечень целевых полей
    if (columns.empty())
    {
        throw std::runtime_error("INSERT должен содержать хотя бы один столбец"); // Ошибка синтаксиса вставки
    }

    std::set<std::string> usedColumns; // Множество для проверки дублирования имен полей в запросе
    std::vector<std::size_t> targetIndexes; // Карта соответствия позиций запроса позициям в схеме таблицы

    // Поиск физических индексов полей в схеме и контроль уникальности упоминания
    for (std::size_t index = 0; index < columns.size(); ++index)
    {
        // Контроль: одна и та же колонка не должна наполняться дважды за запрос
        if (!usedColumns.insert(columns[index]).second)
        {
            throw std::runtime_error("столбец указан дважды в INSERT: " + columns[index]); // Дублирование столбца
        }
        targetIndexes.push_back(findColumnIndex(columns[index])); // Сохранение порядкового номера поля из схемы
    }

    std::vector<Row> preparedRows; // Буфер для валидированных и дополненных строк

    // Цикл разбора и подготовки каждой строки из пакета вставки
    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
    {
        // Количество переданных значений должно строго соответствовать списку колонок INSERT
        if (rows[rowIndex].size() != columns.size())
        {
            throw std::runtime_error("количество значений в INSERT не совпадает с количеством столбцов"); // Несовпадение размерности аргументов
        }

        Row row(columns_.size(), makeNull()); // Инициализация строки СУБД, заполненной по умолчанию NULL

        // Распределение переданных констант по их реальным физическим позициям в схеме
        for (std::size_t valueIndex = 0; valueIndex < rows[rowIndex].size(); ++valueIndex)
        {
            row[targetIndexes[valueIndex]] = rows[rowIndex][valueIndex]; // Запись значения в нужный слот
        }

        // Автоматическая подстановка DEFAULT-значений в пустые (NULL) ячейки
        for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
        {
            // Если значение пропущено, но у столбца настроен модификатор DEFAULT
            if (row[columnIndex].type == ValueType::Null && columns_[columnIndex].hasDefault)
            {
                row[columnIndex] = columns_[columnIndex].defaultValue; // Замещение NULL дефолтным значением
            }
        }

        validateRow(row); // Комплексная проверка типов данных и констреинтов строки
        preparedRows.push_back(row); // Сохранение полностью готовой строки в буфер
    }

    // Физическое сохранение подготовленных строк на диск и регистрация в B*-деревьях
    for (std::size_t rowIndex = 0; rowIndex < preparedRows.size(); ++rowIndex)
    {
        appendRow(preparedRows[rowIndex]); // Атомарная запись строки таблицы
    }

    return preparedRows.size(); // Возврат общего числа добавленных записей
}

// Модификация (UPDATE) строк, удовлетворяющих выражению WHERE
std::size_t Table::updateRows(const std::vector<UpdateAssignment>& assignments, const Expr& condition)
{
    validateCondition(condition); // Семантическая проверка структуры дерева условий WHERE

    // Предварительная валидация: проверяем типы присваиваемых значений еще до изменения данных
    for (std::size_t assignmentIndex = 0; assignmentIndex < assignments.size(); ++assignmentIndex)
    {
        std::size_t columnIndex = findColumnIndex(assignments[assignmentIndex].columnName); // Поиск целевого поля
        validateValueForColumn(assignments[assignmentIndex].value, columns_[columnIndex]); // Проверка типа константы констреинтам поля
    }

    bool usedIndex = false; // Флаг применения индекса при фильтрации
    std::set<std::streamoff> candidates = indexedCandidateOffsets(condition, usedIndex); // Выделение адресов-кандидатов через индекс
    std::vector<StoredRow> stored = loadAllRows(); // Загрузка всех неудаленных строк таблицы для сканирования

    // Локальная структура для кэширования транзакции изменения строки
    struct PendingUpdate
    {
        std::streamoff oldOffset = 0; // Прежний адрес строки в файле
        Row oldRow; // Копия исходной строки до модификации
        Row newRow; // Новая модифицированная строка
    };

    std::vector<PendingUpdate> pending; // Список строк, одобренных к обновлению

    // Этап 1: Поиск строк для изменения и конструирование новых значений
    for (std::size_t rowIndex = 0; rowIndex < stored.size(); ++rowIndex)
    {
        bool mayMatch = true; // Флаг первичного отбора
        // Оптимизация: если сработал индекс, проверяем присутствие адреса строки в кандидатах
        if (usedIndex)
        {
            mayMatch = candidates.find(stored[rowIndex].offset) != candidates.end(); // Проверка вхождения смещения
        }

        // Полное вычисление WHERE для строк, прошедших первичный отбор
        if (mayMatch && rowMatches(stored[rowIndex].row, condition))
        {
            Row newRow = stored[rowIndex].row; // Клонирование текущих данных строки
            // Применение всех инструкций присваивания (SET) к новой строке
            for (std::size_t assignmentIndex = 0; assignmentIndex < assignments.size(); ++assignmentIndex)
            {
                std::size_t columnIndex = findColumnIndex(assignments[assignmentIndex].columnName); // Индекс изменяемого поля
                newRow[columnIndex] = assignments[assignmentIndex].value; // Запись нового значения в ячейку
            }

            validateRow(newRow); // Проверка новой структуры строки на ограничения целостности

            PendingUpdate item; // Создание задачи на обновление
            item.oldOffset = stored[rowIndex].offset; // Фиксация старого адреса
            item.oldRow = stored[rowIndex].row; // Фиксация старых данных
            item.newRow = newRow; // Фиксация новых данных
            pending.push_back(item); // Добавление задачи в пул отложенных действий
        }
    }

    // Этап 2: Превентивная проверка констреинтов UNIQUE для проиндексированных полей
    for (std::size_t updateIndex = 0; updateIndex < pending.size(); ++updateIndex)
    {
        for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
        {
            if (!columns_[columnIndex].indexed)
            {
                continue; // Работаем только со столбцами, имеющими B*-дерево
            }

            const Value& oldValue = pending[updateIndex].oldRow[columnIndex]; // Исходное значение ячейки
            const Value& newValue = pending[updateIndex].newRow[columnIndex]; // Новое значение ячейки

            bool changed = false; // Триггер факта изменения данных в ключевом поле
            if (oldValue.type != newValue.type)
            {
                changed = true; // Смена типа (например, с NULL на INT) означает изменение
            }
            else if (oldValue.type != ValueType::Null)
            {
                changed = compareValues(oldValue, newValue) != 0; // Арифметическое сравнение значений ключа
            }

            // Запрет на генерацию дубликатов в уникальном INDEXED-поле
            if (changed && indexes_[columnIndex]->contains(newValue))
            {
                throw std::runtime_error("UPDATE нарушает уникальность INDEXED-столбца: " + columns_[columnIndex].name); // Конфликт уникальности
            }
        }
    }

    // Этап 3: Физическая перезапись данных на диске и актуализация B*-деревьев
    for (std::size_t updateIndex = 0; updateIndex < pending.size(); ++updateIndex)
    {
        std::streamoff newOffset = writeRowToStorage(pending[updateIndex].newRow); // Запись обновленной строки на новое место диска
        std::streamoff oldSize = secureRecordTotalSizeAtOffset(rowsPath(), pending[updateIndex].oldOffset); // Замер размера старой записи
        appendDeletedOffset(pending[updateIndex].oldOffset, oldSize); // Перевод старого адреса в категорию свободных слотов (tombstone)

        // Обновление указателей внутри B*-деревьев для модифицированной записи
        for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
        {
            if (!columns_[columnIndex].indexed)
            {
                continue; // Игнорирование неиндексируемых столбцов
            }

            indexes_[columnIndex]->erase(pending[updateIndex].oldRow[columnIndex]); // Удаление старого ключа из дерева
            indexes_[columnIndex]->insert(pending[updateIndex].newRow[columnIndex], newOffset); // Монтирование нового ключа со смещением
        }
    }

    // Если изменения состоялись — фиксируем новые состояния B*-деревьев в файлы
    if (!pending.empty())
    {
        saveIndexesToFiles(); // Перезапись файлов индексов на диске
    }

    return pending.size(); // Возврат количества успешно измененных записей
}

// Удаление (DELETE) строк, подпадающих под условия WHERE
std::size_t Table::deleteRows(const Expr& condition)
{
    validateCondition(condition); // Проверка корректности дерева выражений селектора WHERE

    bool usedIndex = false; // Маркер использования индексной оптимизации
    std::set<std::streamoff> candidates = indexedCandidateOffsets(condition, usedIndex); // Поиск адресов через B*-дерево
    std::vector<StoredRow> stored = loadAllRows(); // Загрузка всех активных записей из таблицы
    std::size_t deleted = 0; // Счетчик удаленных записей

    // Перебор строк и их логическое удаление
    for (std::size_t rowIndex = 0; rowIndex < stored.size(); ++rowIndex)
    {
        bool mayMatch = true; // Предварительное согласие на соответствие критериям
        // Фильтрация по списку смещений, если отработал поисковый индекс
        if (usedIndex)
        {
            mayMatch = candidates.find(stored[rowIndex].offset) != candidates.end(); // Проверка вхождения смещения в индексный кэш
        }

        // Финальная проверка строки предикатом WHERE
        if (mayMatch && rowMatches(stored[rowIndex].row, condition))
        {
            std::streamoff oldSize = secureRecordTotalSizeAtOffset(rowsPath(), stored[rowIndex].offset); // Определение длины удаляемой записи
            appendDeletedOffset(stored[rowIndex].offset, oldSize); // Добавление адреса строки в список удалений (free-list слоты)

            // Каскадное изъятие ключей удаленной строки из всех B*-деревьев таблицы
            for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
            {
                if (columns_[columnIndex].indexed)
                {
                    indexes_[columnIndex]->erase(stored[rowIndex].row[columnIndex]); // Удаление ключа из текущего индекса
                }
            }

            ++deleted; // Инкремент счетчика удалений
        }
    }

    // Сохранение изменений в индексах, если хотя бы одна строка была стерта
    if (deleted > 0)
    {
        saveIndexesToFiles(); // Актуализация файлов индексов на диске
    }

    return deleted; // Возврат числа уничтоженных строк
}

// Инспекция полей SELECT: проверяет наличие вызовов агрегатных функций (SUM, COUNT, AVG)
bool Table::itemsContainAggregates(const std::vector<SelectItem>& items) const
{
    for (std::size_t index = 0; index < items.size(); ++index)
    {
        // Если тип элемента выборки отличен от стандартного Column — это агрегат
        if (items[index].kind != SelectItem::Kind::Column)
        {
            return true; // Обнаружена агрегатная функция
        }
    }
    return false; // Запрос содержит только перечисление обычных полей
}

// Формирование итогового текстового ключа (имени колонки) для вывода агрегата в JSON
std::string Table::defaultAggregateName(const SelectItem& item) const
{
    // Приоритет отдается явно заданному пользователем псевдониму (модификатор AS)
    if (item.alias.has_value())
    {
        return item.alias.value(); // Использование алиаса в качестве ключа
    }

    // Генерация автоматического имени для функции подсчета строк COUNT
    if (item.kind == SelectItem::Kind::Count)
    {
        if (item.countStar) return "COUNT(*)"; // Имя для общего подсчета строк
        return "COUNT(" + item.columnName + ")"; // Имя для подсчета строк с NOT NULL значениями поля
    }

    // Автоматическое имя для функции суммирования SUM
    if (item.kind == SelectItem::Kind::Sum)
    {
        return "SUM(" + item.columnName + ")"; // Название результирующего агрегата
    }

    // Автоматическое имя для расчета среднего арифметического AVG
    if (item.kind == SelectItem::Kind::Avg)
    {
        return "AVG(" + item.columnName + ")"; // Название результирующего агрегата
    }

    return item.columnName; // Возврат базового имени, если тип определить не удалось
}

// Расчет агрегатных функций по массиву строк и формирование результирующего JSON-объекта
std::string Table::makeAggregateJson(const std::vector<Row>& rows, const std::vector<SelectItem>& items) const
{
    std::map<std::string, Value> object; // Ассоциативная карта одной строки результатов агрегации

    // Вычисление каждого агрегатного поля, запрошенного в SELECT
    for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex)
    {
        const SelectItem& item = items[itemIndex]; // Ссылка на описание элемента выборки
        std::string name = defaultAggregateName(item); // Вычисление заголовка результирующей колонки

        // Вычисление значения агрегата COUNT
        if (item.kind == SelectItem::Kind::Count)
        {
            if (item.countStar)
            {
                object[name] = makeInt(static_cast<int>(rows.size())); // COUNT(*) возвращает общий размер выборки
            }
            else
            {
                std::size_t columnIndex = findColumnIndex(item.columnName); // Поиск номера поля в схеме
                int count = 0; // Инициализация локального счетчика живых значений
                // Подсчет количества полей, не содержащих NULL
                for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
                {
                    if (rows[rowIndex][columnIndex].type != ValueType::Null)
                    {
                        ++count; // Инкремент при наличии реальных данных
                    }
                }
                object[name] = makeInt(count); // Запись вычисленного счетчика
            }
        }
        // Вычисление математических агрегатов SUM и AVG
        else if (item.kind == SelectItem::Kind::Sum || item.kind == SelectItem::Kind::Avg)
        {
            std::size_t columnIndex = findColumnIndex(item.columnName); // Нахождение столбца в схеме таблицы
            // Ограничение: суммирование и среднее применимы исключительно к типу INT
            if (columns_[columnIndex].type != ColumnType::Int)
            {
                throw std::runtime_error("SUM и AVG поддерживаются только для INT-столбцов"); // Ошибка бизнес-логики вычислений
            }

            int sum = 0; // Накопитель суммы значений
            int count = 0; // Накопитель количества валидных числовых слагаемых
            // Итеративный расчет базовых математических метрик по массиву строк
            for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
            {
                if (rows[rowIndex][columnIndex].type == ValueType::Int)
                {
                    sum += rows[rowIndex][columnIndex].intValue; // Прибавление числового значения ячейки
                    ++count; // Увеличение числа слагаемых
                }
            }

            // Формирование итогового ответа для математической суммы SUM
            if (item.kind == SelectItem::Kind::Sum)
            {
                object[name] = makeInt(sum); // Фиксация итоговой суммы
            }
            // Формирование ответа для среднего арифметического AVG с защитой от деления на ноль
            else
            {
                if (count == 0) object[name] = makeNull(); // Если строк нет — среднее значение не определено (NULL)
                else object[name] = makeInt(sum / count); // Целочисленное деление для вычисления среднего
            }
        }
        // Запрет на одновременный вывод сырых текстовых полей и сгруппированных агрегатов
        else
        {
            throw std::runtime_error("нельзя смешивать обычные столбцы и агрегаты в этой простой реализации"); // Ошибка структуры SELECT
        }
    }

    std::vector<std::map<std::string, Value> > objects; // Обертка в массив для совместимости с JSON-конвертером
    objects.push_back(object); // Помещение единственного результирующего агрегатного объекта
    return objectsToJson(objects); // Сериализация структуры в финальный JSON-текст
}

// Формирование стандартного JSON-массива строк на основе результатов выборки
std::string Table::makeRegularJson(const std::vector<Row>& rows, bool selectAll, const std::vector<SelectItem>& items) const
{
    std::vector<std::map<std::string, Value> > objects; // Коллекция будущих JSON-объектов

    // Трансформация каждой внутренней строки СУБД в ассоциативную структуру ключ-значение
    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
    {
        std::map<std::string, Value> object; // Картографическая модель текущей строки

        // Сценарий А: Запрос SELECT * (вывод абсолютно всех полей таблицы)
        if (selectAll)
        {
            for (std::size_t columnIndex = 0; columnIndex < columns_.size(); ++columnIndex)
            {
                object[columns_[columnIndex].name] = rows[rowIndex][columnIndex]; // Маппинг оригинального имени и значения
            }
        }
        // Сценарий Б: Выборочный вывод конкретно перечисленных столбцов
        else
        {
            for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex)
            {
                const SelectItem& item = items[itemIndex]; // Ссылка на описание целевого поля выборки
                // Повторная защитная проверка на отсутствие агрегатных функций в данном методе
                if (item.kind != SelectItem::Kind::Column)
                {
                    throw std::runtime_error("агрегаты нельзя выводить как обычные строки"); // Нарушение логики маршрутизации вызовов
                }

                std::size_t columnIndex = findColumnIndex(item.columnName); // Нахождение физической позиции поля
                std::string outputName = item.alias.has_value() ? item.alias.value() : item.columnName; // Выбор имени (алиас или оригинал)
                object[outputName] = rows[rowIndex][columnIndex]; // Запись ячейки в выходную структуру под выбранным именем
            }
        }

        objects.push_back(object); // Помещение сформированного объекта строки в общий результирующий массив
    }

    return objectsToJson(objects); // Вызов утилиты сериализации коллекции объектов в текстовый JSON-формат
}

// Основной метод выполнения SELECT: маршрутизирует выборку по индексам или полным проходом (Full Scan)
SelectResult Table::selectRows(bool selectAll, const std::vector<SelectItem>& items, const std::optional<Expr>& condition) const
{
    std::vector<Row> rows; // Коллекция строк, прошедших фильтрацию предикатами WHERE
    bool usedIndex = false; // Локальный триггер успешности задействования B*-дерева

    // Запрос на выборку обязан иметь спецификацию структуры вывода
    if (!selectAll && items.empty())
    {
        throw std::runtime_error("SELECT должен содержать * или список столбцов"); // Ошибка пустого проектора полей
    }

    // Предварительная валидация названий колонок в списке SELECT на их реальное существование
    if (!selectAll)
    {
        for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex)
        {
            // Пропускаем проверку, если это общий системный подсчет COUNT(*)
            if (items[itemIndex].kind != SelectItem::Kind::Count || !items[itemIndex].countStar)
            {
                if (!items[itemIndex].columnName.empty())
                {
                    findColumnIndex(items[itemIndex].columnName); // Проверка наличия поля в схеме (кидает ошибку, если нет)
                }
            }
        }
    }

    // Сценарий 1: Обработка запроса, содержащего блок условной фильтрации WHERE
    if (condition.has_value())
    {
        validateCondition(condition.value()); // Аналитическая проверка типов внутри дерева условий

        std::vector<std::streamoff> offsets; // Буфер для хранения адресов строк от индексного менеджера
        // Попытка применить B*-дерево для сужения круга поиска записей
        if (tryUseIndex(condition.value(), offsets))
        {
            usedIndex = true; // Индекс успешно перехватил и ускорил выполнение запроса
            rows = loadRowsByOffsets(offsets); // Точечная выгрузка с диска только потенциально подходящих строк

            std::vector<Row> filtered; // Контейнер для финишной фильтрации составных условий
            // Дополнительная перепроверка строк (необходима при оптимизации сложных условий AND/OR)
            for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
            {
                if (rowMatches(rows[rowIndex], condition.value()))
                {
                    filtered.push_back(rows[rowIndex]); // Сохранение строки, полностью прошедшей валидацию предикатом
                }
            }
            rows = filtered; // Обновление результирующего массива отфильтрованными данными
        }
        // Худший сценарий: Индекс неприменим, выполняется линейный перебор всей таблицы (Full Table Scan)
        else
        {
            std::vector<StoredRow> stored = loadAllRows(); // Последовательное считывание абсолютно всех живых строк с диска
            // Построчное вычисление предиката WHERE для всего объема данных таблицы
            for (std::size_t rowIndex = 0; rowIndex < stored.size(); ++rowIndex)
            {
                if (rowMatches(stored[rowIndex].row, condition.value()))
                {
                    rows.push_back(stored[rowIndex].row); // Фиксация строки при полном совпадении условий
                }
            }
        }
    }
    // Сценарий 2: Запрос без условий WHERE — безусловное чтение всех записей таблицы
    else
    {
        std::vector<StoredRow> stored = loadAllRows(); // Прямая выгрузка всех строк из файла rows.dat
        // Наполнение вектора результатов чистыми объектами Row без файловых смещений
        for (std::size_t rowIndex = 0; rowIndex < stored.size(); ++rowIndex)
        {
            rows.push_back(stored[rowIndex].row); // Извлечение строки данных
        }
    }

    SelectResult result; // Формирование итоговой структуры ответа СУБД
    result.rowCount = rows.size(); // Сохранение количества записей в ответе
    result.usedIndex = usedIndex; // Фиксация диагностического флага использования индекса для профилирования

    // Финальный этап: Преобразование отобранных структур данных в JSON формат в зависимости от режима
    if (!selectAll && itemsContainAggregates(items))
    {
        result.json = makeAggregateJson(rows, items); // Генерация сжатого JSON с результатами математических агрегатов
    }
    else
    {
        result.json = makeRegularJson(rows, selectAll, items); // Генерация плоского JSON со списком стандартных строк
    }

    return result; // Возврат сформированного ответа на уровень диспетчеризации выполнения команд СУБД
}


