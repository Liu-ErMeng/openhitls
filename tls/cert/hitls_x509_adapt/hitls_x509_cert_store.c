/*
 * This file is part of the openHiTLS project.
 *
 * openHiTLS is licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *     http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <stdint.h>
#include "bsl_err_internal.h"
#include "crypt_errno.h"
#include "hitls_cert_type.h"
#include "hitls_type.h"
#include "hitls_cert_local.h"
#include "hitls_x509.h"
#include "hitls_error.h"
#include "hitls_x509_adapt_local.h"

HITLS_CERT_Store *HITLS_X509_Adapt_StoreNew(void)
{
    return (HITLS_CERT_Store *)HITLS_X509_StoreCtxNew();
}

HITLS_CERT_Store *HITLS_X509_Adapt_StoreDup(HITLS_CERT_Store *store)
{
    int references = 0;
    int32_t ret = HITLS_X509_StoreCtxCtrl((HITLS_X509_StoreCtx *)store, HITLS_X509_STORECTX_REF_UP, &references,
        sizeof(int));
    if (ret != CRYPT_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return NULL;
    }

    return store;
}

void HITLS_X509_Adapt_StoreFree(HITLS_CERT_Store *store)
{
    HITLS_X509_StoreCtxFree(store);
}

int32_t HITLS_X509_Adapt_StoreCtrl(HITLS_Config *config, HITLS_CERT_Store *store, HITLS_CERT_CtrlCmd cmd,
    void *input, void *output)
{
    (void)config;
    (void)output;
    int32_t inputLen = 0;
    int32_t x509Cmd;
    switch (cmd) {
        case CERT_STORE_CTRL_SET_VERIFY_DEPTH:
            x509Cmd = HITLS_X509_STORECTX_SET_PARAM_DEPTH;
            inputLen = sizeof(int32_t);
            break;
        case CERT_STORE_CTRL_DEEP_COPY_ADD_CERT_LIST:
            x509Cmd = HITLS_X509_STORECTX_DEEP_COPY_SET_CA;
            inputLen = sizeof(HITLS_X509_Cert);
            break;
        case CERT_STORE_CTRL_SHALLOW_COPY_ADD_CERT_LIST:
            x509Cmd = HITLS_X509_STORECTX_SHALLOW_COPY_SET_CA;
            inputLen = sizeof(HITLS_X509_Cert);
            break;
        default:
            return HITLS_X509_ADAPT_ERR;
    }

    return HITLS_X509_StoreCtxCtrl(store, x509Cmd, input, inputLen);
}
