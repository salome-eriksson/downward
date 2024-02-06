#ifndef TASKS_NEGATED_AXIOMS_TASK_H
#define TASKS_NEGATED_AXIOMS_TASK_H

#include "delegating_task.h"

#include "../task_proxy.h" // TODO: can we get rid of this include?

#include "../algorithms/sccs.h"

namespace plugins {
class Options;
}

namespace tasks {
struct NegatedAxiom {
    FactPair head;
    std::vector<FactPair> condition;
    std::string name;

    // TODO: move constructor?
    NegatedAxiom(FactPair head, std::vector<FactPair> condition, std::string name)
        : head(head), condition(condition), name(name) {}
};

class NegatedAxiomsTask : public DelegatingTask {
    std::vector<NegatedAxiom> negated_axioms;
    int negated_axioms_start_index;

    void add_negated_axioms(FactPair head, std::vector<OperatorProxy> &axioms, TaskProxy &task_proxy);
public:
    NegatedAxiomsTask(
        const std::shared_ptr<AbstractTask> &parent);
    virtual ~NegatedAxiomsTask() override = default;

    virtual int get_operator_cost(int index, bool is_axiom) const override;
    virtual std::string get_operator_name(int index, bool is_axiom) const override;
    virtual int get_num_operator_preconditions(int index, bool is_axiom) const override;
    virtual FactPair get_operator_precondition(
            int op_index, int fact_index, bool is_axiom) const override;
    virtual int get_num_operator_effects(int op_index, bool is_axiom) const override;
    virtual int get_num_operator_effect_conditions(
            int op_index, int eff_index, bool is_axiom) const override;
    virtual FactPair get_operator_effect_condition(
            int op_index, int eff_index, int cond_index, bool is_axiom) const override;
    virtual FactPair get_operator_effect(
            int op_index, int eff_index, bool is_axiom) const override;
    // TODO: Is this only called for actual operators or also axioms? I assume so because there is no bool is_axiom...
    virtual int convert_operator_index_to_parent(int index) const override;

    virtual int get_num_axioms() const override;
};
}

#endif
