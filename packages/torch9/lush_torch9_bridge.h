/* lush_torch9_bridge.h -- extern "C" bridge between Lush and libtorch
 *
 * Opaque-handle pattern: all torch objects are void* on the C side.
 * Lush treats them as gptr values.  The bridge manages heap-allocated
 * torch::Tensor objects behind these handles.
 *
 * dtype int maps Lush ST_* enum to torch ScalarType:
 *   ST_F(2)  -> kFloat32    ST_D(3)  -> kFloat64
 *   ST_I32(4)-> kInt32      ST_I16(5)-> kInt16
 *   ST_I8(6) -> kInt8       ST_U8(7) -> kUInt8
 *   ST_I64(9)-> kInt64
 */

#ifndef LUSH_TORCH9_BRIDGE_H
#define LUSH_TORCH9_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* lt9_tensor;

/* ---- Initialization ---- */

/* Initialize libtorch.  Returns 1 if CUDA is available, 0 otherwise. */
int         lt9_init(void);

/* Clean up libtorch state. */
void        lt9_cleanup(void);

/* ---- Tensor creation from raw data (zero-copy via from_blob) ---- */

/* Wrap existing memory as a tensor.  The caller must keep the data alive
 * for the lifetime of this tensor (or clone it).
 *   data    - pointer to first element (already offset-adjusted)
 *   sizes   - dimension sizes, length ndim
 *   strides - element strides per dimension, length ndim
 *   ndim    - number of dimensions (1..11)
 *   dtype   - Lush ST_* enum value (2=float, 3=double, etc.)
 */
lt9_tensor  lt9_from_blob(void *data, int64_t *sizes, int64_t *strides,
                          int ndim, int dtype);

/* ---- Tensor introspection ---- */

void*       lt9_data_ptr(lt9_tensor t);
int         lt9_ndim(lt9_tensor t);
void        lt9_sizes(lt9_tensor t, int64_t *out);
void        lt9_strides(lt9_tensor t, int64_t *out);
int         lt9_dtype(lt9_tensor t);       /* returns Lush ST_* value */
int64_t     lt9_numel(lt9_tensor t);

/* ---- Core operations ---- */

lt9_tensor  lt9_matmul(lt9_tensor a, lt9_tensor b);
lt9_tensor  lt9_add(lt9_tensor a, lt9_tensor b);
lt9_tensor  lt9_sub(lt9_tensor a, lt9_tensor b);
lt9_tensor  lt9_mul(lt9_tensor a, lt9_tensor b);
lt9_tensor  lt9_div(lt9_tensor a, lt9_tensor b);

/* ---- Activations ---- */

lt9_tensor  lt9_relu(lt9_tensor tens);
lt9_tensor  lt9_sigmoid(lt9_tensor tens);
lt9_tensor  lt9_tanh(lt9_tensor tens);
lt9_tensor  lt9_softmax(lt9_tensor tens, int dim);
lt9_tensor  lt9_log_softmax(lt9_tensor tens, int dim);

/* ---- Shape manipulation ---- */

lt9_tensor  lt9_reshape(lt9_tensor tens, int64_t *shape, int ndim);
lt9_tensor  lt9_transpose(lt9_tensor tens, int dim0, int dim1);
lt9_tensor  lt9_permute(lt9_tensor tens, int64_t *dims, int ndim);
lt9_tensor  lt9_squeeze(lt9_tensor tens, int dim);
lt9_tensor  lt9_unsqueeze(lt9_tensor tens, int dim);
lt9_tensor  lt9_cat2(lt9_tensor a, lt9_tensor b, int dim);
lt9_tensor  lt9_cat3(lt9_tensor a, lt9_tensor b, lt9_tensor c, int dim);
lt9_tensor  lt9_cat4(lt9_tensor a, lt9_tensor b, lt9_tensor c,
                      lt9_tensor d, int dim);

/* ---- GPU transfer ---- */

lt9_tensor  lt9_to_cuda(lt9_tensor tens, int device);
lt9_tensor  lt9_to_cpu(lt9_tensor tens);
int         lt9_device_type(lt9_tensor tens);  /* 0=CPU, 1=CUDA */

/* ---- Lifecycle ---- */

lt9_tensor  lt9_clone(lt9_tensor t);
void        lt9_free(lt9_tensor t);

/* ---- Debug ---- */

void        lt9_print(lt9_tensor t);

/* ============================================================
 * Stage 3: TorchScript Model Loading + IValue + NN Functional
 * ============================================================ */

typedef void* lt9_model;
typedef void* lt9_ivalue;

/* ---- Model loading ---- */

/* Load a TorchScript model from disk (.pt file). */
lt9_model   lt9_model_load(const char *path);

/* Run forward pass with single tensor input, return tensor output. */
lt9_tensor  lt9_model_forward(lt9_model m, lt9_tensor input);

/* Run forward pass returning raw IValue (for tuple/list outputs like LSTM). */
lt9_ivalue  lt9_model_forward_ivalue(lt9_model m, lt9_tensor input);

/* Set model to eval mode (disables dropout, batchnorm training behavior). */
void        lt9_model_eval(lt9_model m);

/* Move model to CUDA device. */
void        lt9_model_to_cuda(lt9_model m, int device);

/* Free model. */
void        lt9_model_free(lt9_model m);

/* Create a simple test model in C++ (no Python needed).
 * The model computes: forward(x) = x * 2 + 1 */
lt9_model   lt9_model_create_test(void);

/* ---- IValue navigation (for structured outputs) ---- */

int         lt9_ivalue_is_tensor(lt9_ivalue iv);
int         lt9_ivalue_is_tuple(lt9_ivalue iv);
int         lt9_ivalue_is_list(lt9_ivalue iv);
lt9_tensor  lt9_ivalue_to_tensor(lt9_ivalue iv);
int         lt9_ivalue_tuple_size(lt9_ivalue iv);
lt9_ivalue  lt9_ivalue_tuple_get(lt9_ivalue iv, int index);
void        lt9_ivalue_free(lt9_ivalue iv);

/* ---- Functional NN operations ---- */

/* Conv2d: input[N,Cin,H,W] * weight[Cout,Cin,kH,kW] + bias[Cout] */
lt9_tensor  lt9_conv2d(lt9_tensor input, lt9_tensor weight, lt9_tensor bias,
                        int stride_h, int stride_w, int pad_h, int pad_w);

/* Batch normalization (inference mode). */
lt9_tensor  lt9_batch_norm(lt9_tensor input, lt9_tensor weight, lt9_tensor bias,
                            lt9_tensor running_mean, lt9_tensor running_var,
                            double eps, double momentum);

/* Layer normalization. */
lt9_tensor  lt9_layer_norm(lt9_tensor input, lt9_tensor weight, lt9_tensor bias,
                            int64_t *normalized_shape, int shape_len, double eps);

/* Max pool 2D. */
lt9_tensor  lt9_max_pool2d(lt9_tensor input, int kh, int kw,
                            int stride_h, int stride_w, int pad_h, int pad_w);

/* Average pool 2D. */
lt9_tensor  lt9_avg_pool2d(lt9_tensor input, int kh, int kw,
                            int stride_h, int stride_w, int pad_h, int pad_w);

/* Linear: xW^T + b. bias may be NULL. */
lt9_tensor  lt9_linear(lt9_tensor input, lt9_tensor weight, lt9_tensor bias);

/* Embedding lookup: weight[indices]. */
lt9_tensor  lt9_embedding(lt9_tensor weight, lt9_tensor indices);

/* Dropout (only active when training=1; pass-through when training=0). */
lt9_tensor  lt9_dropout(lt9_tensor input, double p, int training);


/* ============================================================
 * Stage 4: Training Support (Autograd + Optimizers + Save/Load)
 * ============================================================ */

typedef void* lt9_optimizer;
typedef void* lt9_param_group;

/* ---- Data-owning tensor creation ---- */

lt9_tensor  lt9_randn(int64_t *sizes, int ndim, int dtype);
lt9_tensor  lt9_zeros(int64_t *sizes, int ndim, int dtype);
lt9_tensor  lt9_ones(int64_t *sizes, int ndim, int dtype);
lt9_tensor  lt9_full(int64_t *sizes, int ndim, int dtype, double fill_value);

/* ---- Reductions + scalar extraction ---- */

lt9_tensor  lt9_sum(lt9_tensor t);
lt9_tensor  lt9_mean(lt9_tensor t);
double      lt9_item(lt9_tensor t);

/* ---- Element-wise ops ---- */

lt9_tensor  lt9_neg(lt9_tensor t);

/* ---- Autograd ---- */

int         lt9_requires_grad(lt9_tensor t);
void        lt9_requires_grad_(lt9_tensor t, int flag);
void        lt9_backward(lt9_tensor t);
void        lt9_backward_with_grad(lt9_tensor t, lt9_tensor gradient);
lt9_tensor  lt9_grad(lt9_tensor t);
int         lt9_grad_defined(lt9_tensor t);
int         lt9_grad_enabled(void);
void        lt9_set_grad_enabled(int enabled);
lt9_tensor  lt9_detach(lt9_tensor t);

/* ---- Loss functions (reduction: 0=None, 1=Mean, 2=Sum) ---- */

lt9_tensor  lt9_mse_loss(lt9_tensor input, lt9_tensor target, int reduction);
lt9_tensor  lt9_cross_entropy_loss(lt9_tensor input, lt9_tensor target,
                                    int reduction);

/* ---- Optimizer: param group builder ---- */

lt9_param_group lt9_param_group_create(void);
void            lt9_param_group_add(lt9_param_group pg, lt9_tensor t);
void            lt9_param_group_free(lt9_param_group pg);

/* ---- Optimizer: creation ---- */

lt9_optimizer lt9_sgd_create(lt9_param_group pg, double lr, double momentum,
                              double dampening, double weight_decay, int nesterov);
lt9_optimizer lt9_adam_create(lt9_param_group pg, double lr, double beta1,
                               double beta2, double eps, double weight_decay,
                               int amsgrad);

/* ---- Optimizer: operations ---- */

void   lt9_optimizer_step(lt9_optimizer opt);
void   lt9_optimizer_zero_grad(lt9_optimizer opt);
double lt9_optimizer_get_lr(lt9_optimizer opt);
void   lt9_optimizer_set_lr(lt9_optimizer opt, double lr);
void   lt9_optimizer_free(lt9_optimizer opt);

/* ---- Tensor save/load ---- */

int         lt9_tensor_save(lt9_tensor t, const char *path);
lt9_tensor  lt9_tensor_load(const char *path);


/* ============================================================
 * Stage 5: Training Toolkit
 * ============================================================ */

/* ---- Element-wise math ---- */

lt9_tensor  lt9_exp(lt9_tensor t);
lt9_tensor  lt9_log(lt9_tensor t);
lt9_tensor  lt9_sqrt(lt9_tensor t);
lt9_tensor  lt9_abs(lt9_tensor t);
lt9_tensor  lt9_pow(lt9_tensor t, double exponent);
lt9_tensor  lt9_clamp(lt9_tensor t, double min_val, double max_val);

/* ---- Scalar-tensor ops ---- */

lt9_tensor  lt9_add_scalar(lt9_tensor t, double scalar);
lt9_tensor  lt9_mul_scalar(lt9_tensor t, double scalar);

/* ---- Type casting ---- */

lt9_tensor  lt9_to_dtype(lt9_tensor t, int dtype);

/* ---- Index generation ---- */

lt9_tensor  lt9_arange(double start, double end, double step, int dtype);

/* ---- Comparisons (return uint8 tensors) ---- */

lt9_tensor  lt9_eq(lt9_tensor a, lt9_tensor b);
lt9_tensor  lt9_gt(lt9_tensor a, lt9_tensor b);

/* ---- Reductions with dimension ---- */

lt9_tensor  lt9_argmax(lt9_tensor t, int dim);
lt9_tensor  lt9_sum_dim(lt9_tensor t, int dim, int keepdim);
lt9_tensor  lt9_mean_dim(lt9_tensor t, int dim, int keepdim);
lt9_tensor  lt9_max_dim(lt9_tensor t, int dim);

/* ---- Indexing ---- */

lt9_tensor  lt9_narrow(lt9_tensor t, int dim, int64_t start, int64_t length);
lt9_tensor  lt9_index_select(lt9_tensor t, int dim, lt9_tensor index);
lt9_tensor  lt9_select(lt9_tensor t, int dim, int64_t index);

/* ---- Flatten ---- */

lt9_tensor  lt9_flatten(lt9_tensor t, int start_dim, int end_dim);

/* ---- In-place operations ---- */

void  lt9_add_inplace(lt9_tensor t, lt9_tensor other);
void  lt9_mul_inplace(lt9_tensor t, lt9_tensor other);
void  lt9_sub_inplace(lt9_tensor t, lt9_tensor other);
void  lt9_zero_inplace(lt9_tensor t);
void  lt9_fill_inplace(lt9_tensor t, double value);
void  lt9_copy_inplace(lt9_tensor dst, lt9_tensor src);

/* ---- Parameter initialization (in-place) ---- */

void  lt9_normal_init(lt9_tensor t, double mean, double std);
void  lt9_uniform_init(lt9_tensor t, double low, double high);
void  lt9_kaiming_normal_init(lt9_tensor t, double a, int mode,
                               int nonlinearity);
void  lt9_xavier_normal_init(lt9_tensor t, double gain);

/* ---- Gradient clipping ---- */

double lt9_clip_grad_norm(lt9_param_group pg, double max_norm);

/* ---- Extra activations ---- */

lt9_tensor  lt9_gelu(lt9_tensor t);
lt9_tensor  lt9_silu(lt9_tensor t);
lt9_tensor  lt9_elu(lt9_tensor t, double alpha);

/* ---- Extra loss functions (reduction: 0=None, 1=Mean, 2=Sum) ---- */

lt9_tensor  lt9_nll_loss(lt9_tensor input, lt9_tensor target, int reduction);
lt9_tensor  lt9_bce_with_logits_loss(lt9_tensor input, lt9_tensor target,
                                      int reduction);
lt9_tensor  lt9_l1_loss(lt9_tensor input, lt9_tensor target, int reduction);
lt9_tensor  lt9_smooth_l1_loss(lt9_tensor input, lt9_tensor target,
                                int reduction, double beta);

#ifdef __cplusplus
}
#endif

#endif /* LUSH_TORCH9_BRIDGE_H */
