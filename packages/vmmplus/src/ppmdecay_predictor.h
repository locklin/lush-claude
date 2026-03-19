#ifndef PPMDECAY_PREDICTOR_H
#define PPMDECAY_PREDICTOR_H

#include "vmm_predictor.h"
#include "context_buffer.h"
#include <vector>
#include <cstdint>
#include <cmath>

namespace vmm {

// PPM-C with exponential decay on counts.
// Harrison, Sherstan, Doucet & White (2020): "An entropy-based approach
// to adaptive non-stationary sequence prediction."
//
// Each node stores continuous "effective counts" that decay over time.
// Recent observations have weight ~w0, decaying to w_inf with the given
// half-life.  This makes the model adaptive to non-stationary sources.
class PPMDecayPredictor : public VMMPredictor {
public:
    PPMDecayPredictor(uint16_t alphabet_size, int max_order,
                      double w0, double w_inf, double half_life);
    ~PPMDecayPredictor() override = default;

    void learn(const Symbol* data, size_t len) override;
    double predict(Symbol symbol, const Symbol* context, size_t ctx_len) override;
    void predict_distribution(const Symbol* context, size_t ctx_len,
                              double* out_probs) override;
    double log_eval(const Symbol* data, size_t len) override;
    double log_eval(const Symbol* data, size_t len,
                    const Symbol* ctx, size_t ctx_len) override;
    uint16_t alphabet_size() const override { return ab_size_; }

private:
    struct DecayNode {
        std::vector<double> counts;     // effective counts per symbol
        std::vector<double> last_time;  // last update time per symbol
        uint16_t num_outcomes;          // distinct symbols ever seen
        std::vector<int32_t> children;  // indexed by symbol, -1 if absent

        DecayNode(uint16_t ab_size)
            : counts(ab_size, 0.0), last_time(ab_size, 0.0),
              num_outcomes(0), children(ab_size, -1) {}
    };

    uint16_t ab_size_;
    int max_order_;
    double w0_, w_inf_, decay_rate_;
    size_t global_time_;
    std::vector<DecayNode> nodes_;
    ContextBuffer<Symbol> context_;

    int32_t alloc_node();
    int32_t get_or_create(int32_t parent, Symbol sym);

    // Compute the decayed count at current global_time_
    double decayed_count(double count, double last_t) const;
    // Total decayed count at a node
    double total_decayed(const DecayNode& node) const;

    // PPM-C style prediction with decayed counts
    // Walks context from longest to shortest, using escape mechanism
    double predict_symbol(Symbol sym, const Symbol* ctx, int ctx_len) const;

    // Training: update tree along context path
    void train_symbol(Symbol sym);
};

} // namespace vmm

#endif // PPMDECAY_PREDICTOR_H
