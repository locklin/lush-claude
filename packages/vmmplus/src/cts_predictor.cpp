#include "cts_predictor.h"
#include <cmath>
#include <algorithm>
#include <limits>

namespace vmm {

CTSPredictor::CTSPredictor(uint16_t alphabet_size, int max_depth,
                           double switch_rate)
    : ab_size_(alphabet_size)
    , max_depth_(max_depth)
    , switch_rate_(switch_rate)
    , context_(max_depth)
{
    nodes_.reserve(1024);
    alloc_node(); // root node (index 0)
}

int32_t CTSPredictor::alloc_node() {
    int32_t idx = static_cast<int32_t>(nodes_.size());
    nodes_.emplace_back(ab_size_);
    return idx;
}

int32_t CTSPredictor::get_or_create(int32_t parent, Symbol sym) {
    int32_t& child = nodes_[parent].children[sym];
    if (child == -1)
        child = alloc_node();
    return child;
}

double CTSPredictor::kt_probability(const CTSNode& node, Symbol sym) const {
    double total = 0.0;
    for (uint16_t i = 0; i < ab_size_; ++i)
        total += node.counts[i];
    return (node.counts[sym] + 0.5) / (total + ab_size_ * 0.5);
}

double CTSPredictor::log_sum_exp(double log_a, double log_b) {
    if (log_a == -std::numeric_limits<double>::infinity()) return log_b;
    if (log_b == -std::numeric_limits<double>::infinity()) return log_a;
    double m = std::max(log_a, log_b);
    return m + std::log(std::exp(log_a - m) + std::exp(log_b - m));
}

double CTSPredictor::local_weight(const CTSNode& node) const {
    // w_local = exp(log_local) / (exp(log_local) + exp(log_switch))
    // Computed via log-sum-exp for numerical stability
    double log_total = log_sum_exp(node.log_local_prob, node.log_switch_prob);
    return std::exp(node.log_local_prob - log_total);
}

double CTSPredictor::weighted_prob(int32_t node_idx, Symbol sym,
                                    const Symbol* ctx, int depth, int max_d) const {
    if (node_idx == -1) {
        return 1.0 / ab_size_;
    }

    const CTSNode& node = nodes_[node_idx];
    double p_kt = kt_probability(node, sym);

    if (depth >= max_d) {
        // Leaf: return KT estimate only
        return p_kt;
    }

    // Children probability
    Symbol ctx_sym = ctx[max_d - depth - 1];
    int32_t child_idx = node.children[ctx_sym];
    double p_children = weighted_prob(child_idx, sym, ctx, depth + 1, max_d);

    // CTS mixture: w_local * P_kt + (1 - w_local) * P_children
    double w = local_weight(node);
    return w * p_kt + (1.0 - w) * p_children;
}

void CTSPredictor::train_symbol(Symbol sym) {
    int ctx_len = std::min(static_cast<int>(context_.size()), max_depth_);

    // Collect path from root to deepest context node
    std::vector<int32_t> path;
    path.push_back(0);

    for (int d = 0; d < ctx_len; ++d) {
        Symbol ctx_sym = context_[context_.size() - d - 1];
        int32_t parent = path.back();
        int32_t child = get_or_create(parent, ctx_sym);
        path.push_back(child);
    }

    // Update from deepest to root
    for (int i = static_cast<int>(path.size()) - 1; i >= 0; --i) {
        CTSNode& node = nodes_[path[i]];
        double p_kt = kt_probability(node, sym);
        double log_p_kt = std::log(std::max(p_kt, 1e-300));

        if (i == static_cast<int>(path.size()) - 1 || i >= max_depth_) {
            // Leaf node: only local model applies
            node.log_local_prob += log_p_kt;
            node.log_switch_prob += log_p_kt;
        } else {
            // Internal node: CTS switching update
            // Compute p_children from the child's KT estimate
            Symbol ctx_sym = context_[context_.size() - i - 1];
            int32_t child_idx = node.children[ctx_sym];
            double p_child;
            if (child_idx != -1) {
                p_child = kt_probability(nodes_[child_idx], sym);
            } else {
                p_child = 1.0 / ab_size_;
            }
            double log_p_child = std::log(std::max(p_child, 1e-300));

            // CTS switching recursion (Veness et al.):
            // P_switch(x_t) = (1-gamma) * P_same(x_t) + gamma * P_other(x_t)
            //
            // The switching model tracks two hypotheses:
            // - "currently local": switches to children with prob gamma
            // - "currently children": switches to local with prob gamma
            //
            // For simplicity, we compute:
            // log_switch_new = log[ (1-gamma)*exp(log_switch_old + log_p_child)
            //                     + gamma*exp(log_local_old + log_p_child) ]
            // (i.e., the switching model uses children when "same" = children,
            //  and also gets contribution from local switching to children)
            //
            // But the full CTS tracks the mixture weight dynamically.
            // A practical approximation: use the Volf-style beta update
            // but with a switching discount.
            //
            // Following Veness (2012) more precisely:
            // w_t = (1-gamma) * w_{t-1} * p_kt / p_w  +  gamma * p_kt / p_w
            // where p_w = w_{t-1} * p_kt + (1-w_{t-1}) * p_child
            //
            // We implement this in log-space via accumulated log probabilities.

            double w = local_weight(node);

            // Mixed probability under current weighting
            double p_mix = w * p_kt + (1.0 - w) * p_child;
            double log_p_mix = std::log(std::max(p_mix, 1e-300));

            // Update log_local: log_local += log(p_kt)
            node.log_local_prob += log_p_kt;

            // Update log_switch: the switching model's total probability is
            // p_switch = (1-gamma)*[w*p_kt + (1-w)*p_child] + gamma*[(1-w)*p_kt + w*p_child]
            // Simplify: p_switch = p_kt * [(1-gamma)*w + gamma*(1-w)]
            //                    + p_child * [(1-gamma)*(1-w) + gamma*w]
            // This mixes more toward uniform weighting with higher gamma.
            double p_switch_sym = p_kt * ((1.0 - switch_rate_) * w + switch_rate_ * (1.0 - w))
                                + p_child * ((1.0 - switch_rate_) * (1.0 - w) + switch_rate_ * w);
            node.log_switch_prob += std::log(std::max(p_switch_sym, 1e-300));
        }

        // Update KT counts
        node.counts[sym] += 1.0;
    }

    context_.push(sym);
}

void CTSPredictor::learn(const Symbol* data, size_t len) {
    context_.clear();
    nodes_.clear();
    nodes_.reserve(1024);
    alloc_node();
    for (size_t i = 0; i < len; ++i)
        train_symbol(data[i]);
}

double CTSPredictor::predict(Symbol symbol, const Symbol* context,
                              size_t ctx_len) {
    int depth = std::min(static_cast<int>(ctx_len), max_depth_);
    if (depth == 0) {
        return kt_probability(nodes_[0], symbol);
    }
    return weighted_prob(0, symbol, context + ctx_len - depth, 0, depth);
}

void CTSPredictor::predict_distribution(const Symbol* context, size_t ctx_len,
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

double CTSPredictor::log_eval(const Symbol* data, size_t len) {
    static constexpr double NEG_INV_LOG2 = -1.0 / 0.6931471805599453;
    context_.clear();
    nodes_.clear();
    nodes_.reserve(1024);
    alloc_node();

    double value = 0.0;
    for (size_t i = 0; i < len; ++i) {
        int ctx_len = std::min(static_cast<int>(context_.size()), max_depth_);
        std::vector<Symbol> ctx(ctx_len);
        context_.get_last(ctx.data(), ctx_len);
        double p = predict(data[i], ctx.data(), ctx_len);
        if (p < 1e-300) p = 1e-300;
        value += std::log(p);
        train_symbol(data[i]);
    }
    return value * NEG_INV_LOG2;
}

double CTSPredictor::log_eval(const Symbol* data, size_t len,
                               const Symbol* /*ctx*/, size_t /*ctx_len*/) {
    return log_eval(data, len);
}

} // namespace vmm
