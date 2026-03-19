#ifndef MDI_PREDICTOR_H
#define MDI_PREDICTOR_H

#include "vmm_predictor.h"
#include "context_buffer.h"
#include <vector>
#include <cstdint>

namespace vmm {

// MDI (Merge-and-Diverge Identification) predictor.
// Thollard, Dupont & de la Higuera (2000): "Probabilistic DFA Inference
// using Kullback-Leibler Divergence and Minimality."
//
// Two-phase algorithm:
// 1. Build phase: construct a full context tree (trie) like PPM-C.
// 2. Merge phase: iteratively merge nodes whose conditional distributions
//    have symmetric KL-divergence below merge_threshold.
//
// After merging, prediction uses PPM-C-style escape on the merged trie.
class MDIPredictor : public VMMPredictor {
public:
    MDIPredictor(uint16_t alphabet_size, int max_order, double merge_threshold);
    ~MDIPredictor() override = default;

    void learn(const Symbol* data, size_t len) override;
    double predict(Symbol symbol, const Symbol* context, size_t ctx_len) override;
    void predict_distribution(const Symbol* context, size_t ctx_len,
                              double* out_probs) override;
    double log_eval(const Symbol* data, size_t len) override;
    double log_eval(const Symbol* data, size_t len,
                    const Symbol* ctx, size_t ctx_len) override;
    uint16_t alphabet_size() const override { return ab_size_; }

private:
    struct MDINode {
        std::vector<double> counts;
        std::vector<int32_t> children;  // indexed by symbol
        int32_t merged_into;            // -1 if not merged, else target idx

        MDINode(uint16_t ab_size)
            : counts(ab_size, 0.0), children(ab_size, -1), merged_into(-1) {}
    };

    uint16_t ab_size_;
    int max_order_;
    double merge_threshold_;
    std::vector<MDINode> nodes_;
    bool merged_;
    ContextBuffer<Symbol> context_;

    int32_t alloc_node();
    int32_t get_or_create(int32_t parent, Symbol sym);

    // Build phase: add a symbol at each context depth
    void build_symbol(Symbol sym);

    // Merge phase
    void merge_states();
    double symmetric_kl(int32_t a_idx, int32_t b_idx) const;
    int32_t resolve(int32_t idx) const;  // follow merged_into chain

    // Prediction on merged trie (PPM-C style escape)
    double predict_from_trie(Symbol sym, const Symbol* ctx, int ctx_len) const;
};

} // namespace vmm

#endif // MDI_PREDICTOR_H
