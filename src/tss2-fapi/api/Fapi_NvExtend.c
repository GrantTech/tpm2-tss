/* SPDX-License-Identifier: BSD-2-Clause */
/*******************************************************************************
 * Copyright 2018-2019, Fraunhofer SIT sponsored by Infineon Technologies AG
 * All rights reserved.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "tss2_fapi.h"
#include "ifapi_json_serialize.h"
#include "fapi_crypto.h"
#include "fapi_int.h"
#include "fapi_util.h"
#include "tss2_esys.h"
#define LOGMODULE fapi
#include "util/log.h"
#include "util/aux_util.h"

/** One-Call function for Fapi_NvExtend
 *
 * Performs an extend operation on an NV index with the type extend.
 *
 * @param [in, out] context the FAPI context
 * @param [in] nvPath The path to the NV index that is extended
 * @param [in] data The data to extend on the NV index
 * @param [in] dataSize The size of the data to extend. Must be smaller than
 *             1024
 * @param [in] logData A JSON representation of the data that is written to the
 *         event log. May be NULL
 *
 * @retval TSS2_RC_SUCCESS: if the function call was a success.
 * @retval TSS2_FAPI_RC_BAD_REFERENCE: if context, nvPath, or data is NULL.
 * @retval TSS2_FAPI_RC_BAD_CONTEXT: if context corruption is detected.
 * @retval TSS2_FAPI_RC_BAD_PATH: if nvPath is not found.
 * @retval TSS2_FAPI_RC_NV_WRONG_TYPE: if the NV is not an extendable index.
 * @retval TSS2_FAPI_RC_POLICY_UNKNOWN: if the policy is unknown.
 * @retval TSS2_FAPI_RC_BAD_SEQUENCE: if the context has an asynchronous
 *         operation already pending.
 * @retval TSS2_FAPI_RC_IO_ERROR: if the data cannot be saved.
 * @retval TSS2_FAPI_RC_MEMORY: if the FAPI cannot allocate enough memory for
 *         internal operations or return parameters.
 */
TSS2_RC
Fapi_NvExtend(
    FAPI_CONTEXT  *context,
    char    const *nvPath,
    uint8_t const *data,
    size_t         dataSize,
    char    const *logData)
{
    LOG_TRACE("called for context:%p", context);

    TSS2_RC r, r2;

    /* Check for NULL parameters */
    check_not_null(context);
    check_not_null(nvPath);
    check_not_null(data);

    /* Check whether TCTI and ESYS are initialized */
    return_if_null(context->esys, "Command can't be executed in none TPM mode.",
                   TSS2_FAPI_RC_NO_TPM);

    /* If the async state automata of FAPI shall be tested, then we must not set
       the timeouts of ESYS to blocking mode.
       During testing, the mssim tcti will ensure multiple re-invocations.
       Usually however the synchronous invocations of FAPI shall instruct ESYS
       to block until a result is available. */
#ifndef TEST_FAPI_ASYNC
    r = Esys_SetTimeout(context->esys, TSS2_TCTI_TIMEOUT_BLOCK);
    return_if_error_reset_state(r, "Set Timeout to blocking");
#endif /* TEST_FAPI_ASYNC */

    r = Fapi_NvExtend_Async(context, nvPath, data, dataSize, logData);
    return_if_error_reset_state(r, "NV_Extend");

    do {
        /* We wait for file I/O to be ready if the FAPI state automata
           are in a file I/O state. */
        r = ifapi_io_poll(&context->io);
        return_if_error(r, "Something went wrong with IO polling");

        /* Repeatedly call the finish function, until FAPI has transitioned
           through all execution stages / states of this invocation. */
        r = Fapi_NvExtend_Finish(context);
    } while ((r & ~TSS2_RC_LAYER_MASK) == TSS2_BASE_RC_TRY_AGAIN);

    /* Reset the ESYS timeout to non-blocking, immediate response. */
    r2 = Esys_SetTimeout(context->esys, 0);
    return_if_error(r2, "Set Timeout to non-blocking");

    return_if_error_reset_state(r, "NV_Extend");

    LOG_TRACE("finsihed");
    return TSS2_RC_SUCCESS;
}

/** Asynchronous function for Fapi_NvExtend
 *
 * Performs an extend operation on an NV index with the type extend.
 *
 * Call Fapi_NvExtend_Finish to finish the execution of this command.
 *
 * @param [in, out] context the FAPI context
 * @param [in] nvPath The path to the NV index that is extended
 * @param [in] data The data to extend on the NV index
 * @param [in] dataSize The size of the data to extend. Must be smaller than
 *             1024
 * @param [in] logData A JSON representation of the data that is written to the
 *         event log. May be NULL
 *
 * @retval TSS2_RC_SUCCESS: if the function call was a success.
 * @retval TSS2_FAPI_RC_BAD_REFERENCE: if context, nvPath, or data is NULL.
 * @retval TSS2_FAPI_RC_BAD_CONTEXT: if context corruption is detected.
 * @retval TSS2_FAPI_RC_BAD_PATH: if nvPath is not found.
 * @retval TSS2_FAPI_RC_NV_WRONG_TYPE: if the NV is not an extendable index.
 * @retval TSS2_FAPI_RC_POLICY_UNKNOWN: if the policy is unknown.
 * @retval TSS2_FAPI_RC_BAD_SEQUENCE: if the context has an asynchronous
 *         operation already pending.
 * @retval TSS2_FAPI_RC_IO_ERROR: if the data cannot be saved.
 * @retval TSS2_FAPI_RC_MEMORY: if the FAPI cannot allocate enough memory for
 *         internal operations or return parameters.
 */
TSS2_RC
Fapi_NvExtend_Async(
    FAPI_CONTEXT  *context,
    char    const *nvPath,
    uint8_t const *data,
    size_t         dataSize,
    char    const *logData)
{
    LOG_TRACE("called for context:%p", context);
    LOG_TRACE("nvPath: %s", nvPath);
    if (data) {
        LOGBLOB_TRACE(data, dataSize, "data");
    } else {
        LOG_TRACE("data: (null) dataSize: %zi", dataSize);
    }
    LOG_TRACE("logData: %s", logData);

    TSS2_RC r;

    /* Check for NULL parameters */
    check_not_null(context);
    check_not_null(nvPath);
    check_not_null(data);

    /* Helpful alias pointers */
    IFAPI_NV_Cmds * command = &context->nv_cmd;

    memset(command, 0 ,sizeof(IFAPI_NV_Cmds));
    command->offset = 0;
    command->data = malloc(dataSize);
    goto_if_null2(command->data, "Out of memory", r, TSS2_FAPI_RC_MEMORY,
            error_cleanup);
    strdup_check(command->nvPath, nvPath, r, error_cleanup);
    strdup_check(command->logData, logData, r, error_cleanup);

    command->numBytes = dataSize;
    if (context->state == _FAPI_STATE_INIT)
        ifapi_session_init(context);

    r = ifapi_keystore_load_async(&context->keystore, &context->io, command->nvPath);
    goto_if_error2(r, "Could not open: %s", error_cleanup, command->nvPath);

    context->state = NV_EXTEND_READ;
    LOG_TRACE("finsihed");
    return r;
error_cleanup:
    SAFE_FREE(command->data);
    SAFE_FREE(command->nvPath);
    SAFE_FREE(command->logData);
    return r;
}

/** Asynchronous finish function for Fapi_NvExtend
 *
 * This function should be called after a previous Fapi_NvExtend.
 *
 * @param [in, out] context The FAPI_CONTEXT
 *
 * @retval TSS2_RC_SUCCESS: if the function call was a success.
 * @retval TSS2_FAPI_RC_BAD_REFERENCE: if context is NULL.
 * @retval TSS2_FAPI_RC_BAD_CONTEXT: if context corruption is detected.
 * @retval TSS2_FAPI_RC_BAD_SEQUENCE: if the context has an asynchronous
 *         operation already pending.
 * @retval TSS2_FAPI_RC_IO_ERROR: if the data cannot be saved.
 * @retval TSS2_FAPI_RC_MEMORY: if the FAPI cannot allocate enough memory for
 *         internal operations or return parameters.
 * @retval TSS2_FAPI_RC_TRY_AGAIN: if the asynchronous operation is not yet
 *         complete. Call this function again later.
 */
TSS2_RC
Fapi_NvExtend_Finish(
    FAPI_CONTEXT  *context)
{
    LOG_TRACE("called for context:%p", context);

    TSS2_RC r;
    ESYS_TR authIndex;
    json_object *jso = NULL;
    IFAPI_CRYPTO_CONTEXT_BLOB *cryptoContext = NULL;
    TPMI_ALG_HASH hashAlg;
    size_t hashSize;
    ESYS_TR auth_session;

    /* Check for NULL parameters */
    check_not_null(context);

    /* Helpful alias pointers */
    IFAPI_NV_Cmds * command = &context->nv_cmd;
    TPM2B_MAX_NV_BUFFER *auxData = (TPM2B_MAX_NV_BUFFER *)&context->aux_data;
    size_t dataIdx = command->data_idx;
    ESYS_TR nvIndex =  command->esys_handle;
    const uint8_t *data = command->data;
    IFAPI_OBJECT *object = &command->nv_object;
    IFAPI_OBJECT *authObject = &command->auth_object;
    IFAPI_EVENT *event = &command->pcr_event;

    switch (context->state) {
    statecase(context->state, NV_EXTEND_READ)
        /* First check whether the file in object store can be updated. */
        r = ifapi_keystore_check_writeable(&context->keystore, &context->io, command->nvPath);
        goto_if_error_reset_state(r, "Check whether update object store is possible.", error_cleanup);

        r = ifapi_keystore_load_finish(&context->keystore, &context->io, object);
        return_try_again(r);
        return_if_error_reset_state(r, "read_finish failed");

        if (object->objectType != IFAPI_NV_OBJ)
            goto_error(r, TSS2_FAPI_RC_BAD_PATH, "%s is no NV object.", error_cleanup,
                       command->nvPath);

        r = ifapi_initialize_object(context->esys, object);
        goto_if_error_reset_state(r, "Initialize NV object", error_cleanup);

        /* Store object info in context */
        nvIndex = command->nv_object.handle;
        command->esys_handle =  context->nv_cmd.nv_object.handle;
        command->nv_obj = object->misc.nv;

        if (object->misc.nv.public.nvPublic.attributes & TPMA_NV_PPWRITE) {
            ifapi_init_hierarchy_object(authObject, ESYS_TR_RH_PLATFORM);
            authIndex = ESYS_TR_RH_PLATFORM;
        } else {
            if (object->misc.nv.public.nvPublic.attributes & TPMA_NV_OWNERWRITE) {
                ifapi_init_hierarchy_object(authObject, ESYS_TR_RH_OWNER);
                authIndex = ESYS_TR_RH_OWNER;
            } else {
                authIndex = nvIndex;
            }
            *authObject = *object;
        }
        command->auth_index = authIndex;
        context->primary_state = PRIMARY_INIT;
        r = ifapi_get_sessions_async(context,
                                     IFAPI_SESSION_GENEK | IFAPI_SESSION1,
                                     TPMA_SESSION_DECRYPT, 0);
        goto_if_error_reset_state(r, "Create sessions", error_cleanup);


        context->state = NV_EXTEND_WAIT_FOR_SESSION;
        return TSS2_FAPI_RC_TRY_AGAIN;

    statecase(context->state, NV_EXTEND_WAIT_FOR_SESSION)
//TODO: Pass the namealg of the NV index into the session to be created
        r = ifapi_get_sessions_finish(context, &context->profiles.default_profile);
        return_try_again(r);

        goto_if_error_reset_state(r, " FAPI create session", error_cleanup);

        if (command->numBytes > context->nv_buffer_max)
            auxData->size = context->nv_buffer_max;
        else
            auxData->size = command->numBytes;
        memcpy(&auxData->buffer[0], &data[0], auxData->size);
        command->data_idx = auxData->size;

        /* Authorization needed if NO_DA is not  set */
        if (!(object->misc.nv.public.nvPublic.attributes & TPMA_NV_NO_DA)) {
            r = ifapi_set_auth(context, authObject, "NV Extend");
            goto_if_error_reset_state(r, "Fapi_NV_UndefineSpace", error_cleanup);
        }
        context->state = NV_EXTEND_AUTHORIZE;
        fallthrough;

    statecase(context->state, NV_EXTEND_AUTHORIZE)
        r = ifapi_authorize_object(context, authObject, &auth_session);
        return_try_again(r);
        goto_if_error(r, "Authorize NV object.", error_cleanup);

        r = Esys_NV_Extend_Async(context->esys,
                                 command->auth_index,
                                 nvIndex,
                                 auth_session,
                                 ESYS_TR_NONE,
                                 ESYS_TR_NONE,
                                 auxData);
        goto_if_error_reset_state(r, " Fapi_NvExtend_Async", error_cleanup);

        command->bytesRequested = auxData->size;
        command->data = (uint8_t *)data;

        context->state = NV_EXTEND_AUTH_SENT;
        return TSS2_FAPI_RC_TRY_AGAIN;

    statecase(context->state, NV_EXTEND_AUTH_SENT)

        r = Esys_NV_Extend_Finish(context->esys);
        return_try_again(r);

        goto_if_error_reset_state(r, "FAPI NV_Extend_Finish", error_cleanup);

        command->numBytes -= context->nv_cmd.bytesRequested;

        if (command->numBytes > 0) {
            if (command->numBytes > context->nv_buffer_max)
                auxData->size = context->nv_buffer_max;
            else
                auxData->size = command->numBytes;
            memcpy(&auxData->buffer[0], &data[dataIdx], auxData->size);
            r = Esys_NV_Extend_Async(context->esys,
                                     command->auth_index,
                                     nvIndex,
                                     context->session1,
                                     ESYS_TR_NONE,
                                     ESYS_TR_NONE,
                                     auxData);
            goto_if_error_reset_state(r, "FAPI NV_Extend", error_cleanup);

            command->bytesRequested = auxData->size;
            return TSS2_FAPI_RC_TRY_AGAIN;
        }

        /* Compute Digest of the current event */
        hashAlg = object->misc.nv.public.nvPublic.nameAlg;
        r = ifapi_crypto_hash_start(&cryptoContext, hashAlg);
        return_if_error(r, "crypto hash start");

        HASH_UPDATE_BUFFER(cryptoContext,
                           &auxData->buffer[0], auxData->size,
                           r, error_cleanup);

        r = ifapi_crypto_hash_finish(&cryptoContext,
                                     (uint8_t *)
                                     &event->digests.digests[0].digest,
                                     &hashSize);
        return_if_error(r, "crypto hash finish");

        event->digests.digests[0].hashAlg = hashAlg;
        event->digests.count = 1;
        event->pcr = object->misc.nv.public.nvPublic.nvIndex;
        event->type = IFAPI_TSS_EVENT_TAG;
        memcpy(&event->sub_event.tss_event.data.buffer[0],
               &auxData->buffer[0], auxData->size);
        event->sub_event.tss_event.data.size = auxData->size;
        if (command->logData) {
            strdup_check(event->sub_event.tss_event.event, command->logData,
                    r, error_cleanup);
        } else {
            event->sub_event.tss_event.event = NULL;
        }

        /* Event log of the NV object has to be extended */
        if (command->nv_object.misc.nv.event_log) {
            command->jso_event_log
                = json_tokener_parse(command->nv_object.misc.nv.event_log);
            goto_if_null2(command->jso_event_log, "Out of memory", r,
                          TSS2_FAPI_RC_MEMORY,
                          error_cleanup);
            json_type jsoType = json_object_get_type(command->jso_event_log);
            /* libjson-c does not deliver an array if array has only one element */
            if (jsoType != json_type_array) {
                json_object *jsonArray = json_object_new_array();
                json_object_array_add(jsonArray, command->jso_event_log);
                command->jso_event_log = jsonArray;
            }
        } else {
            /* First event */
            command->jso_event_log = json_object_new_array();
        }
        command->pcr_event.recnum =
            json_object_array_length(command->jso_event_log) + 1;

        r = ifapi_json_IFAPI_EVENT_serialize(&command->pcr_event, &jso);
        goto_if_error(r, "Error serialize event", error_cleanup);

        json_object_array_add(command->jso_event_log, jso);
        SAFE_FREE(object->misc.nv.event_log);
        strdup_check(object->misc.nv.event_log,
            json_object_to_json_string_ext(command->jso_event_log,
                                                    JSON_C_TO_STRING_PRETTY),
            r, error_cleanup);

        /* Perform esys serialization if necessary */
        r = ifapi_esys_serialize_object(context->esys, &command->nv_object);
        goto_if_error(r, "Prepare serialization", error_cleanup);

        /* Start writing the NV object to the key store */
        r = ifapi_keystore_store_async(&context->keystore, &context->io,
                                       command->nvPath,
                                       &command->nv_object);

        goto_if_error_reset_state(r, "Could not open: %sh", error_cleanup,
                                  command->nvPath);

        fallthrough;

    statecase(context->state, NV_EXTEND_WRITE)
        /* Finish writing the NV object to the key store */
        r = ifapi_keystore_store_finish(&context->keystore, &context->io);
        return_try_again(r);
        return_if_error_reset_state(r, "write_finish failed");
        fallthrough;

    statecase(context->state, NV_EXTEND_CLEANUP)
        r = ifapi_cleanup_session(context);
        try_again_or_error_goto(r, "Cleanup", error_cleanup);

        context->state = _FAPI_STATE_INIT;
        r = TSS2_RC_SUCCESS;

        break;

    statecasedefault(context->state);
    }

error_cleanup:
    if (command->jso_event_log)
        json_object_put(command->jso_event_log);
    ifapi_cleanup_ifapi_object(object);
    ifapi_cleanup_ifapi_object(&context->loadKey.auth_object);
    ifapi_cleanup_ifapi_object(context->loadKey.key_object);
    ifapi_cleanup_ifapi_object(&context->createPrimary.pkey_object);
    if (cryptoContext) {
        ifapi_crypto_hash_abort(&cryptoContext);
    }
    if (event)
        ifapi_cleanup_event(event);
    SAFE_FREE(command->data);
    SAFE_FREE(command->nvPath);
    SAFE_FREE(command->logData);
    SAFE_FREE(object->misc.nv.event_log);
    ifapi_session_clean(context);
    LOG_TRACE("finsihed");
    return r;
}
