#ifndef PST_PREDICTOR_H
#define PST_PREDICTOR_H

#include "vmm_predictor.h"
#include "context_buffer.h"
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace vmm {

// Probabilistic Suffix Tree (PST) predictor.
// Matches Java PSTPredictor / PSTBuilder:
//   - Build a suffix tree from training data
//   - Prune using statistical tests: pmin, alpha, gamma, r
//   - Variable-depth contexts based on significance
//   - Prediction walks the longest matching suffix
class PSTPredictor : public VMMPredictor {
public:
    // pmin: minimum probability threshold
    // alpha: significance for initial count check
    // gamma: smoothing / minimum probability in leaves
    // r: ratio test threshold for distribution difference
    PSTPredictor(uint16_t alphabet_size, int max_order,
                 double pmin, double alpha, double gamma_param, double r);
    ~PSTPredictor() override = default;

    void learn(const Symbol* data, size_t len) override;
    double predict(Symbol symbol, const Symbol* context, size_t ctx_len) override;
    void predict_distribution(const Symbol* context, size_t ctx_len,
                              double* out_probs) override;
    double log_eval(const Symbol* data, size_t len) override;
    double log_eval(const Symbol* data, size_t len,
                    const Symbol* ctx, size_t ctx_len) override;
    uint16_t alphabet_size() const override { return ab_size_; }

private:
    struct PSTNode {
        std::vector<double> probs;    // P(symbol | this context), size = ab_size
        std::vector<int32_t> children; // indexed by symbol, -1 if absent
        int32_t count;                // how many times this context appeared

        PSTNode(uint16_t ab_size) : probs(ab_size, 0.0), children(ab_size, -1), count(0) {}
    };

    uint16_t ab_size_;
    int max_order_;
    double pmin_;
    double alpha_;
    double gamma_;
    double r_;
    std::vector<PSTNode> nodes_;
    bool trained_;

    // Training data (stored for building the tree)
    std::vector<Symbol> train_data_;

    int32_t alloc_node();
    void build_pst();

    // Count occurrences of a context in training data
    int count_context(const Symbol* ctx, int ctx_len) const;
    // Count occurrences of symbol following context
    int count_symbol_after_context(Symbol sym, const Symbol* ctx, int ctx_len) const;

    // Walk PST to find the deepest matching node
    int32_t find_deepest_match(const Symbol* context, size_t ctx_len) const;
};

} // namespace vmm

#endif // PST_PREDICTOR_H
