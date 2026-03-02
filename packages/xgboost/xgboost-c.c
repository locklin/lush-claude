/* xgboost-c.c -- Thin C wrappers around xgboost C API for Lush FFI
 *
 * Each function calls the xgboost C API and reports errors via
 * int *status (0=success, -1=error). Opaque handles are passed
 * as void* so Lush can treat them as gptr values.
 */

#include "xgboost-c.h"
#include <xgboost/c_api.h>
#include <stdlib.h>
#include <string.h>

/* ---- DMatrix ---- */

void *lush_xgb_dmatrix_create(float *data, int nrows, int ncols,
                               float missing, int *status)
{
    DMatrixHandle handle = NULL;
    int rc = XGDMatrixCreateFromMat(data, (bst_ulong)nrows, (bst_ulong)ncols,
                                    missing, &handle);
    *status = (rc == 0) ? 0 : -1;
    return (void *)handle;
}

void lush_xgb_dmatrix_free(void *handle)
{
    if (handle)
        XGDMatrixFree((DMatrixHandle)handle);
}

void lush_xgb_dmatrix_set_labels(void *handle, float *labels, int n,
                                  int *status)
{
    int rc = XGDMatrixSetFloatInfo((DMatrixHandle)handle, "label",
                                   labels, (bst_ulong)n);
    *status = (rc == 0) ? 0 : -1;
}

/* ---- Booster ---- */

void *lush_xgb_booster_create(void **dmats, int n, int *status)
{
    BoosterHandle handle = NULL;
    int rc = XGBoosterCreate((DMatrixHandle *)dmats, (bst_ulong)n, &handle);
    *status = (rc == 0) ? 0 : -1;
    return (void *)handle;
}

void lush_xgb_booster_free(void *handle)
{
    if (handle)
        XGBoosterFree((BoosterHandle)handle);
}

void lush_xgb_booster_set_param(void *handle, const char *key,
                                 const char *val, int *status)
{
    int rc = XGBoosterSetParam((BoosterHandle)handle, key, val);
    *status = (rc == 0) ? 0 : -1;
}

void lush_xgb_booster_update(void *handle, void *dtrain, int iter,
                              int *status)
{
    int rc = XGBoosterUpdateOneIter((BoosterHandle)handle, iter,
                                    (DMatrixHandle)dtrain);
    *status = (rc == 0) ? 0 : -1;
}

/* ---- Prediction ---- */

float *lush_xgb_predict(void *booster, void *dmat, int *out_len,
                         int *status)
{
    bst_ulong len = 0;
    const float *raw = NULL;
    float *copy = NULL;

    int rc = XGBoosterPredict((BoosterHandle)booster, (DMatrixHandle)dmat,
                               0, 0, 0, &len, &raw);
    if (rc != 0 || len == 0 || !raw) {
        *out_len = 0;
        *status = -1;
        return NULL;
    }

    copy = (float *)malloc(len * sizeof(float));
    if (!copy) {
        *out_len = 0;
        *status = -1;
        return NULL;
    }
    memcpy(copy, raw, len * sizeof(float));
    *out_len = (int)len;
    *status = 0;
    return copy;
}

void lush_xgb_free_prediction(float *pred)
{
    if (pred) free(pred);
}

/* ---- Model I/O ---- */

void lush_xgb_save_model(void *handle, const char *path, int *status)
{
    int rc = XGBoosterSaveModel((BoosterHandle)handle, path);
    *status = (rc == 0) ? 0 : -1;
}

void *lush_xgb_load_model(const char *path, int *status)
{
    BoosterHandle handle = NULL;
    int rc;

    /* Create an empty booster first, then load into it */
    rc = XGBoosterCreate(NULL, 0, &handle);
    if (rc != 0) {
        *status = -1;
        return NULL;
    }
    rc = XGBoosterLoadModel(handle, path);
    if (rc != 0) {
        XGBoosterFree(handle);
        *status = -1;
        return NULL;
    }
    *status = 0;
    return (void *)handle;
}
