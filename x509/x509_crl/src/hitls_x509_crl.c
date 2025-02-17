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

#include "hitls_x509.h"
#include "bsl_sal.h"
#include "sal_file.h"
#include "securec.h"
#include "hitls_x509_errno.h"
#include "hitls_x509_local.h"
#include "hitls_crl_local.h"
#include "bsl_obj_internal.h"
#include "bsl_pem_internal.h"
#include "bsl_err_internal.h"

#define HITLS_CRL_CTX_SPECIFIC_TAG_EXTENSION 0

BSL_ASN1_TemplateItem g_crlTempl[] = {
    {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 0}, /* x509 */
        {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 1}, /* tbs */
            /* 2: version */
            {BSL_ASN1_TAG_INTEGER, BSL_ASN1_FLAG_DEFAULT, 2},
            /* 2: signature info */
            {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 2},
                {BSL_ASN1_TAG_OBJECT_ID, 0, 3},
                {BSL_ASN1_TAG_ANY, BSL_ASN1_FLAG_OPTIONAL, 3}, // 6
            /* 2: issuer */
            {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_HEADERONLY | BSL_ASN1_FLAG_SAME, 2},
            /* 2: validity */
            {BSL_ASN1_TAG_CHOICE, 0, 2},
            {BSL_ASN1_TAG_CHOICE, BSL_ASN1_FLAG_OPTIONAL, 2},
            /* 2: revoked crl list */
            {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE,
            BSL_ASN1_FLAG_HEADERONLY | BSL_ASN1_FLAG_SAME | BSL_ASN1_FLAG_OPTIONAL, 2},
            /* 2: extension */
            {BSL_ASN1_CLASS_CTX_SPECIFIC | BSL_ASN1_TAG_CONSTRUCTED | HITLS_CRL_CTX_SPECIFIC_TAG_EXTENSION,
            BSL_ASN1_FLAG_OPTIONAL | BSL_ASN1_FLAG_HEADERONLY | BSL_ASN1_FLAG_SAME, 2}, // 11
        {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 1}, /* signAlg */
            {BSL_ASN1_TAG_OBJECT_ID, 0, 2},
            {BSL_ASN1_TAG_ANY, BSL_ASN1_FLAG_OPTIONAL, 2},
        {BSL_ASN1_TAG_BITSTRING, 0, 1} /* sig */
};

typedef enum {
    HITLS_X509_CRL_VERSION_IDX,
    HITLS_X509_CRL_TBS_SIGNALG_OID_IDX,
    HITLS_X509_CRL_TBS_SIGNALG_ANY_IDX,
    HITLS_X509_CRL_ISSUER_IDX,
    HITLS_X509_CRL_BEFORE_VALID_IDX,
    HITLS_X509_CRL_AFTER_VALID_IDX,
    HITLS_X509_CRL_CRL_LIST_IDX,
    HITLS_X509_CRL_EXT_IDX,
    HITLS_X509_CRL_SIGNALG_IDX,
    HITLS_X509_CRL_SIGNALG_ANY_IDX,
    HITLS_X509_CRL_SIGN_IDX,
    HITLS_X509_CRL_MAX_IDX,
} HITLS_X509_CRL_IDX;

int32_t HITLS_X509_CrlTagGetOrCheck(int32_t type, int32_t idx, void *data, void *expVal)
{
    (void) idx;
    switch (type) {
        case BSL_ASN1_TYPE_CHECK_CHOICE_TAG: {
            uint8_t tag = *(uint8_t *) data;
            if ((tag == BSL_ASN1_TAG_UTCTIME) || (tag == BSL_ASN1_TAG_GENERALIZEDTIME)) {
                *(uint8_t *) expVal = tag;
                return BSL_SUCCESS;
            }
            return HITLS_X509_ERR_CHECK_TAG;
        }
        case BSL_ASN1_TYPE_GET_ANY_TAG: {
            BSL_ASN1_Buffer *param = (BSL_ASN1_Buffer *) data;
            BslOidString oidStr = {param->len, (char *)param->buff, 0};
            BslCid cid = BSL_OBJ_GetCIDFromOid(&oidStr);
            if (cid == BSL_CID_UNKNOWN) {
                return HITLS_X509_ERR_GET_ANY_TAG;
            }
            if (cid == BSL_CID_RSASSAPSS) {
                // note: any It can be encoded empty or it can be null
                *(uint8_t *) expVal = BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE;
                return BSL_SUCCESS;
            } else {
                *(uint8_t *) expVal = BSL_ASN1_TAG_NULL; // is null
                return BSL_SUCCESS;
            }
            return HITLS_X509_ERR_GET_ANY_TAG;
        }
        default:
            return HITLS_X509_ERR_INVALID_PARAM;
    }
}

void HITLS_X509_CrlFree(HITLS_X509_Crl *crl)
{
    if (crl == NULL) {
        return;
    }

    int ret = 0;
    BSL_SAL_AtomicDownReferences(&(crl->references), &ret);
    if (ret > 0) {
        return;
    }

    BSL_LIST_FREE(crl->tbs.issuerName, NULL);
    BSL_LIST_FREE(crl->tbs.revokedCerts, NULL);
    BSL_LIST_FREE(crl->tbs.crlExt.extList, NULL);
    BSL_SAL_ReferencesFree(&(crl->references));
    if (crl->isCopy == true) {
        BSL_SAL_FREE(crl->rawData);
    }
    BSL_SAL_Free(crl);
    return;
}

HITLS_X509_Crl *HITLS_X509_CrlNew()
{
    HITLS_X509_Crl *crl = NULL;
    BSL_ASN1_List *issuerName = NULL;
    BSL_ASN1_List *entryList = NULL;
    BSL_ASN1_List *extList = NULL;
    crl = (HITLS_X509_Crl *)BSL_SAL_Calloc(1, sizeof(HITLS_X509_Crl));
    if (crl == NULL) {
        return NULL;
    }
    
    issuerName = BSL_LIST_New(sizeof(HITLS_X509_NameNode));
    if (issuerName == NULL) {
        goto ERR;
    }
    
    entryList = BSL_LIST_New(sizeof(HITLS_X509_CrlEntry));
    if (entryList == NULL) {
        goto ERR;
    }
    extList = BSL_LIST_New(sizeof(HITLS_X509_ExtEntry));
    if (extList == NULL) {
        goto ERR;
    }
    BSL_SAL_ReferencesInit(&(crl->references));
    crl->tbs.issuerName = issuerName;
    crl->tbs.revokedCerts = entryList;
    crl->tbs.crlExt.extList = extList;
    return crl;
ERR:
    BSL_SAL_Free(crl);
    BSL_SAL_Free(issuerName);
    BSL_SAL_Free(entryList);
    return NULL;
}

int32_t HITLS_CRL_ParseExtAsnItem(BSL_ASN1_Buffer *asn, void *param, BSL_ASN1_List *list)
{
    (void) param;
    HITLS_X509_ExtEntry extEntry = {0};
    int32_t ret = HITLS_X509_ParseExtItem(asn, &extEntry);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }

    return HITLS_X509_AddListItemDefault(&extEntry, sizeof(HITLS_X509_ExtEntry), list);
}

int32_t HITLS_CRL_ParseExtSeqof(uint32_t layer, BSL_ASN1_Buffer *asn, void *param, BSL_ASN1_List *list)
{
    if (layer == 1) {
        return HITLS_X509_SUCCESS;
    }
    return HITLS_CRL_ParseExtAsnItem(asn, param, list);
}

int32_t HITLS_X509_ParseCrlExt(BSL_ASN1_Buffer *ext, HITLS_X509_Crl *crl)
{
    uint8_t expTag[] = {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE,
        BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE};
    BSL_ASN1_DecodeListParam listParam = {2, expTag};
    int ret = BSL_ASN1_DecodeListItem(&listParam, ext, &HITLS_CRL_ParseExtSeqof, crl, crl->tbs.crlExt.extList);
    if (ret != BSL_SUCCESS) {
        BSL_LIST_DeleteAll(crl->tbs.crlExt.extList, NULL);
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    return ret;
}

BSL_ASN1_TemplateItem g_crlEntryTempl[] = {
    {BSL_ASN1_TAG_INTEGER, 0, 0},
    {BSL_ASN1_TAG_CHOICE, 0, 0},
    {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_OPTIONAL | BSL_ASN1_FLAG_HEADERONLY, 0}
};

typedef enum {
    HITLS_X509_CRLENTRY_NUM_IDX,
    HITLS_X509_CRLENTRY_TIME_IDX,
    HITLS_X509_CRLENTRY_EXT_IDX,
    HITLS_X509_CRLENTRY_MAX_IDX
} HITLS_X509_CRLENTRY_IDX;

int32_t HITLS_X509_CrlEntryChoiceCheck(int32_t type, int32_t idx, void *data, void *expVal)
{
    (void) idx;
    (void) expVal;
    if (type == BSL_ASN1_TYPE_CHECK_CHOICE_TAG) {
        uint8_t tag = *(uint8_t *) data;
        if ((tag & BSL_ASN1_TAG_UTCTIME) || (tag & BSL_ASN1_TAG_GENERALIZEDTIME)) {
            *(uint8_t *) expVal = tag;
            return BSL_SUCCESS;
        }
        return HITLS_X509_ERR_CHECK_TAG;
    }
    return HITLS_X509_ERR_CHECK_TAG;
}
#define BSL_TIME_REVOKE_TIME_IS_GMT  0x1
int32_t HITLS_CRL_ParseCrlEntry(BSL_ASN1_Buffer *extItem, HITLS_X509_CrlEntry *crlEntry)
{
    uint8_t *temp = extItem->buff;
    uint32_t tempLen = extItem->len;
    BSL_ASN1_Buffer asnArr[HITLS_X509_CRLENTRY_MAX_IDX] = {0};
    BSL_ASN1_Template templ = {g_crlEntryTempl, sizeof(g_crlEntryTempl) / sizeof(g_crlEntryTempl[0])};
    int32_t ret = BSL_ASN1_DecodeTemplate(&templ, &HITLS_X509_CrlEntryChoiceCheck,
        &temp, &tempLen, asnArr, HITLS_X509_CRLENTRY_MAX_IDX);
    if (tempLen != 0) {
        ret = HITLS_X509_ERR_CRL_ENTRY;
    }
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    crlEntry->serialNumber = asnArr[HITLS_X509_CRLENTRY_NUM_IDX];

    ret = BSL_ASN1_DecodePrimitiveItem(&asnArr[HITLS_X509_CRLENTRY_TIME_IDX], &crlEntry->time);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    if (asnArr[HITLS_X509_CRLENTRY_TIME_IDX].tag == BSL_ASN1_TAG_GENERALIZEDTIME) {
        crlEntry->flag |= BSL_TIME_REVOKE_TIME_IS_GMT;
    }
    // optinal
    crlEntry->entryExt = asnArr[HITLS_X509_CRLENTRY_EXT_IDX];
    return ret;
}

int32_t HITLS_CRL_ParseCrlAsnItem(uint32_t layer, BSL_ASN1_Buffer *asn, void *param, BSL_ASN1_List *list)
{
    (void) param;
    (void) layer;
    HITLS_X509_CrlEntry crlEntry = {0};
    int32_t ret = HITLS_CRL_ParseCrlEntry(asn, &crlEntry);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }

    return HITLS_X509_AddListItemDefault(&crlEntry, sizeof(HITLS_X509_CrlEntry), list);
}

int32_t HITLS_X509_ParseCrlList(BSL_ASN1_Buffer *crl, BSL_ASN1_List *list)
{
    // crl is optional
    if (crl->tag == 0) {
        return HITLS_X509_SUCCESS;
    }
    
    uint8_t expTag = (BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE);
    BSL_ASN1_DecodeListParam listParam = {1, &expTag};
    int32_t ret = BSL_ASN1_DecodeListItem(&listParam, crl, &HITLS_CRL_ParseCrlAsnItem, NULL, list);
    if (ret != BSL_SUCCESS) {
        BSL_LIST_DeleteAll(list, NULL);
        return ret;
    }
    return ret;
}

int32_t HITLS_X509_ParseCrlTbs(BSL_ASN1_Buffer *asnArr, HITLS_X509_Crl *crl)
{
    int32_t ret;
    if (asnArr[HITLS_X509_CRL_VERSION_IDX].tag != 0) {
        ret = BSL_ASN1_DecodePrimitiveItem(&asnArr[HITLS_X509_CRL_VERSION_IDX], &crl->tbs.version);
        if (ret != BSL_SUCCESS) {
            BSL_ERR_PUSH_ERROR(ret);
            return ret;
        }
    } else {
        crl->tbs.version = 0;
    }

    // sign alg
    ret = HITLS_X509_ParseSignAlgInfo(&asnArr[HITLS_X509_CRL_TBS_SIGNALG_OID_IDX],
        &asnArr[HITLS_X509_CRL_TBS_SIGNALG_ANY_IDX], &crl->tbs.signAlgId);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }

    // issuer name
    ret = HITLS_X509_ParseNameList(&asnArr[HITLS_X509_CRL_ISSUER_IDX], crl->tbs.issuerName);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    
    // validity
    ret = HITLS_X509_ParseTime(&asnArr[HITLS_X509_CRL_BEFORE_VALID_IDX], &asnArr[HITLS_X509_CRL_AFTER_VALID_IDX],
        &crl->tbs.validTime);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto ERR;
    }

    // crl list
    ret = HITLS_X509_ParseCrlList(&asnArr[HITLS_X509_CRL_CRL_LIST_IDX], crl->tbs.revokedCerts);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    // ext
    ret = HITLS_X509_ParseCrlExt(&asnArr[HITLS_X509_CRL_EXT_IDX], crl);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto ERR;
    }

    return ret;
ERR:

    BSL_LIST_DeleteAll(crl->tbs.issuerName, NULL);
    BSL_LIST_DeleteAll(crl->tbs.revokedCerts, NULL);
    return ret;
}

static void X509_EncodeCrlValidTime(HITLS_X509_ValidTime *crlTime, BSL_ASN1_Buffer *validTime)
{
    validTime[0].tag = (crlTime->flag & BSL_TIME_BEFORE_IS_UTC) ? BSL_ASN1_TAG_UTCTIME : BSL_ASN1_TAG_GENERALIZEDTIME;
    validTime[0].len = sizeof(BSL_TIME);
    validTime[0].buff = (uint8_t *)&(crlTime->start);

    validTime[1].tag = (crlTime->flag & BSL_TIME_AFTER_IS_UTC) ? BSL_ASN1_TAG_UTCTIME : BSL_ASN1_TAG_GENERALIZEDTIME;
    if (crlTime->flag & BSL_TIME_AFTER_SET) {
        validTime[1].len = sizeof(BSL_TIME);
        validTime[1].buff = (uint8_t *)&(crlTime->end);
    } else {
        validTime[1].len = 0;
        validTime[1].buff = NULL;
    }
    return;
}

static void X509_EncodeCrlEntry(HITLS_X509_CrlEntry *crlEntry, BSL_ASN1_Buffer *asnBuf)
{
    asnBuf[0].tag = crlEntry->serialNumber.tag;
    asnBuf[0].buff = crlEntry->serialNumber.buff;
    asnBuf[0].len = crlEntry->serialNumber.len;
    asnBuf[1].tag = (crlEntry->flag & BSL_TIME_REVOKE_TIME_IS_GMT) ?
        BSL_ASN1_TAG_GENERALIZEDTIME : BSL_ASN1_TAG_UTCTIME;
    asnBuf[1].buff = (uint8_t *)&(crlEntry->time);
    asnBuf[1].len = sizeof(BSL_TIME);
    asnBuf[2].tag = crlEntry->entryExt.tag; // 2 : extension
    asnBuf[2].buff = crlEntry->entryExt.buff; // 2 : extension
    asnBuf[2].len = crlEntry->entryExt.len; // 2 : extension
}

#define X509_CRLENTRY_ELEM_NUMBER 3
int32_t HITLS_X509_EncodeRevokeCrlList(BSL_ASN1_List *crlList, BSL_ASN1_Buffer *revokeBuf)
{
    int32_t count = BSL_LIST_COUNT(crlList);
    if (count == 0) {
        revokeBuf->buff = NULL;
        revokeBuf->len = 0;
        revokeBuf->tag = BSL_ASN1_TAG_SEQUENCE;
        return HITLS_X509_SUCCESS;
    }
    BSL_ASN1_Buffer *asnBuf = BSL_SAL_Malloc(count * sizeof(BSL_ASN1_Buffer) * X509_CRLENTRY_ELEM_NUMBER);
    if (asnBuf == NULL) {
        BSL_ERR_PUSH_ERROR(BSL_MALLOC_FAIL);
        return BSL_MALLOC_FAIL;
    }
    HITLS_X509_CrlEntry *crlEntry = NULL;
    uint32_t iter = 0;
    for (crlEntry = BSL_LIST_GET_FIRST(crlList); crlEntry != NULL; crlEntry = BSL_LIST_GET_NEXT(crlList)) {
        X509_EncodeCrlEntry(crlEntry, &asnBuf[iter]);
        iter += X509_CRLENTRY_ELEM_NUMBER;
    }
    BSL_ASN1_TemplateItem crlEntryTempl[] = {
        {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_SAME | BSL_ASN1_FLAG_OPTIONAL, 0},
            {BSL_ASN1_TAG_INTEGER, 0, 1},
            {BSL_ASN1_TAG_CHOICE, 0, 1},
            {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_OPTIONAL, 1}
    };
    BSL_ASN1_Template templ = {crlEntryTempl, sizeof(crlEntryTempl) / sizeof(crlEntryTempl[0])};
    int32_t ret = BSL_ASN1_EncodeListItem(BSL_ASN1_TAG_SEQUENCE, count, &templ, asnBuf, iter, revokeBuf);
    BSL_SAL_Free(asnBuf);
    return ret;
}

BSL_ASN1_TemplateItem g_crlTbsTempl[] = {
    /* 1: version */
    {BSL_ASN1_TAG_INTEGER, BSL_ASN1_FLAG_DEFAULT, 0},
    /* 2: signature info */
    {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_HEADERONLY, 0},
    /* 3: issuer */
    {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_HEADERONLY | BSL_ASN1_FLAG_SAME, 0},
    /* 4-5: validity */
    {BSL_ASN1_TAG_CHOICE, 0, 0},
    {BSL_ASN1_TAG_CHOICE, BSL_ASN1_FLAG_OPTIONAL, 0},
    /* 6: revoked crl list */
    {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE,
        BSL_ASN1_FLAG_HEADERONLY | BSL_ASN1_FLAG_SAME | BSL_ASN1_FLAG_OPTIONAL, 0},
    /* 7: extension */
    {BSL_ASN1_CLASS_CTX_SPECIFIC | BSL_ASN1_TAG_CONSTRUCTED | HITLS_CRL_CTX_SPECIFIC_TAG_EXTENSION,
        BSL_ASN1_FLAG_OPTIONAL | BSL_ASN1_FLAG_HEADERONLY | BSL_ASN1_FLAG_SAME, 0}, // 11
};

int32_t HITLS_X509_EncodeCrlExt(HITLS_X509_CrlExt *crlExt, BSL_ASN1_Buffer *ext)
{
    return HITLS_X509_EncodeExt(
        BSL_ASN1_CLASS_CTX_SPECIFIC | BSL_ASN1_TAG_CONSTRUCTED | HITLS_CRL_CTX_SPECIFIC_TAG_EXTENSION,
        crlExt->extList, ext);
}
/**
 * RFC 5280 sec 5.1.2.1
 * This optional field describes the version of the encoded CRL.  When
 * extensions are used, as required by this profile, this field MUST be
 * present and MUST specify version 2 (the integer value is 1).
 */
static void X509_EncodeVersion(uint8_t *version, BSL_ASN1_Buffer *asn)
{
    if (*version == 1) {
        asn->tag = BSL_ASN1_TAG_INTEGER;
        asn->len = 1;
        asn->buff = version;
    } else {
        asn->tag = BSL_ASN1_TAG_INTEGER;
        asn->len = 0;
        asn->buff = NULL;
    }
}

#define X509_CRLTBS_ELEM_NUMBER 7
int32_t HITLS_X509_EncodeCrlTbs(HITLS_X509_CrlTbs *crlTbs, BSL_ASN1_Buffer *asn)
{
    BSL_ASN1_Buffer asnArr[X509_CRLTBS_ELEM_NUMBER] = {0};
    BSL_ASN1_Buffer *revokeBuf = NULL;
    BSL_ASN1_Buffer *crlExt = NULL;
    uint8_t version = (uint8_t)crlTbs->version;
    X509_EncodeVersion(&version, asnArr);
    BSL_ASN1_Buffer *signAlgAsn = &asnArr[1];
    int32_t ret = HITLS_X509_EncodeSignAlgInfo(&crlTbs->signAlgId, signAlgAsn);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    BSL_ASN1_Buffer *issuerAsn = &asnArr[2]; // 2 is issuer name
    ret = HITLS_X509_EncodeNameList(crlTbs->issuerName, issuerAsn);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto EXIT;
    }
    X509_EncodeCrlValidTime(&crlTbs->validTime, &asnArr[3]); // 3 is valid time
    revokeBuf = &asnArr[5]; // 5 is revoke list
    ret = HITLS_X509_EncodeRevokeCrlList(crlTbs->revokedCerts, revokeBuf);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto EXIT;
    }
    crlExt = &asnArr[6]; // 6 is crl extension
    ret = HITLS_X509_EncodeCrlExt(&(crlTbs->crlExt), crlExt);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto EXIT;
    }
    BSL_ASN1_Template templ = {g_crlTbsTempl, sizeof(g_crlTbsTempl) / sizeof(g_crlTbsTempl[0])};
    ret = BSL_ASN1_EncodeTemplate(&templ, asnArr, X509_CRLTBS_ELEM_NUMBER, &(asn->buff), &(asn->len));
    if (ret != HITLS_X509_SUCCESS) {
        goto EXIT;
    }
    asn->tag = BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE;
EXIT:
    BSL_SAL_Free(signAlgAsn->buff);
    BSL_SAL_Free(issuerAsn->buff);
    BSL_SAL_Free(revokeBuf->buff);
    BSL_SAL_Free(crlExt->buff);
    return ret;
}

#define X509_CRL_ELEM_NUMBER 3
int32_t HITLS_X509_EncodeAsn1Crl(HITLS_X509_Crl *crl, uint8_t **encode, uint32_t *encodeLen)
{
    BSL_ASN1_Buffer asnArr[X509_CRL_ELEM_NUMBER] = {0};
    int32_t ret = HITLS_X509_EncodeCrlTbs(&crl->tbs, asnArr);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    ret = HITLS_X509_EncodeSignAlgInfo(&crl->signAlgId, &asnArr[1]);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_SAL_Free(asnArr[0].buff);
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    asnArr[2].tag = BSL_ASN1_TAG_BITSTRING; // 2 is signAlg
    asnArr[2].len = sizeof(BSL_ASN1_BitString); // 2 is signAlg
    asnArr[2].buff = (uint8_t *)&(crl->signature); // 2 is signAlg
    BSL_ASN1_TemplateItem crlTempl[] = {
        {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 0}, /* x509 */
            {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_HEADERONLY, 1}, /* tbs */
            {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_HEADERONLY, 1}, /* signAlg */
            {BSL_ASN1_TAG_BITSTRING, 0, 1} /* sig */
    };
    BSL_ASN1_Template templ = {crlTempl, sizeof(crlTempl) / sizeof(crlTempl[0])};
    ret = BSL_ASN1_EncodeTemplate(&templ, asnArr, X509_CRL_ELEM_NUMBER, encode, encodeLen);
    BSL_SAL_Free(asnArr[0].buff);
    BSL_SAL_Free(asnArr[1].buff);
    return ret;
}

int32_t HITLS_X509_EncodePemCrl(HITLS_X509_Crl *crl, uint8_t **encode, uint32_t *encodeLen)
{
    uint8_t *asn1 = NULL;
    uint32_t asn1Len;
    int32_t ret = HITLS_X509_EncodeAsn1Crl(crl, &asn1, &asn1Len);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    BSL_PEM_Symbol symbol = {BSL_PEM_CRL_BEGIN_STR, BSL_PEM_CRL_END_STR};
    ret = BSL_PEM_EncodeAsn1ToPem(asn1, asn1Len, &symbol, (char **)encode, encodeLen);
    BSL_SAL_Free(asn1);
    return ret;
}

static int32_t X509_CrlCheckValid(HITLS_X509_Crl *crl)
{
    if (crl->tbs.crlExt.extList != NULL && BSL_LIST_COUNT(crl->tbs.crlExt.extList) != 0) {
        if (crl->tbs.version != 1) {
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CRL_INACCRACY_VERSION);
            return HITLS_X509_ERR_CRL_INACCRACY_VERSION;
        }
    }

    return HITLS_X509_SUCCESS;
}

int32_t HITLS_X509_CrlGenBuff(int32_t format, HITLS_X509_Crl *crl, uint8_t **encode, uint32_t *encodeLen)
{
    if (crl == NULL || encode == NULL || encodeLen == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    int32_t ret = X509_CrlCheckValid(crl);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    if (format == BSL_FORMAT_PEM) {
        return HITLS_X509_EncodePemCrl(crl, encode, encodeLen);
    }
    return HITLS_X509_EncodeAsn1Crl(crl, encode, encodeLen);
}

int32_t HITLS_X509_CrlGenFile(int32_t format, HITLS_X509_Crl *crl, const char *path)
{
    if (path == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    uint8_t *encode = NULL;
    uint32_t encodeLen;
    int32_t ret = HITLS_X509_CrlGenBuff(format, crl, &encode, &encodeLen);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    ret = BSL_SAL_WriteFile(path, encode, encodeLen);
    BSL_SAL_Free(encode);
    return ret;
}

int32_t HITLS_X509_ParseAsn1Crl(bool isCopy, uint8_t **encode, uint32_t *encodeLen, HITLS_X509_Crl *crl)
{
    uint8_t *temp = *encode;
    uint32_t tempLen = *encodeLen;
    crl->isCopy = isCopy;
    // template parse
    BSL_ASN1_Buffer asnArr[HITLS_X509_CRL_MAX_IDX] = {0};
    BSL_ASN1_Template templ = {g_crlTempl, sizeof(g_crlTempl) / sizeof(g_crlTempl[0])};
    int32_t ret = BSL_ASN1_DecodeTemplate(&templ, HITLS_X509_CrlTagGetOrCheck,
        &temp, &tempLen, asnArr, HITLS_X509_CRL_MAX_IDX);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    // parse tbs raw data
    ret = HITLS_X509_ParseTbsRawData(*encode, *encodeLen, &crl->tbs.tbsRawData, &crl->tbs.tbsRawDataLen);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    // parse tbs
    ret = HITLS_X509_ParseCrlTbs(asnArr, crl);
    if (ret != HITLS_X509_SUCCESS) {
        return ret;
    }
    // parse sign alg
    ret = HITLS_X509_ParseSignAlgInfo(&asnArr[HITLS_X509_CRL_SIGNALG_IDX],
        &asnArr[HITLS_X509_CRL_SIGNALG_ANY_IDX], &crl->signAlgId);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto ERR;
    }
    // parse signature
    ret = BSL_ASN1_DecodePrimitiveItem(&asnArr[HITLS_X509_CRL_SIGN_IDX], &crl->signature);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto ERR;
    }

    crl->rawData = *encode;
    crl->rawDataLen = *encodeLen - tempLen;
    *encode = temp;
    *encodeLen = tempLen;
    return HITLS_X509_SUCCESS;
ERR:
    BSL_LIST_DeleteAll(crl->tbs.issuerName, NULL);
    BSL_LIST_DeleteAll(crl->tbs.revokedCerts, NULL);
    BSL_LIST_DeleteAll(crl->tbs.crlExt.extList, NULL);
    return ret;
}

int32_t HITLS_X509_CrlMulParseBuff(int32_t format, BSL_Buffer *encode, HITLS_X509_List **crllist)
{
    if (encode == NULL || encode->data == NULL || encode->dataLen == 0 || crllist == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }

    X509_ParseFuncCbk crlCbk = {
        (HITLS_X509_Asn1Parse)HITLS_X509_ParseAsn1Crl,
        (HITLS_X509_New)HITLS_X509_CrlNew,
        (HITLS_X509_Free)HITLS_X509_CrlFree,
    };
    HITLS_X509_List *list = BSL_LIST_New(sizeof(HITLS_X509_Crl));
    if (list == NULL) {
        BSL_ERR_PUSH_ERROR(BSL_MALLOC_FAIL);
        return BSL_MALLOC_FAIL;
    }
    int32_t ret = HITLS_X509_ParseX509(format, encode, false, &crlCbk, list);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LIST_FREE(list, (BSL_LIST_PFUNC_FREE)HITLS_X509_CrlFree);
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    *crllist = list;
    return HITLS_X509_SUCCESS;
}

int32_t HITLS_X509_CrlParseBuff(int32_t format, BSL_Buffer *encode, HITLS_X509_Crl **crl)
{
    HITLS_X509_List *list = NULL;
    if (crl == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    int32_t ret = HITLS_X509_CrlMulParseBuff(format, encode, &list);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    HITLS_X509_Crl *tmp = BSL_LIST_GET_FIRST(list);
    int ref;
    ret = HITLS_X509_CrlCtrl(tmp, HITLS_X509_CRL_REF_UP, &ref, sizeof(int));
    BSL_LIST_FREE(list, (BSL_LIST_PFUNC_FREE)HITLS_X509_CrlFree);
    if (ret != HITLS_X509_SUCCESS) {
        return ret;
    }
    *crl = tmp;
    return HITLS_X509_SUCCESS;
}

int32_t HITLS_X509_CrlParseFile(int32_t format, const char *path, HITLS_X509_Crl **crl)
{
    uint8_t *data = NULL;
    uint32_t dataLen = 0;
    int32_t ret = BSL_SAL_ReadFile(path, &data, &dataLen);
    if (ret != BSL_SUCCESS) {
        return ret;
    }

    BSL_Buffer encode = {data, dataLen};
    ret = HITLS_X509_CrlParseBuff(format, &encode, crl);
    BSL_SAL_Free(data);
    return ret;
}

int32_t HITLS_X509_CrlMulParseFile(int32_t format, const char *path, HITLS_X509_List **crllist)
{
    uint8_t *data = NULL;
    uint32_t dataLen = 0;
    int32_t ret = BSL_SAL_ReadFile(path, &data, &dataLen);
    if (ret != BSL_SUCCESS) {
        return ret;
    }

    BSL_Buffer encode = {data, dataLen};
    ret = HITLS_X509_CrlMulParseBuff(format, &encode, crllist);
    BSL_SAL_Free(data);
    return ret;
}

static int32_t X509_CrlRefUp(HITLS_X509_Crl *crl, int32_t *val, int32_t valLen)
{
    if (val == NULL || valLen != sizeof(int32_t)) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    return BSL_SAL_AtomicUpReferences(&crl->references, val);
}

int32_t HITLS_X509_CrlCtrl(HITLS_X509_Crl *crl, int32_t cmd, void *val, int32_t valLen)
{
    if (crl == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    switch (cmd) {
        case HITLS_X509_CRL_REF_UP:
            return X509_CrlRefUp(crl, val, valLen);
        default:
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
            return HITLS_X509_ERR_INVALID_PARAM;
    }
}