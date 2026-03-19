#include "vmm_bridge.h"
#include "vmm_predictor.h"
#include <cstdio>

extern "C" {

vmm_handle vmm_create(const char* algorithm, uint16_t alphabet_size,
                       int max_order, const double* params, int n_params) {
    if (!algorithm) return nullptr;
    try {
        auto* pred = vmm::create_predictor(algorithm, alphabet_size,
                                           max_order, params, n_params);
        return static_cast<vmm_handle>(pred);
    } catch (...) {
        fprintf(stderr, "vmm_create: failed to create predictor '%s'\n", algorithm);
        return nullptr;
    }
}

void vmm_destroy(vmm_handle h) {
    if (h) delete static_cast<vmm::VMMPredictor*>(h);
}

void vmm_learn(vmm_handle h, const uint16_t* data, size_t len) {
    if (!h || !data) return;
    static_cast<vmm::VMMPredictor*>(h)->learn(data, len);
}

double vmm_predict(vmm_handle h, uint16_t symbol,
                   const uint16_t* context, size_t ctx_len) {
    if (!h) return 0.0;
    return static_cast<vmm::VMMPredictor*>(h)->predict(symbol, context, ctx_len);
}

void vmm_predict_distribution(vmm_handle h,
                              const uint16_t* context, size_t ctx_len,
                              double* out_probs) {
    if (!h || !out_probs) return;
    static_cast<vmm::VMMPredictor*>(h)->predict_distribution(context, ctx_len, out_probs);
}

double vmm_log_eval(vmm_handle h, const uint16_t* data, size_t len) {
    if (!h || !data) return 0.0;
    return static_cast<vmm::VMMPredictor*>(h)->log_eval(data, len);
}

double vmm_log_eval_ctx(vmm_handle h, const uint16_t* data, size_t len,
                        const uint16_t* ctx, size_t ctx_len) {
    if (!h || !data) return 0.0;
    return static_cast<vmm::VMMPredictor*>(h)->log_eval(data, len, ctx, ctx_len);
}

uint16_t vmm_alphabet_size(vmm_handle h) {
    if (!h) return 0;
    return static_cast<vmm::VMMPredictor*>(h)->alphabet_size();
}

} // extern "C"
