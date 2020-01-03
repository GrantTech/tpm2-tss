/* SPDX-License-Identifier: BSD-2-Clause */
/*******************************************************************************
 * Copyright 2018-2019, Fraunhofer SIT sponsored by Infineon Technologies AG
 * All rights reserved.
 ******************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "tss2_mu.h"
#include "tss2_fapi.h"
#include "fapi_int.h"
#include "fapi_util.h"
#include "tss2_esys.h"
#include "fapi_policy.h"
#include "fapi_crypto.h"
#define LOGMODULE fapi
#include "util/log.h"
#include "util/aux_util.h"

/** One-Call function for Fapi_WriteAuthorizeNv
 *
 * Write the policyDigest of a policy to an NV index so it can be used in policies
 * containing PolicyAuthorizeNV elements.
 *
 * @param [in, out] context The FAPI_CONTEXT
 * @param [in] nvPath The path of the NV index
 * @param [in] policyPath The path of the new policy
 *
 * @retval TSS2_RC_SUCCESS: if the function call was a success.
 * @retval TSS2_FAPI_RC_BAD_REFERENCE: if context or path is NULL.
 * @retval TSS2_FAPI_RC_BAD_CONTEXT: if context corruption is detected.
 * @retval TSS2_FAPI_RC_BAD_PATH: if nvPath or policyPath does not map to a
 *         FAPI policy or NV index.
 * @retval TSS2_FAPI_RC_MEMORY: if the FAPI cannot allocate enough memory
           for internal operations or return parameters.
 * @retval TSS2_FAPI_RC_BAD_SEQUENCE: if the context has an asynchronous
 *         operation already pending.
 * @retval TSS2_FAPI_RC_IO_ERROR: if the data cannot be saved.
 * @retval TSS2_FAPI_RC_MEMORY: if the FAPI cannot allocate enough memory for
 *         internal operations or return parameters.
 */
TSS2_RC
Fapi_WriteAuthorizeNv(
    FAPI_CONTEXT  *context,
    char    const *nvPath,
    char    const *policyPath)
{
    LOG_TRACE("called for context:%p", context);

    TSS2_RC r, r2;

    /* Check for NULL parameters */
    check_not_null(context);
    check_not_null(nvPath);
    check_not_null(policyPath);

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

    r = Fapi_WriteAuthorizeNv_Async(context, nvPath, policyPath);
    return_if_error_reset_state(r, "WriteAuthorizeNV");

    do {
        /* We wait for file I/O to be ready if the FAPI state automata
           are in a file I/O state. */
        r = ifapi_io_poll(&context->io);
        return_if_error(r, "Something went wrong with IO polling");

        /* Repeatedly call the finish function, until FAPI has transitioned
           through all execution stages / states of this invocation. */
        r = Fapi_WriteAuthorizeNv_Finish(context);
    } while ((r & ~TSS2_RC_LAYER_MASK) == TSS2_BASE_RC_TRY_AGAIN);

    /* Reset the ESYS timeout to non-blocking, immediate response. */
    r2 = Esys_SetTimeout(context->esys, 0);
    return_if_error(r2, "Set Timeout to non-blocking");

    return_if_error_reset_state(r, "WriteAuthorizeNV");

    LOG_TRACE("finsihed");
    return TSS2_RC_SUCCESS;
}

/** Asynchronous function for Fapi_WriteAuthorizeNv
 *
 * Write the policyDigest of a policy to an NV index so it can be used in policies
 * containing PolicyAuthorizeNV elements.
 *
 * Call Fapi_WriteAuthorizeNv_Finish to finish the execution of this command.
 *
 * @param [in, out] context The FAPI_CONTEXT
 * @param [in] nvPath The path of the NV index
 * @param [in] policyPath The path of the new policy
 *
 * @retval TSS2_RC_SUCCESS: if the function call was a success.
 * @retval TSS2_FAPI_RC_BAD_REFERENCE: if context or path is NULL.
 * @retval TSS2_FAPI_RC_BAD_CONTEXT: if context corruption is detected.
 * @retval TSS2_FAPI_RC_BAD_PATH: if nvPath or policyPath does not map to a
 *         FAPI policy or NV index.
 * @retval TSS2_FAPI_RC_MEMORY: if the FAPI cannot allocate enough memory
           for internal operations or return parameters.
 * @retval TSS2_FAPI_RC_BAD_SEQUENCE: if the context has an asynchronous
 *         operation already pending.
 * @retval TSS2_FAPI_RC_IO_ERROR: if the data cannot be saved.
 * @retval TSS2_FAPI_RC_MEMORY: if the FAPI cannot allocate enough memory for
 *         internal operations or return parameters.
 */
TSS2_RC
Fapi_WriteAuthorizeNv_Async(
    FAPI_CONTEXT  *context,
    char    const *nvPath,
    char    const *policyPath)
{
    LOG_TRACE("called for context:%p", context);
    LOG_TRACE("nvPath: %s", nvPath);
    LOG_TRACE("policyPath: %s", policyPath);

    TSS2_RC r;

    /* Check for NULL parameters */
    check_not_null(context);
    check_not_null(nvPath);
    check_not_null(policyPath);

    /* Helpful alias pointers */
    IFAPI_api_WriteAuthorizeNv * command = &context->cmd.WriteAuthorizeNV;
    IFAPI_NV_Cmds * nvCmd = &context->nv_cmd;

    r = ifapi_session_init(context);
    return_if_error(r, "Initialize WriterAuthorizeNv");

    strdup_check(command->policyPath, policyPath, r, error_cleanup);
    strdup_check(nvCmd->nvPath, nvPath, r, error_cleanup);

    r = ifapi_keystore_load_async(&context->keystore, &context->io, nvCmd->nvPath);
    goto_if_error2(r, "Could not open: %s", error_cleanup, nvCmd->nvPath);

    context->state = WRITE_AUTHORIZE_NV_READ_NV;
    LOG_TRACE("finsihed");
    return TSS2_RC_SUCCESS;
error_cleanup:
    SAFE_FREE(command->policyPath);
    SAFE_FREE(nvCmd->nvPath);
    return r;
}

/** Asynchronous finish function for Fapi_WriteAuthorizeNv
 *
 * This function should be called after a previous Fapi_WriteAuthorizeNv_Async.
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
Fapi_WriteAuthorizeNv_Finish(
    FAPI_CONTEXT  *context)
{
    LOG_TRACE("called for context:%p", context);

    TSS2_RC r;
    const size_t maxNvSize = sizeof(TPMU_HA) + sizeof(TPMI_ALG_HASH);
    BYTE nvBuffer[maxNvSize];
    size_t offset = 0;

    /* Check for NULL parameters */
    check_not_null(context);

    /* Helpful alias pointers */
    IFAPI_api_WriteAuthorizeNv * command = &context->cmd.WriteAuthorizeNV;
    IFAPI_NV_Cmds * nvCmd = &context->nv_cmd;
    IFAPI_OBJECT *object = &nvCmd->nv_object;
    TPMI_ALG_HASH hashAlg;
    TPMS_POLICY_HARNESS * policyHarness = &context->policy.harness;

    switch (context->state) {
        statecase(context->state, WRITE_AUTHORIZE_NV_READ_NV)
            /* First check whether the file in object store can be updated. */
            r = ifapi_keystore_check_writeable(&context->keystore, &context->io,
                    nvCmd->nvPath);
            goto_if_error_reset_state(r,
                    "Check whether update object store is possible.", error_cleanup);

            r = ifapi_keystore_load_finish(&context->keystore, &context->io, object);
            return_try_again(r);
            return_if_error_reset_state(r, "read_finish failed");

            ifapi_cleanup_ifapi_object(object);
            r = ifapi_initialize_object(context->esys, object);
            goto_if_error_reset_state(r, "Initialize NV object", error_cleanup);

            context->state = WRITE_AUTHORIZE_NV_CALCULATE_POLICY;
            fallthrough;

        statecase(context->state, WRITE_AUTHORIZE_NV_CALCULATE_POLICY)
            hashAlg = object->misc.nv.public.nvPublic.nameAlg;
            r = ifapi_calculate_tree(context, command->policyPath,
                    policyHarness, hashAlg, &command->digest_idx,
                    &command->hash_size);
            if (r != TSS2_RC_SUCCESS) {
                ifapi_cleanup_ifapi_object(object);
            }
            return_try_again(r);
            goto_if_error(r, "Fapi calculate tree.", error_cleanup);

            r = ifapi_get_sessions_async(context, IFAPI_SESSION_GENEK | IFAPI_SESSION1,
                    TPMA_SESSION_DECRYPT, 0);
            goto_if_error_reset_state(r, "Create sessions", error_cleanup);

            context->state = WRITE_AUTHORIZE_NV_WAIT_FOR_SESSION;
            fallthrough;

        statecase(context->state, WRITE_AUTHORIZE_NV_WAIT_FOR_SESSION)
    //TODO: Pass the namealg of the NV index into the session to be created
            r = ifapi_get_sessions_finish(context, &context->profiles.default_profile);
            return_try_again(r);
            goto_if_error_reset_state(r, " FAPI create session", error_cleanup);

            context->state = WRITE_AUTHORIZE_NV_WRITE_NV_RAM;
            fallthrough;

        statecase(context->state, WRITE_AUTHORIZE_NV_WRITE_NV_RAM_PREPARE)

            /* Copy hash alg followed by digest into buffer to be written to NV ram */
            r = Tss2_MU_TPMI_ALG_HASH_Marshal(
                    object->misc.nv.public.nvPublic.nameAlg,
                    &nvBuffer[0], maxNvSize, &offset);
            goto_if_error_reset_state(r, "FAPI marshal hash alg", error_cleanup);

            void * currentDigest =
                &policyHarness->policyDigests.digests[command->digest_idx].digest;
            memcpy(&nvBuffer[offset], currentDigest, command->hash_size);

            nvCmd->data = &nvBuffer[0];
            nvCmd->numBytes = command->hash_size + sizeof(TPMI_ALG_HASH);
            fallthrough;

        statecase(context->state, WRITE_AUTHORIZE_NV_WRITE_NV_RAM)
            r = ifapi_nv_write(context, nvCmd->nvPath, 0,
                    nvCmd->data, context->nv_cmd.numBytes);
            return_try_again(r);
            goto_if_error_reset_state(r, " FAPI NV Write", error_cleanup);

            /* Perform esys serialization if necessary */
            r = ifapi_esys_serialize_object(context->esys, object);
            goto_if_error(r, "Prepare serialization", error_cleanup);

             /* Save NV object to ensure that changed flags are updated. */
            r = ifapi_keystore_store_async(&context->keystore, &context->io,
                    nvCmd->nvPath, object);
            goto_if_error_reset_state(r, "Could not open: %sh", error_cleanup,
                    nvCmd->nvPath);

            context->state = WRITE_AUTHORIZE_NV_WRITE_OBJCECT;
            fallthrough;

        statecase(context->state, WRITE_AUTHORIZE_NV_WRITE_OBJCECT)
            /* Finish writing the NV object to the key store */
            r = ifapi_keystore_store_finish(&context->keystore, &context->io);
            return_try_again(r);
            return_if_error_reset_state(r, "write_finish failed");

            context->state = WRITE_AUTHORIZE_NV_WRITE_POLICY;
            fallthrough;

        statecase(context->state, WRITE_AUTHORIZE_NV_WRITE_POLICY_PREPARE)
            r = ifapi_policy_store_store_async(&context->pstore, &context->io,
                                               command->policyPath, policyHarness);
            goto_if_error_reset_state(r, "Could not open: %s", error_cleanup,
                    command->policyPath);
            fallthrough;

        statecase(context->state,  WRITE_AUTHORIZE_NV_WRITE_POLICY)
            /* Save policy with computed digest */
            r = ifapi_policy_store_store_finish(&context->pstore, &context->io);
            return_try_again(r);
            return_if_error_reset_state(r, "write_finish failed");

            fallthrough;

        statecase(context->state, WRITE_AUTHORIZE_NV_CLEANUP)
            r = ifapi_cleanup_session(context);
            try_again_or_error_goto(r, "Cleanup", error_cleanup);
            context->state = _FAPI_STATE_INIT;
            break;

        statecasedefault(context->state);
    }

error_cleanup:
    SAFE_FREE(command->policyPath);
    SAFE_FREE(nvCmd->nvPath);
    ifapi_session_clean(context);
    ifapi_cleanup_policy_harness(policyHarness);
    ifapi_cleanup_ifapi_object(&context->loadKey.auth_object);
    ifapi_cleanup_ifapi_object(context->loadKey.key_object);
    ifapi_cleanup_ifapi_object(&context->createPrimary.pkey_object);
    ifapi_cleanup_ifapi_object(object);
    LOG_TRACE("finsihed");
    return r;
}
