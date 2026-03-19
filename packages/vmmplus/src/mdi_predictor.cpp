#include "mdi_predictor.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace vmm {

MDIPredictor::MDIPredictor(uint16_t alphabet_size, int max_order,
                           double merge_threshold)
    : ab_size_(alphabet_size)
    , max_order_(max_order)
    , merge_threshold_(merge_threshold)
    , merged_(false)
    , context_(max_order)
{
    nodes_.reserve(1024);
    alloc_node(); // root node (index 0)
}

int32_t MDIPredictor::alloc_node() {
    int32_t idx = static_cast<int32_t>(nodes_.size());
    nodes_.emplace_back(ab_size_);
    return idx;
}

int32_t MDIPredictor::get_or_create(int32_t parent, Symbol sym) {
    int32_t& child = nodes_[parent].children[sym];
    if (child == -1)
        child = alloc_node();
    return child;
}

void MDIPredictor::build_symbol(Symbol sym) {
    int ctx_len = std::min(static_cast<int>(context_.size()), max_order_);

    // Update root
    nodes_[0].counts[sym] += 1.0;

    // Walk down context tree, updating counts at each depth
    int32_t cur = 0;
    for (int d = 0; d < ctx_len; ++d) {
        Symbol ctx_sym = context_[context_.size() - d - 1];
        int32_t child = get_or_create(cur, ctx_sym);
        nodes_[child].counts[sym] += 1.0;
        cur = child;
    }

    context_.push(sym);
}

int32_t MDIPredictor::resolve(int32_t idx) const {
    while (idx >= 0 && nodes_[idx].merged_into != -1) {
        idx = nodes_[idx].merged_into;
    }
    return idx;
}

double MDIPredictor::symmetric_kl(int32_t a_idx, int32_t b_idx) const {
    // Compute 0.5 * (KL(p||q) + KL(q||p)) where p and q are the
    // conditional distributions at nodes a and b.
    // Use smoothed (add-epsilon) distributions to avoid log(0).
    const double eps = 1e-10;

    double total_a = 0.0, total_b = 0.0;
    for (uint16_t s = 0; s < ab_size_; ++s) {
        total_a += nodes_[a_idx].counts[s];
        total_b += nodes_[b_idx].counts[s];
    }

    if (total_a == 0.0 || total_b == 0.0) return 0.0;

    double kl_ab = 0.0, kl_ba = 0.0;
    for (uint16_t s = 0; s < ab_size_; ++s) {
        double pa = (nodes_[a_idx].counts[s] + eps) / (total_a + ab_size_ * eps);
        double pb = (nodes_[b_idx].counts[s] + eps) / (total_b + ab_size_ * eps);
        kl_ab += pa * std::log(pa / pb);
        kl_ba += pb * std::log(pb / pa);
    }

    return 0.5 * (kl_ab + kl_ba);
}

void MDIPredictor::merge_states() {
    if (merge_threshold_ <= 0.0) {
        merged_ = true;
        return;
    }

    // Collect nodes by depth. We do BFS from root.
    // depth_nodes[d] = list of node indices at depth d
    std::vector<std::vector<int32_t>> depth_nodes(max_order_ + 1);
    depth_nodes[0].push_back(0);

    for (int d = 0; d < max_order_; ++d) {
        for (int32_t parent : depth_nodes[d]) {
            if (nodes_[parent].merged_into != -1) continue;
            for (uint16_t s = 0; s < ab_size_; ++s) {
                int32_t child = nodes_[parent].children[s];
                if (child != -1 && nodes_[child].merged_into == -1) {
                    depth_nodes[d + 1].push_back(child);
                }
            }
        }
    }

    // Merge bottom-up from deepest to shallowest
    for (int d = max_order_; d >= 1; --d) {
        const auto& level = depth_nodes[d];
        int n = static_cast<int>(level.size());
        for (int i = 0; i < n; ++i) {
            int32_t ai = resolve(level[i]);
            if (ai < 0 || nodes_[ai].merged_into != -1) continue;
            for (int j = i + 1; j < n; ++j) {
                int32_t bj = resolve(level[j]);
                if (bj < 0 || bj == ai || nodes_[bj].merged_into != -1) continue;
                double skl = symmetric_kl(ai, bj);
                if (skl < merge_threshold_) {
                    // Merge bj into ai: sum counts, redirect
                    for (uint16_t s = 0; s < ab_size_; ++s) {
                        nodes_[ai].counts[s] += nodes_[bj].counts[s];
                        // Merge children pointers
                        if (nodes_[ai].children[s] == -1 && nodes_[bj].children[s] != -1) {
                            nodes_[ai].children[s] = nodes_[bj].children[s];
                        }
                    }
                    nodes_[bj].merged_into = ai;
                    // Redirect parent pointers that pointed to bj
                    if (d > 0) {
                        for (int32_t parent : depth_nodes[d - 1]) {
                            if (nodes_[parent].merged_into != -1) continue;
                            for (uint16_t s = 0; s < ab_size_; ++s) {
                                if (nodes_[parent].children[s] == bj) {
                                    nodes_[parent].children[s] = ai;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    merged_ = true;
}

void MDIPredictor::learn(const Symbol* data, size_t len) {
    context_.clear();
    nodes_.clear();
    nodes_.reserve(1024);
    alloc_node();
    merged_ = false;

    for (size_t i = 0; i < len; ++i)
        build_symbol(data[i]);

    // Run merge phase
    merge_states();
}

double MDIPredictor::predict_from_trie(Symbol sym, const Symbol* ctx,
                                        int ctx_len) const {
    // PPM-C style escape on the merged trie.
    // Walk from deepest context to shallowest.
    double p_accum = 1.0;

    for (int depth = ctx_len; depth >= 0; --depth) {
        // Find node at this depth
        int32_t node_idx = 0;
        bool found = true;
        for (int d = 0; d < depth; ++d) {
            Symbol cs = ctx[ctx_len - depth + d];
            int32_t child = nodes_[node_idx].children[cs];
            child = (child != -1) ? resolve(child) : -1;
            if (child == -1) {
                found = false;
                break;
            }
            node_idx = child;
        }
        node_idx = resolve(node_idx);
        if (!found) continue;

        const MDINode& node = nodes_[node_idx];
        double total = 0.0;
        uint16_t distinct = 0;
        for (uint16_t s = 0; s < ab_size_; ++s) {
            total += node.counts[s];
            if (node.counts[s] > 0.0) ++distinct;
        }

        if (total == 0.0 && distinct == 0) continue;

        double d = static_cast<double>(distinct);
        if (node.counts[sym] > 0.0) {
            // Symbol found
            double denom = total + d;
            if (denom > 0.0) {
                p_accum *= node.counts[sym] / denom;
            }
            return p_accum;
        } else {
            // Escape
            double denom = total + d;
            if (denom > 0.0) {
                p_accum *= d / denom;
            }
        }
    }

    // Uniform backoff
    p_accum *= 1.0 / ab_size_;
    return p_accum;
}

double MDIPredictor::predict(Symbol symbol, const Symbol* context,
                              size_t ctx_len) {
    int depth = std::min(static_cast<int>(ctx_len), max_order_);
    return predict_from_trie(symbol, context + ctx_len - depth, depth);
}

void MDIPredictor::predict_distribution(const Symbol* context, size_t ctx_len,
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

double MDIPredictor::log_eval(const Symbol* data, size_t len) {
    static constexpr double NEG_INV_LOG2 = -1.0 / 0.6931471805599453;

    // Use the already-trained (and merged) model
    ContextBuffer<Symbol> eval_ctx(max_order_);
    double value = 0.0;
    for (size_t i = 0; i < len; ++i) {
        int ctx_len = std::min(static_cast<int>(eval_ctx.size()), max_order_);
        std::vector<Symbol> ctx(ctx_len);
        eval_ctx.get_last(ctx.data(), ctx_len);
        double p = predict(data[i], ctx.data(), ctx_len);
        if (p < 1e-300) p = 1e-300;
        value += std::log(p);
        eval_ctx.push(data[i]);
    }
    return value * NEG_INV_LOG2;
}

double MDIPredictor::log_eval(const Symbol* data, size_t len,
                               const Symbol* /*ctx*/, size_t /*ctx_len*/) {
    return log_eval(data, len);
}

} // namespace vmm
