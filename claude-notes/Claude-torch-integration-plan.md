# Bringing Torch Back to Lush: Integration Plan

## A Historical Return to Roots

Lush (1987 as SN → 2001+) birthed the idx/srg tensor abstraction that directly
became Torch7's THTensor/THStorage (2009), which became PyTorch's ATen (2016+).
Yann LeCun has stated: "Torch7 was very much inspired by Lush (transcribed from
Lisp to Lua)." Integrating modern Torch back into Lush would complete a
remarkable circle — and would in principle allow Claude (which runs on PyTorch
infrastructure) to execute inside the Lisp system whose ideas made it possible.

---

## Part 1: Architecture Comparison — Lush idx vs Torch Tensors

### The Two-Struct Pattern Is Already Shared

| Concept | Lush | Torch7 TH | PyTorch ATen |
|---------|------|-----------|-------------|
| Data container | `struct srg` | `THStorage` | `c10::StorageImpl` |
| View/descriptor | `struct idx` | `THTensor` | `c10::TensorImpl` |
| Dimension sizes | `dim[]` | `size[]` | `sizes()` |
| Strides | `mod[]` | `stride[]` | `strides()` |
| Data offset | `offset` | `storageOffset` | `storage_offset()` |
| Max dimensions | 8 (MAXDIMS) | Dynamic | Dynamic |
| Element type | `srg.type` (runtime) | Compile-time `real` | `ScalarType` enum |
| Ref counting | In srg | In both | In both |
| Multiple views | Yes (idx → srg) | Yes (THTensor → THStorage) | Yes |
| Allocator | Fixed (malloc/mmap) | Pluggable vtable | Pluggable |

### Type Mapping (Lush ST_* → torch ScalarType)

| Lush Storage Type | C Type | torch ScalarType | Size |
|---|---|---|---|
| `ST_U8` | `unsigned char` | `kUInt8` | 1 |
| `ST_I8` | `signed char` | `kInt8` | 1 |
| `ST_I16` | `short` | `kInt16` | 2 |
| `ST_I32` | `int` | `kInt32` | 4 |
| `ST_I64` | `int64_t` | `kInt64` | 8 |
| `ST_F` | `float` | `kFloat32` | 4 |
| `ST_D` | `double` | `kFloat64` | 8 |

The type mapping is direct. Every Lush storage type has a PyTorch equivalent.
Lush's ST_AT (Lush objects) and ST_GPTR (void pointers) have no torch
equivalents, but those aren't used in numerical computation.

### The Critical Bridge Function: `torch::from_blob`

PyTorch's `torch::from_blob()` wraps an existing C memory buffer as a tensor
**without copying data**, and it supports strides:

```cpp
float *ptr = (float*)idx->srg->data + idx->offset;
int64_t sizes[MAXDIMS], strides[MAXDIMS];
for (int i = 0; i < idx->ndim; i++) {
    sizes[i]   = (int64_t)idx->dim[i];
    strides[i] = (int64_t)idx->mod[i];
}
torch::Tensor t = torch::from_blob(ptr,
    {sizes, sizes + idx->ndim},
    {strides, strides + idx->ndim},
    torch::TensorOptions().dtype(torch::kFloat32));
```

This is the zero-copy bridge: a Lush idx's data becomes a torch Tensor with
no allocation or memcpy. The reverse direction (`tensor.data_ptr<float>()`)
gives back a raw pointer that can populate a Lush idx.

---

## Part 2: Integration Strategies (Three Options)

### Strategy A: libtorch with extern "C" Bridge (Full Featured)

**What:** Build a shared library (`liblush_torch.so`) in C++ that wraps
libtorch behind an `extern "C"` API. Lush loads it via `dlopen`.

**Capabilities:** Training, inference, model loading, GPU tensors, autograd,
all 2000+ torch operations.

**Binary size:** ~267 MB (CPU), ~1.2 GB (CUDA). Large but self-contained.

**Bridge layer architecture:**

```c
/* lush_torch.h -- C API loaded by Lush via dlopen */

typedef void* ltorch_tensor;
typedef void* ltorch_model;

/* Lifecycle */
int    ltorch_init(void);                    /* returns 1 if CUDA available */
void   ltorch_cleanup(void);

/* Tensor creation from Lush idx data (zero-copy) */
ltorch_tensor ltorch_from_idx(void *data, int64_t *sizes, int64_t *strides,
                              int ndim, int dtype);

/* Move to/from GPU */
ltorch_tensor ltorch_to_cuda(ltorch_tensor t, int device);
ltorch_tensor ltorch_to_cpu(ltorch_tensor t);

/* Get data pointer back (for copying to Lush idx) */
void*  ltorch_data_ptr(ltorch_tensor t);
int    ltorch_ndim(ltorch_tensor t);
void   ltorch_sizes(ltorch_tensor t, int64_t *out);
void   ltorch_strides(ltorch_tensor t, int64_t *out);
int    ltorch_dtype(ltorch_tensor t);

/* Core operations */
ltorch_tensor ltorch_matmul(ltorch_tensor a, ltorch_tensor b);
ltorch_tensor ltorch_add(ltorch_tensor a, ltorch_tensor b);
ltorch_tensor ltorch_relu(ltorch_tensor t);
ltorch_tensor ltorch_softmax(ltorch_tensor t, int dim);
ltorch_tensor ltorch_conv2d(ltorch_tensor input, ltorch_tensor weight,
                            ltorch_tensor bias, int stride, int padding);
ltorch_tensor ltorch_batchnorm(ltorch_tensor input, ltorch_tensor mean,
                               ltorch_tensor var, ltorch_tensor weight,
                               ltorch_tensor bias, double eps);
/* ... ~50-100 wrapped operations for neural net inference */

/* Model loading (TorchScript) */
ltorch_model  ltorch_load_model(const char *path);
ltorch_tensor ltorch_forward(ltorch_model m, ltorch_tensor input);
ltorch_tensor ltorch_forward_multi(ltorch_model m, ltorch_tensor *inputs, int n);
void          ltorch_model_to_cuda(ltorch_model m, int device);

/* Memory management */
void   ltorch_free_tensor(ltorch_tensor t);
void   ltorch_free_model(ltorch_model m);
```

The existing project [c-libtorch](https://github.com/lighttransport/c-libtorch)
follows exactly this pattern. We could fork or reference it.

**Pros:**
- Full PyTorch capability (training + inference)
- Can define and train new models from Lush
- Access to all 2000+ torch ops including autograd
- Direct idx ↔ tensor bridge via `from_blob`

**Cons:**
- Requires C++ compiler and ~267 MB+ binary dependency
- The C wrapper needs to cover each operation explicitly
- TorchScript model loading is deprecated (replaced by torch.export + AOTInductor)
- Significant maintenance burden as PyTorch API evolves

### Strategy B: ONNX Runtime with C API (Inference Only, Recommended First)

**What:** Link against ONNX Runtime's **native C API** (`onnxruntime_c_api.h`)
which requires no C++ wrapper at all.

**Capabilities:** Inference only. Load and run any model exported to `.onnx`
format from PyTorch, TensorFlow, JAX, etc.

**Binary size:** ~7.5 MB default, down to ~1 MB with custom operator pruning.

**Bridge layer architecture:**

```c
/* No C++ wrapper needed -- ONNX Runtime has a pure C API */
#include <onnxruntime_c_api.h>

/* Lush DX functions (registered directly) */
DX(xonnx_load_model)     /* (onnx-load "model.onnx") -> model handle */
DX(xonnx_run)            /* (onnx-run model input-idx) -> output idx */
DX(xonnx_run_multi)      /* multi-input/output variant */
DX(xonnx_free)           /* release model */
DX(xonnx_set_device)     /* "cpu" or "cuda:0" */
```

Key ONNX RT function for zero-copy tensor creation from Lush idx:
```c
OrtStatus *CreateTensorWithDataAsOrtValue(
    OrtMemoryInfo *info,
    void *p_data,          /* idx->srg->data + offset */
    size_t data_len,
    int64_t *shape,        /* idx->dim[] */
    size_t shape_len,      /* idx->ndim */
    ONNXTensorElementDataType type,
    OrtValue **out);
```

**Important limitation:** ONNX RT requires contiguous data. Lush idx can
have non-unit strides. A contiguity check (or copy to contiguous buffer)
is needed:

```c
/* Check if idx is contiguous */
int idx_is_contiguous(struct idx *x) {
    intg expected = 1;
    for (int i = x->ndim - 1; i >= 0; i--) {
        if (x->mod[i] != expected) return 0;
        expected *= x->dim[i];
    }
    return 1;
}
```

**Pros:**
- Pure C API — no C++ wrapper needed, `dlopen` directly from Lush
- Tiny binary (~7.5 MB vs ~267 MB)
- Universal model format (PyTorch, TF, JAX all export to ONNX)
- Multi-backend: CUDA, TensorRT, OpenVINO, CoreML execution providers
- Custom builds can strip operators you don't need
- Stable C ABI — less maintenance than chasing libtorch API changes

**Cons:**
- Inference only — no training
- No autograd, no defining new layers in Lush
- ONNX format doesn't support all PyTorch ops (most are covered)
- Requires contiguous data (minor — just copy if non-contiguous)

### Strategy C: Dual Integration (Recommended Long-Term)

Start with Strategy B (ONNX Runtime) for immediate inference capability,
then add Strategy A (libtorch) later for training. They coexist — ONNX
for deployment, libtorch for experimentation.

---

## Part 3: Lush-Level API Design

### The `torch` Package

```lisp
;; Loading the package
(libload "torch/torch")

;; ===== Creating tensors =====
;; From existing Lush idx (zero-copy):
(setq m (float-matrix 3 4))
(setq t (torch-from-idx m))            ;; wraps idx data, no copy

;; To Lush idx from torch tensor:
(setq m2 (torch-to-idx t))             ;; creates new idx viewing tensor data

;; ===== GPU operations =====
(setq g (torch-cuda t))                ;; copy to GPU
(setq result (torch-matmul g g2))      ;; GPU matrix multiply
(setq back (torch-cpu result))         ;; copy result back to CPU
(setq m3 (torch-to-idx back))          ;; back to Lush idx

;; ===== Loading and running pre-trained models =====
(setq model (torch-load-model "resnet18.onnx"))
(setq input (float-matrix 1 3 224 224))
;; ... fill input with image data ...
(setq output (torch-forward model input))
;; output is a Lush idx with class probabilities

;; ===== Neural net building blocks (libtorch path) =====
(setq w (torch-randn 784 256))         ;; random weight matrix
(setq x (torch-from-idx input-data))
(setq h (torch-relu (torch-matmul x w)))
(setq out (torch-softmax (torch-matmul h w2) 1))

;; ===== Integration with gblearn2 (existing Lush NN framework) =====
;; gblearn2 modules could delegate fprop to torch operations:
(defmethod TorchLinear fprop (input output)
  (let ((t-in  (torch-from-idx (idx-storage input)))
        (t-w   (torch-from-idx (idx-storage this.weight))))
    (let ((t-out (torch-matmul t-in t-w)))
      (torch-copy-to-idx t-out output))))
```

### Integration with Existing idx Operations

The design principle: **torch operations are available alongside native
Lush idx operations, not replacing them.** You use torch when you need:
- GPU acceleration
- Pre-trained model inference
- Operations not in Lush (advanced NN ops like attention, group norm)

For CPU matrix multiply on small matrices, Lush's existing BLAS integration
may be faster (no overhead of crossing the bridge).

---

## Part 4: Implementation Stages

### Stage 1: ONNX Runtime Inference Package (Pure C, Small)

**Scope:** New Lush package `packages/onnxrt/` providing model loading and
inference via ONNX Runtime's C API.

**Files to create:**
```
packages/onnxrt/
  onnxrt.lsh           ;; Lush-level API
  onnxrt-config.lsh    ;; find libonnxruntime.so
  onnxrt.c             ;; DX functions wrapping ORT C API
```

**C functions to implement:**
- `xonnx_init()` — initialize ORT environment
- `xonnx_load(path)` — create session from .onnx file
- `xonnx_run(session, input_idx)` — run inference, return output idx
- `xonnx_run_multi(session, input_list)` — multi-input variant
- `xonnx_set_provider(session, "cuda")` — select execution provider
- `xonnx_free(session)` — cleanup
- `xonnx_model_inputs(session)` — return input names/shapes
- `xonnx_model_outputs(session)` — return output names/shapes

**Key implementation detail:** The idx→OrtValue bridge:
```c
DX(xonnx_run) {
    ARG_NUMBER(2);
    ARG_EVAL(1); ARG_EVAL(2);
    /* Get session handle */
    OrtSession *session = (OrtSession*)AGPTR(1);
    /* Get idx from second arg */
    struct index *ind = AINDEX(2);
    struct idx idx;
    index_read_idx(ind, &idx);

    /* Ensure contiguous (copy if needed) */
    /* ... */

    /* Create OrtValue from idx data */
    int64_t shape[MAXDIMS];
    for (int i = 0; i < idx.ndim; i++)
        shape[i] = (int64_t)idx.dim[i];

    OrtValue *input_tensor;
    ort->CreateTensorWithDataAsOrtValue(
        mem_info, IDX_DATA_PTR(&idx),
        idx_nelems(&idx) * storage_type_size[idx.srg->type],
        shape, idx.ndim, onnx_type_from_st(idx.srg->type),
        &input_tensor);

    /* Run inference */
    OrtValue *output = NULL;
    ort->Run(session, NULL,
        input_names, &input_tensor, 1,
        output_names, 1, &output);

    /* Wrap output in new Lush idx */
    float *out_data;
    ort->GetTensorMutableData(output, (void**)&out_data);
    /* ... create idx backed by this data ... */
}
```

**Estimated scope:** ~500 lines of C, ~200 lines of Lush. One developer,
not a huge project.

**Deliverable:** Load any .onnx model and run it from the Lush prompt:
```lisp
(setq model (onnx-load "resnet18.onnx"))
(setq img (float-matrix 1 3 224 224))
(setq probs (onnx-run model img))
(printf "Top class: %d\n" (idx-i1max probs))
```

### Stage 2: libtorch Bridge Library (C++, Large)

**Scope:** Shared library `liblush_torch.so` wrapping libtorch operations
behind `extern "C"` functions, loaded via Lush's dlopen.

**Files:**
```
packages/torch/
  torch.lsh             ;; Lush-level API
  torch-config.lsh      ;; find libtorch
  lush_torch_bridge.cpp ;; extern "C" wrappers
  CMakeLists.txt        ;; build with cmake
```

**Operations to wrap (priority order):**

Tier 1 (inference essentials):
- `from_blob` / `data_ptr` (the idx bridge)
- `to(device)` (CPU↔CUDA transfer)
- `matmul`, `mm`, `bmm` (matrix multiply)
- `add`, `sub`, `mul`, `div` (element-wise)
- `relu`, `sigmoid`, `tanh`, `gelu` (activations)
- `softmax`, `log_softmax`
- `conv2d`, `max_pool2d`, `avg_pool2d`
- `batch_norm`, `layer_norm`
- `linear` (fully connected)
- `cat`, `stack`, `reshape`, `transpose`, `permute`

Tier 2 (transformer / modern architectures):
- `scaled_dot_product_attention`
- `embedding`, `dropout`
- `cross_entropy_loss`
- Multi-head attention helpers

Tier 3 (training):
- `requires_grad_`, `backward()`
- Optimizer creation (Adam, SGD)
- `save` / `load` state dict

**TorchScript model loading:**
```cpp
extern "C" ltorch_model ltorch_load_model(const char *path) {
    auto *module = new torch::jit::script::Module(
        torch::jit::load(path));
    module->eval();
    return (ltorch_model)module;
}
```

Note: TorchScript is deprecated in PyTorch 2.10+ but still works. The
newer path is `torch.export` + AOTInductor which compiles models to
standalone `.so` files. For Lush, TorchScript is simpler (single function
call to load), and AOTInductor `.so` files can be `dlopen`'d directly
as an alternative.

**Estimated scope:** ~2000 lines of C++, ~500 lines of Lush. Significant
project, depends on Stage 1 experience.

### Stage 3: gblearn2 Integration

**Scope:** Extend Lush's existing gblearn2 neural network framework to
optionally delegate computations to torch.

gblearn2 already defines:
- `gb-module` base class with `fprop`, `bprop`, `bbprop` methods
- `idx3-module`, `idx3-squasher`, `idx3-ddparam`
- LeNet-5, feedforward, and other architectures

**Approach:** Add torch-accelerated variants of key modules:
```lisp
(defclass TorchLinear gb-module
  ;; torch tensor handles for weight and bias
  ((-gptr-) t-weight)
  ((-gptr-) t-bias))

(defmethod TorchLinear fprop (input output)
  ;; Convert input idx to torch tensor
  ;; Run torch::linear (GPU-accelerated matmul + bias)
  ;; Write result back to output idx
  ...)
```

This allows mixing Lush-native and torch-accelerated layers in the same
network definition.

### Stage 4: Pre-trained Model Zoo

**Scope:** Lush scripts for loading and running popular pre-trained models.

- Image classification (ResNet, EfficientNet, ViT)
- Object detection (YOLO)
- Text embeddings (BERT sentence transformers)
- Generative (Stable Diffusion, GPT-2)

Each model gets a Lush package with preprocessing, postprocessing, and
example usage.

---

## Part 5: Tradeoffs and Alternatives

### Build System Complexity

libtorch requires C++17 and ~267 MB of shared libraries. The build story
is non-trivial:
- CMake is effectively required (libtorch ships CMake config files)
- The C++ ABI must match (pre-cxx11 vs cxx11)
- CUDA version must match if using GPU
- The bridge library needs its own build step separate from Lush's
  autotools/make

**Mitigation:** The bridge library builds independently. Lush loads it at
runtime via `dlopen`. If it's not present, torch features are simply
unavailable — the rest of Lush works fine. This is the same pattern Lush
uses for optional packages like OpenCV, OpenGL, etc.

### The TorchScript Deprecation Problem

PyTorch is moving away from TorchScript toward `torch.export` +
AOTInductor. The new path compiles models into standalone `.so` files
containing GPU kernels as precompiled cubins, loadable from C++ with
no Python dependency.

**For Lush this is actually better:** An AOTInductor `.so` is just a
shared library with a small C++ API. We could `dlopen` it directly:
```c
/* Each exported model becomes a simple dlopen'd function */
void *model_lib = dlopen("resnet18.so", RTLD_NOW);
typedef at::Tensor (*RunFn)(const std::vector<at::Tensor>&);
RunFn run = (RunFn)dlsym(model_lib, "run");
```

The downside is that each model requires a separate compilation step
in Python before Lush can use it. The ONNX path avoids this — you just
export once to `.onnx` and any runtime can load it.

### Pure C Alternatives (No C++ at All)

If avoiding C++ entirely is desirable:

1. **ONNX Runtime C API** — Already covered above. Best option for
   inference.

2. **ggml** (the engine behind llama.cpp) — Pure C tensor library with
   CPU and CUDA backends. Supports quantized inference. Much smaller
   than libtorch. Good for running LLMs specifically.

3. **TinyGrad** — Not C, but extremely minimal Python. Could be
   interesting to port its ~1000-line core to Lush directly.

4. **Direct CUDA/cuBLAS from C** — For maximum control, Lush could
   call cuBLAS directly for matrix operations and write custom CUDA
   kernels for activations. Avoids any framework dependency but
   requires reimplementing everything.

### What About Training?

For training neural nets in Lush:
- **Option 1:** Use libtorch's autograd (Strategy A, Stage 2-3).
  Full-featured but heavyweight.
- **Option 2:** Extend gblearn2 with GPU-accelerated fprop/bprop
  while keeping Lush's own backprop logic. Train on CPU with Lush's
  existing system, accelerate just the matrix multiplies via torch.
- **Option 3:** Train in Python/PyTorch, export to ONNX, run inference
  in Lush. This is the most pragmatic for large models.

For most use cases, Option 3 is sufficient. Training large models
requires massive GPU clusters regardless of the host language.

---

## Part 6: Detailed Comparison — Lush idx vs Torch Tensor Internals

### Memory Layout Comparison

```
Lush idx (from include/header.h):
┌──────────────┐     ┌───────────────┐
│ struct index  │     │ struct storage │
│ ndim=2        │     │ type=ST_F      │
│ dim[0]=3      │     │ size=12        │
│ dim[1]=4      │────→│ data=0xABC...  │──→ [f f f f f f f f f f f f]
│ mod[0]=4      │     │ flags=MALLOC   │
│ mod[1]=1      │     └───────────────┘
│ offset=0      │
└──────────────┘

Torch THTensor (from torch7 generic/THTensor.h):
┌──────────────┐     ┌───────────────┐
│ THTensor      │     │ THStorage     │
│ nDimension=2  │     │ data=0xDEF... │──→ [f f f f f f f f f f f f]
│ size[0]=3     │     │ size=12       │
│ size[1]=4     │────→│ refcount=1    │
│ stride[0]=4   │     │ allocator=... │
│ stride[1]=1   │     └───────────────┘
│ storageOffset │
│ refcount=1    │
└──────────────┘
```

The layouts are structurally identical. The naming convention (mod vs
stride, dim vs size) is the only difference at this level.

### Operations That Map Directly

| Lush idx operation | Torch equivalent | Notes |
|---|---|---|
| `idx-select` | `tensor.select(dim, index)` | Same semantics |
| `idx-narrow` | `tensor.narrow(dim, start, len)` | Same semantics |
| `idx-transpose` | `tensor.transpose(dim0, dim1)` | Same semantics |
| `idx-unfold` | `tensor.unfold(dim, size, step)` | Same semantics |
| `idx-reshape` | `tensor.reshape(sizes)` | Lush: only contiguous |
| `idx-copy` | `tensor.copy_(other)` | In-place copy |
| `idx-dotm2` | `torch.matmul(a, b)` | 2D matrix multiply |
| `idx-m2timesm2` | `torch.mm(a, b)` | Same as above |
| `idx-sum` | `tensor.sum()` | Reduction |
| `idx-sqrdist` | via `(a-b).pow(2).sum()` | No direct equivalent |
| `idx-sortup` | `tensor.sort()` | Returns values+indices |

### Lush's DHC Compiler as an Alternative

Lush's `dhc-make` compiles Lush functions with type annotations to C.
For CPU-only workflows, compiled Lush code + BLAS is competitive with
libtorch for many operations. The torch integration is primarily valuable
for:

1. **GPU acceleration** — Lush has no GPU backend at all
2. **Pre-trained model loading** — Thousands of models in PyTorch format
3. **Cutting-edge ops** — Attention mechanisms, flash attention, etc.
4. **Community ecosystem** — Transformers library, HuggingFace, etc.

---

## Part 7: Show-Stopper Analysis

### Definite Show-Stoppers: None

The integration is technically feasible. The idx/tensor mapping is
structurally trivial. Both ONNX Runtime and libtorch have well-documented
C/C++ APIs with zero-copy tensor creation from raw pointers.

### Significant Challenges

1. **MAXDIMS=8 in Lush:** Some modern models use >8 dimensions (attention
   tensors can be 5-6D, but batch operations could go higher). Torch has
   no dimension limit. This doesn't block integration (torch tensors
   aren't constrained by MAXDIMS), but it limits which results can be
   converted back to Lush idx.

2. **Lush has no half-precision (float16) support.** Modern GPU inference
   often uses float16 or bfloat16. Lush would need to handle these as
   opaque torch tensors (no direct idx mapping) or add ST_F16 support.

3. **Garbage collection boundary.** Lush uses its own GC; torch uses
   reference counting. The bridge must ensure torch tensors created from
   Lush idx data aren't freed while the idx is still alive, and vice
   versa. Solution: reference the Lush storage AT object from the torch
   bridge handle, preventing GC.

4. **Build system mismatch.** Lush uses autotools/make. libtorch wants
   CMake. The bridge library must be built separately. This is awkward
   but workable — same as any Lush package that wraps a C++ library.

5. **libtorch binary size.** ~267 MB for CPU, ~1.2 GB for CUDA. This
   dwarfs the Lush interpreter (~2 MB). Users need to accept this as
   an optional dependency. ONNX Runtime at ~7.5 MB is much more
   reasonable.

6. **No interactive tensor display on GPU.** Lush's idx pretty-printer
   works on CPU memory. GPU tensors would need to be copied to CPU
   for display. This is standard behavior (Python PyTorch does the same).

### Non-Issues

- **Performance of the bridge:** The overhead of one C function call
  per operation is negligible compared to the GPU kernel execution time.
  This is the same pattern as Python→C++ in PyTorch.

- **Thread safety:** Lush is single-threaded. libtorch internally uses
  threads for CPU parallelism and CUDA streams for GPU, but the Lush-
  facing API is single-threaded.

- **Model compatibility:** ONNX is a universal format. Any PyTorch,
  TensorFlow, or JAX model can be exported to ONNX. TorchScript covers
  most PyTorch models directly.

---

## Part 8: Revised Implementation Plan (2026-03-05)

### Key Decisions

- **Package name: `torch9`** — the existing `packages/torch/` wraps Torch3
  (Ronan Collobert, 2002).  The new package is entirely separate.
- **Skip ONNX, go straight to libtorch.** The ~267 MB binary size is
  acceptable in 2026.  libtorch gives us training + inference + autograd +
  GPU, not just inference.  ONNX can be revisited later if needed.
- **R's `torch` package (mlverse/torch) proves this works.** They built a
  C shim layer called "lantern" (`extern "C"` over libtorch) and it ships
  as a production CRAN package.  Our architecture is the same pattern.

### Vendored libtorch via lush-pkg

PyTorch publishes prebuilt libtorch zip archives for every release on
their official CDN at `download.pytorch.org`.  All three GPU backends
are available — no building from source needed:

| Backend | URL Pattern |
|---------|-------------|
| CPU     | `https://download.pytorch.org/libtorch/cpu/libtorch-cxx11-abi-shared-with-deps-{VER}%2Bcpu.zip` |
| CUDA    | `https://download.pytorch.org/libtorch/cu{CUVER}/libtorch-cxx11-abi-shared-with-deps-{VER}%2Bcu{CUVER}.zip` |
| ROCm    | `https://download.pytorch.org/libtorch/rocm{ROCVER}/libtorch-cxx11-abi-shared-with-deps-{VER}%2Brocm{ROCVER}.zip` |

As of PyTorch 2.9.1: CUDA 12.6/12.8/13.0, ROCm 6.4, CPU.
Linux has two ABI variants (pre-CXX11 and CXX11); we use CXX11.
macOS is CPU-only (or MPS via Metal on Apple Silicon).
Windows has Release and Debug variants.

The `lush-pkg` install recipe for torch9:

1. **Detect backend**: check for ROCm (`/opt/rocm/bin/rocminfo`), then
   CUDA (`nvidia-smi`), fall back to CPU.
2. **Fetch**: download the appropriate libtorch zip from
   `download.pytorch.org` (~267 MB CPU, larger for GPU).
3. **Unpack** to `packages/torch9/lib/`.
4. **Build bridge**: compile `lush_torch9_bridge.cpp` against the
   unpacked libtorch headers/libs.  This is the only compile step and
   takes seconds — it's just our small `extern "C"` shim, not libtorch
   itself.
5. **Done**: user does `(libload "torch9/torch9")` and has GPU tensors.

Note: ROCm/CUDA libtorch zips link dynamically against the GPU runtime.
The user must have ROCm or CUDA drivers installed on their system.  The
libtorch zip does NOT bundle the GPU driver/runtime.

### Implementation Stages

```
Stage 1: torch9 package skeleton + bridge + matmul demo  ✅ IMPLEMENTED 2026-03-05
  - packages/torch9/torch9-config.lsh (detect backend, download libtorch, compile bridge)
  - lush_torch9_bridge.h/.cpp (extern "C" API: from_blob, matmul, add, clone, free, print)
  - torch9.lsh (DHC wrappers + interpreted API: torch9-from-idx, torch9-matmul, torch9-to-idx)
  - tests/test-torch9.lsh (59 tests: roundtrip, matmul, add, clone, dtype preservation)
  - Zero-copy idx→tensor via from_blob, tensor→idx via data_ptr copy
  - Supports double/float/int, rank 1-2
  ↓
Stage 2: More operations + GPU + higher ranks  ✅ IMPLEMENTED 2026-03-05
  - Element-wise ops: sub, mul, div
  - Activations: relu, sigmoid, tanh, softmax, log-softmax
  - Higher-rank tensors: rank 3-4 for double/float/int (from-idx + to-idx)
  - Shape manipulation: reshape, transpose, permute, squeeze, unsqueeze, cat
  - GPU transfer: to-cuda, to-cpu, device-type (auto CPU copy in to-idx)
  - 33 C bridge functions, 50 DHC wrappers, ~31 high-level API functions
  - tests/test-torch9.lsh (228 tests, all passing)
  ↓
  Milestone: GPU matrix multiply from Lush REPL (ready when CUDA hardware available)
  ↓
Stage 3: Model loading + NN functional ops  ✅ IMPLEMENTED 2026-03-05
  - TorchScript model loading: load .pt, forward, eval, to-cuda, free
  - Test model creation in C++ (no Python needed for testing)
  - IValue navigation: is-tensor/tuple/list, to-tensor, tuple-get, unpack
  - NN functional ops: conv2d, batch_norm, layer_norm, linear,
    max_pool2d, avg_pool2d, embedding, dropout
  - LSTM convenience: torch9-lstm-forward via TorchScript + IValue unpack
  - Python export script: scripts/export_lstm.py
  - ~55 C bridge functions, ~72 DHC wrappers, ~49 high-level API functions
  - tests/test-torch9.lsh (~358 tests)
  ↓
  Milestone: Load and run TorchScript models from Lush REPL
  ↓
Stage 4: Training support ✅ COMPLETE (2026-03-05)
  - Data-owning tensor creation: randn, zeros, ones, full
  - Reductions: sum, mean, item; neg
  - Autograd: requires_grad, requires_grad!, backward, grad, detach, grad_enabled
  - Loss functions: mse_loss, cross_entropy_loss
  - Optimizers: SGD, Adam (with lr get/set, step, zero_grad)
  - Tensor save/load
  - ~85 C bridge functions, ~103 DHC wrappers, ~75 high-level API functions
  - tests/test-torch9.lsh (488 tests, all passing)
  ↓
  Milestone: Train models from Lush REPL (SGD/Adam converge on regression + classification)
  ↓
Stage 5: gblearn2 integration + model zoo
  - Optional torch-accelerated gblearn2 modules
  - Pre-trained model scripts (ResNet, ViT, BERT, etc.)
```

### Reference: R torch package architecture

R's `torch` package (https://github.com/mlverse/torch) uses this stack:

```
R  →  R C FFI  →  lantern.so (extern "C")  →  libtorch (C++)
```

Our equivalent:

```
Lush  →  dlopen  →  lush_torch9_bridge.so (extern "C")  →  libtorch (C++)
```

R's `lantern` source is open (MIT license) and can be studied for the
C bridge patterns.  They fetch libtorch from the same PyTorch CDN URLs.
Their custom `lantern` bridge binary is hosted on their own CDN
(`torch-cdn.mlverse.org`), but we compile ours at install time via
lush-pkg, so we don't need to host prebuilt bridges.

---

## Part 9: Open Questions

1. **Should we support ggml as a third backend?** ggml is pure C and
   optimized for LLM inference (quantized models). It would let Lush
   run LLMs without the libtorch dependency. But it's a separate
   ecosystem from ONNX.

2. **Should Lush's MAXDIMS be increased?** The current limit of 8 is
   adequate for most neural network tensors, but it means some torch
   operations might produce results that can't be represented as Lush
   idx. We could increase MAXDIMS or leave it and handle >8D tensors
   as opaque torch handles.

3. **Should we add float16 (ST_F16) to Lush's storage types?** This
   would enable half-precision idx objects and direct mapping to torch
   float16 tensors. Useful for GPU inference but Lush has no float16
   arithmetic otherwise.

4. **Is the gblearn2 framework worth extending, or should training be
   done entirely via libtorch?** gblearn2 has unique features (bbprop
   for second-order methods) but is architecturally dated. A hybrid
   approach (Lush control flow + torch computation) might be best.

5. **How should we handle the Python model export step?** Users need
   Python + PyTorch to export models to ONNX or TorchScript. We could
   provide a helper script, document the process, or even embed a
   minimal Python subprocess for conversion.
