#ifndef DCTW_PREDICTOR_H
#define DCTW_PREDICTOR_H

#include "vmm_predictor.h"
#include "context_buffer.h"
#include <vector>
#include <cstdint>

namespace vmm {

// Decomposed CTW (DCTW) predictor.
// Matches Java DCTWPredictor / DecompositionTreeBuilder:
//   - Builds a binary decomposition tree over the alphabet
//   - Each internal node in the decomposition tree has its own binary CTW model
//   - P(symbol) = product of binary CTW decisions along path to symbol's leaf
//   - Context for each binary CTW includes the same-bit-position bits of preceding symbols
class DCTWPredictor : public VMMPredictor {
public:
    DCTWPredictor(uint16_t alphabet_size, int max_depth);
    ~DCTWPredictor() override = default;

    void learn(const Symbol* data, size_t len) override;
    double predict(Symbol symbol, const Symbol* context, size_t ctx_len) override;
    void predict_distribution(const Symbol* context, size_t ctx_len,
                              double* out_probs) override;
    double log_eval(const Symbol* data, size_t len) override;
    double log_eval(const Symbol* data, size_t len,
                    const Symbol* ctx, size_t ctx_len) override;
    uint16_t alphabet_size() const override { return ab_size_; }

private:
    // Binary CTW node for one bit position
    struct BinNode {
        double count0, count1;
        double beta;
        int32_t child0, child1;
        BinNode() : count0(0), count1(0), beta(1.0), child0(-1), child1(-1) {}
    };

    // Each bit position has its own binary CTW tree
    struct BitModel {
        std::vector<BinNode> nodes;
        ContextBuffer<uint8_t> context;

        BitModel(int ctx_cap) : context(ctx_cap) {
            nodes.reserve(256);
            // Create root
            nodes.emplace_back();
        }

        int32_t alloc_node() {
            int32_t idx = static_cast<int32_t>(nodes.size());
            nodes.emplace_back();
            return idx;
        }
    };

    uint16_t ab_size_;
    int max_depth_;
    int bits_per_symbol_;
    std::vector<BitModel> bit_models_; // one per bit position

    // Binary CTW operations for a single bit model
    double kt_prob(const BinNode& node, int bit) const;
    double weighted_prob(const BitModel& model, int32_t node_idx,
                         int bit, int depth) const;
    void train_bit(BitModel& model, int bit);
    double predict_bit(const BitModel& model, int bit) const;
};

} // namespace vmm

#endif // DCTW_PREDICTOR_H
