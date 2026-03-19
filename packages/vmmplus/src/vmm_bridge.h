#ifndef VMM_BRIDGE_H
#define VMM_BRIDGE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* vmm_handle;

/* Lifecycle */
vmm_handle vmm_create(const char* algorithm, uint16_t alphabet_size,
                       int max_order, const double* params, int n_params);
void       vmm_destroy(vmm_handle h);

/* Training */
void vmm_learn(vmm_handle h, const uint16_t* data, size_t len);

/* Prediction */
double vmm_predict(vmm_handle h, uint16_t symbol,
                   const uint16_t* context, size_t ctx_len);
void   vmm_predict_distribution(vmm_handle h,
                                const uint16_t* context, size_t ctx_len,
                                double* out_probs);

/* Evaluation */
double vmm_log_eval(vmm_handle h, const uint16_t* data, size_t len);
double vmm_log_eval_ctx(vmm_handle h, const uint16_t* data, size_t len,
                        const uint16_t* ctx, size_t ctx_len);

/* Query */
uint16_t vmm_alphabet_size(vmm_handle h);

#ifdef __cplusplus
}
#endif

#endif /* VMM_BRIDGE_H */
