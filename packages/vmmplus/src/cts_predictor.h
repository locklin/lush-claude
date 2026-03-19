#ifndef CTS_PREDICTOR_H
#define CTS_PREDICTOR_H

#include "vmm_predictor.h"
#include "context_buffer.h"
#include <vector>
#include <cstdint>

namespace vmm {

// Context Tree Switching predictor.
// Veness, Ng, Hutter & Silver (2012): "Context Tree Switching."
//
// Drop-in replacement for CTW's Bayesian weighting that allows the data
// source to *switch* between local and children models over time.
// With switch_rate=0 this degenerates to standard CTW.
class CTSPredictor : public VMMPredictor {
public:
    CTSPredictor(uint16_t alphabet_size, int max_depth, double switch_rate);
    ~CTSPredictor() override = default;

    void learn(const Symbol* data, size_t len) override;
    double predict(Symbol symbol, const Symbol* context, size_t ctx_len) override;
    void predict_distribution(const Symbol* context, size_t ctx_len,
                              double* out_probs) override;
    double log_eval(const Symbol* data, size_t len) override;
    double log_eval(const Symbol* data, size_t len,
                    const Symbol* ctx, size_t ctx_len) override;
    uint16_t alphabet_size() const override { return ab_size_; }

private:
    struct CTSNode {
        std::vector<double> counts;       // KT estimator counts
        double log_local_prob;            // accumulated log P_local
        double log_switch_prob;           // accumulated log P_switch
        std::vector<int32_t> children;

        CTSNode(uint16_t ab_size)
            : counts(ab_size, 0.0), log_local_prob(0.0),
              log_switch_prob(0.0), children(ab_size, -1) {}
    };

    uint16_t ab_size_;
    int max_depth_;
    double switch_rate_;  // gamma in Veness et al.
    std::vector<CTSNode> nodes_;
    ContextBuffer<Symbol> context_;

    int32_t alloc_node();
    int32_t get_or_create(int32_t parent, Symbol sym);

    // KT estimator probability
    double kt_probability(const CTSNode& node, Symbol sym) const;

    // Weighted prediction mixing local and children
    double weighted_prob(int32_t node_idx, Symbol sym,
                         const Symbol* ctx, int depth, int max_d) const;

    // Compute log-sum-exp of two log values
    static double log_sum_exp(double log_a, double log_b);

    // Get the local weight w_local from log probs
    double local_weight(const CTSNode& node) const;

    // Training: update tree along context path
    void train_symbol(Symbol sym);
};

} // namespace vmm

#endif // CTS_PREDICTOR_H
