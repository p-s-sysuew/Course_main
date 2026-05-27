#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


template <typename Key, typename Data, std::size_t T = 2, typename Compare = std::less<Key>>
class BStarIndex
{
private:
    struct Entry
    {
        Key key;
        Data data;
    };

    struct Node
    {
        bool isLeaf = true;
        std::vector<Entry> entries;
        std::vector<Node*> children;
    };

    struct erase_result
    {
        bool removed = false;
        bool underflow = false;
    };

    static constexpr std::size_t kMinKeys = 2 * T - 1;
    static constexpr std::size_t kMaxNonRootKeys = 3 * T - 1;
    static constexpr std::size_t kMaxRootKeys = 4 * T - 1;

public:
    BStarIndex() = default;
    BStarIndex(const BStarIndex&) = delete;
    BStarIndex& operator=(const BStarIndex&) = delete;

    ~BStarIndex()
    {
        clear();
    }

    void clear()
    {
        destroy(root_);
        root_ = nullptr;
        size_ = 0;
    }

    bool empty() const
    {
        return size_ == 0;
    }

    std::size_t size() const
    {
        return size_;
    }

    bool insert(const Key& key, const Data& data)
    {
        if (root_ == nullptr)
        {
            root_ = makeLeaf();
            root_->entries.push_back({key, data});
            size_ = 1;
            return true;
        }

        const bool inserted = insertIntoSubtree(root_, key, data);
        if (!inserted)
        {
            return false;
        }

        ++size_;
        splitRoot();
        return true;
    }

    bool erase(const Key& key)
    {
        if (root_ == nullptr)
        {
            return false;
        }

        const erase_result result = eraseFromSubtree(root_, key);
        if (!result.removed)
        {
            return false;
        }

        --size_;
        fixRoot();
        return true;
    }

    std::optional<Data> find(const Key& key) const
    {
        return findInSubtree(root_, key);
    }

    std::vector<Data> lessThan(const Key& key, bool inclusive) const
    {
        std::vector<Data> result;
        collectLessThan(root_, key, inclusive, result);
        return result;
    }

    std::vector<Data> greaterThan(const Key& key, bool inclusive) const
    {
        std::vector<Data> result;
        collectGreaterThan(root_, key, inclusive, result);
        return result;
    }

    std::vector<Data> between(const Key& low, const Key& high) const
    {
        std::vector<Data> result;
        collectBetween(root_, low, high, result);
        return result;
    }

    struct SerializedEntry
    {
        Key key;
        Data data;
    };

    struct SerializedNode
    {
        bool isLeaf = true;
        std::vector<SerializedEntry> entries;
        std::vector<SerializedNode> children;
    };

    bool exportTree(SerializedNode& outRoot) const
    {
        if (root_ == nullptr)
        {
            return false;
        }

        exportNode(root_, outRoot);
        return true;
    }

    void importTree(const SerializedNode& inputRoot)
    {
        clear();
        root_ = importNode(inputRoot);
        size_ = countEntries(root_);
    }
    
    // Added систему валидации just in case

    bool validateStructure(std::string* errorMessage = nullptr) const
    {
        if (root_ == nullptr)
        {
            return true;
        }

        int expectedLeafDepth = -1;
        std::optional<Key> minKey;
        std::optional<Key> maxKey;
        return validateNode(root_, true, 0, expectedLeafDepth, minKey, maxKey, errorMessage);
    }

private:
    Compare compare_;
    Node* root_ = nullptr;
    std::size_t size_ = 0;

    Node* makeLeaf() const
    {
        Node* node = new Node();
        node->isLeaf = true;
        return node;
    }

    Node* makeInternal() const
    {
        Node* node = new Node();
        node->isLeaf = false;
        return node;
    }

    void destroy(Node* node)
    {
        if (node == nullptr)
        {
            return;
        }

        for (Node* child : node->children)
        {
            destroy(child);
        }

        delete node;
    }

    bool keysEqual(const Key& left, const Key& right) const
    {
        return !compare_(left, right) && !compare_(right, left);
    }

    int compareKeys(const Key& left, const Key& right) const
    {
        if (compare_(left, right))
        {
            return -1;
        }
        if (compare_(right, left))
        {
            return 1;
        }
        return 0;
    }

    std::size_t maxKeysForNode(const Node* node) const noexcept
    {
        return node == root_ ? kMaxRootKeys : kMaxNonRootKeys;
    }

    std::size_t minKeysForNode(const Node* node) const noexcept
    {
        return node == root_ ? 1 : kMinKeys;
    }

    bool nodeOverflowed(const Node* node) const noexcept
    {
        return node != nullptr && node->entries.size() > maxKeysForNode(node);
    }

    bool nodeUnderflowed(const Node* node) const noexcept
    {
        return node != nullptr && node != root_ && node->entries.size() < kMinKeys;
    }

    std::size_t lowerBoundInNode(const Node* node, const Key& key) const
    {
        return static_cast<std::size_t>(std::lower_bound(node->entries.begin(), node->entries.end(), key, [this](const Entry& entry, const Key& value) { return compareKeys(entry.key, value) < 0; }) - node->entries.begin());
    }

    std::size_t childIndexInNode(const Node* node, const Key& key) const
    {
        return lowerBoundInNode(node, key);
    }

    Entry maxEntryInSubtree(const Node* node) const
    {
        if (node == nullptr || node->entries.empty())
        {
            throw std::runtime_error("Ошибка maxEntryInSubtree: поддерево пусто");
        }

        const Node* current = node;
        while (!current->isLeaf)
        {
            current = current->children.back();
        }

        return current->entries.back();
    }

    std::optional<Data> findInSubtree(const Node* node, const Key& key) const
    {
        if (node == nullptr)
        {
            return std::nullopt;
        }

        const std::size_t index = lowerBoundInNode(node, key);

        if (index < node->entries.size() && keysEqual(node->entries[index].key, key))
        {
            return node->entries[index].data;
        }

        if (node->isLeaf)
        {
            return std::nullopt;
        }

        return findInSubtree(node->children[index], key);
    }

    // Собирательство

    Node* importNode(const SerializedNode& inputNode)
    {
        Node* node = new Node();
        node->isLeaf = inputNode.isLeaf;

        for (const SerializedEntry& entry : inputNode.entries)
        {
            node->entries.push_back(Entry{entry.key, entry.data});
        }

        for (const SerializedNode& child : inputNode.children)
        {
            node->children.push_back(importNode(child));
        }

        return node;
    }

    std::size_t countEntries(const Node* node) const
    {
        if (node == nullptr)
        {
            return 0;
        }

        std::size_t result = node->entries.size();
        for (const Node* child : node->children)
        {
            result += countEntries(child);
        }
        return result;
    }

    void exportNode(const Node* node, SerializedNode& outNode) const
    {
        outNode.isLeaf = node->isLeaf;
        outNode.entries.clear();
        outNode.children.clear();

        for (const Entry& entry : node->entries)
        {
            outNode.entries.push_back(SerializedEntry{entry.key, entry.data});
        }

        for (const Node* child : node->children)
        {
            SerializedNode serializedChild;
            exportNode(child, serializedChild);
            outNode.children.push_back(serializedChild);
        }
    }

    void collectInOrder(const Node* node, std::vector<Entry>& result) const
    {
        if (node == nullptr)
        {
            return;
        }

        if (node->isLeaf)
        {
            result.insert(result.end(), node->entries.begin(), node->entries.end());
            return;
        }

        for (std::size_t index = 0; index < node->entries.size(); ++index)
        {
            collectInOrder(node->children[index], result);
            result.push_back(node->entries[index]);
        }

        collectInOrder(node->children.back(), result);
    }
    
    bool collectLessThan(const Node* node, const Key& key, bool inclusive, std::vector<Data>& result) const
    {
        if (node == nullptr)
        {
            return false;
        }

        if (node->isLeaf)
        {
            for (const Entry& entry : node->entries)
            {
                const int cmp = compareKeys(entry.key, key);
                if (cmp < 0 || (inclusive && cmp == 0))
                {
                    result.push_back(entry.data);
                }
                else
                {
                    return false;
                }
            }
            return true;
        }

        for (std::size_t index = 0; index < node->entries.size(); ++index)
        {
            if (!collectLessThan(node->children[index], key, inclusive, result))
            {
                return false;
            }

            const int cmp = compareKeys(node->entries[index].key, key);
            if (cmp < 0 || (inclusive && cmp == 0))
            {
                result.push_back(node->entries[index].data);
            }
            else
            {
                return false;
            }
        }

        return collectLessThan(node->children.back(), key, inclusive, result);
    }

    void collectGreaterThan(const Node* node, const Key& key, bool inclusive, std::vector<Data>& result) const
    {
        if (node == nullptr)
        {
            return;
        }

        if (node->isLeaf)
        {
            for (const Entry& entry : node->entries)
            {
                const int cmp = compareKeys(entry.key, key);
                if (cmp > 0 || (inclusive && cmp == 0))
                {
                    result.push_back(entry.data);
                }
            }
            return;
        }

        for (std::size_t index = 0; index < node->entries.size(); ++index)
        {
            collectGreaterThan(node->children[index], key, inclusive, result);

            const int cmp = compareKeys(node->entries[index].key, key);
            if (cmp > 0 || (inclusive && cmp == 0))
            {
                result.push_back(node->entries[index].data);
            }
        }

        collectGreaterThan(node->children.back(), key, inclusive, result);
    }

    bool collectBetween(const Node* node, const Key& low, const Key& high, std::vector<Data>& result) const
    {
        if (node == nullptr)
        {
            return false;
        }

        if (node->isLeaf)
        {
            for (const Entry& entry : node->entries)
            {
                if (compareKeys(entry.key, low) >= 0 && compareKeys(entry.key, high) < 0)
                {
                    result.push_back(entry.data);
                }
                else if (compareKeys(entry.key, high) >= 0)
                {
                    return false;
                }
            }
            return true;
        }

        for (std::size_t index = 0; index < node->entries.size(); ++index)
        {
            if (!collectBetween(node->children[index], low, high, result))
            {
                return false;
            }

            if (compareKeys(node->entries[index].key, low) >= 0 &&
                compareKeys(node->entries[index].key, high) < 0)
            {
                result.push_back(node->entries[index].data);
            }
            else if (compareKeys(node->entries[index].key, high) >= 0)
            {
                return false;
            }
        }

        return collectBetween(node->children.back(), low, high, result);
    }

    // Добавление

    bool insertIntoSubtree(Node* node, const Key& key, const Data& data)
    {
        const std::size_t index = lowerBoundInNode(node, key);

        if (index < node->entries.size() && keysEqual(node->entries[index].key, key))
        {
            return false;
        }

        if (node->isLeaf)
        {
            node->entries.insert(node->entries.begin() + static_cast<std::ptrdiff_t>(index), Entry{key, data});
            return true;
        }

        const std::size_t childIndex = childIndexInNode(node, key);
        const bool inserted = insertIntoSubtree(node->children[childIndex], key, data);
        if (!inserted)
        {
            return false;
        }

        if (nodeOverflowed(node->children[childIndex]))
        {
            fixChildOverflow(node, childIndex);
        }

        return true;
    }

    void fixChildOverflow(Node* parent, std::size_t childIndex)
    {
        if (!nodeOverflowed(parent->children[childIndex]))
        {
            return;
        }

        // В1 - перераспределение с левым соседом
        if (childIndex > 0 && parent->children[childIndex - 1]->entries.size() < kMaxNonRootKeys)
        {
            redistributeTwo(parent, childIndex - 1);
            return;
        }

        // В2 - перераспределение с правым соседом
        if (childIndex + 1 < parent->children.size() &&
            parent->children[childIndex + 1]->entries.size() < kMaxNonRootKeys)
        {
            redistributeTwo(parent, childIndex);
            return;
        }

        // В3 - 2 -> 3
        if (childIndex > 0)
        {
            twoThree(parent, childIndex - 1);
        }
        else
        {
            twoThree(parent, childIndex);
        }
    }

    void redistributeTwo(Node* parent, std::size_t leftChildIndex)
    {
        Node* left = parent->children[leftChildIndex];
        Node* right = parent->children[leftChildIndex + 1];

        std::vector<Entry> combinedEntries;
        combinedEntries.reserve(left->entries.size() + 1 + right->entries.size());

        combinedEntries.insert(combinedEntries.end(), left->entries.begin(), left->entries.end());
        combinedEntries.push_back(parent->entries[leftChildIndex]);
        combinedEntries.insert(combinedEntries.end(), right->entries.begin(), right->entries.end());

        const std::size_t separatorIndex = combinedEntries.size() / 2;

        left->entries.assign(combinedEntries.begin(), combinedEntries.begin() + static_cast<std::ptrdiff_t>(separatorIndex));
        parent->entries[leftChildIndex] = combinedEntries[separatorIndex];
        right->entries.assign(combinedEntries.begin() + static_cast<std::ptrdiff_t>(separatorIndex + 1), combinedEntries.end());

        if (!left->isLeaf)
        {
            std::vector<Node*> combinedChildren;
            combinedChildren.reserve(left->children.size() + right->children.size());
            combinedChildren.insert(combinedChildren.end(), left->children.begin(), left->children.end());
            combinedChildren.insert(combinedChildren.end(), right->children.begin(), right->children.end());

            const std::size_t leftChildrenCount = left->entries.size() + 1;

            left->children.assign(combinedChildren.begin(), combinedChildren.begin() + static_cast<std::ptrdiff_t>(leftChildrenCount));
            right->children.assign(combinedChildren.begin() + static_cast<std::ptrdiff_t>(leftChildrenCount), combinedChildren.end());
        }
    }

    void twoThree(Node* parent, std::size_t leftChildIndex)
    {
        Node* left = parent->children[leftChildIndex];
        Node* right = parent->children[leftChildIndex + 1];
        Node* middle = left->isLeaf ? makeLeaf() : makeInternal();

        std::vector<Entry> combinedEntries;
        combinedEntries.reserve(left->entries.size() + 1 + right->entries.size());
        combinedEntries.insert(combinedEntries.end(), left->entries.begin(), left->entries.end());
        combinedEntries.push_back(parent->entries[leftChildIndex]);
        combinedEntries.insert(combinedEntries.end(), right->entries.begin(), right->entries.end());

        const std::size_t firstSeparatorIndex = combinedEntries.size() / 3;
        const std::size_t secondSeparatorIndex = (2 * combinedEntries.size()) / 3;

        left->entries.assign(combinedEntries.begin(), combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstSeparatorIndex));

        middle->entries.assign(combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstSeparatorIndex + 1), combinedEntries.begin() + static_cast<std::ptrdiff_t>(secondSeparatorIndex));

        right->entries.assign(combinedEntries.begin() + static_cast<std::ptrdiff_t>(secondSeparatorIndex + 1), combinedEntries.end());

        parent->entries[leftChildIndex] = combinedEntries[firstSeparatorIndex];
        parent->entries.insert(parent->entries.begin() + static_cast<std::ptrdiff_t>(leftChildIndex + 1), combinedEntries[secondSeparatorIndex]);

        parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(leftChildIndex + 1), middle);

        if (!left->isLeaf)
        {
            std::vector<Node*> combinedChildren;
            combinedChildren.reserve(left->children.size() + right->children.size());
            combinedChildren.insert(combinedChildren.end(), left->children.begin(), left->children.end());
            combinedChildren.insert(combinedChildren.end(), right->children.begin(), right->children.end());

            const std::size_t leftChildrenCount = left->entries.size() + 1;
            const std::size_t middleChildrenCount = middle->entries.size() + 1;

            left->children.assign(combinedChildren.begin(), combinedChildren.begin() + static_cast<std::ptrdiff_t>(leftChildrenCount));
            middle->children.assign(combinedChildren.begin() + static_cast<std::ptrdiff_t>(leftChildrenCount), combinedChildren.begin() + static_cast<std::ptrdiff_t>(leftChildrenCount + middleChildrenCount));
            right->children.assign(combinedChildren.begin() + static_cast<std::ptrdiff_t>(leftChildrenCount + middleChildrenCount), combinedChildren.end());
        }
    }

    void splitRoot()
    {
        if (!nodeOverflowed(root_))
        {
            return;
        }

        Node* oldRoot = root_;
        Node* right = oldRoot->isLeaf ? makeLeaf() : makeInternal();
        Node* newRoot = makeInternal();

        const std::size_t middleIndex = oldRoot->entries.size() / 2;
        const Entry promoted = oldRoot->entries[middleIndex];

        right->entries.assign(oldRoot->entries.begin() + static_cast<std::ptrdiff_t>(middleIndex + 1), oldRoot->entries.end());

        oldRoot->entries.erase(oldRoot->entries.begin() + static_cast<std::ptrdiff_t>(middleIndex), oldRoot->entries.end());

        if (!oldRoot->isLeaf)
        {
            right->children.assign(oldRoot->children.begin() + static_cast<std::ptrdiff_t>(middleIndex + 1), oldRoot->children.end());

            oldRoot->children.erase(oldRoot->children.begin() + static_cast<std::ptrdiff_t>(middleIndex + 1), oldRoot->children.end());
        }

        newRoot->isLeaf = false;
        newRoot->entries.push_back(promoted);
        newRoot->children.push_back(oldRoot);
        newRoot->children.push_back(right);
        root_ = newRoot;
    }

    // Удаление

    erase_result eraseFromSubtree(Node*& node, const Key& key)
    {
        if (node == nullptr)
        {
            return {false, false};
        }

        const std::size_t index = lowerBoundInNode(node, key);

        // В1 - лист
        if (node->isLeaf)
        {
            if (index >= node->entries.size() || !keysEqual(node->entries[index].key, key))
            {
                return {false, false};
            }

            node->entries.erase(node->entries.begin() + static_cast<std::ptrdiff_t>(index));
            return {true, nodeUnderflowed(node)};
        }

        // В2 - внутренний узел
        if (index < node->entries.size() && keysEqual(node->entries[index].key, key))
        {
            const Entry newRoot = maxEntryInSubtree(node->children[index]);
            node->entries[index] = newRoot;

            erase_result childResult = eraseFromSubtree(node->children[index], newRoot.key);
            if (childResult.underflow)
            {
                fixChildUnderflow(node, index);
            }

            return {true, nodeUnderflowed(node)};
        }

        // В3 - ключа нет, спускаемся в ребёнка
        const std::size_t childIndex = childIndexInNode(node, key);
        erase_result childResult = eraseFromSubtree(node->children[childIndex], key);
        if (!childResult.removed)
        {
            return {false, false};
        }

        if (childResult.underflow)
        {
            fixChildUnderflow(node, childIndex);
        }

        return {true, nodeUnderflowed(node)};
    }

    void fixChildUnderflow(Node* parent, std::size_t childIndex)
    {
        Node* child = parent->children[childIndex];
        if (!nodeUnderflowed(child))
        {
            return;
        }

        if (child->isLeaf)
        {
            if (childIndex > 0 && parent->children[childIndex - 1]->entries.size() > kMinKeys)
            {
                borrowFromLeftLeaf(parent, childIndex);
                return;
            }

            if (childIndex + 1 < parent->children.size() &&
                parent->children[childIndex + 1]->entries.size() > kMinKeys)
            {
                borrowFromRightLeaf(parent, childIndex);
                return;
            }

            if (tryRedistributeLeafs(parent, childIndex))
            {
                return;
            }

            if (parent == root_ && parent->children.size() == 2)
            {
                TwoOneLeafs(parent, 0);
                return;
            }

            if (childIndex > 0 && childIndex + 1 < parent->children.size())
            {
                ThreeTwoLeafs(parent, childIndex - 1);
                return;
            }

            if (childIndex + 2 < parent->children.size())
            {
                ThreeTwoLeafs(parent, childIndex);
                return;
            }

            if (childIndex >= 2)
            {
                ThreeTwoLeafs(parent, childIndex - 2);
                return;
            }

            if (childIndex > 0)
            {
                TwoOneLeafs(parent, childIndex - 1);
            }
            else if (childIndex + 1 < parent->children.size())
            {
                TwoOneLeafs(parent, childIndex);
            }
            return;
        }

        if (childIndex > 0 && parent->children[childIndex - 1]->entries.size() > kMinKeys)
        {
            borrowFromLeftMiddle(parent, childIndex);
            return;
        }

        if (childIndex + 1 < parent->children.size() &&
            parent->children[childIndex + 1]->entries.size() > kMinKeys)
        {
            borrowFromRightMiddle(parent, childIndex);
            return;
        }

        if (tryRedistributeMiddle(parent, childIndex))
        {
            return;
        }

        if (parent == root_ && parent->children.size() == 2)
        {
            TwoOneMiddle(parent, 0);
            return;
        }

        if (childIndex > 0 && childIndex + 1 < parent->children.size())
        {
            ThreeTwoMiddle(parent, childIndex - 1);
            return;
        }

        if (childIndex + 2 < parent->children.size())
        {
            ThreeTwoMiddle(parent, childIndex);
            return;
        }

        if (childIndex >= 2)
        {
            ThreeTwoMiddle(parent, childIndex - 2);
            return;
        }

        if (childIndex > 0)
        {
            TwoOneMiddle(parent, childIndex - 1);
        }
        else if (childIndex + 1 < parent->children.size())
        {
            TwoOneMiddle(parent, childIndex);
        }
    }

    void borrowFromLeftLeaf(Node* parent, std::size_t childIndex)
    {
        Node* left = parent->children[childIndex - 1];
        Node* child = parent->children[childIndex];

        child->entries.insert(child->entries.begin(), parent->entries[childIndex - 1]);
        parent->entries[childIndex - 1] = left->entries.back();
        left->entries.pop_back();
    }

    void borrowFromRightLeaf(Node* parent, std::size_t childIndex)
    {
        Node* child = parent->children[childIndex];
        Node* right = parent->children[childIndex + 1];

        child->entries.push_back(parent->entries[childIndex]);
        parent->entries[childIndex] = right->entries.front();
        right->entries.erase(right->entries.begin());
    }

    void borrowFromLeftMiddle(Node* parent, std::size_t childIndex)
    {
        Node* left = parent->children[childIndex - 1];
        Node* child = parent->children[childIndex];

        child->entries.insert(child->entries.begin(), parent->entries[childIndex - 1]);
        parent->entries[childIndex - 1] = left->entries.back();
        left->entries.pop_back();

        child->children.insert(child->children.begin(), left->children.back());
        left->children.pop_back();
    }

    void borrowFromRightMiddle(Node* parent, std::size_t childIndex)
    {
        Node* child = parent->children[childIndex];
        Node* right = parent->children[childIndex + 1];

        child->entries.push_back(parent->entries[childIndex]);
        parent->entries[childIndex] = right->entries.front();
        right->entries.erase(right->entries.begin());

        child->children.push_back(right->children.front());
        right->children.erase(right->children.begin());
    }
    
    bool calculateAmountThree(std::size_t totalChildEntries, std::size_t& first, std::size_t& second, std::size_t& third) const
    {
        for (std::size_t a = kMinKeys; a <= kMaxNonRootKeys; ++a)
        {
            for (std::size_t b = kMinKeys; b <= kMaxNonRootKeys; ++b)
            {
                if (a + b > totalChildEntries)
                {
                    break;
                }

                const std::size_t c = totalChildEntries - a - b;
                if (c >= kMinKeys && c <= kMaxNonRootKeys)
                {
                    first = a;
                    second = b;
                    third = c;
                    return true;
                }
            }
        }
        return false;
    }

    bool calculateAmountTwo(std::size_t totalChildEntries, std::size_t& first, std::size_t& second) const
    {
        for (std::size_t a = kMinKeys; a <= kMaxNonRootKeys; ++a)
        {
            if (a > totalChildEntries)
            {
                break;
            }

            const std::size_t b = totalChildEntries - a;
            if (b >= kMinKeys && b <= kMaxNonRootKeys)
            {
                first = a;
                second = b;
                return true;
            }
        }
        return false;
    }

    bool RedistributeLeafs(Node* parent, std::size_t leftIndex)
    {
        Node* first = parent->children[leftIndex];
        Node* second = parent->children[leftIndex + 1];
        Node* third = parent->children[leftIndex + 2];

        std::vector<Entry> combinedEntries;
        combinedEntries.reserve(first->entries.size() + second->entries.size() + third->entries.size() + 2);
        combinedEntries.insert(combinedEntries.end(), first->entries.begin(), first->entries.end());
        combinedEntries.push_back(parent->entries[leftIndex]);
        combinedEntries.insert(combinedEntries.end(), second->entries.begin(), second->entries.end());
        combinedEntries.push_back(parent->entries[leftIndex + 1]);
        combinedEntries.insert(combinedEntries.end(), third->entries.begin(), third->entries.end());

        const std::size_t totalChildEntries = combinedEntries.size() - 2;
        std::size_t firstCount = 0;
        std::size_t secondCount = 0;
        std::size_t thirdCount = 0;
        if (!calculateAmountThree(totalChildEntries, firstCount, secondCount, thirdCount))
        {
            return false;
        }

        first->entries.assign(combinedEntries.begin(), combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstCount));
        parent->entries[leftIndex] = combinedEntries[firstCount];
        second->entries.assign(combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstCount + 1), combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstCount + 1 + secondCount));
        parent->entries[leftIndex + 1] = combinedEntries[firstCount + 1 + secondCount];
        third->entries.assign(combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstCount + 1 + secondCount + 1), combinedEntries.end());

        return true;
    }

    bool RedistributeMiddle(Node* parent, std::size_t leftIndex)
    {
        Node* first = parent->children[leftIndex];
        Node* second = parent->children[leftIndex + 1];
        Node* third = parent->children[leftIndex + 2];

        std::vector<Entry> combinedEntries;
        combinedEntries.reserve(first->entries.size() + second->entries.size() + third->entries.size() + 2);
        combinedEntries.insert(combinedEntries.end(), first->entries.begin(), first->entries.end());
        combinedEntries.push_back(parent->entries[leftIndex]);
        combinedEntries.insert(combinedEntries.end(), second->entries.begin(), second->entries.end());
        combinedEntries.push_back(parent->entries[leftIndex + 1]);
        combinedEntries.insert(combinedEntries.end(), third->entries.begin(), third->entries.end());

        const std::size_t totalChildEntries = combinedEntries.size() - 2;
        std::size_t firstCount = 0;
        std::size_t secondCount = 0;
        std::size_t thirdCount = 0;
        if (!calculateAmountThree(totalChildEntries, firstCount, secondCount, thirdCount))
        {
            return false;
        }

        first->entries.assign(combinedEntries.begin(), combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstCount));
        parent->entries[leftIndex] = combinedEntries[firstCount];
        second->entries.assign(combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstCount + 1), combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstCount + 1 + secondCount));
        parent->entries[leftIndex + 1] = combinedEntries[firstCount + 1 + secondCount];
        third->entries.assign(combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstCount + 1 + secondCount + 1), combinedEntries.end());

        std::vector<Node*> combinedChildren;
        combinedChildren.reserve(first->children.size() + second->children.size() + third->children.size());
        combinedChildren.insert(combinedChildren.end(), first->children.begin(), first->children.end());
        combinedChildren.insert(combinedChildren.end(), second->children.begin(), second->children.end());
        combinedChildren.insert(combinedChildren.end(), third->children.begin(), third->children.end());

        const std::size_t firstChildrenCount = firstCount + 1;
        const std::size_t secondChildrenCount = secondCount + 1;

        first->children.assign(combinedChildren.begin(), combinedChildren.begin() + static_cast<std::ptrdiff_t>(firstChildrenCount));
        second->children.assign(combinedChildren.begin() + static_cast<std::ptrdiff_t>(firstChildrenCount), combinedChildren.begin() + static_cast<std::ptrdiff_t>(firstChildrenCount + secondChildrenCount));
        third->children.assign(combinedChildren.begin() + static_cast<std::ptrdiff_t>(firstChildrenCount + secondChildrenCount), combinedChildren.end());
        return true;
    }

    bool tryRedistributeLeafs(Node* parent, std::size_t childIndex)
    {
        if (childIndex > 0 && childIndex + 1 < parent->children.size() && RedistributeLeafs(parent, childIndex - 1))
        {
            return true;
        }
        if (childIndex + 2 < parent->children.size() && RedistributeLeafs(parent, childIndex))
        {
            return true;
        }
        if (childIndex >= 2 && RedistributeLeafs(parent, childIndex - 2))
        {
            return true;
        }
        return false;
    }

    bool tryRedistributeMiddle(Node* parent, std::size_t childIndex)
    {
        if (childIndex > 0 && childIndex + 1 < parent->children.size() && RedistributeMiddle(parent, childIndex - 1))
        {
            return true;
        }
        if (childIndex + 2 < parent->children.size() && RedistributeMiddle(parent, childIndex))
        {
            return true;
        }
        if (childIndex >= 2 && RedistributeMiddle(parent, childIndex - 2))
        {
            return true;
        }
        return false;
    }

    void ThreeTwoLeafs(Node* parent, std::size_t leftIndex)
    {
        Node* first = parent->children[leftIndex];
        Node* second = parent->children[leftIndex + 1];
        Node* third = parent->children[leftIndex + 2];

        std::vector<Entry> combinedEntries;
        combinedEntries.reserve(
            first->entries.size() + second->entries.size() + third->entries.size() + 2
        );

        combinedEntries.insert(combinedEntries.end(), first->entries.begin(), first->entries.end());
        combinedEntries.push_back(parent->entries[leftIndex]);
        combinedEntries.insert(combinedEntries.end(), second->entries.begin(), second->entries.end());
        combinedEntries.push_back(parent->entries[leftIndex + 1]);
        combinedEntries.insert(combinedEntries.end(), third->entries.begin(), third->entries.end());

        const std::size_t totalChildEntries = combinedEntries.size() - 1;
        std::size_t firstCount = 0;
        std::size_t secondCount = 0;
        if (!calculateAmountTwo(totalChildEntries, firstCount, secondCount))
        {
            throw std::runtime_error("Ошибка ThreeTwoLeafs: распределение невозможно");
        }

        first->entries.assign(combinedEntries.begin(), combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstCount));
        parent->entries[leftIndex] = combinedEntries[firstCount];
        second->entries.assign(combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstCount + 1), combinedEntries.end());

        delete third;
        parent->entries.erase(parent->entries.begin() + static_cast<std::ptrdiff_t>(leftIndex + 1));
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(leftIndex + 2));
    }

    void ThreeTwoMiddle(Node* parent, std::size_t leftIndex)
    {
        Node* first = parent->children[leftIndex];
        Node* second = parent->children[leftIndex + 1];
        Node* third = parent->children[leftIndex + 2];

        std::vector<Entry> combinedEntries;
        combinedEntries.reserve(
            first->entries.size() + second->entries.size() + third->entries.size() + 2
        );

        combinedEntries.insert(combinedEntries.end(), first->entries.begin(), first->entries.end());
        combinedEntries.push_back(parent->entries[leftIndex]);
        combinedEntries.insert(combinedEntries.end(), second->entries.begin(), second->entries.end());
        combinedEntries.push_back(parent->entries[leftIndex + 1]);
        combinedEntries.insert(combinedEntries.end(), third->entries.begin(), third->entries.end());

        const std::size_t totalChildEntries = combinedEntries.size() - 1;
        std::size_t firstCount = 0;
        std::size_t secondCount = 0;
        if (!calculateAmountTwo(totalChildEntries, firstCount, secondCount))
        {
            throw std::runtime_error("Ошибка ThreeTwoMiddle: распределение невозможно");
        }

        first->entries.assign(combinedEntries.begin(), combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstCount));
        parent->entries[leftIndex] = combinedEntries[firstCount];
        second->entries.assign(combinedEntries.begin() + static_cast<std::ptrdiff_t>(firstCount + 1), combinedEntries.end());

        std::vector<Node*> combinedChildren;
        combinedChildren.reserve(first->children.size() + second->children.size() + third->children.size());
        combinedChildren.insert(combinedChildren.end(), first->children.begin(), first->children.end());
        combinedChildren.insert(combinedChildren.end(), second->children.begin(), second->children.end());
        combinedChildren.insert(combinedChildren.end(), third->children.begin(), third->children.end());

        const std::size_t firstChildrenCount = firstCount + 1;

        first->children.assign(combinedChildren.begin(), combinedChildren.begin() + static_cast<std::ptrdiff_t>(firstChildrenCount));
        second->children.assign(combinedChildren.begin() + static_cast<std::ptrdiff_t>(firstChildrenCount), combinedChildren.end());

        delete third;
        parent->entries.erase(parent->entries.begin() + static_cast<std::ptrdiff_t>(leftIndex + 1));
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(leftIndex + 2));
    }

    void TwoOneLeafs(Node* parent, std::size_t leftIndex)
    {
        Node* left = parent->children[leftIndex];
        Node* right = parent->children[leftIndex + 1];

        left->entries.push_back(parent->entries[leftIndex]);
        left->entries.insert(left->entries.end(), right->entries.begin(), right->entries.end());

        delete right;
        parent->entries.erase(parent->entries.begin() + static_cast<std::ptrdiff_t>(leftIndex));
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(leftIndex + 1));
    }

    void TwoOneMiddle(Node* parent, std::size_t leftIndex)
    {
        Node* left = parent->children[leftIndex];
        Node* right = parent->children[leftIndex + 1];

        left->entries.push_back(parent->entries[leftIndex]);
        left->entries.insert(left->entries.end(), right->entries.begin(), right->entries.end());
        left->children.insert(left->children.end(), right->children.begin(), right->children.end());

        delete right;
        parent->entries.erase(parent->entries.begin() + static_cast<std::ptrdiff_t>(leftIndex));
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(leftIndex + 1));
    }

    void fixRoot()
    {
        if (root_ == nullptr)
        {
            return;
        }

        if (root_->isLeaf)
        {
            if (root_->entries.empty())
            {
                delete root_;
                root_ = nullptr;
            }
            return;
        }

        if (root_->entries.empty() && root_->children.size() == 1)
        {
            Node* oldRoot = root_;
            root_ = root_->children.front();
            oldRoot->children.clear();
            delete oldRoot;
            return;
        }
    }

    void validationTextHelper(std::string* errorMessage, const std::string& text) const
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = text;
        }
    }

    bool validateNode(const Node* node, bool isRoot, int depth, int& expectedLeafDepth, std::optional<Key>& outMinKey, std::optional<Key>& outMaxKey, std::string* errorMessage) const
    {
        if (node == nullptr)
        {
            validationTextHelper(errorMessage, "В дереве вместо узла встречен nullptr");
            return false;
        }

        if (!isRoot && node->entries.size() < kMinKeys)
        {
            validationTextHelper(errorMessage, "Неверное число узлов (меньше ожидаемого)");
            return false;
        }

        const std::size_t maxKeys = isRoot ? kMaxRootKeys : kMaxNonRootKeys;
        if (node->entries.size() > maxKeys)
        {
            validationTextHelper(errorMessage, "Неверное число узлов (больше ожидаемого)");
            return false;
        }

        for (std::size_t index = 1; index < node->entries.size(); ++index)
        {
            if (!compare_(node->entries[index - 1].key, node->entries[index].key))
            {
                validationTextHelper(errorMessage, "Ключи внутри узла не отсортированы верно");
                return false;
            }
        }

        if (node->isLeaf)
        {
            if (!node->children.empty())
            {
                validationTextHelper(errorMessage, "Дети у листа?");
                return false;
            }

            if (expectedLeafDepth == -1)
            {
                expectedLeafDepth = depth;
            }
            else if (expectedLeafDepth != depth)
            {
                validationTextHelper(errorMessage, "Листья на разном уровне");
                return false;
            }

            if (!node->entries.empty())
            {
                outMinKey = node->entries.front().key;
                outMaxKey = node->entries.back().key;
            }
            else
            {
                outMinKey.reset();
                outMaxKey.reset();
            }

            return true;
        }

        if (node->children.size() != node->entries.size() + 1)
        {
            validationTextHelper(errorMessage, "Неверное число детей");
            return false;
        }

        std::optional<Key> subtreeMin;
        std::optional<Key> subtreeMax;

        for (std::size_t childIndex = 0; childIndex < node->children.size(); ++childIndex)
        {
            std::optional<Key> childMin;
            std::optional<Key> childMax;

            if (!validateNode(node->children[childIndex], false, depth + 1, expectedLeafDepth, childMin, childMax, errorMessage))
            {
                return false;
            }

            if (!childMin.has_value() || !childMax.has_value())
            {
                validationTextHelper(errorMessage, "Ребёнок пуст");
                return false;
            }

            if (childIndex == 0)
            {
                if (!compare_(childMax.value(), node->entries[0].key))
                {
                    validationTextHelper(errorMessage, "Левый ребёнок не меньше сепаратора");
                    return false;
                }
            }
            else if (childIndex == node->children.size() - 1)
            {
                if (!compare_(node->entries.back().key, childMin.value()))
                {
                    validationTextHelper(errorMessage, "Правый ребёнок не больше сепаратора");
                    return false;
                }
            }
            else
            {
                if (!compare_(node->entries[childIndex - 1].key, childMin.value()))
                {
                    validationTextHelper(errorMessage, "Внутренний узел не больше левого соседа");
                    return false;
                }
                if (!compare_(childMax.value(), node->entries[childIndex].key))
                {
                    validationTextHelper(errorMessage, "Внутренний узел не меньше правого соседа");
                    return false;
                }
            }

            if (!subtreeMin.has_value() || compare_(childMin.value(), subtreeMin.value()))
            {
                subtreeMin = childMin;
            }
            if (!subtreeMax.has_value() || compare_(subtreeMax.value(), childMax.value()))
            {
                subtreeMax = childMax;
            }
        }

        if (!subtreeMin.has_value() || !subtreeMax.has_value())
        {
            validationTextHelper(errorMessage, "Некорректный диапазон поддерева");
            return false;
        }

        outMinKey = subtreeMin;
        outMaxKey = subtreeMax;
        return true;
    }
};