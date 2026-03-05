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

/* ---- Lifecycle ---- */

lt9_tensor  lt9_clone(lt9_tensor t);
void        lt9_free(lt9_tensor t);

/* ---- Debug ---- */

void        lt9_print(lt9_tensor t);

#ifdef __cplusplus
}
#endif

#endif /* LUSH_TORCH9_BRIDGE_H */
