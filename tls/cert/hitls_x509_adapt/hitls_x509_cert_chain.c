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
#include "hitls_cert_type.h"
#include "hitls_type.h"
#include "hitls_x509.h"
#include "bsl_list.h"
#include "hitls_error.h"

static int32_t BuildArrayFromList(HITLS_X509_List *list, HITLS_CERT_X509 **listArray, uint32_t *num)
{
    HITLS_X509_Cert *elemt = NULL;
    int32_t i = 0;
    int32_t ret;
    for (list->curr = list->first; list->curr != NULL; list->curr = list->curr->next, i++) {
        elemt = (HITLS_X509_Cert *)list->curr->data;
        if (elemt == NULL || i >= list->count) {
            BSL_ERR_PUSH_ERROR(HITLS_X509_ADAPT_BUILD_CERT_CHAIN_ERR);
            return HITLS_X509_ADAPT_BUILD_CERT_CHAIN_ERR;
        }

        int ref = 0;
        ret = HITLS_X509_CertCtrl(elemt, HITLS_X509_REF_UP, (void *)&ref, (int32_t)sizeof(int));
        if (ret != HITLS_SUCCESS) {
            BSL_ERR_PUSH_ERROR(ret);
            return ret;
        }
        listArray[i] = elemt;
    }

    *num = i;
    return HITLS_SUCCESS;
}

static int32_t BuildCertListFromCertArray(HITLS_CERT_X509 **listCert, uint32_t num, HITLS_X509_List **list)
{
    int32_t ret = HITLS_SUCCESS;
    HITLS_X509_Cert **listArray = (HITLS_X509_Cert **)listCert;
    *list = BSL_LIST_New(num);
    if (*list == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_MEMALLOC_FAIL);
        return HITLS_MEMALLOC_FAIL;
    }
    for (uint32_t i = 0; i < num; i++) {
        int ref = 0;
        ret = HITLS_X509_CertCtrl(listArray[i], HITLS_X509_REF_UP, (void *)&ref, (int32_t)sizeof(int));
        if (ret != HITLS_SUCCESS) {
            BSL_LIST_FREE(*list, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
            return ret;
        }
        ret = BSL_LIST_AddElement(*list, listArray[i], BSL_LIST_POS_END);
        if (ret != HITLS_SUCCESS) {
            BSL_ERR_PUSH_ERROR(ret);
            BSL_LIST_FREE(*list, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
            return ret;
        }
    }
    return HITLS_SUCCESS;
}

int32_t HITLS_X509_Adapt_BuildCertChain(HITLS_Config *config, HITLS_CERT_Store *store, HITLS_CERT_X509 *cert,
    HITLS_CERT_X509 **list, uint32_t *num)
{
    (void)config;
    *num = 0;
    HITLS_X509_List *certChain = NULL;
    int32_t ret = HITLS_X509_CertChainBuild((HITLS_X509_StoreCtx *)store, cert, &certChain);
    if (ret != HITLS_SUCCESS) {
        return ret;
    }
    ret = BuildArrayFromList(certChain, list, num);
    BSL_LIST_FREE(certChain, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
    return ret;
}

int32_t HITLS_X509_Adapt_VerifyCertChain(HITLS_Ctx *ctx, HITLS_CERT_Store *store, HITLS_CERT_X509 **list, uint32_t num)
{
    (void)ctx;
    HITLS_X509_List *certList = NULL;
    int32_t ret = BuildCertListFromCertArray(list, num, &certList);
    if (ret != HITLS_SUCCESS) {
        return ret;
    }
    ret = HITLS_X509_CertVerify((HITLS_X509_StoreCtx *)store, certList);
    if (ret != HITLS_SUCCESS) {
        BSL_LIST_FREE(certList, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
        return ret;
    }

    BSL_LIST_FREE(certList, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
    return HITLS_SUCCESS;
}
