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
