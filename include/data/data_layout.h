//
// Created by fmend on 4/22/2023.
//

#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include "ObservableMap.h"

struct LogEntries {
    LogEntries(std::function<void(uint64_t, std::string)> insert_callback) : logs(insert_callback) {

    }

    ObservableMap<uint64_t , std::string> logs;
};

struct SettingsData {
    ObservableMap<std::string, std::string > key_value_pairs;
};
