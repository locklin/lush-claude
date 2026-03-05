/* lush_torch9_bridge.cpp -- C++ implementation of the Lush-libtorch bridge
 *
 * Each lt9_tensor handle is a heap-allocated torch::Tensor*.
 * This file is compiled into lush_torch9_bridge.so and loaded
 * via mod-load from torch9-config.lsh.
 */

#include <torch/torch.h>
#include <iostream>
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
