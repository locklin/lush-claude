#include "dctw_predictor.h"
#include <cmath>
#include <algorithm>

namespace vmm {

DCTWPredictor::DCTWPredictor(uint16_t alphabet_size, int max_depth)
    : ab_size_(alphabet_size)
    , max_depth_(max_depth)
    , bits_per_symbol_(0)
{
    int tmp = alphabet_size - 1;
    if (tmp <= 0) bits_per_symbol_ = 1;
    else { while (tmp > 0) { bits_per_symbol_++; tmp >>= 1; } }

    // Create one binary CTW model per bit position
    bit_models_.reserve(bits_per_symbol_);
    for (int i = 0; i < bits_per_symbol_; ++i)
        bit_models_.emplace_back(max_depth);
}

double DCTWPredictor::kt_prob(const BinNode& node, int bit) const {
    double total = node.count0 + node.count1;
    if (bit)
        return (node.count1 + 0.5) / (total + 1.0);
    else
        return (node.count0 + 0.5) / (total + 1.0);
}

double DCTWPredictor::weighted_prob(const BitModel& model, int32_t node_idx,
                                     int bit, int depth) const {
    if (node_idx == -1) return 0.5;

    const BinNode& node = model.nodes[node_idx];
    double p_kt = kt_prob(node, bit);

    if (depth >= max_depth_ || static_cast<size_t>(depth) >= model.context.size())
        return p_kt;

    uint8_t ctx_bit = model.context[model.context.size() - depth - 1];
    int32_t child_idx = ctx_bit ? node.child1 : node.child0;
    double p_child = weighted_prob(model, child_idx, bit, depth + 1);

    double beta_w = node.beta / (node.beta + 1.0);
    return beta_w * p_kt + (1.0 - beta_w) * p_child;
}

void DCTWPredictor::train_bit(BitModel& model, int bit) {
    int ctx_len = std::min(static_cast<int>(model.context.size()), max_depth_);
    std::vector<int32_t> path;
    path.push_back(0); // root

    for (int d = 0; d < ctx_len; ++d) {
        uint8_t ctx_bit = model.context[model.context.size() - d - 1];
        int32_t parent = path.back();
        int32_t& child = ctx_bit ? model.nodes[parent].child1
                                 : model.nodes[parent].child0;
        if (child == -1) child = model.alloc_node();
        path.push_back(child);
    }

    for (int i = static_cast<int>(path.size()) - 1; i >= 0; --i) {
        BinNode& node = model.nodes[path[i]];
        double p_kt = kt_prob(node, bit);

        if (i < static_cast<int>(path.size()) - 1) {
            uint8_t ctx_bit = model.context[model.context.size() - i - 1];
            int32_t child_idx = ctx_bit ? node.child1 : node.child0;
            double p_child = (child_idx != -1) ? kt_prob(model.nodes[child_idx], bit) : 0.5;
            double beta_w = node.beta / (node.beta + 1.0);
            double p_w = beta_w * p_kt + (1.0 - beta_w) * p_child;
            if (p_w > 0.0) node.beta = node.beta * p_kt / p_w;
        }

        if (bit) node.count1 += 1.0;
        else     node.count0 += 1.0;
    }

    model.context.push(static_cast<uint8_t>(bit));
}

double DCTWPredictor::predict_bit(const BitModel& model, int bit) const {
    return weighted_prob(model, 0, bit, 0);
}

void DCTWPredictor::learn(const Symbol* data, size_t len) {
    for (auto& m : bit_models_) m.context.clear();
    for (size_t i = 0; i < len; ++i) {
        // Train each bit position's model with the corresponding bit
        for (int b = 0; b < bits_per_symbol_; ++b) {
            int bit_pos = bits_per_symbol_ - 1 - b; // MSB first
            int bit = (data[i] >> bit_pos) & 1;
            train_bit(bit_models_[b], bit);
        }
    }
}

double DCTWPredictor::predict(Symbol symbol, const Symbol* context, size_t ctx_len) {
    // Set up each bit model's context from the context symbols
    for (auto& m : bit_models_) m.context.clear();
    for (size_t i = 0; i < ctx_len; ++i) {
        for (int b = 0; b < bits_per_symbol_; ++b) {
            int bit_pos = bits_per_symbol_ - 1 - b;
            int bit = (context[i] >> bit_pos) & 1;
            bit_models_[b].context.push(static_cast<uint8_t>(bit));
        }
    }

    // Predict: P(symbol) = product of P(bit_b | bit_model_b) for each bit position
    double p = 1.0;
    for (int b = 0; b < bits_per_symbol_; ++b) {
        int bit_pos = bits_per_symbol_ - 1 - b;
        int bit = (symbol >> bit_pos) & 1;
        p *= predict_bit(bit_models_[b], bit);
    }
    return p;
}

void DCTWPredictor::predict_distribution(const Symbol* context, size_t ctx_len,
                                          double* out_probs) {
    for (uint16_t s = 0; s < ab_size_; ++s)
        out_probs[s] = predict(static_cast<Symbol>(s), context, ctx_len);

    double sum = 0.0;
    for (uint16_t s = 0; s < ab_size_; ++s) sum += out_probs[s];
    if (sum > 0.0)
        for (uint16_t s = 0; s < ab_size_; ++s) out_probs[s] /= sum;
}

double DCTWPredictor::log_eval(const Symbol* data, size_t len) {
    static constexpr double NEG_INV_LOG2 = -1.0 / 0.6931471805599453;
    for (auto& m : bit_models_) m.context.clear();
    double value = 0.0;
    for (size_t i = 0; i < len; ++i) {
        double p = 1.0;
        for (int b = 0; b < bits_per_symbol_; ++b) {
            int bit_pos = bits_per_symbol_ - 1 - b;
            int bit = (data[i] >> bit_pos) & 1;
            p *= predict_bit(bit_models_[b], bit);
            bit_models_[b].context.push(static_cast<uint8_t>(bit));
        }
        value += std::log(p);
    }
    return value * NEG_INV_LOG2;
}

double DCTWPredictor::log_eval(const Symbol* data, size_t len,
                                const Symbol* /*ctx*/, size_t /*ctx_len*/) {
    return log_eval(data, len);
}

} // namespace vmm
