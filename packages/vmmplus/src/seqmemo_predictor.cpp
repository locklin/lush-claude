#include "seqmemo_predictor.h"
#include <cmath>
#include <algorithm>

namespace vmm {

SeqMemoPredictor::SeqMemoPredictor(uint16_t alphabet_size, int max_order,
                                   double discount, double concentration)
    : ab_size_(alphabet_size)
    , max_order_(max_order)
    , discount_(discount)
    , concentration_(concentration)
    , context_(max_order)
{
    nodes_.reserve(1024);
    alloc_node(); // root node (index 0)
}

int32_t SeqMemoPredictor::alloc_node() {
    int32_t idx = static_cast<int32_t>(nodes_.size());
    nodes_.emplace_back(ab_size_);
    return idx;
}

int32_t SeqMemoPredictor::get_or_create(int32_t parent, Symbol sym) {
    int32_t& child = nodes_[parent].children[sym];
    if (child == -1)
        child = alloc_node();
    return child;
}

double SeqMemoPredictor::py_predict(int32_t node_idx, Symbol sym,
                                     const Symbol* ctx, int depth, int max_d) const {
    if (node_idx == -1) {
        // Non-existent node: fall through to parent recursion
        // If we're at depth 0 with no node, use base distribution
        return base_prob(sym);
    }

    const Restaurant& r = nodes_[node_idx];

    // Get parent probability recursively
    double p_parent;
    if (depth >= max_d) {
        // At the deepest level or beyond: parent is either a shallower node or base
        p_parent = base_prob(sym);
    } else {
        // Go deeper into the context tree
        Symbol ctx_sym = ctx[max_d - depth - 1];
        int32_t child_idx = r.children[ctx_sym];
        p_parent = py_predict(child_idx, sym, ctx, depth + 1, max_d);
    }

    // PY predictive formula:
    // P(sym) = (c(sym) - d*t(sym) + (theta + d*T)*P_parent(sym)) / (C + theta)
    double C = r.total_customers;
    double T = r.total_tables;
    double c_sym = r.customer_counts[sym];
    double t_sym = r.table_counts[sym];

    if (C + concentration_ <= 0.0) {
        return p_parent;
    }

    double numerator = std::max(c_sym - discount_ * t_sym, 0.0)
                     + (concentration_ + discount_ * T) * p_parent;
    double denominator = C + concentration_;

    return numerator / denominator;
}

void SeqMemoPredictor::seat_customer(int32_t node_idx, Symbol sym,
                                      const Symbol* ctx, int depth, int max_d) {
    Restaurant& r = nodes_[node_idx];
    double C = r.total_customers;
    double T = r.total_tables;

    // Compute probability of opening a new table
    // P(new table) = (theta + d*T) * P_parent(sym) / (C + theta)
    double p_parent;
    if (depth >= max_d) {
        p_parent = base_prob(sym);
    } else {
        Symbol ctx_sym = ctx[max_d - depth - 1];
        int32_t child_idx = r.children[ctx_sym];
        if (child_idx == -1) {
            p_parent = base_prob(sym);
        } else {
            p_parent = py_predict(child_idx, sym, ctx, depth + 1, max_d);
        }
    }

    double new_table_prob;
    if (C + concentration_ <= 0.0) {
        new_table_prob = 1.0;
    } else {
        new_table_prob = (concentration_ + discount_ * T) * p_parent
                       / (C + concentration_);
    }

    // Expected seating: always add 1 customer, add fractional table
    r.customer_counts[sym] += 1.0;
    r.total_customers += 1.0;
    r.table_counts[sym] += new_table_prob;
    r.total_tables += new_table_prob;

    // Propagate to deeper context node if it exists
    if (depth < max_d) {
        Symbol ctx_sym = ctx[max_d - depth - 1];
        int32_t child_idx = get_or_create(node_idx, ctx_sym);
        seat_customer(child_idx, sym, ctx, depth + 1, max_d);
    }
}

void SeqMemoPredictor::train_symbol(Symbol sym) {
    int ctx_len = std::min(static_cast<int>(context_.size()), max_order_);

    // Prepare context array
    std::vector<Symbol> ctx(ctx_len);
    context_.get_last(ctx.data(), ctx_len);

    // Seat customer starting from root, propagating deeper
    seat_customer(0, sym, ctx.data(), 0, ctx_len);

    context_.push(sym);
}

void SeqMemoPredictor::learn(const Symbol* data, size_t len) {
    context_.clear();
    nodes_.clear();
    nodes_.reserve(1024);
    alloc_node();

    for (size_t i = 0; i < len; ++i)
        train_symbol(data[i]);
}

double SeqMemoPredictor::predict(Symbol symbol, const Symbol* context,
                                  size_t ctx_len) {
    int depth = std::min(static_cast<int>(ctx_len), max_order_);
    if (depth == 0) {
        return py_predict(0, symbol, nullptr, 0, 0);
    }
    return py_predict(0, symbol, context + ctx_len - depth, 0, depth);
}

void SeqMemoPredictor::predict_distribution(const Symbol* context, size_t ctx_len,
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

double SeqMemoPredictor::log_eval(const Symbol* data, size_t len) {
    static constexpr double NEG_INV_LOG2 = -1.0 / 0.6931471805599453;

    context_.clear();
    nodes_.clear();
    nodes_.reserve(1024);
    alloc_node();

    double value = 0.0;
    for (size_t i = 0; i < len; ++i) {
        // Predict before seating
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

double SeqMemoPredictor::log_eval(const Symbol* data, size_t len,
                                   const Symbol* /*ctx*/, size_t /*ctx_len*/) {
    return log_eval(data, len);
}

} // namespace vmm
