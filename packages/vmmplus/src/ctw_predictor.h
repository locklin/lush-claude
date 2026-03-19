#ifndef CTW_PREDICTOR_H
#define CTW_PREDICTOR_H

#include "vmm_predictor.h"
#include "context_buffer.h"
#include <vector>
#include <cstdint>

namespace vmm {

// Context Tree Weighting (Volf variant) predictor.
// Matches the Begleiter/Java CTWVolfModel:
//   - Full context tree of depth D with alphabet_size children per node
//   - Each node has a KT estimator and a beta-weighted mixture
//   - Beta parameter updated online using Bayesian rule
//   - Prediction: weighted mixture of local KT estimate and children product
class CTWVolfPredictor : public VMMPredictor {
public:
    CTWVolfPredictor(uint16_t alphabet_size, int max_depth);
    ~CTWVolfPredictor() override = default;

    void learn(const Symbol* data, size_t len) override;
    double predict(Symbol symbol, const Symbol* context, size_t ctx_len) override;
    void predict_distribution(const Symbol* context, size_t ctx_len,
                              double* out_probs) override;
    double log_eval(const Symbol* data, size_t len) override;
    double log_eval(const Symbol* data, size_t len,
                    const Symbol* ctx, size_t ctx_len) override;
    uint16_t alphabet_size() const override { return ab_size_; }

private:
    // VolfNode: stores KT estimator counts and beta mixture weight
    struct VolfNode {
        std::vector<double> counts;   // KT estimator counts (size = ab_size)
        double beta;                  // mixture weight: P(local) vs P(children)
        std::vector<int32_t> children; // indices into nodes_ (size = ab_size, -1 if absent)

        VolfNode(uint16_t ab_size)
            : counts(ab_size, 0.0), beta(1.0), children(ab_size, -1) {}
    };

    uint16_t ab_size_;
    int max_depth_;
    std::vector<VolfNode> nodes_;
    ContextBuffer<Symbol> context_;

    int32_t alloc_node();
    int32_t get_or_create(int32_t parent, Symbol sym);

    // KT estimator: P(symbol | counts) = (count[symbol] + 0.5) / (total + ab_size/2)
    double kt_probability(const VolfNode& node, Symbol sym) const;

    // Recursive weighted probability
    double weighted_prob(int32_t node_idx, Symbol sym, const Symbol* ctx,
                         int depth, int max_d) const;

    // Training: update tree from leaf to root
    void train_symbol(Symbol sym);
};

} // namespace vmm

#endif // CTW_PREDICTOR_H
