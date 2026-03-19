#ifndef BCTW_PREDICTOR_H
#define BCTW_PREDICTOR_H

#include "vmm_predictor.h"
#include "context_buffer.h"
#include <vector>
#include <cstdint>

namespace vmm {

// Binary CTW predictor.
// Matches Java BinaryCTWPredictor:
//   - Each symbol is decomposed into ceil(log2(ab_size)) bits
//   - A single binary CTW tree is used
//   - Context for each bit includes the preceding symbols' bits plus
//     the higher-order bits of the current symbol
class BinaryCTWPredictor : public VMMPredictor {
public:
    BinaryCTWPredictor(uint16_t alphabet_size, int max_depth);
    ~BinaryCTWPredictor() override = default;

    void learn(const Symbol* data, size_t len) override;
    double predict(Symbol symbol, const Symbol* context, size_t ctx_len) override;
    void predict_distribution(const Symbol* context, size_t ctx_len,
                              double* out_probs) override;
    double log_eval(const Symbol* data, size_t len) override;
    double log_eval(const Symbol* data, size_t len,
                    const Symbol* ctx, size_t ctx_len) override;
    uint16_t alphabet_size() const override { return ab_size_; }

private:
    struct BinaryNode {
        double count0, count1;  // KT estimator counts
        double beta;
        int32_t child0, child1;

        BinaryNode() : count0(0), count1(0), beta(1.0), child0(-1), child1(-1) {}
    };

    uint16_t ab_size_;
    int max_depth_;
    int bits_per_symbol_;
    std::vector<BinaryNode> nodes_;
    ContextBuffer<uint8_t> bit_context_; // binary context

    int32_t alloc_node();
    int32_t get_or_create_child(int32_t parent, int bit);
    double kt_prob(const BinaryNode& node, int bit) const;

    // Train one bit
    void train_bit(int bit);

    // Predict P(bit=1 | context)
    double predict_bit(int bit);

    // Weighted probability for binary CTW
    double weighted_prob_binary(int32_t node_idx, int bit, int depth) const;
};

} // namespace vmm

#endif // BCTW_PREDICTOR_H
