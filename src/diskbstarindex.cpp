#include "diskbstarindex.h"
#include "securefile.h"
#include "storage.h"
#include "stringpool.h"
#include "utils.h"

#include <algorithm>
#include <stdexcept>

DiskBStarIndex::DiskBStarIndex(
    const std::filesystem::path& pageFilePath,
    const std::filesystem::path& metaFilePath,
    ColumnType keyType
)
    : pageFilePath_(pageFilePath), metaFilePath_(metaFilePath), keyType_(keyType)
{
    ensureFilesExist();
}

void DiskBStarIndex::ensureFilesExist() const
{
    if (!secureFileExists(pageFilePath_))
    {
        createEmptySecureFile(pageFilePath_);
    }

    if (!secureFileExists(metaFilePath_))
    {
        HeaderData header;
        saveHeader(header);
    }
}

void DiskBStarIndex::clear()
{
    std::vector<std::string> emptyPages;
    writeSecureRecords(pageFilePath_, emptyPages);
    HeaderData header;
    saveHeader(header);
}

DiskBStarIndex::HeaderData DiskBStarIndex::loadHeader() const
{
    ensureFilesExist();

    HeaderData header;
    std::vector<std::string> records = readSecureRecords(metaFilePath_);
    if (records.empty())
    {
        return header;
    }

    ProtoBStarDiskHeader proto;
    if (!proto.ParseFromString(records[0]))
    {
        throw std::runtime_error("не удалось прочитать meta-файл файлового B*-индекса");
    }

    header.rootExists = proto.root_exists();
    header.rootPageId = proto.root_page_id();
    header.nextPageId = proto.next_page_id();
    header.size = proto.size();

    for (int index = 0; index < proto.locations_size(); ++index)
    {
        const ProtoBStarPageLocation& location = proto.locations(index);
        PageLocation item;
        item.offset = static_cast<std::streamoff>(location.offset());
        item.size = static_cast<std::streamoff>(location.size());
        header.locations[location.page_id()] = item;
    }

    for (int index = 0; index < proto.free_slots_size(); ++index)
    {
        const ProtoBStarFreeSlot& slot = proto.free_slots(index);
        FreeSlot item;
        item.offset = static_cast<std::streamoff>(slot.offset());
        item.size = static_cast<std::streamoff>(slot.size());
        header.freeSlots.push_back(item);
    }

    return header;
}

void DiskBStarIndex::saveHeader(const HeaderData& header) const
{
    ProtoBStarDiskHeader proto;
    proto.set_root_exists(header.rootExists);
    proto.set_root_page_id(header.rootPageId);
    proto.set_next_page_id(header.nextPageId);
    proto.set_size(header.size);

    for (std::map<long long, PageLocation>::const_iterator it = header.locations.begin(); it != header.locations.end(); ++it)
    {
        ProtoBStarPageLocation* location = proto.add_locations();
        location->set_page_id(it->first);
        location->set_offset(static_cast<long long>(it->second.offset));
        location->set_size(static_cast<long long>(it->second.size));
    }

    for (std::size_t index = 0; index < header.freeSlots.size(); ++index)
    {
        ProtoBStarFreeSlot* slot = proto.add_free_slots();
        slot->set_offset(static_cast<long long>(header.freeSlots[index].offset));
        slot->set_size(static_cast<long long>(header.freeSlots[index].size));
    }

    std::string bytes;
    if (!proto.SerializeToString(&bytes))
    {
        throw std::runtime_error("не удалось сериализовать meta-файл B*-индекса");
    }

    std::vector<std::string> records;
    records.push_back(bytes);
    writeSecureRecords(metaFilePath_, records);
}

ProtoBStarPage DiskBStarIndex::makePage(HeaderData& header, bool isLeaf) const
{
    ProtoBStarPage page;
    page.set_page_id(header.nextPageId);
    header.nextPageId += 1;
    page.set_is_leaf(isLeaf);
    page.set_deleted(false);
    return page;
}

ProtoBStarPage DiskBStarIndex::readPage(const HeaderData& header, long long pageId) const
{
    std::map<long long, PageLocation>::const_iterator found = header.locations.find(pageId);
    if (found == header.locations.end())
    {
        throw std::runtime_error("в файловом B*-индексе нет страницы page_id=" + std::to_string(pageId));
    }

    std::string bytes = readSecureRecordAtOffset(pageFilePath_, found->second.offset);
    ProtoBStarPage page;
    if (!page.ParseFromString(bytes))
    {
        throw std::runtime_error("не удалось прочитать страницу B*-индекса из .tree.pb");
    }

    if (page.deleted())
    {
        throw std::runtime_error("попытка прочитать tombstone-страницу B*-индекса");
    }

    return page;
}

void DiskBStarIndex::writePage(HeaderData& header, const ProtoBStarPage& page) const
{
    std::string bytes;
    if (!page.SerializeToString(&bytes))
    {
        throw std::runtime_error("не удалось сериализовать страницу файлового B*-индекса");
    }

    std::map<long long, PageLocation>::iterator current = header.locations.find(page.page_id());
    if (current != header.locations.end())
    {
        if (overwriteSecureRecordInSlot(pageFilePath_, bytes, current->second.offset, current->second.size))
        {
            return;
        }

        header.freeSlots.push_back(FreeSlot{current->second.offset, current->second.size});
        header.locations.erase(current);
    }

    for (std::size_t index = 0; index < header.freeSlots.size(); ++index)
    {
        if (overwriteSecureRecordInSlot(pageFilePath_, bytes, header.freeSlots[index].offset, header.freeSlots[index].size))
        {
            PageLocation location;
            location.offset = header.freeSlots[index].offset;
            location.size = header.freeSlots[index].size;
            header.locations[page.page_id()] = location;
            header.freeSlots.erase(header.freeSlots.begin() + static_cast<std::ptrdiff_t>(index));
            return;
        }
    }

    std::streamoff offset = 0;
    appendSecureRecord(pageFilePath_, bytes, &offset);
    PageLocation location;
    location.offset = offset;
    location.size = secureRecordTotalSizeAtOffset(pageFilePath_, offset);
    header.locations[page.page_id()] = location;
}

void DiskBStarIndex::freeOldPageSlot(HeaderData& header, long long pageId) const
{
    std::map<long long, PageLocation>::iterator found = header.locations.find(pageId);
    if (found == header.locations.end())
    {
        return;
    }
    header.freeSlots.push_back(FreeSlot{found->second.offset, found->second.size});
    header.locations.erase(found);
}

Value DiskBStarIndex::entryKey(const ProtoIndexEntry& entry) const
{
    return valueFromProto(entry.key(), keyType_);
}

ProtoIndexEntry DiskBStarIndex::makeEntry(const Value& key, std::streamoff offset, bool deleted) const
{
    ProtoIndexEntry entry;
    *entry.mutable_key() = valueToProto(key);
    entry.set_offset(static_cast<long long>(offset));
    entry.set_deleted(deleted);
    return entry;
}

int DiskBStarIndex::compareKeyWithEntry(const Value& key, const ProtoIndexEntry& entry) const
{
    Value right = entryKey(entry);
    return compareValues(key, right);
}

int DiskBStarIndex::findEntryPosition(const ProtoBStarPage& page, const Value& key) const
{
    int index = 0;
    while (index < page.entries_size() && compareKeyWithEntry(key, page.entries(index)) > 0)
    {
        ++index;
    }
    return index;
}

std::vector<ProtoIndexEntry> DiskBStarIndex::pageEntries(const ProtoBStarPage& page) const
{
    std::vector<ProtoIndexEntry> entries;
    for (int index = 0; index < page.entries_size(); ++index)
    {
        entries.push_back(page.entries(index));
    }
    return entries;
}

std::vector<long long> DiskBStarIndex::pageChildren(const ProtoBStarPage& page) const
{
    std::vector<long long> children;
    for (int index = 0; index < page.child_page_ids_size(); ++index)
    {
        children.push_back(page.child_page_ids(index));
    }
    return children;
}

void DiskBStarIndex::replaceEntries(ProtoBStarPage& page, const std::vector<ProtoIndexEntry>& entries) const
{
    page.clear_entries();
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        *page.add_entries() = entries[index];
    }
}

void DiskBStarIndex::replaceChildren(ProtoBStarPage& page, const std::vector<long long>& children) const
{
    page.clear_child_page_ids();
    for (std::size_t index = 0; index < children.size(); ++index)
    {
        page.add_child_page_ids(children[index]);
    }
}

void DiskBStarIndex::compactLeafTombstones(ProtoBStarPage& page) const
{
    if (!page.is_leaf())
    {
        return;
    }

    std::vector<ProtoIndexEntry> active;
    for (int index = 0; index < page.entries_size(); ++index)
    {
        if (!page.entries(index).deleted())
        {
            active.push_back(page.entries(index));
        }
    }
    replaceEntries(page, active);
}

std::optional<std::streamoff> DiskBStarIndex::findInPage(const HeaderData& header, long long pageId, const Value& key) const
{
    ProtoBStarPage page = readPage(header, pageId);
    const int position = findEntryPosition(page, key);

    if (position < page.entries_size() && compareKeyWithEntry(key, page.entries(position)) == 0)
    {
        if (page.entries(position).deleted())
        {
            return std::nullopt;
        }
        return static_cast<std::streamoff>(page.entries(position).offset());
    }

    if (page.is_leaf())
    {
        return std::nullopt;
    }

    if (position >= page.child_page_ids_size())
    {
        return std::nullopt;
    }

    return findInPage(header, page.child_page_ids(position), key);
}

std::optional<std::streamoff> DiskBStarIndex::find(const Value& key) const
{
    HeaderData header = loadHeader();
    if (!header.rootExists)
    {
        return std::nullopt;
    }
    return findInPage(header, header.rootPageId, key);
}

bool DiskBStarIndex::contains(const Value& key) const
{
    return find(key).has_value();
}

bool DiskBStarIndex::redistributeWithLeftForInsert(HeaderData& header, ProtoBStarPage& parent, int childIndex)
{
    if (childIndex <= 0)
    {
        return false;
    }

    ProtoBStarPage left = readPage(header, parent.child_page_ids(childIndex - 1));
    ProtoBStarPage child = readPage(header, parent.child_page_ids(childIndex));
    if (left.is_leaf() != child.is_leaf() || left.entries_size() >= kMaxEntries)
    {
        return false;
    }

    std::vector<ProtoIndexEntry> combined;
    std::vector<ProtoIndexEntry> leftEntries = pageEntries(left);
    std::vector<ProtoIndexEntry> childEntries = pageEntries(child);
    combined.insert(combined.end(), leftEntries.begin(), leftEntries.end());
    combined.push_back(parent.entries(childIndex - 1));
    combined.insert(combined.end(), childEntries.begin(), childEntries.end());

    const int remaining = static_cast<int>(combined.size()) - 1;
    const int newLeftCount = remaining / 2;
    const int separatorIndex = newLeftCount;

    std::vector<ProtoIndexEntry> newLeftEntries(combined.begin(), combined.begin() + newLeftCount);
    std::vector<ProtoIndexEntry> newChildEntries(combined.begin() + separatorIndex + 1, combined.end());
    replaceEntries(left, newLeftEntries);
    replaceEntries(child, newChildEntries);
    *parent.mutable_entries(childIndex - 1) = combined[separatorIndex];

    if (!child.is_leaf())
    {
        std::vector<long long> combinedChildren;
        std::vector<long long> leftChildren = pageChildren(left);
        std::vector<long long> childChildren = pageChildren(child);
        combinedChildren.insert(combinedChildren.end(), leftChildren.begin(), leftChildren.end());
        combinedChildren.insert(combinedChildren.end(), childChildren.begin(), childChildren.end());

        std::vector<long long> newLeftChildren(combinedChildren.begin(), combinedChildren.begin() + newLeftCount + 1);
        std::vector<long long> newChildChildren(combinedChildren.begin() + newLeftCount + 1, combinedChildren.end());
        replaceChildren(left, newLeftChildren);
        replaceChildren(child, newChildChildren);
    }

    writePage(header, left);
    writePage(header, child);
    return true;
}

bool DiskBStarIndex::redistributeWithRightForInsert(HeaderData& header, ProtoBStarPage& parent, int childIndex)
{
    if (childIndex + 1 >= parent.child_page_ids_size())
    {
        return false;
    }

    ProtoBStarPage child = readPage(header, parent.child_page_ids(childIndex));
    ProtoBStarPage right = readPage(header, parent.child_page_ids(childIndex + 1));
    if (right.is_leaf() != child.is_leaf() || right.entries_size() >= kMaxEntries)
    {
        return false;
    }

    std::vector<ProtoIndexEntry> combined;
    std::vector<ProtoIndexEntry> childEntries = pageEntries(child);
    std::vector<ProtoIndexEntry> rightEntries = pageEntries(right);
    combined.insert(combined.end(), childEntries.begin(), childEntries.end());
    combined.push_back(parent.entries(childIndex));
    combined.insert(combined.end(), rightEntries.begin(), rightEntries.end());

    const int remaining = static_cast<int>(combined.size()) - 1;
    const int newChildCount = remaining / 2;
    const int separatorIndex = newChildCount;

    std::vector<ProtoIndexEntry> newChildEntries(combined.begin(), combined.begin() + newChildCount);
    std::vector<ProtoIndexEntry> newRightEntries(combined.begin() + separatorIndex + 1, combined.end());
    replaceEntries(child, newChildEntries);
    replaceEntries(right, newRightEntries);
    *parent.mutable_entries(childIndex) = combined[separatorIndex];

    if (!child.is_leaf())
    {
        std::vector<long long> combinedChildren;
        std::vector<long long> childChildren = pageChildren(child);
        std::vector<long long> rightChildren = pageChildren(right);
        combinedChildren.insert(combinedChildren.end(), childChildren.begin(), childChildren.end());
        combinedChildren.insert(combinedChildren.end(), rightChildren.begin(), rightChildren.end());

        std::vector<long long> newChildChildren(combinedChildren.begin(), combinedChildren.begin() + newChildCount + 1);
        std::vector<long long> newRightChildren(combinedChildren.begin() + newChildCount + 1, combinedChildren.end());
        replaceChildren(child, newChildChildren);
        replaceChildren(right, newRightChildren);
    }

    writePage(header, child);
    writePage(header, right);
    return true;
}

bool DiskBStarIndex::splitTwoToThreeWithLeft(HeaderData& header, ProtoBStarPage& parent, int childIndex)
{
    if (childIndex <= 0)
    {
        return false;
    }

    ProtoBStarPage left = readPage(header, parent.child_page_ids(childIndex - 1));
    ProtoBStarPage child = readPage(header, parent.child_page_ids(childIndex));
    if (left.is_leaf() != child.is_leaf() || left.entries_size() < kMaxEntries || child.entries_size() < kMaxEntries)
    {
        return false;
    }

    std::vector<ProtoIndexEntry> combined;
    std::vector<ProtoIndexEntry> leftEntries = pageEntries(left);
    std::vector<ProtoIndexEntry> childEntries = pageEntries(child);
    combined.insert(combined.end(), leftEntries.begin(), leftEntries.end());
    combined.push_back(parent.entries(childIndex - 1));
    combined.insert(combined.end(), childEntries.begin(), childEntries.end());

    const int total = static_cast<int>(combined.size());
    const int remaining = total - 2;
    const int leftCount = remaining / 3;
    const int middleCount = remaining / 3;
    const int firstSeparator = leftCount;
    const int secondSeparator = leftCount + 1 + middleCount;

    ProtoBStarPage middle = makePage(header, child.is_leaf());
    std::vector<ProtoIndexEntry> newLeftEntries(combined.begin(), combined.begin() + leftCount);
    std::vector<ProtoIndexEntry> middleEntries(combined.begin() + firstSeparator + 1, combined.begin() + secondSeparator);
    std::vector<ProtoIndexEntry> newChildEntries(combined.begin() + secondSeparator + 1, combined.end());

    replaceEntries(left, newLeftEntries);
    replaceEntries(middle, middleEntries);
    replaceEntries(child, newChildEntries);

    if (!child.is_leaf())
    {
        std::vector<long long> combinedChildren;
        std::vector<long long> leftChildren = pageChildren(left);
        std::vector<long long> childChildren = pageChildren(child);
        combinedChildren.insert(combinedChildren.end(), leftChildren.begin(), leftChildren.end());
        combinedChildren.insert(combinedChildren.end(), childChildren.begin(), childChildren.end());

        std::vector<long long> newLeftChildren(combinedChildren.begin(), combinedChildren.begin() + leftCount + 1);
        std::vector<long long> middleChildren(combinedChildren.begin() + leftCount + 1, combinedChildren.begin() + leftCount + 1 + middleCount + 1);
        std::vector<long long> newChildChildren(combinedChildren.begin() + leftCount + 1 + middleCount + 1, combinedChildren.end());
        replaceChildren(left, newLeftChildren);
        replaceChildren(middle, middleChildren);
        replaceChildren(child, newChildChildren);
    }

    std::vector<ProtoIndexEntry> parentEntries = pageEntries(parent);
    std::vector<long long> parentChildren = pageChildren(parent);
    parentEntries[childIndex - 1] = combined[firstSeparator];
    parentEntries.insert(parentEntries.begin() + childIndex, combined[secondSeparator]);
    parentChildren.insert(parentChildren.begin() + childIndex, middle.page_id());
    replaceEntries(parent, parentEntries);
    replaceChildren(parent, parentChildren);

    writePage(header, left);
    writePage(header, middle);
    writePage(header, child);
    return true;
}

bool DiskBStarIndex::splitTwoToThreeWithRight(HeaderData& header, ProtoBStarPage& parent, int childIndex)
{
    if (childIndex + 1 >= parent.child_page_ids_size())
    {
        return false;
    }

    ProtoBStarPage child = readPage(header, parent.child_page_ids(childIndex));
    ProtoBStarPage right = readPage(header, parent.child_page_ids(childIndex + 1));
    if (right.is_leaf() != child.is_leaf() || right.entries_size() < kMaxEntries || child.entries_size() < kMaxEntries)
    {
        return false;
    }

    std::vector<ProtoIndexEntry> combined;
    std::vector<ProtoIndexEntry> childEntries = pageEntries(child);
    std::vector<ProtoIndexEntry> rightEntries = pageEntries(right);
    combined.insert(combined.end(), childEntries.begin(), childEntries.end());
    combined.push_back(parent.entries(childIndex));
    combined.insert(combined.end(), rightEntries.begin(), rightEntries.end());

    const int total = static_cast<int>(combined.size());
    const int remaining = total - 2;
    const int leftCount = remaining / 3;
    const int middleCount = remaining / 3;
    const int firstSeparator = leftCount;
    const int secondSeparator = leftCount + 1 + middleCount;

    ProtoBStarPage middle = makePage(header, child.is_leaf());
    std::vector<ProtoIndexEntry> newChildEntries(combined.begin(), combined.begin() + leftCount);
    std::vector<ProtoIndexEntry> middleEntries(combined.begin() + firstSeparator + 1, combined.begin() + secondSeparator);
    std::vector<ProtoIndexEntry> newRightEntries(combined.begin() + secondSeparator + 1, combined.end());

    replaceEntries(child, newChildEntries);
    replaceEntries(middle, middleEntries);
    replaceEntries(right, newRightEntries);

    if (!child.is_leaf())
    {
        std::vector<long long> combinedChildren;
        std::vector<long long> childChildren = pageChildren(child);
        std::vector<long long> rightChildren = pageChildren(right);
        combinedChildren.insert(combinedChildren.end(), childChildren.begin(), childChildren.end());
        combinedChildren.insert(combinedChildren.end(), rightChildren.begin(), rightChildren.end());

        std::vector<long long> newChildChildren(combinedChildren.begin(), combinedChildren.begin() + leftCount + 1);
        std::vector<long long> middleChildren(combinedChildren.begin() + leftCount + 1, combinedChildren.begin() + leftCount + 1 + middleCount + 1);
        std::vector<long long> newRightChildren(combinedChildren.begin() + leftCount + 1 + middleCount + 1, combinedChildren.end());
        replaceChildren(child, newChildChildren);
        replaceChildren(middle, middleChildren);
        replaceChildren(right, newRightChildren);
    }

    std::vector<ProtoIndexEntry> parentEntries = pageEntries(parent);
    std::vector<long long> parentChildren = pageChildren(parent);
    parentEntries[childIndex] = combined[firstSeparator];
    parentEntries.insert(parentEntries.begin() + childIndex + 1, combined[secondSeparator]);
    parentChildren.insert(parentChildren.begin() + childIndex + 1, middle.page_id());
    replaceEntries(parent, parentEntries);
    replaceChildren(parent, parentChildren);

    writePage(header, child);
    writePage(header, middle);
    writePage(header, right);
    return true;
}

bool DiskBStarIndex::prepareChildForInsert(HeaderData& header, ProtoBStarPage& parent, int childIndex)
{
    ProtoBStarPage child = readPage(header, parent.child_page_ids(childIndex));
    compactLeafTombstones(child);
    writePage(header, child);

    if (child.entries_size() < kMaxEntries)
    {
        return false;
    }

    if (redistributeWithLeftForInsert(header, parent, childIndex)) return true;
    if (redistributeWithRightForInsert(header, parent, childIndex)) return true;
    if (splitTwoToThreeWithLeft(header, parent, childIndex)) return true;
    if (splitTwoToThreeWithRight(header, parent, childIndex)) return true;

    splitChild(header, parent, childIndex);
    return true;
}

void DiskBStarIndex::splitChild(HeaderData& header, ProtoBStarPage& parent, int childIndex)
{
    ProtoBStarPage child = readPage(header, parent.child_page_ids(childIndex));
    std::vector<ProtoIndexEntry> childEntries = pageEntries(child);
    std::vector<long long> childChildren = pageChildren(child);

    const int medianIndex = kMinDegree - 1;
    ProtoIndexEntry median = childEntries[medianIndex];

    ProtoBStarPage right = makePage(header, child.is_leaf());

    std::vector<ProtoIndexEntry> leftEntries(childEntries.begin(), childEntries.begin() + medianIndex);
    std::vector<ProtoIndexEntry> rightEntries(childEntries.begin() + medianIndex + 1, childEntries.end());
    replaceEntries(child, leftEntries);
    replaceEntries(right, rightEntries);

    if (!child.is_leaf())
    {
        std::vector<long long> leftChildren(childChildren.begin(), childChildren.begin() + kMinDegree);
        std::vector<long long> rightChildren(childChildren.begin() + kMinDegree, childChildren.end());
        replaceChildren(child, leftChildren);
        replaceChildren(right, rightChildren);
    }

    std::vector<ProtoIndexEntry> parentEntries = pageEntries(parent);
    std::vector<long long> parentChildren = pageChildren(parent);
    parentEntries.insert(parentEntries.begin() + childIndex, median);
    parentChildren.insert(parentChildren.begin() + childIndex + 1, right.page_id());
    replaceEntries(parent, parentEntries);
    replaceChildren(parent, parentChildren);

    writePage(header, child);
    writePage(header, right);
}

bool DiskBStarIndex::borrowFromLeftAfterErase(HeaderData& header, ProtoBStarPage& parent, int childIndex)
{
    if (childIndex <= 0)
    {
        return false;
    }

    ProtoBStarPage left = readPage(header, parent.child_page_ids(childIndex - 1));
    ProtoBStarPage child = readPage(header, parent.child_page_ids(childIndex));
    compactLeafTombstones(left);
    compactLeafTombstones(child);

    if (left.is_leaf() != child.is_leaf() || left.entries_size() <= kMinEntries)
    {
        return false;
    }

    std::vector<ProtoIndexEntry> combined;
    std::vector<ProtoIndexEntry> leftEntries = pageEntries(left);
    std::vector<ProtoIndexEntry> childEntries = pageEntries(child);
    combined.insert(combined.end(), leftEntries.begin(), leftEntries.end());
    combined.push_back(parent.entries(childIndex - 1));
    combined.insert(combined.end(), childEntries.begin(), childEntries.end());

    const int remaining = static_cast<int>(combined.size()) - 1;
    const int newLeftCount = remaining / 2;
    const int separatorIndex = newLeftCount;

    replaceEntries(left, std::vector<ProtoIndexEntry>(combined.begin(), combined.begin() + newLeftCount));
    replaceEntries(child, std::vector<ProtoIndexEntry>(combined.begin() + separatorIndex + 1, combined.end()));
    *parent.mutable_entries(childIndex - 1) = combined[separatorIndex];

    if (!child.is_leaf())
    {
        std::vector<long long> combinedChildren;
        std::vector<long long> leftChildren = pageChildren(left);
        std::vector<long long> childChildren = pageChildren(child);
        combinedChildren.insert(combinedChildren.end(), leftChildren.begin(), leftChildren.end());
        combinedChildren.insert(combinedChildren.end(), childChildren.begin(), childChildren.end());
        replaceChildren(left, std::vector<long long>(combinedChildren.begin(), combinedChildren.begin() + newLeftCount + 1));
        replaceChildren(child, std::vector<long long>(combinedChildren.begin() + newLeftCount + 1, combinedChildren.end()));
    }

    writePage(header, left);
    writePage(header, child);
    return true;
}

bool DiskBStarIndex::borrowFromRightAfterErase(HeaderData& header, ProtoBStarPage& parent, int childIndex)
{
    if (childIndex + 1 >= parent.child_page_ids_size())
    {
        return false;
    }

    ProtoBStarPage child = readPage(header, parent.child_page_ids(childIndex));
    ProtoBStarPage right = readPage(header, parent.child_page_ids(childIndex + 1));
    compactLeafTombstones(child);
    compactLeafTombstones(right);

    if (right.is_leaf() != child.is_leaf() || right.entries_size() <= kMinEntries)
    {
        return false;
    }

    std::vector<ProtoIndexEntry> combined;
    std::vector<ProtoIndexEntry> childEntries = pageEntries(child);
    std::vector<ProtoIndexEntry> rightEntries = pageEntries(right);
    combined.insert(combined.end(), childEntries.begin(), childEntries.end());
    combined.push_back(parent.entries(childIndex));
    combined.insert(combined.end(), rightEntries.begin(), rightEntries.end());

    const int remaining = static_cast<int>(combined.size()) - 1;
    const int newChildCount = remaining / 2;
    const int separatorIndex = newChildCount;

    replaceEntries(child, std::vector<ProtoIndexEntry>(combined.begin(), combined.begin() + newChildCount));
    replaceEntries(right, std::vector<ProtoIndexEntry>(combined.begin() + separatorIndex + 1, combined.end()));
    *parent.mutable_entries(childIndex) = combined[separatorIndex];

    if (!child.is_leaf())
    {
        std::vector<long long> combinedChildren;
        std::vector<long long> childChildren = pageChildren(child);
        std::vector<long long> rightChildren = pageChildren(right);
        combinedChildren.insert(combinedChildren.end(), childChildren.begin(), childChildren.end());
        combinedChildren.insert(combinedChildren.end(), rightChildren.begin(), rightChildren.end());
        replaceChildren(child, std::vector<long long>(combinedChildren.begin(), combinedChildren.begin() + newChildCount + 1));
        replaceChildren(right, std::vector<long long>(combinedChildren.begin() + newChildCount + 1, combinedChildren.end()));
    }

    writePage(header, child);
    writePage(header, right);
    return true;
}

bool DiskBStarIndex::mergeThreeToTwoAfterErase(HeaderData& header, ProtoBStarPage& parent, int childIndex)
{
    if (childIndex <= 0 || childIndex + 1 >= parent.child_page_ids_size())
    {
        return false;
    }

    ProtoBStarPage left = readPage(header, parent.child_page_ids(childIndex - 1));
    ProtoBStarPage middle = readPage(header, parent.child_page_ids(childIndex));
    ProtoBStarPage right = readPage(header, parent.child_page_ids(childIndex + 1));
    compactLeafTombstones(left);
    compactLeafTombstones(middle);
    compactLeafTombstones(right);

    if (left.is_leaf() != middle.is_leaf() || right.is_leaf() != middle.is_leaf())
    {
        return false;
    }

    std::vector<ProtoIndexEntry> combined;
    std::vector<ProtoIndexEntry> leftEntries = pageEntries(left);
    std::vector<ProtoIndexEntry> middleEntries = pageEntries(middle);
    std::vector<ProtoIndexEntry> rightEntries = pageEntries(right);
    combined.insert(combined.end(), leftEntries.begin(), leftEntries.end());
    combined.push_back(parent.entries(childIndex - 1));
    combined.insert(combined.end(), middleEntries.begin(), middleEntries.end());
    combined.push_back(parent.entries(childIndex));
    combined.insert(combined.end(), rightEntries.begin(), rightEntries.end());

    if (combined.size() > static_cast<std::size_t>(2 * kMaxEntries + 1))
    {
        return false;
    }

    const int remaining = static_cast<int>(combined.size()) - 1;
    const int newLeftCount = remaining / 2;
    const int separatorIndex = newLeftCount;

    replaceEntries(left, std::vector<ProtoIndexEntry>(combined.begin(), combined.begin() + newLeftCount));
    replaceEntries(right, std::vector<ProtoIndexEntry>(combined.begin() + separatorIndex + 1, combined.end()));

    if (!middle.is_leaf())
    {
        std::vector<long long> combinedChildren;
        std::vector<long long> leftChildren = pageChildren(left);
        std::vector<long long> middleChildren = pageChildren(middle);
        std::vector<long long> rightChildren = pageChildren(right);
        combinedChildren.insert(combinedChildren.end(), leftChildren.begin(), leftChildren.end());
        combinedChildren.insert(combinedChildren.end(), middleChildren.begin(), middleChildren.end());
        combinedChildren.insert(combinedChildren.end(), rightChildren.begin(), rightChildren.end());
        replaceChildren(left, std::vector<long long>(combinedChildren.begin(), combinedChildren.begin() + newLeftCount + 1));
        replaceChildren(right, std::vector<long long>(combinedChildren.begin() + newLeftCount + 1, combinedChildren.end()));
    }

    std::vector<ProtoIndexEntry> parentEntries = pageEntries(parent);
    std::vector<long long> parentChildren = pageChildren(parent);
    parentEntries[childIndex - 1] = combined[separatorIndex];
    parentEntries.erase(parentEntries.begin() + childIndex);
    parentChildren.erase(parentChildren.begin() + childIndex);
    replaceEntries(parent, parentEntries);
    replaceChildren(parent, parentChildren);

    writePage(header, left);
    writePage(header, right);
    freeOldPageSlot(header, middle.page_id());
    return true;
}

bool DiskBStarIndex::mergeTwoToOneAfterErase(HeaderData& header, ProtoBStarPage& parent, int childIndex)
{
    std::vector<ProtoIndexEntry> parentEntries = pageEntries(parent);
    std::vector<long long> parentChildren = pageChildren(parent);

    if (childIndex > 0)
    {
        ProtoBStarPage left = readPage(header, parent.child_page_ids(childIndex - 1));
        ProtoBStarPage child = readPage(header, parent.child_page_ids(childIndex));
        compactLeafTombstones(left);
        compactLeafTombstones(child);
        if (left.is_leaf() != child.is_leaf()) return false;

        std::vector<ProtoIndexEntry> combined;
        std::vector<ProtoIndexEntry> leftEntries = pageEntries(left);
        std::vector<ProtoIndexEntry> childEntries = pageEntries(child);
        combined.insert(combined.end(), leftEntries.begin(), leftEntries.end());
        combined.push_back(parent.entries(childIndex - 1));
        combined.insert(combined.end(), childEntries.begin(), childEntries.end());
        if (combined.size() > static_cast<std::size_t>(kMaxEntries)) return false;
        replaceEntries(left, combined);

        if (!child.is_leaf())
        {
            std::vector<long long> combinedChildren;
            std::vector<long long> leftChildren = pageChildren(left);
            std::vector<long long> childChildren = pageChildren(child);
            combinedChildren.insert(combinedChildren.end(), leftChildren.begin(), leftChildren.end());
            combinedChildren.insert(combinedChildren.end(), childChildren.begin(), childChildren.end());
            replaceChildren(left, combinedChildren);
        }

        parentEntries.erase(parentEntries.begin() + childIndex - 1);
        parentChildren.erase(parentChildren.begin() + childIndex);
        replaceEntries(parent, parentEntries);
        replaceChildren(parent, parentChildren);
        writePage(header, left);
        freeOldPageSlot(header, child.page_id());
        return true;
    }

    if (childIndex + 1 < parent.child_page_ids_size())
    {
        ProtoBStarPage child = readPage(header, parent.child_page_ids(childIndex));
        ProtoBStarPage right = readPage(header, parent.child_page_ids(childIndex + 1));
        compactLeafTombstones(child);
        compactLeafTombstones(right);
        if (child.is_leaf() != right.is_leaf()) return false;

        std::vector<ProtoIndexEntry> combined;
        std::vector<ProtoIndexEntry> childEntries = pageEntries(child);
        std::vector<ProtoIndexEntry> rightEntries = pageEntries(right);
        combined.insert(combined.end(), childEntries.begin(), childEntries.end());
        combined.push_back(parent.entries(childIndex));
        combined.insert(combined.end(), rightEntries.begin(), rightEntries.end());
        if (combined.size() > static_cast<std::size_t>(kMaxEntries)) return false;
        replaceEntries(child, combined);

        if (!child.is_leaf())
        {
            std::vector<long long> combinedChildren;
            std::vector<long long> childChildren = pageChildren(child);
            std::vector<long long> rightChildren = pageChildren(right);
            combinedChildren.insert(combinedChildren.end(), childChildren.begin(), childChildren.end());
            combinedChildren.insert(combinedChildren.end(), rightChildren.begin(), rightChildren.end());
            replaceChildren(child, combinedChildren);
        }

        parentEntries.erase(parentEntries.begin() + childIndex);
        parentChildren.erase(parentChildren.begin() + childIndex + 1);
        replaceEntries(parent, parentEntries);
        replaceChildren(parent, parentChildren);
        writePage(header, child);
        freeOldPageSlot(header, right.page_id());
        return true;
    }

    return false;
}

void DiskBStarIndex::repairChildAfterErase(HeaderData& header, ProtoBStarPage& parent, int childIndex)
{
    if (childIndex < 0 || childIndex >= parent.child_page_ids_size())
    {
        return;
    }

    ProtoBStarPage child = readPage(header, parent.child_page_ids(childIndex));
    compactLeafTombstones(child);
    writePage(header, child);
    if (child.entries_size() >= kMinEntries)
    {
        return;
    }

    if (borrowFromLeftAfterErase(header, parent, childIndex)) return;
    if (borrowFromRightAfterErase(header, parent, childIndex)) return;
    if (mergeThreeToTwoAfterErase(header, parent, childIndex)) return;
    mergeTwoToOneAfterErase(header, parent, childIndex);
}

void DiskBStarIndex::fixRootAfterErase(HeaderData& header)
{
    if (!header.rootExists)
    {
        return;
    }

    ProtoBStarPage root = readPage(header, header.rootPageId);
    compactLeafTombstones(root);

    if (root.is_leaf())
    {
        if (root.entries_size() == 0)
        {
            freeOldPageSlot(header, root.page_id());
            header.rootExists = false;
            header.rootPageId = -1;
            header.nextPageId = header.nextPageId;
        }
        else
        {
            writePage(header, root);
        }
        return;
    }

    if (root.entries_size() == 0 && root.child_page_ids_size() == 1)
    {
        const long long newRoot = root.child_page_ids(0);
        freeOldPageSlot(header, root.page_id());
        header.rootPageId = newRoot;
        header.rootExists = true;
        return;
    }

    writePage(header, root);
}

bool DiskBStarIndex::insertNonFull(HeaderData& header, long long pageId, const Value& key, std::streamoff offset)
{
    ProtoBStarPage page = readPage(header, pageId);
    int position = findEntryPosition(page, key);

    if (position < page.entries_size() && compareKeyWithEntry(key, page.entries(position)) == 0)
    {
        if (!page.entries(position).deleted())
        {
            return false;
        }
        *page.mutable_entries(position) = makeEntry(key, offset, false);
        writePage(header, page);
        return true;
    }

    if (page.is_leaf())
    {
        compactLeafTombstones(page);
        position = findEntryPosition(page, key);
        std::vector<ProtoIndexEntry> entries = pageEntries(page);
        entries.insert(entries.begin() + position, makeEntry(key, offset, false));
        replaceEntries(page, entries);
        writePage(header, page);
        return true;
    }

    prepareChildForInsert(header, page, position);
    writePage(header, page);
    page = readPage(header, pageId);
    position = findEntryPosition(page, key);

    if (position < page.entries_size() && compareKeyWithEntry(key, page.entries(position)) == 0)
    {
        if (!page.entries(position).deleted())
        {
            return false;
        }
        *page.mutable_entries(position) = makeEntry(key, offset, false);
        writePage(header, page);
        return true;
    }

    if (position >= page.child_page_ids_size())
    {
        throw std::runtime_error("повреждена структура файлового B*-индекса: нет child после балансировки");
    }

    return insertNonFull(header, page.child_page_ids(position), key, offset);
}

bool DiskBStarIndex::insert(const Value& key, std::streamoff offset)
{
    if (key.type == ValueType::Null || !valueHasColumnType(key, keyType_))
    {
        throw std::runtime_error("ключ не соответствует типу файлового B*-индекса");
    }

    HeaderData header = loadHeader();
    if (header.rootExists && contains(key))
    {
        return false;
    }

    if (!header.rootExists)
    {
        ProtoBStarPage root = makePage(header, true);
        *root.add_entries() = makeEntry(key, offset, false);
        writePage(header, root);
        header.rootExists = true;
        header.rootPageId = root.page_id();
        header.size = 1;
        saveHeader(header);
        return true;
    }

    ProtoBStarPage root = readPage(header, header.rootPageId);
    compactLeafTombstones(root);
    if (root.entries_size() >= kMaxEntries)
    {
        ProtoBStarPage newRoot = makePage(header, false);
        newRoot.add_child_page_ids(root.page_id());
        splitChild(header, newRoot, 0);
        writePage(header, newRoot);
        header.rootPageId = newRoot.page_id();
    }

    const bool inserted = insertNonFull(header, header.rootPageId, key, offset);
    if (inserted)
    {
        header.size += 1;
        saveHeader(header);
    }
    return inserted;
}

bool DiskBStarIndex::eraseInPage(HeaderData& header, long long pageId, const Value& key)
{
    ProtoBStarPage page = readPage(header, pageId);
    const int position = findEntryPosition(page, key);

    if (position < page.entries_size() && compareKeyWithEntry(key, page.entries(position)) == 0)
    {
        if (page.entries(position).deleted())
        {
            return false;
        }

        if (page.is_leaf())
        {
            std::vector<ProtoIndexEntry> entries = pageEntries(page);
            entries.erase(entries.begin() + position);
            replaceEntries(page, entries);
            writePage(header, page);
            return true;
        }

        ProtoIndexEntry entry = page.entries(position);
        entry.set_deleted(true);
        entry.set_offset(0);
        *page.mutable_entries(position) = entry;
        writePage(header, page);
        return true;
    }

    if (page.is_leaf())
    {
        return false;
    }

    if (position >= page.child_page_ids_size())
    {
        return false;
    }

    const bool erased = eraseInPage(header, page.child_page_ids(position), key);
    if (erased)
    {
        page = readPage(header, pageId);
        repairChildAfterErase(header, page, position);
        writePage(header, page);
    }
    return erased;
}


bool DiskBStarIndex::erase(const Value& key)
{
    HeaderData header = loadHeader();
    if (!header.rootExists)
    {
        return false;
    }

    const bool erased = eraseInPage(header, header.rootPageId, key);
    if (erased)
    {
        header.size -= 1;
        if (header.size < 0)
        {
            header.size = 0;
        }

        fixRootAfterErase(header);
        saveHeader(header);
    }
    return erased;
}


void DiskBStarIndex::collectInOrder(const HeaderData& header, long long pageId, std::vector<ProtoIndexEntry>& entries) const
{
    ProtoBStarPage page = readPage(header, pageId);

    if (page.is_leaf())
    {
        for (int index = 0; index < page.entries_size(); ++index)
        {
            if (!page.entries(index).deleted())
            {
                entries.push_back(page.entries(index));
            }
        }
        return;
    }

    for (int index = 0; index < page.entries_size(); ++index)
    {
        if (index < page.child_page_ids_size())
        {
            collectInOrder(header, page.child_page_ids(index), entries);
        }
        if (!page.entries(index).deleted())
        {
            entries.push_back(page.entries(index));
        }
    }

    if (page.child_page_ids_size() > page.entries_size())
    {
        collectInOrder(header, page.child_page_ids(page.entries_size()), entries);
    }
}

std::vector<std::streamoff> DiskBStarIndex::filterOffsets(
    const std::vector<ProtoIndexEntry>& entries,
    const Value& low,
    bool hasLow,
    const Value& high,
    bool hasHigh,
    bool includeLow,
    bool includeHigh
) const
{
    std::vector<std::streamoff> offsets;
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        Value key = entryKey(entries[index]);
        bool ok = true;
        if (hasLow)
        {
            int cmp = compareValues(key, low);
            ok = ok && (includeLow ? cmp >= 0 : cmp > 0);
        }
        if (hasHigh)
        {
            int cmp = compareValues(key, high);
            ok = ok && (includeHigh ? cmp <= 0 : cmp < 0);
        }
        if (ok)
        {
            offsets.push_back(static_cast<std::streamoff>(entries[index].offset()));
        }
    }
    return offsets;
}

std::vector<std::streamoff> DiskBStarIndex::lessThan(const Value& key, bool inclusive) const
{
    HeaderData header = loadHeader();
    std::vector<ProtoIndexEntry> entries;
    if (header.rootExists)
    {
        collectInOrder(header, header.rootPageId, entries);
    }
    Value dummy = makeNull();
    return filterOffsets(entries, dummy, false, key, true, false, inclusive);
}

std::vector<std::streamoff> DiskBStarIndex::greaterThan(const Value& key, bool inclusive) const
{
    HeaderData header = loadHeader();
    std::vector<ProtoIndexEntry> entries;
    if (header.rootExists)
    {
        collectInOrder(header, header.rootPageId, entries);
    }
    Value dummy = makeNull();
    return filterOffsets(entries, key, true, dummy, false, inclusive, false);
}

std::vector<std::streamoff> DiskBStarIndex::between(const Value& low, const Value& high) const
{
    HeaderData header = loadHeader();
    std::vector<ProtoIndexEntry> entries;
    if (header.rootExists)
    {
        collectInOrder(header, header.rootPageId, entries);
    }
    return filterOffsets(entries, low, true, high, true, true, true);
}

// Useless:
std::string DiskBStarIndex::exportTreeToProtoBytes() const
{
    HeaderData header = loadHeader();
    ProtoBStarDiskHeader proto;
    proto.set_root_exists(header.rootExists);
    proto.set_root_page_id(header.rootPageId);
    proto.set_next_page_id(header.nextPageId);
    proto.set_size(header.size);
    std::string bytes;
    proto.SerializeToString(&bytes);
    return bytes;
}

void DiskBStarIndex::importTreeFromProtoBytes(const std::string& bytes)
{
    (void)bytes;
}
