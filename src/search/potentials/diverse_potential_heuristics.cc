#include "diverse_potential_heuristics.h"

#include "potential_function.h"
#include "potential_max_heuristic.h"
#include "util.h"

#include "../option_parser.h"
#include "../plugin.h"

#include "../utils/logging.h"
#include "../utils/rng.h"
#include "../utils/rng_options.h"
#include "../utils/timer.h"

#include <unordered_set>

using namespace std;

namespace potentials {
DiversePotentialHeuristics::DiversePotentialHeuristics(const Options &opts)
    : optimizer(opts),
      max_num_heuristics(opts.get<int>("max_num_heuristics")),
      num_samples(opts.get<int>("num_samples")),
      rng(utils::parse_rng_from_options(opts)),
      log(utils::get_log_from_options(opts)) {
}

SamplesToFunctionsMap
DiversePotentialHeuristics::filter_samples_and_compute_functions(
    const vector<State> &samples) {
    utils::Timer filtering_timer;
    utils::HashSet<State> dead_ends;
    int num_duplicates = 0;
    int num_dead_ends = 0;
    SamplesToFunctionsMap samples_to_functions;
    for (const State &sample : samples) {
        // Skipping duplicates is not necessary, but saves LP evaluations.
        if (samples_to_functions.count(sample) || dead_ends.count(sample)) {
            ++num_duplicates;
            continue;
        }
        optimizer.optimize_for_state(sample);
        if (optimizer.has_optimal_solution()) {
            samples_to_functions[sample] = optimizer.get_potential_function();
        } else {
            dead_ends.insert(sample);
            ++num_dead_ends;
        }
    }
    if (log.is_at_least_normal()) {
        log << "Time for filtering dead ends: " << filtering_timer << endl;
        log << "Duplicate samples: " << num_duplicates << endl;
        log << "Dead end samples: " << num_dead_ends << endl;
        log << "Unique non-dead-end samples: " << samples_to_functions.size() << endl;
    }
    assert(num_duplicates + num_dead_ends + samples_to_functions.size() == samples.size());
    return samples_to_functions;
}

void DiversePotentialHeuristics::remove_covered_samples(
    const PotentialFunction &chosen_function,
    SamplesToFunctionsMap &samples_to_functions) const {
    for (auto it = samples_to_functions.begin();
         it != samples_to_functions.end();) {
        const State &sample = it->first;
        const PotentialFunction &sample_function = *it->second;
        int max_h = sample_function.get_value(sample);
        int h = chosen_function.get_value(sample);
        assert(h <= max_h);
        // TODO: Count as covered if max_h <= 0.
        if (h == max_h) {
            it = samples_to_functions.erase(it);
        } else {
            ++it;
        }
    }
}

unique_ptr<PotentialFunction>
DiversePotentialHeuristics::find_function_and_remove_covered_samples(
    SamplesToFunctionsMap &samples_to_functions) {
    vector<State> uncovered_samples;
    for (auto &sample_and_function : samples_to_functions) {
        const State &state = sample_and_function.first;
        uncovered_samples.push_back(state);
    }
    optimizer.optimize_for_samples(uncovered_samples);
    unique_ptr<PotentialFunction> function = optimizer.get_potential_function();
    size_t last_num_samples = samples_to_functions.size();
    remove_covered_samples(*function, samples_to_functions);
    if (samples_to_functions.size() == last_num_samples) {
        if (log.is_at_least_verbose()) {
            log << "No sample removed -> Use arbitrary precomputed function."
                 << endl;
        }
        function = move(samples_to_functions.begin()->second);
        // The move operation invalidated the entry, remove it.
        samples_to_functions.erase(samples_to_functions.begin());
        remove_covered_samples(*function, samples_to_functions);
    }
    if (log.is_at_least_verbose()) {
        log << "Removed " << last_num_samples - samples_to_functions.size()
             << " samples. " << samples_to_functions.size() << " remaining."
             << endl;
    }
    return function;
}

void DiversePotentialHeuristics::cover_samples(
    SamplesToFunctionsMap &samples_to_functions) {
    utils::Timer covering_timer;
    while (!samples_to_functions.empty() &&
           static_cast<int>(diverse_functions.size()) < max_num_heuristics) {
        if (log.is_at_least_verbose()) {
            log << "Find heuristic #" << diverse_functions.size() + 1 << endl;
        }
        diverse_functions.push_back(
            find_function_and_remove_covered_samples(samples_to_functions));
    }
    if (log.is_at_least_normal()) {
        log << "Time for covering samples: " << covering_timer << endl;
    }
}

vector<unique_ptr<PotentialFunction>>
DiversePotentialHeuristics::find_functions() {
    assert(diverse_functions.empty());
    utils::Timer init_timer;

    // Sample states.
    vector<State> samples = sample_without_dead_end_detection(
        optimizer, num_samples, *rng);

    // Filter dead end samples.
    SamplesToFunctionsMap samples_to_functions =
        filter_samples_and_compute_functions(samples);

    // Iteratively cover samples.
    cover_samples(samples_to_functions);

    if (log.is_at_least_normal()) {
        log << "Potential heuristics: " << diverse_functions.size() << endl;
        log << "Initialization of potential heuristics: " << init_timer << endl;
    }

    return move(diverse_functions);
}

static shared_ptr<Heuristic> _parse(OptionParser &parser) {
    parser.document_synopsis(
        "Diverse potential heuristics",
        get_admissible_potentials_reference());
    parser.add_option<int>(
        "num_samples",
        "Number of states to sample",
        "1000",
        Bounds("0", "infinity"));
    parser.add_option<int>(
        "max_num_heuristics",
        "maximum number of potential heuristics",
        "infinity",
        Bounds("0", "infinity"));
    prepare_parser_for_admissible_potentials(parser);
    utils::add_rng_options(parser);
    utils::add_log_options_to_parser(parser);
    Options opts = parser.parse();
    if (parser.dry_run())
        return nullptr;

    DiversePotentialHeuristics factory(opts);
    return make_shared<PotentialMaxHeuristic>(opts, factory.find_functions());
}

static Plugin<Evaluator> _plugin(
    "diverse_potentials", _parse, "heuristics_potentials");
}
