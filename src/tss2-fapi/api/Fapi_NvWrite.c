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
#include "fapi_int.h"
#include "fapi_util.h"
#include "tss2_esys.h"
#include "fapi_policy.h"
#define LOGMODULE fapi
#include "util/log.h"
#include "util/aux_util.h"

/** One-Call function for Fapi_NvWrite
 *
 * Writes data to a "regular" (not pin, extend or counter) NV index.
 *
 * @param [in, out] context The FAPI_CONTEXT
 * @param [in] nvPath The path of the NV index to write
 * @param [in] data The data to write to the NV index
 * @param [in] size The size of data in bytes
 *
 * @retval TSS2_RC_SUCCESS: if the function call was a success.
 * @retval TSS2_FAPI_RC_BAD_REFERENCE: if context, nvPath, or data is NULL.
 * @retval TSS2_FAPI_RC_BAD_CONTEXT: if context corruption is detected.
 * @retval TSS2_FAPI_RC_BAD_PATH: if nvPath is not found.
 * @retval TSS2_FAPI_RC_NV_EXCEEDED: if the NV is not large enough for the data
 *         to be written.
 * @retval TSS2_FAPI_RC_NV_WRONG_TYPE: if the NV index is not a "regular" one.
 * @retval TSS2_FAPI_RC_NV_NOT_WRITEABLE: if the NV is not a writeable index.
 * @retval TSS2_FAPI_RC_POLICY_UNKNOWN: if the policy is unknown.
 * @retval TSS2_FAPI_RC_BAD_SEQUENCE: if the context has an asynchronous
 *         operation already pending.
 * @retval TSS2_FAPI_RC_IO_ERROR: if the data cannot be saved.
 * @retval TSS2_FAPI_RC_MEMORY: if the FAPI cannot allocate enough memory for
 *         internal operations or return parameters.
 */
TSS2_RC
Fapi_NvWrite(
    FAPI_CONTEXT  *context,
    char    const *nvPath,
    uint8_t const *data,
    size_t         size)
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

    r = Fapi_NvWrite_Async(context, nvPath, data, size);
    return_if_error_reset_state(r, "NV_Write");

    do {
        /* We wait for file I/O to be ready if the FAPI state automata
           are in a file I/O state. */
        r = ifapi_io_poll(&context->io);
        return_if_error(r, "Something went wrong with IO polling");

        /* Repeatedly call the finish function, until FAPI has transitioned
           through all execution stages / states of this invocation. */
        r = Fapi_NvWrite_Finish(context);
    } while ((r & ~TSS2_RC_LAYER_MASK) == TSS2_BASE_RC_TRY_AGAIN);

    /* Reset the ESYS timeout to non-blocking, immediate response. */
    r2 = Esys_SetTimeout(context->esys, 0);
    return_if_error(r2, "Set Timeout to non-blocking");

    return_if_error_reset_state(r, "NV_Write");

    LOG_TRACE("finsihed");
    return TSS2_RC_SUCCESS;
}

/** Asynchronous function for Fapi_NvWrite
 *
 * Writes data to a "regular" (not pin, extend or counter) NV index.
 *
 * Call Fapi_NvWrite_Finish to finish the execution of this command.
 *
 * @param [in, out] context The FAPI_CONTEXT
 * @param [in] nvPath The path of the NV index to write
 * @param [in] data The data to write to the NV index
 * @param [in] size The size of data in bytes
 *
 * @retval TSS2_RC_SUCCESS: if the function call was a success.
 * @retval TSS2_FAPI_RC_BAD_REFERENCE: if context, nvPath, or data is NULL.
 * @retval TSS2_FAPI_RC_BAD_CONTEXT: if context corruption is detected.
 * @retval TSS2_FAPI_RC_BAD_PATH: if nvPath is not found.
 * @retval TSS2_FAPI_RC_NV_EXCEEDED: if the NV is not large enough for the data
 *         to be written.
 * @retval TSS2_FAPI_RC_NV_WRONG_TYPE: if the NV index is not a "regular" one.
 * @retval TSS2_FAPI_RC_NV_NOT_WRITEABLE: if the NV is not a writeable index.
 * @retval TSS2_FAPI_RC_POLICY_UNKNOWN: if the policy is unknown.
 * @retval TSS2_FAPI_RC_BAD_SEQUENCE: if the context has an asynchronous
 *         operation already pending.
 * @retval TSS2_FAPI_RC_IO_ERROR: if the data cannot be saved.
 * @retval TSS2_FAPI_RC_MEMORY: if the FAPI cannot allocate enough memory for
 *         internal operations or return parameters.
 */
TSS2_RC
Fapi_NvWrite_Async(
    FAPI_CONTEXT  *context,
    char    const *nvPath,
    uint8_t const *data,
    size_t         size)
{
    LOG_TRACE("called for context:%p", context);
    LOG_TRACE("nvPath: %s", nvPath);
    if (data) {
        LOGBLOB_TRACE(data, size, "data");
    } else {
        LOG_TRACE("data: (null) size: %zi", size);
    }

    TSS2_RC r;

    /* Check for NULL parameters */
    check_not_null(context);
    check_not_null(nvPath);
    check_not_null(data);

    /* Helpful alias pointers */
    IFAPI_NV_Cmds * command = &context->nv_cmd;

    r = ifapi_session_init(context);
    return_if_error(r, "Initialize NV_Write");

    /* Initialize the command */
    uint8_t * commandData = NULL;
    memset(&context->nv_cmd, 0, sizeof(IFAPI_NV_Cmds));
    command->offset = 0;
    command->data = NULL;
    strdup_check(command->nvPath, nvPath, r, error_cleanup);

    commandData = malloc(size);
    goto_if_null2(commandData, "Out of memory", r, TSS2_FAPI_RC_MEMORY,
            error_cleanup);
    memcpy(commandData, data, size);
    command->data = commandData;

    context->primary_state = PRIMARY_INIT;
    r = ifapi_get_sessions_async(context,
                                 IFAPI_SESSION_GENEK | IFAPI_SESSION1,
                                 TPMA_SESSION_DECRYPT, 0);
    goto_if_error_reset_state(r, "Create sessions", error_cleanup);

    context->state = NV_WRITE_WAIT_FOR_SESSION;
    command->numBytes = size;
    if (context->state == _FAPI_STATE_INIT)
        ifapi_session_init(context);
    context->state = NV_WRITE_WAIT_FOR_SESSION;
    LOG_TRACE("finsihed");
    return TSS2_RC_SUCCESS;

error_cleanup:
    SAFE_FREE(command->nvPath);
    SAFE_FREE(command->data);
    return r;
}

/** Asynchronous finish function for Fapi_NvWrite
 *
 * This function should be called after a previous Fapi_NvWrite.
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
Fapi_NvWrite_Finish(
    FAPI_CONTEXT  *context)
{
    LOG_TRACE("called for context:%p", context);

    TSS2_RC r;
    json_object *jso = NULL;

    /* Check for NULL parameters */
    check_not_null(context);

    /* Helpful alias pointers */
    IFAPI_NV_Cmds * command = &context->nv_cmd;

    switch (context->state) {
    statecase(context->state, NV_WRITE_WAIT_FOR_SESSION);
//TODO: Pass the namealg of the NV index into the session to be created
        r = ifapi_get_sessions_finish(context, &context->profiles.default_profile);
        return_try_again(r);
        goto_if_error_reset_state(r, " FAPI create session", error_cleanup);

        context->state = NV_WRITE_READ;
        fallthrough;

    statecase(context->state, NV_WRITE_READ);
        /* First check whether the file in object store can be updated. */
        r = ifapi_keystore_check_writeable(&context->keystore, &context->io, command->nvPath);
        goto_if_error_reset_state(r, "Check whether update object store is possible.", error_cleanup);

        r = ifapi_nv_write(context, command->nvPath, command->offset,
                           command->data, command->numBytes);

        return_try_again(r);

        goto_if_error_reset_state(r, " FAPI NV Write", error_cleanup);


        /* Perform esys serialization if necessary */
        r = ifapi_esys_serialize_object(context->esys, &command->nv_object);
        goto_if_error(r, "Prepare serialization", error_cleanup);

        /* Start writing the NV object to the key store */
        r = ifapi_keystore_store_async(&context->keystore, &context->io,
                                       command->nvPath,
                                       &command->nv_object);
        goto_if_error_reset_state(r, "Could not open: %sh", error_cleanup,
                                  command->nvPath);

        context->state = NV_WRITE_WRITE;
        fallthrough;

    statecase(context->state, NV_WRITE_WRITE);
        /* Finish writing the NV object to the key store */
        r = ifapi_keystore_store_finish(&context->keystore, &context->io);
        return_try_again(r);
        return_if_error_reset_state(r, "write_finish failed");
        fallthrough;

    statecase(context->state, NV_WRITE_CLEANUP)
        r = ifapi_cleanup_session(context);
        try_again_or_error_goto(r, "Cleanup", error_cleanup);

        context->state =  _FAPI_STATE_INIT;
        break;

    statecasedefault(context->state);
    }

error_cleanup:
    ifapi_cleanup_ifapi_object(&command->nv_object);
    ifapi_cleanup_ifapi_object(&context->loadKey.auth_object);
    ifapi_cleanup_ifapi_object(context->loadKey.key_object);
    ifapi_cleanup_ifapi_object(&context->createPrimary.pkey_object);
    SAFE_FREE(context->nv_cmd.write_data);
    SAFE_FREE(command->nvPath);
    SAFE_FREE(command->data);
    SAFE_FREE(jso);
    ifapi_session_clean(context);

    LOG_TRACE("finsihed");
    return r;
}
