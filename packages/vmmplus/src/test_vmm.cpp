#include "vmm_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>

static int tests_run = 0;
static int tests_passed = 0;

static void check(const char* name, bool cond) {
    tests_run++;
    if (cond) {
        tests_passed++;
    } else {
        printf("  FAIL %s\n", name);
    }
}

static void check_near(const char* name, double actual, double expected, double tol) {
    tests_run++;
    double diff = fabs(actual - expected);
    if (diff <= tol) {
        tests_passed++;
    } else {
        printf("  FAIL %s: expected %.5f, got %.5f (diff=%.6f)\n",
               name, expected, actual, diff);
    }
}

static void check_sum(const char* name, const double* probs, int size, double tol) {
    tests_run++;
    double sum = 0.0;
    for (int i = 0; i < size; i++) sum += probs[i];
    if (fabs(sum - 1.0) <= tol) {
        tests_passed++;
    } else {
        printf("  FAIL %s: distribution sums to %.6f, expected 1.0\n", name, sum);
    }
}

// Convert string to uint16_t array using ASCII values
static void str_to_symbols(const char* s, uint16_t* out, size_t len) {
    for (size_t i = 0; i < len; i++)
        out[i] = static_cast<uint16_t>(s[i]);
}

// ========== Generic algorithm tests ==========

// Test that a predictor can be created and destroyed
static void test_create_destroy(const char* algo, uint16_t ab_size, int order,
                                 const double* params, int n_params) {
    vmm_handle h = vmm_create(algo, ab_size, order, params, n_params);
    check("create not null", h != NULL);
    if (h) {
        check("alphabet_size", vmm_alphabet_size(h) == ab_size);
        vmm_destroy(h);
    }
}

// Test that predict_distribution sums to ~1 after training
static void test_distribution_sums(const char* algo, uint16_t ab_size, int order,
                                    const double* params, int n_params) {
    vmm_handle h = vmm_create(algo, ab_size, order, params, n_params);
    if (!h) { printf("  FAIL: could not create %s\n", algo); tests_run++; return; }

    // Train on repeating pattern
    uint16_t data[20];
    for (int i = 0; i < 20; i++) data[i] = i % ab_size;
    vmm_learn(h, data, 20);

    // Predict with context
    uint16_t ctx[3] = {0, 1, 2};
    int ctx_len = (ab_size > 3) ? 3 : 1;
    double* probs = new double[ab_size];
    vmm_predict_distribution(h, ctx, ctx_len, probs);
    check_sum("dist sums to 1", probs, ab_size, 0.02);

    // All probabilities should be non-negative
    bool all_nonneg = true;
    for (int i = 0; i < ab_size; i++) {
        if (probs[i] < 0.0) { all_nonneg = false; break; }
    }
    check("all probs >= 0", all_nonneg);

    delete[] probs;
    vmm_destroy(h);
}

// Test that log_eval returns a finite non-negative value (in bits)
static void test_log_eval(const char* algo, uint16_t ab_size, int order,
                           const double* params, int n_params) {
    vmm_handle h = vmm_create(algo, ab_size, order, params, n_params);
    if (!h) { printf("  FAIL: could not create %s\n", algo); tests_run++; return; }

    uint16_t data[20];
    for (int i = 0; i < 20; i++) data[i] = i % ab_size;
    vmm_learn(h, data, 20);

    uint16_t test[10];
    for (int i = 0; i < 10; i++) test[i] = i % ab_size;
    double val = vmm_log_eval(h, test, 10);
    check("log_eval is finite", std::isfinite(val));
    check("log_eval >= 0 (bits)", val >= 0.0);

    vmm_destroy(h);
}

// Test that untrained predictor gives uniform distribution
static void test_untrained(const char* algo, uint16_t ab_size, int order,
                            const double* params, int n_params) {
    vmm_handle h = vmm_create(algo, ab_size, order, params, n_params);
    if (!h) { printf("  FAIL: could not create %s\n", algo); tests_run++; return; }

    double* probs = new double[ab_size];
    vmm_predict_distribution(h, NULL, 0, probs);

    double expected = 1.0 / ab_size;
    bool near_uniform = true;
    for (int i = 0; i < ab_size; i++) {
        if (fabs(probs[i] - expected) > 0.1) { near_uniform = false; break; }
    }
    check("untrained is ~uniform", near_uniform);
    check_sum("untrained dist sums", probs, ab_size, 0.01);

    delete[] probs;
    vmm_destroy(h);
}

// Test that training on a biased sequence produces biased predictions
static void test_biased_learning(const char* algo, uint16_t ab_size, int order,
                                  const double* params, int n_params) {
    vmm_handle h = vmm_create(algo, ab_size, order, params, n_params);
    if (!h) { printf("  FAIL: could not create %s\n", algo); tests_run++; return; }

    // Train with symbol 0 being very common
    uint16_t data[40];
    for (int i = 0; i < 40; i++) data[i] = (i % 5 == 0) ? 1 : 0;
    vmm_learn(h, data, 40);

    // With no context, P(0) should be higher than P(1)
    double p0 = vmm_predict(h, 0, NULL, 0);
    double p1 = vmm_predict(h, 1, NULL, 0);
    check("biased P(0) > P(1)", p0 > p1);

    vmm_destroy(h);
}

// ========== PPM-C specific tests ==========

static void test_ppmc_reference() {
    printf("=== PPM-C on abracadabra ===\n");

    const char* train_str = "abracadabra";
    size_t train_len = strlen(train_str);
    uint16_t train[11];
    str_to_symbols(train_str, train, train_len);

    vmm_handle h = vmm_create("ppmc", 128, 5, NULL, 0);
    if (!h) { printf("FAIL: could not create PPMC predictor\n"); return; }

    vmm_learn(h, train, train_len);

    // Context: "ab"
    uint16_t context[2] = { 'a', 'b' };

    // Verified values from tracing through Java PPMNode.increment():
    double p_a = vmm_predict(h, 'a', context, 2);
    double p_b = vmm_predict(h, 'b', context, 2);
    double p_c = vmm_predict(h, 'c', context, 2);
    double p_d = vmm_predict(h, 'd', context, 2);
    double p_r = vmm_predict(h, 'r', context, 2);

    printf("  P(a|ab)=%.5f P(b|ab)=%.5f P(c|ab)=%.5f P(d|ab)=%.5f P(r|ab)=%.5f\n",
           p_a, p_b, p_c, p_d, p_r);

    check_near("P(r|ab)", p_r, 0.80000, 0.001);
    check_near("P(a|ab)", p_a, 0.00882, 0.001);
    check_near("P(b|ab)", p_b, 0.00441, 0.001);
    check_near("P(c|ab)", p_c, 0.00294, 0.001);
    check_near("P(d|ab)", p_d, 0.00294, 0.001);

    double probs[128];
    vmm_predict_distribution(h, context, 2, probs);
    check_sum("dist sums to 1", probs, 128, 0.01);

    // Context "a" — P(b|a) should be > P(c|a)
    uint16_t ctx_a[1] = { 'a' };
    double p_b_given_a = vmm_predict(h, 'b', ctx_a, 1);
    double p_c_given_a = vmm_predict(h, 'c', ctx_a, 1);
    printf("  P(b|a)=%.5f, P(c|a)=%.5f\n", p_b_given_a, p_c_given_a);
    check("P(b|a) > P(c|a)", p_b_given_a > p_c_given_a);

    vmm_destroy(h);
}

static void test_ppmc_basic() {
    printf("=== PPM-C Basic Properties ===\n");

    vmm_handle h = vmm_create("ppmc", 2, 3, NULL, 0);
    uint16_t data[] = {0, 1, 0, 1, 0, 1, 0, 1};
    vmm_learn(h, data, 8);

    uint16_t ctx1[] = {1};
    double p0_given_1 = vmm_predict(h, 0, ctx1, 1);
    printf("  P(0|1)=%.5f (should be high)\n", p0_given_1);
    check("P(0|1) > 0.5", p0_given_1 > 0.5);

    uint16_t ctx0[] = {0};
    double p1_given_0 = vmm_predict(h, 1, ctx0, 1);
    printf("  P(1|0)=%.5f (should be high)\n", p1_given_0);
    check("P(1|0) > 0.5", p1_given_0 > 0.5);

    double probs[2];
    vmm_predict_distribution(h, ctx1, 1, probs);
    check_sum("binary dist", probs, 2, 0.001);

    vmm_destroy(h);
}

static void test_ppmc_empty_context() {
    printf("=== PPM-C Empty Context ===\n");

    vmm_handle h = vmm_create("ppmc", 4, 3, NULL, 0);
    uint16_t data[] = {0, 1, 2, 3, 0, 1, 2, 3};
    vmm_learn(h, data, 8);

    double probs[4];
    vmm_predict_distribution(h, NULL, 0, probs);
    check_sum("empty ctx dist", probs, 4, 0.001);

    printf("  P(0|empty)=%.5f P(1|empty)=%.5f\n", probs[0], probs[1]);

    vmm_destroy(h);
}

// ========== CTW-specific tests ==========

static void test_ctw_alternating() {
    printf("=== CTW Alternating Pattern ===\n");

    vmm_handle h = vmm_create("ctw", 2, 4, NULL, 0);
    uint16_t data[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    vmm_learn(h, data, 12);

    uint16_t ctx1[] = {1};
    double p0 = vmm_predict(h, 0, ctx1, 1);
    printf("  P(0|1)=%.5f\n", p0);
    check("CTW P(0|1) > 0.5", p0 > 0.5);

    uint16_t ctx0[] = {0};
    double p1 = vmm_predict(h, 1, ctx0, 1);
    printf("  P(1|0)=%.5f\n", p1);
    check("CTW P(1|0) > 0.5", p1 > 0.5);

    vmm_destroy(h);
}

// ========== Binary CTW-specific tests ==========

static void test_bctw_biased_binary() {
    printf("=== Binary CTW Biased Binary ===\n");

    vmm_handle h = vmm_create("bctw", 2, 4, NULL, 0);
    // Train: mostly 0s
    uint16_t data[30];
    for (int i = 0; i < 30; i++) data[i] = (i % 7 == 0) ? 1 : 0;
    vmm_learn(h, data, 30);

    double p0 = vmm_predict(h, 0, NULL, 0);
    double p1 = vmm_predict(h, 1, NULL, 0);
    printf("  P(0)=%.5f P(1)=%.5f\n", p0, p1);
    check("BCTW P(0) > P(1)", p0 > p1);

    vmm_destroy(h);
}

// ========== DCTW-specific tests ==========

static void test_dctw_quaternary() {
    printf("=== DCTW Quaternary Alphabet ===\n");

    vmm_handle h = vmm_create("dctw", 4, 4, NULL, 0);
    // Repeating 0-1-2-3 pattern
    uint16_t data[20];
    for (int i = 0; i < 20; i++) data[i] = i % 4;
    vmm_learn(h, data, 20);

    // After context [2, 3], symbol 0 should be most likely
    uint16_t ctx[] = {2, 3};
    double probs[4];
    vmm_predict_distribution(h, ctx, 2, probs);
    printf("  P(0|23)=%.5f P(1|23)=%.5f P(2|23)=%.5f P(3|23)=%.5f\n",
           probs[0], probs[1], probs[2], probs[3]);
    check_sum("DCTW dist", probs, 4, 0.02);

    vmm_destroy(h);
}

// ========== PST-specific tests ==========

static void test_pst_abracadabra() {
    printf("=== PST on abracadabra ===\n");

    const char* train_str = "abracadabra";
    size_t train_len = strlen(train_str);
    uint16_t train[11];
    str_to_symbols(train_str, train, train_len);

    // PST with relaxed params to build non-trivial tree
    double params[] = {0.001, 0.0, 0.5, 1.05};
    vmm_handle h = vmm_create("pst", 128, 5, params, 4);
    if (!h) { printf("FAIL: could not create PST\n"); tests_run++; return; }

    vmm_learn(h, train, train_len);

    // 'a' is most frequent, so P(a|empty) should be relatively high
    double p_a = vmm_predict(h, 'a', NULL, 0);
    double p_z = vmm_predict(h, 'z', NULL, 0);
    printf("  P(a|empty)=%.5f P(z|empty)=%.5f\n", p_a, p_z);
    check("PST P(a) > P(z)", p_a > p_z);

    double probs[128];
    vmm_predict_distribution(h, NULL, 0, probs);
    check_sum("PST dist", probs, 128, 0.01);

    vmm_destroy(h);
}

// ========== LZms-specific tests ==========

static void test_lzms_basic() {
    printf("=== LZms Basic ===\n");

    double params[] = {2, 2}; // m=2, s=2
    vmm_handle h = vmm_create("lzms", 4, 0, params, 2);
    if (!h) { printf("FAIL: could not create LZms\n"); tests_run++; return; }

    uint16_t data[] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
    vmm_learn(h, data, 12);

    double probs[4];
    vmm_predict_distribution(h, NULL, 0, probs);
    check_sum("LZms dist", probs, 4, 0.02);

    // With repeating pattern, prediction should not be perfectly uniform
    printf("  P(0)=%.5f P(1)=%.5f P(2)=%.5f P(3)=%.5f\n",
           probs[0], probs[1], probs[2], probs[3]);

    vmm_destroy(h);
}

// ========== LZ78-specific tests (LZms with m=0, s=0) ==========

static void test_lz78_basic() {
    printf("=== LZ78 Basic ===\n");

    vmm_handle h = vmm_create("lz78", 4, 0, NULL, 0);
    if (!h) { printf("FAIL: could not create LZ78\n"); tests_run++; return; }

    uint16_t data[] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
    vmm_learn(h, data, 12);

    double probs[4];
    vmm_predict_distribution(h, NULL, 0, probs);
    check_sum("LZ78 dist", probs, 4, 0.02);

    double le = vmm_log_eval(h, data, 12);
    printf("  log_eval=%.5f bits\n", le);
    check("LZ78 log_eval >= 0", le >= 0.0);
    check("LZ78 log_eval is finite", std::isfinite(le));

    vmm_destroy(h);
}

// ========== Cross-algorithm comparison ==========

static void test_cross_algorithm() {
    printf("=== Cross-Algorithm Comparison ===\n");

    const char* algos[] = {"ppmc", "ctw", "bctw", "dctw", "pst", "lzms", "lz78"};
    int n_algos = 7;

    uint16_t data[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    int data_len = 10;

    for (int a = 0; a < n_algos; a++) {
        const double* params = NULL;
        int n_params = 0;
        double pst_params[] = {0.001, 0.0, 0.5, 1.05};
        double lzms_params[] = {2.0, 2.0};

        if (strcmp(algos[a], "pst") == 0) { params = pst_params; n_params = 4; }
        if (strcmp(algos[a], "lzms") == 0) { params = lzms_params; n_params = 2; }

        vmm_handle h = vmm_create(algos[a], 2, 4, params, n_params);
        if (!h) {
            printf("  FAIL: could not create %s\n", algos[a]);
            tests_run++;
            continue;
        }

        vmm_learn(h, data, data_len);

        double probs[2];
        vmm_predict_distribution(h, NULL, 0, probs);
        double le = vmm_log_eval(h, data, data_len);

        printf("  %s: P(0)=%.4f P(1)=%.4f logeval=%.2f bits\n",
               algos[a], probs[0], probs[1], le);

        check_sum((std::string(algos[a]) + " dist").c_str(), probs, 2, 0.02);
        check((std::string(algos[a]) + " log_eval finite").c_str(),
              std::isfinite(le) && le >= 0.0);

        vmm_destroy(h);
    }
}

// ========== Run generic tests for each algorithm ==========

static void run_generic_tests(const char* algo, uint16_t ab_size, int order,
                               const double* params, int n_params) {
    printf("=== %s Generic Tests (ab=%d, order=%d) ===\n", algo, ab_size, order);
    test_create_destroy(algo, ab_size, order, params, n_params);
    test_untrained(algo, ab_size, order, params, n_params);
    test_distribution_sums(algo, ab_size, order, params, n_params);
    test_log_eval(algo, ab_size, order, params, n_params);
    test_biased_learning(algo, ab_size, order, params, n_params);
}

int main() {
    // PPM-C specific
    test_ppmc_basic();
    printf("\n");
    test_ppmc_reference();
    printf("\n");
    test_ppmc_empty_context();
    printf("\n");

    // CTW specific
    test_ctw_alternating();
    printf("\n");

    // Binary CTW specific
    test_bctw_biased_binary();
    printf("\n");

    // DCTW specific
    test_dctw_quaternary();
    printf("\n");

    // PST specific
    test_pst_abracadabra();
    printf("\n");

    // LZms specific
    test_lzms_basic();
    printf("\n");

    // LZ78 specific
    test_lz78_basic();
    printf("\n");

    // Generic tests for all algorithms
    double pst_params[] = {0.001, 0.0, 0.5, 1.05};
    double lzms_params[] = {2.0, 2.0};

    run_generic_tests("ppmc", 4, 5, NULL, 0);
    printf("\n");
    run_generic_tests("ctw", 4, 5, NULL, 0);
    printf("\n");
    run_generic_tests("bctw", 4, 5, NULL, 0);
    printf("\n");
    run_generic_tests("dctw", 4, 5, NULL, 0);
    printf("\n");
    run_generic_tests("pst", 4, 5, pst_params, 4);
    printf("\n");
    run_generic_tests("lzms", 4, 0, lzms_params, 2);
    printf("\n");
    run_generic_tests("lz78", 4, 0, NULL, 0);
    printf("\n");

    // Cross-algorithm comparison
    test_cross_algorithm();
    printf("\n");

    printf("=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
