#include "vmm_predictor.h"
#include "ppmc_predictor.h"
#include "ctw_predictor.h"
#include "bctw_predictor.h"
#include "dctw_predictor.h"
#include "pst_predictor.h"
#include "lzms_predictor.h"
#include "ppmdecay_predictor.h"
#include "cts_predictor.h"
#include "mdi_predictor.h"
#include "seqmemo_predictor.h"
#include <cstring>
#ifdef VMM_R_PACKAGE
#include <R_ext/Print.h>
#define VMM_ERRMSG REprintf
#else
#include <cstdio>
#define VMM_ERRMSG(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
#endif

namespace vmm {

VMMPredictor* create_predictor(const char* algorithm, uint16_t alphabet_size,
                               int max_order, const double* params, int n_params) {
    // PPM-C: params unused
    if (strcmp(algorithm, "ppmc") == 0 || strcmp(algorithm, "PPMC") == 0) {
        return new PPMCPredictor(alphabet_size, max_order);
    }

    // CTW Volf: params unused
    if (strcmp(algorithm, "ctw") == 0 || strcmp(algorithm, "CTW") == 0) {
        return new CTWVolfPredictor(alphabet_size, max_order);
    }

    // Binary CTW: params unused
    if (strcmp(algorithm, "bctw") == 0 || strcmp(algorithm, "BCTW") == 0) {
        return new BinaryCTWPredictor(alphabet_size, max_order);
    }

    // Decomposed CTW: params unused
    if (strcmp(algorithm, "dctw") == 0 || strcmp(algorithm, "DCTW") == 0) {
        return new DCTWPredictor(alphabet_size, max_order);
    }

    // PST: params = [pmin, alpha, gamma, r]
    if (strcmp(algorithm, "pst") == 0 || strcmp(algorithm, "PST") == 0) {
        double pmin = (n_params > 0) ? params[0] : 0.001;
        double alpha = (n_params > 1) ? params[1] : 0.0;
        double gamma_param = (n_params > 2) ? params[2] : 0.006;
        double r = (n_params > 3) ? params[3] : 1.05;
        return new PSTPredictor(alphabet_size, max_order, pmin, alpha, gamma_param, r);
    }

    // LZms: params = [m, s]
    if (strcmp(algorithm, "lzms") == 0 || strcmp(algorithm, "LZms") == 0) {
        int m = (n_params > 0) ? static_cast<int>(params[0]) : 2;
        int s = (n_params > 1) ? static_cast<int>(params[1]) : 2;
        return new LZmsPredictor(alphabet_size, m, s);
    }

    // LZ78: special case of LZms with m=0, s=0
    if (strcmp(algorithm, "lz78") == 0 || strcmp(algorithm, "LZ78") == 0) {
        return new LZmsPredictor(alphabet_size, 0, 0);
    }

    // PPM-Decay: params = [w0, w_inf, half_life]
    if (strcmp(algorithm, "ppmdecay") == 0 || strcmp(algorithm, "PPMDECAY") == 0) {
        double w0 = (n_params > 0) ? params[0] : 1.0;
        double w_inf = (n_params > 1) ? params[1] : 0.001;
        double half_life = (n_params > 2) ? params[2] : 100.0;
        return new PPMDecayPredictor(alphabet_size, max_order, w0, w_inf, half_life);
    }

    // CTS: params = [switch_rate]
    if (strcmp(algorithm, "cts") == 0 || strcmp(algorithm, "CTS") == 0) {
        double switch_rate = (n_params > 0) ? params[0] : 0.5;
        return new CTSPredictor(alphabet_size, max_order, switch_rate);
    }

    // MDI: params = [merge_threshold]
    if (strcmp(algorithm, "mdi") == 0 || strcmp(algorithm, "MDI") == 0) {
        double merge_threshold = (n_params > 0) ? params[0] : 0.1;
        return new MDIPredictor(alphabet_size, max_order, merge_threshold);
    }

    // Sequence Memoizer: params = [discount, concentration]
    if (strcmp(algorithm, "seqmemo") == 0 || strcmp(algorithm, "SEQMEMO") == 0) {
        double discount = (n_params > 0) ? params[0] : 0.75;
        double concentration = (n_params > 1) ? params[1] : 1.0;
        return new SeqMemoPredictor(alphabet_size, max_order, discount, concentration);
    }

    VMM_ERRMSG("create_predictor: unknown algorithm '%s'\n", algorithm);
    return nullptr;
}

} // namespace vmm
