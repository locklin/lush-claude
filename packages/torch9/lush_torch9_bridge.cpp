/* lush_torch9_bridge.cpp -- C++ implementation of the Lush-libtorch bridge
 *
 * Each lt9_tensor handle is a heap-allocated torch::Tensor*.
 * This file is compiled into lush_torch9_bridge.so and loaded
 * via mod-load from torch9-config.lsh.
 */

#include <torch/torch.h>
#include <torch/script.h>
#include <iostream>
#include <vector>
#include "lush_torch9_bridge.h"

/* ---- Internal: Lush ST_* enum → torch ScalarType mapping ---- */

/* Lush storage_type enum (from include/header.h):
 *   ST_AT=0, ST_P=1, ST_F=2, ST_D=3, ST_I32=4,
 *   ST_I16=5, ST_I8=6, ST_U8=7, ST_GPTR=8, ST_I64=9
 */
static const torch::ScalarType st_to_torch[] = {
    torch::kFloat32,   /* 0: ST_AT   (placeholder, unused) */
    torch::kFloat32,   /* 1: ST_P    (placeholder, unused) */
    torch::kFloat32,   /* 2: ST_F    */
    torch::kFloat64,   /* 3: ST_D    */
    torch::kInt32,     /* 4: ST_I32  */
    torch::kInt16,     /* 5: ST_I16  */
    torch::kInt8,      /* 6: ST_I8   */
    torch::kUInt8,     /* 7: ST_U8   */
    torch::kFloat32,   /* 8: ST_GPTR (placeholder, unused) */
    torch::kInt64,     /* 9: ST_I64  */
};

/* Reverse mapping: torch ScalarType → Lush ST_* */
static int torch_to_st(torch::ScalarType st) {
    switch (st) {
        case torch::kFloat32: return 2;   /* ST_F   */
        case torch::kFloat64: return 3;   /* ST_D   */
        case torch::kInt32:   return 4;   /* ST_I32 */
        case torch::kInt16:   return 5;   /* ST_I16 */
        case torch::kInt8:    return 6;   /* ST_I8  */
        case torch::kUInt8:   return 7;   /* ST_U8  */
        case torch::kInt64:   return 9;   /* ST_I64 */
        default:              return 2;   /* fallback to ST_F */
    }
}

/* ---- Initialization ---- */

int lt9_init(void) {
    return torch::cuda::is_available() ? 1 : 0;
}

void lt9_cleanup(void) {
    /* Nothing to do currently; libtorch cleans up at process exit. */
}

/* ---- Tensor creation (zero-copy) ---- */

lt9_tensor lt9_from_blob(void *data, int64_t *sizes, int64_t *strides,
                         int ndim, int dtype) {
    if (!data || ndim < 1 || dtype < 0 || dtype > 9)
        return nullptr;

    auto opts = torch::TensorOptions().dtype(st_to_torch[dtype]);
    auto t = torch::from_blob(
        data,
        c10::IntArrayRef(sizes, static_cast<size_t>(ndim)),
        c10::IntArrayRef(strides, static_cast<size_t>(ndim)),
        opts);
    return static_cast<lt9_tensor>(new torch::Tensor(t));
}

/* ---- Tensor introspection ---- */

void* lt9_data_ptr(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return tp->data_ptr();
}

int lt9_ndim(lt9_tensor t) {
    if (!t) return 0;
    auto *tp = static_cast<torch::Tensor*>(t);
    return static_cast<int>(tp->dim());
}

void lt9_sizes(lt9_tensor t, int64_t *out) {
    if (!t || !out) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    auto s = tp->sizes();
    for (int64_t i = 0; i < tp->dim(); i++)
        out[i] = s[i];
}

void lt9_strides(lt9_tensor t, int64_t *out) {
    if (!t || !out) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    auto s = tp->strides();
    for (int64_t i = 0; i < tp->dim(); i++)
        out[i] = s[i];
}

int lt9_dtype(lt9_tensor t) {
    if (!t) return 2;  /* default ST_F */
    auto *tp = static_cast<torch::Tensor*>(t);
    return torch_to_st(tp->scalar_type());
}

int64_t lt9_numel(lt9_tensor t) {
    if (!t) return 0;
    auto *tp = static_cast<torch::Tensor*>(t);
    return tp->numel();
}

/* ---- Core operations ---- */

lt9_tensor lt9_matmul(lt9_tensor a, lt9_tensor b) {
    if (!a || !b) return nullptr;
    auto *ta = static_cast<torch::Tensor*>(a);
    auto *tb = static_cast<torch::Tensor*>(b);
    return static_cast<lt9_tensor>(new torch::Tensor(torch::matmul(*ta, *tb)));
}

lt9_tensor lt9_add(lt9_tensor a, lt9_tensor b) {
    if (!a || !b) return nullptr;
    auto *ta = static_cast<torch::Tensor*>(a);
    auto *tb = static_cast<torch::Tensor*>(b);
    return static_cast<lt9_tensor>(new torch::Tensor(torch::add(*ta, *tb)));
}

lt9_tensor lt9_sub(lt9_tensor a, lt9_tensor b) {
    if (!a || !b) return nullptr;
    auto *ta = static_cast<torch::Tensor*>(a);
    auto *tb = static_cast<torch::Tensor*>(b);
    return static_cast<lt9_tensor>(new torch::Tensor(torch::sub(*ta, *tb)));
}

lt9_tensor lt9_mul(lt9_tensor a, lt9_tensor b) {
    if (!a || !b) return nullptr;
    auto *ta = static_cast<torch::Tensor*>(a);
    auto *tb = static_cast<torch::Tensor*>(b);
    return static_cast<lt9_tensor>(new torch::Tensor(torch::mul(*ta, *tb)));
}

lt9_tensor lt9_div(lt9_tensor a, lt9_tensor b) {
    if (!a || !b) return nullptr;
    auto *ta = static_cast<torch::Tensor*>(a);
    auto *tb = static_cast<torch::Tensor*>(b);
    return static_cast<lt9_tensor>(new torch::Tensor(torch::div(*ta, *tb)));
}

/* ---- Activations ---- */

lt9_tensor lt9_relu(lt9_tensor tens) {
    if (!tens) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(tens);
    return static_cast<lt9_tensor>(new torch::Tensor(torch::relu(*tp)));
}

lt9_tensor lt9_sigmoid(lt9_tensor tens) {
    if (!tens) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(tens);
    return static_cast<lt9_tensor>(new torch::Tensor(torch::sigmoid(*tp)));
}

lt9_tensor lt9_tanh(lt9_tensor tens) {
    if (!tens) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(tens);
    return static_cast<lt9_tensor>(new torch::Tensor(torch::tanh(*tp)));
}

lt9_tensor lt9_softmax(lt9_tensor tens, int dim) {
    if (!tens) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(tens);
    return static_cast<lt9_tensor>(
        new torch::Tensor(torch::softmax(*tp, dim)));
}

lt9_tensor lt9_log_softmax(lt9_tensor tens, int dim) {
    if (!tens) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(tens);
    return static_cast<lt9_tensor>(
        new torch::Tensor(torch::log_softmax(*tp, dim)));
}

/* ---- Shape manipulation ---- */

lt9_tensor lt9_reshape(lt9_tensor tens, int64_t *shape, int ndim) {
    if (!tens || !shape || ndim < 1) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(tens);
    auto result = tp->reshape(c10::IntArrayRef(shape, static_cast<size_t>(ndim)));
    return static_cast<lt9_tensor>(new torch::Tensor(result.contiguous()));
}

lt9_tensor lt9_transpose(lt9_tensor tens, int dim0, int dim1) {
    if (!tens) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(tens);
    auto result = tp->transpose(dim0, dim1);
    return static_cast<lt9_tensor>(new torch::Tensor(result.contiguous()));
}

lt9_tensor lt9_permute(lt9_tensor tens, int64_t *dims, int ndim) {
    if (!tens || !dims || ndim < 1) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(tens);
    auto result = tp->permute(c10::IntArrayRef(dims, static_cast<size_t>(ndim)));
    return static_cast<lt9_tensor>(new torch::Tensor(result.contiguous()));
}

lt9_tensor lt9_squeeze(lt9_tensor tens, int dim) {
    if (!tens) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(tens);
    auto result = tp->squeeze(dim);
    return static_cast<lt9_tensor>(new torch::Tensor(result.contiguous()));
}

lt9_tensor lt9_unsqueeze(lt9_tensor tens, int dim) {
    if (!tens) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(tens);
    auto result = tp->unsqueeze(dim);
    return static_cast<lt9_tensor>(new torch::Tensor(result.contiguous()));
}

lt9_tensor lt9_cat2(lt9_tensor a, lt9_tensor b, int dim) {
    if (!a || !b) return nullptr;
    auto *ta = static_cast<torch::Tensor*>(a);
    auto *tb = static_cast<torch::Tensor*>(b);
    std::vector<torch::Tensor> tensors = {*ta, *tb};
    return static_cast<lt9_tensor>(new torch::Tensor(torch::cat(tensors, dim)));
}

lt9_tensor lt9_cat3(lt9_tensor a, lt9_tensor b, lt9_tensor c, int dim) {
    if (!a || !b || !c) return nullptr;
    auto *ta = static_cast<torch::Tensor*>(a);
    auto *tb = static_cast<torch::Tensor*>(b);
    auto *tc = static_cast<torch::Tensor*>(c);
    std::vector<torch::Tensor> tensors = {*ta, *tb, *tc};
    return static_cast<lt9_tensor>(new torch::Tensor(torch::cat(tensors, dim)));
}

lt9_tensor lt9_cat4(lt9_tensor a, lt9_tensor b, lt9_tensor c,
                    lt9_tensor d, int dim) {
    if (!a || !b || !c || !d) return nullptr;
    auto *ta = static_cast<torch::Tensor*>(a);
    auto *tb = static_cast<torch::Tensor*>(b);
    auto *tc = static_cast<torch::Tensor*>(c);
    auto *td = static_cast<torch::Tensor*>(d);
    std::vector<torch::Tensor> tensors = {*ta, *tb, *tc, *td};
    return static_cast<lt9_tensor>(new torch::Tensor(torch::cat(tensors, dim)));
}

/* ---- GPU transfer ---- */

lt9_tensor lt9_to_cuda(lt9_tensor tens, int device) {
    if (!tens) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(tens);
    if (!torch::cuda::is_available()) return nullptr;
    auto result = tp->to(torch::Device(torch::kCUDA, device));
    return static_cast<lt9_tensor>(new torch::Tensor(result));
}

lt9_tensor lt9_to_cpu(lt9_tensor tens) {
    if (!tens) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(tens);
    auto result = tp->to(torch::kCPU);
    return static_cast<lt9_tensor>(new torch::Tensor(result));
}

int lt9_device_type(lt9_tensor tens) {
    if (!tens) return 0;
    auto *tp = static_cast<torch::Tensor*>(tens);
    return tp->device().is_cuda() ? 1 : 0;
}

/* ---- Lifecycle ---- */

lt9_tensor lt9_clone(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return static_cast<lt9_tensor>(new torch::Tensor(tp->clone()));
}

void lt9_free(lt9_tensor t) {
    if (t)
        delete static_cast<torch::Tensor*>(t);
}

/* ---- Debug ---- */

void lt9_print(lt9_tensor t) {
    if (!t) {
        std::cout << "lt9_tensor: (null)" << std::endl;
        return;
    }
    auto *tp = static_cast<torch::Tensor*>(t);
    std::cout << *tp << std::endl;
}

/* ============================================================
 * Stage 3: TorchScript Model Loading + IValue + NN Functional
 * ============================================================ */

/* ---- Model loading ---- */

lt9_model lt9_model_load(const char *path) {
    if (!path) return nullptr;
    try {
        auto *mod = new torch::jit::Module(torch::jit::load(path));
        return static_cast<lt9_model>(mod);
    } catch (const c10::Error &e) {
        std::cerr << "lt9_model_load error: " << e.what() << std::endl;
        return nullptr;
    }
}

lt9_tensor lt9_model_forward(lt9_model m, lt9_tensor input) {
    if (!m || !input) return nullptr;
    auto *mod = static_cast<torch::jit::Module*>(m);
    auto *in  = static_cast<torch::Tensor*>(input);
    try {
        auto result = mod->forward({*in});
        return new torch::Tensor(result.toTensor());
    } catch (const c10::Error &e) {
        std::cerr << "lt9_model_forward error: " << e.what() << std::endl;
        return nullptr;
    }
}

lt9_ivalue lt9_model_forward_ivalue(lt9_model m, lt9_tensor input) {
    if (!m || !input) return nullptr;
    auto *mod = static_cast<torch::jit::Module*>(m);
    auto *in  = static_cast<torch::Tensor*>(input);
    try {
        auto result = mod->forward({*in});
        return new torch::jit::IValue(result);
    } catch (const c10::Error &e) {
        std::cerr << "lt9_model_forward_ivalue error: " << e.what() << std::endl;
        return nullptr;
    }
}

void lt9_model_eval(lt9_model m) {
    if (!m) return;
    auto *mod = static_cast<torch::jit::Module*>(m);
    mod->eval();
}

void lt9_model_to_cuda(lt9_model m, int device) {
    if (!m) return;
    auto *mod = static_cast<torch::jit::Module*>(m);
    if (torch::cuda::is_available())
        mod->to(torch::Device(torch::kCUDA, device));
}

void lt9_model_free(lt9_model m) {
    if (m)
        delete static_cast<torch::jit::Module*>(m);
}

lt9_model lt9_model_create_test(void) {
    try {
        auto mod = new torch::jit::Module("TestModule");
        mod->define(
            "def forward(self, x: Tensor) -> Tensor:\n"
            "    return x * 2 + 1\n"
        );
        return static_cast<lt9_model>(mod);
    } catch (const c10::Error &e) {
        std::cerr << "lt9_model_create_test error: " << e.what() << std::endl;
        return nullptr;
    }
}

/* ---- IValue navigation ---- */

int lt9_ivalue_is_tensor(lt9_ivalue iv) {
    if (!iv) return 0;
    auto *val = static_cast<torch::jit::IValue*>(iv);
    return val->isTensor() ? 1 : 0;
}

int lt9_ivalue_is_tuple(lt9_ivalue iv) {
    if (!iv) return 0;
    auto *val = static_cast<torch::jit::IValue*>(iv);
    return val->isTuple() ? 1 : 0;
}

int lt9_ivalue_is_list(lt9_ivalue iv) {
    if (!iv) return 0;
    auto *val = static_cast<torch::jit::IValue*>(iv);
    return val->isList() ? 1 : 0;
}

lt9_tensor lt9_ivalue_to_tensor(lt9_ivalue iv) {
    if (!iv) return nullptr;
    auto *val = static_cast<torch::jit::IValue*>(iv);
    if (!val->isTensor()) return nullptr;
    return new torch::Tensor(val->toTensor());
}

int lt9_ivalue_tuple_size(lt9_ivalue iv) {
    if (!iv) return 0;
    auto *val = static_cast<torch::jit::IValue*>(iv);
    if (!val->isTuple()) return 0;
    return static_cast<int>(val->toTuple()->elements().size());
}

lt9_ivalue lt9_ivalue_tuple_get(lt9_ivalue iv, int index) {
    if (!iv) return nullptr;
    auto *val = static_cast<torch::jit::IValue*>(iv);
    if (!val->isTuple()) return nullptr;
    auto &elems = val->toTuple()->elements();
    if (index < 0 || index >= static_cast<int>(elems.size())) return nullptr;
    return new torch::jit::IValue(elems[index]);
}

void lt9_ivalue_free(lt9_ivalue iv) {
    if (iv)
        delete static_cast<torch::jit::IValue*>(iv);
}

/* ---- Functional NN operations ---- */

lt9_tensor lt9_conv2d(lt9_tensor input, lt9_tensor weight, lt9_tensor bias,
                       int stride_h, int stride_w, int pad_h, int pad_w) {
    if (!input || !weight) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    auto *w  = static_cast<torch::Tensor*>(weight);
    auto b = bias ? c10::optional<torch::Tensor>(*static_cast<torch::Tensor*>(bias))
                  : c10::nullopt;
    return new torch::Tensor(torch::conv2d(*in, *w, b,
        /*stride=*/{stride_h, stride_w}, /*padding=*/{pad_h, pad_w}));
}

lt9_tensor lt9_batch_norm(lt9_tensor input, lt9_tensor weight, lt9_tensor bias,
                           lt9_tensor running_mean, lt9_tensor running_var,
                           double eps, double momentum) {
    if (!input) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    auto w    = weight       ? c10::optional<torch::Tensor>(*static_cast<torch::Tensor*>(weight))       : c10::nullopt;
    auto b    = bias         ? c10::optional<torch::Tensor>(*static_cast<torch::Tensor*>(bias))         : c10::nullopt;
    auto mean = running_mean ? c10::optional<torch::Tensor>(*static_cast<torch::Tensor*>(running_mean)) : c10::nullopt;
    auto var  = running_var  ? c10::optional<torch::Tensor>(*static_cast<torch::Tensor*>(running_var))  : c10::nullopt;
    return new torch::Tensor(torch::batch_norm(*in, w, b, mean, var,
        /*training=*/false, momentum, eps, /*cudnn_enabled=*/true));
}

lt9_tensor lt9_layer_norm(lt9_tensor input, lt9_tensor weight, lt9_tensor bias,
                           int64_t *normalized_shape, int shape_len, double eps) {
    if (!input || !normalized_shape || shape_len < 1) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    auto shape = c10::IntArrayRef(normalized_shape, static_cast<size_t>(shape_len));
    auto w = weight ? c10::optional<torch::Tensor>(*static_cast<torch::Tensor*>(weight)) : c10::nullopt;
    auto b = bias   ? c10::optional<torch::Tensor>(*static_cast<torch::Tensor*>(bias))   : c10::nullopt;
    return new torch::Tensor(torch::layer_norm(*in, shape, w, b, eps));
}

lt9_tensor lt9_max_pool2d(lt9_tensor input, int kh, int kw,
                           int stride_h, int stride_w, int pad_h, int pad_w) {
    if (!input) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    return new torch::Tensor(torch::max_pool2d(*in,
        /*kernel_size=*/{kh, kw}, /*stride=*/{stride_h, stride_w},
        /*padding=*/{pad_h, pad_w}));
}

lt9_tensor lt9_avg_pool2d(lt9_tensor input, int kh, int kw,
                           int stride_h, int stride_w, int pad_h, int pad_w) {
    if (!input) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    return new torch::Tensor(torch::avg_pool2d(*in,
        /*kernel_size=*/{kh, kw}, /*stride=*/{stride_h, stride_w},
        /*padding=*/{pad_h, pad_w}));
}

lt9_tensor lt9_linear(lt9_tensor input, lt9_tensor weight, lt9_tensor bias) {
    if (!input || !weight) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    auto *w  = static_cast<torch::Tensor*>(weight);
    auto b = bias ? c10::optional<torch::Tensor>(*static_cast<torch::Tensor*>(bias))
                  : c10::nullopt;
    return new torch::Tensor(torch::linear(*in, *w, b));
}

lt9_tensor lt9_embedding(lt9_tensor weight, lt9_tensor indices) {
    if (!weight || !indices) return nullptr;
    auto *w = static_cast<torch::Tensor*>(weight);
    auto *i = static_cast<torch::Tensor*>(indices);
    return new torch::Tensor(torch::embedding(*w, *i));
}

lt9_tensor lt9_dropout(lt9_tensor input, double p, int training) {
    if (!input) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    return new torch::Tensor(torch::dropout(*in, p, training != 0));
}

/* ============================================================
 * Stage 4: Training Support (Autograd + Optimizers + Save/Load)
 * ============================================================ */

/* ---- Data-owning tensor creation ---- */

lt9_tensor lt9_randn(int64_t *sizes, int ndim, int dtype) {
    if (!sizes || ndim < 1 || dtype < 0 || dtype > 9) return nullptr;
    auto opts = torch::TensorOptions().dtype(st_to_torch[dtype]);
    return new torch::Tensor(torch::randn(
        c10::IntArrayRef(sizes, static_cast<size_t>(ndim)), opts));
}

lt9_tensor lt9_zeros(int64_t *sizes, int ndim, int dtype) {
    if (!sizes || ndim < 1 || dtype < 0 || dtype > 9) return nullptr;
    auto opts = torch::TensorOptions().dtype(st_to_torch[dtype]);
    return new torch::Tensor(torch::zeros(
        c10::IntArrayRef(sizes, static_cast<size_t>(ndim)), opts));
}

lt9_tensor lt9_ones(int64_t *sizes, int ndim, int dtype) {
    if (!sizes || ndim < 1 || dtype < 0 || dtype > 9) return nullptr;
    auto opts = torch::TensorOptions().dtype(st_to_torch[dtype]);
    return new torch::Tensor(torch::ones(
        c10::IntArrayRef(sizes, static_cast<size_t>(ndim)), opts));
}

lt9_tensor lt9_full(int64_t *sizes, int ndim, int dtype, double fill_value) {
    if (!sizes || ndim < 1 || dtype < 0 || dtype > 9) return nullptr;
    auto opts = torch::TensorOptions().dtype(st_to_torch[dtype]);
    return new torch::Tensor(torch::full(
        c10::IntArrayRef(sizes, static_cast<size_t>(ndim)),
        fill_value, opts));
}

/* ---- Reductions + scalar extraction ---- */

lt9_tensor lt9_sum(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(tp->sum());
}

lt9_tensor lt9_mean(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(tp->mean());
}

double lt9_item(lt9_tensor t) {
    if (!t) return 0.0;
    auto *tp = static_cast<torch::Tensor*>(t);
    return tp->item<double>();
}

/* ---- Element-wise ops ---- */

lt9_tensor lt9_neg(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::neg(*tp));
}

/* ---- Autograd ---- */

int lt9_requires_grad(lt9_tensor t) {
    if (!t) return 0;
    auto *tp = static_cast<torch::Tensor*>(t);
    return tp->requires_grad() ? 1 : 0;
}

void lt9_requires_grad_(lt9_tensor t, int flag) {
    if (!t) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    tp->set_requires_grad(flag != 0);
}

void lt9_backward(lt9_tensor t) {
    if (!t) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    tp->backward();
}

void lt9_backward_with_grad(lt9_tensor t, lt9_tensor gradient) {
    if (!t || !gradient) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    auto *gp = static_cast<torch::Tensor*>(gradient);
    tp->backward(*gp);
}

lt9_tensor lt9_grad(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    if (!tp->grad().defined()) return nullptr;
    return new torch::Tensor(tp->grad());
}

int lt9_grad_defined(lt9_tensor t) {
    if (!t) return 0;
    auto *tp = static_cast<torch::Tensor*>(t);
    return tp->grad().defined() ? 1 : 0;
}

int lt9_grad_enabled(void) {
    return torch::GradMode::is_enabled() ? 1 : 0;
}

void lt9_set_grad_enabled(int enabled) {
    torch::GradMode::set_enabled(enabled != 0);
}

lt9_tensor lt9_detach(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(tp->detach());
}

/* ---- Loss functions ---- */

static torch::Reduction::Reduction int_to_reduction(int r) {
    switch (r) {
        case 0: return torch::Reduction::None;
        case 1: return torch::Reduction::Mean;
        case 2: return torch::Reduction::Sum;
        default: return torch::Reduction::Mean;
    }
}

lt9_tensor lt9_mse_loss(lt9_tensor input, lt9_tensor target, int reduction) {
    if (!input || !target) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    auto *tgt = static_cast<torch::Tensor*>(target);
    return new torch::Tensor(torch::mse_loss(*in, *tgt, int_to_reduction(reduction)));
}

lt9_tensor lt9_cross_entropy_loss(lt9_tensor input, lt9_tensor target,
                                   int reduction) {
    if (!input || !target) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    auto *tgt = static_cast<torch::Tensor*>(target);
    /* cross_entropy_loss requires Long (int64) targets */
    auto tgt64 = tgt->to(torch::kInt64);
    return new torch::Tensor(torch::cross_entropy_loss(
        *in, tgt64, {}, int_to_reduction(reduction)));
}

/* ---- Optimizer: param group builder ---- */

lt9_param_group lt9_param_group_create(void) {
    return new std::vector<torch::Tensor>();
}

void lt9_param_group_add(lt9_param_group pg, lt9_tensor t) {
    if (!pg || !t) return;
    auto *params = static_cast<std::vector<torch::Tensor>*>(pg);
    auto *tp = static_cast<torch::Tensor*>(t);
    params->push_back(*tp);
}

void lt9_param_group_free(lt9_param_group pg) {
    if (pg)
        delete static_cast<std::vector<torch::Tensor>*>(pg);
}

/* ---- Optimizer: creation ---- */

lt9_optimizer lt9_sgd_create(lt9_param_group pg, double lr, double momentum,
                              double dampening, double weight_decay, int nesterov) {
    if (!pg) return nullptr;
    auto *params = static_cast<std::vector<torch::Tensor>*>(pg);
    try {
        auto opts = torch::optim::SGDOptions(lr)
            .momentum(momentum)
            .dampening(dampening)
            .weight_decay(weight_decay)
            .nesterov(nesterov != 0);
        return new torch::optim::SGD(*params, opts);
    } catch (const c10::Error &e) {
        std::cerr << "lt9_sgd_create error: " << e.what() << std::endl;
        return nullptr;
    }
}

lt9_optimizer lt9_adam_create(lt9_param_group pg, double lr, double beta1,
                               double beta2, double eps, double weight_decay,
                               int amsgrad) {
    if (!pg) return nullptr;
    auto *params = static_cast<std::vector<torch::Tensor>*>(pg);
    try {
        auto opts = torch::optim::AdamOptions(lr)
            .betas(std::make_tuple(beta1, beta2))
            .eps(eps)
            .weight_decay(weight_decay)
            .amsgrad(amsgrad != 0);
        return new torch::optim::Adam(*params, opts);
    } catch (const c10::Error &e) {
        std::cerr << "lt9_adam_create error: " << e.what() << std::endl;
        return nullptr;
    }
}

/* ---- Optimizer: operations ---- */

void lt9_optimizer_step(lt9_optimizer opt) {
    if (!opt) return;
    auto *op = static_cast<torch::optim::Optimizer*>(opt);
    op->step();
}

void lt9_optimizer_zero_grad(lt9_optimizer opt) {
    if (!opt) return;
    auto *op = static_cast<torch::optim::Optimizer*>(opt);
    op->zero_grad();
}

double lt9_optimizer_get_lr(lt9_optimizer opt) {
    if (!opt) return 0.0;
    auto *op = static_cast<torch::optim::Optimizer*>(opt);
    for (auto &pg : op->param_groups()) {
        if (auto *sgd_opts = dynamic_cast<torch::optim::SGDOptions*>(&pg.options())) {
            return sgd_opts->lr();
        } else if (auto *adam_opts = dynamic_cast<torch::optim::AdamOptions*>(&pg.options())) {
            return adam_opts->lr();
        }
    }
    return 0.0;
}

void lt9_optimizer_set_lr(lt9_optimizer opt, double lr) {
    if (!opt) return;
    auto *op = static_cast<torch::optim::Optimizer*>(opt);
    /* Update defaults and all param groups */
    for (auto &pg : op->param_groups()) {
        if (auto *sgd_opts = dynamic_cast<torch::optim::SGDOptions*>(&pg.options())) {
            sgd_opts->lr(lr);
        } else if (auto *adam_opts = dynamic_cast<torch::optim::AdamOptions*>(&pg.options())) {
            adam_opts->lr(lr);
        }
    }
}

void lt9_optimizer_free(lt9_optimizer opt) {
    if (!opt) return;
    /* We need to determine the actual type to delete properly */
    auto *op = static_cast<torch::optim::Optimizer*>(opt);
    if (auto *sgd = dynamic_cast<torch::optim::SGD*>(op)) {
        delete sgd;
    } else if (auto *adam = dynamic_cast<torch::optim::Adam*>(op)) {
        delete adam;
    } else {
        /* Fallback: delete as base (may leak, but shouldn't reach here) */
        delete op;
    }
}

/* ---- Tensor save/load ---- */

int lt9_tensor_save(lt9_tensor t, const char *path) {
    if (!t || !path) return -1;
    auto *tp = static_cast<torch::Tensor*>(t);
    try {
        torch::save(*tp, path);
        return 0;
    } catch (...) {
        return -1;
    }
}

lt9_tensor lt9_tensor_load(const char *path) {
    if (!path) return nullptr;
    torch::Tensor t;
    try {
        torch::load(t, path);
        return new torch::Tensor(t);
    } catch (...) {
        return nullptr;
    }
}

/* ============================================================
 * Stage 5: Training Toolkit
 * ============================================================ */

/* ---- Element-wise math ---- */

lt9_tensor lt9_exp(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::exp(*tp));
}

lt9_tensor lt9_log(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::log(*tp));
}

lt9_tensor lt9_sqrt(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::sqrt(*tp));
}

lt9_tensor lt9_abs(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::abs(*tp));
}

lt9_tensor lt9_pow(lt9_tensor t, double exponent) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::pow(*tp, exponent));
}

lt9_tensor lt9_clamp(lt9_tensor t, double min_val, double max_val) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::clamp(*tp, min_val, max_val));
}

/* ---- Scalar-tensor ops ---- */

lt9_tensor lt9_add_scalar(lt9_tensor t, double scalar) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(*tp + scalar);
}

lt9_tensor lt9_mul_scalar(lt9_tensor t, double scalar) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(*tp * scalar);
}

/* ---- Type casting ---- */

lt9_tensor lt9_to_dtype(lt9_tensor t, int dtype) {
    if (!t || dtype < 0 || dtype > 9) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(tp->to(st_to_torch[dtype]));
}

/* ---- Index generation ---- */

lt9_tensor lt9_arange(double start, double end, double step, int dtype) {
    if (dtype < 0 || dtype > 9) return nullptr;
    auto opts = torch::TensorOptions().dtype(st_to_torch[dtype]);
    return new torch::Tensor(torch::arange(start, end, step, opts));
}

/* ---- Comparisons ---- */

lt9_tensor lt9_eq(lt9_tensor a, lt9_tensor b) {
    if (!a || !b) return nullptr;
    auto *ta = static_cast<torch::Tensor*>(a);
    auto *tb = static_cast<torch::Tensor*>(b);
    return new torch::Tensor(torch::eq(*ta, *tb));
}

lt9_tensor lt9_gt(lt9_tensor a, lt9_tensor b) {
    if (!a || !b) return nullptr;
    auto *ta = static_cast<torch::Tensor*>(a);
    auto *tb = static_cast<torch::Tensor*>(b);
    return new torch::Tensor(torch::gt(*ta, *tb));
}

/* ---- Reductions with dimension ---- */

lt9_tensor lt9_argmax(lt9_tensor t, int dim) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::argmax(*tp, dim));
}

lt9_tensor lt9_sum_dim(lt9_tensor t, int dim, int keepdim) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::sum(*tp, dim, keepdim != 0));
}

lt9_tensor lt9_mean_dim(lt9_tensor t, int dim, int keepdim) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::mean(*tp, dim, keepdim != 0));
}

lt9_tensor lt9_max_dim(lt9_tensor t, int dim) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(std::get<0>(torch::max(*tp, dim)));
}

/* ---- Indexing ---- */

lt9_tensor lt9_narrow(lt9_tensor t, int dim, int64_t start, int64_t length) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    auto result = tp->narrow(dim, start, length);
    return new torch::Tensor(result.contiguous());
}

lt9_tensor lt9_index_select(lt9_tensor t, int dim, lt9_tensor index) {
    if (!t || !index) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    auto *idx = static_cast<torch::Tensor*>(index);
    return new torch::Tensor(torch::index_select(*tp, dim, *idx));
}

lt9_tensor lt9_select(lt9_tensor t, int dim, int64_t index) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    auto result = torch::select(*tp, dim, index);
    return new torch::Tensor(result.contiguous());
}

/* ---- Flatten ---- */

lt9_tensor lt9_flatten(lt9_tensor t, int start_dim, int end_dim) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::flatten(*tp, start_dim, end_dim));
}

/* ---- In-place operations ---- */

void lt9_add_inplace(lt9_tensor t, lt9_tensor other) {
    if (!t || !other) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    auto *op = static_cast<torch::Tensor*>(other);
    tp->add_(*op);
}

void lt9_mul_inplace(lt9_tensor t, lt9_tensor other) {
    if (!t || !other) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    auto *op = static_cast<torch::Tensor*>(other);
    tp->mul_(*op);
}

void lt9_sub_inplace(lt9_tensor t, lt9_tensor other) {
    if (!t || !other) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    auto *op = static_cast<torch::Tensor*>(other);
    tp->sub_(*op);
}

void lt9_zero_inplace(lt9_tensor t) {
    if (!t) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    tp->zero_();
}

void lt9_fill_inplace(lt9_tensor t, double value) {
    if (!t) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    tp->fill_(value);
}

void lt9_copy_inplace(lt9_tensor dst, lt9_tensor src) {
    if (!dst || !src) return;
    auto *dp = static_cast<torch::Tensor*>(dst);
    auto *sp = static_cast<torch::Tensor*>(src);
    dp->copy_(*sp);
}

/* ---- Parameter initialization ---- */

void lt9_normal_init(lt9_tensor t, double mean, double std) {
    if (!t) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    {
        torch::NoGradGuard no_grad;
        tp->normal_(mean, std);
    }
}

void lt9_uniform_init(lt9_tensor t, double low, double high) {
    if (!t) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    {
        torch::NoGradGuard no_grad;
        tp->uniform_(low, high);
    }
}

void lt9_kaiming_normal_init(lt9_tensor t, double a, int mode,
                              int nonlinearity) {
    if (!t) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    torch::nn::init::NonlinearityType nl = torch::kLeakyReLU;
    switch (nonlinearity) {
        case 1:  nl = torch::kReLU; break;
        case 2:  nl = torch::kTanh; break;
        case 3:  nl = torch::kSigmoid; break;
        case 4:  nl = torch::kLinear; break;
        default: nl = torch::kLeakyReLU; break;
    }
    {
        torch::NoGradGuard no_grad;
        if (mode == 1)
            torch::nn::init::kaiming_normal_(*tp, a, torch::kFanOut, nl);
        else
            torch::nn::init::kaiming_normal_(*tp, a, torch::kFanIn, nl);
    }
}

void lt9_xavier_normal_init(lt9_tensor t, double gain) {
    if (!t) return;
    auto *tp = static_cast<torch::Tensor*>(t);
    {
        torch::NoGradGuard no_grad;
        torch::nn::init::xavier_normal_(*tp, gain);
    }
}

/* ---- Gradient clipping ---- */

double lt9_clip_grad_norm(lt9_param_group pg, double max_norm) {
    if (!pg) return 0.0;
    auto *params = static_cast<std::vector<torch::Tensor>*>(pg);
    return torch::nn::utils::clip_grad_norm_(*params, max_norm);
}

/* ---- Extra activations ---- */

lt9_tensor lt9_gelu(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::gelu(*tp));
}

lt9_tensor lt9_silu(lt9_tensor t) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::silu(*tp));
}

lt9_tensor lt9_elu(lt9_tensor t, double alpha) {
    if (!t) return nullptr;
    auto *tp = static_cast<torch::Tensor*>(t);
    return new torch::Tensor(torch::elu(*tp, alpha));
}

/* ---- Extra loss functions ---- */

lt9_tensor lt9_nll_loss(lt9_tensor input, lt9_tensor target, int reduction) {
    if (!input || !target) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    auto *tgt = static_cast<torch::Tensor*>(target);
    auto tgt64 = tgt->to(torch::kInt64);
    return new torch::Tensor(torch::nll_loss(
        *in, tgt64, {}, int_to_reduction(reduction)));
}

lt9_tensor lt9_bce_with_logits_loss(lt9_tensor input, lt9_tensor target,
                                     int reduction) {
    if (!input || !target) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    auto *tgt = static_cast<torch::Tensor*>(target);
    return new torch::Tensor(torch::binary_cross_entropy_with_logits(
        *in, *tgt, {}, {}, int_to_reduction(reduction)));
}

lt9_tensor lt9_l1_loss(lt9_tensor input, lt9_tensor target, int reduction) {
    if (!input || !target) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    auto *tgt = static_cast<torch::Tensor*>(target);
    return new torch::Tensor(torch::l1_loss(*in, *tgt, int_to_reduction(reduction)));
}

lt9_tensor lt9_smooth_l1_loss(lt9_tensor input, lt9_tensor target,
                               int reduction, double beta) {
    if (!input || !target) return nullptr;
    auto *in = static_cast<torch::Tensor*>(input);
    auto *tgt = static_cast<torch::Tensor*>(target);
    return new torch::Tensor(torch::smooth_l1_loss(
        *in, *tgt, int_to_reduction(reduction), beta));
}
