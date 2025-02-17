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

#ifndef HITLS_X509_LOCAL_H
#define HITLS_X509_LOCAL_H

#include <stdint.h>
#include "bsl_asn1.h"
#include "bsl_obj.h"
#include "hitls_x509.h"
#include "crypt_types.h"
#include "crypt_eal_pkey.h"
#include "sal_atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Check whether conditions are met. If yes, an error code is returned.
 */
#define HITLS_X509_RETURN_RET_IF(ret)            \
    do {                                         \
        if ((ret) != 0) {                          \
            BSL_ERR_PUSH_ERROR((ret));             \
            return (ret);                          \
        }                                        \
    } while (0)

#define HITLS_X509_GOTO_RET_IF(ret)              \
    do {                                         \
        if ((ret) != 0) {                          \
            BSL_ERR_PUSH_ERROR((ret));             \
            goto ERR;                            \
        }                                        \
    } while (0)

/**
 * RFC 5280: section 4.1.2.5.1
 */
#define BSL_TIME_UTC_MAX_YEAR 2049

#define BSL_TIME_BEFORE_SET         0x01
#define BSL_TIME_AFTER_SET          0x02
#define BSL_TIME_BEFORE_IS_UTC      0x04
#define BSL_TIME_AFTER_IS_UTC       0x08

#define HITLS_X509_EXT_FLAG_PARSE (1 << 0)
#define HITLS_X509_EXT_FLAG_SET (1 << 1)
#define HITLS_X509_EXT_FLAG_KUSAGE (1 << 2)
#define HITLS_X509_EXT_FLAG_BCONS (1 << 3)

#define HITLS_X509_GN_OTHER (HITLS_X509_GN_IP + 1)
#define HITLS_X509_GN_X400  (HITLS_X509_GN_OTHER + 1)
#define HITLS_X509_GN_EDI   (HITLS_X509_GN_X400 + 1)
#define HITLS_X509_GN_RID   (HITLS_X509_GN_EDI + 1)

typedef struct _HITLS_X509_NameNode {
    BSL_ASN1_Buffer nameType;
    BSL_ASN1_Buffer nameValue;
    uint8_t layer;
} HITLS_X509_NameNode;

typedef struct _HITLS_X509_ExtEntry {
    BslCid cid;
    BSL_ASN1_Buffer extnId;
    bool critical;
    BSL_ASN1_Buffer extnValue;
} HITLS_X509_ExtEntry;

typedef struct _HITLS_X509_Ext {
    BslList *list;
    uint32_t extFlags;
    // basic usage ext
    bool isCa;
    // -1 no check, 0 no intermediate certificate
    int32_t maxPathLen;
    // key usage ext
    uint32_t keyUsage;
} HITLS_X509_Ext;

typedef struct _HITLS_X509_AttrEntry {
    BslCid cid;
    BSL_ASN1_Buffer attrId;
    BSL_ASN1_Buffer attrValue;
} HITLS_X509_AttrEntry;

typedef struct _HITLS_X509_ValidTime {
    uint8_t flag;
    BSL_TIME start;
    BSL_TIME end;
} HITLS_X509_ValidTime;

typedef struct _HITLS_X509_Asn1AlgId {
    BslCid algId;
    union {
        CRYPT_RSA_PssPara rsaPssParam;
    };
} HITLS_X509_Asn1AlgId;

typedef int32_t (*HITLS_X509_Asn1Parse)(bool isCopy, uint8_t **encode, uint32_t *encodeLen, void *out);
typedef void *(*HITLS_X509_New)(void);
typedef void (*HITLS_X509_Free)(void *elem);

typedef struct {
    HITLS_X509_Asn1Parse asn1Parse;
    HITLS_X509_New x509New;
    HITLS_X509_Free x509Free;
} X509_ParseFuncCbk;

int32_t HITLS_X509_ParseTbsRawData(uint8_t *encode, uint32_t encodeLen, uint8_t **tbsRawData, uint32_t *tbsRawDataLen);

// The public key  parsing is more complex, and the crypto module completes it
int32_t HITLS_X509_ParseSignAlgInfo(BSL_ASN1_Buffer *algId, BSL_ASN1_Buffer *param, HITLS_X509_Asn1AlgId *x509Alg);

int32_t HITLS_X509_EncodeSignAlgInfo(HITLS_X509_Asn1AlgId *x509Alg, BSL_ASN1_Buffer *asn);

void HITLS_X509_FreeNameNode(HITLS_X509_NameNode *node);

int32_t HITLS_X509_SetNameList(BslList **dest, void *val, int32_t valLen);

int32_t HITLS_X509_ParseNameList(BSL_ASN1_Buffer *name, BSL_ASN1_List *list);

int32_t HITLS_X509_EncodeNameList(BSL_ASN1_List *list, BSL_ASN1_Buffer *name);

int32_t HITLS_X509_ParseGeneralNames(uint8_t *encode, uint32_t encLen, BslList *list);

void HITLS_X509_FreeGeneralName(HITLS_X509_GeneralName *data);

void HITLS_X509_FreeGeneralNames(BslList *names);

void HITLS_X509_ClearGeneralNames(BslList *names);

int32_t HITLS_X509_ParseAuthorityKeyId(HITLS_X509_ExtEntry *extEntry, HITLS_X509_ExtAki *aki);

void HITLS_X509_ClearAuthorityKeyId(HITLS_X509_ExtAki *aki);

int32_t HITLS_X509_ParseSubjectKeyId(HITLS_X509_ExtEntry *extEntry, HITLS_X509_ExtSki *ski);

int32_t HITLS_X509_ParseExtendedKeyUsage(HITLS_X509_ExtEntry *extEntry, HITLS_X509_ExtExKeyUsage *exku);

void HITLS_X509_ClearExtendedKeyUsage(HITLS_X509_ExtExKeyUsage *exku);

int32_t HITLS_X509_ParseSubjectAltName(HITLS_X509_ExtEntry *extEntry,  HITLS_X509_ExtSan *san);

void HITLS_X509_ClearSubjectAltName(HITLS_X509_ExtSan *san);

HITLS_X509_Ext *HITLS_X509_ExtNew(void);

void HITLS_X509_ExtFree(HITLS_X509_Ext *ext);

int32_t HITLS_X509_ParseExt(BSL_ASN1_Buffer *ext, HITLS_X509_Ext *certExt);

int32_t HITLS_X509_EncodeExt(uint8_t tag, BSL_ASN1_List *list, BSL_ASN1_Buffer *ext);

int32_t HITLS_X509_ParseExtItem(BSL_ASN1_Buffer *extItem, HITLS_X509_ExtEntry *extEntry);

int32_t HITLS_X509_ParseExtItem(BSL_ASN1_Buffer *extItem, HITLS_X509_ExtEntry *extEntry);

void HITLS_X509_ExtEntryFree(HITLS_X509_ExtEntry *entry);

int32_t HITLS_X509_AddListItemDefault(void *item, uint32_t len, BSL_ASN1_List *list);

int32_t HITLS_X509_ParseTime(BSL_ASN1_Buffer *before, BSL_ASN1_Buffer *after, HITLS_X509_ValidTime *time);

int32_t HITLS_X509_ParseX509(int32_t format, BSL_Buffer *encode, bool isCert, X509_ParseFuncCbk *parsefun,
    HITLS_X509_List *list);
int32_t HITLS_X509_CmpNameNode(BSL_ASN1_List *nameOri, BSL_ASN1_List *name);

int32_t HITLS_X509_CheckAlg(CRYPT_EAL_PkeyCtx *pubkey, HITLS_X509_Asn1AlgId *subAlg);

int32_t HITLS_X509_ParseAttrList(BSL_ASN1_Buffer *attrs, BSL_ASN1_List *list);

void HITLS_X509_AttrEntryFree(HITLS_X509_AttrEntry *attr);

int32_t HITLS_X509_SignAsn1Data(CRYPT_EAL_PkeyCtx *priv, CRYPT_MD_AlgId mdId,
    BSL_ASN1_Buffer *asn1Buff, BSL_Buffer *rawSignBuff, BSL_ASN1_BitString *sign);

int32_t HITLS_X509_EncodeAttrList(uint8_t tag, BSL_ASN1_List *list, BSL_ASN1_Buffer *attr);

int32_t HITLS_X509_CheckSignature(const CRYPT_EAL_PkeyCtx *pubKey, uint8_t *rawData, uint32_t rawDataLen,
    HITLS_X509_Asn1AlgId *alg, BSL_ASN1_BitString *signature);

int32_t HITLS_X509_RefUp(BSL_SAL_RefCount *references, int32_t *val, int32_t valLen);

int32_t HITLS_X509_GetList(BslList *list, void *val, int32_t valLen);

int32_t HITLS_X509_GetPubKey(void *ealPubKey, void **val);

int32_t HITLS_X509_GetSignAlg(BslCid signAlgId, int32_t *val, int32_t valLen);

int32_t HITLS_X509_GetEncodeLen(uint32_t encodeLen, uint32_t *val, int32_t valLen);

int32_t HITLS_X509_GetEncodeData(uint8_t *rawData, uint8_t **val);

int32_t HITLS_X509_SetPkey(void **pkey, void *val);

int32_t HITLS_X509_SetSignAlgInfo(CRYPT_EAL_PkeyCtx *privKey, CRYPT_MD_AlgId mdId, HITLS_X509_Asn1AlgId *x509Alg);

int32_t HITLS_X509_SetRsaPadding(CRYPT_EAL_PkeyCtx *privKey, void *val, int32_t valLen);

int32_t HITLS_X509_SetRsaPssPara(CRYPT_EAL_PkeyCtx *privKey, void *val, int32_t valLen);

int32_t HITLS_X509_SetSignMdId(CRYPT_MD_AlgId *mdAlgId, void *val, int32_t valLen);

int32_t HITLS_X509_ExtReplace(HITLS_X509_Ext *dest, HITLS_X509_Ext *src);

#ifdef __cplusplus
}
#endif

#endif // HITLS_X509_LOCAL_H