#ifndef COMMAND_LINE_H
#define COMMAND_LINE_H

#include <memory>
#include <string>

class TaskIndependentSearchEngine;

extern std::shared_ptr<TaskIndependentSearchEngine> parse_cmd_line(
    int argc, const char **argv, bool is_unit_cost);

extern std::string usage(const std::string &progname);

#endif
