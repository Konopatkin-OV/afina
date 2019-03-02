#include "SimpleLRU.h"
#include <iostream>

namespace Afina {
namespace Backend {


bool SimpleLRU::InsertHead (const std::string &key, const std::string &value) {
	if (key.size() + value.size() > this->_max_size) {
		return false;
	}

	std::cout << "InsertHead: start" << std::endl;
	while (this->_cur_size + key.size() + value.size() > this->_max_size) {
		this->RemoveTail();
		std::cout << "InsertHead: removed one tail" << std::endl;
	}
	std::cout << "InsertHead: removed last" << std::endl;

	lru_node* cur;
	// if the list is empty
	if (this->_lru_tail == nullptr) {
		std::cout << "InsertHead: list is empty" << std::endl;
		cur = new lru_node{key, value, nullptr, std::unique_ptr<lru_node>()};
		this->_lru_head = std::unique_ptr<lru_node>(cur);
		this->_lru_tail = cur;
	} else {
		std::cout << "InsertHead: list is not empty" << std::endl;
		cur = new lru_node{key, value, nullptr, std::move(this->_lru_head)};
		this->_lru_head = std::unique_ptr<lru_node>(cur);
		this->_lru_head->next->prev = cur;
	}

	this->_cur_size += key.size() + value.size();
	this->_lru_index.insert({std::reference_wrapper<const std::string>(key), std::reference_wrapper<lru_node>(*cur)});
	return true;
}

void SimpleLRU::RemoveTail () {
	// for unforseen occurences
	if (this->_lru_tail == nullptr) {
		return;
	}
	std::cout << "RemoveTail: start, has tail" << std::endl;

	this->_lru_index.erase(this->_lru_tail->key);

	// if there is only one element in the list
	if (this->_lru_tail->prev == nullptr) {
		this->_cur_size = 0;
		this->_lru_tail = nullptr;
		this->_lru_head.reset(nullptr);
		return;
	}

	this->_cur_size -= this->_lru_tail->key.size() + this->_lru_tail->value.size();
	this->_lru_tail = this->_lru_tail->prev;
	this->_lru_tail->next.reset(nullptr);
	return;
}

void SimpleLRU::MoveToHead(lru_node *node) {
	// if node is head
	std::cout << "MoveToHead: start" << std::endl;
	if (node->prev == nullptr) {
		std::cout << "MoveToHead: node is head" << std::endl;
		return;
	}

	if (node == this->_lru_tail) {
		std::cout << "MoveToHead: node is tail" << std::endl;
		this->_lru_tail = node->prev;
		node->next = std::move(this->_lru_head);
		this->_lru_head = std::move(node->prev->next);
		this->_lru_tail->next.reset(nullptr);
		node->prev = nullptr;
		return;
	}

	std::cout << "MoveToHead: node is inner" << std::endl;
	node->next->prev = node->prev;
	std::unique_ptr<lru_node> tmp = std::move(this->_lru_head);
	this->_lru_head = std::move(node->prev->next);
	node->prev->next = std::move(node->next);
	node->next = std::move(tmp);
	node->prev = nullptr;
}


// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Put(const std::string &key, const std::string &value) {
	std::cout << "Put: start!" << std::endl;

	for (auto it = this->_lru_index.begin(); it != this->_lru_index.end(); ++it) {
		std::cout << it->first.get() << ": " << it->second.get().value << ";" << std::endl;
	}
	std::cout << std::endl;
	
	auto found = this->_lru_index.find(key);
	std::cout << "Put: checked!" << std::endl;
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
	std::cout << "PutIfAbsent: start!" << std::endl;
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
	std::cout << "Set: start!" << std::endl;
	auto found = this->_lru_index.find(key);
	// if elem not in cache
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
	std::cout << "Delete: start!" << std::endl;
	auto found = this->_lru_index.find(key);
	// if elem not in cache
	if (found != this->_lru_index.end()) {
		this->_lru_index.erase(found);

		lru_node *node = &(found->second.get());
		this->_cur_size -= node->key.size() + node->value.size();

		// if it is head
		if (node->prev == nullptr) {
			this->_lru_head = std::move(node->next);
			this->_lru_head->prev = nullptr;
			return true;
		}

		// if it is tail
		if (node == this->_lru_tail) {
			node->prev->next = std::move(node->next);
			return true;
		}

		node->next->prev = node->prev;
		node->prev->next = std::move(node->next);
		return true;
	} else {
		return false;
	}
}

// See MapBasedGlobalLockImpl.h
bool SimpleLRU::Get(const std::string &key, std::string &value) const {
	std::cout << "Get: start!" << std::endl;

	for (auto it = this->_lru_index.begin(); it != this->_lru_index.end(); ++it) {
		std::cout << it->first.get() << ": " << it->second.get().value << ";" << std::endl;
	}
	std::cout << std::endl;

	auto found = this->_lru_index.find(key);
	std::cout << "Get: checked!" << std::endl;
	// if elem not in cache
	if (found != this->_lru_index.end()) {
		value = found->second.get().value;
		return true;
	} else {
		return false;
	}
}

} // namespace Backend
} // namespace Afina
