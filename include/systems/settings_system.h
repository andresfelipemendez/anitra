//
// Created by fmend on 4/22/2023.
//

#pragma once

#include "data/data_layout.h"

void set_setting(SettingsData& settings_data, const std::string& key, const std::string& value);
std::string get_setting(const SettingsData& settings_data, const std::string& key);