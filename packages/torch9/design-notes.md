# torch9 Design Notes

## Architecture

### Historical Context

Lush (1987 as SN, 2001+) created the idx/srg tensor abstraction that directly
became Torch7's THTensor/THStorage (2009), which became PyTorch's ATen (2016+).
Integrating libtorch back into Lush completes that circle.

### Stack

```
Lush  ->  DHC wrappers  ->  mod-load  ->  lush_torch9_bridge.so (extern "C")  ->  libtorch (C++)
```

This mirrors R's `torch` package (mlverse/torch), which uses a similar "lantern"
C shim layer over libtorch and ships as a production CRAN package.

### Why libtorch (not ONNX Runtime)

ONNX Runtime was considered first (pure C API, ~7.5 MB, inference only). We went
straight to libtorch because:

- Training + inference + autograd + GPU in one package
- The ~267 MB binary size is acceptable in 2026
- ONNX can be revisited later if an inference-only path is needed

### Why `torch9` (not `torch`)

The existing `packages/torch/` wraps Torch3 (Ronan Collobert, 2002). The new
package is entirely separate and uses libtorch 2.7.0 from download.pytorch.org.

### Key Design Decisions

- **Opaque handles**: Tensors, models, IValues, optimizers, and param groups are
  passed as `void*` (opaque `gptr` in Lush). This avoids exposing C++ types across
  the bridge.
- **Zero-copy idx->tensor**: Uses `torch::from_blob()` to wrap Lush idx data as a
  torch Tensor without allocation or memcpy. Strides are preserved.
- **Copy on tensor->idx**: `torch9-to-idx` copies data out of the tensor into a
  new Lush idx (via `data_ptr` + memcpy). This is necessary because GPU tensors
  cannot be directly accessed, and it avoids lifetime issues.
- **Type dispatch**: Uses `(is-of-class (idx-storage m) |DSTORAGE|)` etc. to
  determine element type (not `idx-element-type`).
- **DHC naming**: `!` is not allowed in DHC function names; in-place ops use
  `_torch9-add-inplace` internally, exposed as `torch9-add!` at the Lush level.
- **libtorch downloaded, not built**: `torch9-config.lsh` detects the GPU backend
  (ROCm, CUDA, or CPU), downloads the matching libtorch zip from
  `download.pytorch.org`, unpacks to `packages/torch9/lib/`, and compiles only the
  small bridge .cpp file against it.

### The idx <-> Tensor Bridge

Lush's `struct idx` (dim/mod/offset + storage) maps directly to Torch's
THTensor (size/stride/storageOffset + THStorage). The naming differs
(mod vs stride, dim vs size) but the structure is identical.

```
Lush idx:                          Torch Tensor:
  ndim, dim[], mod[], offset         nDimension, size[], stride[], storageOffset
  -> struct srg (data, type, size)   -> THStorage (data, size, refcount)
```

Type mapping: ST_D->kFloat64, ST_F->kFloat32, ST_I32->kInt32. Lush's ST_AT
and ST_GPTR have no torch equivalents (not needed for numerical computation).

### File Layout

```
packages/torch9/
  torch9-config.lsh          -- backend detection, libtorch download, bridge compile
  torch9.lsh                 -- ~149 DHC wrappers + ~121 high-level API functions
  lush_torch9_bridge.h       -- extern "C" API declarations (~131 functions)
  lush_torch9_bridge.cpp     -- C++ implementations wrapping libtorch
  lush_torch9_bridge.so      -- compiled bridge (linked against libtorch)
  lib/                       -- unpacked libtorch (downloaded at install time)
  tests/test-torch9.lsh      -- 665 tests
  examples/                  -- mnist-train.lsh, resnet-inference.lsh,
                                export-mnist-mlp.lsh, build-resnet18.lsh
  scripts/                   -- export_lstm.py, prepare_mnist.sh
  C/                         -- build artifacts
```

---

## API Summary

### Stage 1 -- Core Bridge (done)

- Creation/conversion: `torch9-from-idx`, `torch9-to-idx`, `torch9-clone`, `torch9-free`
- Arithmetic: `torch9-matmul`, `torch9-add`
- Supports double/float/int, rank 1-2
- Zero-copy idx->tensor via `from_blob`

### Stage 2 -- Operations + GPU + Higher Ranks (done)

- Element-wise: `torch9-sub`, `torch9-mul`, `torch9-div`, `torch9-neg`
- Activations: `torch9-relu`, `torch9-sigmoid`, `torch9-tanh`, `torch9-softmax`, `torch9-log-softmax`
- Shape manipulation: `torch9-reshape`, `torch9-transpose`, `torch9-permute`, `torch9-squeeze`, `torch9-unsqueeze`, `torch9-cat`
- GPU transfer: `torch9-to-cuda`, `torch9-to-cpu`, `torch9-device-type`
- Introspection: `torch9-ndim`, `torch9-dtype`, `torch9-numel`, `torch9-sizes`, `torch9-strides`
- Higher-rank support: rank 3-4 for double/float/int

### Stage 3 -- Model Loading + NN Functional (done)

- TorchScript models: `torch9-model-load`, `torch9-model-forward`, `torch9-model-eval`, `torch9-model-to-cuda`, `torch9-model-free`
- IValue navigation: `torch9-ivalue-to-tensor`, `torch9-ivalue-tuple-ref`, `torch9-ivalue-unpack`, `torch9-ivalue-free`
- NN functional: `torch9-conv2d`, `torch9-batch-norm`, `torch9-layer-norm`, `torch9-linear`, `torch9-max-pool2d`, `torch9-avg-pool2d`, `torch9-embedding`, `torch9-dropout`
- IValue-based forward: `torch9-model-forward-ivalue`
- LSTM convenience: `torch9-lstm-forward` (TorchScript model + IValue unpack)

### Stage 4 -- Training Support (done)

- Data-owning tensors: `torch9-randn`, `torch9-zeros`, `torch9-ones`, `torch9-full`
- Reductions: `torch9-sum`, `torch9-mean`, `torch9-item`
- Autograd: `torch9-requires-grad`, `torch9-requires-grad!`, `torch9-backward`, `torch9-grad`, `torch9-detach`, `torch9-grad-enabled`, `torch9-set-grad-enabled`
- Loss functions: `torch9-mse-loss`, `torch9-cross-entropy-loss`
- Optimizers: `torch9-sgd`, `torch9-adam`, `torch9-optimizer-step`, `torch9-optimizer-zero-grad`, `torch9-optimizer-get-lr`, `torch9-optimizer-set-lr`, `torch9-optimizer-free`
- Tensor persistence: `torch9-tensor-save`, `torch9-tensor-load`

### Stage 5 -- Training Toolkit (done)

- Math: `torch9-exp`, `torch9-log`, `torch9-sqrt`, `torch9-abs`, `torch9-pow`, `torch9-clamp`
- Scalar ops: `torch9-add-scalar`, `torch9-mul-scalar`
- Type casting: `torch9-to-dtype`; Index generation: `torch9-arange`
- Comparisons: `torch9-eq`, `torch9-gt` (return uint8 tensors)
- Dim reductions: `torch9-argmax`, `torch9-sum-dim`, `torch9-mean-dim`, `torch9-max-dim` (with keepdim)
- Indexing: `torch9-narrow`, `torch9-index-select`, `torch9-select`, `torch9-flatten`
- In-place: `torch9-add!`, `torch9-mul!`, `torch9-sub!`, `torch9-zero!`, `torch9-fill!`, `torch9-copy!`
- Param init: `torch9-normal!`, `torch9-uniform!`, `torch9-kaiming-normal!`, `torch9-xavier-normal!`
- Grad clipping: `torch9-clip-grad-norm`
- Extra activations: `torch9-gelu`, `torch9-silu`, `torch9-elu`
- Extra losses: `torch9-nll-loss`, `torch9-bce-with-logits-loss`, `torch9-l1-loss`, `torch9-smooth-l1-loss`

### Stage 6 -- Module Building (done)

- Module creation: `torch9-module-create`, `torch9-module-define`
- Registration: `torch9-module-register-parameter`, `torch9-module-register-buffer`, `torch9-module-register-module`
- Persistence: `torch9-model-save`, `torch9-model-get-parameter`, `torch9-model-num-parameters`

---

## Implementation Details

### libtorch Download and Build

`torch9-config.lsh` handles the full bootstrap:

1. Detect GPU backend: check for ROCm (`/opt/rocm/bin/rocminfo`), then CUDA
   (`nvidia-smi`), fall back to CPU.
2. Download the matching libtorch zip from `download.pytorch.org` (CXX11 ABI
   variant on Linux).
3. Unpack to `packages/torch9/lib/`.
4. Compile `lush_torch9_bridge.cpp` with g++ against the unpacked headers/libs.
   This is the only compile step and takes seconds.

GPU backend libtorch zips link dynamically against the GPU runtime. The user must
have ROCm or CUDA drivers installed; libtorch does not bundle them.

### C Bridge Patterns

All bridge functions use opaque `void*` handles:

```c
typedef void* lt9_tensor;
typedef void* lt9_model;
typedef void* lt9_ivalue;
typedef void* lt9_optimizer;
typedef void* lt9_param_group;
```

Internally each handle is a `new`-allocated C++ object (e.g., `torch::Tensor*`).
The Lush side holds a `gptr` and must call the corresponding `_free` function to
release it.

### TorchScript define() Quirks

When building models via `torch9-module-define`, the TorchScript source passed to
`define()` has several restrictions vs normal Python:

- No `torch.nn.functional.*` -- use aten ops instead (`torch.addmm`, `torch.conv2d`,
  `torch.relu`, etc.)
- Call `.forward()` explicitly on submodules (not `()`)
- `torch.adaptive_avg_pool2d` and `torch.max_pool2d` are available as aten ops

### cross_entropy_loss Target Handling

The C bridge auto-converts targets to int64 because libtorch's
`cross_entropy_loss` requires integer class indices, but Lush idx may arrive as
int32 or double.

### torch9-full Signature

`(torch9-full <value> . <dims>)` -- value comes FIRST, then dimensions. This
differs from PyTorch's Python API where size comes first.

### kaiming_normal_ C++ API

Uses `torch::kFanIn`/`torch::kFanOut`, `torch::kReLU`/`torch::kLeakyReLU` enum
values (not enum member access syntax).

### clip_grad_norm_

Returns `double` directly (not a Tensor) in libtorch 2.7.0.

### Module::register_parameter

Requires 3 arguments `(name, tensor, is_buffer=false)` in libtorch 2.7.0.

### Lush `let` is Parallel

A recurring pitfall: `(let ((a 1) (b a)))` -- `b` cannot see `a` because `let`
bindings are evaluated in parallel. Use `let*` for sequential binding.

---

## Known Issues / Limitations

### Architectural Constraints

- **MAXDIMS=8 in Lush**: Some modern models produce >8D tensors (attention
  tensors can be 5-6D, batched higher). Torch has no dimension limit. Results
  exceeding 8D cannot be converted back to Lush idx; they remain as opaque
  torch handles.
- **No half-precision**: Lush has no float16/bfloat16 storage type. Modern GPU
  inference often uses these. Such tensors can only exist as opaque torch handles.
- **GC boundary**: Lush uses its own GC; torch uses reference counting. The
  bridge must ensure torch tensors created from Lush idx data are not freed
  while the idx is alive. Currently handled by copying on tensor->idx conversion.

### Not Implemented

- **gblearn2 integration** (Stage 7): Optional torch-accelerated gblearn2
  modules. Not started.
- **ONNX Runtime**: Decided against for now. Could be added later as a
  lightweight inference-only path.
- **ggml backend**: Pure C tensor library for quantized LLM inference. Not
  planned.
- **float16 storage type**: Would enable direct idx mapping to torch float16
  tensors. Not planned.

### Build Requirements

- C++ compiler (g++ with C++17 support)
- ~267 MB disk for CPU libtorch, ~1.2 GB for CUDA variant
- CUDA or ROCm drivers if using GPU backend (libtorch zip does not bundle them)

### TorchScript Deprecation

TorchScript is deprecated in newer PyTorch versions in favor of `torch.export` +
AOTInductor. For now TorchScript still works in libtorch 2.7.0 and is simpler for
our use case (single function call to load). AOTInductor produces standalone .so
files that could be dlopen'd directly as an alternative path in the future.

### Python Dependencies

Python is only needed for:
- Exporting LSTM models (`scripts/export_lstm.py`)
- Obtaining pretrained weights from torchvision (pickle files)

Model creation (MLP, ResNet18) is done entirely in Lush via the Stage 6 module
building API. The Python export scripts for MNIST MLP and ResNet18 were deleted
and replaced by Lush equivalents (`examples/export-mnist-mlp.lsh`,
`examples/build-resnet18.lsh`).
