#include "SimpleLRU.h"
#include <iostream>

namespace Afina {
namespace Backend {

bool SimpleLRU::InsertHead(const std::string &key, const std::string &value) {
    if (key.size() + value.size() > this->_max_size) {
        return false;
    }

    while (this->_cur_size + key.size() + value.size() > this->_max_size) {
        this->RemoveTail();
    }

    lru_node *cur;
    // if the list is empty
    if (this->_lru_tail == nullptr) {
        cur = new lru_node{key, value, nullptr, std::unique_ptr<lru_node>()};
        this->_lru_head.reset(cur);
        this->_lru_tail = cur;
    } else {
        cur = new lru_node{key, value, nullptr, std::move(this->_lru_head)};
        this->_lru_head.reset(cur);
        this->_lru_head->next->prev = cur;
    }

    this->_cur_size += key.size() + value.size();
    this->_lru_index.insert(
        {std::reference_wrapper<const std::string>(cur->key), std::reference_wrapper<lru_node>(*cur)});

    return true;
}

void SimpleLRU::RemoveTail() {
    // for unforseen occurences
    if (this->_lru_tail == nullptr) {
        return;
    }

    this->_lru_index.erase(this->_lru_tail->key);

    // if there is only one element in the list
    if (this->_lru_tail->prev == nullptr) {
        this->_cur_size = 0;
        this->_lru_tail = nullptr;
        this->_lru_head.reset();
        return;
    }

    this->_cur_size -= this->_lru_tail->key.size() + this->_lru_tail->value.size();
    this->_lru_tail = this->_lru_tail->prev;
    this->_lru_tail->next.reset();
    return;
}

void SimpleLRU::MoveToHead(lru_node *node) {
    // if node is head
    if (node->prev == nullptr) {
        return;
    }

    if (node == this->_lru_tail) {
        this->_lru_tail = node->prev;
        node->next = std::move(this->_lru_head);
        this->_lru_head = std::move(node->prev->next);
        this->_lru_tail->next.reset();
        node->prev = nullptr;
        node->next->prev = node;
        return;
    }

    node->next->prev = node->prev;
    std::unique_ptr<lru_node> tmp = std::move(this->_lru_head);
    this->_lru_head = std::move(node->prev->next);
    node->prev->next = std::move(node->next);
    node->next = std::move(tmp);
    node->prev = nullptr;
    node->next->prev = node;
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Put(const std::string &key, const std::string &value) {
    auto found = this->_lru_index.find(key);

    // if elem not in cache
    if (found == this->_lru_index.end()) {
        return this->InsertHead(key, value);
    } else {
        found->second.get().value = value;
        this->MoveToHead(&(found->second.get()));
        return true;
    }
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::PutIfAbsent(const std::string &key, const std::string &value) {
    auto found = this->_lru_index.find(key);

    // if elem not in cache
    if (found == this->_lru_index.end()) {
        return this->InsertHead(key, value);
    } else {
        return false;
    }
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Set(const std::string &key, const std::string &value) {
    auto found = this->_lru_index.find(key);

    // if elem in cache
    if (found != this->_lru_index.end()) {
        found->second.get().value = value;
        this->MoveToHead(&(found->second.get()));
        return true;
    } else {
        return false;
    }
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Delete(const std::string &key) {
    auto found = this->_lru_index.find(key);

    // if elem in cache
    if (found != this->_lru_index.end()) {
        lru_node *node = &(found->second.get());

        this->_cur_size -= node->key.size() + node->value.size();

        if (node->prev == nullptr) {
            // if it is head
            this->_lru_head = std::move(node->next);
            this->_lru_head->prev = nullptr;
        } else if (node == this->_lru_tail) {
            // if it is tail
            this->_lru_tail = node->prev;
            this->_lru_tail->next.reset();
        } else {
            node->next->prev = node->prev;
            node->prev->next = std::move(node->next);
        }

        this->_lru_index.erase(found);

        return true;
    } else {
        return false;
    }
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Get(const std::string &key, std::string &value) {
    auto found = this->_lru_index.find(key);

    // if elem in cache
    if (found != this->_lru_index.end()) {
        value = found->second.get().value;
        this->MoveToHead(&(found->second.get()));
        return true;
    } else {
        return false;
    }
}

} // namespace Backend
} // namespace Afina
