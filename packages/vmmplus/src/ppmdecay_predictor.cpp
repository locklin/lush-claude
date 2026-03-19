#include "ppmdecay_predictor.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace vmm {

PPMDecayPredictor::PPMDecayPredictor(uint16_t alphabet_size, int max_order,
                                     double w0, double w_inf, double half_life)
    : ab_size_(alphabet_size)
    , max_order_(max_order)
    , w0_(w0)
    , w_inf_(w_inf)
    , decay_rate_(std::log(2.0) / half_life)
    , global_time_(0)
    , context_(max_order)
{
    nodes_.reserve(1024);
    alloc_node(); // root node (index 0)
}

int32_t PPMDecayPredictor::alloc_node() {
    int32_t idx = static_cast<int32_t>(nodes_.size());
    nodes_.emplace_back(ab_size_);
    return idx;
}

int32_t PPMDecayPredictor::get_or_create(int32_t parent, Symbol sym) {
    int32_t& child = nodes_[parent].children[sym];
    if (child == -1)
        child = alloc_node();
    return child;
}

double PPMDecayPredictor::decayed_count(double count, double last_t) const {
    if (count <= 0.0) return 0.0;
    double dt = static_cast<double>(global_time_) - last_t;
    // effective = w_inf + (count - w_inf) * exp(-decay_rate * dt)
    // But count already includes prior w_inf contributions, so we decay
    // the whole count toward w_inf
    double decayed = w_inf_ + (count - w_inf_) * std::exp(-decay_rate_ * dt);
    return std::max(decayed, 0.0);
}

double PPMDecayPredictor::total_decayed(const DecayNode& node) const {
    double total = 0.0;
    for (uint16_t s = 0; s < ab_size_; ++s) {
        total += decayed_count(node.counts[s], node.last_time[s]);
    }
    return total;
}

void PPMDecayPredictor::train_symbol(Symbol sym) {
    int ctx_len = std::min(static_cast<int>(context_.size()), max_order_);

    // Walk down context tree from root to deepest, collecting path
    std::vector<int32_t> path;
    path.push_back(0); // root

    for (int d = 0; d < ctx_len; ++d) {
        Symbol ctx_sym = context_[context_.size() - d - 1];
        int32_t parent = path.back();
        int32_t child = get_or_create(parent, ctx_sym);
        path.push_back(child);
    }

    // Update counts at every node along the path
    for (int32_t node_idx : path) {
        DecayNode& node = nodes_[node_idx];
        // Decay existing count to current time, then add w0
        double old = decayed_count(node.counts[sym], node.last_time[sym]);
        if (old == 0.0 && node.counts[sym] == 0.0) {
            ++node.num_outcomes;
        }
        node.counts[sym] = old + w0_;
        node.last_time[sym] = static_cast<double>(global_time_);
    }

    context_.push(sym);
    ++global_time_;
}

void PPMDecayPredictor::learn(const Symbol* data, size_t len) {
    context_.clear();
    global_time_ = 0;
    nodes_.clear();
    nodes_.reserve(1024);
    alloc_node(); // fresh root
    for (size_t i = 0; i < len; ++i)
        train_symbol(data[i]);
}

double PPMDecayPredictor::predict_symbol(Symbol sym, const Symbol* ctx,
                                          int ctx_len) const {
    // PPM-C with decayed counts: walk from deepest context to shallowest.
    // At each level, if symbol has non-zero decayed count, compute P(sym).
    // Otherwise escape to shorter context.
    //
    // P(sym | node) = decayed_count(sym) / (T_decayed + d)
    // P(esc | node) = d / (T_decayed + d)
    // where d = num_outcomes (distinct symbols ever seen)

    double p_accum = 1.0;

    // Walk the trie to find nodes at each depth
    // ctx is ordered oldest..newest, so ctx[ctx_len-1] is most recent
    for (int depth = ctx_len; depth >= 0; --depth) {
        // Find the node at this depth
        int32_t node_idx = 0; // root
        bool found_node = true;
        for (int d = 0; d < depth; ++d) {
            Symbol cs = ctx[ctx_len - depth + d];
            int32_t child = nodes_[node_idx].children[cs];
            if (child == -1) {
                found_node = false;
                break;
            }
            node_idx = child;
        }

        if (!found_node) continue;

        const DecayNode& node = nodes_[node_idx];
        double dc = decayed_count(node.counts[sym], node.last_time[sym]);
        double T = total_decayed(node);
        double d = static_cast<double>(node.num_outcomes);

        if (d == 0.0 && T == 0.0) {
            // Empty node, escape
            continue;
        }

        if (dc > 0.0) {
            // Symbol found at this depth
            double denom = T + d;
            if (denom > 0.0) {
                p_accum *= dc / denom;
            }
            return p_accum;
        } else {
            // Escape: multiply by escape probability
            double denom = T + d;
            if (denom > 0.0) {
                p_accum *= d / denom;
            }
        }
    }

    // Fell through to uniform backoff
    p_accum *= 1.0 / ab_size_;
    return p_accum;
}

double PPMDecayPredictor::predict(Symbol symbol, const Symbol* context,
                                   size_t ctx_len) {
    int depth = std::min(static_cast<int>(ctx_len), max_order_);
    if (depth == 0) {
        // No context: use root node
        const DecayNode& root = nodes_[0];
        double dc = decayed_count(root.counts[symbol], root.last_time[symbol]);
        double T = total_decayed(root);
        double d = static_cast<double>(root.num_outcomes);
        if (T + d > 0.0 && dc > 0.0) {
            return dc / (T + d);
        }
        // Escape to uniform
        double esc = (T + d > 0.0) ? d / (T + d) : 1.0;
        return esc / ab_size_;
    }
    return predict_symbol(symbol, context + ctx_len - depth, depth);
}

void PPMDecayPredictor::predict_distribution(const Symbol* context, size_t ctx_len,
                                              double* out_probs) {
    for (uint16_t s = 0; s < ab_size_; ++s)
        out_probs[s] = predict(static_cast<Symbol>(s), context, ctx_len);

    // Normalize
    double sum = 0.0;
    for (uint16_t s = 0; s < ab_size_; ++s) sum += out_probs[s];
    if (sum > 0.0) {
        for (uint16_t s = 0; s < ab_size_; ++s) out_probs[s] /= sum;
    }
}

double PPMDecayPredictor::log_eval(const Symbol* data, size_t len) {
    static constexpr double NEG_INV_LOG2 = -1.0 / 0.6931471805599453;

    // Re-train from scratch while accumulating log-loss
    context_.clear();
    global_time_ = 0;
    nodes_.clear();
    nodes_.reserve(1024);
    alloc_node();

    double value = 0.0;
    for (size_t i = 0; i < len; ++i) {
        // Predict before training on this symbol
        int ctx_len = std::min(static_cast<int>(context_.size()), max_order_);
        std::vector<Symbol> ctx(ctx_len);
        context_.get_last(ctx.data(), ctx_len);
        double p = predict(data[i], ctx.data(), ctx_len);
        if (p < 1e-300) p = 1e-300;
        value += std::log(p);
        train_symbol(data[i]);
    }
    return value * NEG_INV_LOG2;
}

double PPMDecayPredictor::log_eval(const Symbol* data, size_t len,
                                    const Symbol* /*ctx*/, size_t /*ctx_len*/) {
    return log_eval(data, len);
}

} // namespace vmm
