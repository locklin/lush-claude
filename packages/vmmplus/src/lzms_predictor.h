#ifndef LZMS_PREDICTOR_H
#define LZMS_PREDICTOR_H

#include "vmm_predictor.h"
#include <vector>
#include <cstdint>

namespace vmm {

// LZms predictor (LZ with model selection).
// Matches Java LZmsPredictor / LZmsTree:
//   - Builds an LZ78-style trie from training data with m backward shifts and s forward shifts
//   - Prediction: P(a|X) = P(Xa) / P(X) using trie statistics
//   - LZ78 is the special case with m=0, s=0
class LZmsPredictor : public VMMPredictor {
public:
    // m: minimum context length (backward shifts)
    // s: number of forward shifts
    LZmsPredictor(uint16_t alphabet_size, int m, int s);
    ~LZmsPredictor() override = default;

    void learn(const Symbol* data, size_t len) override;
    double predict(Symbol symbol, const Symbol* context, size_t ctx_len) override;
    void predict_distribution(const Symbol* context, size_t ctx_len,
                              double* out_probs) override;
    double log_eval(const Symbol* data, size_t len) override;
    double log_eval(const Symbol* data, size_t len,
                    const Symbol* ctx, size_t ctx_len) override;
    uint16_t alphabet_size() const override { return ab_size_; }

private:
    struct LZNode {
        int32_t count;                 // number of times this node was visited
        std::vector<int32_t> children; // indexed by symbol, -1 if absent
        LZNode(uint16_t ab_size) : count(0), children(ab_size, -1) {}
    };

    uint16_t ab_size_;
    int m_;
    int s_;
    std::vector<LZNode> nodes_;
    bool trained_;

    int32_t alloc_node();

    // Insert a sequence into the LZ trie
    void insert_phrase(const Symbol* data, size_t len);

    // Build the trie from training data using LZ78 parsing with shifts
    void build_trie(const Symbol* data, size_t len);

    // Find the deepest matching node for a context
    int32_t find_node(const Symbol* seq, size_t len) const;

    // P(sequence) from the trie = product of child_count/parent_count along path
    double sequence_prob(const Symbol* seq, size_t len) const;
};

} // namespace vmm

#endif // LZMS_PREDICTOR_H
