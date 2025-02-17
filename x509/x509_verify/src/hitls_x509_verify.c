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

#include <string.h>
#include "securec.h"
#include "hitls_x509.h"
#include "hitls_error.h"
#include "sal_atomic.h"
#include "crypt_eal_pkey.h"
#include "bsl_err_internal.h"
#include "hitls_crl_local.h"
#include "hitls_cert_local.h"
#include "hitls_x509_local.h"
#include "bsl_obj_internal.h"
#include "hitls_x509_errno.h"
#include "crypt_errno.h"
#include "bsl_list.h"
#include "bsl_list_internal.h"
#include "hitls_x509_verify.h"

typedef int32_t (*HITLS_X509_TrvListCallBack)(void *ctx, void *node);
typedef int32_t (*HITLS_X509_TrvListWithParentCallBack)(void *ctx, void *node, void *parent);

// lists can be cert, ext, and so on.
static int32_t HITLS_X509_TrvList(BslList *list, HITLS_X509_TrvListCallBack callBack, void *ctx)
{
    int32_t ret = HITLS_X509_SUCCESS;
    void *node = BSL_LIST_GET_FIRST(list);
    while (node != NULL) {
        ret = callBack(ctx, node);
        if (ret != BSL_SUCCESS) {
            return ret;
        }
        node = BSL_LIST_GET_NEXT(list);
    }
    return ret;
}

// lists can be cert, ext, and so on.
static int32_t HITLS_X509_TrvListWithParent(BslList *list, HITLS_X509_TrvListWithParentCallBack callBack, void *ctx)
{
    int32_t ret = HITLS_X509_SUCCESS;
    void *node = BSL_LIST_GET_FIRST(list);
    void *parentNode = BSL_LIST_GET_NEXT(list);
    while (node != NULL && parentNode != NULL) {
        ret = callBack(ctx, node, parentNode);
        if (ret != BSL_SUCCESS) {
            return ret;
        }
        node = parentNode;
        parentNode = BSL_LIST_GET_NEXT(list);
    }
    return ret;
}

#define HITLS_X509_MAX_DEPTH 20

void HITLS_X509_StoreCtxFree(HITLS_X509_StoreCtx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    int ret;
    (void)BSL_SAL_AtomicDownReferences(&ctx->references, &ret);
    if (ret > 0) {
        return;
    }
    if (ctx->store != NULL) {
        BSL_LIST_FREE(ctx->store, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
    }
    if (ctx->crl != NULL) {
        BSL_LIST_FREE(ctx->crl, (BSL_LIST_PFUNC_FREE)HITLS_X509_CrlFree);
    }
    BSL_SAL_ReferencesFree(&ctx->references);
    BSL_SAL_Free(ctx);
}

static int32_t X509_CrlCmp(HITLS_X509_Crl *crlOri, HITLS_X509_Crl *crl)
{
    if (crlOri == crl) {
        return 0;
    }
    if (HITLS_X509_CmpNameNode(crlOri->tbs.issuerName, crl->tbs.issuerName) != 0) {
        return 1;
    }
    if (crlOri->tbs.tbsRawDataLen != crl->tbs.tbsRawDataLen) {
        return 1;
    }
    return memcmp(crlOri->tbs.tbsRawData, crl->tbs.tbsRawData, crl->tbs.tbsRawDataLen);
}

static int32_t X509_CertCmp(HITLS_X509_Cert *certOri, HITLS_X509_Cert *cert)
{
    if (certOri == cert) {
        return 0;
    }
    if (HITLS_X509_CmpNameNode(certOri->tbs.subjectName, cert->tbs.subjectName) != 0) {
        return 1;
    }
    if (certOri->tbs.tbsRawDataLen != cert->tbs.tbsRawDataLen) {
        return 1;
    }
    return memcmp(certOri->tbs.tbsRawData, cert->tbs.tbsRawData, cert->tbs.tbsRawDataLen);
}

HITLS_X509_StoreCtx *HITLS_X509_StoreCtxNew(void)
{
    HITLS_X509_StoreCtx *ctx = (HITLS_X509_StoreCtx *)BSL_SAL_Malloc(sizeof(HITLS_X509_StoreCtx));
    if (ctx == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_MEMALLOC_FAIL);
        return NULL;
    }

    (void)memset_s(ctx, sizeof(HITLS_X509_StoreCtx), 0, sizeof(HITLS_X509_StoreCtx));
    ctx->store = BSL_LIST_New(sizeof(HITLS_X509_Cert *));
    if (ctx->store == NULL) {
        BSL_SAL_Free(ctx);
        BSL_ERR_PUSH_ERROR(HITLS_MEMALLOC_FAIL);
        return NULL;
    }
    ctx->crl = BSL_LIST_New(sizeof(HITLS_X509_Crl *));
    if (ctx->crl == NULL) {
        BSL_SAL_FREE(ctx->store);
        BSL_SAL_Free(ctx);
        BSL_ERR_PUSH_ERROR(HITLS_MEMALLOC_FAIL);
        return NULL;
    }

    ctx->verifyParam.maxDepth = HITLS_X509_MAX_DEPTH;
    ctx->verifyParam.securityBits = 128; // 128: The default number of secure bits.
    BSL_SAL_ReferencesInit(&(ctx->references));
    return ctx;
}

static int32_t X509_SetMaxDepth(HITLS_X509_StoreCtx *storeCtx, void *val, int32_t valLen)
{
    if (val == NULL || valLen != sizeof(int32_t)) {
        BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
        return HITLS_INVALID_INPUT;
    }

    int32_t depth = *(int32_t *)val;
    if (depth > HITLS_X509_MAX_DEPTH) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    storeCtx->verifyParam.maxDepth = depth;
    return HITLS_SUCCESS;
}

static int32_t X509_SetParamFlag(HITLS_X509_StoreCtx *storeCtx, void *val, int32_t valLen)
{
    if (val == NULL || valLen != sizeof(int64_t)) {
        BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
        return HITLS_INVALID_INPUT;
    }

    storeCtx->verifyParam.flags |= *(int64_t *)val;
    return HITLS_SUCCESS;
}

static int32_t X509_SetVerifyTime(HITLS_X509_StoreCtx *storeCtx, void *val, int32_t valLen)
{
    if (val == NULL || valLen != sizeof(int64_t)) {
        BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
        return HITLS_INVALID_INPUT;
    }

    storeCtx->verifyParam.time |= *(int64_t *)val;
    storeCtx->verifyParam.flags |= HITLS_X509_VFY_FLAG_TIME;
    return HITLS_SUCCESS;
}

static int32_t X509_SetVerifySecurityBits(HITLS_X509_StoreCtx *storeCtx, void *val, int32_t valLen)
{
    if (val == NULL || valLen != sizeof(uint32_t)) {
        BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
        return HITLS_INVALID_INPUT;
    }

    storeCtx->verifyParam.securityBits = *(uint32_t *)val;
    storeCtx->verifyParam.flags |= HITLS_X509_VFY_FLAG_SECBITS;
    return HITLS_SUCCESS;
}

static int32_t X509_ClearParamFlag(HITLS_X509_StoreCtx *storeCtx, void *val, int32_t valLen)
{
    if (val == NULL || valLen != sizeof(int64_t)) {
        BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
        return HITLS_INVALID_INPUT;
    }

    storeCtx->verifyParam.flags &= ~(*(int64_t *)val);
    return HITLS_SUCCESS;
}

static int32_t X509_CheckCert(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_Cert *cert)
{
    bool res = false;
    int32_t ret = HITLS_X509_CertIsCA(cert, &res);
    if (ret != HITLS_SUCCESS) {
        return ret;
    }
    if (!res) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CERT_NOT_CA);
        return HITLS_X509_ERR_CERT_NOT_CA;
    }
    HITLS_X509_List *certStore = storeCtx->store;
    HITLS_X509_Cert * tmp = BSL_LIST_SearchEx(certStore, cert, (BSL_LIST_PFUNC_CMP)X509_CertCmp);
    if (tmp != NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CERT_EXIST);
        return HITLS_X509_ERR_CERT_EXIST;
    }

    return HITLS_X509_SUCCESS;
}

static int32_t X509_SetCA(HITLS_X509_StoreCtx *storeCtx, void *val, int32_t valLen, bool isCopy)
{
    if (val == NULL || valLen != sizeof(HITLS_X509_Cert)) {
        BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
        return HITLS_INVALID_INPUT;
    }
    int32_t ret = X509_CheckCert(storeCtx, val);
    if (ret != HITLS_X509_SUCCESS) {
        return ret;
    }
    if (isCopy) {
        int ref;
        ret = HITLS_X509_CertCtrl(val, HITLS_X509_REF_UP, &ref, sizeof(int));
        if (ret != HITLS_SUCCESS) {
            return ret;
        }
    }

    ret = BSL_LIST_AddElement(storeCtx->store, val, BSL_LIST_POS_BEFORE);
    if (ret != HITLS_SUCCESS) {
        if (isCopy) {
            HITLS_X509_CertFree(val);
        }
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    return ret;
}

static int32_t X509_CheckCRL(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_Crl *crl)
{
    HITLS_X509_List *crlStore = storeCtx->crl;
    HITLS_X509_Crl *tmp = BSL_LIST_SearchEx(crlStore, crl, (BSL_LIST_PFUNC_CMP)X509_CrlCmp);
    if (tmp != NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CRL_EXIST);
        return HITLS_X509_ERR_CRL_EXIST;
    }

    return HITLS_X509_SUCCESS;
}

static int32_t X509_SetCRL(HITLS_X509_StoreCtx *storeCtx, void *val, int32_t valLen)
{
    if (val == NULL || valLen != sizeof(HITLS_X509_Crl)) {
        BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
        return HITLS_INVALID_INPUT;
    }
    int32_t ret = X509_CheckCRL(storeCtx, val);
    if (ret != HITLS_SUCCESS) {
        return ret;
    }
    int ref;
    ret = HITLS_X509_CrlCtrl(val, HITLS_X509_CRL_REF_UP, &ref, sizeof(int));
    if (ret != HITLS_SUCCESS) {
        return ret;
    }
    ret = BSL_LIST_AddElement(storeCtx->crl, val, BSL_LIST_POS_BEFORE);
    if (ret != HITLS_SUCCESS) {
        HITLS_X509_CrlFree(val);
        BSL_ERR_PUSH_ERROR(ret);
    }
    return ret;
}

static int32_t X509_RefUp(HITLS_X509_StoreCtx *storeCtx, void *val, int32_t valLen)
{
    if (val == NULL || valLen != sizeof(int)) {
        BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
        return HITLS_INVALID_INPUT;
    }

    return BSL_SAL_AtomicUpReferences(&storeCtx->references, val);
}

int32_t HITLS_X509_StoreCtxCtrl(HITLS_X509_StoreCtx *storeCtx, int32_t cmd, void *val, int32_t valLen)
{
    if (storeCtx == NULL || cmd < HITLS_X509_STORECTX_SET_PARAM_DEPTH || cmd >= HITLS_X509_STORECTX_MAX) {
        BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
        return HITLS_INVALID_INPUT;
    }
    switch (cmd) {
        case HITLS_X509_STORECTX_SET_PARAM_DEPTH:
            return X509_SetMaxDepth(storeCtx, val, valLen);
        case HITLS_X509_STORECTX_SET_PARAM_FLAGS:
            return X509_SetParamFlag(storeCtx, val, valLen);
        case HITLS_X509_STORECTX_SET_TIME:
            return X509_SetVerifyTime(storeCtx, val, valLen);
        case HITLS_X509_STORECTX_SET_SECBITS:
            return X509_SetVerifySecurityBits(storeCtx, val, valLen);
        case HITLS_X509_STORECTX_CLR_PARAM_FLAGS:
            return X509_ClearParamFlag(storeCtx, val, valLen);
        case HITLS_X509_STORECTX_DEEP_COPY_SET_CA:
            return X509_SetCA(storeCtx, val, valLen, true);
        case HITLS_X509_STORECTX_SHALLOW_COPY_SET_CA:
            return X509_SetCA(storeCtx, val, valLen, false);
        case HITLS_X509_STORECTX_SET_CRL:
            return X509_SetCRL(storeCtx, val, valLen);
        case HITLS_X509_STORECTX_REF_UP:
            return X509_RefUp(storeCtx, val, valLen);
        default:
            BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
            return HITLS_INVALID_INPUT;
    }
}

static int32_t X509_SelfSigned(HITLS_X509_Cert *cert, bool *selfSigned)
{
    int32_t ret = HITLS_X509_CheckIssued(cert, cert, selfSigned);
    if (ret != HITLS_X509_SUCCESS) {
        return ret;
    }
    return HITLS_X509_SUCCESS;
}

int32_t HITLS_X509_CheckTime(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_ValidTime *validTime)
{
    int64_t start = 0;
    int64_t end = 0;
    if ((storeCtx->verifyParam.flags & HITLS_X509_VFY_FLAG_TIME) == 0) {
        return HITLS_X509_SUCCESS;
    }
    
    int32_t ret = BSL_SAL_DateToUtcTimeConvert(&validTime->start, &start);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    if (start > storeCtx->verifyParam.time) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_TIME_FUTURE);
        return HITLS_X509_ERR_TIME_FUTURE;
    }

    if ((validTime->flag & BSL_TIME_AFTER_SET) == 0) {
        return HITLS_X509_SUCCESS;
    }
    
    ret = BSL_SAL_DateToUtcTimeConvert(&validTime->end, &end);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    if (end < storeCtx->verifyParam.time) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_TIME_EXPIRED);
        return HITLS_X509_ERR_TIME_EXPIRED;
    }
    return HITLS_X509_SUCCESS;
}

static int32_t X509_AddCertToChain(HITLS_X509_List *chain, HITLS_X509_Cert *cert)
{
    int ref;
    int32_t ret = HITLS_X509_CertCtrl(cert, HITLS_X509_REF_UP, &ref, sizeof(int));
    if (ret != HITLS_SUCCESS) {
        return ret;
    }
    ret = BSL_LIST_AddElement(chain, cert, BSL_LIST_POS_END);
    if (ret != HITLS_SUCCESS) {
        HITLS_X509_CertFree(cert);
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    return ret;
}

int32_t X509_GetIssueFromChain(HITLS_X509_List *certChain, HITLS_X509_Cert *cert, HITLS_X509_Cert **issue)
{
    int32_t ret;
    for(HITLS_X509_Cert *tmp = BSL_LIST_GET_FIRST(certChain); tmp != NULL; tmp = BSL_LIST_GET_NEXT(certChain)) {
        bool res = false;
        ret = HITLS_X509_CheckIssued(tmp, cert, &res);
        if (ret != HITLS_SUCCESS) {
            return ret;
        }
        if (!res) {
            continue;
        }
        *issue = tmp;
        return HITLS_SUCCESS;
    }
    BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_ISSUE_CERT_NOT_FOUND);
    return HITLS_X509_ERR_ISSUE_CERT_NOT_FOUND;
}

int32_t X509_FindIssueCert(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_List *certChain, HITLS_X509_Cert *cert, HITLS_X509_Cert **issue,
    bool *issueInTrust)
{
    HITLS_X509_List *store = storeCtx->store;
    int32_t ret = X509_GetIssueFromChain(store, cert, issue);
    if (ret == HITLS_X509_SUCCESS) {
        *issueInTrust = true;
        return ret;
    }
    if (certChain == NULL) {
        return ret;
    }
    ret = X509_GetIssueFromChain(certChain, cert, issue);
    if (ret != HITLS_X509_SUCCESS) {
        return ret;
    }
    *issueInTrust = false;
    return ret;
}

int32_t X509_BuildChain(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_List *certChain, HITLS_X509_Cert *cert, HITLS_X509_List *chain, HITLS_X509_Cert **root)
{
    HITLS_X509_Cert *cur = cert;
    int32_t ret;
    while (cur != NULL) {
        HITLS_X509_Cert *issue = NULL;
        bool isTructCa = false;
        ret = X509_FindIssueCert(storeCtx, certChain, cur, &issue, &isTructCa);
        if (ret != HITLS_SUCCESS) {
            return ret;
        }
        // depth
        if (BSL_LIST_COUNT(chain) + 1 > storeCtx->verifyParam.maxDepth) {
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CHAIN_DEPTH_UP_LIMIT);
            return HITLS_X509_ERR_CHAIN_DEPTH_UP_LIMIT;
        }
        bool selfSigned = false;
        ret = X509_SelfSigned(issue, &selfSigned);
        if (ret != HITLS_SUCCESS) {
            return ret;
        }
        if (selfSigned) {
            if (root != NULL && isTructCa) {
                *root = issue;
            }
            break;
        }
        ret = X509_AddCertToChain(chain, issue);
        if (ret != HITLS_SUCCESS) {
            return ret;
        }
        cur = issue;
    }
    return HITLS_SUCCESS;
}

static HITLS_X509_List *X509_NewCertChain(HITLS_X509_Cert *cert)
{
    HITLS_X509_List *tmpChain = BSL_LIST_New(sizeof(HITLS_X509_Cert *));
    if (tmpChain == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_MEMALLOC_FAIL);
        return NULL;
    }
    int32_t ret = X509_AddCertToChain(tmpChain, cert);
    if (ret != HITLS_SUCCESS) {
        BSL_SAL_Free(tmpChain);
        BSL_ERR_PUSH_ERROR(ret);
        return NULL;
    }
    return tmpChain;
}

int32_t HITLS_X509_CertChainBuild(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_Cert *cert, HITLS_X509_List **chain)
{
    if (storeCtx == NULL || cert == NULL || chain == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
        return HITLS_INVALID_INPUT;
    }
    HITLS_X509_List *tmpChain = X509_NewCertChain(cert);
    if (tmpChain == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_MEMALLOC_FAIL);
        return HITLS_MEMALLOC_FAIL;
    }
    bool selfSigned = false;
    int32_t ret = X509_SelfSigned(cert, &selfSigned);
    if (ret != HITLS_SUCCESS) {
        BSL_LIST_FREE(tmpChain, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
        return ret;
    }
    if (selfSigned) {
        *chain = tmpChain;
        return HITLS_X509_SUCCESS;
    }
    (void)X509_BuildChain(storeCtx, NULL, cert, tmpChain, NULL);
    *chain = tmpChain;

    return HITLS_X509_SUCCESS;
}

static int32_t HITLS_X509_SecBitsCheck(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_Cert *cert)
{
    uint32_t secBits = CRYPT_EAL_PkeyGetSecurityBits(cert->tbs.ealPubKey);
    if (secBits < storeCtx->verifyParam.securityBits) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_VFY_CHECK_SECBITS);
        return HITLS_X509_ERR_VFY_CHECK_SECBITS;
    }
    return HITLS_X509_SUCCESS;
}

int32_t HITLS_X509_CheckVerifyParam(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_List *chain)
{
    if (storeCtx->verifyParam.flags & HITLS_X509_VFY_FLAG_SECBITS) {
        return HITLS_X509_TrvList(chain, (HITLS_X509_TrvListCallBack)HITLS_X509_SecBitsCheck, storeCtx);
    }
    return HITLS_X509_SUCCESS;
}

static int32_t HITLS_X509_CheckCertExtNode(void *ctx, HITLS_X509_ExtEntry *extNode)
{
    (void)ctx;
    if (extNode->cid != BSL_CID_CE_KEYUSAGE && extNode->cid != BSL_CID_CE_BASICCONSTRAINTS &&
        extNode->critical == true) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_PROCESS_CRITICALEXT);
        return HITLS_X509_ERR_PROCESS_CRITICALEXT; // not process critical ext
    }
    return HITLS_X509_SUCCESS;
}

static int32_t HITLS_X509_CheckCertExt(void *ctx, HITLS_X509_Cert *cert)
{
    (void) ctx;
    if (cert->tbs.version != 2) { // no ext v1 cert
        return HITLS_X509_SUCCESS;
    }
    return HITLS_X509_TrvList(cert->tbs.ext.list,
        (HITLS_X509_TrvListCallBack)HITLS_X509_CheckCertExtNode, NULL);
}

int32_t HITLS_X509_VerifyParamAndExt(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_List *chain)
{
    int32_t ret = HITLS_X509_CheckVerifyParam(storeCtx, chain);
    if (ret != HITLS_X509_SUCCESS) {
        return ret;
    }
    ret = HITLS_X509_TrvList(chain, (HITLS_X509_TrvListCallBack)HITLS_X509_CheckCertExt, NULL);
    if (ret != HITLS_X509_SUCCESS) {
        return ret;
    }
    return ret;
}

int32_t HITLS_X509_CheckCertRevoked(HITLS_X509_Cert *cert, HITLS_X509_CrlEntry *crlEntry)
{
    if (cert->tbs.serialNum.tag == crlEntry->serialNumber.tag &&
        cert->tbs.serialNum.len == crlEntry->serialNumber.len &&
        memcmp(cert->tbs.serialNum.buff, crlEntry->serialNumber.buff, crlEntry->serialNumber.len) == 0) {
        return HITLS_X509_ERR_VFY_CERT_REVOKED;
    }
    return HITLS_X509_SUCCESS;
}

int32_t HITLS_X509_CheckCertCrl(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_Cert *cert, HITLS_X509_Cert *parent)
{
    int32_t ret = HITLS_X509_ERR_CRL_NOT_FOUND;
    HITLS_X509_Crl *crl = BSL_LIST_GET_FIRST(storeCtx->crl);
    if (parent->tbs.ext.extFlags & HITLS_X509_EXT_FLAG_KUSAGE) {
        if (!(parent->tbs.ext.keyUsage & HITLS_X509_EXT_KU_CRL_SIGN)) {
            return HITLS_X509_ERR_VFY_KU_NO_CRLSIGN;
        }
    }
    while (crl != NULL) {
        if (HITLS_X509_CmpNameNode(crl->tbs.issuerName, parent->tbs.subjectName) != 0) {
            crl = BSL_LIST_GET_NEXT(storeCtx->crl);
            continue;
        }

        if (HITLS_X509_CheckTime(storeCtx, &(crl->tbs.validTime)) != HITLS_X509_SUCCESS) {
            crl = BSL_LIST_GET_NEXT(storeCtx->crl);
            continue;
        }
        ret = HITLS_X509_TrvList(crl->tbs.crlExt.extList,
            (HITLS_X509_TrvListCallBack)HITLS_X509_CheckCertExtNode, NULL);
        if (ret != HITLS_X509_SUCCESS) {
            BSL_ERR_PUSH_ERROR(ret);
            return ret;
        }
        ret = HITLS_X509_CheckSignature(parent->tbs.ealPubKey, crl->tbs.tbsRawData, crl->tbs.tbsRawDataLen,
            &(crl->signAlgId), &(crl->signature));
        if (ret != HITLS_X509_SUCCESS) {
            BSL_ERR_PUSH_ERROR(ret);
            return ret;
        }
        ret = HITLS_X509_TrvList(crl->tbs.revokedCerts,
            (HITLS_X509_TrvListCallBack)HITLS_X509_CheckCertRevoked, cert);
        if (ret != HITLS_X509_SUCCESS) {
            BSL_ERR_PUSH_ERROR(ret);
            return ret;
        }
        crl = BSL_LIST_GET_NEXT(storeCtx->crl);
    }
    return ret;
}

int32_t HITLS_X509_CrlVerify(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_List *chain)
{
    // Only the self-signed certificate, and the CRL is not verified
    if (BSL_LIST_COUNT(chain) == 1) {
        return HITLS_X509_SUCCESS;
    }
    
    if (storeCtx->verifyParam.flags & HITLS_X509_VFY_FLAG_CRL_ALL) {
        // Device certificate check is included
        return HITLS_X509_TrvListWithParent(chain,
            (HITLS_X509_TrvListWithParentCallBack)HITLS_X509_CheckCertCrl, storeCtx);
    }
    
    if (storeCtx->verifyParam.flags & HITLS_X509_VFY_FLAG_CRL_DEV) {
        HITLS_X509_Cert *cert = BSL_LIST_GET_FIRST(chain);
        HITLS_X509_Cert *parent = BSL_LIST_GET_NEXT(chain);
        return HITLS_X509_CheckCertCrl(storeCtx, cert, parent);
    }
    
    return HITLS_X509_SUCCESS;
}

int32_t X509_VerifyChainCert(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_List *chain)
{
    HITLS_X509_Cert *issue = BSL_LIST_GET_LAST(chain);
    HITLS_X509_Cert *cur = issue;
    int32_t ret;
    while(cur != NULL) {
        if (storeCtx->verifyParam.flags & HITLS_X509_VFY_FLAG_TIME) {
            ret = HITLS_X509_CheckTime(storeCtx, &cur->tbs.validTime);
            if (ret != HITLS_X509_SUCCESS) {
                return ret;
            }
        }
        ret = HITLS_X509_CheckSignature(issue->tbs.ealPubKey, cur->tbs.tbsRawData, cur->tbs.tbsRawDataLen, &cur->signAlgId, &cur->signature);
        if (ret != HITLS_X509_SUCCESS) {
            return ret;
        }
        issue = cur;
        cur = BSL_LIST_GET_PREV(chain);
    };
    return HITLS_X509_SUCCESS;
}

static int32_t X509_GetVerifyCertChain(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_List *chain, HITLS_X509_List **comChain)
{
    HITLS_X509_Cert *cert = BSL_LIST_GET_FIRST(chain);
    if (cert == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
        return HITLS_INVALID_INPUT;
    }
    HITLS_X509_List *tmpChain = X509_NewCertChain(cert);
    if (tmpChain == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_MEMALLOC_FAIL);
        return HITLS_MEMALLOC_FAIL;
    }
    HITLS_X509_Cert *root = NULL;
    int32_t ret = X509_BuildChain(storeCtx, chain, cert, tmpChain, &root);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LIST_FREE(tmpChain, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    if (root == NULL) {
        BSL_LIST_FREE(tmpChain, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_ROOT_CERT_NOT_FOUND);
        return HITLS_X509_ERR_ROOT_CERT_NOT_FOUND;
    }
    if (X509_CertCmp(cert, root) != 0) {
        ret = X509_AddCertToChain(tmpChain, root);
        if (ret != HITLS_X509_SUCCESS) {
            BSL_LIST_FREE(tmpChain, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
            return ret;
        }
    }
    *comChain = tmpChain;
    return HITLS_X509_SUCCESS;
}

int32_t HITLS_X509_CertVerify(HITLS_X509_StoreCtx *storeCtx, HITLS_X509_List *chain)
{
    if (storeCtx == NULL || chain == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_INVALID_INPUT);
        return HITLS_INVALID_INPUT;
    }
    if (BSL_LIST_COUNT(chain) == 0) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CERT_CHAIN_COUNT_IS0);
        return HITLS_X509_ERR_CERT_CHAIN_COUNT_IS0;
    }
    HITLS_X509_List *tmpChain = NULL;
    int32_t ret = X509_GetVerifyCertChain(storeCtx, chain, &tmpChain);
    if (ret != HITLS_X509_SUCCESS) {
        return ret;
    }
    ret = HITLS_X509_VerifyParamAndExt(storeCtx, tmpChain);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LIST_FREE(tmpChain, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
        return ret;
    }
    ret = HITLS_X509_CrlVerify(storeCtx, tmpChain);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LIST_FREE(tmpChain, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
        return ret;
    }
    ret = X509_VerifyChainCert(storeCtx, tmpChain);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LIST_FREE(tmpChain, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
        return ret;
    }
    BSL_LIST_FREE(tmpChain, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
    return HITLS_X509_SUCCESS;
}
