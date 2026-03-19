#include "pst_predictor.h"
#include <cmath>
#include <algorithm>
#include <queue>

namespace vmm {

PSTPredictor::PSTPredictor(uint16_t alphabet_size, int max_order,
                           double pmin, double alpha, double gamma_param, double r)
    : ab_size_(alphabet_size)
    , max_order_(max_order)
    , pmin_(pmin)
    , alpha_(alpha)
    , gamma_(gamma_param)
    , r_(r)
    , trained_(false)
{
    nodes_.reserve(256);
    alloc_node(); // root = index 0
}

int32_t PSTPredictor::alloc_node() {
    int32_t idx = static_cast<int32_t>(nodes_.size());
    nodes_.emplace_back(ab_size_);
    return idx;
}

int PSTPredictor::count_context(const Symbol* ctx, int ctx_len) const {
    int count = 0;
    int n = static_cast<int>(train_data_.size());
    for (int i = ctx_len; i <= n; ++i) {
        bool match = true;
        for (int j = 0; j < ctx_len; ++j) {
            if (train_data_[i - ctx_len + j] != ctx[j]) {
                match = false;
                break;
            }
        }
        if (match) count++;
    }
    return count;
}

int PSTPredictor::count_symbol_after_context(Symbol sym, const Symbol* ctx, int ctx_len) const {
    int count = 0;
    int n = static_cast<int>(train_data_.size());
    for (int i = ctx_len; i < n; ++i) {
        if (train_data_[i] != sym) continue;
        bool match = true;
        for (int j = 0; j < ctx_len; ++j) {
            if (train_data_[i - ctx_len + j] != ctx[j]) {
                match = false;
                break;
            }
        }
        if (match) count++;
    }
    return count;
}

void PSTPredictor::build_pst() {
    int n = static_cast<int>(train_data_.size());
    if (n == 0) return;

    // Reset tree
    nodes_.clear();
    alloc_node(); // root

    // Compute root probabilities (unigram)
    PSTNode& root = nodes_[0];
    root.count = n;
    for (uint16_t s = 0; s < ab_size_; ++s) {
        int c = 0;
        for (int i = 0; i < n; ++i)
            if (train_data_[i] == s) c++;
        root.probs[s] = (c + gamma_) / (n + ab_size_ * gamma_);
    }

    // BFS to build the PST layer by layer
    // Each candidate context is represented as a sequence of symbols
    // Context is stored in reverse order: ctx[0] = most recent, ctx[len-1] = oldest
    struct Candidate {
        std::vector<Symbol> ctx; // context in chronological order (oldest..newest)
        int32_t node_idx;
        int32_t parent_idx;
        Symbol parent_sym; // symbol that leads from parent to this node
    };

    std::queue<Candidate> work;

    // Add alphabet symbols as initial candidates (depth 1)
    for (uint16_t s = 0; s < ab_size_; ++s) {
        std::vector<Symbol> ctx = {static_cast<Symbol>(s)};
        int ctx_count = count_context(ctx.data(), 1);

        // Condition A: context probability >= pmin
        double ctx_prob = static_cast<double>(ctx_count) / n;
        if (ctx_prob < pmin_) continue;

        // Build probability distribution for this context
        // Use sum of symbol-after-context counts as denominator (not ctx_count)
        // to handle contexts at end of sequence that have no following symbol
        bool significant = false;
        std::vector<double> probs(ab_size_);
        int total_following = 0;
        for (uint16_t a = 0; a < ab_size_; ++a)
            total_following += count_symbol_after_context(a, ctx.data(), 1);
        for (uint16_t a = 0; a < ab_size_; ++a) {
            int sym_count = count_symbol_after_context(a, ctx.data(), 1);
            probs[a] = (sym_count + gamma_) / (total_following + ab_size_ * gamma_);

            // Condition B: ratio test — P(a|s) / P(a) significantly different
            double root_p = root.probs[a];
            if (root_p > 0) {
                double ratio = probs[a] / root_p;
                if (ratio > r_ || ratio < 1.0 / r_) significant = true;
            }
        }

        if (!significant && alpha_ > 0) continue;

        // Add to tree
        int32_t node = alloc_node();
        nodes_[0].children[s] = node;
        nodes_[node].count = ctx_count;
        nodes_[node].probs = probs;

        if (1 < max_order_) {
            Candidate cand;
            cand.ctx = ctx;
            cand.node_idx = node;
            cand.parent_idx = 0;
            cand.parent_sym = s;
            work.push(cand);
        }
    }

    // Extend to deeper contexts
    while (!work.empty()) {
        Candidate cand = work.front();
        work.pop();
        int depth = static_cast<int>(cand.ctx.size());
        if (depth >= max_order_) continue;

        for (uint16_t s = 0; s < ab_size_; ++s) {
            // Prepend s to the context (older symbol)
            std::vector<Symbol> new_ctx;
            new_ctx.push_back(static_cast<Symbol>(s));
            new_ctx.insert(new_ctx.end(), cand.ctx.begin(), cand.ctx.end());
            int new_len = static_cast<int>(new_ctx.size());

            int ctx_count = count_context(new_ctx.data(), new_len);
            double ctx_prob = static_cast<double>(ctx_count) / n;
            if (ctx_prob < pmin_) continue;

            // Check ratio test against parent's distribution
            bool significant = false;
            std::vector<double> probs(ab_size_);
            int total_following = 0;
            for (uint16_t a = 0; a < ab_size_; ++a)
                total_following += count_symbol_after_context(a, new_ctx.data(), new_len);
            for (uint16_t a = 0; a < ab_size_; ++a) {
                int sym_count = count_symbol_after_context(a, new_ctx.data(), new_len);
                probs[a] = (sym_count + gamma_) / (total_following + ab_size_ * gamma_);

                double parent_p = nodes_[cand.node_idx].probs[a];
                if (parent_p > 0) {
                    double ratio = probs[a] / parent_p;
                    if (ratio > r_ || ratio < 1.0 / r_) significant = true;
                }
            }

            if (!significant) continue;

            int32_t node = alloc_node();
            nodes_[cand.node_idx].children[s] = node;
            nodes_[node].count = ctx_count;
            nodes_[node].probs = probs;

            if (new_len < max_order_) {
                Candidate new_cand;
                new_cand.ctx = new_ctx;
                new_cand.node_idx = node;
                new_cand.parent_idx = cand.node_idx;
                new_cand.parent_sym = s;
                work.push(new_cand);
            }
        }
    }

    trained_ = true;
}

int32_t PSTPredictor::find_deepest_match(const Symbol* context, size_t ctx_len) const {
    // Walk the PST from root, using context from most recent to oldest
    int32_t node = 0;
    for (size_t i = 0; i < ctx_len; ++i) {
        Symbol sym = context[ctx_len - 1 - i]; // most recent first
        int32_t child = nodes_[node].children[sym];
        if (child == -1) break;
        node = child;
    }
    return node;
}

void PSTPredictor::learn(const Symbol* data, size_t len) {
    train_data_.assign(data, data + len);
    build_pst();
}

double PSTPredictor::predict(Symbol symbol, const Symbol* context, size_t ctx_len) {
    if (!trained_ || nodes_.empty()) return 1.0 / ab_size_;
    int32_t node = find_deepest_match(context, ctx_len);
    return nodes_[node].probs[symbol];
}

void PSTPredictor::predict_distribution(const Symbol* context, size_t ctx_len,
                                         double* out_probs) {
    if (!trained_ || nodes_.empty()) {
        for (uint16_t s = 0; s < ab_size_; ++s)
            out_probs[s] = 1.0 / ab_size_;
        return;
    }
    int32_t node = find_deepest_match(context, ctx_len);
    for (uint16_t s = 0; s < ab_size_; ++s)
        out_probs[s] = nodes_[node].probs[s];
}

double PSTPredictor::log_eval(const Symbol* data, size_t len) {
    static constexpr double NEG_INV_LOG2 = -1.0 / 0.6931471805599453;
    ContextBuffer<Symbol> ctx(max_order_);
    double value = 0.0;
    for (size_t i = 0; i < len; ++i) {
        std::vector<Symbol> context(ctx.size());
        ctx.get_last(context.data(), ctx.size());
        double p = predict(data[i], context.data(), context.size());
        value += std::log(p);
        ctx.push(data[i]);
    }
    return value * NEG_INV_LOG2;
}

double PSTPredictor::log_eval(const Symbol* data, size_t len,
                               const Symbol* /*ctx*/, size_t /*ctx_len*/) {
    return log_eval(data, len);
}

} // namespace vmm
