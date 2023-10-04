#include "iterated_search.h"

#include "../plugins/plugin.h"
#include "../utils/logging.h"

#include <iostream>

using namespace std;

namespace iterated_search {
IteratedSearch::IteratedSearch(const plugins::Options &opts)
    : SearchEngine(opts),
      engine_configs(opts.get_list<parser::LazyValue>("engine_configs")),
      pass_bound(opts.get<bool>("pass_bound")),
      repeat_last_phase(opts.get<bool>("repeat_last")),
      continue_on_fail(opts.get<bool>("continue_on_fail")),
      continue_on_solve(opts.get<bool>("continue_on_solve")),
      phase(0),
      last_phase_found_solution(false),
      best_bound(bound),
      iterated_found_solution(false) {
}

IteratedSearch::IteratedSearch(utils::Verbosity verbosity,
                               OperatorCost cost_type,
                               double max_time,
                               int bound,
                               const shared_ptr<AbstractTask> &task,
                               vector<parser::LazyValue> engine_configs,
                               bool pass_bound,
                               bool repeat_last_phase,
                               bool continue_on_fail,
                               bool continue_on_solve,
                               string unparsed_config
                               ) : SearchEngine(verbosity,
                                                cost_type,
                                                max_time,
                                                bound,
                                                unparsed_config,
                                                task),
                                   engine_configs(engine_configs),
                                   pass_bound(pass_bound),
                                   repeat_last_phase(repeat_last_phase),
                                   continue_on_fail(continue_on_fail),
                                   continue_on_solve(continue_on_solve),
                                   phase(0),
                                   last_phase_found_solution(false),
                                   best_bound(bound),
                                   iterated_found_solution(false)
                               {
    cout << "IteratedSearch::IteratedSearch" << endl;
    cout << "  cost bound: " << bound << endl;
                               }

shared_ptr<TaskIndependentSearchEngine> IteratedSearch::get_search_engine(
    int engine_configs_index) {
    cout << "in IteratedSearch::get_search_engine(int)" << endl;
    cout << "about to parser::LazyValue &engine_config = engine_configs[engine_configs_index];" << endl;
    parser::LazyValue &engine_config = engine_configs[engine_configs_index];
    cout << "done with parser::LazyValue &engine_config = engine_configs[engine_configs_index];" << endl;
    shared_ptr<TaskIndependentSearchEngine> engine;
    try{
        cout << "about to engine = engine_config.construct<shared_ptr<SearchEngine>>();" << endl;
        engine = engine_config.construct<shared_ptr<TaskIndependentSearchEngine>>();
        cout << "done with engine = engine_config.construct<shared_ptr<SearchEngine>>();" << endl;
    } catch (const utils::ContextError &e) {
        cerr << "Delayed construction of LazyValue failed" << endl;
        cerr << e.get_message() << endl;
        utils::exit_with(utils::ExitCode::SEARCH_INPUT_ERROR);
    }
    auto x = "engine->get_description(); is only possible in TS_Component";
    log << "Starting search: " << x << endl;
    return engine;
}

shared_ptr<SearchEngine> IteratedSearch::create_current_phase() {

    cout << "in IteratedSearch::create_current_phase()" << endl;

    cout << "about to int num_phases = engine_configs.size();" << endl;
    int num_phases = engine_configs.size();
    cout << "done with int num_phases = engine_configs.size();" << endl;

    if (phase >= num_phases) {
        /* We've gone through all searches. We continue if
           repeat_last_phase is true, but *not* if we didn't find a
           solution the last time around, since then this search would
           just behave the same way again (assuming determinism, which
           we might not actually have right now, but strive for). So
           this overrides continue_on_fail.
        */
        if (repeat_last_phase && last_phase_found_solution) {
            cout << "CASE: repeat_last_phase && last_phase_found_solution" << endl;
            auto x1 = get_search_engine(engine_configs.size() - 1);
            auto ret1 = x1->create_task_specific_SearchEngine(task,1);
            return ret1;
        } else {
            cout << "CASE: NOT (repeat_last_phase && last_phase_found_solution)" << endl;
            return nullptr;
        }
    }
    auto x2 = get_search_engine(phase);
    auto ret2 = x2->create_task_specific_SearchEngine(task, 1);
    return ret2;
}

SearchStatus IteratedSearch::step() {
    cout << "about to shared_ptr<SearchEngine> current_search = create_current_phase()" << endl;
    shared_ptr<SearchEngine> current_search = create_current_phase();
    cout << "done with shared_ptr<SearchEngine> current_search = create_current_phase()" << endl;
    if (!current_search) {
        return found_solution() ? SOLVED : FAILED;
    }
    if (pass_bound) {
        current_search->set_bound(best_bound);
    }
    ++phase;


    cout << "about to current_search->search();" << endl;
    current_search->search();
    cout << "done with current_search->search();" << endl;

    Plan found_plan;
    int plan_cost = 0;
    last_phase_found_solution = current_search->found_solution();
    if (last_phase_found_solution) {
        iterated_found_solution = true;
        found_plan = current_search->get_plan();
        plan_cost = calculate_plan_cost(found_plan, task_proxy);
        if (plan_cost < best_bound) {
            plan_manager.save_plan(found_plan, task_proxy, true);
            best_bound = plan_cost;
            set_plan(found_plan);
        }
    }
    current_search->print_statistics();

    const SearchStatistics &current_stats = current_search->get_statistics();
    statistics.inc_expanded(current_stats.get_expanded());
    statistics.inc_evaluated_states(current_stats.get_evaluated_states());
    statistics.inc_evaluations(current_stats.get_evaluations());
    statistics.inc_generated(current_stats.get_generated());
    statistics.inc_generated_ops(current_stats.get_generated_ops());
    statistics.inc_reopened(current_stats.get_reopened());

    return step_return_value();
}

SearchStatus IteratedSearch::step_return_value() {
    if (iterated_found_solution)
        log << "Best solution cost so far: " << best_bound << endl;

    if (last_phase_found_solution) {
        if (continue_on_solve) {
            log << "Solution found - keep searching" << endl;
            return IN_PROGRESS;
        } else {
            log << "Solution found - stop searching" << endl;
            return SOLVED;
        }
    } else {
        if (continue_on_fail) {
            log << "No solution found - keep searching" << endl;
            return IN_PROGRESS;
        } else {
            log << "No solution found - stop searching" << endl;
            return iterated_found_solution ? SOLVED : FAILED;
        }
    }
}

void IteratedSearch::print_statistics() const {
    log << "Cumulative statistics:" << endl;
    statistics.print_detailed_statistics();
}

void IteratedSearch::save_plan_if_necessary() {
    // We don't need to save here, as we automatically save after
    // each successful search iteration.
}


TaskIndependentIteratedSearch::TaskIndependentIteratedSearch(utils::Verbosity verbosity,
                                                             OperatorCost cost_type,
                                                             double max_time,
                                                             string unparsed_config,
                                                             vector<parser::LazyValue> engine_configs,
                                                             bool pass_bound,
                                                             bool repeat_last_phase,
                                                             bool continue_on_fail,
                                                             bool continue_on_solve
)
        : TaskIndependentSearchEngine(verbosity,
                                      cost_type,
                                      max_time,
                                      bound,
                                      unparsed_config),
          engine_configs(engine_configs),
          pass_bound(pass_bound),
          repeat_last_phase(repeat_last_phase),
          continue_on_fail(continue_on_fail),
          continue_on_solve(continue_on_solve) {
}

TaskIndependentIteratedSearch::~TaskIndependentIteratedSearch() {
}


shared_ptr<IteratedSearch> TaskIndependentIteratedSearch::create_task_specific_IteratedSearch(const shared_ptr<AbstractTask> &task, std::shared_ptr<ComponentMap> &component_map, int depth) {
    shared_ptr<IteratedSearch> task_specific_x;
    if (component_map->contains_key(make_pair(task, static_cast<void *>(this)))) {
        utils::g_log << std::string(depth, ' ') << "Reusing task IteratedSearch..." << endl;
        task_specific_x = plugins::any_cast<shared_ptr<IteratedSearch>>(
                component_map->get_dual_key_value(task, this));
    } else {
        utils::g_log << std::string(depth, ' ') << "Creating task specific IteratedSearch..." << endl;

        task_specific_x = make_shared<IteratedSearch>(verbosity,
                cost_type,
                max_time,
                bound,
                task,
                engine_configs,
                pass_bound,
                repeat_last_phase,
                continue_on_fail,
                continue_on_solve);
        component_map->add_dual_key_entry(task, this, plugins::Any(task_specific_x));
        utils::g_log << "Created task specific IteratedSearch..." << endl;
    }
    return task_specific_x;
}




shared_ptr<IteratedSearch> TaskIndependentIteratedSearch::create_task_specific_IteratedSearch(const shared_ptr<AbstractTask> &task, int depth) {
    utils::g_log << "Creating IteratedSearch as root component..." << endl;
    std::shared_ptr<ComponentMap> component_map = std::make_shared<ComponentMap>();
    return create_task_specific_IteratedSearch(task, component_map, depth);
}



shared_ptr<SearchEngine> TaskIndependentIteratedSearch::create_task_specific_SearchEngine(const shared_ptr<AbstractTask> &task, shared_ptr<ComponentMap> &component_map, int depth) {
    shared_ptr<SearchEngine> x = create_task_specific_IteratedSearch(task, component_map, depth);
    return static_pointer_cast<SearchEngine>(x);
}

class TaskIndependentIteratedSearchFeature : public plugins::TypedFeature<TaskIndependentSearchEngine, TaskIndependentIteratedSearch> {
public:
    TaskIndependentIteratedSearchFeature() : TypedFeature("iterated") {
        document_title("Iterated search");
        document_synopsis("");

        add_list_option<shared_ptr<TaskIndependentSearchEngine>>(
            "engine_configs",
            "list of search engines for each phase",
            "",
            true);
        add_option<bool>(
            "pass_bound",
            "use bound from previous search. The bound is the real cost "
            "of the plan found before, regardless of the cost_type parameter.",
            "true");
        add_option<bool>(
            "repeat_last",
            "repeat last phase of search",
            "false");
        add_option<bool>(
            "continue_on_fail",
            "continue search after no solution found",
            "false");
        add_option<bool>(
            "continue_on_solve",
            "continue search after solution found",
            "true");
        SearchEngine::add_options_to_feature(*this);

        document_note(
            "Note 1",
            "We don't cache heuristic values between search iterations at"
            " the moment. If you perform a LAMA-style iterative search,"
            " heuristic values will be computed multiple times.");
        document_note(
            "Note 2",
            "The configuration\n```\n"
            "--search \"iterated([lazy_wastar(merge_and_shrink(),w=10), "
            "lazy_wastar(merge_and_shrink(),w=5), lazy_wastar(merge_and_shrink(),w=3), "
            "lazy_wastar(merge_and_shrink(),w=2), lazy_wastar(merge_and_shrink(),w=1)])\"\n"
            "```\nwould perform the preprocessing phase of the merge and shrink heuristic "
            "5 times (once before each iteration).\n\n"
            "To avoid this, use heuristic predefinition, which avoids duplicate "
            "preprocessing, as follows:\n```\n"
            "--evaluator \"h=merge_and_shrink()\" --search "
            "\"iterated([lazy_wastar(h,w=10), lazy_wastar(h,w=5), lazy_wastar(h,w=3), "
            "lazy_wastar(h,w=2), lazy_wastar(h,w=1)])\"\n"
            "```");
        document_note(
            "Note 3",
            "If you reuse the same landmark count heuristic "
            "(using heuristic predefinition) between iterations, "
            "the path data (that is, landmark status for each visited state) "
            "will be saved between iterations.");
    }

    virtual shared_ptr<TaskIndependentIteratedSearch> create_component(const plugins::Options &opts, const utils::Context &context) const override {
        plugins::Options options_copy(opts);
        /*
          The options entry 'engine_configs' is a LazyValue representing a list
          of search engines. But iterated search expects a list of LazyValues,
          each representing a search engine. We unpack this first layer of
          laziness here to report potential errors in a more useful context.

          TODO: the medium-term plan is to get rid of LazyValue completely
          and let the features create builders that in turn create the actual
          search engines. Then we no longer need to be lazy because creating
          the builder is a light-weight operation.
        */
        vector<parser::LazyValue> engine_configs =
            opts.get<parser::LazyValue>("engine_configs").construct_lazy_list();
        options_copy.set("engine_configs", engine_configs);
        plugins::verify_list_non_empty<parser::LazyValue>(context, options_copy, "engine_configs");

        return make_shared<TaskIndependentIteratedSearch>(opts.get<utils::Verbosity>("verbosity"),
                                           opts.get<OperatorCost>("cost_type"),
                                           opts.get<double>("max_time"),
                                           opts.get_unparsed_config(),
                                           opts.get<parser::LazyValue>("engine_configs").construct_lazy_list(),
                                           opts.get<bool>("pass_bound"),
                                           opts.get<bool>("repeat_last"),
                                           opts.get<bool>("continue_on_fail"),
                                           opts.get<bool>("continue_on_solve")
                                           );
    }
};


static plugins::FeaturePlugin<TaskIndependentIteratedSearchFeature> _plugin;

}
