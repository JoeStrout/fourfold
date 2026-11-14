// OrderedMap: a map from string keys that remembers the order keys were first
// added, as MiniScript 2 maps do. Datum's maps use it, so a map keeps its
// order on the way from a script into data, the session, a job, JSON or a
// template, and back.
//
// The interface is the part of std::map Fourfold uses (find, at, count,
// operator[], emplace, insert_or_assign, erase, iteration), so code written
// for a std::map reads the same. Iteration is in insertion order. Replacing a
// key's value keeps its place; erasing a key and adding it again puts it last.
//
// Equality ignores order, as MiniScript's does: two maps are equal if they
// have the same keys with equal values.
//
// Lookup is a linear scan for small maps and a hash index past
// kIndexThreshold entries. The index is built as entries are added, never
// lazily, so a const OrderedMap is safe to read from several threads.
#pragma once

#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fourfold {

template <typename V>
class OrderedMap {
public:
    using key_type = std::string;
    using mapped_type = V;
    using value_type = std::pair<std::string, V>;
    using Entries = std::vector<value_type>;
    using iterator = typename Entries::iterator;
    using const_iterator = typename Entries::const_iterator;
    using size_type = std::size_t;

    static constexpr size_t kIndexThreshold = 8;

    OrderedMap() = default;

    // Like std::map, a repeated key keeps its first value.
    OrderedMap(std::initializer_list<value_type> items) {
        for (const auto& item : items) emplace(item.first, item.second);
    }

    template <typename It>
    OrderedMap(It first, It last) {
        for (; first != last; ++first) emplace(first->first, first->second);
    }

    iterator begin() { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }
    const_iterator cbegin() const { return entries_.begin(); }
    const_iterator cend() const { return entries_.end(); }

    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    void clear() {
        entries_.clear();
        index_.clear();
    }

    iterator find(std::string_view key) {
        size_t i = position(key);
        return i == npos ? entries_.end() : entries_.begin() + static_cast<std::ptrdiff_t>(i);
    }
    const_iterator find(std::string_view key) const {
        size_t i = position(key);
        return i == npos ? entries_.end() : entries_.begin() + static_cast<std::ptrdiff_t>(i);
    }

    size_t count(std::string_view key) const { return position(key) == npos ? 0 : 1; }
    bool contains(std::string_view key) const { return position(key) != npos; }

    V& at(std::string_view key) {
        size_t i = position(key);
        if (i == npos) throw std::out_of_range("OrderedMap::at: no key " + std::string(key));
        return entries_[i].second;
    }
    const V& at(std::string_view key) const {
        size_t i = position(key);
        if (i == npos) throw std::out_of_range("OrderedMap::at: no key " + std::string(key));
        return entries_[i].second;
    }

    // Adds the key (with a default value, at the end) if it's missing.
    V& operator[](const std::string& key) {
        size_t i = position(key);
        if (i != npos) return entries_[i].second;
        append(key, V());
        return entries_.back().second;
    }

    // Adds the key at the end if it's missing; otherwise changes nothing.
    template <typename K, typename T>
    std::pair<iterator, bool> emplace(K&& key, T&& value) {
        std::string k(std::forward<K>(key));
        size_t i = position(k);
        if (i != npos) return {entries_.begin() + static_cast<std::ptrdiff_t>(i), false};
        append(std::move(k), V(std::forward<T>(value)));
        return {entries_.end() - 1, true};
    }

    std::pair<iterator, bool> insert(value_type item) {
        return emplace(std::move(item.first), std::move(item.second));
    }

    // Replaces the key's value in place, or adds it at the end.
    template <typename T>
    std::pair<iterator, bool> insert_or_assign(const std::string& key, T&& value) {
        size_t i = position(key);
        if (i != npos) {
            entries_[i].second = std::forward<T>(value);
            return {entries_.begin() + static_cast<std::ptrdiff_t>(i), false};
        }
        append(key, V(std::forward<T>(value)));
        return {entries_.end() - 1, true};
    }

    size_t erase(std::string_view key) {
        size_t i = position(key);
        if (i == npos) return 0;
        eraseAt(i);
        return 1;
    }

    iterator erase(const_iterator it) {
        size_t i = static_cast<size_t>(it - entries_.cbegin());
        eraseAt(i);
        return entries_.begin() + static_cast<std::ptrdiff_t>(i);
    }

    // Order-blind, as in MiniScript.
    bool operator==(const OrderedMap& other) const {
        if (size() != other.size()) return false;
        for (const auto& [key, value] : entries_) {
            size_t j = other.position(key);
            if (j == npos || !(other.entries_[j].second == value)) return false;
        }
        return true;
    }
    bool operator!=(const OrderedMap& other) const { return !(*this == other); }

private:
    static constexpr size_t npos = static_cast<size_t>(-1);

    size_t position(std::string_view key) const {
        if (!index_.empty()) {
            auto it = index_.find(std::string(key));
            return it == index_.end() ? npos : it->second;
        }
        for (size_t i = 0; i < entries_.size(); i++) {
            if (entries_[i].first == key) return i;
        }
        return npos;
    }

    void append(std::string key, V value) {
        entries_.emplace_back(std::move(key), std::move(value));
        if (!index_.empty()) {
            index_.emplace(entries_.back().first, entries_.size() - 1);
        } else if (entries_.size() > kIndexThreshold) {
            rebuildIndex();
        }
    }

    void eraseAt(size_t i) {
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
        if (entries_.size() > kIndexThreshold) {
            rebuildIndex();
        } else {
            index_.clear();
        }
    }

    void rebuildIndex() {
        index_.clear();
        index_.reserve(entries_.size());
        for (size_t i = 0; i < entries_.size(); i++) index_.emplace(entries_[i].first, i);
    }

    Entries entries_;
    std::unordered_map<std::string, size_t> index_;   // empty while small
};

}  // namespace fourfold
