/* //device/libs/telephony/ril.cpp
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "RIL_CPP"
#define NDEBUG 1

#include <telephony/record_stream.h>
#include <telephony/ril.h>

#include <assert.h>
#include <binder/Parcel.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <jstring.h>
#include <limits.h>
#include <log/log_radio.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <local_socket.h>
#include <ril_event.h>
#define INVALID_HEX_CHAR 16
using namespace android;

extern "C" void RIL_onRequestComplete(RIL_Token t, RIL_Errno e, void* response, size_t responselen);

#define PHONE_PROCESS "radio"

#define SOCKET_NAME_RIL "rild"

#define SOCKET_NAME_RIL_DEBUG "rild-debug"

// match with constant in RIL.java
#define MAX_COMMAND_BYTES (8 * 1024)

// Basically: memset buffers that the client library
// shouldn't be using anymore in an attempt to find
// memory usage issues sooner.
#define MEMSET_FREED 1

#define NUM_ELEMS(a) (sizeof(a) / sizeof(a)[0])

#define MIN(a, b) ((a) < (b) ? (a) : (b))

/* Constants for response types */
#define RESPONSE_SOLICITED 0
#define RESPONSE_UNSOLICITED 1

/* Negative values for private RIL errno's */
#define RIL_ERRNO_INVALID_RESPONSE -1

// request, response, and unsolicited msg print macro
#define PRINTBUF_SIZE 8096

// Enable verbose logging
#define VDBG 0

// Enable RILC log
#define RILC_LOG 0

#if RILC_LOG
#define startRequest sprintf(printBuf, "(")
#define closeRequest sprintf(printBuf, "%s)", printBuf)
#define printRequest(token, req) \
    RLOGD("[%04d]> %s %s", token, requestToString(req), printBuf)

#define startResponse sprintf(printBuf, "%s {", printBuf)
#define closeResponse sprintf(printBuf, "%s}", printBuf)
#define printResponse RLOGD("%s", printBuf)

#define clearPrintBuf printBuf[0] = 0
#define removeLastChar printBuf[strlen(printBuf) - 1] = 0
#define appendPrintBuf(x...) snprintf(printBuf, PRINTBUF_SIZE, x)
#else
#define startRequest
#define closeRequest
#define printRequest(token, req)
#define startResponse
#define closeResponse
#define printResponse
#define clearPrintBuf
#define removeLastChar
#define appendPrintBuf(x...)
#endif

enum WakeType {
    DONT_WAKE,
    WAKE_PARTIAL
};

typedef struct {
    int requestNumber;
    void (*dispatchFunction)(Parcel& p, struct RequestInfo* pRI);
    int (*responseFunction)(Parcel& p, void* response, size_t responselen);
} CommandInfo;

typedef struct {
    int requestNumber;
    int (*responseFunction)(Parcel& p, void* response, size_t responselen);
    WakeType wakeType;
} UnsolResponseInfo;

typedef struct RequestInfo {
    int32_t token; // this is not RIL_Token
    CommandInfo* pCI;
    struct RequestInfo* p_next;
    char cancelled;
    char local; // responses to local commands do not go back to command process
} RequestInfo;

typedef struct UserCallbackInfo {
    RIL_TimedCallback p_callback;
    void* userParam;
    struct ril_event event;
    struct UserCallbackInfo* p_next;
} UserCallbackInfo;

extern "C" const char* requestToString(int request);
extern "C" const char* failCauseToString(RIL_Errno);
extern "C" const char* callStateToString(RIL_CallState);
extern "C" const char* radioStateToString(RIL_RadioState);
extern "C" uint8_t hexCharToInt(uint8_t c)
{
    if (c >= '0' && c <= '9')
        return (c - '0');
    if (c >= 'A' && c <= 'F')
        return (c - 'A' + 10);
    if (c >= 'a' && c <= 'f')
        return (c - 'a' + 10);

    return INVALID_HEX_CHAR;
}

extern "C" uint8_t* convertHexStringToBytes(void* response, size_t responseLen)
{
    if (responseLen % 2 != 0) {
        return NULL;
    }

    uint8_t* bytes = (uint8_t*)calloc(responseLen / 2, sizeof(uint8_t));
    if (bytes == NULL) {
        return NULL;
    }
    uint8_t* hexString = (uint8_t*)response;

    for (size_t i = 0; i < responseLen; i += 2) {
        uint8_t hexChar1 = hexCharToInt(hexString[i]);
        uint8_t hexChar2 = hexCharToInt(hexString[i + 1]);

        if (hexChar1 == INVALID_HEX_CHAR || hexChar2 == INVALID_HEX_CHAR) {
            free(bytes);
            return NULL;
        }
        bytes[i / 2] = ((hexChar1 << 4) | hexChar2);
    }

    return bytes;
}

/*******************************************************************/

RIL_RadioFunctions s_callbacks = { 0, NULL, NULL, NULL, NULL, NULL };
static int s_registerCalled = 0;

static pthread_t s_tid_dispatch;
static int s_started = 0;

static int s_fdListen = -1;
static int s_fdCommand = -1;

static int s_fdWakeupRead;
static int s_fdWakeupWrite;

static struct ril_event s_commands_event;
static struct ril_event s_wakeupfd_event;
static struct ril_event s_listen_event;

static pthread_mutex_t s_pendingRequestsMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_writeMutex = PTHREAD_MUTEX_INITIALIZER;
static RequestInfo* s_pendingRequests = NULL;

static const struct timeval TIMEVAL_WAKE_TIMEOUT = { 1, 0 };

static UserCallbackInfo* s_last_wake_timeout_info = NULL;

static void* s_lastNITZTimeData = NULL;
static size_t s_lastNITZTimeDataSize;

#if RILC_LOG
static char printBuf[PRINTBUF_SIZE];
#endif

/*******************************************************************/
static int sendResponse(Parcel& p);

static void dispatchVoid(Parcel& p, RequestInfo* pRI);
static void dispatchString(Parcel& p, RequestInfo* pRI);
static void dispatchStrings(Parcel& p, RequestInfo* pRI);
static void dispatchInts(Parcel& p, RequestInfo* pRI);
static void dispatchDial(Parcel& p, RequestInfo* pRI);
static void dispatchSIM_IO(Parcel& p, RequestInfo* pRI);
static void dispatchSIM_APDU(Parcel& p, RequestInfo* pRI);
static void dispatchCallForward(Parcel& p, RequestInfo* pRI);
static void dispatchRaw(Parcel& p, RequestInfo* pRI);
static void dispatchSmsWrite(Parcel& p, RequestInfo* pRI);
static void dispatchDataCall(Parcel& p, RequestInfo* pRI);
static void dispatchVoiceRadioTech(Parcel& p, RequestInfo* pRI);
static void dispatchSetInitialAttachApn(Parcel& p, RequestInfo* pRI);
static void dispatchImsSms(Parcel& p, RequestInfo* pRI);
static void dispatchCallForward(Parcel& p, RequestInfo* pRI);
static void dispatchImsGsmSms(Parcel& p, RequestInfo* pRI, uint8_t retry, int32_t messageRef);
static void dispatchGsmBrSmsCnf(Parcel& p, RequestInfo* pRI);
static void dispatchDataProfile(Parcel& p, RequestInfo* pRI);
static void dispatchManualSelection(Parcel& p, RequestInfo* pRI);
static void dispatchConferenceInvite(Parcel& p, RequestInfo* pRI);
static int responseInts(Parcel& p, void* response, size_t responselen);
static int responseStrings(Parcel& p, void* response, size_t responselen);
static int responseString(Parcel& p, void* response, size_t responselen);
static int responseVoid(Parcel& p, void* response, size_t responselen);
static int responseCallList(Parcel& p, void* response, size_t responselen);
static int responseSMS(Parcel& p, void* response, size_t responselen);
static int responseSIM_IO(Parcel& p, void* response, size_t responselen);
static int responseCallForwards(Parcel& p, void* response, size_t responselen);
static int responseDataCallList(Parcel& p, void* response, size_t responselen);
static int responseSetupDataCall(Parcel& p, void* response, size_t responselen);
static int responseRaw(Parcel& p, void* response, size_t responselen);
static int responseSsn(Parcel& p, void* response, size_t responselen);
static int responseSimStatus(Parcel& p, void* response, size_t responselen);
static int responseImsStatus(Parcel& p, void* response, size_t responselen);
static int responseGsmBrSmsCnf(Parcel& p, void* response, size_t responselen);
static int responseCellList(Parcel& p, void* response, size_t responselen);
static int responseRilSignalStrength(Parcel& p, void* response, size_t responselen);
static int responseSimRefresh(Parcel& p, void* response, size_t responselen);
static int responseCellInfoList(Parcel& p, void* response, size_t responselen);
static int responseStringsWithVersion(int version, Parcel& p, void* response, size_t responselen);
static int responseActivityData(Parcel& p, void* response, size_t responselen);
static int decodeVoiceRadioTechnology(RIL_RadioState radioState);
static RIL_RadioState processRadioState(RIL_RadioState newRadioState);

extern "C" void RIL_onUnsolicitedResponse(int unsolResponse, const void* data,
    size_t datalen);

static UserCallbackInfo* internalRequestTimedCallback(RIL_TimedCallback callback, void* param,
    const struct timeval* relativeTime);

static void wakeTimeoutCallback(void* param);

/* Index == requestNumber */
static CommandInfo s_commands[] = {
#include "ril_commands.h"
};

/* Index == requestNumber */
static CommandInfo s_second_commands[] = {
#include "ril_second_commands.h"
};

/* Index == requestNumber */
static CommandInfo s_ims_commands[] = {
#include "ril_ims_commands.h"
};

static UnsolResponseInfo s_unsolResponses[] = {
#include "ril_unsol_commands.h"
};

/* Index == requestNumber */
static CommandInfo s_cus_commands[] = {
#include "ril_cus_commands.h"
};

/* For older RILs that do not support new commands RIL_REQUEST_VOICE_RADIO_TECH and
 * RIL_UNSOL_VOICE_RADIO_TECH_CHANGED messages, decode the voice radio tech from
 * radio state message and store it. Every time there is a change in Radio State
 * check to see if voice radio tech changes and notify telephony */
int voiceRadioTech = -1;

/* For older RILs that do not send RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, decode the
 * SIM/RUIM state from radio state and store it. Every time there is a change in Radio State,
 * check to see if SIM/RUIM status changed and notify telephony */
int simRuimStatus = -1;

static char* strdupReadString(Parcel& p)
{
    size_t stringlen;
    const char16_t* s16;

    s16 = p.readString16Inplace(&stringlen);

    return strndup16to8(s16, stringlen);
}

static void writeStringToParcel(Parcel& p, const char* s)
{
    char16_t* s16;
    size_t s16_len;
    s16 = strdup8to16(s, &s16_len);
    p.writeString16(s16, s16_len);
    free(s16);
}

static void memsetString(char* s)
{
    if (s != NULL) {
        memset(s, 0, strlen(s));
    }
}

static int processCommandBuffer(void* buffer, size_t buflen)
{
    Parcel p;
    status_t status;
    int32_t request;
    int32_t token;
    RequestInfo* pRI;
    int ret = 0;

    (void)ret;

    p.setData((uint8_t*)buffer, buflen);

    // status checked at end
    status = p.readInt32(&request);
    status = p.readInt32(&token);

    if (status != NO_ERROR) {
        RLOGE("invalid request block");
        return 0;
    }

    if (request < 1
        || (request >= (int32_t)NUM_ELEMS(s_commands)
            && request <= RIL_SECOND_REQUEST_BASE)
        || (request >= RIL_SECOND_REQUEST_BASE + (int32_t)NUM_ELEMS(s_second_commands)
            && request <= RIL_IMS_REQUEST_BASE)
        || (request >= RIL_IMS_REQUEST_BASE + (int32_t)NUM_ELEMS(s_ims_commands)
            && request <= RIL_CUS_REQUEST_BASE)
        || request >= RIL_CUS_REQUEST_BASE + (int32_t)NUM_ELEMS(s_cus_commands)) {
        Parcel pErr;
        RLOGE("unsupported request code %ld token %ld", request, token);
        // FIXME this should perhaps return a response
        status = pErr.writeInt32(RESPONSE_SOLICITED);
        status = pErr.writeInt32(token);
        status = pErr.writeInt32(RIL_E_GENERIC_FAILURE);

        if (status != NO_ERROR) {
            RLOGE("failed to construct error response parcel");
            return 0;
        }

        if (sendResponse(pErr) < 0) {
            RLOGE("failed to send error response parcel");
        }

        return 0;
    }

    pRI = (RequestInfo*)calloc(1, sizeof(RequestInfo));
    if (pRI == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(request));
        return 0;
    }

    pRI->token = token;
    if (request > 0 && request < (int32_t)NUM_ELEMS(s_commands)) {
        pRI->pCI = &(s_commands[request]);
    } else if (request > RIL_SECOND_REQUEST_BASE
        && request < RIL_SECOND_REQUEST_BASE + (int32_t)NUM_ELEMS(s_second_commands)) {
        request = request - RIL_SECOND_REQUEST_BASE;
        pRI->pCI = &(s_second_commands[request]);
    } else if (request > RIL_IMS_REQUEST_BASE
        && request < RIL_IMS_REQUEST_BASE + (int32_t)NUM_ELEMS(s_ims_commands)) {
        request = request - RIL_IMS_REQUEST_BASE;
        pRI->pCI = &(s_ims_commands[request]);
    } else if (request > RIL_CUS_REQUEST_BASE
        && request < RIL_CUS_REQUEST_BASE + (int32_t)NUM_ELEMS(s_cus_commands)) {
        request = request - RIL_CUS_REQUEST_BASE;
        pRI->pCI = &(s_cus_commands[request]);
    }

    ret = pthread_mutex_lock(&s_pendingRequestsMutex);
    assert(ret == 0);

    pRI->p_next = s_pendingRequests;
    s_pendingRequests = pRI;

    ret = pthread_mutex_unlock(&s_pendingRequestsMutex);
    assert(ret == 0);

    /* sLastDispatchedToken = token; */
    if (NULL == pRI->pCI->dispatchFunction) {
        RIL_onRequestComplete(pRI, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        return 0;
    }
    pRI->pCI->dispatchFunction(p, pRI);

    return 0;
}

static void invalidCommandBlock(RequestInfo* pRI)
{
    RLOGE("invalid command block for token %ld request %s",
        pRI->token, requestToString(pRI->pCI->requestNumber));
}

/* Callee expects NULL */
static void dispatchVoid(Parcel& p, RequestInfo* pRI)
{
    clearPrintBuf;
    printRequest(pRI->token, pRI->pCI->requestNumber);
    s_callbacks.onRequest(pRI->pCI->requestNumber, NULL, 0, pRI);
}

/* Callee expects const char * */
static void dispatchString(Parcel& p, RequestInfo* pRI)
{
    char* string8 = NULL;

    string8 = strdupReadString(p);
    if (!string8) {
        invalidCommandBlock(pRI);
        return;
    }

    startRequest;
    appendPrintBuf("%s%s", printBuf, string8);
    closeRequest;
    printRequest(pRI->token, pRI->pCI->requestNumber);

    s_callbacks.onRequest(pRI->pCI->requestNumber, string8,
        sizeof(char*), pRI);

#ifdef MEMSET_FREED
    memsetString(string8);
#endif

    free(string8);
    return;
}

/* Callee expects const char ** */
static void dispatchStrings(Parcel& p, RequestInfo* pRI)
{
    int32_t countStrings;
    status_t status;
    size_t datalen;
    char** pStrings;

    status = p.readInt32(&countStrings);

    if (status != NO_ERROR) {
        goto invalid;
    }

    startRequest;
    if (countStrings == 0) {
        // just some non-null pointer
        pStrings = (char**)calloc(1, sizeof(char*));
        if (pStrings == NULL) {
            RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
            closeRequest;
            return;
        }

        datalen = 0;
    } else if (countStrings < 0) {
        pStrings = NULL;
        datalen = 0;
    } else {
        datalen = sizeof(char*) * countStrings;

        pStrings = (char**)calloc(countStrings, sizeof(char*));
        if (pStrings == NULL) {
            RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
            closeRequest;
            return;
        }

        for (int i = 0; i < countStrings; i++) {
            pStrings[i] = strdupReadString(p);
            appendPrintBuf("%s%s,", printBuf, pStrings[i]);
        }
    }
    removeLastChar;
    closeRequest;
    printRequest(pRI->token, pRI->pCI->requestNumber);

    s_callbacks.onRequest(pRI->pCI->requestNumber, pStrings, datalen, pRI);

    if (pStrings != NULL) {
        for (int i = 0; i < countStrings; i++) {
#ifdef MEMSET_FREED
            memsetString(pStrings[i]);
#endif
            free(pStrings[i]);
        }

#ifdef MEMSET_FREED
        memset(pStrings, 0, datalen);
#endif
        free(pStrings);
    }

    return;
invalid:
    invalidCommandBlock(pRI);
    return;
}

/* Callee expects const int * */
static void dispatchInts(Parcel& p, RequestInfo* pRI)
{
    int32_t count;
    status_t status;
    size_t datalen;
    int* pInts;

    status = p.readInt32(&count);

    if (status != NO_ERROR || count <= 0) {
        goto invalid;
    }

    datalen = sizeof(int) * count;
    pInts = (int*)calloc(count, sizeof(int));
    if (pInts == NULL) {
        RLOGE("Memory allocation failed for request %s", requestToString(pRI->pCI->requestNumber));
        return;
    }

    startRequest;
    for (int i = 0; i < count; i++) {
        int32_t t;

        status = p.readInt32(&t);
        pInts[i] = (int)t;
        appendPrintBuf("%s%d,", printBuf, t);

        if (status != NO_ERROR) {
            free(pInts);
            goto invalid;
        }
    }
    removeLastChar;
    closeRequest;
    printRequest(pRI->token, pRI->pCI->requestNumber);

    s_callbacks.onRequest(pRI->pCI->requestNumber, const_cast<int*>(pInts),
        datalen, pRI);

#ifdef MEMSET_FREED
    memset(pInts, 0, datalen);
#endif
    free(pInts);
    return;
invalid:
    invalidCommandBlock(pRI);
    return;
}

/**
 * Callee expects const RIL_SMS_WriteArgs *
 * Payload is:
 *   int32_t status
 *   String pdu
 */
static void dispatchSmsWrite(Parcel& p, RequestInfo* pRI)
{
    RIL_SMS_WriteArgs args;
    int32_t t;
    status_t status;

    RLOGD("dispatchSmsWrite");
    memset(&args, 0, sizeof(args));

    status = p.readInt32(&t);
    args.status = (int)t;

    args.pdu = strdupReadString(p);

    if (status != NO_ERROR || args.pdu == NULL) {
        goto invalid;
    }

    args.smsc = strdupReadString(p);

    startRequest;
    appendPrintBuf("%s%d,%s,smsc=%s", printBuf, args.status,
        (char*)args.pdu, (char*)args.smsc);
    closeRequest;
    printRequest(pRI->token, pRI->pCI->requestNumber);

    s_callbacks.onRequest(pRI->pCI->requestNumber, &args, sizeof(args), pRI);

#ifdef MEMSET_FREED
    memsetString(args.pdu);
#endif

    free(args.pdu);

#ifdef MEMSET_FREED
    memset(&args, 0, sizeof(args));
#endif

    return;
invalid:
    invalidCommandBlock(pRI);
    return;
}

/**
 * Callee expects const RIL_Dial *
 * Payload is:
 *   String address
 *   int32_t clir
 */
static void dispatchDial(Parcel& p, RequestInfo* pRI)
{
    RIL_Dial dial;
    RIL_UUS_Info uusInfo;
    int32_t sizeOfDial;
    int32_t t;
    int32_t uusPresent;
    status_t status;

    RLOGD("dispatchDial");
    memset(&dial, 0, sizeof(dial));

    dial.address = strdupReadString(p);

    status = p.readInt32(&t);
    dial.clir = (int)t;

    if (status != NO_ERROR || dial.address == NULL) {
        goto invalid;
    }

    if (s_callbacks.version < 3) { // Remove when partners upgrade to version 3
        uusPresent = 0;
        sizeOfDial = sizeof(dial) - sizeof(RIL_UUS_Info*);
    } else {
        status = p.readInt32(&uusPresent);

        if (status != NO_ERROR) {
            goto invalid;
        }

        if (uusPresent == 0) {
            dial.uusInfo = NULL;
        } else {
            int32_t len;

            memset(&uusInfo, 0, sizeof(RIL_UUS_Info));

            status = p.readInt32(&t);
            uusInfo.uusType = (RIL_UUS_Type)t;

            status = p.readInt32(&t);
            uusInfo.uusDcs = (RIL_UUS_DCS)t;

            status = p.readInt32(&len);
            if (status != NO_ERROR) {
                goto invalid;
            }

            // The java code writes -1 for null arrays
            if (((int)len) == -1) {
                uusInfo.uusData = NULL;
                len = 0;
            } else {
                uusInfo.uusData = (char*)p.readInplace(len);
            }

            uusInfo.uusLength = len;
            dial.uusInfo = &uusInfo;
        }
        sizeOfDial = sizeof(dial);
    }

    startRequest;
    appendPrintBuf("%snum=%s,clir=%d", printBuf, dial.address, dial.clir);
    if (uusPresent) {
        appendPrintBuf("%s,uusType=%d,uusDcs=%d,uusLen=%d", printBuf,
            dial.uusInfo->uusType, dial.uusInfo->uusDcs,
            dial.uusInfo->uusLength);
    }
    closeRequest;
    printRequest(pRI->token, pRI->pCI->requestNumber);

    s_callbacks.onRequest(pRI->pCI->requestNumber, &dial, sizeOfDial, pRI);

#ifdef MEMSET_FREED
    memsetString(dial.address);
#endif

    free(dial.address);

#ifdef MEMSET_FREED
    memset(&uusInfo, 0, sizeof(RIL_UUS_Info));
    memset(&dial, 0, sizeof(dial));
#endif

    return;
invalid:
    invalidCommandBlock(pRI);
    return;
}

/**
 * Callee expects const RIL_SIM_IO *
 * Payload is:
 *   int32_t command
 *   int32_t fileid
 *   String path
 *   int32_t p1, p2, p3
 *   String data
 *   String pin2
 *   String aidPtr
 */
static void dispatchSIM_IO(Parcel& p, RequestInfo* pRI)
{
    union RIL_SIM_IO {
        RIL_SIM_IO_v6 v6;
        RIL_SIM_IO_v5 v5;
    } simIO;

    int32_t t;
    int size;
    status_t status;

    RLOGD("dispatchSIM_IO");
    memset(&simIO, 0, sizeof(simIO));

    // note we only check status at the end

    status = p.readInt32(&t);
    simIO.v6.command = (int)t;

    status = p.readInt32(&t);
    simIO.v6.fileid = (int)t;

    simIO.v6.path = strdupReadString(p);

    status = p.readInt32(&t);
    simIO.v6.p1 = (int)t;

    status = p.readInt32(&t);
    simIO.v6.p2 = (int)t;

    status = p.readInt32(&t);
    simIO.v6.p3 = (int)t;

    simIO.v6.data = strdupReadString(p);
    simIO.v6.pin2 = strdupReadString(p);
    simIO.v6.aidPtr = strdupReadString(p);

    startRequest;
    appendPrintBuf("%scmd=0x%X,efid=0x%X,path=%s,%d,%d,%d,%s,pin2=%s,aid=%s", printBuf,
        simIO.v6.command, simIO.v6.fileid, (char*)simIO.v6.path,
        simIO.v6.p1, simIO.v6.p2, simIO.v6.p3,
        (char*)simIO.v6.data, (char*)simIO.v6.pin2, simIO.v6.aidPtr);
    closeRequest;
    printRequest(pRI->token, pRI->pCI->requestNumber);

    if (status != NO_ERROR) {
        goto invalid;
    }

    size = (s_callbacks.version < 6) ? sizeof(simIO.v5) : sizeof(simIO.v6);
    s_callbacks.onRequest(pRI->pCI->requestNumber, &simIO, size, pRI);

#ifdef MEMSET_FREED
    memsetString(simIO.v6.path);
    memsetString(simIO.v6.data);
    memsetString(simIO.v6.pin2);
    memsetString(simIO.v6.aidPtr);
#endif

    free(simIO.v6.path);
    free(simIO.v6.data);
    free(simIO.v6.pin2);
    free(simIO.v6.aidPtr);

#ifdef MEMSET_FREED
    memset(&simIO, 0, sizeof(simIO));
#endif

    return;
invalid:
    invalidCommandBlock(pRI);
    return;
}

/**
 * Callee expects const RIL_SIM_APDU *
 * Payload is:
 *   int32_t sessionid
 *   int32_t cla
 *   int32_t instruction
 *   int32_t p1, p2, p3
 *   String data
 */
static void dispatchSIM_APDU(Parcel& p, RequestInfo* pRI)
{
    int32_t t;
    status_t status;
    RIL_SIM_APDU apdu;

    RLOGD("dispatchSIM_APDU");
    memset(&apdu, 0, sizeof(RIL_SIM_APDU));

    // Note we only check status at the end. Any single failure leads to
    // subsequent reads filing.
    status = p.readInt32(&t);
    apdu.sessionid = (int)t;

    status = p.readInt32(&t);
    apdu.cla = (int)t;

    status = p.readInt32(&t);
    apdu.instruction = (int)t;

    status = p.readInt32(&t);
    apdu.p1 = (int)t;

    status = p.readInt32(&t);
    apdu.p2 = (int)t;

    status = p.readInt32(&t);
    apdu.p3 = (int)t;

    apdu.data = strdupReadString(p);

    startRequest;
    appendPrintBuf("%ssessionid=%d,cla=%d,ins=%d,p1=%d,p2=%d,p3=%d,data=%s",
        printBuf, apdu.sessionid, apdu.cla, apdu.instruction, apdu.p1, apdu.p2,
        apdu.p3, (char*)apdu.data);
    closeRequest;
    printRequest(pRI->token, pRI->pCI->requestNumber);

    if (status != NO_ERROR) {
        goto invalid;
    }

    s_callbacks.onRequest(pRI->pCI->requestNumber, &apdu, sizeof(RIL_SIM_APDU), pRI);

#ifdef MEMSET_FREED
    memsetString(apdu.data);
#endif
    free(apdu.data);

#ifdef MEMSET_FREED
    memset(&apdu, 0, sizeof(RIL_SIM_APDU));
#endif

    return;
invalid:
    invalidCommandBlock(pRI);
    return;
}

/**
 * Callee expects const RIL_CallForwardInfo *
 * Payload is:
 *  int32_t status/action
 *  int32_t reason
 *  int32_t serviceCode
 *  int32_t toa
 *  String number  (0 length -> null)
 *  int32_t timeSeconds
 */
static void dispatchCallForward(Parcel& p, RequestInfo* pRI)
{
    RIL_CallForwardInfo cff;
    int32_t t;
    status_t status;

    RLOGD("dispatchCallForward");
    memset(&cff, 0, sizeof(cff));

    // note we only check status at the end

    status = p.readInt32(&t);
    cff.status = (int)t;

    status = p.readInt32(&t);
    cff.reason = (int)t;

    status = p.readInt32(&t);
    cff.serviceClass = (int)t;

    status = p.readInt32(&t);
    cff.toa = (int)t;

    cff.number = strdupReadString(p);

    status = p.readInt32(&t);
    cff.timeSeconds = (int)t;

    if (status != NO_ERROR) {
        goto invalid;
    }

    // special case: number 0-length fields is null

    if (cff.number != NULL && strlen(cff.number) == 0) {
        cff.number = NULL;
    }

    startRequest;
    appendPrintBuf("%sstat=%d,reason=%d,serv=%d,toa=%d,%s,tout=%d", printBuf,
        cff.status, cff.reason, cff.serviceClass, cff.toa,
        (char*)cff.number, cff.timeSeconds);
    closeRequest;
    printRequest(pRI->token, pRI->pCI->requestNumber);

    s_callbacks.onRequest(pRI->pCI->requestNumber, &cff, sizeof(cff), pRI);

#ifdef MEMSET_FREED
    memsetString(cff.number);
#endif

    free(cff.number);

#ifdef MEMSET_FREED
    memset(&cff, 0, sizeof(cff));
#endif

    return;
invalid:
    invalidCommandBlock(pRI);
    return;
}

static void dispatchRaw(Parcel& p, RequestInfo* pRI)
{
    int32_t len;
    status_t status;
    const void* data;

    RLOGD("dispatchRaw");
    status = p.readInt32(&len);

    if (status != NO_ERROR) {
        goto invalid;
    }

    // The java code writes -1 for null arrays
    if (((int)len) == -1) {
        data = NULL;
        len = 0;
    }

    data = p.readInplace(len);

    startRequest;
    appendPrintBuf("%sraw_size=%d", printBuf, len);
    closeRequest;
    printRequest(pRI->token, pRI->pCI->requestNumber);

    s_callbacks.onRequest(pRI->pCI->requestNumber, const_cast<void*>(data), len, pRI);

    return;
invalid:
    invalidCommandBlock(pRI);
    return;
}

static void dispatchImsGsmSms(Parcel& p, RequestInfo* pRI, uint8_t retry,
    int32_t messageRef)
{
    RIL_IMS_SMS_Message rism;
    int32_t countStrings;
    status_t status;
    size_t datalen;
    char** pStrings;
    RLOGD("dispatchImsGsmSms: retry=%d, messageRef=%ld", retry, messageRef);

    status = p.readInt32(&countStrings);

    if (status != NO_ERROR) {
        goto invalid;
    }

    memset(&rism, 0, sizeof(rism));
    rism.tech = RADIO_TECH_3GPP;
    rism.retry = retry;
    rism.messageRef = messageRef;

    startRequest;
    appendPrintBuf("%stech=%d, retry=%d, messageRef=%d, ", printBuf,
        (int)rism.tech, (int)rism.retry, rism.messageRef);
    if (countStrings == 0) {
        // just some non-null pointer
        pStrings = (char**)calloc(1, sizeof(char*));
        if (pStrings == NULL) {
            RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
            closeRequest;
            return;
        }

        datalen = 0;
    } else if (countStrings < 0) {
        pStrings = NULL;
        datalen = 0;
    } else {
        if ((unsigned int)countStrings > (INT_MAX / sizeof(char*))) {
            RLOGE("Invalid value of countStrings: \n");
            closeRequest;
            return;
        }
        datalen = sizeof(char*) * countStrings;

        pStrings = (char**)calloc(countStrings, sizeof(char*));
        if (pStrings == NULL) {
            RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
            closeRequest;
            return;
        }

        for (int i = 0; i < countStrings; i++) {
            pStrings[i] = strdupReadString(p);
            appendPrintBuf("%s%s,", printBuf, pStrings[i]);
        }
    }
    removeLastChar;
    closeRequest;
    printRequest(pRI->token, pRI->pCI->requestNumber);

    rism.message.gsmMessage = pStrings;
    s_callbacks.onRequest(pRI->pCI->requestNumber, &rism,
        sizeof(RIL_RadioTechnologyFamily) + sizeof(uint8_t) + sizeof(int32_t) + datalen,
        pRI);

    if (pStrings != NULL) {
        for (int i = 0; i < countStrings; i++) {
#ifdef MEMSET_FREED
            memsetString(pStrings[i]);
#endif
            free(pStrings[i]);
        }

#ifdef MEMSET_FREED
        memset(pStrings, 0, datalen);
#endif
        free(pStrings);
    }

#ifdef MEMSET_FREED
    memset(&rism, 0, sizeof(rism));
#endif
    return;
invalid:
    RLOGE("dispatchImsGsmSms invalid block");
    invalidCommandBlock(pRI);
    return;
}

static void dispatchImsSms(Parcel& p, RequestInfo* pRI)
{
    int32_t t;
    status_t status = p.readInt32(&t);
    RIL_RadioTechnologyFamily format;
    uint8_t retry;
    int32_t messageRef;

    RLOGD("dispatchImsSms");
    if (status != NO_ERROR) {
        goto invalid;
    }
    format = (RIL_RadioTechnologyFamily)t;

    // read retry field
    status = p.read(&retry, sizeof(retry));
    if (status != NO_ERROR) {
        goto invalid;
    }
    // read messageRef field
    status = p.read(&messageRef, sizeof(messageRef));
    if (status != NO_ERROR) {
        goto invalid;
    }

    if (RADIO_TECH_3GPP == format) {
        dispatchImsGsmSms(p, pRI, retry, messageRef);
    } else {
        RLOGE("requestImsSendSMS invalid format value = %d", format);
    }

    return;

invalid:
    invalidCommandBlock(pRI);
    return;
}

static void dispatchGsmBrSmsCnf(Parcel& p, RequestInfo* pRI)
{
    int32_t t;
    status_t status;
    int32_t num;

    status = p.readInt32(&num);
    if (status != NO_ERROR) {
        goto invalid;
    }

    {
        RIL_GSM_BroadcastSmsConfigInfo gsmBci[num];
        RIL_GSM_BroadcastSmsConfigInfo* gsmBciPtrs[num];

        startRequest;
        for (int i = 0; i < num; i++) {
            gsmBciPtrs[i] = &gsmBci[i];

            status = p.readInt32(&t);
            gsmBci[i].fromServiceId = (int)t;

            status = p.readInt32(&t);
            gsmBci[i].toServiceId = (int)t;

            status = p.readInt32(&t);
            gsmBci[i].fromCodeScheme = (int)t;

            status = p.readInt32(&t);
            gsmBci[i].toCodeScheme = (int)t;

            status = p.readInt32(&t);
            gsmBci[i].selected = (uint8_t)t;

            appendPrintBuf("%s [%d: fromServiceId=%d, toServiceId =%d, \
                  fromCodeScheme=%d, toCodeScheme=%d, selected =%d]",
                printBuf, i,
                gsmBci[i].fromServiceId, gsmBci[i].toServiceId,
                gsmBci[i].fromCodeScheme, gsmBci[i].toCodeScheme,
                gsmBci[i].selected);
        }
        closeRequest;

        if (status != NO_ERROR) {
            goto invalid;
        }

        s_callbacks.onRequest(pRI->pCI->requestNumber,
            gsmBciPtrs,
            num * sizeof(RIL_GSM_BroadcastSmsConfigInfo*),
            pRI);

#ifdef MEMSET_FREED
        memset(gsmBci, 0, num * sizeof(RIL_GSM_BroadcastSmsConfigInfo));
        memset(gsmBciPtrs, 0, num * sizeof(RIL_GSM_BroadcastSmsConfigInfo*));
#endif
    }

    return;

invalid:
    invalidCommandBlock(pRI);
    return;
}

// For backwards compatibility in RIL_REQUEST_SETUP_DATA_CALL.
// Version 4 of the RIL interface adds a new PDP type parameter to support
// IPv6 and dual-stack PDP contexts. When dealing with a previous version of
// RIL, remove the parameter from the request.
static void dispatchDataCall(Parcel& p, RequestInfo* pRI)
{
    // In RIL v3, REQUEST_SETUP_DATA_CALL takes 6 parameters.
    const int numParamsRilV3 = 6;

    // The first bytes of the RIL parcel contain the request number and the
    // serial number - see processCommandBuffer(). Copy them over too.
    int pos = p.dataPosition();

    int numParams = p.readInt32();
    if (s_callbacks.version < 4 && numParams > numParamsRilV3) {
        Parcel p2;
        p2.appendFrom(&p, 0, pos);
        p2.writeInt32(numParamsRilV3);
        p2.setDataPosition(pos);
        dispatchStrings(p2, pRI);
    } else {
        p.setDataPosition(pos);
        dispatchStrings(p, pRI);
    }
}

// For backwards compatibility with RILs that dont support RIL_REQUEST_VOICE_RADIO_TECH.
// When all RILs handle this request, this function can be removed and
// the request can be sent directly to the RIL using dispatchVoid.
static void dispatchVoiceRadioTech(Parcel& p, RequestInfo* pRI)
{
    RIL_RadioState state = s_callbacks.onStateRequest();

    if ((RADIO_STATE_UNAVAILABLE == state) || (RADIO_STATE_OFF == state)) {
        RIL_onRequestComplete(pRI, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
    }

    // RILs that support RADIO_STATE_ON should support this request.
    if (RADIO_STATE_ON == state) {
        dispatchVoid(p, pRI);
        return;
    }

    // For Older RILs, that do not support RADIO_STATE_ON, assume that they
    // will not support this new request either and decode Voice Radio Technology
    // from Radio State
    voiceRadioTech = decodeVoiceRadioTechnology(state);

    if (voiceRadioTech < 0)
        RIL_onRequestComplete(pRI, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onRequestComplete(pRI, RIL_E_SUCCESS, &voiceRadioTech, sizeof(int));
}

static void dispatchSetInitialAttachApn(Parcel& p, RequestInfo* pRI)
{
    RIL_InitialAttachApn pf;
    int32_t t;
    status_t status;

    memset(&pf, 0, sizeof(pf));

    pf.apn = strdupReadString(p);
    pf.protocol = strdupReadString(p);

    status = p.readInt32(&t);
    pf.authtype = (int)t;

    pf.username = strdupReadString(p);
    pf.password = strdupReadString(p);

    startRequest;
    appendPrintBuf("%sapn=%s, protocol=%s, authtype=%d, username=%s, password=%s",
        printBuf, pf.apn, pf.protocol, pf.authtype, pf.username, pf.password);
    closeRequest;
    printRequest(pRI->token, pRI->pCI->requestNumber);

    if (status != NO_ERROR) {
        goto invalid;
    }
    s_callbacks.onRequest(pRI->pCI->requestNumber, &pf, sizeof(pf), pRI);

#ifdef MEMSET_FREED
    memsetString(pf.apn);
    memsetString(pf.protocol);
    memsetString(pf.username);
    memsetString(pf.password);
#endif

    free(pf.apn);
    free(pf.protocol);
    free(pf.username);
    free(pf.password);

#ifdef MEMSET_FREED
    memset(&pf, 0, sizeof(pf));
#endif

    return;
invalid:
    invalidCommandBlock(pRI);
    return;
}

static void dispatchManualSelection(Parcel& p, RequestInfo* pRI)
{
    int32_t t;
    status_t status;
    RIL_NetworkOperator op;

    RLOGD("dispatchManualSelection");
    memset(&op, 0, sizeof(op));

    op.operatorNumeric = strdupReadString(p);

    status = p.readInt32(&t);
    op.act = (RIL_RadioAccessNetworks)t;

    startRequest;
    appendPrintBuf("op=%s,act=%d", op.operatorNumeric, op.act);
    closeRequest;
    printRequest(pRI->token, pRI->pCI->requestNumber);

    if (status != NO_ERROR) {
        goto invalid;
    }

    s_callbacks.onRequest(pRI->pCI->requestNumber, &op, sizeof(RIL_NetworkOperator), pRI);

#ifdef MEMSET_FREED
    memsetString(op.operatorNumeric);
#endif
    free(op.operatorNumeric);

#ifdef MEMSET_FREED
    memset(&op, 0, sizeof(op));
#endif

    return;
invalid:
    invalidCommandBlock(pRI);
    return;
}

static void dispatchDataProfile(Parcel& p, RequestInfo* pRI)
{
    int32_t t;
    status_t status;
    int32_t num;

    status = p.readInt32(&num);
    if (status != NO_ERROR || num < 0) {
        goto invalid;
    }

    {
        RIL_DataProfileInfo* dataProfiles = (RIL_DataProfileInfo*)calloc(num, sizeof(RIL_DataProfileInfo));
        if (dataProfiles == NULL) {
            RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
            return;
        }
        RIL_DataProfileInfo** dataProfilePtrs = (RIL_DataProfileInfo**)calloc(num, sizeof(RIL_DataProfileInfo*));
        if (dataProfilePtrs == NULL) {
            RLOGE("Memory allocation failed for request %s",
                requestToString(pRI->pCI->requestNumber));
            free(dataProfiles);
            return;
        }

        startRequest;
        for (int i = 0; i < num; i++) {
            dataProfilePtrs[i] = &dataProfiles[i];

            status = p.readInt32(&t);
            dataProfiles[i].profileId = (int)t;

            dataProfiles[i].apn = strdupReadString(p);
            dataProfiles[i].protocol = strdupReadString(p);
            status = p.readInt32(&t);
            dataProfiles[i].authType = (int)t;

            dataProfiles[i].user = strdupReadString(p);
            dataProfiles[i].password = strdupReadString(p);

            status = p.readInt32(&t);
            dataProfiles[i].type = (int)t;

            status = p.readInt32(&t);
            dataProfiles[i].maxConnsTime = (int)t;
            status = p.readInt32(&t);
            dataProfiles[i].maxConns = (int)t;
            status = p.readInt32(&t);
            dataProfiles[i].waitTime = (int)t;

            status = p.readInt32(&t);
            dataProfiles[i].enabled = (int)t;

            appendPrintBuf("%s [%d: profileId=%d, apn =%s, protocol =%s, authType =%d, \
                    user =%s, password =%s, type =%d, maxConnsTime =%d, maxConns =%d, \
                    waitTime =%d, enabled =%d]",
                printBuf, i, dataProfiles[i].profileId,
                dataProfiles[i].apn, dataProfiles[i].protocol, dataProfiles[i].authType,
                dataProfiles[i].user, dataProfiles[i].password, dataProfiles[i].type,
                dataProfiles[i].maxConnsTime, dataProfiles[i].maxConns,
                dataProfiles[i].waitTime, dataProfiles[i].enabled);
        }
        closeRequest;
        printRequest(pRI->token, pRI->pCI->requestNumber);
        clearPrintBuf;
        if (status != NO_ERROR) {
            for (int i = 0; i < num; i++) {
                free(dataProfiles[i].apn);
                free(dataProfiles[i].protocol);
                free(dataProfiles[i].user);
                free(dataProfiles[i].password);
            }
            free(dataProfiles);
            free(dataProfilePtrs);
            goto invalid;
        }

        s_callbacks.onRequest(pRI->pCI->requestNumber,
            dataProfilePtrs,
            num * sizeof(RIL_DataProfileInfo*),
            pRI);

#ifdef MEMSET_FREED
        memset(dataProfiles, 0, num * sizeof(RIL_DataProfileInfo));
        memset(dataProfilePtrs, 0, num * sizeof(RIL_DataProfileInfo*));
#endif
        for (int i = 0; i < num; i++) {
            free(dataProfiles[i].apn);
            free(dataProfiles[i].protocol);
            free(dataProfiles[i].user);
            free(dataProfiles[i].password);
        }
        free(dataProfiles);
        free(dataProfilePtrs);
    }

    return;

invalid:
    invalidCommandBlock(pRI);
    return;
}

static void dispatchConferenceInvite(Parcel& p, RequestInfo* pRI)
{
    RIL_ConferenceInvite cinfo;
    int32_t t;
    status_t status;

    RLOGD("dispatchConferenceInvite");
    memset(&cinfo, 0, sizeof(RIL_ConferenceInvite));

    status = p.readInt32(&t);
    cinfo.nparticipants = (int)t;

    if (status != NO_ERROR) {
        goto invalid;
    }

    cinfo.numbers = strdupReadString(p);

    if (!cinfo.numbers) {
        goto invalid;
    }

    s_callbacks.onRequest(pRI->pCI->requestNumber, &cinfo, sizeof(RIL_ConferenceInvite), pRI);

#ifdef MEMSET_FREED
    memsetString(cinfo.numbers);
#endif

    free(cinfo.numbers);
    return;

invalid:
    invalidCommandBlock(pRI);
    return;
}

static int blockingWrite(int fd, const void* buffer, size_t len)
{
    size_t writeOffset = 0;
    const uint8_t* toWrite;

    toWrite = (const uint8_t*)buffer;

    while (writeOffset < len) {
        ssize_t written;
        do {
            written = write(fd, toWrite + writeOffset,
                len - writeOffset);
        } while (written < 0 && ((errno == EINTR) || (errno == EAGAIN)));

        if (written >= 0) {
            writeOffset += written;
        } else { // written < 0
            RLOGE("RIL Response: unexpected error on write errno: %d", errno);
            close(fd);
            return -1;
        }
    }

    RLOGD("RIL Response bytes written: %zu", writeOffset);

    return 0;
}

static int sendResponseRaw(const void* data, size_t dataSize)
{
    int fd = s_fdCommand;
    int ret;
    uint32_t header;

    if (s_fdCommand < 0) {
        RLOGE("RIL: no valid fd for URC channel");
        return -1;
    }

    if (dataSize > MAX_COMMAND_BYTES) {
        RLOGE("RIL: packet larger than %u (%u)",
            MAX_COMMAND_BYTES, (unsigned int)dataSize);

        return -1;
    }

    pthread_mutex_lock(&s_writeMutex);

    header = htonl(dataSize);

    ret = blockingWrite(fd, (void*)&header, sizeof(header));

    if (ret < 0) {
        pthread_mutex_unlock(&s_writeMutex);
        return ret;
    }

    ret = blockingWrite(fd, data, dataSize);

    if (ret < 0) {
        pthread_mutex_unlock(&s_writeMutex);
        return ret;
    }

    pthread_mutex_unlock(&s_writeMutex);

    return 0;
}

static int sendResponse(Parcel& p)
{
    printResponse;
    return sendResponseRaw(p.data(), p.dataSize());
}

static int responseInts(Parcel& p, void* response, size_t responselen)
{
    int numInts;

    if (response == NULL && responselen != 0) {
        RLOGE("invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }
    if (responselen % sizeof(int) != 0) {
        RLOGE("responseInts: invalid response length %d expected multiple of %d\n",
            (int)responselen, (int)sizeof(int));
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    int* p_int = (int*)response;

    numInts = responselen / sizeof(int*);
    p.writeInt32(numInts);

    /* each int*/
    startResponse;
    for (int i = 0; i < numInts; i++) {
        appendPrintBuf("%s%d,", printBuf, p_int[i]);
        p.writeInt32(p_int[i]);
    }
    removeLastChar;
    closeResponse;

    return 0;
}

/* response is a char **, pointing to an array of char *'s
 * The parcel will begin with the version */
static int responseStringsWithVersion(int version, Parcel& p, void* response,
    size_t responselen)
{
    p.writeInt32(version);
    return responseStrings(p, response, responselen);
}

/* response is a char **, pointing to an array of char *'s */
static int responseStrings(Parcel& p, void* response, size_t responselen)
{
    int numStrings;

    if (response == NULL && responselen != 0) {
        RLOGE("invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }
    if (responselen % sizeof(char*) != 0) {
        RLOGE("responseStrings: invalid response length %d expected multiple of %d\n",
            (int)responselen, (int)sizeof(char*));
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    if (response == NULL) {
        p.writeInt32(0);
    } else {
        char** p_cur = (char**)response;

        numStrings = responselen / sizeof(char*);
        p.writeInt32(numStrings);

        /* each string*/
        startResponse;
        for (int i = 0; i < numStrings; i++) {
            appendPrintBuf("%s%s,", printBuf, (char*)p_cur[i]);
            writeStringToParcel(p, p_cur[i]);
        }
        removeLastChar;
        closeResponse;
    }
    return 0;
}

/**
 * NULL strings are accepted
 * FIXME currently ignores responselen
 */
static int responseString(Parcel& p, void* response, size_t responselen)
{
    /* one string only */
    startResponse;
    appendPrintBuf("%s%s", printBuf, (char*)response);
    closeResponse;

    writeStringToParcel(p, (const char*)response);

    return 0;
}

static int responseVoid(Parcel& p, void* response, size_t responselen)
{
    startResponse;
    removeLastChar;
    return 0;
}

static int responseCallList(Parcel& p, void* response, size_t responselen)
{
    int num;

    if (response == NULL && responselen != 0) {
        RLOGE("invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    if (responselen % sizeof(RIL_Call*) != 0) {
        RLOGE("responseCallList: invalid response length %d expected multiple of %d\n",
            (int)responselen, (int)sizeof(RIL_Call*));
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    startResponse;
    /* number of call info's */
    num = responselen / sizeof(RIL_Call*);
    p.writeInt32(num);

    for (int i = 0; i < num; i++) {
        RIL_Call* p_cur = ((RIL_Call**)response)[i];
        /* each call info */
        p.writeInt32(p_cur->state);
        p.writeInt32(p_cur->index);
        p.writeInt32(p_cur->toa);
        p.writeInt32(p_cur->isMpty);
        p.writeInt32(p_cur->isMT);
        p.writeInt32(p_cur->als);
        p.writeInt32(p_cur->isVoice);
        p.writeInt32(p_cur->isVoicePrivacy);
        writeStringToParcel(p, p_cur->number);
        p.writeInt32(p_cur->numberPresentation);
        writeStringToParcel(p, p_cur->name);
        p.writeInt32(p_cur->namePresentation);
        // Remove when partners upgrade to version 3
        if ((s_callbacks.version < 3) || (p_cur->uusInfo == NULL || p_cur->uusInfo->uusData == NULL)) {
            p.writeInt32(0); /* UUS Information is absent */
        } else {
            RIL_UUS_Info* uusInfo = p_cur->uusInfo;
            p.writeInt32(1); /* UUS Information is present */
            p.writeInt32(uusInfo->uusType);
            p.writeInt32(uusInfo->uusDcs);
            p.writeInt32(uusInfo->uusLength);
            p.write(uusInfo->uusData, uusInfo->uusLength);
        }
        appendPrintBuf("%s[id=%d,%s,toa=%d,",
            printBuf,
            p_cur->index,
            callStateToString(p_cur->state),
            p_cur->toa);
        appendPrintBuf("%s%s,%s,als=%d,%s,%s,",
            printBuf,
            (p_cur->isMpty) ? "conf" : "norm",
            (p_cur->isMT) ? "mt" : "mo",
            p_cur->als,
            (p_cur->isVoice) ? "voc" : "nonvoc",
            (p_cur->isVoicePrivacy) ? "evp" : "noevp");
        appendPrintBuf("%s%s,cli=%d,name='%s',%d]",
            printBuf,
            p_cur->number,
            p_cur->numberPresentation,
            p_cur->name,
            p_cur->namePresentation);
    }
    removeLastChar;
    closeResponse;

    return 0;
}

static int responseSMS(Parcel& p, void* response, size_t responselen)
{
    if (response == NULL) {
        RLOGE("invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    if (responselen != sizeof(RIL_SMS_Response)) {
        RLOGE("invalid response length %d expected %d",
            (int)responselen, (int)sizeof(RIL_SMS_Response));
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    RIL_SMS_Response* p_cur = (RIL_SMS_Response*)response;

    p.writeInt32(p_cur->messageRef);
    writeStringToParcel(p, p_cur->ackPDU);
    p.writeInt32(p_cur->errorCode);

    startResponse;
    appendPrintBuf("%s%d,%s,%d", printBuf, p_cur->messageRef,
        (char*)p_cur->ackPDU, p_cur->errorCode);
    closeResponse;

    return 0;
}

static int responseDataCallListV4(Parcel& p, void* response, size_t responselen)
{
    if (response == NULL && responselen != 0) {
        RLOGE("invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    if (responselen % sizeof(RIL_Data_Call_Response_v4) != 0) {
        RLOGE("responseDataCallListV4: invalid response length %d expected multiple of %d",
            (int)responselen, (int)sizeof(RIL_Data_Call_Response_v4));
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    int num = responselen / sizeof(RIL_Data_Call_Response_v4);
    p.writeInt32(num);

    RIL_Data_Call_Response_v4* p_cur = (RIL_Data_Call_Response_v4*)response;
    startResponse;
    int i;
    for (i = 0; i < num; i++) {
        p.writeInt32(p_cur[i].cid);
        p.writeInt32(p_cur[i].active);
        writeStringToParcel(p, p_cur[i].type);
        // apn is not used, so don't send.
        writeStringToParcel(p, p_cur[i].address);
        appendPrintBuf("%s[cid=%d,%s,%s,%s],", printBuf,
            p_cur[i].cid,
            (p_cur[i].active == 0) ? "down" : "up",
            (char*)p_cur[i].type,
            (char*)p_cur[i].address);
    }
    removeLastChar;
    closeResponse;

    return 0;
}

static int responseDataCallList(Parcel& p, void* response, size_t responselen)
{
    // Write version
    p.writeInt32(s_callbacks.version);

    if (s_callbacks.version < 5) {
        return responseDataCallListV4(p, response, responselen);
    } else {
        if (response == NULL && responselen != 0) {
            RLOGE("invalid response: NULL \n");
            return RIL_ERRNO_INVALID_RESPONSE;
        }

        if (responselen % sizeof(RIL_Data_Call_Response_v11) != 0) {
            RLOGE("invalid response length %d expected multiple of %d",
                (int)responselen, (int)sizeof(RIL_Data_Call_Response_v11));
            return RIL_ERRNO_INVALID_RESPONSE;
        }

        int num = responselen / sizeof(RIL_Data_Call_Response_v11);
        p.writeInt32(num);

        RIL_Data_Call_Response_v11* p_cur = (RIL_Data_Call_Response_v11*)response;
        startResponse;
        int i;
        for (i = 0; i < num; i++) {
            p.writeInt32((int)p_cur[i].status);
            p.writeInt32(p_cur[i].suggestedRetryTime);
            p.writeInt32(p_cur[i].cid);
            p.writeInt32(p_cur[i].active);
            writeStringToParcel(p, p_cur[i].type);
            writeStringToParcel(p, p_cur[i].ifname);
            writeStringToParcel(p, p_cur[i].addresses);
            writeStringToParcel(p, p_cur[i].dnses);
            writeStringToParcel(p, p_cur[i].gateways);
            writeStringToParcel(p, p_cur[i].pcscf);
            p.writeInt32(p_cur[i].mtu);
            appendPrintBuf("%s[status=%d,retry=%d,cid=%d,%s,%s,%s,%s,%s,%s],", printBuf,
                p_cur[i].status,
                p_cur[i].suggestedRetryTime,
                p_cur[i].cid,
                (p_cur[i].active == 0) ? "down" : "up",
                (char*)p_cur[i].type,
                (char*)p_cur[i].ifname,
                (char*)p_cur[i].addresses,
                (char*)p_cur[i].dnses,
                (char*)p_cur[i].gateways,
                (char*)p_cur[i].pcscf,
                p_cur[i].mtu);
        }
        removeLastChar;
        closeResponse;
    }

    return 0;
}

static int responseSetupDataCall(Parcel& p, void* response, size_t responselen)
{
    if (s_callbacks.version < 5) {
        return responseStringsWithVersion(s_callbacks.version, p, response, responselen);
    } else {
        return responseDataCallList(p, response, responselen);
    }
}

static int responseRaw(Parcel& p, void* response, size_t responselen)
{
    if (response == NULL && responselen != 0) {
        RLOGE("invalid response: NULL with responselen != 0");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    // The java code reads -1 size as null byte array
    if (response == NULL) {
        p.writeInt32(-1);
    } else {
        p.writeInt32(responselen);
        p.write(response, responselen);
    }

    return 0;
}

static int responseSIM_IO(Parcel& p, void* response, size_t responselen)
{
    if (response == NULL) {
        RLOGE("invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    if (responselen != sizeof(RIL_SIM_IO_Response)) {
        RLOGE("invalid response length was %d expected %d",
            (int)responselen, (int)sizeof(RIL_SIM_IO_Response));
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    RIL_SIM_IO_Response* p_cur = (RIL_SIM_IO_Response*)response;
    p.writeInt32(p_cur->sw1);
    p.writeInt32(p_cur->sw2);
    writeStringToParcel(p, p_cur->simResponse);

    startResponse;
    appendPrintBuf("%ssw1=0x%X,sw2=0x%X,%s", printBuf, p_cur->sw1, p_cur->sw2,
        (char*)p_cur->simResponse);
    closeResponse;

    return 0;
}

static int responseCallForwards(Parcel& p, void* response, size_t responselen)
{
    int num;

    if (response == NULL && responselen != 0) {
        RLOGE("invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    if (responselen % sizeof(RIL_CallForwardInfo*) != 0) {
        RLOGE("responseCallForwards: invalid response length %d expected multiple of %d",
            (int)responselen, (int)sizeof(RIL_CallForwardInfo*));
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    /* number of call info's */
    num = responselen / sizeof(RIL_CallForwardInfo*);
    p.writeInt32(num);

    startResponse;
    for (int i = 0; i < num; i++) {
        RIL_CallForwardInfo* p_cur = ((RIL_CallForwardInfo**)response)[i];

        p.writeInt32(p_cur->status);
        p.writeInt32(p_cur->reason);
        p.writeInt32(p_cur->serviceClass);
        p.writeInt32(p_cur->toa);
        writeStringToParcel(p, p_cur->number);
        p.writeInt32(p_cur->timeSeconds);
        appendPrintBuf("%s[%s,reason=%d,cls=%d,toa=%d,%s,tout=%d],", printBuf,
            (p_cur->status == 1) ? "enable" : "disable",
            p_cur->reason, p_cur->serviceClass, p_cur->toa,
            (char*)p_cur->number,
            p_cur->timeSeconds);
    }
    removeLastChar;
    closeResponse;

    return 0;
}

static int responseSsn(Parcel& p, void* response, size_t responselen)
{
    if (response == NULL) {
        RLOGE("invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    if (responselen != sizeof(RIL_SuppSvcNotification)) {
        RLOGE("invalid response length was %d expected %d",
            (int)responselen, (int)sizeof(RIL_SuppSvcNotification));
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    RIL_SuppSvcNotification* p_cur = (RIL_SuppSvcNotification*)response;
    p.writeInt32(p_cur->notificationType);
    p.writeInt32(p_cur->code);
    p.writeInt32(p_cur->index);
    p.writeInt32(p_cur->type);
    writeStringToParcel(p, p_cur->number);

    startResponse;
    appendPrintBuf("%s%s,code=%d,id=%d,type=%d,%s", printBuf,
        (p_cur->notificationType == 0) ? "mo" : "mt",
        p_cur->code, p_cur->index, p_cur->type,
        (char*)p_cur->number);
    closeResponse;

    return 0;
}

static int responseCellList(Parcel& p, void* response, size_t responselen)
{
    int num;

    if (response == NULL && responselen != 0) {
        RLOGE("invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    if (responselen % sizeof(RIL_NeighboringCell*) != 0) {
        RLOGE("responseCellList: invalid response length %d expected multiple of %d\n",
            (int)responselen, (int)sizeof(RIL_NeighboringCell*));
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    startResponse;
    /* number of records */
    num = responselen / sizeof(RIL_NeighboringCell*);
    p.writeInt32(num);

    for (int i = 0; i < num; i++) {
        RIL_NeighboringCell* p_cur = &((RIL_NeighboringCell*)response)[i];

        p.writeInt32(p_cur->rssi);
        writeStringToParcel(p, p_cur->cid);

        appendPrintBuf("%s[cid=%s,rssi=%d],", printBuf,
            p_cur->cid, p_cur->rssi);
    }
    removeLastChar;
    closeResponse;

    return 0;
}

static int responseRilSignalStrength(Parcel& p,
    void* response, size_t responselen)
{
    if (response == NULL && responselen != 0) {
        RLOGE("invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    if (responselen >= sizeof(RIL_SignalStrength_v5)) {
        RIL_SignalStrength_v6* p_cur = ((RIL_SignalStrength_v6*)response);

        p.writeInt32(p_cur->GW_SignalStrength.signalStrength);
        p.writeInt32(p_cur->GW_SignalStrength.bitErrorRate);
        p.writeInt32(p_cur->CDMA_SignalStrength.dbm);
        p.writeInt32(p_cur->CDMA_SignalStrength.ecio);
        p.writeInt32(p_cur->EVDO_SignalStrength.dbm);
        p.writeInt32(p_cur->EVDO_SignalStrength.ecio);
        p.writeInt32(p_cur->EVDO_SignalStrength.signalNoiseRatio);
        if (responselen >= sizeof(RIL_SignalStrength_v6)) {
            /* Fixup LTE for backwards compatibility */
            if (s_callbacks.version <= 6) {
                // signalStrength: -1 -> 99
                if (p_cur->LTE_SignalStrength.signalStrength == -1) {
                    p_cur->LTE_SignalStrength.signalStrength = 99;
                }
                // rsrp: -1 -> INT_MAX all other negative value to positive.
                // So remap here
                if (p_cur->LTE_SignalStrength.rsrp == -1) {
                    p_cur->LTE_SignalStrength.rsrp = INT_MAX;
                } else if (p_cur->LTE_SignalStrength.rsrp < -1) {
                    p_cur->LTE_SignalStrength.rsrp = -p_cur->LTE_SignalStrength.rsrp;
                }
                // rsrq: -1 -> INT_MAX
                if (p_cur->LTE_SignalStrength.rsrq == -1) {
                    p_cur->LTE_SignalStrength.rsrq = INT_MAX;
                }
                // Not remapping rssnr is already using INT_MAX

                // cqi: -1 -> INT_MAX
                if (p_cur->LTE_SignalStrength.cqi == -1) {
                    p_cur->LTE_SignalStrength.cqi = INT_MAX;
                }
            }
            p.writeInt32(p_cur->LTE_SignalStrength.signalStrength);
            p.writeInt32(p_cur->LTE_SignalStrength.rsrp);
            p.writeInt32(p_cur->LTE_SignalStrength.rsrq);
            p.writeInt32(p_cur->LTE_SignalStrength.rssnr);
            p.writeInt32(p_cur->LTE_SignalStrength.cqi);
        } else {
            p.writeInt32(99);
            p.writeInt32(INT_MAX);
            p.writeInt32(INT_MAX);
            p.writeInt32(INT_MAX);
            p.writeInt32(INT_MAX);
        }

        startResponse;
        appendPrintBuf("%s[signalStrength=%d,bitErrorRate=%d,\
                CDMA_SS.dbm=%d,CDMA_SSecio=%d,\
                EVDO_SS.dbm=%d,EVDO_SS.ecio=%d,\
                EVDO_SS.signalNoiseRatio=%d,\
                LTE_SS.signalStrength=%d,LTE_SS.rsrp=%d,LTE_SS.rsrq=%d,\
                LTE_SS.rssnr=%d,LTE_SS.cqi=%d]",
            printBuf,
            p_cur->GW_SignalStrength.signalStrength,
            p_cur->GW_SignalStrength.bitErrorRate,
            p_cur->CDMA_SignalStrength.dbm,
            p_cur->CDMA_SignalStrength.ecio,
            p_cur->EVDO_SignalStrength.dbm,
            p_cur->EVDO_SignalStrength.ecio,
            p_cur->EVDO_SignalStrength.signalNoiseRatio,
            p_cur->LTE_SignalStrength.signalStrength,
            p_cur->LTE_SignalStrength.rsrp,
            p_cur->LTE_SignalStrength.rsrq,
            p_cur->LTE_SignalStrength.rssnr,
            p_cur->LTE_SignalStrength.cqi);
        closeResponse;

    } else if (responselen % sizeof(int) == 0) {
        // Old RIL deprecated
        int* p_cur = (int*)response;

        startResponse;

        // With the Old RIL we see one or 2 integers.
        size_t num = responselen / sizeof(int); // Number of integers from ril
        size_t totalIntegers = 7; // Number of integers in RIL_SignalStrength
        size_t i;

        appendPrintBuf("%s[", printBuf);
        for (i = 0; i < num; i++) {
            appendPrintBuf("%s %d", printBuf, *p_cur);
            p.writeInt32(*p_cur++);
        }
        appendPrintBuf("%s]", printBuf);

        // Fill the remainder with zero's.
        for (; i < totalIntegers; i++) {
            p.writeInt32(0);
        }

        closeResponse;

    } else {
        RLOGE("invalid response length \n");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    return 0;
}

static int responseSimRefresh(Parcel& p, void* response, size_t responselen)
{
    if (response == NULL && responselen != 0) {
        RLOGE("responseSimRefresh: invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    if (!response && !responselen) {
        RLOGW("Empty response");
        return 0;
    }

    startResponse;
    if (s_callbacks.version == 7) {
        RIL_SimRefreshResponse_v7* p_cur = ((RIL_SimRefreshResponse_v7*)response);
        p.writeInt32(p_cur->result);
        p.writeInt32(p_cur->ef_id);
        writeStringToParcel(p, p_cur->aid);

        appendPrintBuf("%sresult=%d, ef_id=%d, aid=%s",
            printBuf,
            p_cur->result,
            p_cur->ef_id,
            p_cur->aid);
    } else {
        int* p_cur = ((int*)response);
        p.writeInt32(p_cur[0]);
        p.writeInt32(p_cur[1]);
        writeStringToParcel(p, NULL);

        appendPrintBuf("%sresult=%d, ef_id=%d",
            printBuf,
            p_cur[0],
            p_cur[1]);
    }
    closeResponse;

    return 0;
}

static int responseCellInfoList(Parcel& p, void* response, size_t responselen)
{
    if (response == NULL && responselen != 0) {
        RLOGE("invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    if (responselen % sizeof(RIL_CellInfo) != 0) {
        RLOGE("responseCellInfoList: invalid response length %d expected multiple of %d",
            (int)responselen, (int)sizeof(RIL_CellInfo));
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    int num = responselen / sizeof(RIL_CellInfo);
    p.writeInt32(num);

    RIL_CellInfo* p_cur = (RIL_CellInfo*)response;
    startResponse;
    int i;
    for (i = 0; i < num; i++) {
        appendPrintBuf("%s[%d: type=%d,registered=%d,timeStampType=%d,timeStamp=%lld", printBuf, i,
            p_cur->cellInfoType, p_cur->registered, p_cur->timeStampType, p_cur->timeStamp);
        p.writeInt32((int)p_cur->cellInfoType);
        p.writeInt32(p_cur->registered);
        p.writeInt32(p_cur->timeStampType);
        p.writeInt64(p_cur->timeStamp);
        switch (p_cur->cellInfoType) {
        case RIL_CELL_INFO_TYPE_GSM: {
            appendPrintBuf("%s GSM id: mcc=%d,mnc=%d,lac=%d,cid=%d,", printBuf,
                p_cur->CellInfo.gsm.cellIdentityGsm.mcc,
                p_cur->CellInfo.gsm.cellIdentityGsm.mnc,
                p_cur->CellInfo.gsm.cellIdentityGsm.lac,
                p_cur->CellInfo.gsm.cellIdentityGsm.cid);
            appendPrintBuf("%s gsmSS: ss=%d,ber=%d],", printBuf,
                p_cur->CellInfo.gsm.signalStrengthGsm.signalStrength,
                p_cur->CellInfo.gsm.signalStrengthGsm.bitErrorRate);

            p.writeInt32(p_cur->CellInfo.gsm.cellIdentityGsm.mcc);
            p.writeInt32(p_cur->CellInfo.gsm.cellIdentityGsm.mnc);
            p.writeInt32(p_cur->CellInfo.gsm.cellIdentityGsm.lac);
            p.writeInt32(p_cur->CellInfo.gsm.cellIdentityGsm.cid);
            p.writeInt32(p_cur->CellInfo.gsm.signalStrengthGsm.signalStrength);
            p.writeInt32(p_cur->CellInfo.gsm.signalStrengthGsm.bitErrorRate);
            break;
        }
        case RIL_CELL_INFO_TYPE_WCDMA: {
            appendPrintBuf("%s WCDMA id: mcc=%d,mnc=%d,lac=%d,cid=%d,psc=%d,", printBuf,
                p_cur->CellInfo.wcdma.cellIdentityWcdma.mcc,
                p_cur->CellInfo.wcdma.cellIdentityWcdma.mnc,
                p_cur->CellInfo.wcdma.cellIdentityWcdma.lac,
                p_cur->CellInfo.wcdma.cellIdentityWcdma.cid,
                p_cur->CellInfo.wcdma.cellIdentityWcdma.psc);
            appendPrintBuf("%s wcdmaSS: ss=%d,ber=%d],", printBuf,
                p_cur->CellInfo.wcdma.signalStrengthWcdma.signalStrength,
                p_cur->CellInfo.wcdma.signalStrengthWcdma.bitErrorRate);

            p.writeInt32(p_cur->CellInfo.wcdma.cellIdentityWcdma.mcc);
            p.writeInt32(p_cur->CellInfo.wcdma.cellIdentityWcdma.mnc);
            p.writeInt32(p_cur->CellInfo.wcdma.cellIdentityWcdma.lac);
            p.writeInt32(p_cur->CellInfo.wcdma.cellIdentityWcdma.cid);
            p.writeInt32(p_cur->CellInfo.wcdma.cellIdentityWcdma.psc);
            p.writeInt32(p_cur->CellInfo.wcdma.signalStrengthWcdma.signalStrength);
            p.writeInt32(p_cur->CellInfo.wcdma.signalStrengthWcdma.bitErrorRate);
            break;
        }
        case RIL_CELL_INFO_TYPE_LTE: {
            appendPrintBuf("%s LTE id: mcc=%d,mnc=%d,ci=%d,pci=%d,tac=%d", printBuf,
                p_cur->CellInfo.lte.cellIdentityLte.mcc,
                p_cur->CellInfo.lte.cellIdentityLte.mnc,
                p_cur->CellInfo.lte.cellIdentityLte.ci,
                p_cur->CellInfo.lte.cellIdentityLte.pci,
                p_cur->CellInfo.lte.cellIdentityLte.tac);

            p.writeInt32(p_cur->CellInfo.lte.cellIdentityLte.mcc);
            p.writeInt32(p_cur->CellInfo.lte.cellIdentityLte.mnc);
            p.writeInt32(p_cur->CellInfo.lte.cellIdentityLte.ci);
            p.writeInt32(p_cur->CellInfo.lte.cellIdentityLte.pci);
            p.writeInt32(p_cur->CellInfo.lte.cellIdentityLte.tac);

            appendPrintBuf("%s lteSS: ss=%d,rsrp=%d,rsrq=%d,rssnr=%d,cqi=%d,ta=%d", printBuf,
                p_cur->CellInfo.lte.signalStrengthLte.signalStrength,
                p_cur->CellInfo.lte.signalStrengthLte.rsrp,
                p_cur->CellInfo.lte.signalStrengthLte.rsrq,
                p_cur->CellInfo.lte.signalStrengthLte.rssnr,
                p_cur->CellInfo.lte.signalStrengthLte.cqi,
                p_cur->CellInfo.lte.signalStrengthLte.timingAdvance);
            p.writeInt32(p_cur->CellInfo.lte.signalStrengthLte.signalStrength);
            p.writeInt32(p_cur->CellInfo.lte.signalStrengthLte.rsrp);
            p.writeInt32(p_cur->CellInfo.lte.signalStrengthLte.rsrq);
            p.writeInt32(p_cur->CellInfo.lte.signalStrengthLte.rssnr);
            p.writeInt32(p_cur->CellInfo.lte.signalStrengthLte.cqi);
            p.writeInt32(p_cur->CellInfo.lte.signalStrengthLte.timingAdvance);
            break;
        }
        default:
            break;
        }
        p_cur += 1;
    }
    removeLastChar;
    closeResponse;

    return 0;
}

static void triggerEvLoop(void)
{
    int ret = 0;
    /* trigger event loop to wakeup. No reason to do this,
     * if we're in the event loop thread */
    if (!pthread_equal(pthread_self(), s_tid_dispatch)) {
        do {
            ret = write(s_fdWakeupWrite, " ", 1);
        } while (ret < 0 && errno == EINTR);
    }
}

static void rilEventAddWakeup(struct ril_event* ev)
{
    ril_event_add(ev);
    triggerEvLoop();
}

static void sendSimStatusAppInfo(Parcel& p, int num_apps, RIL_AppStatus appStatus[])
{
    p.writeInt32(num_apps);
    startResponse;
    for (int i = 0; i < num_apps; i++) {
        p.writeInt32(appStatus[i].app_type);
        p.writeInt32(appStatus[i].app_state);
        p.writeInt32(appStatus[i].perso_substate);
        writeStringToParcel(p, (const char*)(appStatus[i].aid_ptr));
        writeStringToParcel(p, (const char*)(appStatus[i].app_label_ptr));
        p.writeInt32(appStatus[i].pin1_replaced);
        p.writeInt32(appStatus[i].pin1);
        p.writeInt32(appStatus[i].pin2);
        appendPrintBuf("%s[app_type=%d,app_state=%d,perso_substate=%d,\
                    aid_ptr=%s,app_label_ptr=%s,pin1_replaced=%d,pin1=%d,pin2=%d],",
            printBuf,
            appStatus[i].app_type,
            appStatus[i].app_state,
            appStatus[i].perso_substate,
            appStatus[i].aid_ptr,
            appStatus[i].app_label_ptr,
            appStatus[i].pin1_replaced,
            appStatus[i].pin1,
            appStatus[i].pin2);
    }
    closeResponse;
}

static int responseSimStatus(Parcel& p, void* response, size_t responselen)
{
    if (response == NULL && responselen != 0) {
        RLOGE("invalid response: NULL");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    if (responselen == sizeof(RIL_CardStatus_v1_5)) {
        RIL_CardStatus_v1_5* p_cur = ((RIL_CardStatus_v1_5*)response);

        p.writeInt32(p_cur->base.base.base.card_state);
        p.writeInt32(p_cur->base.base.base.universal_pin_state);
        p.writeInt32(p_cur->base.base.base.gsm_umts_subscription_app_index);
        p.writeInt32(p_cur->base.base.base.cdma_subscription_app_index);
        p.writeInt32(p_cur->base.base.base.ims_subscription_app_index);

        sendSimStatusAppInfo(p, p_cur->base.base.base.num_applications, p_cur->base.base.base.applications);
    } else {
        RLOGE("responseSimStatus: RilCardStatus version error\n");
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    return 0;
}

static int responseImsStatus(Parcel& p, void* response, size_t responselen)
{
    RIL_IMS_REGISTRATION_STATE_RESPONSE* p_cur = NULL;

    p_cur = ((RIL_IMS_REGISTRATION_STATE_RESPONSE*)response);

    p.writeInt32(p_cur->reg_state);
    p.writeInt32(p_cur->service_type);
    writeStringToParcel(p, (const char*)p_cur->uri_response);

    return 0;
}

static int responseGsmBrSmsCnf(Parcel& p, void* response, size_t responselen)
{
    int num = responselen / sizeof(RIL_GSM_BroadcastSmsConfigInfo*);
    p.writeInt32(num);

    startResponse;
    RIL_GSM_BroadcastSmsConfigInfo** p_cur = (RIL_GSM_BroadcastSmsConfigInfo**)response;
    for (int i = 0; i < num; i++) {
        p.writeInt32(p_cur[i]->fromServiceId);
        p.writeInt32(p_cur[i]->toServiceId);
        p.writeInt32(p_cur[i]->fromCodeScheme);
        p.writeInt32(p_cur[i]->toCodeScheme);
        p.writeInt32(p_cur[i]->selected);

        appendPrintBuf("%s [%d: fromServiceId=%d, toServiceId=%d, \
                fromCodeScheme=%d, toCodeScheme=%d, selected =%d]",
            printBuf, i, p_cur[i]->fromServiceId, p_cur[i]->toServiceId,
            p_cur[i]->fromCodeScheme, p_cur[i]->toCodeScheme,
            p_cur[i]->selected);
    }
    closeResponse;

    return 0;
}

static int responseActivityData(Parcel& p, void* response, size_t responselen)
{
    if (response == NULL || responselen != sizeof(RIL_ActivityStatsInfo)) {
        if (response == NULL) {
            RLOGE("invalid response: NULL");
        } else {
            RLOGE("responseActivityData: invalid response length %d expecting len: %d",
                sizeof(RIL_ActivityStatsInfo), responselen);
        }
        return RIL_ERRNO_INVALID_RESPONSE;
    }

    RIL_ActivityStatsInfo* p_cur = (RIL_ActivityStatsInfo*)response;
    p.writeInt32(p_cur->sleep_mode_time_ms);
    p.writeInt32(p_cur->idle_mode_time_ms);
    for (int i = 0; i < RIL_NUM_TX_POWER_LEVELS; i++) {
        p.writeInt32(p_cur->tx_mode_time_ms[i]);
    }
    p.writeInt32(p_cur->rx_mode_time_ms);

    startResponse;
    appendPrintBuf("Modem activity info received: sleep_mode_time_ms %d idle_mode_time_ms %d \
                  tx_mode_time_ms %d %d %d %d %d and rx_mode_time_ms %d",
        p_cur->sleep_mode_time_ms, p_cur->idle_mode_time_ms, p_cur->tx_mode_time_ms[0],
        p_cur->tx_mode_time_ms[1], p_cur->tx_mode_time_ms[2], p_cur->tx_mode_time_ms[3],
        p_cur->tx_mode_time_ms[4], p_cur->rx_mode_time_ms);
    closeResponse;

    return 0;
}

/**
 * A write on the wakeup fd is done just to pop us out of select()
 * We empty the buffer here and then ril_event will reset the timers on the
 * way back down
 */
static void processWakeupCallback(int fd, short flags, void* param)
{
    char buff[16];
    int ret;

    /* empty our wakeup socket out */
    do {
        ret = read(s_fdWakeupRead, &buff, sizeof(buff));
    } while (ret > 0 || (ret < 0 && errno == EINTR));
}

static void onCommandsSocketClosed(void)
{
    int ret = 0;
    RequestInfo* p_cur;

    (void)ret;

    /* mark pending requests as "cancelled" so we dont report responses */
    ret = pthread_mutex_lock(&s_pendingRequestsMutex);
    assert(ret == 0);

    p_cur = s_pendingRequests;

    for (p_cur = s_pendingRequests; p_cur != NULL; p_cur = p_cur->p_next) {
        p_cur->cancelled = 1;
    }

    ret = pthread_mutex_unlock(&s_pendingRequestsMutex);
    assert(ret == 0);
}

static void processCommandsCallback(int fd, short flags, void* param)
{
    RecordStream* p_rs;
    void* p_record;
    size_t recordlen;
    int ret;

    assert(fd == s_fdCommand);

    p_rs = (RecordStream*)param;

    for (;;) {
        /* loop until EAGAIN/EINTR, end of stream, or other error */
        ret = record_stream_get_next(p_rs, &p_record, &recordlen);

        if (ret == 0 && p_record == NULL) {
            /* end-of-stream */
            break;
        } else if (ret < 0) {
            break;
        } else if (ret == 0) { /* && p_record != NULL */
            processCommandBuffer(p_record, recordlen);
        }
    }

    if (ret == 0 || !(errno == EAGAIN || errno == EINTR)) {
        /* fatal error or end-of-stream */
        if (ret != 0) {
            RLOGE("error on reading command socket errno: %d\n", errno);
        } else {
            RLOGW("EOS.  Closing command socket.");
        }

        close(s_fdCommand);
        s_fdCommand = -1;

        ril_event_del(&s_commands_event);

        record_stream_free(p_rs);

        /* start listening for new connections again */
        rilEventAddWakeup(&s_listen_event);

        onCommandsSocketClosed();
    }
}

static void onNewCommandConnect(void)
{
    // Inform we are connected and the ril version
    int rilVer = s_callbacks.version;
    RLOGD("RIL_UNSOL_RIL_CONNECTED message send");
    RIL_onUnsolicitedResponse(RIL_UNSOL_RIL_CONNECTED, &rilVer, sizeof(rilVer));

    // implicit radio state changed
    RLOGD("RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED message send");
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED, NULL, 0);

    // Send last NITZ time data, in case it was missed
    if (s_lastNITZTimeData != NULL) {
        sendResponseRaw(s_lastNITZTimeData, s_lastNITZTimeDataSize);

        free(s_lastNITZTimeData);
        s_lastNITZTimeData = NULL;
    }
}

static void listenCallback(int fd, short flags, void* param)
{
    int ret;
    RecordStream* p_rs;

    struct sockaddr_un peeraddr;
    socklen_t socklen = sizeof(peeraddr);

    assert(s_fdCommand < 0);
    assert(fd == s_fdListen);

    s_fdCommand = accept(s_fdListen, (struct sockaddr*)&peeraddr, &socklen);

    if (s_fdCommand < 0) {
        RLOGE("Error on accept() errno: %d", errno);
        /* start listening for new connections again */
        rilEventAddWakeup(&s_listen_event);
        return;
    }

    /* check the credential of the other side and only accept socket from
     * phone process */
    errno = 0;

    ret = fcntl(s_fdCommand, F_SETFL, O_NONBLOCK);

    if (ret < 0) {
        RLOGE("Error setting O_NONBLOCK errno: %d", errno);
    }

    RLOGI("new client connect");
    p_rs = record_stream_new(s_fdCommand, MAX_COMMAND_BYTES);

    ril_event_set(&s_commands_event, s_fdCommand, 1,
        processCommandsCallback, p_rs);

    rilEventAddWakeup(&s_commands_event);

    onNewCommandConnect();
}

static void userTimerCallback(int fd, short flags, void* param)
{
    UserCallbackInfo* p_info;

    p_info = (UserCallbackInfo*)param;

    p_info->p_callback(p_info->userParam);

    // FIXME generalize this...there should be a cancel mechanism
    if (s_last_wake_timeout_info != NULL && s_last_wake_timeout_info == p_info) {
        s_last_wake_timeout_info = NULL;
    }

    free(p_info);
}

static void eventLoop(void* param)
{
    ril_event_loop();
    RLOGE("error in event_loop_base errno: %d", errno);
    // kill self to restart on error
    kill(0, SIGKILL);
    return;
}

extern "C" void RIL_startEventLoop(void)
{
    int ret = 0;
    int filedes[2] = { 0 };

    s_fdListen = local_get_control_socket(SOCKET_NAME_RIL);
    if (s_fdListen < 0) {
        RLOGE("Failed to get socket '" SOCKET_NAME_RIL "'");
        exit(-1);
    }
    s_tid_dispatch = pthread_self();

    ret = listen(s_fdListen, 4);

    if (ret < 0) {
        RLOGE("Failed to listen on control socket '%d': %s", s_fdListen, strerror(errno));
        exit(-1);
    }

    ril_event_init();
    ret = pipe(filedes);

    if (ret < 0) {
        RLOGE("Error in pipe() errno: %d", errno);
        return;
    }

    s_fdWakeupRead = filedes[0];
    s_fdWakeupWrite = filedes[1];
    RLOGD("start eventLoop PIPE SUCCESS");

    fcntl(s_fdWakeupRead, F_SETFL, O_NONBLOCK);
    ril_event_set(&s_wakeupfd_event, s_fdWakeupRead, true,
        processWakeupCallback, NULL);

    rilEventAddWakeup(&s_wakeupfd_event);
    ril_event_set(&s_listen_event, s_fdListen, false,
        listenCallback, NULL);
    rilEventAddWakeup(&s_listen_event);
    eventLoop(NULL);
}

extern "C" void RIL_register(const RIL_RadioFunctions* callbacks)
{

    if (callbacks == NULL) {
        RLOGE("RIL_register: RIL_RadioFunctions * null");
        return;
    }
    if (callbacks->version < RIL_VERSION_MIN) {
        RLOGE("RIL_register: version %d is to old, min version is %d",
            callbacks->version, RIL_VERSION_MIN);
        return;
    }
    if (callbacks->version > RIL_VERSION) {
        RLOGE("RIL_register: version %d is too new, max version is %d",
            callbacks->version, RIL_VERSION);
        return;
    }

    RLOGI("RIL_register: RIL version %d", callbacks->version);

    if (s_registerCalled > 0) {
        RLOGE("RIL_register has been called more than once. "
              "Subsequent call ignored");
        return;
    }

    memcpy(&s_callbacks, callbacks, sizeof(RIL_RadioFunctions));

    s_registerCalled = 1;

    RLOGI("s_registerCalled flag set, %d", s_started);

    // Little self-check
    for (int i = 0; i < (int)NUM_ELEMS(s_commands); i++) {
        assert(i == s_commands[i].requestNumber);
    }

    for (int i = 1; i < (int)NUM_ELEMS(s_second_commands); i++) {
        assert(i == (s_second_commands[i].requestNumber - RIL_SECOND_REQUEST_BASE));
    }

    // check ims
    for (int i = 1; i < (int)NUM_ELEMS(s_ims_commands); i++) {
        assert(i == (s_ims_commands[i].requestNumber - RIL_IMS_REQUEST_BASE));
    }

    for (int i = 1; i < (int)NUM_ELEMS(s_cus_commands); i++) {
        assert(i == (s_cus_commands[i].requestNumber - RIL_CUS_REQUEST_BASE));
    }

    for (int i = 0; i < (int)NUM_ELEMS(s_unsolResponses); i++) {
        assert(i + RIL_UNSOL_RESPONSE_BASE
            == s_unsolResponses[i].requestNumber);
    }

    // start listen socket
    RLOGI("RIL_register s_starte %d", s_started);

    if (s_started == 0) {
        RIL_startEventLoop();
    }
}

static int checkAndDequeueRequestInfo(struct RequestInfo* pRI)
{
    int ret = 0;

    if (pRI == NULL) {
        return 0;
    }

    pthread_mutex_lock(&s_pendingRequestsMutex);

    for (RequestInfo** ppCur = &s_pendingRequests; *ppCur != NULL;
         ppCur = &((*ppCur)->p_next)) {
        if (pRI == *ppCur) {
            ret = 1;

            *ppCur = (*ppCur)->p_next;
            break;
        }
    }

    pthread_mutex_unlock(&s_pendingRequestsMutex);

    return ret;
}

extern "C" void RIL_onRequestComplete(RIL_Token t, RIL_Errno e, void* response,
    size_t responselen)
{
    RequestInfo* pRI;
    int ret;
    size_t errorOffset;
    Parcel p;
    pRI = (RequestInfo*)t;

    if (!checkAndDequeueRequestInfo(pRI)) {
        RLOGE("RIL_onRequestComplete: invalid RIL_Token");
        return;
    }

    RLOGD("RequestComplete");

    if (pRI->local > 0) {
        // Locally issued command...void only!
        // response does not go back up the command socket
        RLOGD("C[locl]< %s", requestToString(pRI->pCI->requestNumber));

        goto done;
    }

    if (pRI->cancelled == 0) {
        p.writeInt32(RESPONSE_SOLICITED);
        p.writeInt32(pRI->token);
        errorOffset = p.dataPosition();

        p.writeInt32(e);

        if (response != NULL) {
            // there is a response payload, no matter success or not.
            ret = pRI->pCI->responseFunction(p, response, responselen);

            /* if an error occurred, rewind and mark it */
            if (ret != 0) {
                RLOGE("responseFunction error, ret: %d", ret);
                p.setDataPosition(errorOffset);
                p.writeInt32(ret);
            }
        }

        if (e != RIL_E_SUCCESS) {
            appendPrintBuf("%s fails by %s", printBuf, failCauseToString(e));
        }

        if (s_fdCommand < 0) {
            RLOGD("RIL onRequestComplete: Command channel closed");
        }

        if (sendResponse(p) < 0) {
            RLOGE("failed to send solicited command response");
        }
    }

done:
    free(pRI);
}

static void grabPartialWakeLock()
{
    // acquire_wake_lock(PARTIAL_WAKE_LOCK, ANDROID_WAKE_LOCK_NAME);
}

static void releaseWakeLock()
{
    // release_wake_lock(ANDROID_WAKE_LOCK_NAME);
}

static void wakeTimeoutCallback(void* param)
{
    // We're using "param != NULL" as a cancellation mechanism
    if (param == NULL) {
        releaseWakeLock();
    }
}

static int decodeVoiceRadioTechnology(RIL_RadioState radioState)
{
    switch (radioState) {
    case RADIO_STATE_SIM_NOT_READY:
    case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
    case RADIO_STATE_SIM_READY:
        return RADIO_TECH_UMTS;

    case RADIO_STATE_RUIM_NOT_READY:
    case RADIO_STATE_RUIM_READY:
    case RADIO_STATE_RUIM_LOCKED_OR_ABSENT:
    case RADIO_STATE_NV_NOT_READY:
    case RADIO_STATE_NV_READY:
        return RADIO_TECH_1xRTT;

    default:
        RLOGD("decodeVoiceRadioTechnology: Invoked with incorrect RadioState");
        return -1;
    }
}

static int decodeSimStatus(RIL_RadioState radioState)
{
    switch (radioState) {
    case RADIO_STATE_SIM_NOT_READY:
    case RADIO_STATE_RUIM_NOT_READY:
    case RADIO_STATE_NV_NOT_READY:
    case RADIO_STATE_NV_READY:
        return -1;
    case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
    case RADIO_STATE_SIM_READY:
    case RADIO_STATE_RUIM_READY:
    case RADIO_STATE_RUIM_LOCKED_OR_ABSENT:
        return radioState;
    default:
        RLOGD("decodeSimStatus: Invoked with incorrect RadioState");
        return -1;
    }
}

/* If RIL sends SIM states or RUIM states, store the voice radio
 * technology and subscription source information so that they can be
 * returned when telephony framework requests them
 */
static RIL_RadioState processRadioState(RIL_RadioState newRadioState)
{

    if ((newRadioState > RADIO_STATE_UNAVAILABLE) && (newRadioState < RADIO_STATE_ON)) {
        int newVoiceRadioTech;
        int newSimStatus;

        /* This is old RIL. Decode Subscription source and Voice Radio Technology
         * from Radio State and send change notifications if there has been a change */
        newVoiceRadioTech = decodeVoiceRadioTechnology(newRadioState);
        if (newVoiceRadioTech != voiceRadioTech) {
            voiceRadioTech = newVoiceRadioTech;
            RIL_onUnsolicitedResponse(RIL_UNSOL_VOICE_RADIO_TECH_CHANGED,
                &voiceRadioTech, sizeof(voiceRadioTech));
        }
        newSimStatus = decodeSimStatus(newRadioState);
        if (newSimStatus != simRuimStatus) {
            simRuimStatus = newSimStatus;
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        }

        /* Send RADIO_ON to telephony */
        newRadioState = RADIO_STATE_ON;
    }

    return newRadioState;
}

extern "C" void RIL_onUnsolicitedResponse(int unsolResponse, const void* data,
    size_t datalen)
{
    int unsolResponseIndex;
    int ret;
    int64_t timeReceived = 0;
    bool shouldScheduleTimeout = false;
    RIL_RadioState newState;

    if (s_registerCalled == 0) {
        // Ignore RIL_onUnsolicitedResponse before RIL_register
        RLOGW("RIL_onUnsolicitedResponse called before RIL_register");
        return;
    }

    unsolResponseIndex = unsolResponse - RIL_UNSOL_RESPONSE_BASE;

    if ((unsolResponseIndex < 0)
        || (unsolResponseIndex >= (int32_t)NUM_ELEMS(s_unsolResponses))) {
        RLOGE("unsupported unsolicited response code %d", unsolResponse);
        return;
    }

    // Grab a wake lock if needed for this reponse,
    // as we exit we'll either release it immediately
    // or set a timer to release it later.
    switch (s_unsolResponses[unsolResponseIndex].wakeType) {
    case WAKE_PARTIAL:
        grabPartialWakeLock();
        shouldScheduleTimeout = true;
        break;

    case DONT_WAKE:
    default:
        // No wake lock is grabed so don't set timeout
        shouldScheduleTimeout = false;
        break;
    }

    Parcel p;

    p.writeInt32(RESPONSE_UNSOLICITED);
    p.writeInt32(unsolResponse);

    ret = s_unsolResponses[unsolResponseIndex]
              .responseFunction(p, const_cast<void*>(data), datalen);
    if (ret != 0) {
        // Problem with the response. Don't continue;
        goto error_exit;
    }

    // some things get more payload
    switch (unsolResponse) {
    case RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED:
        newState = processRadioState(s_callbacks.onStateRequest());
        RLOGD("state change");
        p.writeInt32(newState);
        appendPrintBuf("%s {%s}", printBuf,
            radioStateToString(s_callbacks.onStateRequest()));
        break;

    case RIL_UNSOL_NITZ_TIME_RECEIVED:
        // Store the time that this was received so the
        // handler of this message can account for
        // the time it takes to arrive and process. In
        // particular the system has been known to sleep
        // before this message can be processed.
        p.writeInt64(timeReceived);
        break;
    }

    if (s_callbacks.version < 13) {
        if (shouldScheduleTimeout) {
            UserCallbackInfo* p_info = internalRequestTimedCallback(wakeTimeoutCallback, NULL,
                &TIMEVAL_WAKE_TIMEOUT);

            if (p_info == NULL) {
                goto error_exit;
            } else {
                // Cancel the previous request
                if (s_last_wake_timeout_info != NULL) {
                    s_last_wake_timeout_info->userParam = (void*)1;
                }
                s_last_wake_timeout_info = p_info;
            }
        }
    }

#if VDBG
    RLOGI("%s UNSOLICITED: %s length:%d", rilSocketIdToString(soc_id), requestToString(unsolResponse), p.dataSize());
#endif
    ret = sendResponse(p);
    if (ret != 0 && unsolResponse == RIL_UNSOL_NITZ_TIME_RECEIVED) {

        // Unfortunately, NITZ time is not poll/update like everything
        // else in the system. So, if the upstream client isn't connected,
        // keep a copy of the last NITZ response (with receive time noted
        // above) around so we can deliver it when it is connected

        if (s_lastNITZTimeData != NULL) {
            free(s_lastNITZTimeData);
            s_lastNITZTimeData = NULL;
        }

        s_lastNITZTimeData = calloc(p.dataSize(), 1);
        if (s_lastNITZTimeData == NULL) {
            RLOGE("Memory allocation failed in RIL_onUnsolicitedResponse");
            goto error_exit;
        }
        s_lastNITZTimeDataSize = p.dataSize();
        memcpy(s_lastNITZTimeData, p.data(), p.dataSize());
    }

    // Normal exit
    return;

error_exit:
    if (shouldScheduleTimeout) {
        releaseWakeLock();
    }
}

/* FIXME generalize this if you track UserCAllbackInfo, clear it
 * when the callback occurs */
static UserCallbackInfo* internalRequestTimedCallback(RIL_TimedCallback callback,
    void* param, const struct timeval* relativeTime)
{
    struct timeval myRelativeTime;
    UserCallbackInfo* p_info;

    p_info = (UserCallbackInfo*)calloc(1, sizeof(UserCallbackInfo));
    if (p_info == NULL) {
        RLOGE("Memory allocation failed in internalRequestTimedCallback");
        return p_info;
    }

    p_info->p_callback = callback;
    p_info->userParam = param;

    if (relativeTime == NULL) {
        /* treat null parameter as a 0 relative time */
        memset(&myRelativeTime, 0, sizeof(myRelativeTime));
    } else {
        /* FIXME I think event_add's tv param is really const anyway */
        memcpy(&myRelativeTime, relativeTime, sizeof(myRelativeTime));
    }

    ril_event_set(&(p_info->event), -1, false, userTimerCallback, p_info);

    ril_timer_add(&(p_info->event), &myRelativeTime);

    triggerEvLoop();
    return p_info;
}

extern "C" void RIL_requestTimedCallback(RIL_TimedCallback callback, void* param,
    const struct timeval* relativeTime)
{
    internalRequestTimedCallback(callback, param, relativeTime);
}

const char* failCauseToString(RIL_Errno e)
{
    switch (e) {
    case RIL_E_SUCCESS:
        return "E_SUCCESS";
    case RIL_E_RADIO_NOT_AVAILABLE:
        return "E_RADIO_NOT_AVAILABLE";
    case RIL_E_GENERIC_FAILURE:
        return "E_GENERIC_FAILURE";
    case RIL_E_PASSWORD_INCORRECT:
        return "E_PASSWORD_INCORRECT";
    case RIL_E_SIM_PIN2:
        return "E_SIM_PIN2";
    case RIL_E_SIM_PUK2:
        return "E_SIM_PUK2";
    case RIL_E_REQUEST_NOT_SUPPORTED:
        return "E_REQUEST_NOT_SUPPORTED";
    case RIL_E_CANCELLED:
        return "E_CANCELLED";
    case RIL_E_OP_NOT_ALLOWED_DURING_VOICE_CALL:
        return "E_OP_NOT_ALLOWED_DURING_VOICE_CALL";
    case RIL_E_OP_NOT_ALLOWED_BEFORE_REG_TO_NW:
        return "E_OP_NOT_ALLOWED_BEFORE_REG_TO_NW";
    case RIL_E_SMS_SEND_FAIL_RETRY:
        return "E_SMS_SEND_FAIL_RETRY";
    case RIL_E_SIM_ABSENT:
        return "E_SIM_ABSENT";
    case RIL_E_ILLEGAL_SIM_OR_ME:
        return "E_ILLEGAL_SIM_OR_ME";
#ifdef FEATURE_MULTIMODE_ANDROID
    case RIL_E_SUBSCRIPTION_NOT_AVAILABLE:
        return "E_SUBSCRIPTION_NOT_AVAILABLE";
    case RIL_E_MODE_NOT_SUPPORTED:
        return "E_MODE_NOT_SUPPORTED";
#endif
    case RIL_E_FDN_CHECK_FAILURE:
        return "E_FDN_CHECK_FAILURE";
    case RIL_E_MISSING_RESOURCE:
        return "E_MISSING_RESOURCE";
    case RIL_E_NO_SUCH_ELEMENT:
        return "E_NO_SUCH_ELEMENT";
    case RIL_E_DIAL_MODIFIED_TO_USSD:
        return "E_DIAL_MODIFIED_TO_USSD";
    case RIL_E_DIAL_MODIFIED_TO_SS:
        return "E_DIAL_MODIFIED_TO_SS";
    case RIL_E_DIAL_MODIFIED_TO_DIAL:
        return "E_DIAL_MODIFIED_TO_DIAL";
    case RIL_E_USSD_MODIFIED_TO_DIAL:
        return "E_USSD_MODIFIED_TO_DIAL";
    case RIL_E_USSD_MODIFIED_TO_SS:
        return "E_USSD_MODIFIED_TO_SS";
    case RIL_E_USSD_MODIFIED_TO_USSD:
        return "E_USSD_MODIFIED_TO_USSD";
    case RIL_E_SS_MODIFIED_TO_DIAL:
        return "E_SS_MODIFIED_TO_DIAL";
    case RIL_E_SS_MODIFIED_TO_USSD:
        return "E_SS_MODIFIED_TO_USSD";
    case RIL_E_SUBSCRIPTION_NOT_SUPPORTED:
        return "E_SUBSCRIPTION_NOT_SUPPORTED";
    case RIL_E_SS_MODIFIED_TO_SS:
        return "E_SS_MODIFIED_TO_SS";
    case RIL_E_LCE_NOT_SUPPORTED:
        return "E_LCE_NOT_SUPPORTED";
    case RIL_E_NO_MEMORY:
        return "E_NO_MEMORY";
    case RIL_E_INTERNAL_ERR:
        return "E_INTERNAL_ERR";
    case RIL_E_SYSTEM_ERR:
        return "E_SYSTEM_ERR";
    case RIL_E_MODEM_ERR:
        return "E_MODEM_ERR";
    case RIL_E_INVALID_STATE:
        return "E_INVALID_STATE";
    case RIL_E_NO_RESOURCES:
        return "E_NO_RESOURCES";
    case RIL_E_SIM_ERR:
        return "E_SIM_ERR";
    case RIL_E_INVALID_ARGUMENTS:
        return "E_INVALID_ARGUMENTS";
    case RIL_E_INVALID_SIM_STATE:
        return "E_INVALID_SIM_STATE";
    case RIL_E_INVALID_MODEM_STATE:
        return "E_INVALID_MODEM_STATE";
    case RIL_E_INVALID_CALL_ID:
        return "E_INVALID_CALL_ID";
    case RIL_E_NO_SMS_TO_ACK:
        return "E_NO_SMS_TO_ACK";
    case RIL_E_NETWORK_ERR:
        return "E_NETWORK_ERR";
    case RIL_E_REQUEST_RATE_LIMITED:
        return "E_REQUEST_RATE_LIMITED";
    case RIL_E_SIM_BUSY:
        return "E_SIM_BUSY";
    case RIL_E_SIM_FULL:
        return "E_SIM_FULL";
    case RIL_E_NETWORK_REJECT:
        return "E_NETWORK_REJECT";
    case RIL_E_OPERATION_NOT_ALLOWED:
        return "E_OPERATION_NOT_ALLOWED";
    // case RIL_E_EMPTY_RECORD: "E_EMPTY_RECORD";
    case RIL_E_INVALID_SMS_FORMAT:
        return "E_INVALID_SMS_FORMAT";
    case RIL_E_ENCODING_ERR:
        return "E_ENCODING_ERR";
    case RIL_E_INVALID_SMSC_ADDRESS:
        return "E_INVALID_SMSC_ADDRESS";
    case RIL_E_NO_SUCH_ENTRY:
        return "E_NO_SUCH_ENTRY";
    case RIL_E_NETWORK_NOT_READY:
        return "E_NETWORK_NOT_READY";
    case RIL_E_NOT_PROVISIONED:
        return "E_NOT_PROVISIONED";
    case RIL_E_NO_SUBSCRIPTION:
        return "E_NO_SUBSCRIPTION";
    case RIL_E_NO_NETWORK_FOUND:
        return "E_NO_NETWORK_FOUND";
    case RIL_E_DEVICE_IN_USE:
        return "E_DEVICE_IN_USE";
    case RIL_E_ABORTED:
        return "E_ABORTED";
    case RIL_E_OEM_ERROR_1:
        return "E_OEM_ERROR_1";
    case RIL_E_OEM_ERROR_2:
        return "E_OEM_ERROR_2";
    case RIL_E_OEM_ERROR_3:
        return "E_OEM_ERROR_3";
    case RIL_E_OEM_ERROR_4:
        return "E_OEM_ERROR_4";
    case RIL_E_OEM_ERROR_5:
        return "E_OEM_ERROR_5";
    case RIL_E_OEM_ERROR_6:
        return "E_OEM_ERROR_6";
    case RIL_E_OEM_ERROR_7:
        return "E_OEM_ERROR_7";
    case RIL_E_OEM_ERROR_8:
        return "E_OEM_ERROR_8";
    case RIL_E_OEM_ERROR_9:
        return "E_OEM_ERROR_9";
    case RIL_E_OEM_ERROR_10:
        return "E_OEM_ERROR_10";
    case RIL_E_OEM_ERROR_11:
        return "E_OEM_ERROR_11";
    case RIL_E_OEM_ERROR_12:
        return "E_OEM_ERROR_12";
    case RIL_E_OEM_ERROR_13:
        return "E_OEM_ERROR_13";
    case RIL_E_OEM_ERROR_14:
        return "E_OEM_ERROR_14";
    case RIL_E_OEM_ERROR_15:
        return "E_OEM_ERROR_15";
    case RIL_E_OEM_ERROR_16:
        return "E_OEM_ERROR_16";
    case RIL_E_OEM_ERROR_17:
        return "E_OEM_ERROR_17";
    case RIL_E_OEM_ERROR_18:
        return "E_OEM_ERROR_18";
    case RIL_E_OEM_ERROR_19:
        return "E_OEM_ERROR_19";
    case RIL_E_OEM_ERROR_20:
        return "E_OEM_ERROR_20";
    case RIL_E_OEM_ERROR_21:
        return "E_OEM_ERROR_21";
    case RIL_E_OEM_ERROR_22:
        return "E_OEM_ERROR_22";
    case RIL_E_OEM_ERROR_23:
        return "E_OEM_ERROR_23";
    case RIL_E_OEM_ERROR_24:
        return "E_OEM_ERROR_24";
    case RIL_E_OEM_ERROR_25:
        return "E_OEM_ERROR_25";
    default:
        return "<unknown error>";
    }
}

const char* radioStateToString(RIL_RadioState s)
{
    switch (s) {
    case RADIO_STATE_OFF:
        return "RADIO_OFF";
    case RADIO_STATE_UNAVAILABLE:
        return "RADIO_UNAVAILABLE";
    case RADIO_STATE_SIM_NOT_READY:
        return "RADIO_SIM_NOT_READY";
    case RADIO_STATE_SIM_LOCKED_OR_ABSENT:
        return "RADIO_SIM_LOCKED_OR_ABSENT";
    case RADIO_STATE_SIM_READY:
        return "RADIO_SIM_READY";
    case RADIO_STATE_RUIM_NOT_READY:
        return "RADIO_RUIM_NOT_READY";
    case RADIO_STATE_RUIM_READY:
        return "RADIO_RUIM_READY";
    case RADIO_STATE_RUIM_LOCKED_OR_ABSENT:
        return "RADIO_RUIM_LOCKED_OR_ABSENT";
    case RADIO_STATE_NV_NOT_READY:
        return "RADIO_NV_NOT_READY";
    case RADIO_STATE_NV_READY:
        return "RADIO_NV_READY";
    case RADIO_STATE_ON:
        return "RADIO_ON";
    default:
        return "<unknown state>";
    }
}

const char* callStateToString(RIL_CallState s)
{
    switch (s) {
    case RIL_CALL_ACTIVE:
        return "ACTIVE";
    case RIL_CALL_HOLDING:
        return "HOLDING";
    case RIL_CALL_DIALING:
        return "DIALING";
    case RIL_CALL_ALERTING:
        return "ALERTING";
    case RIL_CALL_INCOMING:
        return "INCOMING";
    case RIL_CALL_WAITING:
        return "WAITING";
    default:
        return "<unknown state>";
    }
}

extern "C" const char* requestToString(int request)
{
    /*
     cat libs/telephony/ril_commands.h \
     | egrep "^ *{RIL_" \
     | sed -re 's/\{RIL_([^,]+),[^,]+,([^}]+).+/case RIL_\1: return "\1";/'


     cat libs/telephony/ril_unsol_commands.h \
     | egrep "^ *{RIL_" \
     | sed -re 's/\{RIL_([^,]+),([^}]+).+/case RIL_\1: return "\1";/'

    */
    switch (request) {
    case RIL_REQUEST_GET_SIM_STATUS:
        return "GET_SIM_STATUS";
    case RIL_REQUEST_ENTER_SIM_PIN:
        return "ENTER_SIM_PIN";
    case RIL_REQUEST_ENTER_SIM_PUK:
        return "ENTER_SIM_PUK";
    case RIL_REQUEST_ENTER_SIM_PIN2:
        return "ENTER_SIM_PIN2";
    case RIL_REQUEST_ENTER_SIM_PUK2:
        return "ENTER_SIM_PUK2";
    case RIL_REQUEST_CHANGE_SIM_PIN:
        return "CHANGE_SIM_PIN";
    case RIL_REQUEST_CHANGE_SIM_PIN2:
        return "CHANGE_SIM_PIN2";
    case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION:
        return "ENTER_NETWORK_DEPERSONALIZATION";
    case RIL_REQUEST_GET_CURRENT_CALLS:
        return "GET_CURRENT_CALLS";
    case RIL_REQUEST_DIAL:
        return "DIAL";
    case RIL_REQUEST_GET_IMSI:
        return "GET_IMSI";
    case RIL_REQUEST_HANGUP:
        return "HANGUP";
    case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
        return "HANGUP_WAITING_OR_BACKGROUND";
    case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
        return "HANGUP_FOREGROUND_RESUME_BACKGROUND";
    case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
        return "SWITCH_WAITING_OR_HOLDING_AND_ACTIVE";
    case RIL_REQUEST_CONFERENCE:
        return "CONFERENCE";
    case RIL_REQUEST_UDUB:
        return "UDUB";
    case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
        return "LAST_CALL_FAIL_CAUSE";
    case RIL_REQUEST_SIGNAL_STRENGTH:
        return "SIGNAL_STRENGTH";
    case RIL_REQUEST_VOICE_REGISTRATION_STATE:
        return "VOICE_REGISTRATION_STATE";
    case RIL_REQUEST_DATA_REGISTRATION_STATE:
        return "DATA_REGISTRATION_STATE";
    case RIL_REQUEST_OPERATOR:
        return "OPERATOR";
    case RIL_REQUEST_RADIO_POWER:
        return "RADIO_POWER";
    case RIL_REQUEST_DTMF:
        return "DTMF";
    case RIL_REQUEST_SEND_SMS:
        return "SEND_SMS";
    case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
        return "SEND_SMS_EXPECT_MORE";
    case RIL_REQUEST_SETUP_DATA_CALL:
        return "SETUP_DATA_CALL";
    case RIL_REQUEST_SIM_IO:
        return "SIM_IO";
    case RIL_REQUEST_SEND_USSD:
        return "SEND_USSD";
    case RIL_REQUEST_CANCEL_USSD:
        return "CANCEL_USSD";
    case RIL_REQUEST_GET_CLIR:
        return "GET_CLIR";
    case RIL_REQUEST_SET_CLIR:
        return "SET_CLIR";
    case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS:
        return "QUERY_CALL_FORWARD_STATUS";
    case RIL_REQUEST_SET_CALL_FORWARD:
        return "SET_CALL_FORWARD";
    case RIL_REQUEST_QUERY_CALL_WAITING:
        return "QUERY_CALL_WAITING";
    case RIL_REQUEST_SET_CALL_WAITING:
        return "SET_CALL_WAITING";
    case RIL_REQUEST_SMS_ACKNOWLEDGE:
        return "SMS_ACKNOWLEDGE";
    case RIL_REQUEST_GET_IMEI:
        return "GET_IMEI";
    case RIL_REQUEST_GET_IMEISV:
        return "GET_IMEISV";
    case RIL_REQUEST_ANSWER:
        return "ANSWER";
    case RIL_REQUEST_DEACTIVATE_DATA_CALL:
        return "DEACTIVATE_DATA_CALL";
    case RIL_REQUEST_QUERY_FACILITY_LOCK:
        return "QUERY_FACILITY_LOCK";
    case RIL_REQUEST_SET_FACILITY_LOCK:
        return "SET_FACILITY_LOCK";
    case RIL_REQUEST_CHANGE_BARRING_PASSWORD:
        return "CHANGE_BARRING_PASSWORD";
    case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
        return "QUERY_NETWORK_SELECTION_MODE";
    case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
        return "SET_NETWORK_SELECTION_AUTOMATIC";
    case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
        return "SET_NETWORK_SELECTION_MANUAL";
    case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS:
        return "QUERY_AVAILABLE_NETWORKS ";
    case RIL_REQUEST_DTMF_START:
        return "DTMF_START";
    case RIL_REQUEST_DTMF_STOP:
        return "DTMF_STOP";
    case RIL_REQUEST_BASEBAND_VERSION:
        return "BASEBAND_VERSION";
    case RIL_REQUEST_SEPARATE_CONNECTION:
        return "SEPARATE_CONNECTION";
    case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
        return "SET_PREFERRED_NETWORK_TYPE";
    case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
        return "GET_PREFERRED_NETWORK_TYPE";
    case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
        return "GET_NEIGHBORING_CELL_IDS";
    case RIL_REQUEST_SET_MUTE:
        return "SET_MUTE";
    case RIL_REQUEST_GET_MUTE:
        return "GET_MUTE";
    case RIL_REQUEST_QUERY_CLIP:
        return "QUERY_CLIP";
    case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE:
        return "LAST_DATA_CALL_FAIL_CAUSE";
    case RIL_REQUEST_DATA_CALL_LIST:
        return "DATA_CALL_LIST";
    case RIL_REQUEST_RESET_RADIO:
        return "RESET_RADIO";
    case RIL_REQUEST_OEM_HOOK_RAW:
        return "OEM_HOOK_RAW";
    case RIL_REQUEST_OEM_HOOK_STRINGS:
        return "OEM_HOOK_STRINGS";
    case RIL_REQUEST_SET_BAND_MODE:
        return "SET_BAND_MODE";
    case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
        return "QUERY_AVAILABLE_BAND_MODE";
    case RIL_REQUEST_STK_GET_PROFILE:
        return "STK_GET_PROFILE";
    case RIL_REQUEST_STK_SET_PROFILE:
        return "STK_SET_PROFILE";
    case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND:
        return "STK_SEND_ENVELOPE_COMMAND";
    case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE:
        return "STK_SEND_TERMINAL_RESPONSE";
    case RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM:
        return "STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM";
    case RIL_REQUEST_SCREEN_STATE:
        return "SCREEN_STATE";
    case RIL_REQUEST_EXPLICIT_CALL_TRANSFER:
        return "EXPLICIT_CALL_TRANSFER";
    case RIL_REQUEST_SET_LOCATION_UPDATES:
        return "SET_LOCATION_UPDATES";
    case RIL_REQUEST_SET_TTY_MODE:
        return "SET_TTY_MODE";
    case RIL_REQUEST_QUERY_TTY_MODE:
        return "QUERY_TTY_MODE";
    case RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG:
        return "GSM_GET_BROADCAST_SMS_CONFIG";
    case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:
        return "GSM_SET_BROADCAST_SMS_CONFIG";
    case RIL_REQUEST_DEVICE_IDENTITY:
        return "DEVICE_IDENTITY";
    case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE:
        return "EXIT_EMERGENCY_CALLBACK_MODE";
    case RIL_REQUEST_GET_SMSC_ADDRESS:
        return "GET_SMSC_ADDRESS";
    case RIL_REQUEST_SET_SMSC_ADDRESS:
        return "SET_SMSC_ADDRESS";
    case RIL_REQUEST_REPORT_SMS_MEMORY_STATUS:
        return "REPORT_SMS_MEMORY_STATUS";
    case RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING:
        return "REPORT_STK_SERVICE_IS_RUNNING";
    case RIL_REQUEST_ISIM_AUTHENTICATION:
        return "ISIM_AUTHENTICATION";
    case RIL_REQUEST_ACKNOWLEDGE_INCOMING_GSM_SMS_WITH_PDU:
        return "RIL_REQUEST_ACKNOWLEDGE_INCOMING_GSM_SMS_WITH_PDU";
    case RIL_REQUEST_STK_SEND_ENVELOPE_WITH_STATUS:
        return "RIL_REQUEST_STK_SEND_ENVELOPE_WITH_STATUS";
    case RIL_REQUEST_VOICE_RADIO_TECH:
        return "VOICE_RADIO_TECH";
    case RIL_REQUEST_WRITE_SMS_TO_SIM:
        return "WRITE_SMS_TO_SIM";
    case RIL_REQUEST_GET_CELL_INFO_LIST:
        return "GET_CELL_INFO_LIST";
    case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
        return "SET_UNSOL_CELL_INFO_LIST_RATE";
    case RIL_REQUEST_SET_INITIAL_ATTACH_APN:
        return "RIL_REQUEST_SET_INITIAL_ATTACH_APN";
    case RIL_REQUEST_IMS_REGISTRATION_STATE:
        return "IMS_REGISTRATION_STATE";
    case RIL_REQUEST_IMS_SEND_SMS:
        return "IMS_SEND_SMS";
    case RIL_REQUEST_SIM_TRANSMIT_APDU_BASIC:
        return "SIM_TRANSMIT_APDU_BASIC";
    case RIL_REQUEST_SIM_OPEN_CHANNEL:
        return "SIM_OPEN_CHANNEL";
    case RIL_REQUEST_SIM_CLOSE_CHANNEL:
        return "SIM_CLOSE_CHANNEL";
    case RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL:
        return "SIM_TRANSMIT_APDU_CHANNEL";
    case RIL_REQUEST_SET_DATA_PROFILE:
        return "SET_DATA_PROFILE";
    case RIL_REQUEST_GET_ACTIVITY_INFO:
        return "RIL_REQUEST_GET_ACTIVITY_INFO";
    case RIL_REQUEST_GET_MODEM_STATUS:
        return "GET_MODEM_STATUS";
    case RIL_REQUEST_EMERGENCY_DIAL:
        return "EMERGENCY_DIAL";
    case RIL_REQUEST_ENABLE_MODEM:
        return "RIL_REQUEST_ENABLE_MODEM";
    case RIL_REQUEST_IMS_REG_STATE_CHANGE:
        return "IMS_REG_STATE_CHANGE";
    case RIL_REQUEST_IMS_SET_SERVICE_STATUS:
        return "IMS_SET_SERVICE_STATUS";
    case RIL_REQUEST_DIAL_CONFERENCE:
        return "DIAL_CONFERENCE";
    case RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED:
        return "UNSOL_RESPONSE_RADIO_STATE_CHANGED";
    case RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED:
        return "UNSOL_RESPONSE_CALL_STATE_CHANGED";
    case RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED:
        return "UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED";
    case RIL_UNSOL_RESPONSE_NEW_SMS:
        return "UNSOL_RESPONSE_NEW_SMS";
    case RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT:
        return "UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT";
    case RIL_UNSOL_RESPONSE_NEW_SMS_ON_SIM:
        return "UNSOL_RESPONSE_NEW_SMS_ON_SIM";
    case RIL_UNSOL_ON_USSD:
        return "UNSOL_ON_USSD";
    case RIL_UNSOL_ON_USSD_REQUEST:
        return "UNSOL_ON_USSD_REQUEST(obsolete)";
    case RIL_UNSOL_NITZ_TIME_RECEIVED:
        return "UNSOL_NITZ_TIME_RECEIVED";
    case RIL_UNSOL_SIGNAL_STRENGTH:
        return "UNSOL_SIGNAL_STRENGTH";
    case RIL_UNSOL_SUPP_SVC_NOTIFICATION:
        return "UNSOL_SUPP_SVC_NOTIFICATION";
    case RIL_UNSOL_STK_SESSION_END:
        return "UNSOL_STK_SESSION_END";
    case RIL_UNSOL_STK_PROACTIVE_COMMAND:
        return "UNSOL_STK_PROACTIVE_COMMAND";
    case RIL_UNSOL_STK_EVENT_NOTIFY:
        return "UNSOL_STK_EVENT_NOTIFY";
    case RIL_UNSOL_STK_CALL_SETUP:
        return "UNSOL_STK_CALL_SETUP";
    case RIL_UNSOL_SIM_SMS_STORAGE_FULL:
        return "UNSOL_SIM_SMS_STORAGE_FUL";
    case RIL_UNSOL_SIM_REFRESH:
        return "UNSOL_SIM_REFRESH";
    case RIL_UNSOL_DATA_CALL_LIST_CHANGED:
        return "UNSOL_DATA_CALL_LIST_CHANGED";
    case RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED:
        return "UNSOL_RESPONSE_SIM_STATUS_CHANGED";
    case RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS:
        return "UNSOL_NEW_BROADCAST_SMS";
    case RIL_UNSOL_RESTRICTED_STATE_CHANGED:
        return "UNSOL_RESTRICTED_STATE_CHANGED";
    case RIL_UNSOL_ENTER_EMERGENCY_CALLBACK_MODE:
        return "UNSOL_ENTER_EMERGENCY_CALLBACK_MODE";
    case RIL_UNSOL_OEM_HOOK_RAW:
        return "UNSOL_OEM_HOOK_RAW";
    case RIL_UNSOL_RINGBACK_TONE:
        return "UNSOL_RINGBACK_TONE";
    case RIL_UNSOL_RESEND_INCALL_MUTE:
        return "UNSOL_RESEND_INCALL_MUTE";
    case RIL_UNSOL_EXIT_EMERGENCY_CALLBACK_MODE:
        return "UNSOL_EXIT_EMERGENCY_CALLBACK_MODE";
    case RIL_UNSOL_RIL_CONNECTED:
        return "UNSOL_RIL_CONNECTED";
    case RIL_UNSOL_VOICE_RADIO_TECH_CHANGED:
        return "UNSOL_VOICE_RADIO_TECH_CHANGED";
    case RIL_UNSOL_CELL_INFO_LIST:
        return "UNSOL_CELL_INFO_LIST";
    case RIL_UNSOL_RESPONSE_IMS_NETWORK_STATE_CHANGED:
        return "RESPONSE_IMS_NETWORK_STATE_CHANGED";
    case RIL_UNSOL_MODEM_RESTART:
        return "RIL_UNSOL_MODEM_RESTART";
    default:
        return "<unknown request>";
    }
}
