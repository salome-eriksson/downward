#include "eager_search.h"

#include "../evaluation_context.h"
#include "../evaluator.h"
#include "../open_list_factory.h"
#include "../pruning_method.h"

#include "../algorithms/ordered_set.h"
#include "../task_utils/successor_generator.h"
#include "../utils/logging.h"

#include <cassert>
#include <cstdlib>
#include <memory>
#include <optional.hh>
#include <set>

using namespace std;

namespace eager_search {
EagerSearch::EagerSearch(utils::Verbosity verbosity,
                         OperatorCost cost_type,
                         double max_time,
                         int bound,
                         bool reopen_closed_nodes,
                         unique_ptr<StateOpenList> open_list,
                         vector<shared_ptr<Evaluator>> preferred_operator_evaluators,
                         shared_ptr<PruningMethod> pruning_method,
                         const shared_ptr<AbstractTask> &task,
                         shared_ptr<Evaluator> f_evaluator,
                         shared_ptr<Evaluator> lazy_evaluator,
                         string unparsed_config
                         )
    : SearchEngine(verbosity,
                   cost_type,
                   max_time,
                   bound,
                   unparsed_config,
                   task),
      reopen_closed_nodes(reopen_closed_nodes),
      open_list(std::move(open_list)),
      f_evaluator(f_evaluator),
      preferred_operator_evaluators(preferred_operator_evaluators),
      lazy_evaluator(lazy_evaluator),
      pruning_method(pruning_method) {
    if (lazy_evaluator && !lazy_evaluator->does_cache_estimates()) {
        cerr << "lazy_evaluator must cache its estimates" << endl;
        utils::exit_with(utils::ExitCode::SEARCH_INPUT_ERROR);
    }
}

void EagerSearch::initialize() {
    log << "Conducting best first search"
        << (reopen_closed_nodes ? " with" : " without")
        << " reopening closed nodes, (real) bound = " << bound
        << endl;
    assert(open_list);

    set<Evaluator *> evals;
    open_list->get_path_dependent_evaluators(evals);

    /*
      Collect path-dependent evaluators that are used for preferred operators
      (in case they are not also used in the open list).
    */
    for (const shared_ptr<Evaluator> &evaluator : preferred_operator_evaluators) {
        evaluator->get_path_dependent_evaluators(evals);
    }

    /*
      Collect path-dependent evaluators that are used in the f_evaluator.
      They are usually also used in the open list and will hence already be
      included, but we want to be sure.
    */
    if (f_evaluator) {
        f_evaluator->get_path_dependent_evaluators(evals);
    }

    /*
      Collect path-dependent evaluators that are used in the lazy_evaluator
      (in case they are not already included).
    */
    if (lazy_evaluator) {
        lazy_evaluator->get_path_dependent_evaluators(evals);
    }

    path_dependent_evaluators.assign(evals.begin(), evals.end());

    State initial_state = state_registry.get_initial_state();
    for (Evaluator *evaluator : path_dependent_evaluators) {
        evaluator->notify_initial_state(initial_state);
    }

    /*
      Note: we consider the initial state as reached by a preferred
      operator.
    */
    EvaluationContext eval_context(initial_state, 0, true, &statistics);

    statistics.inc_evaluated_states();

    if (open_list->is_dead_end(eval_context)) {
        log << "Initial state is a dead end." << endl;
    } else {
        if (search_progress.check_progress(eval_context))
            statistics.print_checkpoint_line(0);
        start_f_value_statistics(eval_context);
        SearchNode node = search_space.get_node(initial_state);
        node.open_initial();

        open_list->insert(eval_context, initial_state.get_id());
    }

    print_initial_evaluator_values(eval_context);

    pruning_method->initialize(task);
}

void EagerSearch::print_statistics() const {
    statistics.print_detailed_statistics();
    search_space.print_statistics();
    pruning_method->print_statistics();
}

SearchStatus EagerSearch::step() {
    tl::optional<SearchNode> node;
    while (true) {
        if (open_list->empty()) {
            log << "Completely explored state space -- no solution!" << endl;
            return FAILED;
        }
        StateID id = open_list->remove_min();
        State s = state_registry.lookup_state(id);
        node.emplace(search_space.get_node(s));

        if (node->is_closed())
            continue;

        /*
          We can pass calculate_preferred=false here since preferred
          operators are computed when the state is expanded.
        */
        EvaluationContext eval_context(s, node->get_g(), false, &statistics);

        if (lazy_evaluator) {
            /*
              With lazy evaluators (and only with these) we can have dead nodes
              in the open list.

              For example, consider a state s that is reached twice before it is expanded.
              The first time we insert it into the open list, we compute a finite
              heuristic value. The second time we insert it, the cached value is reused.

              During first expansion, the heuristic value is recomputed and might become
              infinite, for example because the reevaluation uses a stronger heuristic or
              because the heuristic is path-dependent and we have accumulated more
              information in the meantime. Then upon second expansion we have a dead-end
              node which we must ignore.
            */
            if (node->is_dead_end())
                continue;

            if (lazy_evaluator->is_estimate_cached(s)) {
                int old_h = lazy_evaluator->get_cached_estimate(s);
                int new_h = eval_context.get_evaluator_value_or_infinity(lazy_evaluator.get());
                if (open_list->is_dead_end(eval_context)) {
                    node->mark_as_dead_end();
                    statistics.inc_dead_ends();
                    continue;
                }
                if (new_h != old_h) {
                    open_list->insert(eval_context, id);
                    continue;
                }
            }
        }

        node->close();
        assert(!node->is_dead_end());
        update_f_value_statistics(eval_context);
        statistics.inc_expanded();
        break;
    }

    const State &s = node->get_state();
    if (check_goal_and_set_plan(s))
        return SOLVED;

    vector<OperatorID> applicable_ops;
    successor_generator.generate_applicable_ops(s, applicable_ops);

    /*
      TODO: When preferred operators are in use, a preferred operator will be
      considered by the preferred operator queues even when it is pruned.
    */
    pruning_method->prune_operators(s, applicable_ops);

    // This evaluates the expanded state (again) to get preferred ops
    EvaluationContext eval_context(s, node->get_g(), false, &statistics, true);
    ordered_set::OrderedSet<OperatorID> preferred_operators;
    for (const shared_ptr<Evaluator> &preferred_operator_evaluator : preferred_operator_evaluators) {
        collect_preferred_operators(eval_context,
                                    preferred_operator_evaluator.get(),
                                    preferred_operators);
    }

    for (OperatorID op_id : applicable_ops) {
        OperatorProxy op = task_proxy.get_operators()[op_id];
        if ((node->get_real_g() + op.get_cost()) >= bound)
            continue;

        State succ_state = state_registry.get_successor_state(s, op);
        statistics.inc_generated();
        bool is_preferred = preferred_operators.contains(op_id);

        SearchNode succ_node = search_space.get_node(succ_state);

        for (Evaluator *evaluator : path_dependent_evaluators) {
            evaluator->notify_state_transition(s, op_id, succ_state);
        }

        // Previously encountered dead end. Don't re-evaluate.
        if (succ_node.is_dead_end())
            continue;

        if (succ_node.is_new()) {
            // We have not seen this state before.
            // Evaluate and create a new node.

            // Careful: succ_node.get_g() is not available here yet,
            // hence the stupid computation of succ_g.
            // TODO: Make this less fragile.
            int succ_g = node->get_g() + get_adjusted_cost(op);

            EvaluationContext succ_eval_context(
                succ_state, succ_g, is_preferred, &statistics);
            statistics.inc_evaluated_states();

            if (open_list->is_dead_end(succ_eval_context)) {
                succ_node.mark_as_dead_end();
                statistics.inc_dead_ends();
                continue;
            }
            succ_node.open(*node, op, get_adjusted_cost(op));

            open_list->insert(succ_eval_context, succ_state.get_id());
            if (search_progress.check_progress(succ_eval_context)) {
                statistics.print_checkpoint_line(succ_node.get_g());
                reward_progress();
            }
        } else if (succ_node.get_g() > node->get_g() + get_adjusted_cost(op)) {
            // We found a new cheapest path to an open or closed state.
            if (reopen_closed_nodes) {
                if (succ_node.is_closed()) {
                    /*
                      TODO: It would be nice if we had a way to test
                      that reopening is expected behaviour, i.e., exit
                      with an error when this is something where
                      reopening should not occur (e.g. A* with a
                      consistent heuristic).
                    */
                    statistics.inc_reopened();
                }
                succ_node.reopen(*node, op, get_adjusted_cost(op));

                EvaluationContext succ_eval_context(
                    succ_state, succ_node.get_g(), is_preferred, &statistics);

                /*
                  Note: our old code used to retrieve the h value from
                  the search node here. Our new code recomputes it as
                  necessary, thus avoiding the incredible ugliness of
                  the old "set_evaluator_value" approach, which also
                  did not generalize properly to settings with more
                  than one evaluator.

                  Reopening should not happen all that frequently, so
                  the performance impact of this is hopefully not that
                  large. In the medium term, we want the evaluators to
                  remember evaluator values for states themselves if
                  desired by the user, so that such recomputations
                  will just involve a look-up by the Evaluator object
                  rather than a recomputation of the evaluator value
                  from scratch.
                */
                open_list->insert(succ_eval_context, succ_state.get_id());
            } else {
                // If we do not reopen closed nodes, we just update the parent pointers.
                // Note that this could cause an incompatibility between
                // the g-value and the actual path that is traced back.
                succ_node.update_parent(*node, op, get_adjusted_cost(op));
            }
        }
    }

    return IN_PROGRESS;
}

void EagerSearch::reward_progress() {
    // Boost the "preferred operator" open lists somewhat whenever
    // one of the heuristics finds a state with a new best h value.
    open_list->boost_preferred();
}

void EagerSearch::dump_search_space() const {
    search_space.dump(task_proxy);
}

void EagerSearch::start_f_value_statistics(EvaluationContext &eval_context) {
    if (f_evaluator) {
        int f_value = eval_context.get_evaluator_value(f_evaluator.get());
        statistics.report_f_value_progress(f_value);
    }
}

/* TODO: HACK! This is very inefficient for simply looking up an h value.
   Also, if h values are not saved it would recompute h for each and every state. */
void EagerSearch::update_f_value_statistics(EvaluationContext &eval_context) {
    if (f_evaluator) {
        int f_value = eval_context.get_evaluator_value(f_evaluator.get());
        statistics.report_f_value_progress(f_value);
    }
}

void add_options_to_feature(plugins::Feature &feature) {
    SearchEngine::add_pruning_option(feature);
    SearchEngine::add_options_to_feature(feature);
}


TaskIndependentEagerSearch::TaskIndependentEagerSearch(utils::Verbosity verbosity,
                                                       OperatorCost cost_type,
                                                       double max_time,
                                                       int bound,
                                                       bool reopen_closed_nodes,
                                                       shared_ptr<TaskIndependentOpenListFactory> open_list_factory,
                                                       vector<shared_ptr<TaskIndependentEvaluator>> preferred_operator_evaluators,
                                                       shared_ptr<PruningMethod> pruning_method,
                                                       shared_ptr<TaskIndependentEvaluator> f_evaluator,
                                                       shared_ptr<TaskIndependentEvaluator> lazy_evaluator,
                                                       string unparsed_config
                                                       )
    : TaskIndependentSearchEngine(verbosity,
                                  cost_type,
                                  max_time,
                                  bound,
                                  unparsed_config
                                  ),
      reopen_closed_nodes(reopen_closed_nodes),
      open_list_factory(std::move(open_list_factory)),
      f_evaluator(f_evaluator),
      preferred_operator_evaluators(preferred_operator_evaluators),
      lazy_evaluator(lazy_evaluator),
      pruning_method(pruning_method) {
}

TaskIndependentEagerSearch::~TaskIndependentEagerSearch() {
}


shared_ptr<EagerSearch> TaskIndependentEagerSearch::create_task_specific_EagerSearch(const shared_ptr<AbstractTask> &task, std::unique_ptr<ComponentMap> &component_map, int depth) {
    shared_ptr<EagerSearch> task_specific_eager_search;
    if (component_map->contains_key(make_pair(task, static_cast<void *>(this)))) {
        utils::g_log << std::string(depth, ' ') << "Reusing task EagerSearch..." << endl;
        task_specific_eager_search = plugins::any_cast<shared_ptr<EagerSearch>>(
            component_map->get_dual_key_value(task, this));
    } else {
        utils::g_log << std::string(depth, ' ') << "Creating task specific EagerSearch..." << endl;
        vector<shared_ptr<Evaluator>> td_evaluators(preferred_operator_evaluators.size());
        transform(preferred_operator_evaluators.begin(), preferred_operator_evaluators.end(), td_evaluators.begin(),
                  [this, &task, &component_map, &depth](const shared_ptr<TaskIndependentEvaluator> &eval) {
                      return eval->create_task_specific_Evaluator(task, component_map, depth >= 0 ? depth + 1 : depth);
                  }
                  );

        unique_ptr<StateOpenList> _open_list = unique_ptr<StateOpenList>(
            open_list_factory->create_task_specific_OpenListFactory(task, component_map, depth >= 0 ? depth + 1 : depth)->create_state_open_list());

        task_specific_eager_search = make_shared<EagerSearch>(verbosity,
                                                              cost_type,
                                                              max_time,
                                                              bound,
                                                              reopen_closed_nodes,
                                                              move(_open_list),
                                                              td_evaluators,
                                                              pruning_method,
                                                              task,
                                                              f_evaluator ? f_evaluator->create_task_specific_Evaluator(task, component_map, depth >= 0 ? depth + 1 : depth) : nullptr,
                                                              lazy_evaluator ? lazy_evaluator->create_task_specific_Evaluator(task, component_map, depth >= 0 ? depth + 1 : depth) : nullptr);

        component_map->add_dual_key_entry(task, this, plugins::Any(task_specific_eager_search));
    }
    return task_specific_eager_search;
}




shared_ptr<EagerSearch> TaskIndependentEagerSearch::create_task_specific_EagerSearch(const shared_ptr<AbstractTask> &task, int depth) {
    utils::g_log << std::string(depth, ' ') << "Creating EagerSearch as root component..." << endl;
    std::unique_ptr<ComponentMap> component_map = std::make_unique<ComponentMap>();
    return create_task_specific_EagerSearch(task, component_map, depth);
}



shared_ptr<SearchEngine> TaskIndependentEagerSearch::create_task_specific_SearchEngine(const shared_ptr<AbstractTask> &task, unique_ptr<ComponentMap> &component_map, int depth) {
    shared_ptr<SearchEngine> x = create_task_specific_EagerSearch(task, component_map, depth);
    return static_pointer_cast<SearchEngine>(x);
}
}
