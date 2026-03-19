#ifndef VMM_PREDICTOR_H
#define VMM_PREDICTOR_H

#include <cstdint>
#include <cstddef>

namespace vmm {

using Symbol = uint16_t;

class VMMPredictor {
public:
    virtual ~VMMPredictor() = default;
    virtual void learn(const Symbol* data, size_t len) = 0;
    virtual double predict(Symbol symbol, const Symbol* context, size_t ctx_len) = 0;
    virtual void predict_distribution(const Symbol* context, size_t ctx_len,
                                      double* out_probs) = 0;
    virtual double log_eval(const Symbol* data, size_t len) = 0;
    virtual double log_eval(const Symbol* data, size_t len,
                            const Symbol* ctx, size_t ctx_len) = 0;
    virtual uint16_t alphabet_size() const = 0;
};

// Factory function. params/n_params carry algorithm-specific parameters:
//   PST: params = {pmin, alpha, gamma, r}, n_params = 4
//   LZms: params = {m, s}, n_params = 2
//   Others: params = NULL, n_params = 0
VMMPredictor* create_predictor(const char* algorithm, uint16_t alphabet_size,
                               int max_order, const double* params, int n_params);

} // namespace vmm

#endif // VMM_PREDICTOR_H
