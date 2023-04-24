//
// Created by fmend on 4/23/2023.
//

#pragma once

#include <iostream>
#include <unordered_map>
#include <functional>

template<typename TKey, typename TValue>
class ObservableMap {
public:
    using InsertCallback = std::function<void(const TKey&, const TValue&)>;

    void insert(const TKey& key, const TValue& value) {
        map.insert({key, value});
        insert_callback(key, value);
    }

    void set_insert_callback(InsertCallback callback) {
        insert_callback = callback;
    }

    ObservableMap(InsertCallback callback) : insert_callback(callback) {}

private:
    std::unordered_map<TKey, TValue> map;
    InsertCallback insert_callback;
};
