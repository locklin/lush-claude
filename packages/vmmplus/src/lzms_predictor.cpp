#include "lzms_predictor.h"
#include "context_buffer.h"
#include <cmath>
#include <algorithm>

namespace vmm {

LZmsPredictor::LZmsPredictor(uint16_t alphabet_size, int m, int s)
    : ab_size_(alphabet_size)
    , m_(m)
    , s_(s)
    , trained_(false)
{
    nodes_.reserve(1024);
    alloc_node(); // root = index 0
}

int32_t LZmsPredictor::alloc_node() {
    int32_t idx = static_cast<int32_t>(nodes_.size());
    nodes_.emplace_back(ab_size_);
    return idx;
}

void LZmsPredictor::insert_phrase(const Symbol* data, size_t len) {
    int32_t node = 0; // start at root
    nodes_[0].count++;
    for (size_t i = 0; i < len; ++i) {
        Symbol sym = data[i];
        int32_t& child = nodes_[node].children[sym];
        if (child == -1) {
            child = alloc_node();
        }
        node = child;
        nodes_[node].count++;
    }
}

void LZmsPredictor::build_trie(const Symbol* data, size_t len) {
    // Reset trie
    nodes_.clear();
    alloc_node();

    if (len == 0) return;

    // For each shift value from -m to s, parse the shifted sequence
    for (int shift = -m_; shift <= s_; ++shift) {
        int start = std::max(0, shift);
        int end = static_cast<int>(len);
        if (start >= end) continue;

        const Symbol* seq = data + start;
        int seq_len = end - start;

        // LZ78 parsing on the shifted sequence
        int pos = 0;
        while (pos < seq_len) {
            // Find the longest matching prefix in the trie
            int32_t node = 0;
            int match_len = 0;
            while (pos + match_len < seq_len) {
                Symbol sym = seq[pos + match_len];
                int32_t child = nodes_[node].children[sym];
                if (child == -1) break;
                node = child;
                match_len++;
            }

            // Insert the phrase (match + one more symbol)
            if (pos + match_len < seq_len) {
                // Phrase is match + next symbol
                insert_phrase(seq + pos, match_len + 1);
                pos += match_len + 1;
            } else {
                // No more data — insert partial phrase
                if (match_len > 0) {
                    insert_phrase(seq + pos, match_len);
                }
                break;
            }
        }
    }

    trained_ = true;
}

int32_t LZmsPredictor::find_node(const Symbol* seq, size_t len) const {
    int32_t node = 0;
    for (size_t i = 0; i < len; ++i) {
        int32_t child = nodes_[node].children[seq[i]];
        if (child == -1) return -1;
        node = child;
    }
    return node;
}

double LZmsPredictor::sequence_prob(const Symbol* seq, size_t len) const {
    // P(seq) = product of P(seq[i] | seq[0..i-1])
    //        = product of child_count / parent_count along the path
    double p = 1.0;
    int32_t node = 0;
    for (size_t i = 0; i < len; ++i) {
        int32_t child = nodes_[node].children[seq[i]];
        if (child == -1 || nodes_[node].count == 0) {
            // Symbol not in trie at this point — use uniform
            p *= 1.0 / ab_size_;
        } else {
            p *= static_cast<double>(nodes_[child].count) / nodes_[node].count;
        }
        if (child == -1) break;
        node = child;
    }
    return p;
}

void LZmsPredictor::learn(const Symbol* data, size_t len) {
    build_trie(data, len);
}

double LZmsPredictor::predict(Symbol symbol, const Symbol* context, size_t ctx_len) {
    if (!trained_) return 1.0 / ab_size_;

    // P(symbol | context) = P(context + symbol) / P(context)
    // Build the extended sequence: context + symbol
    std::vector<Symbol> extended(ctx_len + 1);
    for (size_t i = 0; i < ctx_len; ++i)
        extended[i] = context[i];
    extended[ctx_len] = symbol;

    double p_extended = sequence_prob(extended.data(), extended.size());
    double p_context = (ctx_len > 0) ? sequence_prob(context, ctx_len) : 1.0;

    if (p_context <= 0.0) return 1.0 / ab_size_;

    double result = p_extended / p_context;

    // Ensure non-negative
    if (result <= 0.0) return 1.0 / ab_size_;
    return result;
}

void LZmsPredictor::predict_distribution(const Symbol* context, size_t ctx_len,
                                          double* out_probs) {
    if (!trained_) {
        for (uint16_t s = 0; s < ab_size_; ++s)
            out_probs[s] = 1.0 / ab_size_;
        return;
    }

    double sum = 0.0;
    for (uint16_t s = 0; s < ab_size_; ++s) {
        out_probs[s] = predict(static_cast<Symbol>(s), context, ctx_len);
        sum += out_probs[s];
    }

    // Normalize
    if (sum > 0.0) {
        for (uint16_t s = 0; s < ab_size_; ++s)
            out_probs[s] /= sum;
    } else {
        for (uint16_t s = 0; s < ab_size_; ++s)
            out_probs[s] = 1.0 / ab_size_;
    }
}

double LZmsPredictor::log_eval(const Symbol* data, size_t len) {
    static constexpr double NEG_INV_LOG2 = -1.0 / 0.6931471805599453;
    ContextBuffer<Symbol> ctx(32); // reasonable context window
    double value = 0.0;
    for (size_t i = 0; i < len; ++i) {
        std::vector<Symbol> context(ctx.size());
        ctx.get_last(context.data(), ctx.size());
        double p = predict(data[i], context.data(), context.size());
        if (p > 0.0) value += std::log(p);
        ctx.push(data[i]);
    }
    return value * NEG_INV_LOG2;
}

double LZmsPredictor::log_eval(const Symbol* data, size_t len,
                                const Symbol* /*ctx*/, size_t /*ctx_len*/) {
    return log_eval(data, len);
}

} // namespace vmm
