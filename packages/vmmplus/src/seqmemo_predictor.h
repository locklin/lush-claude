#ifndef SEQMEMO_PREDICTOR_H
#define SEQMEMO_PREDICTOR_H

#include "vmm_predictor.h"
#include "context_buffer.h"
#include <vector>
#include <cstdint>

namespace vmm {

// Sequence Memoizer (Hierarchical Pitman-Yor Process) predictor.
// Wood, Gasthaus, Archambeau, Murray & Teh (2011):
// "The Sequence Memoizer." CACM 54(2):91-98.
//
// Each node in the context tree is a Pitman-Yor "restaurant."
// Uses expected (fractional) seating for deterministic, reproducible results.
//
// Predictive distribution:
//   P(sym | node) = (c(sym) - d*t(sym) + (theta + d*T)*P_parent(sym)) / (C + theta)
// where c=customers, t=tables, T=total tables, C=total customers, d=discount, theta=concentration.
class SeqMemoPredictor : public VMMPredictor {
public:
    SeqMemoPredictor(uint16_t alphabet_size, int max_order,
                     double discount, double concentration);
    ~SeqMemoPredictor() override = default;

    void learn(const Symbol* data, size_t len) override;
    double predict(Symbol symbol, const Symbol* context, size_t ctx_len) override;
    void predict_distribution(const Symbol* context, size_t ctx_len,
                              double* out_probs) override;
    double log_eval(const Symbol* data, size_t len) override;
    double log_eval(const Symbol* data, size_t len,
                    const Symbol* ctx, size_t ctx_len) override;
    uint16_t alphabet_size() const override { return ab_size_; }

private:
    struct Restaurant {
        std::vector<double> customer_counts;  // c_w(sym)
        std::vector<double> table_counts;     // t_w(sym)
        double total_customers;               // C_w = sum of customer_counts
        double total_tables;                  // T_w = sum of table_counts
        std::vector<int32_t> children;

        Restaurant(uint16_t ab_size)
            : customer_counts(ab_size, 0.0), table_counts(ab_size, 0.0),
              total_customers(0.0), total_tables(0.0),
              children(ab_size, -1) {}
    };

    uint16_t ab_size_;
    int max_order_;
    double discount_;       // d
    double concentration_;  // theta
    std::vector<Restaurant> nodes_;
    ContextBuffer<Symbol> context_;

    int32_t alloc_node();
    int32_t get_or_create(int32_t parent, Symbol sym);

    // Base distribution (uniform)
    double base_prob(Symbol /*sym*/) const { return 1.0 / ab_size_; }

    // Recursive PY predictive probability
    double py_predict(int32_t node_idx, Symbol sym,
                      const Symbol* ctx, int depth, int max_d) const;

    // Seat a customer using expected seating
    void seat_customer(int32_t node_idx, Symbol sym,
                       const Symbol* ctx, int depth, int max_d);

    void train_symbol(Symbol sym);
};

} // namespace vmm

#endif // SEQMEMO_PREDICTOR_H
