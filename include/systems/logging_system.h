//
// Created by fmend on 4/22/2023.
//

#pragma once

#include <iostream>
#include "data/data_layout.h"

void insert_logging_system_callback (uint64_t unix_time, std::string message);

extern LogEntries logs;
