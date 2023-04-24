//
// Created by fmend on 4/22/2023.
//

#include "systems/database_system.h"
#include "sqlite3.h"



void initialize_database(LogEntries& log_entries) {
    log_entries.logs.insert(0, "initialize_database");
}