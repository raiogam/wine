/*
 * crypt32 Crypt*Object functions
 *
 * Copyright 2007 Juan Lang
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include <stdarg.h>
#define NONAMELESSUNION
#include "windef.h"
#include "winbase.h"
#include "wincrypt.h"
#include "mssip.h"
#include "winuser.h"
#include "wintrust.h"
#include "crypt32_private.h"
#include "cryptres.h"
#include "wine/unicode.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(crypt);

static BOOL CRYPT_ReadBlobFromFile(LPCWSTR fileName, PCERT_BLOB blob)
{
    BOOL ret = FALSE;
    HANDLE file;

    TRACE("%s\n", debugstr_w(fileName));

    file = CreateFileW(fileName, GENERIC_READ, FILE_SHARE_READ, NULL,
     OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        ret = TRUE;
        blob->cbData = GetFileSize(file, NULL);
        if (blob->cbData)
        {
            blob->pbData = CryptMemAlloc(blob->cbData);
            if (blob->pbData)
            {
                DWORD read;

                ret = ReadFile(file, blob->pbData, blob->cbData, &read, NULL);
            }
        }
        CloseHandle(file);
    }
    TRACE("returning %d\n", ret);
    return ret;
}

static BOOL CRYPT_QueryContextObject(DWORD dwObjectType, const void *pvObject,
 DWORD dwExpectedContentTypeFlags, DWORD *pdwMsgAndCertEncodingType,
 DWORD *pdwContentType, HCERTSTORE *phCertStore, const void **ppvContext)
{
    CERT_BLOB fileBlob;
    const CERT_BLOB *blob;
    HCERTSTORE store;
    DWORD contentType;
    BOOL ret;

    switch (dwObjectType)
    {
    case CERT_QUERY_OBJECT_FILE:
        /* Cert, CRL, and CTL contexts can't be "embedded" in a file, so
         * just read the file directly
         */
        ret = CRYPT_ReadBlobFromFile((LPCWSTR)pvObject, &fileBlob);
        blob = &fileBlob;
        break;
    case CERT_QUERY_OBJECT_BLOB:
        blob = (const CERT_BLOB *)pvObject;
        ret = TRUE;
        break;
    default:
        SetLastError(E_INVALIDARG); /* FIXME: is this the correct error? */
        ret = FALSE;
    }
    if (!ret)
        return FALSE;

    store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
     CERT_STORE_CREATE_NEW_FLAG, NULL);
    ret = FALSE;
    if (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_CERT)
    {
        ret = pCertInterface->addEncodedToStore(store, X509_ASN_ENCODING,
         blob->pbData, blob->cbData, CERT_STORE_ADD_ALWAYS, ppvContext);
        if (ret)
            contentType = CERT_QUERY_CONTENT_CERT;
    }
    if (!ret && (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_CRL))
    {
        ret = pCRLInterface->addEncodedToStore(store, X509_ASN_ENCODING,
         blob->pbData, blob->cbData, CERT_STORE_ADD_ALWAYS, ppvContext);
        if (ret)
            contentType = CERT_QUERY_CONTENT_CRL;
    }
    if (!ret && (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_CTL))
    {
        ret = pCTLInterface->addEncodedToStore(store, X509_ASN_ENCODING,
         blob->pbData, blob->cbData, CERT_STORE_ADD_ALWAYS, ppvContext);
        if (ret)
            contentType = CERT_QUERY_CONTENT_CTL;
    }
    if (ret)
    {
        if (pdwMsgAndCertEncodingType)
            *pdwMsgAndCertEncodingType = X509_ASN_ENCODING;
        if (pdwContentType)
            *pdwContentType = contentType;
        if (phCertStore)
            *phCertStore = CertDuplicateStore(store);
    }
    CertCloseStore(store, 0);
    if (blob == &fileBlob)
        CryptMemFree(blob->pbData);
    TRACE("returning %d\n", ret);
    return ret;
}

static BOOL CRYPT_QuerySerializedContextObject(DWORD dwObjectType,
 const void *pvObject, DWORD dwExpectedContentTypeFlags,
 DWORD *pdwMsgAndCertEncodingType, DWORD *pdwContentType,
 HCERTSTORE *phCertStore, const void **ppvContext)
{
    CERT_BLOB fileBlob;
    const CERT_BLOB *blob;
    const WINE_CONTEXT_INTERFACE *contextInterface = NULL;
    const void *context;
    DWORD contextType;
    BOOL ret;

    switch (dwObjectType)
    {
    case CERT_QUERY_OBJECT_FILE:
        /* Cert, CRL, and CTL contexts can't be "embedded" in a file, so
         * just read the file directly
         */
        ret = CRYPT_ReadBlobFromFile((LPCWSTR)pvObject, &fileBlob);
        blob = &fileBlob;
        break;
    case CERT_QUERY_OBJECT_BLOB:
        blob = (const CERT_BLOB *)pvObject;
        ret = TRUE;
        break;
    default:
        SetLastError(E_INVALIDARG); /* FIXME: is this the correct error? */
        ret = FALSE;
    }
    if (!ret)
        return FALSE;

    context = CRYPT_ReadSerializedElement(blob->pbData, blob->cbData,
     CERT_STORE_ALL_CONTEXT_FLAG, &contextType);
    if (context)
    {
        DWORD contentType, certStoreOffset;

        ret = TRUE;
        switch (contextType)
        {
        case CERT_STORE_CERTIFICATE_CONTEXT:
            contextInterface = pCertInterface;
            contentType = CERT_QUERY_CONTENT_SERIALIZED_CERT;
            certStoreOffset = offsetof(CERT_CONTEXT, hCertStore);
            if (!(dwExpectedContentTypeFlags &
             CERT_QUERY_CONTENT_FLAG_SERIALIZED_CERT))
            {
                SetLastError(ERROR_INVALID_DATA);
                ret = FALSE;
                goto end;
            }
            break;
        case CERT_STORE_CRL_CONTEXT:
            contextInterface = pCRLInterface;
            contentType = CERT_QUERY_CONTENT_SERIALIZED_CRL;
            certStoreOffset = offsetof(CRL_CONTEXT, hCertStore);
            if (!(dwExpectedContentTypeFlags &
             CERT_QUERY_CONTENT_FLAG_SERIALIZED_CRL))
            {
                SetLastError(ERROR_INVALID_DATA);
                ret = FALSE;
                goto end;
            }
            break;
        case CERT_STORE_CTL_CONTEXT:
            contextInterface = pCTLInterface;
            contentType = CERT_QUERY_CONTENT_SERIALIZED_CTL;
            certStoreOffset = offsetof(CTL_CONTEXT, hCertStore);
            if (!(dwExpectedContentTypeFlags &
             CERT_QUERY_CONTENT_FLAG_SERIALIZED_CTL))
            {
                SetLastError(ERROR_INVALID_DATA);
                ret = FALSE;
                goto end;
            }
            break;
        default:
            SetLastError(ERROR_INVALID_DATA);
            ret = FALSE;
            goto end;
        }
        if (pdwMsgAndCertEncodingType)
            *pdwMsgAndCertEncodingType = X509_ASN_ENCODING;
        if (pdwContentType)
            *pdwContentType = contentType;
        if (phCertStore)
            *phCertStore = CertDuplicateStore(
             *(HCERTSTORE *)((const BYTE *)context + certStoreOffset));
        if (ppvContext)
            *ppvContext = contextInterface->duplicate(context);
    }

end:
    if (contextInterface && context)
        contextInterface->free(context);
    if (blob == &fileBlob)
        CryptMemFree(blob->pbData);
    TRACE("returning %d\n", ret);
    return ret;
}

static BOOL CRYPT_QuerySerializedStoreObject(DWORD dwObjectType,
 const void *pvObject, DWORD *pdwMsgAndCertEncodingType, DWORD *pdwContentType,
 HCERTSTORE *phCertStore, HCRYPTMSG *phMsg)
{
    LPCWSTR fileName = (LPCWSTR)pvObject;
    HANDLE file;
    BOOL ret = FALSE;

    if (dwObjectType != CERT_QUERY_OBJECT_FILE)
    {
        FIXME("unimplemented for non-file type %d\n", dwObjectType);
        SetLastError(E_INVALIDARG); /* FIXME: is this the correct error? */
        return FALSE;
    }
    TRACE("%s\n", debugstr_w(fileName));
    file = CreateFileW(fileName, GENERIC_READ, FILE_SHARE_READ, NULL,
     OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
         CERT_STORE_CREATE_NEW_FLAG, NULL);

        ret = CRYPT_ReadSerializedStoreFromFile(file, store);
        if (ret)
        {
            if (pdwMsgAndCertEncodingType)
                *pdwMsgAndCertEncodingType = X509_ASN_ENCODING;
            if (pdwContentType)
                *pdwContentType = CERT_QUERY_CONTENT_SERIALIZED_STORE;
            if (phCertStore)
                *phCertStore = CertDuplicateStore(store);
        }
        CertCloseStore(store, 0);
        CloseHandle(file);
    }
    TRACE("returning %d\n", ret);
    return ret;
}

/* Used to decode non-embedded messages */
static BOOL CRYPT_QueryMessageObject(DWORD dwObjectType, const void *pvObject,
 DWORD dwExpectedContentTypeFlags, DWORD *pdwMsgAndCertEncodingType,
 DWORD *pdwContentType, HCERTSTORE *phCertStore, HCRYPTMSG *phMsg)
{
    CERT_BLOB fileBlob;
    const CERT_BLOB *blob;
    BOOL ret;
    HCRYPTMSG msg = NULL;
    DWORD encodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

    switch (dwObjectType)
    {
    case CERT_QUERY_OBJECT_FILE:
        /* This isn't an embedded PKCS7 message, so just read the file
         * directly
         */
        ret = CRYPT_ReadBlobFromFile((LPCWSTR)pvObject, &fileBlob);
        blob = &fileBlob;
        break;
    case CERT_QUERY_OBJECT_BLOB:
        blob = (const CERT_BLOB *)pvObject;
        ret = TRUE;
        break;
    default:
        SetLastError(E_INVALIDARG); /* FIXME: is this the correct error? */
        ret = FALSE;
    }
    if (!ret)
        return FALSE;

    ret = FALSE;
    /* Try it first as a PKCS content info */
    if ((dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED) ||
     (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_PKCS7_UNSIGNED))
    {
        msg = CryptMsgOpenToDecode(encodingType, 0, 0, 0, NULL, NULL);
        if (msg)
        {
            ret = CryptMsgUpdate(msg, blob->pbData, blob->cbData, TRUE);
            if (ret)
            {
                DWORD type, len = sizeof(type);

                ret = CryptMsgGetParam(msg, CMSG_TYPE_PARAM, 0, &type, &len);
                if (ret)
                {
                    if ((dwExpectedContentTypeFlags &
                     CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED))
                    {
                        if (type != CMSG_SIGNED)
                        {
                            SetLastError(ERROR_INVALID_DATA);
                            ret = FALSE;
                        }
                        else if (pdwContentType)
                            *pdwContentType = CERT_QUERY_CONTENT_PKCS7_SIGNED;
                    }
                    else if ((dwExpectedContentTypeFlags &
                     CERT_QUERY_CONTENT_FLAG_PKCS7_UNSIGNED))
                    {
                        if (type != CMSG_DATA)
                        {
                            SetLastError(ERROR_INVALID_DATA);
                            ret = FALSE;
                        }
                        else if (pdwContentType)
                            *pdwContentType = CERT_QUERY_CONTENT_PKCS7_UNSIGNED;
                    }
                }
            }
            if (!ret)
            {
                CryptMsgClose(msg);
                msg = NULL;
            }
        }
    }
    /* Failing that, try explicitly typed messages */
    if (!ret &&
     (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED))
    {
        msg = CryptMsgOpenToDecode(encodingType, 0, CMSG_SIGNED, 0, NULL, NULL);
        if (msg)
        {
            ret = CryptMsgUpdate(msg, blob->pbData, blob->cbData, TRUE);
            if (!ret)
            {
                CryptMsgClose(msg);
                msg = NULL;
            }
        }
        if (msg && pdwContentType)
            *pdwContentType = CERT_QUERY_CONTENT_PKCS7_SIGNED;
    }
    if (!ret &&
     (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_PKCS7_UNSIGNED))
    {
        msg = CryptMsgOpenToDecode(encodingType, 0, CMSG_DATA, 0, NULL, NULL);
        if (msg)
        {
            ret = CryptMsgUpdate(msg, blob->pbData, blob->cbData, TRUE);
            if (!ret)
            {
                CryptMsgClose(msg);
                msg = NULL;
            }
        }
        if (msg && pdwContentType)
            *pdwContentType = CERT_QUERY_CONTENT_PKCS7_UNSIGNED;
    }
    if (pdwMsgAndCertEncodingType)
        *pdwMsgAndCertEncodingType = encodingType;
    if (msg)
    {
        if (phMsg)
            *phMsg = msg;
        if (phCertStore)
            *phCertStore = CertOpenStore(CERT_STORE_PROV_MSG, encodingType, 0,
             0, msg);
    }
    if (blob == &fileBlob)
        CryptMemFree(blob->pbData);
    TRACE("returning %d\n", ret);
    return ret;
}

static BOOL CRYPT_QueryEmbeddedMessageObject(DWORD dwObjectType,
 const void *pvObject, DWORD dwExpectedContentTypeFlags,
 DWORD *pdwMsgAndCertEncodingType, DWORD *pdwContentType,
 HCERTSTORE *phCertStore, HCRYPTMSG *phMsg)
{
    HANDLE file;
    GUID subject;
    BOOL ret = FALSE;

    TRACE("%s\n", debugstr_w((LPCWSTR)pvObject));

    if (dwObjectType != CERT_QUERY_OBJECT_FILE)
    {
        FIXME("don't know what to do for type %d embedded signed messages\n",
         dwObjectType);
        SetLastError(E_INVALIDARG);
        return FALSE;
    }
    file = CreateFileW((LPCWSTR)pvObject, GENERIC_READ, FILE_SHARE_READ,
     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        ret = CryptSIPRetrieveSubjectGuid((LPCWSTR)pvObject, file, &subject);
        if (ret)
        {
            SIP_DISPATCH_INFO sip;

            memset(&sip, 0, sizeof(sip));
            sip.cbSize = sizeof(sip);
            ret = CryptSIPLoad(&subject, 0, &sip);
            if (ret)
            {
                SIP_SUBJECTINFO subjectInfo;
                CERT_BLOB blob;
                DWORD encodingType;

                memset(&subjectInfo, 0, sizeof(subjectInfo));
                subjectInfo.cbSize = sizeof(subjectInfo);
                subjectInfo.pgSubjectType = &subject;
                subjectInfo.hFile = file;
                subjectInfo.pwsFileName = (LPCWSTR)pvObject;
                ret = sip.pfGet(&subjectInfo, &encodingType, 0, &blob.cbData,
                 NULL);
                if (ret)
                {
                    blob.pbData = CryptMemAlloc(blob.cbData);
                    if (blob.pbData)
                    {
                        ret = sip.pfGet(&subjectInfo, &encodingType, 0,
                         &blob.cbData, blob.pbData);
                        if (ret)
                        {
                            ret = CRYPT_QueryMessageObject(
                             CERT_QUERY_OBJECT_BLOB, &blob,
                             CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED,
                             pdwMsgAndCertEncodingType, NULL, phCertStore,
                             phMsg);
                            if (ret && pdwContentType)
                                *pdwContentType =
                                 CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED;
                        }
                        CryptMemFree(blob.pbData);
                    }
                    else
                    {
                        SetLastError(ERROR_OUTOFMEMORY);
                        ret = FALSE;
                    }
                }
            }
        }
        CloseHandle(file);
    }
    TRACE("returning %d\n", ret);
    return ret;
}

BOOL WINAPI CryptQueryObject(DWORD dwObjectType, const void *pvObject,
 DWORD dwExpectedContentTypeFlags, DWORD dwExpectedFormatTypeFlags,
 DWORD dwFlags, DWORD *pdwMsgAndCertEncodingType, DWORD *pdwContentType,
 DWORD *pdwFormatType, HCERTSTORE *phCertStore, HCRYPTMSG *phMsg,
 const void **ppvContext)
{
    static const DWORD unimplementedTypes =
     CERT_QUERY_CONTENT_FLAG_PKCS10 | CERT_QUERY_CONTENT_FLAG_PFX |
     CERT_QUERY_CONTENT_FLAG_CERT_PAIR;
    BOOL ret = TRUE;

    TRACE("(%08x, %p, %08x, %08x, %08x, %p, %p, %p, %p, %p, %p)\n",
     dwObjectType, pvObject, dwExpectedContentTypeFlags,
     dwExpectedFormatTypeFlags, dwFlags, pdwMsgAndCertEncodingType,
     pdwContentType, pdwFormatType, phCertStore, phMsg, ppvContext);

    if (dwExpectedContentTypeFlags & unimplementedTypes)
        WARN("unimplemented for types %08x\n",
         dwExpectedContentTypeFlags & unimplementedTypes);
    if (!(dwExpectedFormatTypeFlags & CERT_QUERY_FORMAT_FLAG_BINARY))
    {
        FIXME("unimplemented for anything but binary\n");
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return FALSE;
    }
    if (pdwFormatType)
        *pdwFormatType = CERT_QUERY_FORMAT_BINARY;

    if (phCertStore)
        *phCertStore = NULL;
    if (phMsg)
        *phMsg = NULL;
    if (ppvContext)
        *ppvContext = NULL;

    ret = FALSE;
    if ((dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_CERT) ||
     (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_CRL) ||
     (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_CTL))
    {
        ret = CRYPT_QueryContextObject(dwObjectType, pvObject,
         dwExpectedContentTypeFlags, pdwMsgAndCertEncodingType, pdwContentType,
         phCertStore, ppvContext);
    }
    if (!ret &&
     (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_SERIALIZED_STORE))
    {
        ret = CRYPT_QuerySerializedStoreObject(dwObjectType, pvObject,
         pdwMsgAndCertEncodingType, pdwContentType, phCertStore, phMsg);
    }
    if (!ret &&
     ((dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_SERIALIZED_CERT) ||
     (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_SERIALIZED_CRL) ||
     (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_SERIALIZED_CTL)))
    {
        ret = CRYPT_QuerySerializedContextObject(dwObjectType, pvObject,
         dwExpectedContentTypeFlags, pdwMsgAndCertEncodingType, pdwContentType,
         phCertStore, ppvContext);
    }
    if (!ret &&
     ((dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED) ||
     (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_PKCS7_UNSIGNED)))
    {
        ret = CRYPT_QueryMessageObject(dwObjectType, pvObject,
         dwExpectedContentTypeFlags, pdwMsgAndCertEncodingType, pdwContentType,
         phCertStore, phMsg);
    }
    if (!ret &&
     (dwExpectedContentTypeFlags & CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED))
    {
        ret = CRYPT_QueryEmbeddedMessageObject(dwObjectType, pvObject,
         dwExpectedContentTypeFlags, pdwMsgAndCertEncodingType, pdwContentType,
         phCertStore, phMsg);
    }
    TRACE("returning %d\n", ret);
    return ret;
}

static BOOL WINAPI CRYPT_FormatHexString(DWORD dwCertEncodingType,
 DWORD dwFormatType, DWORD dwFormatStrType, void *pFormatStruct,
 LPCSTR lpszStructType, const BYTE *pbEncoded, DWORD cbEncoded, void *pbFormat,
 DWORD *pcbFormat)
{
    BOOL ret;
    DWORD bytesNeeded;

    if (cbEncoded)
        bytesNeeded = (cbEncoded * 3) * sizeof(WCHAR);
    else
        bytesNeeded = sizeof(WCHAR);
    if (!pbFormat)
    {
        *pcbFormat = bytesNeeded;
        ret = TRUE;
    }
    else if (*pcbFormat < bytesNeeded)
    {
        *pcbFormat = bytesNeeded;
        SetLastError(ERROR_MORE_DATA);
        ret = FALSE;
    }
    else
    {
        static const WCHAR fmt[] = { '%','0','2','x',' ',0 };
        static const WCHAR endFmt[] = { '%','0','2','x',0 };
        DWORD i;
        LPWSTR ptr = pbFormat;

        *pcbFormat = bytesNeeded;
        if (cbEncoded)
        {
            for (i = 0; i < cbEncoded; i++)
            {
                if (i < cbEncoded - 1)
                    ptr += sprintfW(ptr, fmt, pbEncoded[i]);
                else
                    ptr += sprintfW(ptr, endFmt, pbEncoded[i]);
            }
        }
        else
            *ptr = 0;
        ret = TRUE;
    }
    return ret;
}

#define MAX_STRING_RESOURCE_LEN 128

static const WCHAR crlf[] = { '\r','\n',0 };
static const WCHAR commaSpace[] = { ',',' ',0 };

static WCHAR subjectTypeHeader[MAX_STRING_RESOURCE_LEN];
static WCHAR subjectTypeCA[MAX_STRING_RESOURCE_LEN];
static WCHAR subjectTypeEndCert[MAX_STRING_RESOURCE_LEN];
static WCHAR pathLengthHeader[MAX_STRING_RESOURCE_LEN];

static BOOL WINAPI CRYPT_FormatBasicConstraints2(DWORD dwCertEncodingType,
 DWORD dwFormatType, DWORD dwFormatStrType, void *pFormatStruct,
 LPCSTR lpszStructType, const BYTE *pbEncoded, DWORD cbEncoded, void *pbFormat,
 DWORD *pcbFormat)
{
    DWORD size;
    CERT_BASIC_CONSTRAINTS2_INFO *info;
    BOOL ret;

    if (!cbEncoded)
    {
        SetLastError(E_INVALIDARG);
        return FALSE;
    }
    if ((ret = CryptDecodeObjectEx(dwCertEncodingType, X509_BASIC_CONSTRAINTS2,
     pbEncoded, cbEncoded, CRYPT_DECODE_ALLOC_FLAG, NULL, &info, &size)))
    {
        static const WCHAR pathFmt[] = { '%','d',0 };
        static BOOL stringsLoaded = FALSE;
        DWORD bytesNeeded = sizeof(WCHAR); /* space for the NULL terminator */
        WCHAR pathLength[MAX_STRING_RESOURCE_LEN];
        LPCWSTR sep, subjectType;
        DWORD sepLen;

        if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
        {
            sep = crlf;
            sepLen = strlenW(crlf) * sizeof(WCHAR);
        }
        else
        {
            sep = commaSpace;
            sepLen = strlenW(commaSpace) * sizeof(WCHAR);
        }

        if (!stringsLoaded)
        {
            LoadStringW(hInstance, IDS_SUBJECT_TYPE, subjectTypeHeader,
             sizeof(subjectTypeHeader) / sizeof(subjectTypeHeader[0]));
            LoadStringW(hInstance, IDS_SUBJECT_TYPE_CA, subjectTypeCA,
             sizeof(subjectTypeCA) / sizeof(subjectTypeCA[0]));
            LoadStringW(hInstance, IDS_SUBJECT_TYPE_END_CERT,
             subjectTypeEndCert,
             sizeof(subjectTypeEndCert) / sizeof(subjectTypeEndCert[0]));
            LoadStringW(hInstance, IDS_PATH_LENGTH, pathLengthHeader,
             sizeof(pathLengthHeader) / sizeof(pathLengthHeader[0]));
            stringsLoaded = TRUE;
        }
        bytesNeeded += strlenW(subjectTypeHeader) * sizeof(WCHAR);
        if (info->fCA)
            subjectType = subjectTypeCA;
        else
            subjectType = subjectTypeEndCert;
        bytesNeeded += strlenW(subjectType) * sizeof(WCHAR);
        bytesNeeded += sepLen;
        bytesNeeded += strlenW(pathLengthHeader) * sizeof(WCHAR);
        if (info->fPathLenConstraint)
            sprintfW(pathLength, pathFmt, info->dwPathLenConstraint);
        else
            LoadStringW(hInstance, IDS_PATH_LENGTH_NONE, pathLength,
             sizeof(pathLength) / sizeof(pathLength[0]));
        bytesNeeded += strlenW(pathLength) * sizeof(WCHAR);
        if (!pbFormat)
            *pcbFormat = bytesNeeded;
        else if (*pcbFormat < bytesNeeded)
        {
            *pcbFormat = bytesNeeded;
            SetLastError(ERROR_MORE_DATA);
            ret = FALSE;
        }
        else
        {
            LPWSTR str = pbFormat;

            *pcbFormat = bytesNeeded;
            strcpyW(str, subjectTypeHeader);
            str += strlenW(subjectTypeHeader);
            strcpyW(str, subjectType);
            str += strlenW(subjectType);
            strcpyW(str, sep);
            str += sepLen / sizeof(WCHAR);
            strcpyW(str, pathLengthHeader);
            str += strlenW(pathLengthHeader);
            strcpyW(str, pathLength);
            str += strlenW(pathLength);
        }
        LocalFree(info);
    }
    return ret;
}

static BOOL CRYPT_FormatHexStringWithPrefix(CRYPT_DATA_BLOB *blob, int id,
 LPWSTR str, DWORD *pcbStr)
{
    WCHAR buf[MAX_STRING_RESOURCE_LEN];
    DWORD bytesNeeded;
    BOOL ret;

    LoadStringW(hInstance, id, buf, sizeof(buf) / sizeof(buf[0]));
    CRYPT_FormatHexString(X509_ASN_ENCODING, 0, 0, NULL, NULL,
     blob->pbData, blob->cbData, NULL, &bytesNeeded);
    bytesNeeded += strlenW(buf) * sizeof(WCHAR);
    if (!str)
    {
        *pcbStr = bytesNeeded;
        ret = TRUE;
    }
    else if (*pcbStr < bytesNeeded)
    {
        *pcbStr = bytesNeeded;
        SetLastError(ERROR_MORE_DATA);
        ret = FALSE;
    }
    else
    {
        *pcbStr = bytesNeeded;
        strcpyW(str, buf);
        str += strlenW(str);
        bytesNeeded -= strlenW(str) * sizeof(WCHAR);
        ret = CRYPT_FormatHexString(X509_ASN_ENCODING, 0, 0, NULL, NULL,
         blob->pbData, blob->cbData, str, &bytesNeeded);
    }
    return ret;
}

static BOOL CRYPT_FormatKeyId(CRYPT_DATA_BLOB *keyId, LPWSTR str,
 DWORD *pcbStr)
{
    return CRYPT_FormatHexStringWithPrefix(keyId, IDS_KEY_ID, str, pcbStr);
}

static BOOL CRYPT_FormatCertSerialNumber(CRYPT_DATA_BLOB *serialNum, LPWSTR str,
 DWORD *pcbStr)
{
    return CRYPT_FormatHexStringWithPrefix(serialNum, IDS_CERT_SERIAL_NUMBER,
     str, pcbStr);
}

static const WCHAR indent[] = { ' ',' ',' ',' ',' ',0 };

static BOOL CRYPT_FormatAltNameEntry(DWORD dwFormatStrType, DWORD indentLevel,
 CERT_ALT_NAME_ENTRY *entry, LPWSTR str, DWORD *pcbStr)
{
    BOOL ret;
    WCHAR buf[MAX_STRING_RESOURCE_LEN];
    WCHAR mask[MAX_STRING_RESOURCE_LEN];
    WCHAR ipAddrBuf[32];
    WCHAR maskBuf[16];
    DWORD bytesNeeded = sizeof(WCHAR);

    if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
        bytesNeeded += indentLevel * strlenW(indent) * sizeof(WCHAR);
    switch (entry->dwAltNameChoice)
    {
    case CERT_ALT_NAME_RFC822_NAME:
        LoadStringW(hInstance, IDS_ALT_NAME_RFC822_NAME, buf,
         sizeof(buf) / sizeof(buf[0]));
        bytesNeeded += strlenW(entry->u.pwszRfc822Name) * sizeof(WCHAR);
        ret = TRUE;
        break;
    case CERT_ALT_NAME_DNS_NAME:
        LoadStringW(hInstance, IDS_ALT_NAME_DNS_NAME, buf,
         sizeof(buf) / sizeof(buf[0]));
        bytesNeeded += strlenW(entry->u.pwszDNSName) * sizeof(WCHAR);
        ret = TRUE;
        break;
    case CERT_ALT_NAME_URL:
        LoadStringW(hInstance, IDS_ALT_NAME_URL, buf,
         sizeof(buf) / sizeof(buf[0]));
        bytesNeeded += strlenW(entry->u.pwszURL) * sizeof(WCHAR);
        ret = TRUE;
        break;
    case CERT_ALT_NAME_IP_ADDRESS:
    {
        static const WCHAR ipAddrWithMaskFmt[] = { '%','d','.','%','d','.',
         '%','d','.','%','d','/','%','d','.','%','d','.','%','d','.','%','d',0
        };
        static const WCHAR ipAddrFmt[] = { '%','d','.','%','d','.','%','d',
         '.','%','d',0 };

        LoadStringW(hInstance, IDS_ALT_NAME_IP_ADDRESS, buf,
         sizeof(buf) / sizeof(buf[0]));
        if (entry->u.IPAddress.cbData == 8)
        {
            if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
            {
                LoadStringW(hInstance, IDS_ALT_NAME_MASK, mask,
                 sizeof(mask) / sizeof(mask[0]));
                bytesNeeded += strlenW(mask) * sizeof(WCHAR);
                sprintfW(ipAddrBuf, ipAddrFmt,
                 entry->u.IPAddress.pbData[0],
                 entry->u.IPAddress.pbData[1],
                 entry->u.IPAddress.pbData[2],
                 entry->u.IPAddress.pbData[3]);
                bytesNeeded += strlenW(ipAddrBuf) * sizeof(WCHAR);
                /* indent again, for the mask line */
                bytesNeeded += indentLevel * strlenW(indent) * sizeof(WCHAR);
                sprintfW(maskBuf, ipAddrFmt,
                 entry->u.IPAddress.pbData[4],
                 entry->u.IPAddress.pbData[5],
                 entry->u.IPAddress.pbData[6],
                 entry->u.IPAddress.pbData[7]);
                bytesNeeded += strlenW(maskBuf) * sizeof(WCHAR);
                bytesNeeded += strlenW(crlf) * sizeof(WCHAR);
            }
            else
            {
                sprintfW(ipAddrBuf, ipAddrWithMaskFmt,
                 entry->u.IPAddress.pbData[0],
                 entry->u.IPAddress.pbData[1],
                 entry->u.IPAddress.pbData[2],
                 entry->u.IPAddress.pbData[3],
                 entry->u.IPAddress.pbData[4],
                 entry->u.IPAddress.pbData[5],
                 entry->u.IPAddress.pbData[6],
                 entry->u.IPAddress.pbData[7]);
                bytesNeeded += (strlenW(ipAddrBuf) + 1) * sizeof(WCHAR);
            }
            ret = TRUE;
        }
        else
        {
            FIXME("unknown IP address format (%d bytes)\n",
             entry->u.IPAddress.cbData);
            ret = FALSE;
        }
        break;
    }
    default:
        FIXME("unimplemented for %d\n", entry->dwAltNameChoice);
        ret = FALSE;
    }
    if (ret)
    {
        bytesNeeded += strlenW(buf) * sizeof(WCHAR);
        if (!str)
            *pcbStr = bytesNeeded;
        else if (*pcbStr < bytesNeeded)
        {
            *pcbStr = bytesNeeded;
            SetLastError(ERROR_MORE_DATA);
            ret = FALSE;
        }
        else
        {
            DWORD i;

            *pcbStr = bytesNeeded;
            if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
            {
                for (i = 0; i < indentLevel; i++)
                {
                    strcpyW(str, indent);
                    str += strlenW(indent);
                }
            }
            strcpyW(str, buf);
            str += strlenW(str);
            switch (entry->dwAltNameChoice)
            {
            case CERT_ALT_NAME_RFC822_NAME:
            case CERT_ALT_NAME_DNS_NAME:
            case CERT_ALT_NAME_URL:
                strcpyW(str, entry->u.pwszURL);
                break;
            case CERT_ALT_NAME_IP_ADDRESS:
                if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
                {
                    strcpyW(str, ipAddrBuf);
                    str += strlenW(ipAddrBuf);
                    strcpyW(str, crlf);
                    str += strlenW(crlf);
                    if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
                    {
                        for (i = 0; i < indentLevel; i++)
                        {
                            strcpyW(str, indent);
                            str += strlenW(indent);
                        }
                    }
                    strcpyW(str, mask);
                    str += strlenW(mask);
                    strcpyW(str, maskBuf);
                }
                else
                    strcpyW(str, ipAddrBuf);
                break;
            }
        }
    }
    return ret;
}

static BOOL CRYPT_FormatAltNameInfo(DWORD dwFormatStrType, DWORD indentLevel,
 CERT_ALT_NAME_INFO *name, LPWSTR str, DWORD *pcbStr)
{
    DWORD i, size, bytesNeeded = 0;
    BOOL ret = TRUE;
    LPCWSTR sep;
    DWORD sepLen;

    if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
    {
        sep = crlf;
        sepLen = strlenW(crlf) * sizeof(WCHAR);
    }
    else
    {
        sep = commaSpace;
        sepLen = strlenW(commaSpace) * sizeof(WCHAR);
    }

    for (i = 0; ret && i < name->cAltEntry; i++)
    {
        ret = CRYPT_FormatAltNameEntry(dwFormatStrType, indentLevel,
         &name->rgAltEntry[i], NULL, &size);
        if (ret)
        {
            bytesNeeded += size - sizeof(WCHAR);
            if (i < name->cAltEntry - 1)
                bytesNeeded += sepLen;
        }
    }
    if (ret)
    {
        bytesNeeded += sizeof(WCHAR);
        if (!str)
            *pcbStr = bytesNeeded;
        else if (*pcbStr < bytesNeeded)
        {
            *pcbStr = bytesNeeded;
            SetLastError(ERROR_MORE_DATA);
            ret = FALSE;
        }
        else
        {
            *pcbStr = bytesNeeded;
            for (i = 0; ret && i < name->cAltEntry; i++)
            {
                ret = CRYPT_FormatAltNameEntry(dwFormatStrType, indentLevel,
                 &name->rgAltEntry[i], str, &size);
                if (ret)
                {
                    str += size / sizeof(WCHAR) - 1;
                    if (i < name->cAltEntry - 1)
                    {
                        strcpyW(str, sep);
                        str += sepLen / sizeof(WCHAR);
                    }
                }
            }
        }
    }
    return ret;
}

static BOOL CRYPT_FormatCertIssuer(DWORD dwFormatStrType,
 CERT_ALT_NAME_INFO *issuer, LPWSTR str, DWORD *pcbStr)
{
    WCHAR buf[MAX_STRING_RESOURCE_LEN];
    DWORD bytesNeeded;
    BOOL ret;

    LoadStringW(hInstance, IDS_CERT_ISSUER, buf, sizeof(buf) / sizeof(buf[0]));
    ret = CRYPT_FormatAltNameInfo(dwFormatStrType, 0, issuer, NULL,
     &bytesNeeded);
    bytesNeeded += strlenW(buf) * sizeof(WCHAR);
    if (ret)
    {
        if (!str)
            *pcbStr = bytesNeeded;
        else if (*pcbStr < bytesNeeded)
        {
            *pcbStr = bytesNeeded;
            SetLastError(ERROR_MORE_DATA);
            ret = FALSE;
        }
        else
        {
            *pcbStr = bytesNeeded;
            strcpyW(str, buf);
            str += strlenW(str);
            bytesNeeded -= strlenW(str) * sizeof(WCHAR);
            ret = CRYPT_FormatAltNameInfo(dwFormatStrType, 0, issuer, str,
             &bytesNeeded);
        }
    }
    return ret;
}

static BOOL WINAPI CRYPT_FormatAuthorityKeyId2(DWORD dwCertEncodingType,
 DWORD dwFormatType, DWORD dwFormatStrType, void *pFormatStruct,
 LPCSTR lpszStructType, const BYTE *pbEncoded, DWORD cbEncoded, void *pbFormat,
 DWORD *pcbFormat)
{
    CERT_AUTHORITY_KEY_ID2_INFO *info;
    DWORD size;
    BOOL ret = FALSE;

    if (!cbEncoded)
    {
        SetLastError(E_INVALIDARG);
        return FALSE;
    }
    if ((ret = CryptDecodeObjectEx(dwCertEncodingType, X509_AUTHORITY_KEY_ID2,
     pbEncoded, cbEncoded, CRYPT_DECODE_ALLOC_FLAG, NULL, &info, &size)))
    {
        DWORD bytesNeeded = sizeof(WCHAR); /* space for the NULL terminator */
        LPCWSTR sep;
        DWORD sepLen;
        BOOL needSeparator = FALSE;

        if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
        {
            sep = crlf;
            sepLen = strlenW(crlf) * sizeof(WCHAR);
        }
        else
        {
            sep = commaSpace;
            sepLen = strlenW(commaSpace) * sizeof(WCHAR);
        }

        if (info->KeyId.cbData)
        {
            needSeparator = TRUE;
            ret = CRYPT_FormatKeyId(&info->KeyId, NULL, &size);
            if (ret)
            {
                /* don't include NULL-terminator more than once */
                bytesNeeded += size - sizeof(WCHAR);
            }
        }
        if (info->AuthorityCertIssuer.cAltEntry)
        {
            if (needSeparator)
                bytesNeeded += sepLen;
            needSeparator = TRUE;
            ret = CRYPT_FormatCertIssuer(dwFormatStrType,
             &info->AuthorityCertIssuer, NULL, &size);
            if (ret)
            {
                /* don't include NULL-terminator more than once */
                bytesNeeded += size - sizeof(WCHAR);
            }
        }
        if (info->AuthorityCertSerialNumber.cbData)
        {
            if (needSeparator)
                bytesNeeded += sepLen;
            ret = CRYPT_FormatCertSerialNumber(
             &info->AuthorityCertSerialNumber, NULL, &size);
            if (ret)
            {
                /* don't include NULL-terminator more than once */
                bytesNeeded += size - sizeof(WCHAR);
            }
        }
        if (ret)
        {
            if (!pbFormat)
                *pcbFormat = bytesNeeded;
            else if (*pcbFormat < bytesNeeded)
            {
                *pcbFormat = bytesNeeded;
                SetLastError(ERROR_MORE_DATA);
                ret = FALSE;
            }
            else
            {
                LPWSTR str = pbFormat;

                *pcbFormat = bytesNeeded;
                needSeparator = FALSE;
                if (info->KeyId.cbData)
                {
                    needSeparator = TRUE;
                    /* Overestimate size available, it's already been checked
                     * above.
                     */
                    size = bytesNeeded;
                    ret = CRYPT_FormatKeyId(&info->KeyId, str, &size);
                    if (ret)
                        str += size / sizeof(WCHAR) - 1;
                }
                if (info->AuthorityCertIssuer.cAltEntry)
                {
                    if (needSeparator)
                    {
                        strcpyW(str, sep);
                        str += sepLen / sizeof(WCHAR);
                    }
                    needSeparator = TRUE;
                    /* Overestimate size available, it's already been checked
                     * above.
                     */
                    size = bytesNeeded;
                    ret = CRYPT_FormatCertIssuer(dwFormatStrType,
                     &info->AuthorityCertIssuer, str, &size);
                    if (ret)
                        str += size / sizeof(WCHAR) - 1;
                }
                if (info->AuthorityCertSerialNumber.cbData)
                {
                    if (needSeparator)
                    {
                        strcpyW(str, sep);
                        str += sepLen / sizeof(WCHAR);
                    }
                    /* Overestimate size available, it's already been checked
                     * above.
                     */
                    size = bytesNeeded;
                    ret = CRYPT_FormatCertSerialNumber(
                     &info->AuthorityCertSerialNumber, str, &size);
                }
            }
        }
        LocalFree(info);
    }
    return ret;
}

static const WCHAR colonCrlf[] = { ':','\r','\n',0 };

static WCHAR aia[MAX_STRING_RESOURCE_LEN];
static WCHAR accessMethod[MAX_STRING_RESOURCE_LEN];
static WCHAR ocsp[MAX_STRING_RESOURCE_LEN];
static WCHAR caIssuers[MAX_STRING_RESOURCE_LEN];
static WCHAR unknown[MAX_STRING_RESOURCE_LEN];
static WCHAR accessLocation[MAX_STRING_RESOURCE_LEN];
static BOOL WINAPI CRYPT_FormatAuthorityInfoAccess(DWORD dwCertEncodingType,

 DWORD dwFormatType, DWORD dwFormatStrType, void *pFormatStruct,
 LPCSTR lpszStructType, const BYTE *pbEncoded, DWORD cbEncoded, void *pbFormat,
 DWORD *pcbFormat)
{
    CERT_AUTHORITY_INFO_ACCESS *info;
    DWORD size;
    BOOL ret = FALSE;

    if (!cbEncoded)
    {
        SetLastError(E_INVALIDARG);
        return FALSE;
    }
    if ((ret = CryptDecodeObjectEx(dwCertEncodingType,
     X509_AUTHORITY_INFO_ACCESS, pbEncoded, cbEncoded, CRYPT_DECODE_ALLOC_FLAG,
     NULL, &info, &size)))
    {
        DWORD bytesNeeded = sizeof(WCHAR);

        if (!info->cAccDescr)
        {
            WCHAR infoNotAvailable[MAX_STRING_RESOURCE_LEN];

            LoadStringW(hInstance, IDS_INFO_NOT_AVAILABLE, infoNotAvailable,
             sizeof(infoNotAvailable) / sizeof(infoNotAvailable[0]));
            bytesNeeded += strlenW(infoNotAvailable) * sizeof(WCHAR);
            if (!pbFormat)
                *pcbFormat = bytesNeeded;
            else if (*pcbFormat < bytesNeeded)
            {
                *pcbFormat = bytesNeeded;
                SetLastError(ERROR_MORE_DATA);
                ret = FALSE;
            }
            else
            {
                *pcbFormat = bytesNeeded;
                strcpyW((LPWSTR)pbFormat, infoNotAvailable);
            }
        }
        else
        {
            static const WCHAR colonSep[] = { ':',' ',0 };
            static const WCHAR numFmt[] = { '%','d',0 };
            static const WCHAR equal[] = { '=',0 };
            static BOOL stringsLoaded = FALSE;
            DWORD i;
            LPCWSTR headingSep, accessMethodSep, locationSep;
            WCHAR accessDescrNum[11];

            if (!stringsLoaded)
            {
                LoadStringW(hInstance, IDS_AIA, aia,
                 sizeof(aia) / sizeof(aia[0]));
                LoadStringW(hInstance, IDS_ACCESS_METHOD, accessMethod,
                 sizeof(accessMethod) / sizeof(accessMethod[0]));
                LoadStringW(hInstance, IDS_ACCESS_METHOD_OCSP, ocsp,
                 sizeof(ocsp) / sizeof(ocsp[0]));
                LoadStringW(hInstance, IDS_ACCESS_METHOD_CA_ISSUERS, caIssuers,
                 sizeof(caIssuers) / sizeof(caIssuers[0]));
                LoadStringW(hInstance, IDS_ACCESS_METHOD_UNKNOWN, unknown,
                 sizeof(unknown) / sizeof(unknown[0]));
                LoadStringW(hInstance, IDS_ACCESS_LOCATION, accessLocation,
                 sizeof(accessLocation) / sizeof(accessLocation[0]));
                stringsLoaded = TRUE;
            }
            if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
            {
                headingSep = crlf;
                accessMethodSep = crlf;
                locationSep = colonCrlf;
            }
            else
            {
                headingSep = colonSep;
                accessMethodSep = commaSpace;
                locationSep = equal;
            }

            for (i = 0; ret && i < info->cAccDescr; i++)
            {
                /* Heading */
                bytesNeeded += sizeof(WCHAR); /* left bracket */
                sprintfW(accessDescrNum, numFmt, i + 1);
                bytesNeeded += strlenW(accessDescrNum) * sizeof(WCHAR);
                bytesNeeded += sizeof(WCHAR); /* right bracket */
                bytesNeeded += strlenW(aia) * sizeof(WCHAR);
                bytesNeeded += strlenW(headingSep) * sizeof(WCHAR);
                /* Access method */
                bytesNeeded += strlenW(accessMethod) * sizeof(WCHAR);
                if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
                    bytesNeeded += strlenW(indent) * sizeof(WCHAR);
                if (!strcmp(info->rgAccDescr[i].pszAccessMethod,
                 szOID_PKIX_OCSP))
                    bytesNeeded += strlenW(ocsp) * sizeof(WCHAR);
                else if (!strcmp(info->rgAccDescr[i].pszAccessMethod,
                 szOID_PKIX_CA_ISSUERS))
                    bytesNeeded += strlenW(caIssuers) * sizeof(caIssuers);
                else
                    bytesNeeded += strlenW(unknown) * sizeof(WCHAR);
                bytesNeeded += sizeof(WCHAR); /* space */
                bytesNeeded += sizeof(WCHAR); /* left paren */
                bytesNeeded += strlen(info->rgAccDescr[i].pszAccessMethod)
                 * sizeof(WCHAR);
                bytesNeeded += sizeof(WCHAR); /* right paren */
                /* Delimiter between access method and location */
                bytesNeeded += strlenW(accessMethodSep) * sizeof(WCHAR);
                if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
                    bytesNeeded += strlenW(indent) * sizeof(WCHAR);
                bytesNeeded += strlenW(accessLocation) * sizeof(WCHAR);
                bytesNeeded += strlenW(locationSep) * sizeof(WCHAR);
                ret = CRYPT_FormatAltNameEntry(dwFormatStrType, 2,
                 &info->rgAccDescr[i].AccessLocation, NULL, &size);
                if (ret)
                    bytesNeeded += size - sizeof(WCHAR);
                /* Need extra delimiter between access method entries */
                if (i < info->cAccDescr - 1)
                    bytesNeeded += strlenW(accessMethodSep) * sizeof(WCHAR);
            }
            if (ret)
            {
                if (!pbFormat)
                    *pcbFormat = bytesNeeded;
                else if (*pcbFormat < bytesNeeded)
                {
                    *pcbFormat = bytesNeeded;
                    SetLastError(ERROR_MORE_DATA);
                    ret = FALSE;
                }
                else
                {
                    LPWSTR str = pbFormat;
                    DWORD altNameEntrySize;

                    *pcbFormat = bytesNeeded;
                    for (i = 0; ret && i < info->cAccDescr; i++)
                    {
                        LPCSTR oidPtr;

                        *str++ = '[';
                        sprintfW(accessDescrNum, numFmt, i + 1);
                        strcpyW(str, accessDescrNum);
                        str += strlenW(accessDescrNum);
                        *str++ = ']';
                        strcpyW(str, aia);
                        str += strlenW(aia);
                        strcpyW(str, headingSep);
                        str += strlenW(headingSep);
                        if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
                        {
                            strcpyW(str, indent);
                            str += strlenW(indent);
                        }
                        strcpyW(str, accessMethod);
                        str += strlenW(accessMethod);
                        if (!strcmp(info->rgAccDescr[i].pszAccessMethod,
                         szOID_PKIX_OCSP))
                        {
                            strcpyW(str, ocsp);
                            str += strlenW(ocsp);
                        }
                        else if (!strcmp(info->rgAccDescr[i].pszAccessMethod,
                         szOID_PKIX_CA_ISSUERS))
                        {
                            strcpyW(str, caIssuers);
                            str += strlenW(caIssuers);
                        }
                        else
                        {
                            strcpyW(str, unknown);
                            str += strlenW(unknown);
                        }
                        *str++ = ' ';
                        *str++ = '(';
                        for (oidPtr = info->rgAccDescr[i].pszAccessMethod;
                         *oidPtr; oidPtr++, str++)
                            *str = *oidPtr;
                        *str++ = ')';
                        strcpyW(str, accessMethodSep);
                        str += strlenW(accessMethodSep);
                        if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
                        {
                            strcpyW(str, indent);
                            str += strlenW(indent);
                        }
                        strcpyW(str, accessLocation);
                        str += strlenW(accessLocation);
                        strcpyW(str, locationSep);
                        str += strlenW(locationSep);
                        /* This overestimates the size available, but that
                         * won't matter since we checked earlier whether enough
                         * space for the entire string was available.
                         */
                        altNameEntrySize = bytesNeeded;
                        ret = CRYPT_FormatAltNameEntry(dwFormatStrType, 2,
                         &info->rgAccDescr[i].AccessLocation, str,
                         &altNameEntrySize);
                        if (ret)
                            str += altNameEntrySize / sizeof(WCHAR) - 1;
                        if (i < info->cAccDescr - 1)
                        {
                            strcpyW(str, accessMethodSep);
                            str += strlenW(accessMethodSep);
                        }
                    }
                }
            }
        }
        LocalFree(info);
    }
    return ret;
}

static WCHAR keyCompromise[MAX_STRING_RESOURCE_LEN];
static WCHAR caCompromise[MAX_STRING_RESOURCE_LEN];
static WCHAR affiliationChanged[MAX_STRING_RESOURCE_LEN];
static WCHAR superceded[MAX_STRING_RESOURCE_LEN];
static WCHAR operationCeased[MAX_STRING_RESOURCE_LEN];
static WCHAR certificateHold[MAX_STRING_RESOURCE_LEN];

struct reason_map_entry
{
    BYTE   reasonBit;
    LPWSTR reason;
    int    id;
};
static struct reason_map_entry reason_map[] = {
 { CRL_REASON_KEY_COMPROMISE_FLAG, keyCompromise, IDS_REASON_KEY_COMPROMISE },
 { CRL_REASON_CA_COMPROMISE_FLAG, caCompromise, IDS_REASON_CA_COMPROMISE },
 { CRL_REASON_AFFILIATION_CHANGED_FLAG, affiliationChanged,
   IDS_REASON_AFFILIATION_CHANGED },
 { CRL_REASON_SUPERSEDED_FLAG, superceded, IDS_REASON_SUPERCEDED },
 { CRL_REASON_CESSATION_OF_OPERATION_FLAG, operationCeased,
   IDS_REASON_CESSATION_OF_OPERATION },
 { CRL_REASON_CERTIFICATE_HOLD_FLAG, certificateHold,
   IDS_REASON_CERTIFICATE_HOLD },
};

static BOOL CRYPT_FormatReason(DWORD dwFormatStrType,
 const CRYPT_BIT_BLOB *reasonFlags, LPWSTR str, DWORD *pcbStr)
{
    static const WCHAR sep[] = { ',',' ',0 };
    static const WCHAR bitsFmt[] = { ' ','(','%','0','2','x',')',0 };
    static BOOL stringsLoaded = FALSE;
    int i, numReasons = 0;
    BOOL ret = TRUE;
    DWORD bytesNeeded = sizeof(WCHAR);
    WCHAR bits[6];

    if (!stringsLoaded)
    {
        for (i = 0; i < sizeof(reason_map) / sizeof(reason_map[0]); i++)
            LoadStringW(hInstance, reason_map[i].id, reason_map[i].reason,
             MAX_STRING_RESOURCE_LEN);
        stringsLoaded = TRUE;
    }
    /* No need to check reasonFlags->cbData, we already know it's positive.
     * Ignore any other bytes, as they're for undefined bits.
     */
    for (i = 0; i < sizeof(reason_map) / sizeof(reason_map[0]); i++)
    {
        if (reasonFlags->pbData[0] & reason_map[i].reasonBit)
        {
            bytesNeeded += strlenW(reason_map[i].reason) * sizeof(WCHAR);
            if (numReasons++)
                bytesNeeded += strlenW(sep) * sizeof(WCHAR);
        }
    }
    sprintfW(bits, bitsFmt, reasonFlags->pbData[0]);
    bytesNeeded += strlenW(bits);
    if (!str)
        *pcbStr = bytesNeeded;
    else if (*pcbStr < bytesNeeded)
    {
        *pcbStr = bytesNeeded;
        SetLastError(ERROR_MORE_DATA);
        ret = FALSE;
    }
    else
    {
        *pcbStr = bytesNeeded;
        for (i = 0; i < sizeof(reason_map) / sizeof(reason_map[0]); i++)
        {
            if (reasonFlags->pbData[0] & reason_map[i].reasonBit)
            {
                strcpyW(str, reason_map[i].reason);
                str += strlenW(reason_map[i].reason);
                if (i < sizeof(reason_map) / sizeof(reason_map[0]) - 1 &&
                 numReasons)
                {
                    strcpyW(str, sep);
                    str += strlenW(sep);
                }
            }
        }
        strcpyW(str, bits);
    }
    return ret;
}

static WCHAR crlDistPoint[MAX_STRING_RESOURCE_LEN];
static WCHAR distPointName[MAX_STRING_RESOURCE_LEN];
static WCHAR fullName[MAX_STRING_RESOURCE_LEN];
static WCHAR rdnName[MAX_STRING_RESOURCE_LEN];
static WCHAR reason[MAX_STRING_RESOURCE_LEN];
static WCHAR issuer[MAX_STRING_RESOURCE_LEN];

static BOOL WINAPI CRYPT_FormatCRLDistPoints(DWORD dwCertEncodingType,
 DWORD dwFormatType, DWORD dwFormatStrType, void *pFormatStruct,
 LPCSTR lpszStructType, const BYTE *pbEncoded, DWORD cbEncoded, void *pbFormat,
 DWORD *pcbFormat)
{
    CRL_DIST_POINTS_INFO *info;
    DWORD size;
    BOOL ret = FALSE;

    if (!cbEncoded)
    {
        SetLastError(E_INVALIDARG);
        return FALSE;
    }
    if ((ret = CryptDecodeObjectEx(dwCertEncodingType, X509_CRL_DIST_POINTS,
     pbEncoded, cbEncoded, CRYPT_DECODE_ALLOC_FLAG, NULL, &info, &size)))
    {
        static const WCHAR numFmt[] = { '%','d',0 };
        static const WCHAR colonSep[] = { ':',' ',0 };
        static const WCHAR commaSep[] = { ',',' ',0 };
        static const WCHAR colon[] = { ':',0 };
        static BOOL stringsLoaded = FALSE;
        DWORD bytesNeeded = sizeof(WCHAR); /* space for NULL terminator */
        BOOL haveAnEntry = FALSE;
        LPCWSTR headingSep, distPointSep, nameSep;
        WCHAR distPointNum[11];
        DWORD i;

        if (!stringsLoaded)
        {
            LoadStringW(hInstance, IDS_CRL_DIST_POINT, crlDistPoint,
             sizeof(crlDistPoint) / sizeof(crlDistPoint[0]));
            LoadStringW(hInstance, IDS_CRL_DIST_POINT_NAME, distPointName,
             sizeof(distPointName) / sizeof(distPointName[0]));
            LoadStringW(hInstance, IDS_CRL_DIST_POINT_FULL_NAME, fullName,
             sizeof(fullName) / sizeof(fullName[0]));
            LoadStringW(hInstance, IDS_CRL_DIST_POINT_RDN_NAME, rdnName,
             sizeof(rdnName) / sizeof(rdnName[0]));
            LoadStringW(hInstance, IDS_CRL_DIST_POINT_REASON, reason,
             sizeof(reason) / sizeof(reason[0]));
            LoadStringW(hInstance, IDS_CRL_DIST_POINT_ISSUER, issuer,
             sizeof(issuer) / sizeof(issuer[0]));
            stringsLoaded = TRUE;
        }
        if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
        {
            headingSep = crlf;
            distPointSep = crlf;
            nameSep = colonCrlf;
        }
        else
        {
            headingSep = colonSep;
            distPointSep = commaSep;
            nameSep = colon;
        }

        for (i = 0; ret && i < info->cDistPoint; i++)
        {
            CRL_DIST_POINT *distPoint = &info->rgDistPoint[i];

            if (distPoint->DistPointName.dwDistPointNameChoice !=
             CRL_DIST_POINT_NO_NAME)
            {
                bytesNeeded += strlenW(distPointName) * sizeof(WCHAR);
                bytesNeeded += strlenW(colon) * sizeof(WCHAR);
                if (distPoint->DistPointName.dwDistPointNameChoice ==
                 CRL_DIST_POINT_FULL_NAME)
                    bytesNeeded += strlenW(fullName) * sizeof(WCHAR);
                else
                    bytesNeeded += strlenW(rdnName) * sizeof(WCHAR);
                bytesNeeded += strlenW(nameSep) * sizeof(WCHAR);
                /* The indent level (3) is higher than when used as the issuer,
                 * because the name is suppordinate to the name type (full vs.
                 * RDN.)
                 */
                ret = CRYPT_FormatAltNameInfo(dwFormatStrType, 3,
                 &distPoint->DistPointName.u.FullName, NULL, &size);
                if (ret)
                    bytesNeeded += size - sizeof(WCHAR);
                haveAnEntry = TRUE;
            }
            else if (distPoint->ReasonFlags.cbData)
            {
                bytesNeeded += strlenW(reason) * sizeof(WCHAR);
                ret = CRYPT_FormatReason(dwFormatStrType,
                 &distPoint->ReasonFlags, NULL, &size);
                if (ret)
                    bytesNeeded += size - sizeof(WCHAR);
                haveAnEntry = TRUE;
            }
            else if (distPoint->CRLIssuer.cAltEntry)
            {
                bytesNeeded += strlenW(issuer) * sizeof(WCHAR);
                bytesNeeded += strlenW(nameSep) * sizeof(WCHAR);
                ret = CRYPT_FormatAltNameInfo(dwFormatStrType, 2,
                 &distPoint->CRLIssuer, NULL, &size);
                if (ret)
                    bytesNeeded += size - sizeof(WCHAR);
                haveAnEntry = TRUE;
            }
            if (haveAnEntry)
            {
                bytesNeeded += sizeof(WCHAR); /* left bracket */
                sprintfW(distPointNum, numFmt, i + 1);
                bytesNeeded += strlenW(distPointNum) * sizeof(WCHAR);
                bytesNeeded += sizeof(WCHAR); /* right bracket */
                bytesNeeded += strlenW(crlDistPoint) * sizeof(WCHAR);
                bytesNeeded += strlenW(headingSep) * sizeof(WCHAR);
                if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
                    bytesNeeded += strlenW(indent) * sizeof(WCHAR);
            }
        }
        if (!haveAnEntry)
        {
            WCHAR infoNotAvailable[MAX_STRING_RESOURCE_LEN];

            LoadStringW(hInstance, IDS_INFO_NOT_AVAILABLE, infoNotAvailable,
             sizeof(infoNotAvailable) / sizeof(infoNotAvailable[0]));
            bytesNeeded += strlenW(infoNotAvailable) * sizeof(WCHAR);
            if (!pbFormat)
                *pcbFormat = bytesNeeded;
            else if (*pcbFormat < bytesNeeded)
            {
                *pcbFormat = bytesNeeded;
                SetLastError(ERROR_MORE_DATA);
                ret = FALSE;
            }
            else
            {
                *pcbFormat = bytesNeeded;
                strcpyW((LPWSTR)pbFormat, infoNotAvailable);
            }
        }
        else
        {
            if (!pbFormat)
                *pcbFormat = bytesNeeded;
            else if (*pcbFormat < bytesNeeded)
            {
                *pcbFormat = bytesNeeded;
                SetLastError(ERROR_MORE_DATA);
                ret = FALSE;
            }
            else
            {
                LPWSTR str = pbFormat;

                *pcbFormat = bytesNeeded;
                for (i = 0; ret && i < info->cDistPoint; i++)
                {
                    CRL_DIST_POINT *distPoint = &info->rgDistPoint[i];

                    *str++ = '[';
                    sprintfW(distPointNum, numFmt, i + 1);
                    strcpyW(str, distPointNum);
                    str += strlenW(distPointNum);
                    *str++ = ']';
                    strcpyW(str, crlDistPoint);
                    str += strlenW(crlDistPoint);
                    strcpyW(str, headingSep);
                    str += strlenW(headingSep);
                    if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
                    {
                        strcpyW(str, indent);
                        str += strlenW(indent);
                    }
                    if (distPoint->DistPointName.dwDistPointNameChoice !=
                     CRL_DIST_POINT_NO_NAME)
                    {
                        DWORD altNameSize = bytesNeeded;

                        strcpyW(str, distPointName);
                        str += strlenW(distPointName);
                        strcpyW(str, colon);
                        str += strlenW(colon);
                        if (distPoint->DistPointName.dwDistPointNameChoice ==
                         CRL_DIST_POINT_FULL_NAME)
                        {
                            strcpyW(str, fullName);
                            str += strlenW(fullName);
                        }
                        else
                        {
                            strcpyW(str, rdnName);
                            str += strlenW(rdnName);
                        }
                        strcpyW(str, nameSep);
                        str += strlenW(nameSep);
                        ret = CRYPT_FormatAltNameInfo(dwFormatStrType, 3,
                         &distPoint->DistPointName.u.FullName, str,
                         &altNameSize);
                        if (ret)
                            str += altNameSize / sizeof(WCHAR) - 1;
                    }
                    else if (distPoint->ReasonFlags.cbData)
                    {
                        DWORD reasonSize = bytesNeeded;

                        strcpyW(str, reason);
                        str += strlenW(reason);
                        ret = CRYPT_FormatReason(dwFormatStrType,
                         &distPoint->ReasonFlags, str, &reasonSize);
                        if (ret)
                            str += reasonSize / sizeof(WCHAR) - 1;
                    }
                    else if (distPoint->CRLIssuer.cAltEntry)
                    {
                        DWORD crlIssuerSize = bytesNeeded;

                        strcpyW(str, issuer);
                        str += strlenW(issuer);
                        strcpyW(str, nameSep);
                        str += strlenW(nameSep);
                        ret = CRYPT_FormatAltNameInfo(dwFormatStrType, 2,
                         &distPoint->CRLIssuer, str,
                         &crlIssuerSize);
                        if (ret)
                            str += crlIssuerSize / sizeof(WCHAR) - 1;
                    }
                }
            }
        }
        LocalFree(info);
    }
    return ret;
}

static BOOL WINAPI CRYPT_FormatEnhancedKeyUsage(DWORD dwCertEncodingType,
 DWORD dwFormatType, DWORD dwFormatStrType, void *pFormatStruct,
 LPCSTR lpszStructType, const BYTE *pbEncoded, DWORD cbEncoded, void *pbFormat,
 DWORD *pcbFormat)
{
    CERT_ENHKEY_USAGE *usage;
    DWORD size;
    BOOL ret = FALSE;

    if (!cbEncoded)
    {
        SetLastError(E_INVALIDARG);
        return FALSE;
    }
    if ((ret = CryptDecodeObjectEx(dwCertEncodingType, X509_ENHANCED_KEY_USAGE,
     pbEncoded, cbEncoded, CRYPT_DECODE_ALLOC_FLAG, NULL, &usage, &size)))
    {
        WCHAR unknown[MAX_STRING_RESOURCE_LEN];
        DWORD i;
        DWORD bytesNeeded = sizeof(WCHAR); /* space for the NULL terminator */
        LPCWSTR sep;
        DWORD sepLen;

        if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
        {
            sep = crlf;
            sepLen = strlenW(crlf) * sizeof(WCHAR);
        }
        else
        {
            sep = commaSpace;
            sepLen = strlenW(commaSpace) * sizeof(WCHAR);
        }

        LoadStringW(hInstance, IDS_USAGE_UNKNOWN, unknown,
         sizeof(unknown) / sizeof(unknown[0]));
        for (i = 0; i < usage->cUsageIdentifier; i++)
        {
            PCCRYPT_OID_INFO info = CryptFindOIDInfo(CRYPT_OID_INFO_OID_KEY,
             usage->rgpszUsageIdentifier[i], CRYPT_ENHKEY_USAGE_OID_GROUP_ID);

            if (info)
                bytesNeeded += strlenW(info->pwszName) * sizeof(WCHAR);
            else
                bytesNeeded += strlenW(unknown) * sizeof(WCHAR);
            bytesNeeded += sizeof(WCHAR); /* space */
            bytesNeeded += sizeof(WCHAR); /* left paren */
            bytesNeeded += strlen(usage->rgpszUsageIdentifier[i]) *
             sizeof(WCHAR);
            bytesNeeded += sizeof(WCHAR); /* right paren */
            if (i < usage->cUsageIdentifier - 1)
                bytesNeeded += sepLen;
        }
        if (!pbFormat)
            *pcbFormat = bytesNeeded;
        else if (*pcbFormat < bytesNeeded)
        {
            *pcbFormat = bytesNeeded;
            SetLastError(ERROR_MORE_DATA);
            ret = FALSE;
        }
        else
        {
            LPWSTR str = pbFormat;

            *pcbFormat = bytesNeeded;
            for (i = 0; i < usage->cUsageIdentifier; i++)
            {
                PCCRYPT_OID_INFO info = CryptFindOIDInfo(CRYPT_OID_INFO_OID_KEY,
                 usage->rgpszUsageIdentifier[i],
                 CRYPT_ENHKEY_USAGE_OID_GROUP_ID);
                LPCSTR oidPtr;

                if (info)
                {
                    strcpyW(str, info->pwszName);
                    str += strlenW(info->pwszName);
                }
                else
                {
                    strcpyW(str, unknown);
                    str += strlenW(unknown);
                }
                *str++ = ' ';
                *str++ = '(';
                for (oidPtr = usage->rgpszUsageIdentifier[i]; *oidPtr; oidPtr++)
                    *str++ = *oidPtr;
                *str++ = ')';
                *str = 0;
                if (i < usage->cUsageIdentifier - 1)
                {
                    strcpyW(str, sep);
                    str += sepLen / sizeof(WCHAR);
                }
            }
        }
        LocalFree(usage);
    }
    return ret;
}

static WCHAR financialCriteria[MAX_STRING_RESOURCE_LEN];
static WCHAR available[MAX_STRING_RESOURCE_LEN];
static WCHAR notAvailable[MAX_STRING_RESOURCE_LEN];
static WCHAR meetsCriteria[MAX_STRING_RESOURCE_LEN];
static WCHAR yes[MAX_STRING_RESOURCE_LEN];
static WCHAR no[MAX_STRING_RESOURCE_LEN];

static BOOL WINAPI CRYPT_FormatSpcFinancialCriteria(DWORD dwCertEncodingType,
 DWORD dwFormatType, DWORD dwFormatStrType, void *pFormatStruct,
 LPCSTR lpszStructType, const BYTE *pbEncoded, DWORD cbEncoded, void *pbFormat,
 DWORD *pcbFormat)
{
    SPC_FINANCIAL_CRITERIA criteria;
    DWORD size = sizeof(criteria);
    BOOL ret = FALSE;

    if (!cbEncoded)
    {
        SetLastError(E_INVALIDARG);
        return FALSE;
    }
    if ((ret = CryptDecodeObjectEx(dwCertEncodingType,
     SPC_FINANCIAL_CRITERIA_STRUCT, pbEncoded, cbEncoded, 0, NULL, &criteria,
     &size)))
    {
        static BOOL stringsLoaded = FALSE;
        DWORD bytesNeeded = sizeof(WCHAR);
        LPCWSTR sep;
        DWORD sepLen;

        if (!stringsLoaded)
        {
            LoadStringW(hInstance, IDS_FINANCIAL_CRITERIA, financialCriteria,
             sizeof(financialCriteria) / sizeof(financialCriteria[0]));
            LoadStringW(hInstance, IDS_FINANCIAL_CRITERIA_AVAILABLE, available,
             sizeof(available) / sizeof(available[0]));
            LoadStringW(hInstance, IDS_FINANCIAL_CRITERIA_NOT_AVAILABLE,
             notAvailable, sizeof(notAvailable) / sizeof(notAvailable[0]));
            LoadStringW(hInstance, IDS_FINANCIAL_CRITERIA_MEETS_CRITERIA,
             meetsCriteria, sizeof(meetsCriteria) / sizeof(meetsCriteria[0]));
            LoadStringW(hInstance, IDS_YES, yes, sizeof(yes) / sizeof(yes[0]));
            LoadStringW(hInstance, IDS_NO, no, sizeof(no) / sizeof(no[0]));
            stringsLoaded = TRUE;
        }
        if (dwFormatStrType & CRYPT_FORMAT_STR_MULTI_LINE)
        {
            sep = crlf;
            sepLen = strlenW(crlf) * sizeof(WCHAR);
        }
        else
        {
            sep = commaSpace;
            sepLen = strlenW(commaSpace) * sizeof(WCHAR);
        }
        bytesNeeded += strlenW(financialCriteria) * sizeof(WCHAR);
        if (criteria.fFinancialInfoAvailable)
        {
            bytesNeeded += strlenW(available) * sizeof(WCHAR);
            bytesNeeded += sepLen;
            bytesNeeded += strlenW(meetsCriteria) * sizeof(WCHAR);
            if (criteria.fMeetsCriteria)
                bytesNeeded += strlenW(yes) * sizeof(WCHAR);
            else
                bytesNeeded += strlenW(no) * sizeof(WCHAR);
        }
        else
            bytesNeeded += strlenW(notAvailable) * sizeof(WCHAR);
        if (!pbFormat)
            *pcbFormat = bytesNeeded;
        else if (*pcbFormat < bytesNeeded)
        {
            *pcbFormat = bytesNeeded;
            SetLastError(ERROR_MORE_DATA);
            ret = FALSE;
        }
        else
        {
            LPWSTR str = pbFormat;

            *pcbFormat = bytesNeeded;
            strcpyW(str, financialCriteria);
            str += strlenW(financialCriteria);
            if (criteria.fFinancialInfoAvailable)
            {
                strcpyW(str, available);
                str += strlenW(available);
                strcpyW(str, sep);
                str += sepLen / sizeof(WCHAR);
                strcpyW(str, meetsCriteria);
                str += strlenW(meetsCriteria);
                if (criteria.fMeetsCriteria)
                    strcpyW(str, yes);
                else
                    strcpyW(str, no);
            }
            else
            {
                strcpyW(str, notAvailable);
                str += strlenW(notAvailable);
            }
        }
    }
    return ret;
}

typedef BOOL (WINAPI *CryptFormatObjectFunc)(DWORD, DWORD, DWORD, void *,
 LPCSTR, const BYTE *, DWORD, void *, DWORD *);

static CryptFormatObjectFunc CRYPT_GetBuiltinFormatFunction(DWORD encodingType,
 DWORD formatStrType, LPCSTR lpszStructType)
{
    CryptFormatObjectFunc format = NULL;

    if ((encodingType & CERT_ENCODING_TYPE_MASK) != X509_ASN_ENCODING)
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return NULL;
    }
    if (!HIWORD(lpszStructType))
    {
        switch (LOWORD(lpszStructType))
        {
        case LOWORD(X509_BASIC_CONSTRAINTS2):
            format = CRYPT_FormatBasicConstraints2;
            break;
        case LOWORD(X509_AUTHORITY_KEY_ID2):
            format = CRYPT_FormatAuthorityKeyId2;
            break;
        case LOWORD(X509_AUTHORITY_INFO_ACCESS):
            format = CRYPT_FormatAuthorityInfoAccess;
            break;
        case LOWORD(X509_CRL_DIST_POINTS):
            format = CRYPT_FormatCRLDistPoints;
            break;
        case LOWORD(X509_ENHANCED_KEY_USAGE):
            format = CRYPT_FormatEnhancedKeyUsage;
            break;
        case LOWORD(SPC_FINANCIAL_CRITERIA_STRUCT):
            format = CRYPT_FormatSpcFinancialCriteria;
            break;
        }
    }
    else if (!strcmp(lpszStructType, szOID_BASIC_CONSTRAINTS2))
        format = CRYPT_FormatBasicConstraints2;
    else if (!strcmp(lpszStructType, szOID_AUTHORITY_INFO_ACCESS))
        format = CRYPT_FormatAuthorityInfoAccess;
    else if (!strcmp(lpszStructType, szOID_AUTHORITY_KEY_IDENTIFIER2))
        format = CRYPT_FormatAuthorityKeyId2;
    else if (!strcmp(lpszStructType, szOID_CRL_DIST_POINTS))
        format = CRYPT_FormatCRLDistPoints;
    else if (!strcmp(lpszStructType, szOID_ENHANCED_KEY_USAGE))
        format = CRYPT_FormatEnhancedKeyUsage;
    else if (!strcmp(lpszStructType, SPC_FINANCIAL_CRITERIA_OBJID))
        format = CRYPT_FormatSpcFinancialCriteria;
    if (!format && !(formatStrType & CRYPT_FORMAT_STR_NO_HEX))
        format = CRYPT_FormatHexString;
    return format;
}

BOOL WINAPI CryptFormatObject(DWORD dwCertEncodingType, DWORD dwFormatType,
 DWORD dwFormatStrType, void *pFormatStruct, LPCSTR lpszStructType,
 const BYTE *pbEncoded, DWORD cbEncoded, void *pbFormat, DWORD *pcbFormat)
{
    CryptFormatObjectFunc format = NULL;
    HCRYPTOIDFUNCADDR hFunc = NULL;
    BOOL ret = FALSE;

    TRACE("(%08x, %d, %08x, %p, %s, %p, %d, %p, %p)\n", dwCertEncodingType,
     dwFormatType, dwFormatStrType, pFormatStruct, debugstr_a(lpszStructType),
     pbEncoded, cbEncoded, pbFormat, pcbFormat);

    if (!(format = CRYPT_GetBuiltinFormatFunction(dwCertEncodingType,
     dwFormatStrType, lpszStructType)))
    {
        static HCRYPTOIDFUNCSET set = NULL;

        if (!set)
            set = CryptInitOIDFunctionSet(CRYPT_OID_FORMAT_OBJECT_FUNC, 0);
        CryptGetOIDFunctionAddress(set, dwCertEncodingType, lpszStructType, 0,
         (void **)&format, &hFunc);
    }
    if (format)
        ret = format(dwCertEncodingType, dwFormatType, dwFormatStrType,
         pFormatStruct, lpszStructType, pbEncoded, cbEncoded, pbFormat,
         pcbFormat);
    if (hFunc)
        CryptFreeOIDFunctionAddress(hFunc, 0);
    return ret;
}
