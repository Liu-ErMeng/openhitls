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

#include <stdio.h>
#include "securec.h"
#include "hitls_x509.h"
#include "bsl_sal.h"
#include "sal_file.h"
#include "sal_time.h"
#include "bsl_log_internal.h"
#include "bsl_binlog_id.h"
#include "bsl_log.h"
#include "bsl_obj_internal.h"
#include "hitls_x509_errno.h"
#include "hitls_x509_local.h"
#include "crypt_eal_encode.h"
#include "crypt_encode.h"
#include "crypt_errno.h"
#include "crypt_types.h"
#include "crypt_eal_pkey.h"
#include "crypt_eal_md.h"
#include "bsl_pem_internal.h"
#include "bsl_err_internal.h"
#include "hitls_csr_local.h"
#include "hitls_cert_local.h"
#include "crypt_encode.h"

#define HITLS_CERT_CTX_SPECIFIC_TAG_VER       0
#define HITLS_CERT_CTX_SPECIFIC_TAG_ISSUERID  1
#define HITLS_CERT_CTX_SPECIFIC_TAG_SUBJECTID 2
#define HITLS_CERT_CTX_SPECIFIC_TAG_EXTENSION 3
#define MAX_DN_STR_LEN 256
#define PRINT_TIME_MAX_SIZE 32

typedef enum {
    HITLS_X509_ISSUER_DN_NAME,
    HITLS_X509_SUBJECT_DN_NAME,
} DISTINCT_NAME_TYPE;

typedef enum {
    HITLS_X509_BEFORE_TIME,
    HITLS_X509_AFTER_TIME,
} X509_TIME_TYPE;

BSL_ASN1_TemplateItem g_certTempl[] = {
    {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 0}, /* x509 */
        {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 1}, /* tbs */
            /* 2: version */
            {BSL_ASN1_CLASS_CTX_SPECIFIC | BSL_ASN1_TAG_CONSTRUCTED | HITLS_CERT_CTX_SPECIFIC_TAG_VER,
            BSL_ASN1_FLAG_DEFAULT, 2},
                {BSL_ASN1_TAG_INTEGER, 0, 3},
            /* 2: serial number */
            {BSL_ASN1_TAG_INTEGER, 0, 2},
            /* 2: signature info */
            {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 2},
                {BSL_ASN1_TAG_OBJECT_ID, 0, 3},
                {BSL_ASN1_TAG_ANY, BSL_ASN1_FLAG_OPTIONAL, 3}, // 8
            /* 2: issuer */
            {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_HEADERONLY | BSL_ASN1_FLAG_SAME, 2},
            /* 2: validity */
            {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 2},
                {BSL_ASN1_TAG_CHOICE, 0, 3},
                {BSL_ASN1_TAG_CHOICE, 0, 3}, // 12
            /* 2: subject ref: issuer */
            {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_HEADERONLY | BSL_ASN1_FLAG_SAME, 2},
            /* 2: subject public key info ref signature info */
            {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_HEADERONLY, 2},
            /* 2: issuer id, subject id */
            {BSL_ASN1_CLASS_CTX_SPECIFIC | HITLS_CERT_CTX_SPECIFIC_TAG_ISSUERID, BSL_ASN1_FLAG_OPTIONAL, 2},
            {BSL_ASN1_CLASS_CTX_SPECIFIC | HITLS_CERT_CTX_SPECIFIC_TAG_SUBJECTID, BSL_ASN1_FLAG_OPTIONAL, 2},
            /* 2: extension */
            {BSL_ASN1_CLASS_CTX_SPECIFIC | BSL_ASN1_TAG_CONSTRUCTED | HITLS_CERT_CTX_SPECIFIC_TAG_EXTENSION,
            BSL_ASN1_FLAG_OPTIONAL | BSL_ASN1_FLAG_HEADERONLY | BSL_ASN1_FLAG_SAME, 2}, // 17
        {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 1}, /* signAlg */
            {BSL_ASN1_TAG_OBJECT_ID, 0, 2},
            {BSL_ASN1_TAG_ANY, BSL_ASN1_FLAG_OPTIONAL, 2}, // 20
        {BSL_ASN1_TAG_BITSTRING, 0, 1} /* sig */
};

typedef enum {
    HITLS_X509_CERT_VERSION_IDX = 0,
    HITLS_X509_CERT_SERIAL_IDX = 1,
    HITLS_X509_CERT_TBS_SIGNALG_OID_IDX = 2,
    HITLS_X509_CERT_TBS_SIGNALG_ANY_IDX = 3,
    HITLS_X509_CERT_ISSUER_IDX = 4,
    HITLS_X509_CERT_BEFORE_VALID_IDX = 5,
    HITLS_X509_CERT_AFTER_VALID_IDX = 6,
    HITLS_X509_CERT_SUBJECT_IDX = 7,
    HITLS_X509_CERT_SUBKEYINFO_IDX = 8,
    HITLS_X509_CERT_ISSUERID_IDX = 9,
    HITLS_X509_CERT_SUBJECTID_IDX = 10,
    HITLS_X509_CERT_EXT_IDX = 11,
    HITLS_X509_CERT_SIGNALG_IDX = 12,
    HITLS_X509_CERT_SIGNALG_ANY_IDX = 13,
    HITLS_X509_CERT_SIGN_IDX = 14,
    HITLS_X509_CERT_MAX_IDX = 15,
} HITLS_X509_CERT_IDX;

#define X509_ASN1_START_TIME_IDX 10
#define X509_ASN1_END_TIME_IDX 11

#define X509_ASN1_TBS_SIGNALG_ANY 7
#define X509_ASN1_SIGNALG_ANY 19

int32_t HITLS_X509_CertTagGetOrCheck(int32_t type, int32_t idx, void *data, void *expVal)
{
    switch (type) {
        case BSL_ASN1_TYPE_CHECK_CHOICE_TAG: {
            if (idx == X509_ASN1_START_TIME_IDX || idx == X509_ASN1_END_TIME_IDX) {
                uint8_t tag = *(uint8_t *) data;
                if ((tag == BSL_ASN1_TAG_UTCTIME) || (tag == BSL_ASN1_TAG_GENERALIZEDTIME)) {
                    *(uint8_t *) expVal = tag;
                    return BSL_SUCCESS;
                }
            }
            return HITLS_X509_ERR_CHECK_TAG;
        }
        case BSL_ASN1_TYPE_GET_ANY_TAG: {
            if (idx == X509_ASN1_TBS_SIGNALG_ANY || idx == X509_ASN1_SIGNALG_ANY) {
                BSL_ASN1_Buffer *param = (BSL_ASN1_Buffer *) data;
                BslOidString oidStr = {param->len, (char *)param->buff, 0};
                BslCid cid = BSL_OBJ_GetCIDFromOid(&oidStr);
                if (cid == BSL_CID_UNKNOWN) {
                    return HITLS_X509_ERR_GET_ANY_TAG;
                }
                if (cid == BSL_CID_RSASSAPSS) {
                    // note: any can be encoded empty null
                    *(uint8_t *)expVal = BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE;
                    return BSL_SUCCESS;
                } else {
                    *(uint8_t *)expVal = BSL_ASN1_TAG_NULL; // is null
                    return BSL_SUCCESS;
                }
            }
            return HITLS_X509_ERR_GET_ANY_TAG;
        }
        default:
            return HITLS_X509_ERR_INVALID_PARAM;
    }
}

void HITLS_X509_CertFree(HITLS_X509_Cert *cert)
{
    if (cert == NULL) {
        return;
    }

    int ret = 0;
    BSL_SAL_AtomicDownReferences(&(cert->references), &ret);
    if (ret > 0) {
        return;
    }

    if (cert->flag == HITLS_X509_CERT_GEN_FLAG) {
        BSL_LIST_FREE(cert->tbs.ext.list, (BSL_LIST_PFUNC_FREE)HITLS_X509_ExtEntryFree);
        BSL_SAL_FREE(cert->tbs.serialNum.buff);
        BSL_SAL_FREE(cert->tbs.tbsRawData);
        BSL_SAL_FREE(cert->signature.buff);
        BSL_LIST_FREE(cert->tbs.issuerName, (BSL_LIST_PFUNC_FREE)HITLS_X509_FreeNameNode);
        BSL_LIST_FREE(cert->tbs.subjectName, (BSL_LIST_PFUNC_FREE)HITLS_X509_FreeNameNode);
    } else {
        BSL_LIST_FREE(cert->tbs.ext.list, NULL);
        BSL_LIST_FREE(cert->tbs.issuerName, NULL);
        BSL_LIST_FREE(cert->tbs.subjectName, NULL);
    }
    BSL_SAL_FREE(cert->rawData);
    CRYPT_EAL_PkeyFreeCtx(cert->tbs.ealPubKey);
    CRYPT_EAL_PkeyFreeCtx(cert->ealPrivKey);
    BSL_SAL_ReferencesFree(&(cert->references));
    BSL_SAL_Free(cert);
    return;
}

HITLS_X509_Cert *HITLS_X509_CertNew(void)
{
    HITLS_X509_Cert *cert = NULL;
    BSL_ASN1_List *issuerName = NULL;
    BSL_ASN1_List *subjectName = NULL;
    BSL_ASN1_List *extList = NULL;
    cert = (HITLS_X509_Cert *)BSL_SAL_Calloc(1, sizeof(HITLS_X509_Cert));
    if (cert == NULL) {
        return NULL;
    }

    issuerName = BSL_LIST_New(sizeof(HITLS_X509_NameNode));
    if (issuerName == NULL) {
        goto ERR;
    }

    subjectName = BSL_LIST_New(sizeof(HITLS_X509_NameNode));
    if (subjectName == NULL) {
        goto ERR;
    }
    extList = BSL_LIST_New(sizeof(HITLS_X509_ExtEntry));
    if (extList == NULL) {
        goto ERR;
    }
    BSL_SAL_ReferencesInit(&(cert->references));
    cert->tbs.issuerName = issuerName;
    cert->tbs.subjectName = subjectName;
    cert->tbs.ext.list = extList;
    cert->tbs.ext.maxPathLen = -1;
    cert->flag = HITLS_X509_CERT_GEN_FLAG;
    return cert;
ERR:
    BSL_SAL_Free(cert);
    BSL_SAL_Free(issuerName);
    BSL_SAL_Free(subjectName);
    return NULL;
}

int32_t HITLS_X509_ParseCertTbs(BSL_ASN1_Buffer *asnArr, HITLS_X509_Cert *cert)
{
    int32_t ret;
    // version: default is 0
    if (asnArr[HITLS_X509_CERT_VERSION_IDX].tag != 0) {
        ret = BSL_ASN1_DecodePrimitiveItem(&asnArr[HITLS_X509_CERT_VERSION_IDX], &cert->tbs.version);
        if (ret != BSL_SUCCESS) {
            BSL_ERR_PUSH_ERROR(ret);
            return ret;
        }
    }

    // serialNum
    cert->tbs.serialNum = asnArr[HITLS_X509_CERT_SERIAL_IDX];

    // sign alg
    ret = HITLS_X509_ParseSignAlgInfo(&asnArr[HITLS_X509_CERT_TBS_SIGNALG_OID_IDX],
        &asnArr[HITLS_X509_CERT_TBS_SIGNALG_ANY_IDX], &cert->tbs.signAlgId);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }

    // issuer name
    ret = HITLS_X509_ParseNameList(&asnArr[HITLS_X509_CERT_ISSUER_IDX], cert->tbs.issuerName);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }

    // validity
    ret = HITLS_X509_ParseTime(&asnArr[HITLS_X509_CERT_BEFORE_VALID_IDX], &asnArr[HITLS_X509_CERT_AFTER_VALID_IDX],
        &cert->tbs.validTime);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto ERR;
    }

    // subject name
    ret = HITLS_X509_ParseNameList(&asnArr[HITLS_X509_CERT_SUBJECT_IDX], cert->tbs.subjectName);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto ERR;
    }

    // subject public key info
    ret = CRYPT_EAL_ParseAsn1SubPubkey(asnArr[HITLS_X509_CERT_SUBKEYINFO_IDX].buff,
        asnArr[HITLS_X509_CERT_SUBKEYINFO_IDX].len, &cert->tbs.ealPubKey, false);
    if (ret != CRYPT_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto ERR;
    }

    // ext
    ret = HITLS_X509_ParseExt(&asnArr[HITLS_X509_CERT_EXT_IDX], &cert->tbs.ext);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto ERR;
    }

    return ret;
ERR:
    if (cert->tbs.ealPubKey != NULL) {
        CRYPT_EAL_PkeyFreeCtx(cert->tbs.ealPubKey);
        cert->tbs.ealPubKey = NULL;
    }
    BSL_LIST_DeleteAll(cert->tbs.issuerName, NULL);
    BSL_LIST_DeleteAll(cert->tbs.subjectName, NULL);
    return ret;
}

int32_t HITLS_X509_ParseAsn1Cert(bool isCopy, uint8_t **encode, uint32_t *encodeLen, HITLS_X509_Cert *cert)
{
    uint8_t *temp = *encode;
    uint32_t tempLen = *encodeLen;
    if (isCopy) {
        cert->flag = HITLS_X509_CERT_PARSE_FLAG;
    }
    // template parse
    BSL_ASN1_Buffer asnArr[HITLS_X509_CERT_MAX_IDX] = {0};
    BSL_ASN1_Template templ = {g_certTempl, sizeof(g_certTempl) / sizeof(g_certTempl[0])};
    int32_t ret = BSL_ASN1_DecodeTemplate(&templ, HITLS_X509_CertTagGetOrCheck,
        &temp, &tempLen, asnArr, HITLS_X509_CERT_MAX_IDX);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    // parse tbs raw data
    ret = HITLS_X509_ParseTbsRawData(*encode, *encodeLen, &cert->tbs.tbsRawData, &cert->tbs.tbsRawDataLen);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    // parse tbs
    ret = HITLS_X509_ParseCertTbs(asnArr, cert);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LOG_BINLOG_FIXLEN(BINLOG_ID05065, BSL_LOG_LEVEL_ERR, BSL_LOG_BINLOG_TYPE_RUN,
            "cert: failed to parse tbs.", 0, 0, 0, 0);
        return ret;
    }
    // parse sign alg
    ret = HITLS_X509_ParseSignAlgInfo(&asnArr[HITLS_X509_CERT_SIGNALG_IDX],
        &asnArr[HITLS_X509_CERT_SIGNALG_ANY_IDX], &cert->signAlgId);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto ERR;
    }
    // parse signature
    ret = BSL_ASN1_DecodePrimitiveItem(&asnArr[HITLS_X509_CERT_SIGN_IDX], &cert->signature);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        goto ERR;
    }

    cert->rawData = *encode;
    cert->rawDataLen = *encodeLen - tempLen;
    *encode = temp;
    *encodeLen = tempLen;
    return HITLS_X509_SUCCESS;
ERR:
    CRYPT_EAL_PkeyFreeCtx(cert->tbs.ealPubKey);
    cert->tbs.ealPubKey = NULL;
    BSL_LIST_DeleteAll(cert->tbs.issuerName, NULL);
    BSL_LIST_DeleteAll(cert->tbs.subjectName, NULL);
    BSL_LIST_DeleteAll(cert->tbs.ext.list, NULL);
    return ret;
}


int32_t HITLS_X509_CertMulParseBuff(int32_t format, BSL_Buffer *encode, HITLS_X509_List **certlist)
{
    int32_t ret;
    if (encode == NULL || encode->data == NULL || encode->dataLen == 0 || certlist == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    X509_ParseFuncCbk certCbk = {
        (HITLS_X509_Asn1Parse)HITLS_X509_ParseAsn1Cert,
        (HITLS_X509_New)HITLS_X509_CertNew,
        (HITLS_X509_Free)HITLS_X509_CertFree
    };
    HITLS_X509_List *list = BSL_LIST_New(sizeof(HITLS_X509_Cert));
    if (list == NULL) {
        BSL_ERR_PUSH_ERROR(BSL_MALLOC_FAIL);
        return BSL_MALLOC_FAIL;
    }

    ret = HITLS_X509_ParseX509(format, encode, true, &certCbk, list);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LOG_BINLOG_FIXLEN(BINLOG_ID05065, BSL_LOG_LEVEL_ERR, BSL_LOG_BINLOG_TYPE_RUN,
            "cert: failed to parse the x509 cert.", 0, 0, 0, 0);
        BSL_LIST_FREE(list, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    *certlist = list;
    return ret;
}

int32_t HITLS_X509_CertParseBuff(int32_t format, BSL_Buffer *encode, HITLS_X509_Cert **cert)
{
    HITLS_X509_List *list = NULL;
    if (cert == NULL || *cert != NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    int32_t ret = HITLS_X509_CertMulParseBuff(format, encode, &list);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    HITLS_X509_Cert *tmp = BSL_LIST_GET_FIRST(list);
    int ref;
    ret = HITLS_X509_CertCtrl(tmp, HITLS_X509_REF_UP, &ref, sizeof(int));
    BSL_LIST_FREE(list, (BSL_LIST_PFUNC_FREE)HITLS_X509_CertFree);
    if (ret != HITLS_X509_SUCCESS) {
        return ret;
    }
    *cert = tmp;
    return HITLS_X509_SUCCESS;
}

int32_t HITLS_X509_CertParseFile(int32_t format, const char *path, HITLS_X509_Cert **cert)
{
    uint8_t *data = NULL;
    uint32_t dataLen = 0;
    int32_t ret = BSL_SAL_ReadFile(path, &data, &dataLen);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }

    BSL_Buffer encode = {data, dataLen};
    ret = HITLS_X509_CertParseBuff(format, &encode, cert);
    BSL_SAL_Free(data);
    return ret;
}

int32_t HITLS_X509_CertMulParseFile(int32_t format, const char *path, HITLS_X509_List **certlist)
{
    uint8_t *data = NULL;
    uint32_t dataLen = 0;
    int32_t ret = BSL_SAL_ReadFile(path, &data, &dataLen);
    if (ret != BSL_SUCCESS) {
        return ret;
    }

    BSL_Buffer encode = {data, dataLen};
    ret = HITLS_X509_CertMulParseBuff(format, &encode, certlist);
    BSL_SAL_Free(data);
    return ret;
}

static int32_t X509_KeyUsageCheck(HITLS_X509_Cert *cert, bool *val, int32_t valLen, uint64_t exp)
{
    if (val == NULL || valLen != sizeof(bool)) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    *val = (cert->tbs.ext.keyUsage & exp);
    return HITLS_X509_SUCCESS;
}

/* RFC2253 https://www.rfc-editor.org/rfc/rfc2253 */
static int32_t X509GetPrintSNStr(const BSL_ASN1_Buffer *nameType, char *buff, int32_t buffLen, int32_t *usedLen)
{
    if (nameType == NULL || nameType->buff == NULL || nameType->len == 0) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }

    BslOidString oid = {
        .octs = (char *)nameType->buff,
        .octetLen = nameType->len,
    };
    const char *oidName = BSL_OBJ_GetOidNameFromOid(&oid);
    if (oidName == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CERT_INVALID_DN);
        return HITLS_X509_ERR_CERT_INVALID_DN;
    }
    if (strcpy_s(buff, buffLen, oidName) != EOK) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CERT_INVALID_DN);
        return HITLS_X509_ERR_CERT_INVALID_DN;
    }

    *usedLen = strlen(oidName);
    return HITLS_X509_SUCCESS;
}

static int32_t X509PrintNameNode(const HITLS_X509_NameNode *nameNode, char *buff, int32_t buffLen, int32_t *usedLen)
{
    if (nameNode->layer == 1) {
        return HITLS_X509_SUCCESS;
    }
    int offset = 0;
    *usedLen = 0;
    /* Get the printable type */
    int32_t ret = X509GetPrintSNStr(&nameNode->nameType, buff, buffLen, &offset);
    if (ret != HITLS_X509_SUCCESS) {
        return ret;
    }
    /* print '=' between type and value */
    if (buffLen - offset < 2) { // 2 denote buffer is enough to place two character, i.e '=' and '\0'
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    buff[offset] = '=';
    offset++;
    /* print 'value' */
    if (nameNode->nameValue.buff == NULL || nameNode->nameValue.len == 0) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    if (memcpy_s(buff + offset, buffLen - offset, nameNode->nameValue.buff, nameNode->nameValue.len) != EOK) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CERT_INVALID_DN);
        return HITLS_X509_ERR_CERT_INVALID_DN;
    }
    offset += nameNode->nameValue.len;
    *usedLen = offset;
    return HITLS_X509_SUCCESS;
}

static int32_t GetDistinguishNameStrFromList(BSL_ASN1_List *nameList, BSL_Buffer *buff)
{
    if (nameList == NULL || BSL_LIST_COUNT(nameList) == 0) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    uint32_t offset = 0;
    int32_t ret;
    char tmpBuffStr[MAX_DN_STR_LEN] = {};
    char *tmpBuff = tmpBuffStr;
    uint32_t tmpBuffLen = MAX_DN_STR_LEN;
    (void)BSL_LIST_GET_FIRST(nameList);
    HITLS_X509_NameNode *firstNameNode = BSL_LIST_GET_NEXT(nameList);
    HITLS_X509_NameNode *nameNode = firstNameNode;
    while (nameNode != NULL) {
        if (tmpBuffLen - offset < 2) { // 2 denote buffer is enough to place two character, i.e ',' and '\0'
            ret = HITLS_X509_ERR_INVALID_PARAM;
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
            return HITLS_X509_ERR_INVALID_PARAM;
        }
        if (nameNode != firstNameNode && nameNode->layer == 2) {
            *tmpBuff = ',';
            tmpBuff++;
            offset++;
        }
        int32_t eachUsedLen = 0;
        ret = X509PrintNameNode(nameNode, tmpBuff, tmpBuffLen - offset, &eachUsedLen);
        if (ret != HITLS_X509_SUCCESS) {
            BSL_ERR_PUSH_ERROR(ret);
            return ret;
        }
        tmpBuff += eachUsedLen;
        offset += eachUsedLen;
        nameNode = BSL_LIST_GET_NEXT(nameList);
    }
    buff->data = BSL_SAL_Calloc(offset + 1, sizeof(char));
    if (buff->data == NULL) {
        BSL_ERR_PUSH_ERROR(BSL_MALLOC_FAIL);
        return BSL_MALLOC_FAIL;
    }
    (void)memcpy_s(buff->data, offset + 1, tmpBuffStr, offset);
    buff->dataLen = offset;
    return HITLS_X509_SUCCESS;
}

static int32_t X509_GetDistinguishNameStr(HITLS_X509_Cert *cert, BSL_Buffer *val, int32_t opt)
{
    if (val == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    switch (opt) {
        case HITLS_X509_ISSUER_DN_NAME:
            return GetDistinguishNameStrFromList(cert->tbs.issuerName, val);
        case HITLS_X509_SUBJECT_DN_NAME:
            return GetDistinguishNameStrFromList(cert->tbs.subjectName, val);
        default:
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
            return HITLS_X509_ERR_INVALID_PARAM;
    }
}

static int32_t GetAsn1SerialNumStr(const BSL_ASN1_Buffer *number, BSL_Buffer *val)
{
    if (number == NULL || number->buff == NULL || number->len == 0 || number->tag != BSL_ASN1_TAG_INTEGER ||
        val == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }

    for (size_t i = 0; i < number->len - 1; i++) {
        if (sprintf_s((char *)&val->data[3 * i], val->dataLen - 3 * i, "%02x:", number->buff[i]) == -1) {
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CERT_INVALID_SERIAL_NUM);
            return HITLS_X509_ERR_CERT_INVALID_SERIAL_NUM;
        }
    }
    size_t index = 3 * (number->len - 1);
    if (sprintf_s((char *)&val->data[index], val->dataLen - index, "%02x", number->buff[number->len - 1]) == -1) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CERT_INVALID_SERIAL_NUM);
        return HITLS_X509_ERR_CERT_INVALID_SERIAL_NUM;
    }
    val->dataLen = 3 * number->len - 1;
    return HITLS_X509_SUCCESS;
}

static int32_t X509_GetSerialNumStr(HITLS_X509_Cert *cert, BSL_Buffer *val)
{
    if (val == NULL || cert->tbs.serialNum.buff == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    BSL_ASN1_Buffer serialNum = cert->tbs.serialNum;
    val->data = BSL_SAL_Calloc(serialNum.len * 3, sizeof(uint8_t));
    if (val->data == NULL) {
        BSL_ERR_PUSH_ERROR(BSL_MALLOC_FAIL);
        return BSL_MALLOC_FAIL;
    }
    val->dataLen = serialNum.len * 3;
    int32_t ret = GetAsn1SerialNumStr(&serialNum, val);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_SAL_FREE(val->data);
        val->dataLen = 0;
    }

    return ret;
}

// rfc822: https://www.w3.org/Protocols/rfc822/
static const char g_monAsn1Str[12][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static int32_t GetAsn1BslTimeStr(const BSL_TIME *time, BSL_Buffer *val)
{
    if (time == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    val->data = BSL_SAL_Calloc(PRINT_TIME_MAX_SIZE, sizeof(uint8_t));
    if (val->data == NULL) {
        BSL_ERR_PUSH_ERROR(BSL_MALLOC_FAIL);
        return BSL_MALLOC_FAIL;
    }
    if (sprintf_s((char *)val->data, PRINT_TIME_MAX_SIZE, "%s %u %02u:%02u:%02u %u%s",
        g_monAsn1Str[time->month - 1], time->day, time->hour, time->minute, time->second, time->year, " GMT") == -1) {
        BSL_SAL_FREE(val->data);
        val->dataLen = 0;
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CERT_INVALID_TIME);
        return HITLS_X509_ERR_CERT_INVALID_TIME;
    }
    val->dataLen = strlen((char *)val->data);
    return HITLS_X509_SUCCESS;
}

static int32_t X509_GetAsn1BslTimeStr(HITLS_X509_Cert *cert, BSL_Buffer *val, int32_t opt)
{
    if (val == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }

    switch (opt) {
        case HITLS_X509_BEFORE_TIME:
            return GetAsn1BslTimeStr(&cert->tbs.validTime.start, val);
        case HITLS_X509_AFTER_TIME:
            return GetAsn1BslTimeStr(&cert->tbs.validTime.end, val);
        default:
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
            return HITLS_X509_ERR_INVALID_PARAM;
    }
}

static int32_t X509_GetCertExt(HITLS_X509_Ext *ext, HITLS_X509_Ext **val, int32_t valLen)
{
    if (val == NULL || valLen != sizeof(HITLS_X509_Ext *)) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    *val = ext;
    return HITLS_X509_SUCCESS;
}

static int32_t X509_CertGetCtrl(HITLS_X509_Cert *cert, int32_t cmd, void *val, int32_t valLen)
{
    switch (cmd) {
        case HITLS_X509_GET_ENCODELEN:
            return HITLS_X509_GetEncodeLen(cert->rawDataLen, val, valLen);
        case HITLS_X509_GET_ENCODE:
            return HITLS_X509_GetEncodeData(cert->rawData, val);
        case HITLS_X509_GET_PUBKEY:
            return HITLS_X509_GetPubKey(cert->tbs.ealPubKey, val);
        case HITLS_X509_GET_SIGNALG:
            return HITLS_X509_GetSignAlg(cert->signAlgId.algId, val, valLen);
        case HITLS_X509_GET_SUBJECT_DNNAME:
            return HITLS_X509_GetList(cert->tbs.subjectName, val, valLen);
        case HITLS_X509_GET_ISSUER_DNNAME:
            return HITLS_X509_GetList(cert->tbs.issuerName, val, valLen);
        case HITLS_X509_GET_SUBJECT_DNNAME_STR:
            return X509_GetDistinguishNameStr(cert, val, HITLS_X509_SUBJECT_DN_NAME);
        case HITLS_X509_GET_ISSUER_DNNAME_STR:
            return X509_GetDistinguishNameStr(cert, val, HITLS_X509_ISSUER_DN_NAME);
        case HITLS_X509_GET_SERIALNUM:
            return X509_GetSerialNumStr(cert, val);
        case HITLS_X509_GET_BEFORE_TIME:
            return X509_GetAsn1BslTimeStr(cert, val, HITLS_X509_BEFORE_TIME);
        case HITLS_X509_GET_AFTER_TIME:
            return X509_GetAsn1BslTimeStr(cert, val, HITLS_X509_AFTER_TIME);
        case HITLS_X509_GET_EXT:
            return X509_GetCertExt(&cert->tbs.ext, val, valLen);
        default:
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
            return HITLS_X509_ERR_INVALID_PARAM;
    }
}

typedef bool (*SetParamCheck)(const void *val, int32_t valLen);

static bool VersionCheck(const void *val, int32_t valLen)
{
    return valLen == sizeof(int32_t) && *(int32_t *)val >= HITLS_CERT_VERSION_1 &&
        *(int32_t *)val <= HITLS_CERT_VERSION_3;
}

static int32_t CertSetSerial(BSL_ASN1_Buffer *serial, const void *val, int32_t valLen)
{
    if (valLen <= 0) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CERT_INVALID_SERIAL_NUM);
        return HITLS_X509_ERR_CERT_INVALID_SERIAL_NUM;
    }
    const uint8_t *src = (const uint8_t *)val;
    serial->buff = BSL_SAL_Dump(src, valLen);
    if (serial->buff == NULL) {
        BSL_ERR_PUSH_ERROR(BSL_DUMP_FAIL);
        return BSL_DUMP_FAIL;
    }
    serial->len = valLen;
    serial->tag = BSL_ASN1_TAG_INTEGER;
    return HITLS_X509_SUCCESS;
}

static bool TimeCheck(const void *val, int32_t valLen)
{
    (void)val;
    return valLen == sizeof(BSL_TIME) && BSL_DateTimeCheck((const BSL_TIME *)val);
}

static int32_t CertSet(void *dest, int32_t size, void *val, int32_t valLen, SetParamCheck check)
{
    if (check(val, valLen) != true) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    (void)memcpy_s(dest, size, val, size);
    return HITLS_X509_SUCCESS;
}

static int32_t HITLS_X509_SetCsrExt(HITLS_X509_Ext *ext, HITLS_X509_Csr *csr)
{
    HITLS_X509_Attr attr = {0};
    int32_t ret = HITLS_X509_AttrCtrl(
        csr->reqInfo.attributes, HITLS_X509_ATTR_GET_REQUESTED_EXTENSIONS, &attr, sizeof(HITLS_X509_Attr));
    if (ret == HITLS_X509_ERR_ATTR_NOT_FOUND) {
        return ret;
    }
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    HITLS_X509_Ext *csrExt = (HITLS_X509_Ext *)attr.value;
    ret = HITLS_X509_ExtReplace(ext, csrExt);
    HITLS_X509_ExtFree(csrExt);
    return ret;
}

int32_t X509_CertSetCtrl(HITLS_X509_Cert *cert, int32_t cmd, void *val, int32_t valLen)
{
    if (cert == NULL || val == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    int32_t ret;
    switch (cmd) {
        case HITLS_X509_SET_VERSION:
            return CertSet(&cert->tbs.version, sizeof(int32_t), val, valLen, VersionCheck);
        case HITLS_X509_SET_SERIALNUM:
            return CertSetSerial(&cert->tbs.serialNum, val, valLen);
        case HITLS_X509_SET_BEFORE_TIME:
            ret = CertSet(&cert->tbs.validTime.start, sizeof(BSL_TIME), val, valLen, TimeCheck);
            if (ret == HITLS_X509_SUCCESS) {
                cert->tbs.validTime.flag |= BSL_TIME_BEFORE_SET;
                cert->tbs.validTime.flag |=
                    cert->tbs.validTime.start.year <= BSL_TIME_UTC_MAX_YEAR ? BSL_TIME_BEFORE_IS_UTC : 0;
            }
            return ret;
        case HITLS_X509_SET_AFTER_TIME:
            ret = CertSet(&cert->tbs.validTime.end, sizeof(BSL_TIME), val, valLen, TimeCheck);
            if (ret == HITLS_X509_SUCCESS) {
                cert->tbs.validTime.flag |= BSL_TIME_AFTER_SET;
                cert->tbs.validTime.flag |=
                    cert->tbs.validTime.end.year <= BSL_TIME_UTC_MAX_YEAR ? BSL_TIME_AFTER_IS_UTC : 0;
            }
            return ret;
        case HITLS_X509_SET_PRIVKEY:
            return HITLS_X509_SetPkey(&cert->ealPrivKey, val);
        case HITLS_X509_SET_SIGN_MD_ID:
            return HITLS_X509_SetSignMdId(&cert->signMdId, val, valLen);
        case HITLS_X509_SET_SIGN_RSA_PSS_PARAM:
            return HITLS_X509_SetRsaPssPara(cert->ealPrivKey, val, valLen);
        case HITLS_X509_SET_SIGN_RSA_PADDING:
            return HITLS_X509_SetRsaPadding(cert->ealPrivKey, val, valLen);
        case HITLS_X509_SET_PUBKEY:
            return HITLS_X509_SetPkey(&cert->tbs.ealPubKey, val);
        case HITLS_X509_SET_ISSUER_DNNAME:
            return HITLS_X509_SetNameList(&cert->tbs.issuerName, val, valLen);
        case HITLS_X509_SET_SUBJECT_DNNAME:
            return HITLS_X509_SetNameList(&cert->tbs.subjectName, val, valLen);
        case HITLS_X509_SET_CSR_EXT:
            return HITLS_X509_SetCsrExt(&cert->tbs.ext, val);
        default:
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
            return HITLS_X509_ERR_INVALID_PARAM;
    }
}

static int32_t X509_CertExtCtrl(HITLS_X509_Cert *cert, int32_t cmd, void *val, int32_t valLen)
{
    switch (cmd) {
        case HITLS_X509_EXT_KU_DIGITALSIGN:
            return X509_KeyUsageCheck(cert, val, valLen, HITLS_X509_EXT_KU_DIGITAL_SIGN);
        case HITLS_X509_EXT_KU_CERTSIGN:
            return X509_KeyUsageCheck(cert, val, valLen, HITLS_X509_EXT_KU_KEY_CERT_SIGN);
        case HITLS_X509_EXT_KU_KEYAGREEMENT:
            return X509_KeyUsageCheck(cert, val, valLen, HITLS_X509_EXT_KU_KEY_AGREEMENT);
        case HITLS_X509_EXT_KU_KEYENC:
            return X509_KeyUsageCheck(cert, val, valLen, HITLS_X509_EXT_KU_KEY_ENCIPHERMENT);
        default:
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
            return HITLS_X509_ERR_INVALID_PARAM;
    }
}

int32_t HITLS_X509_CertCtrl(HITLS_X509_Cert *cert, int32_t cmd, void *val, int32_t valLen)
{
    if (cert == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    if (cmd == HITLS_X509_REF_UP) {
        return HITLS_X509_RefUp(&cert->references, val, valLen);
    } else if (cmd >= HITLS_X509_GET_ENCODELEN && cmd < HITLS_X509_SET_VERSION) {
        return X509_CertGetCtrl(cert, cmd, val, valLen);
    } else if (cmd < HITLS_X509_EXT_KU_KEYENC) {
        if (cert->flag == HITLS_X509_CERT_PARSE_FLAG) {
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_SET_AFTER_PARSE);
            return HITLS_X509_ERR_SET_AFTER_PARSE;
        }
        return X509_CertSetCtrl(cert, cmd, val, valLen);
    } else {
        return X509_CertExtCtrl(cert, cmd, val, valLen);
    }
}

int32_t HITLS_X509_CertDup(HITLS_X509_Cert *src, HITLS_X509_Cert **dest)
{
    if (src == NULL || dest == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }

    HITLS_X509_Cert *tempCert = NULL;
    BSL_Buffer encode = {src->rawData, src->rawDataLen};
    int32_t ret = HITLS_X509_CertParseBuff(BSL_FORMAT_ASN1, &encode, &tempCert);
    if (ret != HITLS_X509_SUCCESS) {
        return ret;
    }
    *dest = tempCert;
    return ret;
}

/**
 * Confirm whether the certificate is the issuer of the current certificate
 *   1. Check if the issueName matches the subjectname
 *   2. Is the issuer certificate a CA
 *   3. Check if the algorithm of the issuer certificate matches that of the sub certificate
 *   4. Check if the certificate keyusage has a certificate sign
 */
int32_t HITLS_X509_CheckIssued(HITLS_X509_Cert *issue, HITLS_X509_Cert *subject, bool *res)
{
    int32_t ret = HITLS_X509_CmpNameNode(issue->tbs.subjectName, subject->tbs.issuerName);
    if (ret != 0) {
        *res = false;
        return HITLS_X509_SUCCESS;
    }
    /**
     * If the basic constraints extension is not present in a version 3 certificate,
     * or the extension is present but the cA boolean is not asserted,
     * then the certified public key MUST NOT be used to verify certificate signatures.
     */
    if (issue->tbs.version == HITLS_CERT_VERSION_3 && !(issue->tbs.ext.extFlags & HITLS_X509_EXT_FLAG_BCONS) &&
        !issue->tbs.ext.isCa) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_CERT_NOT_CA);
        return HITLS_X509_ERR_CERT_NOT_CA;
    }

    ret = HITLS_X509_CheckAlg(issue->tbs.ealPubKey, &subject->tbs.signAlgId);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    /**
     * Conforming CAs MUST include this extension
     * in certificates that contain public keys that are used to validate digital signatures on
     * other public key certificates or CRLs.
     */
    if (issue->tbs.ext.extFlags & HITLS_X509_EXT_FLAG_KUSAGE) {
        if (((issue->tbs.ext.keyUsage & HITLS_X509_EXT_KU_KEY_CERT_SIGN)) == 0) {
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_VFY_KU_NO_CERTSIGN);
            return HITLS_X509_ERR_VFY_KU_NO_CERTSIGN;
        }
    }
    *res = true;
    return HITLS_X509_SUCCESS;
}

int32_t HITLS_X509_CertIsCA(HITLS_X509_Cert *cert, bool *res)
{
    *res = true;
    if (cert->tbs.version == HITLS_CERT_VERSION_3) {
        if (!(cert->tbs.ext.extFlags & HITLS_X509_EXT_FLAG_BCONS)) {
            *res = false;
        } else {
            *res = cert->tbs.ext.isCa;
        }
    }
    return HITLS_X509_SUCCESS;
}

static int32_t EncodeTbsItems(HITLS_X509_CertTbs *tbs, BSL_ASN1_Buffer *issuer, BSL_ASN1_Buffer *subject,
    BSL_ASN1_Buffer *pubkey, BSL_ASN1_Buffer *ext)
{
    int32_t ret = HITLS_X509_EncodeNameList(tbs->issuerName, issuer);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    ret = HITLS_X509_EncodeNameList(tbs->subjectName, subject);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        BSL_SAL_Free(issuer->buff);
        return ret;
    }
    BSL_Buffer pub = {0};
    ret = CRYPT_EAL_EncodePubKeyBuffInternal(tbs->ealPubKey, BSL_FORMAT_ASN1, CRYPT_PUBKEY_SUBKEY, false, &pub);
    if (ret != CRYPT_SUCCESS) {
        BSL_SAL_Free(issuer->buff);
        BSL_SAL_Free(subject->buff);
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }

    if (tbs->version == HITLS_CERT_VERSION_3) {
        ret = HITLS_X509_EncodeExt(BSL_ASN1_CLASS_CTX_SPECIFIC | BSL_ASN1_TAG_CONSTRUCTED |
            HITLS_CERT_CTX_SPECIFIC_TAG_EXTENSION, tbs->ext.list, ext);
        if (ret != HITLS_X509_SUCCESS) {
            BSL_SAL_Free(issuer->buff);
            BSL_SAL_Free(subject->buff);
            BSL_SAL_Free(pub.data);
            BSL_ERR_PUSH_ERROR(ret);
        }
    }
    pubkey->buff = pub.data;
    pubkey->len = pub.dataLen;
    return ret;
}

BSL_ASN1_TemplateItem g_tbsTempl[] = {
    /* version */
    {BSL_ASN1_CLASS_CTX_SPECIFIC | BSL_ASN1_TAG_CONSTRUCTED | HITLS_CERT_CTX_SPECIFIC_TAG_VER,
     BSL_ASN1_FLAG_DEFAULT, 0},
        {BSL_ASN1_TAG_INTEGER, BSL_ASN1_FLAG_DEFAULT, 1},
    /* serial number */
    {BSL_ASN1_TAG_INTEGER, 0, 0},
    /* signature info */
    {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 0},
    /* issuer */
    {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_HEADERONLY | BSL_ASN1_FLAG_SAME, 0},
    /* validity */
    {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 0},
        {BSL_ASN1_TAG_CHOICE, 0, 1},
        {BSL_ASN1_TAG_CHOICE, 0, 1},
    /* subject ref: issuer */
    {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_HEADERONLY | BSL_ASN1_FLAG_SAME, 0},
    /* subject public key info ref signature info */
    {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, BSL_ASN1_FLAG_HEADERONLY, 0},
    /* Note!!: issuer id, subject id are not supported */
    /* extension */
    {BSL_ASN1_CLASS_CTX_SPECIFIC | BSL_ASN1_TAG_CONSTRUCTED | HITLS_CERT_CTX_SPECIFIC_TAG_EXTENSION,
     BSL_ASN1_FLAG_OPTIONAL | BSL_ASN1_FLAG_HEADERONLY | BSL_ASN1_FLAG_SAME, 0},
};
#define HITLS_X509_CERT_TBS_SIZE 9

static int32_t EncodeTbsCertificate(HITLS_X509_CertTbs *tbs, BSL_ASN1_Buffer *signAlg, BSL_ASN1_Buffer *tbsBuff)
{
    BSL_ASN1_Buffer issuer = {0};
    BSL_ASN1_Buffer subject = {0};
    BSL_ASN1_Buffer pubkey = {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, NULL};
    BSL_ASN1_Buffer ext = {BSL_ASN1_CLASS_CTX_SPECIFIC | BSL_ASN1_TAG_CONSTRUCTED |
        HITLS_CERT_CTX_SPECIFIC_TAG_EXTENSION, 0, NULL};

    int32_t ret = EncodeTbsItems(tbs, &issuer, &subject, &pubkey, &ext);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }

    uint8_t ver = (uint8_t)tbs->version;
    BSL_ASN1_Template templ = {g_tbsTempl, sizeof(g_tbsTempl) / sizeof(g_tbsTempl[0])};
    BSL_ASN1_Buffer asns[HITLS_X509_CERT_TBS_SIZE] = {
        {BSL_ASN1_TAG_INTEGER, ver == HITLS_CERT_VERSION_1 ? 0 : 1, ver == HITLS_CERT_VERSION_1 ? NULL : &ver}, // 0
        tbs->serialNum,                                        // 1 serial number
        *signAlg,                                              // 2 sigAlg
        issuer,                                                // 3 issuer
        {tbs->validTime.flag & BSL_TIME_BEFORE_IS_UTC ? BSL_ASN1_TAG_UTCTIME : BSL_ASN1_TAG_GENERALIZEDTIME,
         sizeof(BSL_TIME), (uint8_t *)&tbs->validTime.start},  // 4 start
        {tbs->validTime.flag & BSL_TIME_AFTER_IS_UTC ? BSL_ASN1_TAG_UTCTIME : BSL_ASN1_TAG_GENERALIZEDTIME,
         sizeof(BSL_TIME), (uint8_t *)&tbs->validTime.end},    // 5 end
        subject,                                               // 6 subject
        pubkey,                                                // 7 pubkey info
        ext,                                                   // 8 extensions, only for v3
    };
    ret = BSL_ASN1_EncodeTemplate(&templ, asns, HITLS_X509_CERT_TBS_SIZE, &tbsBuff->buff, &tbsBuff->len);
    BSL_SAL_Free(issuer.buff);
    BSL_SAL_Free(subject.buff);
    BSL_SAL_Free(pubkey.buff);
    if (ver == HITLS_CERT_VERSION_3 && ext.buff != NULL) {
        BSL_SAL_Free(ext.buff);
    }
    return ret;
}

BSL_ASN1_TemplateItem g_briefCertTempl[] = {
    {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 0}, /* x509 */
        {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 1}, /* tbs */
        {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, 1}, /* signAlg */
        {BSL_ASN1_TAG_BITSTRING, 0, 1}                            /* sig */
};

#define HITLS_X509_CERT_BRIEF_SIZE 3

static int32_t EncodeAsn1Cert(HITLS_X509_Cert *cert, BSL_Buffer *buff)
{
    BSL_ASN1_Buffer tbsAsn1 = {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, NULL};
    BSL_ASN1_Buffer signAlg = {BSL_ASN1_TAG_CONSTRUCTED | BSL_ASN1_TAG_SEQUENCE, 0, NULL};
    BSL_Buffer tbsBuff = {0};

    int32_t ret = HITLS_X509_SetSignAlgInfo(cert->ealPrivKey, cert->signMdId, &cert->signAlgId);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    ret = HITLS_X509_EncodeSignAlgInfo(&cert->signAlgId, &signAlg);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    cert->tbs.signAlgId = cert->signAlgId;
    ret = EncodeTbsCertificate(&cert->tbs, &signAlg, &tbsAsn1);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LOG_BINLOG_FIXLEN(BINLOG_ID05065, BSL_LOG_LEVEL_ERR, BSL_LOG_BINLOG_TYPE_RUN,
            "cert: failed to encode tbs.", 0, 0, 0, 0);
        BSL_ERR_PUSH_ERROR(ret);
        goto EXIT;
    }

    ret = HITLS_X509_SignAsn1Data(cert->ealPrivKey, cert->signMdId, &tbsAsn1, &tbsBuff, &cert->signature);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LOG_BINLOG_FIXLEN(BINLOG_ID05065, BSL_LOG_LEVEL_ERR, BSL_LOG_BINLOG_TYPE_RUN,
            "cert: failed to sign the cert.", 0, 0, 0, 0);
        BSL_ERR_PUSH_ERROR(ret);
        goto EXIT;
    }

    BSL_ASN1_Buffer asns[HITLS_X509_CERT_BRIEF_SIZE] = {
        tbsAsn1,
        signAlg,
        {BSL_ASN1_TAG_BITSTRING, sizeof(BSL_ASN1_BitString), (uint8_t *)&cert->signature},
    };
    BSL_ASN1_Template templ = {g_briefCertTempl, sizeof(g_briefCertTempl) / sizeof(g_briefCertTempl[0])};
    ret = BSL_ASN1_EncodeTemplate(&templ, asns, HITLS_X509_CERT_BRIEF_SIZE, &buff->data, &buff->dataLen);
EXIT:
    BSL_SAL_Free(signAlg.buff);
    BSL_SAL_Free(tbsAsn1.buff);
    if (ret == HITLS_X509_SUCCESS) {
        cert->tbs.tbsRawData = tbsBuff.data;
        cert->tbs.tbsRawDataLen = tbsBuff.dataLen;
    } else {
        BSL_SAL_FREE(tbsBuff.data);
    }
    return ret;
}

static int32_t CheckCertValid(HITLS_X509_Cert *cert)
{
    if (cert == NULL) {
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    if (BSL_LIST_COUNT(cert->tbs.ext.list) != 0 && cert->tbs.version != HITLS_CERT_VERSION_3) {
        return HITLS_X509_ERR_CERT_INACCRACY_VERSION;
    }
    if (cert->tbs.serialNum.buff == NULL || cert->tbs.serialNum.len == 0) {
        return HITLS_X509_ERR_CERT_INVALID_SERIAL_NUM;
    }
    if (BSL_LIST_COUNT(cert->tbs.issuerName) == 0 || BSL_LIST_COUNT(cert->tbs.subjectName) == 0) {
        return HITLS_X509_ERR_CERT_INVALID_DN;
    }
    if ((cert->tbs.validTime.flag & BSL_TIME_BEFORE_SET) == 0 || (cert->tbs.validTime.flag & BSL_TIME_AFTER_SET) == 0) {
        return HITLS_X509_ERR_CERT_INVALID_TIME;
    }
    int32_t ret = BSL_SAL_DateTimeCompare(&cert->tbs.validTime.start, &cert->tbs.validTime.end, NULL);
    if (ret != BSL_TIME_DATE_BEFORE && ret != BSL_TIME_CMP_EQUAL) {
        return HITLS_X509_ERR_CERT_START_TIME_LATER;
    }
    if (cert->signMdId == 0) {
        return HITLS_X509_ERR_CERT_INVALID_SIGN_MD;
    }
    if (cert->ealPrivKey == NULL) {
        return HITLS_X509_ERR_CERT_INVALID_PRVKEY;
    }
    if (cert->tbs.ealPubKey == NULL) {
        return HITLS_X509_ERR_CERT_INVALID_PUBKEY;
    }

    return HITLS_X509_SUCCESS;
}

int32_t HITLS_X509_EncodeAsn1Cert(HITLS_X509_Cert *cert, BSL_Buffer *buff)
{
    /* Encoded after decoding. */
    if (cert->rawData != NULL && cert->rawDataLen != 0) {
        buff->data = BSL_SAL_Dump(cert->rawData, cert->rawDataLen);
        if (buff->data == NULL) {
            BSL_ERR_PUSH_ERROR(BSL_DUMP_FAIL);
            return BSL_DUMP_FAIL;
        }
        buff->dataLen = cert->rawDataLen;
        return HITLS_X509_SUCCESS;
    }

    /* Generate a new certificate. */
    int32_t ret = CheckCertValid(cert);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LOG_BINLOG_FIXLEN(BINLOG_ID05065, BSL_LOG_LEVEL_ERR, BSL_LOG_BINLOG_TYPE_RUN,
            "cert: encode cert failed due to invalid parameters.", 0, 0, 0, 0);
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    ret = EncodeAsn1Cert(cert, buff);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LOG_BINLOG_FIXLEN(BINLOG_ID05065, BSL_LOG_LEVEL_ERR, BSL_LOG_BINLOG_TYPE_RUN,
            "cert: failed to encode the cert.", 0, 0, 0, 0);
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }

    cert->rawData = BSL_SAL_Dump(buff->data, buff->dataLen);
    if (buff->data == NULL) {
        BSL_SAL_FREE(buff->data);
        BSL_ERR_PUSH_ERROR(BSL_DUMP_FAIL);
        return BSL_DUMP_FAIL;
    }
    cert->rawDataLen = buff->dataLen;
    return HITLS_X509_SUCCESS;
}

int32_t HITLS_X509_EncodePemCert(HITLS_X509_Cert *cert, BSL_Buffer *buff)
{
    BSL_Buffer asn1 = {0};
    int32_t ret = HITLS_X509_EncodeAsn1Cert(cert, &asn1);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LOG_BINLOG_FIXLEN(
            BINLOG_ID05065, BSL_LOG_LEVEL_ERR, BSL_LOG_BINLOG_TYPE_RUN, "cert: failed to encode the cert.", 0, 0, 0, 0);
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }

    BSL_Buffer base64 = {0};
    BSL_PEM_Symbol symbol = {BSL_PEM_CERT_BEGIN_STR, BSL_PEM_CERT_END_STR};
    ret = BSL_PEM_EncodeAsn1ToPem(asn1.data, asn1.dataLen, &symbol, (char **)&base64.data, &base64.dataLen);
    BSL_SAL_Free(asn1.data);
    if (ret != BSL_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    buff->data = base64.data;
    buff->dataLen = base64.dataLen;
    return ret;
}

int32_t HITLS_X509_CertGenBuff(int32_t format, HITLS_X509_Cert *cert, BSL_Buffer *buff)
{
    if (cert == NULL || buff == NULL || buff->data != NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    switch (format) {
        case BSL_FORMAT_ASN1:
            return HITLS_X509_EncodeAsn1Cert(cert, buff);
        case BSL_FORMAT_PEM:
            return HITLS_X509_EncodePemCert(cert, buff);
        default:
            BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
            return HITLS_X509_ERR_INVALID_PARAM;
    }
}

int32_t HITLS_X509_CertGenFile(int32_t format, HITLS_X509_Cert *cert, const char *path)
{
    if (path == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }

    BSL_Buffer encode = {0};
    int32_t ret = HITLS_X509_CertGenBuff(format, cert, &encode);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    ret = BSL_SAL_WriteFile(path, encode.data, encode.dataLen);
    BSL_SAL_Free(encode.data);
    return ret;
}

int32_t HITLS_X509_CertDigest(HITLS_X509_Cert *cert, CRYPT_MD_AlgId mdId, uint8_t *data, uint32_t *dataLen)
{
    if (cert == NULL || data == NULL || dataLen == NULL) {
        BSL_ERR_PUSH_ERROR(HITLS_X509_ERR_INVALID_PARAM);
        return HITLS_X509_ERR_INVALID_PARAM;
    }
    if (cert->rawData != NULL && cert->rawDataLen != 0) {
        return CRYPT_EAL_Md(mdId, cert->rawData, cert->rawDataLen, data, dataLen);
    }
    BSL_Buffer asn1 = {0};
    int32_t ret = HITLS_X509_EncodeAsn1Cert(cert, &asn1);
    if (ret != HITLS_X509_SUCCESS) {
        BSL_LOG_BINLOG_FIXLEN(
            BINLOG_ID05065, BSL_LOG_LEVEL_ERR, BSL_LOG_BINLOG_TYPE_RUN, "cert: failed to encode the cert.", 0, 0, 0, 0);
        BSL_ERR_PUSH_ERROR(ret);
        return ret;
    }
    cert->rawData = asn1.data;
    cert->rawDataLen = asn1.dataLen;
    return CRYPT_EAL_Md(mdId, cert->rawData, cert->rawDataLen, data, dataLen);
}
