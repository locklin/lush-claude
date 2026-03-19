#include "bctw_predictor.h"
#include <cmath>
#include <algorithm>

namespace vmm {

BinaryCTWPredictor::BinaryCTWPredictor(uint16_t alphabet_size, int max_depth)
    : ab_size_(alphabet_size)
    , max_depth_(max_depth)
    , bits_per_symbol_(0)
    , bit_context_(max_depth * 16) // generous capacity for bit context
{
    // Compute bits per symbol
    int tmp = alphabet_size - 1;
    if (tmp <= 0) {
        bits_per_symbol_ = 1;
    } else {
        while (tmp > 0) { bits_per_symbol_++; tmp >>= 1; }
    }

    nodes_.reserve(4096);
    alloc_node(); // root = index 0
}

int32_t BinaryCTWPredictor::alloc_node() {
    int32_t idx = static_cast<int32_t>(nodes_.size());
    nodes_.emplace_back();
    return idx;
}

int32_t BinaryCTWPredictor::get_or_create_child(int32_t parent, int bit) {
    int32_t& child = bit ? nodes_[parent].child1 : nodes_[parent].child0;
    if (child == -1)
        child = alloc_node();
    return child;
}

double BinaryCTWPredictor::kt_prob(const BinaryNode& node, int bit) const {
    double total = node.count0 + node.count1;
    if (bit)
        return (node.count1 + 0.5) / (total + 1.0);
    else
        return (node.count0 + 0.5) / (total + 1.0);
}

double BinaryCTWPredictor::weighted_prob_binary(int32_t node_idx, int bit, int depth) const {
    if (node_idx == -1)
        return 0.5; // uniform binary

    const BinaryNode& node = nodes_[node_idx];
    double p_kt = kt_prob(node, bit);

    if (depth >= max_depth_ || static_cast<size_t>(depth) >= bit_context_.size())
        return p_kt;

    // Get context bit at this depth (most recent first)
    uint8_t ctx_bit = bit_context_[bit_context_.size() - depth - 1];
    int32_t child_idx = ctx_bit ? node.child1 : node.child0;
    double p_child = weighted_prob_binary(child_idx, bit, depth + 1);

    double beta_w = node.beta / (node.beta + 1.0);
    return beta_w * p_kt + (1.0 - beta_w) * p_child;
}

void BinaryCTWPredictor::train_bit(int bit) {
    // Walk down context tree, collect path
    int ctx_len = std::min(static_cast<int>(bit_context_.size()), max_depth_);
    std::vector<int32_t> path;
    path.push_back(0);

    for (int d = 0; d < ctx_len; ++d) {
        uint8_t ctx_bit = bit_context_[bit_context_.size() - d - 1];
        int32_t parent = path.back();
        int32_t child = get_or_create_child(parent, ctx_bit);
        path.push_back(child);
    }

    // Update from deepest to root
    for (int i = static_cast<int>(path.size()) - 1; i >= 0; --i) {
        BinaryNode& node = nodes_[path[i]];
        double p_kt = kt_prob(node, bit);

        if (i < static_cast<int>(path.size()) - 1) {
            uint8_t ctx_bit = bit_context_[bit_context_.size() - i - 1];
            int32_t child_idx = ctx_bit ? node.child1 : node.child0;
            double p_child = (child_idx != -1) ? kt_prob(nodes_[child_idx], bit) : 0.5;
            double beta_w = node.beta / (node.beta + 1.0);
            double p_w = beta_w * p_kt + (1.0 - beta_w) * p_child;
            if (p_w > 0.0)
                node.beta = node.beta * p_kt / p_w;
        }

        if (bit)
            node.count1 += 1.0;
        else
            node.count0 += 1.0;
    }

    bit_context_.push(static_cast<uint8_t>(bit));
}

double BinaryCTWPredictor::predict_bit(int bit) {
    return weighted_prob_binary(0, bit, 0);
}

void BinaryCTWPredictor::learn(const Symbol* data, size_t len) {
    bit_context_.clear();
    for (size_t i = 0; i < len; ++i) {
        // Decompose symbol into bits (MSB first)
        for (int b = bits_per_symbol_ - 1; b >= 0; --b) {
            int bit = (data[i] >> b) & 1;
            train_bit(bit);
        }
    }
}

double BinaryCTWPredictor::predict(Symbol symbol, const Symbol* context, size_t ctx_len) {
    // Replay context bits (non-training)
    bit_context_.clear();
    for (size_t i = 0; i < ctx_len; ++i) {
        for (int b = bits_per_symbol_ - 1; b >= 0; --b) {
            uint8_t bit = (context[i] >> b) & 1;
            bit_context_.push(bit);
        }
    }

    // Predict the target symbol bit by bit
    double p = 1.0;
    for (int b = bits_per_symbol_ - 1; b >= 0; --b) {
        int bit = (symbol >> b) & 1;
        p *= predict_bit(bit);
        bit_context_.push(static_cast<uint8_t>(bit));
    }
    return p;
}

void BinaryCTWPredictor::predict_distribution(const Symbol* context, size_t ctx_len,
                                               double* out_probs) {
    for (uint16_t s = 0; s < ab_size_; ++s)
        out_probs[s] = predict(static_cast<Symbol>(s), context, ctx_len);

    double sum = 0.0;
    for (uint16_t s = 0; s < ab_size_; ++s) sum += out_probs[s];
    if (sum > 0.0)
        for (uint16_t s = 0; s < ab_size_; ++s) out_probs[s] /= sum;
}

double BinaryCTWPredictor::log_eval(const Symbol* data, size_t len) {
    static constexpr double NEG_INV_LOG2 = -1.0 / 0.6931471805599453;
    // For proper log_eval, we need to build context incrementally
    bit_context_.clear();
    double value = 0.0;
    for (size_t i = 0; i < len; ++i) {
        double p = 1.0;
        for (int b = bits_per_symbol_ - 1; b >= 0; --b) {
            int bit = (data[i] >> b) & 1;
            p *= predict_bit(bit);
            bit_context_.push(static_cast<uint8_t>(bit));
        }
        value += std::log(p);
    }
    return value * NEG_INV_LOG2;
}

double BinaryCTWPredictor::log_eval(const Symbol* data, size_t len,
                                     const Symbol* /*ctx*/, size_t /*ctx_len*/) {
    return log_eval(data, len);
}

} // namespace vmm
