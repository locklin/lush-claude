/* xgboost-c.h -- Thin C wrappers around xgboost C API for Lush FFI
 *
 * Each function reports errors via int *status (0=success, -1=error).
 * Opaque handles (DMatrixHandle, BoosterHandle) are passed as void*.
 */

#ifndef LUSH_XGBOOST_C_H
#define LUSH_XGBOOST_C_H

/* DMatrix */
void *lush_xgb_dmatrix_create(float *data, int nrows, int ncols,
                               float missing, int *status);
void  lush_xgb_dmatrix_free(void *handle);
void  lush_xgb_dmatrix_set_labels(void *handle, float *labels, int n,
                                   int *status);

/* Booster */
void *lush_xgb_booster_create(void **dmats, int n, int *status);
void  lush_xgb_booster_free(void *handle);
void  lush_xgb_booster_set_param(void *handle, const char *key,
                                  const char *val, int *status);
void  lush_xgb_booster_update(void *handle, void *dtrain, int iter,
                               int *status);

/* Prediction -- returns malloc'd copy; caller must free via lush_xgb_free_prediction */
float *lush_xgb_predict(void *booster, void *dmat, int *out_len,
                         int *status);
void   lush_xgb_free_prediction(float *pred);

/* Model I/O */
void  lush_xgb_save_model(void *handle, const char *path, int *status);
void *lush_xgb_load_model(const char *path, int *status);

#endif /* LUSH_XGBOOST_C_H */
