#include "command_line.h"
#include "search_engine.h"

#include "tasks/root_task.h"
#include "task_utils/task_properties.h"
#include "utils/logging.h"
#include "utils/system.h"
#include "utils/timer.h"

#include <iostream>


#include "task_independent_search_engine.h"
//for testing

#include "heuristics/lm_cut_heuristic.h"
#include "evaluators/g_evaluator.h"
#include "evaluators/sum_evaluator.h"
#include "open_lists/tiebreaking_open_list.h"
#include "search_engines/eager_search.h"

using namespace std;
using utils::ExitCode;

int main(int argc, const char **argv) {
    utils::register_event_handlers();

    if (argc < 2) {
        utils::g_log << usage(argv[0]) << endl;
        utils::exit_with(ExitCode::SEARCH_INPUT_ERROR);
    }

    bool unit_cost = false;
    if (static_cast<string>(argv[1]) != "--help") {
        utils::g_log << "reading input..." << endl;
        tasks::read_root_task(cin);
        utils::g_log << "done reading input!" << endl;
        TaskProxy task_proxy(*tasks::g_root_task);
        unit_cost = task_properties::is_unit_cost(task_proxy);
    }

    utils::g_log << "Creating task independent SearchEngine..." << endl;
    shared_ptr<TaskIndependentSearchEngine> ti_engine = parse_cmd_line(argc, argv, unit_cost);

    utils::g_log << "Creating task specific SearchEngine..." << endl;
    shared_ptr<SearchEngine> engine = ti_engine->create_task_specific(tasks::g_root_task);

    utils::Timer search_timer;
    engine->search();
    search_timer.stop();
    utils::g_timer.stop();

    engine->save_plan_if_necessary();
    engine->print_statistics();
    utils::g_log << "Search time: " << search_timer << endl;
    utils::g_log << "Total time: " << utils::g_timer << endl;

    ExitCode exitcode = engine->found_solution()
        ? ExitCode::SUCCESS
        : ExitCode::SEARCH_UNSOLVED_INCOMPLETE;
    utils::report_exit_code_reentrant(exitcode);
    return static_cast<int>(exitcode);
}
