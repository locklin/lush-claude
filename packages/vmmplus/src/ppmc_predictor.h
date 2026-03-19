#ifndef PPMC_PREDICTOR_H
#define PPMC_PREDICTOR_H

#include "vmm_predictor.h"
#include "context_buffer.h"
#include <vector>
#include <cstdint>

namespace vmm {

// PPM-C (Prediction by Partial Matching, method C) predictor.
// Matches the Begleiter/Carpenter Java implementation exactly:
//   - Context trie with move-to-front child ordering
//   - Method C escape: P(esc) = d / (T + d) where d = distinct children, T = total count
//   - Update exclusion during prediction
//   - Adaptive unigram backoff model (order -1) initialized to uniform counts of 1
//
// Training uses a separate use() pass (no probability computation).
// Prediction replays context then computes P(symbol).
class PPMCPredictor : public VMMPredictor {
public:
    PPMCPredictor(uint16_t alphabet_size, int max_order);
    ~PPMCPredictor() override = default;

    void learn(const Symbol* data, size_t len) override;
    double predict(Symbol symbol, const Symbol* context, size_t ctx_len) override;
    void predict_distribution(const Symbol* context, size_t ctx_len,
                              double* out_probs) override;
    double log_eval(const Symbol* data, size_t len) override;
    double log_eval(const Symbol* data, size_t len,
                    const Symbol* ctx, size_t ctx_len) override;
    uint16_t alphabet_size() const override { return ab_size_; }

private:
    // Trie node stored as linked-list children (matching Java PPMNode)
    struct Node {
        Symbol byte_val;
        uint16_t count;
        uint16_t num_outcomes; // number of distinct children = PPM-C escape count
        int32_t first_child;  // index into nodes_, -1 if none
        int32_t next_sibling; // index into nodes_, -1 if none
    };

    uint16_t ab_size_;
    int max_order_;

    // Trie storage
    std::vector<Node> nodes_;
    std::vector<int32_t> contexts_; // root children indexed by first symbol

    // Adaptive unigram model (order -1 backoff)
    std::vector<int32_t> unigram_counts_; // initialized to 1 for each symbol

    // Working state for prediction
    ContextBuffer<Symbol> buffer_;
    int context_length_;
    int32_t context_node_; // current deepest matching node index, -1 if none
    std::vector<bool> excluded_;

    // Internal methods
    void use_symbol(Symbol sym);          // training: update trie + backoff
    double predict_symbol(Symbol sym);    // prediction: compute P(sym)
    void clear_context();

    // Trie operations
    int32_t lookup_node(int ctx_len) const;
    int32_t lookup(int32_t node, const Symbol* syms, int len) const;
    void increment(Symbol sym);
    void node_increment(int32_t node_idx, const Symbol* bytes, int offset, int length);
    void node_complete(int32_t node_idx, const Symbol* bytes, int offset, int length);
    void rescale(int32_t node_idx);

    // Context navigation
    void get_context_node_long_to_short();
    void get_context_node_binary_search();
    bool escaped(Symbol sym) const;

    // Interval computation (PPM-C arithmetic coding intervals)
    // result[0] = low, result[1] = high, result[2] = total
    void interval_escape(int* result);
    void interval_byte(Symbol sym, int* result, bool do_increment);
    void node_interval(int32_t node_idx, Symbol sym, int* result) const;
    void node_interval_escape(int32_t node_idx, int* result) const;
    int node_total_count(int32_t node_idx) const;
    bool node_has_daughter(int32_t node_idx, Symbol sym) const;

    // Unigram backoff
    void unigram_interval(Symbol sym, int* result, bool do_increment);
    void unigram_interval_no_increment(Symbol sym, int* result) const;

    int32_t alloc_node(Symbol byte_val, int32_t next_sibling);
};

} // namespace vmm

#endif // PPMC_PREDICTOR_H
