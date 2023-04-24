//
// Created by fmend on 4/22/2023.
//

#include "systems/logging_system.h"

LogEntries logs(insert_logging_system_callback);

void insert_logging_system_callback (uint64_t unix_time, std::string message) {
    std::cout << "insert_logging_system_callback" << unix_time << " " << message << std::endl;
}