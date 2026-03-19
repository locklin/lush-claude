#include "ctw_predictor.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace vmm {

CTWVolfPredictor::CTWVolfPredictor(uint16_t alphabet_size, int max_depth)
    : ab_size_(alphabet_size)
    , max_depth_(max_depth)
    , context_(max_depth)
{
    nodes_.reserve(1024);
    // Create root node (index 0)
    alloc_node();
}

int32_t CTWVolfPredictor::alloc_node() {
    int32_t idx = static_cast<int32_t>(nodes_.size());
    nodes_.emplace_back(ab_size_);
    return idx;
}

int32_t CTWVolfPredictor::get_or_create(int32_t parent, Symbol sym) {
    int32_t& child = nodes_[parent].children[sym];
    if (child == -1)
        child = alloc_node();
    return child;
}

double CTWVolfPredictor::kt_probability(const VolfNode& node, Symbol sym) const {
    double total = 0.0;
    for (uint16_t i = 0; i < ab_size_; ++i)
        total += node.counts[i];
    // KT estimator: (count + 0.5) / (total + ab_size * 0.5)
    return (node.counts[sym] + 0.5) / (total + ab_size_ * 0.5);
}

// Matches Java CTWVolfModel.predict():
// The Volf variant uses per-node beta weights for the Bayesian mixture.
// P_w(sym | node) = beta * P_kt(sym | node) + (1-beta) * prod_children P_w(sym | child)
// For leaf nodes (at max depth), P_w = P_kt.
double CTWVolfPredictor::weighted_prob(int32_t node_idx, Symbol sym,
                                       const Symbol* ctx, int depth, int max_d) const {
    if (node_idx == -1) {
        // Non-existent node — return uniform
        return 1.0 / ab_size_;
    }

    const VolfNode& node = nodes_[node_idx];
    double p_kt = kt_probability(node, sym);

    if (depth >= max_d) {
        // Leaf node — return KT estimate only
        return p_kt;
    }

    // Get the context symbol at this depth
    // ctx is ordered oldest..newest. For depth d from the root:
    //   the context symbol is ctx[max_d - depth - 1] (deepest context first)
    Symbol ctx_sym = ctx[max_d - depth - 1];
    int32_t child_idx = node.children[ctx_sym];

    // Children probability: P_w at the child node for the next depth
    double p_children = weighted_prob(child_idx, sym, ctx, depth + 1, max_d);

    // Volf mixture: beta * P_kt + (1 - beta) * P_children
    // But beta is normalized: beta_w = beta / (beta + 1), (1-beta_w) = 1 / (beta + 1)
    double beta_w = node.beta / (node.beta + 1.0);
    return beta_w * p_kt + (1.0 - beta_w) * p_children;
}

void CTWVolfPredictor::train_symbol(Symbol sym) {
    // Walk down the context tree from root to the deepest context node
    // Update KT counts and beta at each level
    int ctx_len = std::min(static_cast<int>(context_.size()), max_depth_);

    // Collect the path of nodes from root to deepest
    std::vector<int32_t> path;
    path.push_back(0); // root

    for (int d = 0; d < ctx_len; ++d) {
        // Context symbol at depth d: context_[ctx_len - d - 1] (most recent first)
        Symbol ctx_sym = context_[context_.size() - d - 1];
        int32_t parent = path.back();
        int32_t child = get_or_create(parent, ctx_sym);
        path.push_back(child);
    }

    // Update from deepest to root
    // At each node, update beta based on the KT probability vs children probability
    for (int i = static_cast<int>(path.size()) - 1; i >= 0; --i) {
        VolfNode& node = nodes_[path[i]];
        double p_kt = kt_probability(node, sym);

        if (i == static_cast<int>(path.size()) - 1 || i >= max_depth_) {
            // Leaf or at max depth — beta update uses kt only
            // Update counts
            node.counts[sym] += 1.0;
        } else {
            // Internal node — update beta using Bayes rule
            // beta_new = beta * P_kt / P_w
            // where P_w = beta/(beta+1) * P_kt + 1/(beta+1) * P_children
            Symbol ctx_sym = context_[context_.size() - i - 1];
            int32_t child_idx = node.children[ctx_sym];
            double p_child_kt = (child_idx != -1)
                ? kt_probability(nodes_[child_idx], sym) : (1.0 / ab_size_);

            double beta_w = node.beta / (node.beta + 1.0);
            double p_w = beta_w * p_kt + (1.0 - beta_w) * p_child_kt;

            if (p_w > 0.0)
                node.beta = node.beta * p_kt / p_w;

            node.counts[sym] += 1.0;
        }
    }

    context_.push(sym);
}

void CTWVolfPredictor::learn(const Symbol* data, size_t len) {
    context_.clear();
    for (size_t i = 0; i < len; ++i)
        train_symbol(data[i]);
}

double CTWVolfPredictor::predict(Symbol symbol, const Symbol* context, size_t ctx_len) {
    int depth = std::min(static_cast<int>(ctx_len), max_depth_);
    if (depth == 0) {
        // No context — use root KT estimator
        return kt_probability(nodes_[0], symbol);
    }
    return weighted_prob(0, symbol, context + ctx_len - depth, 0, depth);
}

void CTWVolfPredictor::predict_distribution(const Symbol* context, size_t ctx_len,
                                             double* out_probs) {
    for (uint16_t s = 0; s < ab_size_; ++s)
        out_probs[s] = predict(static_cast<Symbol>(s), context, ctx_len);

    // Normalize (should already be close to 1, but ensure exactness)
    double sum = 0.0;
    for (uint16_t s = 0; s < ab_size_; ++s) sum += out_probs[s];
    if (sum > 0.0) {
        for (uint16_t s = 0; s < ab_size_; ++s) out_probs[s] /= sum;
    }
}

double CTWVolfPredictor::log_eval(const Symbol* data, size_t len) {
    static constexpr double NEG_INV_LOG2 = -1.0 / 0.6931471805599453;
    context_.clear();
    double value = 0.0;
    for (size_t i = 0; i < len; ++i) {
        // Build context from preceding symbols
        int ctx_len = std::min(static_cast<int>(context_.size()), max_depth_);
        std::vector<Symbol> ctx(ctx_len);
        context_.get_last(ctx.data(), ctx_len);
        double p = predict(data[i], ctx.data(), ctx_len);
        value += std::log(p);
        context_.push(data[i]);
    }
    return value * NEG_INV_LOG2;
}

double CTWVolfPredictor::log_eval(const Symbol* data, size_t len,
                                   const Symbol* /*ctx*/, size_t /*ctx_len*/) {
    return log_eval(data, len);
}

} // namespace vmm
