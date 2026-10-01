#pragma once

#include "qcae/operation_ledger.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

namespace qcae {
// An immutable AVL tree for completed operation facts. Copies share both nodes and
// key/value payloads; insertion replaces only a bounded search path. Iterators retain
// the old root, so publishing a new root cannot invalidate a read of an old snapshot.
template <class T> class ImmutableFactMap {
  public:
    using value_type = std::pair<const std::string, T>;

  private:
    struct Node;
    using Link = std::shared_ptr<const Node>;
    struct Node {
        std::shared_ptr<const value_type> entry;
        Link left, right;
        int height;
    };
    Link root_;
    std::size_t size_{};
    static int height(const Link& node) noexcept {
        return node ? node->height : 0;
    }
    static Link node(std::shared_ptr<const value_type> entry, Link left, Link right) {
        const auto depth = 1 + std::max(height(left), height(right));
        auto result = std::make_shared<const Node>(
            Node{std::move(entry), std::move(left), std::move(right), depth});
        ledger::add(ledger::Stage::application, ledger::Metric::metadata_copy_bytes, sizeof(Node));
        return result;
    }
    static Link balance(Link current) {
        const auto difference = height(current->left) - height(current->right);
        if (difference > 1) {
            auto left = current->left;
            if (height(left->left) < height(left->right)) {
                const auto pivot = left->right;
                left = node(pivot->entry, node(left->entry, left->left, pivot->left), pivot->right);
            }
            return node(left->entry, left->left, node(current->entry, left->right, current->right));
        }
        if (difference < -1) {
            auto right = current->right;
            if (height(right->right) < height(right->left)) {
                const auto pivot = right->left;
                right =
                    node(pivot->entry, pivot->left, node(right->entry, pivot->right, right->right));
            }
            return node(
                right->entry, node(current->entry, current->left, right->left), right->right);
        }
        return current;
    }
    static Link insert(const Link& current, const std::shared_ptr<const value_type>& entry) {
        if (!current)
            return node(entry, {}, {});
        if (entry->first < current->entry->first)
            return balance(node(current->entry, insert(current->left, entry), current->right));
        if (current->entry->first < entry->first)
            return balance(node(current->entry, current->left, insert(current->right, entry)));
        return node(entry, current->left, current->right);
    }

  public:
    class const_iterator {
        Link root_;
        const Node* current_{};
        friend class ImmutableFactMap;
        const_iterator(Link root, const Node* current)
            : root_(std::move(root)), current_(current) {}

      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = ImmutableFactMap::value_type;
        using difference_type = std::ptrdiff_t;
        using pointer = const value_type*;
        using reference = const value_type&;
        const_iterator() = default;
        reference operator*() const {
            return *current_->entry;
        }
        pointer operator->() const {
            return current_->entry.get();
        }
        const_iterator& operator++() {
            const auto& key = current_->entry->first;
            const Node* next = nullptr;
            auto cursor = root_.get();
            while (cursor) {
                if (key < cursor->entry->first) {
                    next = cursor;
                    cursor = cursor->left.get();
                } else {
                    cursor = cursor->right.get();
                }
            }
            current_ = next;
            return *this;
        }
        const_iterator operator++(int) {
            auto previous = *this;
            ++*this;
            return previous;
        }
        bool operator==(const const_iterator& other) const noexcept {
            return current_ == other.current_;
        }
    };
    std::size_t size() const noexcept {
        return size_;
    }
    bool empty() const noexcept {
        return size_ == 0;
    }
    int depth() const noexcept {
        return height(root_);
    }
    const_iterator begin() const noexcept {
        auto current = root_.get();
        while (current && current->left)
            current = current->left.get();
        return {root_, current};
    }
    const_iterator end() const noexcept {
        return {root_, nullptr};
    }
    const_iterator find(const std::string& key) const noexcept {
        auto current = root_.get();
        while (current) {
            if (key < current->entry->first)
                current = current->left.get();
            else if (current->entry->first < key)
                current = current->right.get();
            else
                break;
        }
        return {root_, current};
    }
    std::pair<const_iterator, bool> emplace(std::string key, T value) {
        if (const auto existing = find(key); existing != end())
            return {existing, false};
        auto entry = std::make_shared<const value_type>(std::move(key), std::move(value));
        auto updated = insert(root_, entry);
        root_ = std::move(updated);
        ++size_;
        ledger::add(ledger::Stage::application,
                    ledger::Metric::metadata_copy_bytes,
                    sizeof(value_type) + entry->first.size());
        return {find(entry->first), true};
    }
    void replace(std::string key, T value) {
        const bool present = find(key) != end();
        auto entry = std::make_shared<const value_type>(std::move(key), std::move(value));
        auto updated = insert(root_, entry);
        root_ = std::move(updated);
        if (!present)
            ++size_;
        ledger::add(ledger::Stage::application,
                    ledger::Metric::metadata_copy_bytes,
                    sizeof(value_type) + entry->first.size());
    }
    void clear() noexcept {
        root_.reset();
        size_ = 0;
    }
};
} // namespace qcae
