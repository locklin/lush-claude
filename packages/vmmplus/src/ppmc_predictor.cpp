#include "ppmc_predictor.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace vmm {

static constexpr int MIN_CONTEXT_LENGTH = 1;
static constexpr int MAX_INDIVIDUAL_COUNT = 8 * 1024;
static constexpr int MIN_COUNT = 128;

PPMCPredictor::PPMCPredictor(uint16_t alphabet_size, int max_order)
    : ab_size_(alphabet_size)
    , max_order_(max_order)
    , contexts_(alphabet_size, -1)
    , unigram_counts_(alphabet_size, 1)  // initialized to 1 (uniform prior)
    , buffer_(max_order + 1)
    , context_length_(0)
    , context_node_(-1)
    , excluded_(alphabet_size, false)
{
    nodes_.reserve(1024);
}

int32_t PPMCPredictor::alloc_node(Symbol byte_val, int32_t next_sibling) {
    int32_t idx = static_cast<int32_t>(nodes_.size());
    nodes_.push_back({byte_val, 1, 0, -1, next_sibling});
    return idx;
}

// --- Training ---

void PPMCPredictor::learn(const Symbol* data, size_t len) {
    clear_context();
    for (size_t i = 0; i < len; ++i)
        use_symbol(data[i]);
}

void PPMCPredictor::use_symbol(Symbol sym) {
    // Walk down from longest context, escaping until symbol is found
    // This mirrors Java's use() which calls escaped() + interval()
    int result[3];
    while (escaped(sym)) {
        interval_escape(result);
    }
    // Found the symbol (or at backoff model) — update trie
    interval_byte(sym, result, true);
}

void PPMCPredictor::clear_context() {
    buffer_.clear();
    context_length_ = 0;
    context_node_ = -1;
    std::fill(excluded_.begin(), excluded_.end(), false);
}

// --- Prediction ---

double PPMCPredictor::predict(Symbol symbol, const Symbol* context, size_t ctx_len) {
    clear_context();
    // Replay context through predict (non-incrementing) to set up context state
    for (size_t i = 0; i < ctx_len; ++i) {
        // For prediction: replay without incrementing
        // Same as Java's OfflinePPMModel.predict() for context setup
        int result[3];
        while (escaped(context[i])) {
            interval_escape(result);
        }
        // Non-incrementing interval
        interval_byte(context[i], result, false);
    }
    // Now predict the target symbol
    return predict_symbol(symbol);
}

double PPMCPredictor::predict_symbol(Symbol sym) {
    double p = 1.0;
    int result[3];
    while (escaped(sym)) {
        interval_escape(result);
        if (result[2] > 0)
            p *= static_cast<double>(result[1] - result[0]) / result[2];
    }
    // Symbol found at current context level (or backoff)
    interval_byte(sym, result, false);
    if (result[2] > 0)
        p *= static_cast<double>(result[1] - result[0]) / result[2];
    return p;
}

void PPMCPredictor::predict_distribution(const Symbol* context, size_t ctx_len,
                                          double* out_probs) {
    for (uint16_t s = 0; s < ab_size_; ++s)
        out_probs[s] = predict(static_cast<Symbol>(s), context, ctx_len);
}

double PPMCPredictor::log_eval(const Symbol* data, size_t len) {
    static constexpr double NEG_INV_LOG2 = -1.0 / 0.6931471805599453; // -1/ln(2)
    clear_context();
    double value = 0.0;
    for (size_t i = 0; i < len; ++i) {
        double p = predict_symbol(data[i]);
        value += std::log(p);
        // After prediction, we need to update the context for the next symbol
        // But we need to replay via use_symbol-like logic
        // Actually, for logEval: predict then advance context without incrementing
        // The Java code calls ppmc.predict() which does both predict and advance context
    }
    return value * NEG_INV_LOG2;
}

double PPMCPredictor::log_eval(const Symbol* data, size_t len,
                                const Symbol* /*ctx*/, size_t /*ctx_len*/) {
    // Not implemented in Java either
    return log_eval(data, len);
}

// --- Escape and context management ---

bool PPMCPredictor::escaped(Symbol sym) const {
    return (context_node_ != -1 && !node_has_daughter(context_node_, sym));
}

void PPMCPredictor::interval_escape(int* result) {
    node_interval_escape(context_node_, result);
    // PPM-C update exclusion: add all children to excluded set
    if (context_length_ >= MIN_CONTEXT_LENGTH) {
        int32_t child = nodes_[context_node_].first_child;
        while (child != -1) {
            excluded_[nodes_[child].byte_val] = true;
            child = nodes_[child].next_sibling;
        }
    }
    --context_length_;
    get_context_node_long_to_short();
}

void PPMCPredictor::interval_byte(Symbol sym, int* result, bool do_increment) {
    if (context_node_ != -1) {
        node_interval(context_node_, sym, result);
        if (do_increment)
            unigram_counts_[sym]++;
    } else {
        if (do_increment)
            unigram_interval(sym, result, true);
        else
            unigram_interval_no_increment(sym, result);
    }
    if (do_increment)
        increment(sym);
    else {
        // Non-incrementing: just advance buffer and context
        buffer_.push(sym);
        context_length_ = std::min(max_order_, static_cast<int>(buffer_.size()));
        get_context_node_binary_search();
    }
    std::fill(excluded_.begin(), excluded_.end(), false);
}

// --- Trie operations ---

void PPMCPredictor::increment(Symbol sym) {
    buffer_.push(sym);

    // Get the first symbol of the current buffer content
    size_t buf_len = buffer_.size();
    std::vector<Symbol> bytes(buf_len);
    for (size_t i = 0; i < buf_len; ++i)
        bytes[i] = buffer_[i];

    size_t offset = (buf_len > static_cast<size_t>(max_order_ + 1))
                    ? buf_len - max_order_ - 1 : 0;
    Symbol first_byte = bytes[offset];

    if (contexts_[first_byte] == -1)
        contexts_[first_byte] = alloc_node(first_byte, -1);

    size_t remaining = buf_len - offset;
    if (remaining > 1)
        node_increment(contexts_[first_byte], bytes.data(), offset + 1, remaining - 1);

    context_length_ = std::min(max_order_, static_cast<int>(buf_len));
    get_context_node_binary_search();
    std::fill(excluded_.begin(), excluded_.end(), false);
}

void PPMCPredictor::node_increment(int32_t node_idx, const Symbol* bytes,
                                    int offset, int length) {
    Node& node = nodes_[node_idx];
    if (node.first_child == -1) {
        ++node.num_outcomes;
        node.first_child = alloc_node(bytes[offset], -1);
        if (length > 1)
            node_complete(node.first_child, bytes, offset + 1, length - 1);
        return;
    }

    int32_t prev = -1;
    int32_t child = node.first_child;
    while (true) {
        Node& ch = nodes_[child];
        if (ch.byte_val == bytes[offset]) {
            if (length > 1)
                node_increment(child, bytes, offset + 1, length - 1);
            // Move-to-front
            if (prev != -1) {
                nodes_[prev].next_sibling = ch.next_sibling;
                ch.next_sibling = node.first_child;
                node.first_child = child;
            }
            if (++ch.count > MAX_INDIVIDUAL_COUNT)
                rescale(node_idx);
            return;
        }
        if (ch.next_sibling == -1) {
            ++node.num_outcomes;
            int32_t new_child = alloc_node(bytes[offset], node.first_child);
            node.first_child = new_child;
            if (length > 1)
                node_complete(new_child, bytes, offset + 1, length - 1);
            return;
        }
        prev = child;
        child = ch.next_sibling;
    }
}

void PPMCPredictor::node_complete(int32_t node_idx, const Symbol* bytes,
                                   int offset, int length) {
    // Create a chain of new nodes for remaining bytes
    int32_t cur = node_idx;
    for (int i = 0; i < length; ++i) {
        int32_t child = alloc_node(bytes[offset + i], -1);
        nodes_[cur].first_child = child;
        nodes_[cur].num_outcomes = 1;
        cur = child;
    }
}

void PPMCPredictor::rescale(int32_t node_idx) {
    Node& node = nodes_[node_idx];
    node.num_outcomes = static_cast<uint16_t>((node.num_outcomes + 1) / 2);
    // Rescale children, removing those with count < MIN_COUNT
    int32_t child = node.first_child;
    int32_t new_first = -1;
    int32_t new_last = -1;
    while (child != -1) {
        int32_t next = nodes_[child].next_sibling;
        nodes_[child].count >>= 1;
        if (nodes_[child].count >= MIN_COUNT) {
            nodes_[child].next_sibling = -1;
            if (new_first == -1) {
                new_first = child;
                new_last = child;
            } else {
                nodes_[new_last].next_sibling = child;
                new_last = child;
            }
        }
        child = next;
    }
    node.first_child = new_first;
}

// --- Node interval computations ---

int PPMCPredictor::node_total_count(int32_t node_idx) const {
    const Node& node = nodes_[node_idx];
    int count = node.num_outcomes; // escape count in PPM-C
    int32_t child = node.first_child;
    while (child != -1) {
        if (!excluded_[nodes_[child].byte_val])
            count += nodes_[child].count;
        child = nodes_[child].next_sibling;
    }
    return count;
}

void PPMCPredictor::node_interval(int32_t node_idx, Symbol sym, int* result) const {
    result[0] = 0;
    int32_t child = nodes_[node_idx].first_child;
    while (child != -1) {
        const Node& ch = nodes_[child];
        if (excluded_[ch.byte_val]) {
            child = ch.next_sibling;
            continue;
        }
        if (ch.byte_val == sym) {
            result[1] = result[0] + ch.count;
            result[2] = result[1] + nodes_[node_idx].num_outcomes;
            // Add remaining children counts
            child = ch.next_sibling;
            while (child != -1) {
                if (!excluded_[nodes_[child].byte_val])
                    result[2] += nodes_[child].count;
                child = nodes_[child].next_sibling;
            }
            return;
        }
        result[0] += ch.count;
        child = ch.next_sibling;
    }
    // Symbol not found — shouldn't happen if escaped() was checked first
    // Fall through to backoff
    result[0] = result[1] = result[2] = 0;
}

void PPMCPredictor::node_interval_escape(int32_t node_idx, int* result) const {
    const Node& node = nodes_[node_idx];
    if (node.num_outcomes == 0) {
        result[0] = 0;
        result[1] = 1;
        result[2] = 1;
        return;
    }
    int total = node_total_count(node_idx);
    result[2] = total;
    result[1] = total;
    result[0] = total - node.num_outcomes;
}

bool PPMCPredictor::node_has_daughter(int32_t node_idx, Symbol sym) const {
    int32_t child = nodes_[node_idx].first_child;
    while (child != -1) {
        if (nodes_[child].byte_val == sym) return true;
        child = nodes_[child].next_sibling;
    }
    return false;
}

// --- Context node lookup ---

int32_t PPMCPredictor::lookup_node(int ctx_len) const {
    size_t buf_len = buffer_.size();
    if (ctx_len < 1 || static_cast<size_t>(ctx_len) > buf_len) return -1;

    // The context starts at buf_len - ctx_len
    Symbol first = buffer_[buf_len - ctx_len];
    int32_t node = contexts_[first];
    if (node == -1) return -1;

    // Walk remaining symbols
    for (int i = 1; i < ctx_len; ++i) {
        Symbol sym = buffer_[buf_len - ctx_len + i];
        node = lookup(node, &sym, 1);
        if (node == -1) return -1;
    }
    return node;
}

int32_t PPMCPredictor::lookup(int32_t node, const Symbol* syms, int len) const {
    for (int i = 0; i < len; ++i) {
        int32_t child = nodes_[node].first_child;
        bool found = false;
        while (child != -1) {
            if (nodes_[child].byte_val == syms[i]) {
                node = child;
                found = true;
                break;
            }
            child = nodes_[child].next_sibling;
        }
        if (!found) return -1;
    }
    return node;
}

void PPMCPredictor::get_context_node_long_to_short() {
    while (context_length_ >= MIN_CONTEXT_LENGTH) {
        context_node_ = lookup_node(context_length_);
        if (context_node_ != -1) return;
        --context_length_;
    }
    context_node_ = -1;
}

void PPMCPredictor::get_context_node_binary_search() {
    // Try the full context length first, then shorten if not found
    // The Java uses binary search but simple linear is fine for correctness
    while (context_length_ >= MIN_CONTEXT_LENGTH) {
        context_node_ = lookup_node(context_length_);
        if (context_node_ != -1) return;
        --context_length_;
    }
    context_node_ = -1;
}

// --- Unigram backoff model ---

void PPMCPredictor::unigram_interval(Symbol sym, int* result, bool do_increment) {
    int sum = 0;
    for (Symbol i = 0; i < sym; ++i)
        if (!excluded_[i]) sum += unigram_counts_[i];
    result[0] = sum;
    sum += unigram_counts_[sym];
    result[1] = sum;
    for (Symbol i = sym + 1; i < ab_size_; ++i)
        if (!excluded_[i]) sum += unigram_counts_[i];
    result[2] = sum;
    if (do_increment) unigram_counts_[sym]++;
}

void PPMCPredictor::unigram_interval_no_increment(Symbol sym, int* result) const {
    int sum = 0;
    for (Symbol i = 0; i < sym; ++i)
        if (!excluded_[i]) sum += unigram_counts_[i];
    result[0] = sum;
    sum += unigram_counts_[sym];
    result[1] = sum;
    for (Symbol i = sym + 1; i < ab_size_; ++i)
        if (!excluded_[i]) sum += unigram_counts_[i];
    result[2] = sum;
}

} // namespace vmm
