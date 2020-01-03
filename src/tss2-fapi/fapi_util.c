/* SPDX-License-Identifier: BSD-2-Clause */
/*******************************************************************************
 * Copyright 2018-2019, Fraunhofer SIT sponsored by Infineon Technologies AG
 * All rights reserved.
 *******************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>

#include "tss2_mu.h"
#include "fapi_util.h"
#include "fapi_crypto.h"
#include "ifapi_helpers.h"
#include "ifapi_json_serialize.h"
#include "ifapi_json_deserialize.h"
#include "tpm_json_deserialize.h"
#include "fapi_policy.h"
#include "ifapi_policyutil_execute.h"
#define LOGMODULE fapi
#include "util/log.h"
#include "util/aux_util.h"

/** State machine for flushing objects.
 *
 * @param[in] context The FAPI_CONTEXT.
 * @param[in] handle of the object to be flushed.
 *
 * @retval TSS2_RC_SUCCESS on success.
 * @retval All possible error codes of ESAPI.
 */
TSS2_RC
ifapi_flush_object(FAPI_CONTEXT *context, ESYS_TR handle)
{
    TSS2_RC r = TSS2_RC_SUCCESS;

    if (handle == ESYS_TR_NONE)
        return r;

    switch (context->flush_object_state) {
    statecase(context->flush_object_state, FLUSH_INIT);
        r = Esys_FlushContext_Async(context->esys, handle);
        return_if_error(r, "Flush Object");
        fallthrough;

    statecase(context->flush_object_state, WAIT_FOR_FLUSH);
        r = Esys_FlushContext_Finish(context->esys);
        if ((r & ~TSS2_RC_LAYER_MASK) == TSS2_BASE_RC_TRY_AGAIN)
            return TSS2_FAPI_RC_TRY_AGAIN;

        return_if_error(r, "FlushContext");

        context->flush_object_state = FLUSH_INIT;
        return TSS2_RC_SUCCESS;

    statecasedefault(context->flush_object_state);
    }
}

/** Preparation for getting a session handle.
 *
 * The corresponding async call be executed and a session secret for encryption
 * TPM2B parameters will be created.
 *
 * @param[in] context The FAPI_CONTEXT.
 * @param[in] tpmkey The key which will be used for the encryption of the sesssion
 *            secret.
 * @param[in] profile The FAPI profile will be used to adjust the sessions symmetric
 *            parameters.
 *
 * @retval TSS2_RC_SUCCESS on success.
 * @retval All possible error codes of ESAPI.
 */
TSS2_RC
ifapi_get_session_async(ESYS_CONTEXT *esys, ESYS_TR saltkey, const IFAPI_PROFILE *profile)
{
    TSS2_RC r;

    r = Esys_StartAuthSession_Async(esys, saltkey,
                                    ESYS_TR_NONE,
                                    ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                                    NULL,
                                    TPM2_SE_HMAC, &profile->session_symmetric,
                                    profile->nameAlg);
//TODO: Get the key objects's nameAlg that the session will be applied to for sessionHash
    return_if_error(r, "Creating session.");

    return TSS2_RC_SUCCESS;
}

/**  Call for getting a session handle and adjust session parameters.
 *
 * @param[in] context The FAPI_CONTEXT.
 * @param[out] session The session handle.
 * @param[in] flags The flags to adjust the session attributes.
 *
 * @retval TSS2_RC_SUCCESS on success.
 * @retval All possible error codes of ESAPI.
 */
TSS2_RC
ifapi_get_session_finish(ESYS_CONTEXT *esys, ESYS_TR *session,
                         TPMA_SESSION flags)
{
    TSS2_RC r;
    TPMA_SESSION sessionAttributes = 0;

    /* Check whether authorization callback is defined */

    r = Esys_StartAuthSession_Finish(esys, session);
    if (r != TSS2_RC_SUCCESS)
        return r;

    sessionAttributes |= flags;
    sessionAttributes |= TPMA_SESSION_CONTINUESESSION;

    r = Esys_TRSess_SetAttributes(esys, *session, sessionAttributes,
                                  0xff);
    return_if_error(r, "Set session attributes.");

    return TSS2_RC_SUCCESS;
}

TSS2_RC
pop_object_from_list(FAPI_CONTEXT *context, NODE_OBJECT_T **object_list)
{
    return_if_null(*object_list, "Pop from list.", TSS2_FAPI_RC_BAD_REFERENCE);

    NODE_OBJECT_T *head = *object_list;
    NODE_OBJECT_T *next = head->next;
    *object_list = next;
    ifapi_free_object(context, (void *)&head->object);
    free(head);
    return TSS2_RC_SUCCESS;
}

/** Set authorization value for a FAPI object.
 *
 * @param[in,out] context The FAPI_CONTEXT.
 *
 * @retval TSS2_RC_SUCCESS on success.
 * @retval TSS2_FAPI_RC_AUTHORIZATION_UNKNOWN If the callback for getting
 *         the auth value is not defined.
 */
TSS2_RC
ifapi_set_auth(
    FAPI_CONTEXT *context,
    IFAPI_OBJECT *auth_object,
    const char *description)
{
    TSS2_RC r;
    char *auth = NULL;
    TPM2B_AUTH authValue = { .size = 0, .buffer = {0} };
    char *obj_description;

    obj_description = get_description(auth_object);

    if (obj_description)
        description = obj_description;

    /* Check whether callback is defined. */
    if (context->callbacks.auth) {
        r = context->callbacks.auth(context, description, &auth,
                                        context->callbacks.authData);
        return_if_error(r, "policyAuthCallback");
        if (auth != NULL) {
            authValue.size = strlen(auth);
            memcpy(&authValue.buffer[0], auth, authValue.size);
        }
        /* Store auth value in the ESYS object. */
        r = Esys_TR_SetAuth(context->esys, auth_object->handle, &authValue);
        return_if_error(r, "Set auth value.");

        SAFE_FREE(auth);
        return TSS2_RC_SUCCESS;
    }
    SAFE_FREE(auth);
    return  TSS2_FAPI_RC_AUTHORIZATION_UNKNOWN;
}

/** Preparation for getting a free handle after a start handle number.
 *
 * @param[in] context The FAPI_CONTEXT.
 * @param[in] handle The start value for handle search.
 *
 * @retval TSS2_RC_SUCCESS on success.
 * @retval All possible error codes of ESAPI.
 */
TSS2_RC
ifapi_get_free_handle_async(FAPI_CONTEXT *fctx, TPM2_HANDLE *handle)
{
    TSS2_RC r = Esys_GetCapability_Async(fctx->esys,
                                         ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                                         TPM2_CAP_HANDLES, *handle, 1);
    return_if_error(r, "GetCapability");
    return r;
}

/** Execution of get capability until a free handle is found.
 *
 * The get capability method is called until a free handle is found
 * or the max number of trials passe to the function is exeeded.
 *
 * @param[in] context The FAPI_CONTEXT.
 * @param[out] handle The free handle.
 * @param[in] max The maximal number of trials.
 *
 * @retval TSS2_RC_SUCCESS on success.
 * @retval TSS2_FAPI_RC_NV_TOO_SMALL if too many NV handles are defined.
 * @retval All possible error codes of ESAPI.
 */
TSS2_RC
ifapi_get_free_handle_finish(FAPI_CONTEXT *fctx, TPM2_HANDLE *handle,
                             TPM2_HANDLE max)
{
    TPMI_YES_NO moreData;
    TPMS_CAPABILITY_DATA *capabilityData = NULL;
    TSS2_RC r = Esys_GetCapability_Finish(fctx->esys,
                                          &moreData, &capabilityData);

    if ((r & ~TSS2_RC_LAYER_MASK) == TSS2_BASE_RC_TRY_AGAIN)
        return TSS2_FAPI_RC_TRY_AGAIN;

    return_if_error(r, "GetCapability");

    if (capabilityData->data.handles.count == 0 ||
            capabilityData->data.handles.handle[0] != *handle) {
        SAFE_FREE(capabilityData);
        return TSS2_RC_SUCCESS;
    }
    SAFE_FREE(capabilityData);
    *handle += 1;
    if (*handle > max) {
        return_error(TSS2_FAPI_RC_NV_TOO_SMALL, "No NV index free.");
    }

    r = ifapi_get_free_handle_async(fctx, handle);
    return_if_error(r, "GetCapability");

    return TSS2_FAPI_RC_TRY_AGAIN;
}

static TSS2_RC
get_explicit_key_path(
    IFAPI_KEYSTORE *keystore,
    const char *ipath,
    NODE_STR_T **result)
{
    NODE_STR_T *list_node1 = NULL;
    NODE_STR_T *list_node = NULL;
    TSS2_RC r = init_explicit_key_path(keystore->defaultprofile, ipath,
                                       &list_node1, &list_node, result);
    goto_if_error(r, "init_explicit_key_path", error);

    while (list_node != NULL) {
        if (!add_string_to_list(*result, list_node->str)) {
            LOG_ERROR("Out of memory");
            r = TSS2_FAPI_RC_MEMORY;
            goto error;
        }
        list_node = list_node->next;
    }
    free_string_list(list_node1);
    return TSS2_RC_SUCCESS;

error:
    if (*result)
        free_string_list(*result);
    if (list_node1)
        free_string_list(list_node1);
    return r;
}

TSS2_RC
ifapi_init_primary_async(FAPI_CONTEXT *context, TSS2_KEY_TYPE ktype)
{
    TSS2_RC r;
    IFAPI_OBJECT *hierarchy;
    hierarchy = &context->cmd.Provision.hierarchy;
    TPMS_POLICY_HARNESS *policy;

    if (ktype == TSS2_EK) {
        /* Value set according to EK credential profile. */
        if (context->cmd.Provision.public_templ.public.publicArea.type == TPM2_ALG_RSA) {
            context->cmd.Provision.public_templ.public.publicArea.unique.rsa.size = 256;
        } else if (context->cmd.Provision.public_templ.public.publicArea.type == TPM2_ALG_ECC) {
            context->cmd.Provision.public_templ.public.publicArea.unique.ecc.x.size = 32;
            context->cmd.Provision.public_templ.public.publicArea.unique.ecc.y.size = 32;
        }
        ifapi_init_hierarchy_object(hierarchy, ESYS_TR_RH_ENDORSEMENT);
        policy = context->profiles.default_profile.ek_policy;
    } else if (ktype == TSS2_SRK) {
        policy = context->profiles.default_profile.srk_policy;
        ifapi_init_hierarchy_object(hierarchy, ESYS_TR_RH_OWNER);
    } else {
        return_error(TSS2_FAPI_RC_BAD_VALUE,
                     "Invalid key type. Only EK or SRK allowed");
    }

    if (policy) {
        /* Duplicate policy to prevent profile policy from cleanup. */
        policy = ifapi_copy_policy_harness(policy);
        return_if_null(policy, "Out of memory.", TSS2_FAPI_RC_MEMORY);

        r = ifapi_calculate_tree(context, NULL, /**< no path needed */
                                 policy,
                                 context->profiles.default_profile.nameAlg,
                                 &context->cmd.Provision.digest_idx,
                                 &context->cmd.Provision.hash_size);
        return_if_error(r, "Policy calculation");

        context->cmd.Provision.public_templ.public.publicArea.authPolicy.size =
            context->cmd.Provision.hash_size;
        memcpy(&context->cmd.Provision.public_templ.public.publicArea.authPolicy.buffer[0],
               &policy->policyDigests.digests[context->policy.digest_idx].digest,
               context->cmd.Provision.hash_size);
    }
    context->createPrimary.pkey_object.policy_harness = policy;

    memset(&context->cmd.Provision.inSensitive, 0, sizeof(TPM2B_SENSITIVE_CREATE));
    memset(&context->cmd.Provision.outsideInfo, 0, sizeof(TPM2B_DATA));
    memset(&context->cmd.Provision.creationPCR, 0, sizeof(TPML_PCR_SELECTION));

    r = Esys_CreatePrimary_Async(context->esys, hierarchy->handle,
                                 ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
                                 &context->cmd.Provision.inSensitive,
                                 &context->cmd.Provision.public_templ.public,
                                 &context->cmd.Provision.outsideInfo,
                                 &context->cmd.Provision.creationPCR);
    return r;
}

TSS2_RC
ifapi_init_primary_finish(FAPI_CONTEXT *context, TSS2_KEY_TYPE ktype)
{
    TSS2_RC r;
    ESYS_TR primaryHandle;
    IFAPI_OBJECT *hierarchy;
    TPM2B_PUBLIC *outPublic = NULL;
    TPM2B_CREATION_DATA *creationData = NULL;
    TPM2B_DIGEST *creationHash = NULL;
    TPMT_TK_CREATION *creationTicket = NULL;
    IFAPI_KEY *pkey = &context->createPrimary.pkey_object.misc.key;
    NODE_STR_T *k_sub_path = NULL;

    hierarchy = &context->cmd.Provision.hierarchy;

    r = Esys_CreatePrimary_Finish(context->esys,
                                  &primaryHandle, &outPublic, &creationData, &creationHash,
                                  &creationTicket);
    if ((r & ~TSS2_RC_LAYER_MASK) == TSS2_BASE_RC_TRY_AGAIN)
        return TSS2_FAPI_RC_TRY_AGAIN;

    /* Retry with authorization callback after trial with null auth */
    if ((((r & ~TPM2_RC_N_MASK) == TPM2_RC_BAD_AUTH))
            && (context->state ==  PROVISION_AUTH_EK_NO_AUTH_SENT ||
                context->state ==  PROVISION_AUTH_SRK_NO_AUTH_SENT)) {
        r = ifapi_set_auth(context, hierarchy, "CreatePrimary");
        goto_if_error_reset_state(r, "CreatePrimary", error_cleanup);

        r = Esys_CreatePrimary_Async(context->esys, hierarchy->handle,
                                     ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
                                     &context->cmd.Provision.inSensitive,
                                     &context->cmd.Provision.public,
                                     &context->cmd.Provision.outsideInfo,
                                     &context->cmd.Provision.creationPCR);
        goto_if_error_reset_state(r, "CreatePrimary", error_cleanup);

        if (ktype == TSS2_EK)
            context->state = PROVISION_AUTH_EK_AUTH_SENT;
        else
            context->state = PROVISION_AUTH_SRK_AUTH_SENT;
        return TSS2_FAPI_RC_TRY_AGAIN;

    } else {
        goto_if_error_reset_state(r, "FAPI Provision", error_cleanup);
    }

    if (ktype == TSS2_EK) {
        context->ek_handle = primaryHandle;
    } else if (ktype == TSS2_SRK) {
        context->srk_handle = primaryHandle;
    } else {
        return_error(TSS2_FAPI_RC_BAD_VALUE,
                     "Invalid key type. Only EK or SRK allowed");
    }

    SAFE_FREE(pkey->serialization.buffer);
    r = Esys_TR_Serialize(context->esys, primaryHandle, &pkey->serialization.buffer,
                          &pkey->serialization.size);
    goto_if_error(r, "Error serialize esys object", error_cleanup);

    r = ifapi_get_name(&outPublic->publicArea, &pkey->name);
    goto_if_error(r, "Get primary name", error_cleanup);

    pkey->public = *outPublic;
    pkey->policyInstance = NULL;
    pkey->creationData = *creationData;
    pkey->creationTicket = *creationTicket;
    pkey->description = NULL;
    pkey->certificate = NULL;

    SAFE_FREE(outPublic);
    SAFE_FREE(creationData);
    SAFE_FREE(creationHash);
    SAFE_FREE(creationTicket);

    if (pkey->public.publicArea.type == TPM2_ALG_RSA)
        pkey->signing_scheme = context->profiles.default_profile.rsa_signing_scheme;
    else
        pkey->signing_scheme = context->profiles.default_profile.ecc_signing_scheme;
    context->createPrimary.pkey_object.handle = primaryHandle;
    SAFE_FREE(pkey->serialization.buffer);
    ifapi_cleanup_ifapi_object(&context->createPrimary.pkey_object);
    return TSS2_RC_SUCCESS;

error_cleanup:
    free_string_list(k_sub_path);
    SAFE_FREE(pkey->serialization.buffer);
    ifapi_cleanup_ifapi_object(&context->createPrimary.pkey_object);
    return r;
}

TSS2_RC
ifapi_load_primary_async(FAPI_CONTEXT *context, char *path)
{

    TSS2_RC r;

    memset(&context->createPrimary.pkey_object, 0, sizeof(IFAPI_OBJECT));
    context->createPrimary.path = path;
    r = ifapi_keystore_load_async(&context->keystore, &context->io, path);
    return_if_error2(r, "Could not open: %s", path);
    context->primary_state = PRIMARY_READ_KEY;
    return TSS2_RC_SUCCESS;

}

TSS2_RC
ifapi_load_primary_finish(FAPI_CONTEXT *context, ESYS_TR *handle)
{
    TSS2_RC r;
    IFAPI_OBJECT *hierarchy = &context->createPrimary.hierarchy;

    TPM2B_PUBLIC *outPublic = NULL;
    TPM2B_CREATION_DATA *creationData = NULL;
    TPM2B_DIGEST *creationHash = NULL;
    TPMT_TK_CREATION *creationTicket = NULL;
    //TPM2B_NAME *name;
    IFAPI_OBJECT *pkey_object =  &context->createPrimary.pkey_object;
    IFAPI_KEY *pkey = &context->createPrimary.pkey_object.misc.key;
    ESYS_TR auth_session;

    LOG_TRACE("call");

    switch (context->primary_state) {
    statecase(context->primary_state, PRIMARY_READ_KEY);
        r = ifapi_keystore_load_finish(&context->keystore, &context->io,
                                       pkey_object);
        return_try_again(r);
        return_if_error(r, "read_finish failed");

        r = ifapi_initialize_object(context->esys, pkey_object);
        goto_if_error_reset_state(r, "Initialize key object", error_cleanup);

        if (pkey_object->handle != ESYS_TR_NONE) {
            if (pkey->creationTicket.hierarchy == TPM2_RH_EK) {
                context->ek_persistent = true;
            } else {
                context->srk_persistent = true;
            }
            *handle = pkey_object->handle;
            break;
        } else {
             if (pkey->creationTicket.hierarchy == TPM2_RH_EK) {
                context->ek_persistent = false;
            } else {
                context->srk_persistent = false;
            }
        }
        fallthrough;

    statecase(context->primary_state, PRIMARY_READ_HIERARCHY);
        if (pkey->creationTicket.hierarchy == TPM2_RH_EK) {
            r = ifapi_keystore_load_async(&context->keystore, &context->io, "/HE");
            return_if_error2(r, "Could not open hierarchy /HE");
        } else {
            r = ifapi_keystore_load_async(&context->keystore, &context->io, "/HS");
            return_if_error2(r, "Could not open hierarchy /HS");
        }
        fallthrough;

    statecase(context->primary_state, PRIMARY_READ_HIERARCHY_FINISH);
        r = ifapi_keystore_load_finish(&context->keystore, &context->io, hierarchy);
        return_try_again(r);
        return_if_error(r, "read_finish failed");

        r = ifapi_initialize_object(context->esys, hierarchy);
        goto_if_error_reset_state(r, "Initialize hierarchy object", error_cleanup);

        if (pkey->creationTicket.hierarchy == TPM2_RH_EK) {
            hierarchy->handle = ESYS_TR_RH_ENDORSEMENT;
        } else {
            hierarchy->handle = ESYS_TR_RH_OWNER;
        }
        fallthrough;

    statecase(context->primary_state, PRIMARY_AUTHORIZE_HIERARCHY);
        r = ifapi_authorize_object(context, hierarchy, &auth_session);
        FAPI_SYNC(r, "Authorize hierarchy.", error_cleanup);

        memset(&context->createPrimary.inSensitive, 0, sizeof(TPM2B_SENSITIVE_CREATE));
        memset(&context->createPrimary.outsideInfo, 0, sizeof(TPM2B_DATA));
        memset(&context->createPrimary.creationPCR, 0, sizeof(TPML_PCR_SELECTION));

        r = Esys_CreatePrimary_Async(context->esys, hierarchy->handle,
                                     auth_session, ESYS_TR_NONE, ESYS_TR_NONE,
                                     &context->createPrimary.inSensitive,
                                     &pkey->public,
                                     &context->createPrimary.outsideInfo,
                                     &context->createPrimary.creationPCR);
        return_if_error(r, "CreatePrimary");
        fallthrough;

    statecase(context->primary_state, PRIMARY_HAUTH_SENT);
        if (context->createPrimary.handle) {
            *handle = context->createPrimary.handle;
            context->primary_state = PRIMARY_CREATED;
            return TSS2_FAPI_RC_TRY_AGAIN;
        } else {
            r = Esys_CreatePrimary_Finish(context->esys,
                                          &pkey_object->handle, &outPublic,
                                          &creationData, &creationHash,
                                          &creationTicket);
            return_try_again(r);
            goto_if_error_reset_state(r, "FAPI regenerate primary", error_cleanup);
        }
        *handle = pkey_object->handle;
        context->primary_state = PRIMARY_INIT;
        break;

    statecasedefault(context->primary_state);
    }
    SAFE_FREE(outPublic);
    SAFE_FREE(creationData);
    SAFE_FREE(creationHash);
    SAFE_FREE(creationTicket);
    ifapi_cleanup_ifapi_object(&context->createPrimary.hierarchy);
    return  TSS2_RC_SUCCESS;

error_cleanup:
    SAFE_FREE(outPublic);
    SAFE_FREE(creationData);
    SAFE_FREE(creationHash);
    SAFE_FREE(creationTicket);
    ifapi_cleanup_ifapi_object(&context->createPrimary.hierarchy);
    ifapi_cleanup_ifapi_object(&context->createPrimary.pkey_object);
    return r;
}

TSS2_RC
ifapi_session_init(FAPI_CONTEXT *context)
{
    LOG_TRACE("call");
    return_if_null(context, "No context", TSS2_FAPI_RC_BAD_REFERENCE);

    return_if_null(context->esys, "No context", TSS2_FAPI_RC_NO_TPM);

    if (context->state != _FAPI_STATE_INIT) {
        return_error(TSS2_FAPI_RC_BAD_SEQUENCE, "Invalid State");
    }

    context->session1 = ESYS_TR_NONE;
    context->session2 = ESYS_TR_NONE;
    context->policy.session = ESYS_TR_NONE;
    context->srk_handle = ESYS_TR_NONE;
    return TSS2_RC_SUCCESS;
}

TSS2_RC
ifapi_non_tpm_mode_init(FAPI_CONTEXT *context)
{
    LOG_TRACE("call");
    return_if_null(context, "No context", TSS2_FAPI_RC_BAD_REFERENCE);

    if (context->state != _FAPI_STATE_INIT) {
        return_error(TSS2_FAPI_RC_BAD_SEQUENCE, "Invalid State");
    }

    context->session1 = ESYS_TR_NONE;
    context->session2 = ESYS_TR_NONE;
    context->policy.session = ESYS_TR_NONE;
    context->srk_handle = ESYS_TR_NONE;
    return TSS2_RC_SUCCESS;
}

void
ifapi_session_clean(FAPI_CONTEXT *context)
{
    if (context->session1 != ESYS_TR_NONE) {
        if (Esys_FlushContext(context->esys, context->session1) != TSS2_RC_SUCCESS) {
            LOG_ERROR("Cleanup session failed.");
        }
        context->session1 = ESYS_TR_NONE;
    }
    if (context->session2 != ESYS_TR_NONE) {
        if (Esys_FlushContext(context->esys, context->session2) != TSS2_RC_SUCCESS) {
            LOG_ERROR("Cleanup session failed.");
            context->session2 = ESYS_TR_NONE;
        }
    }
    if (!context->srk_persistent && context->srk_handle != ESYS_TR_NONE) {
        if (Esys_FlushContext(context->esys, context->srk_handle) != TSS2_RC_SUCCESS) {
            LOG_ERROR("Cleanup Policy Session  failed.");
        }
        context->srk_handle = ESYS_TR_NONE;
    }
    context->srk_persistent = false;
}

/** State machine for cleanup of a FAPI session.
 *
 * Used sessions and the SRK will be flushed.
 *
 * @param[in] context The FAPI_CONTEXT storing the used handles.
 *
 * @retval TSS2_RC_SUCCESS on success.
 * @retval All possible error codes of ESAPI.
 */
TSS2_RC
ifapi_cleanup_session(FAPI_CONTEXT *context)
{
    TSS2_RC r;

    switch (context->cleanup_state) {
        statecase(context->cleanup_state, CLEANUP_INIT);
            if (context->session1 != ESYS_TR_NONE) {
                r = Esys_FlushContext_Async(context->esys, context->session1);
                try_again_or_error(r, "Flush session.");
            }
            fallthrough;

        statecase(context->cleanup_state, CLEANUP_SESSION1);
            if (context->session1 != ESYS_TR_NONE) {
                r = Esys_FlushContext_Finish(context->esys);
                try_again_or_error(r, "Flush session.");
            }
            context->session1 = ESYS_TR_NONE;

            if (context->session2 != ESYS_TR_NONE) {
                r = Esys_FlushContext_Async(context->esys, context->session2);
                try_again_or_error(r, "Flush session.");
            }
            fallthrough;

        statecase(context->cleanup_state, CLEANUP_SESSION2);
            if (context->session2 != ESYS_TR_NONE) {
                r = Esys_FlushContext_Finish(context->esys);
                try_again_or_error(r, "Flush session.");
            }
            context->session2 = ESYS_TR_NONE;

            if (!context->srk_persistent && context->srk_handle != ESYS_TR_NONE) {
                r = Esys_FlushContext_Async(context->esys, context->srk_handle);
                try_again_or_error(r, "Flush SRK.");
            }
            fallthrough;

        statecase(context->cleanup_state, CLEANUP_SRK);
            if (!context->srk_persistent && context->srk_handle != ESYS_TR_NONE) {
                r = Esys_FlushContext_Finish(context->esys);
                try_again_or_error(r, "Flush SRK.");

                context->srk_handle = ESYS_TR_NONE;
                context->srk_persistent = false;
            }
            context->cleanup_state = CLEANUP_INIT;
            return TSS2_RC_SUCCESS;

        statecasedefault(context->state);
    }
}

/** Cleanup primary keys in error cases (non asynchronous).
  */
void
ifapi_primary_clean(FAPI_CONTEXT *context)
{
    if (!context->srk_persistent && context->srk_handle != ESYS_TR_NONE) {
        if (Esys_FlushContext(context->esys, context->srk_handle) != TSS2_RC_SUCCESS) {
            LOG_ERROR("Cleanup session failed.");
        }
        context->srk_handle = ESYS_TR_NONE;
    }
    if (!context->ek_persistent && context->ek_handle != ESYS_TR_NONE) {
        if (Esys_FlushContext(context->esys, context->ek_handle) != TSS2_RC_SUCCESS) {
            LOG_ERROR("Cleanup EK failed.");
        }
        context->ek_handle = ESYS_TR_NONE;
    }
    context->srk_persistent = false;
}

TSS2_RC
ifapi_get_sessions_async(FAPI_CONTEXT *context,
                         IFAPI_SESSION_TYPE session_flags,
                         TPMA_SESSION attribute_flags1,
                         TPMA_SESSION attribute_flags2)
{
    TSS2_RC r;

    LOG_TRACE("call");
    context->session_flags = session_flags;
    context->session1_attribute_flags = attribute_flags1;
    context->session2_attribute_flags = attribute_flags2;
    char *file = NULL;

    if (!(session_flags & IFAPI_SESSION_GENEK)) {
        context->srk_handle = ESYS_TR_NONE;
        context->session_state = SESSION_CREATE_SESSION;
        return TSS2_RC_SUCCESS;
    }

    context->primary_state = PRIMARY_INIT;
    r = ifapi_asprintf(&file, "%s/%s", context->config.profile_name,
                       IFAPI_SRK_KEY_PATH);
    goto_if_error(r, "Error ifapi_asprintf", error_cleanup);

    r = ifapi_load_primary_async(context, file);
    return_if_error_reset_state(r, "Load EK");
    free(file);

    context->session_state = SESSION_WAIT_FOR_PRIMARY;
    return TSS2_RC_SUCCESS;

error_cleanup:
    SAFE_FREE(file);
    return r;
}

TSS2_RC
ifapi_get_sessions_finish(FAPI_CONTEXT *context, const IFAPI_PROFILE *profile)
{
    TSS2_RC r;

    switch (context->session_state) {
    statecase(context->session_state, SESSION_WAIT_FOR_PRIMARY);
        LOG_TRACE("**STATE** SESSION_WAIT_FOR_PRIMARY");
        r = ifapi_load_primary_finish(context, &context->srk_handle);
        return_try_again(r);
        return_if_error(r, "Load primary.");
        fallthrough;

    statecase(context->session_state, SESSION_CREATE_SESSION);
        LOG_TRACE("**STATE** SESSION_CREATE_SESSION");
        if (!(context->session_flags & IFAPI_SESSION1)) {
            LOG_TRACE("finished");
            return TSS2_RC_SUCCESS;
        }

        r = ifapi_get_session_async(context->esys, context->srk_handle, profile);
        return_if_error_reset_state(r, "Create FAPI session async");
        fallthrough;

    statecase(context->session_state, SESSION_WAIT_FOR_SESSION1);
        LOG_TRACE("**STATE** SESSION_WAIT_FOR_SESSION1");
        r = ifapi_get_session_finish(context->esys, &context->session1,
                                     context->session1_attribute_flags);
        return_try_again(r);
        return_if_error_reset_state(r, "Create FAPI session finish");

        if (!(context->session_flags & IFAPI_SESSION2)) {
            LOG_TRACE("finished");
            return TSS2_RC_SUCCESS;
        }

        r = ifapi_get_session_async(context->esys, context->srk_handle, profile);
        return_if_error_reset_state(r, "Create FAPI session async");
        fallthrough;

    statecase(context->session_state, SESSION_WAIT_FOR_SESSION2);
        LOG_TRACE("**STATE** SESSION_WAIT_FOR_SESSION2");
        r = ifapi_get_session_finish(context->esys, &context->session2,
                                     context->session2_attribute_flags);
        return_try_again(r);

        return_if_error_reset_state(r, "Create FAPI session finish");
        break;

    statecasedefault(context->session_state);
    }

    return TSS2_RC_SUCCESS;
}

/** Merge profile already stored in FAPI context into a key template
 */
TSS2_RC
ifapi_merge_profile_into_nv_template(
    FAPI_CONTEXT *context,
    IFAPI_NV_TEMPLATE *template)
{
    const TPMA_NV extend_mask = TPM2_NT_EXTEND << TPMA_NV_TPM2_NT_SHIFT;
    const TPMA_NV counter_mask = TPM2_NT_COUNTER << TPMA_NV_TPM2_NT_SHIFT;
    const TPMA_NV bitfield_mask = TPM2_NT_BITS << TPMA_NV_TPM2_NT_SHIFT;
    const IFAPI_PROFILE *profile = &context->profiles.default_profile;
    size_t hash_size;

    template->public.nameAlg = profile->nameAlg;
    if ((template->public.attributes & extend_mask) == extend_mask) {
        /* The size of the NV ram to be extended must be read from the
           profile */
        hash_size = ifapi_hash_get_digest_size(profile->nameAlg);
        template->public.dataSize = hash_size;
    } else if ((template->public.attributes & counter_mask) == counter_mask ||
               (template->public.attributes & bitfield_mask) == bitfield_mask) {
        /* For bit fields and counters only size 8 is possible */
        template->public.dataSize = 8;
    } else {
        /* For normal NV ram the passed size will be used. */
        template->public.dataSize = context->nv_cmd.numBytes;
    }

    return TSS2_RC_SUCCESS;
}

/** Merge profile already stored in FAPI context into a key template
 */
TSS2_RC
ifapi_merge_profile_into_template(
    const IFAPI_PROFILE *profile,
    IFAPI_KEY_TEMPLATE *template)
{
    /* Merge profile parameters */
    template->public.publicArea.type = profile->type;
    template->public.publicArea.nameAlg = profile->nameAlg;
    if (profile->type == TPM2_ALG_RSA) {
        template->public.publicArea.parameters.rsaDetail.keyBits = profile->keyBits;
        template->public.publicArea.parameters.rsaDetail.exponent = profile->exponent;
    } else if (profile->type == TPM2_ALG_ECC) {
        template->public.publicArea.parameters.eccDetail.curveID = profile->curveID;
        template->public.publicArea.parameters.eccDetail.kdf.scheme = TPM2_ALG_NULL;
    }

    /* Set remaining parameters depending on key type */
    if (template->public.publicArea.objectAttributes & TPMA_OBJECT_RESTRICTED) {
        if (template->public.publicArea.objectAttributes & TPMA_OBJECT_DECRYPT) {
            template->public.publicArea.parameters.asymDetail.symmetric =
            profile->sym_parameters;
        } else {
            template->public.publicArea.parameters.asymDetail.symmetric.algorithm =
            TPM2_ALG_NULL;
        }
        if (profile->type == TPM2_ALG_RSA) {
            if (template->public.publicArea.objectAttributes & TPMA_OBJECT_SIGN_ENCRYPT) {
                template->public.publicArea.parameters.rsaDetail.scheme.scheme =
                profile->rsa_signing_scheme.scheme;
                memcpy(&template->public.publicArea.parameters.rsaDetail.scheme.details,
                       &profile->rsa_signing_scheme.details, sizeof(TPMU_ASYM_SCHEME));
            } else {
                template->public.publicArea.parameters.rsaDetail.scheme.scheme = TPM2_ALG_NULL;
            }
        } else if (profile->type == TPM2_ALG_ECC) {
            if (template->public.publicArea.objectAttributes & TPMA_OBJECT_SIGN_ENCRYPT) {
                template->public.publicArea.parameters.eccDetail.scheme.scheme =
                profile->ecc_signing_scheme.scheme;
                memcpy(&template->public.publicArea.parameters.eccDetail.scheme.details,
                       &profile->rsa_signing_scheme.details, sizeof(TPMU_ASYM_SCHEME));
            } else {
                template->public.publicArea.parameters.eccDetail.scheme.scheme = TPM2_ALG_NULL;
            }
        } else {
            template->public.publicArea.parameters.asymDetail.scheme.scheme = TPM2_ALG_NULL;
        }
    } else {
        /* Non restricted key */
        template->public.publicArea.parameters.asymDetail.symmetric.algorithm =
        TPM2_ALG_NULL;
        template->public.publicArea.parameters.asymDetail.scheme.scheme = TPM2_ALG_NULL;
    }
    return TSS2_RC_SUCCESS;
}

static void
full_path_to_fapi_path(IFAPI_KEYSTORE *keystore, char *path)
{
    unsigned int start_pos, end_pos, i;
    const unsigned int path_length = strlen(path);
    size_t keystore_length = strlen(keystore->userdir);
    char fapi_path_delim;

    start_pos = 0;

    if (strncmp(&path[0], keystore->userdir, keystore_length) == 0) {
        start_pos =  strlen(keystore->userdir);
    } else {
        keystore_length = strlen(keystore->systemdir);
        if (strncmp(&path[0], keystore->systemdir, keystore_length) == 0)
            start_pos = strlen(keystore->systemdir);
    }
    if (!start_pos)
        return;

    end_pos = path_length - start_pos;
    memmove(&path[0], &path[start_pos], end_pos);
    size_t ip = 0;
    size_t lp = strlen(path);
    while (ip < lp) {
        if (strncmp(&path[ip], "//", 2) == 0) {
            memmove(&path[ip], &path[ip+1], lp-ip);
            lp -= 1;
        } else {
            ip += 1;
        }
    }

    if (ifapi_path_type_p(path, IFAPI_POLICY_PATH))
        fapi_path_delim = '.';
    else
        fapi_path_delim = IFAPI_FILE_DELIM_CHAR;

    for (i = end_pos - 1; i > 0; i--) {
        if (path[i] == fapi_path_delim) {
            path[i] = '\0';
            break;
        }
    }
}

/** Asynchronous function for loading a key.
  *
  * @param[in,out] context The FAPI_CONTEXT.
  * @param[in]     keyPath the key path without the parent directories storing
  *                of the key store. (e.g. HE/EK, HS/SRK/mykey)
  * @retval All possible error codes of FAPI
  * @retval TSS2_RC_SUCCESS if the function call was a success.
  */
TSS2_RC
ifapi_load_keys_async(FAPI_CONTEXT *context, char const *keyPath)
{
    TSS2_RC r;
    NODE_STR_T *path_list;
    size_t path_length;
    char *fapi_key_path = NULL;

    LOG_TRACE("Load key: %s", keyPath);
    fapi_key_path = strdup(keyPath);
    check_oom(fapi_key_path);
    full_path_to_fapi_path(&context->keystore, fapi_key_path);
    r = get_explicit_key_path(&context->keystore, fapi_key_path, &path_list);
    SAFE_FREE(fapi_key_path);
    return_if_error(r, "Compute explicit path.");

    context->loadKey.path_list = path_list;
    path_length = ifapi_path_length(path_list);
    r = ifapi_load_key_async(context, path_length);
    return_if_error(r, "Load key async.");

    return TSS2_RC_SUCCESS;
}

/** Asynchronous finish function for loading a key.
  *
  * @param[in,out] context The FAPI_CONTEXT.
  * @param[in] flush_parent If false the parent of the key to be loaded
  *            will not be flushed.
  * @param[out]    handle The ESYS handle of the key.
  * @retval All possible error codes of FAPI
  * @retval TSS2_RC_SUCCESS if the function call was a success.
 */
TSS2_RC
ifapi_load_keys_finish(
    FAPI_CONTEXT *context,
    bool flush_parent,
    ESYS_TR *handle,
    IFAPI_OBJECT **key_object)
{
    TSS2_RC r;

    r = ifapi_load_key_finish(context, flush_parent);
    if (r == TSS2_FAPI_RC_TRY_AGAIN)
        return r;

    return_if_error(r, "Load keys");

    *handle = context->loadKey.auth_object.handle;
    /* The current authorization object is the last key loaded and
       will be used. */
    *key_object = &context->loadKey.auth_object;
    free_string_list(context->loadKey.path_list);
    return TSS2_RC_SUCCESS;
}

/** Initialize state machine for loading a key.
 *
 * @param[in,out] context for storing all state information.
 * @param[in] position the position of the key in path list stored in
 *            context->loadKey.path_list.
 * @retval TSS2_RC_SUCCESS If data can be read.
 */
TSS2_RC
ifapi_load_key_async(FAPI_CONTEXT *context, size_t position)
{
    context->loadKey.state = LOAD_KEY_GET_PATH;
    context->loadKey.position = position;
    context->loadKey.key_list = NULL;
    context->loadKey.parent_handle = ESYS_TR_NONE;
    // context->loadKey.auth_object = NULL;

    return TSS2_RC_SUCCESS;
}

/** State machine for loading a key.
 *
 * A stack with all sup keys will be created and decremented during
 * the loading auf all keys.
 * The object of the loaded key will be stored in:
 * context->loadKey.auth_object
 *
 * @param[in,out] context for storing all state information.
 * @retval TSS2_RC_SUCCESS If data can be read.
 */
TSS2_RC
ifapi_load_key_finish(FAPI_CONTEXT *context, bool flush_parent)
{
    TSS2_RC r;
    NODE_STR_T *path_list = context->loadKey.path_list;
    size_t *position =  &context->loadKey.position;
    IFAPI_OBJECT *key_object = NULL;
    IFAPI_KEY *key = NULL;
    ESYS_TR auth_session;

    switch (context->loadKey.state) {
    statecase(context->loadKey.state, LOAD_KEY_GET_PATH);
        context->loadKey.key_path = NULL;
        r = ifapi_path_string_n(&context->loadKey.key_path, NULL, path_list, NULL,
                                *position);
        return_if_error(r, "Compute key path.");

        context->loadKey.key_object = ifapi_allocate_object(context);
        goto_if_null2(context->loadKey.key_object, "Allocating key", r,
                      TSS2_FAPI_RC_MEMORY, error_cleanup);

        goto_if_null2(context->loadKey.key_path, "Invalid path", r,
                      TSS2_FAPI_RC_GENERAL_FAILURE,
                      error_cleanup); /**< to avoid scan-build errors. */

        r = ifapi_keystore_load_async(&context->keystore, &context->io,
                                      context->loadKey.key_path);
        return_if_error2(r, "Could not open: %s", context->loadKey.key_path);
        fallthrough;

    statecase(context->loadKey.state, LOAD_KEY_READ_KEY);
        goto_if_null2(context->loadKey.key_path, "Invalid path", r,
                      TSS2_FAPI_RC_GENERAL_FAILURE,
                      error_cleanup); /**< to avoid scan-build errors. */

        key = &context->loadKey.key_object->misc.key;

        r = ifapi_keystore_load_finish(&context->keystore, &context->io,
                                       context->loadKey.key_object);
        if (r != TSS2_RC_SUCCESS) {
            ifapi_cleanup_ifapi_object(context->loadKey.key_object);
        }
        return_try_again(r);
        return_if_error(r, "read_finish failed");

        if (context->loadKey.key_object->objectType != IFAPI_KEY_OBJ)
            goto_error(r, TSS2_FAPI_RC_BAD_KEY, "%s is no key", error_cleanup,
                       context->loadKey.key_path);

        r = ifapi_initialize_object(context->esys, context->loadKey.key_object);
        goto_if_error_reset_state(r, "Initialize key object", error_cleanup);

        SAFE_FREE(context->loadKey.key_path);
        context->loadKey.handle = context->loadKey.key_object->handle;
        if (context->loadKey.handle != ESYS_TR_NONE) {
            /* Persistent key could be desearialized keys can be loaded */
            r = ifapi_copy_ifapi_key_object(&context->loadKey.auth_object,
                context->loadKey.key_object);
            goto_if_error(r, "Could not copy key object", error_cleanup);
            ifapi_cleanup_ifapi_object(context->loadKey.key_object);
            context->loadKey.state = LOAD_KEY_LOAD_KEY;

            return TSS2_FAPI_RC_TRY_AGAIN;
        }

        if (key->private.size == 0) {
            /* Create a deep copy of the primary key */
            ifapi_cleanup_ifapi_key(key);
            r = ifapi_copy_ifapi_key(key, &context->createPrimary.pkey_object.misc.key);
            goto_if_error(r, "Could not copy primary key", error_cleanup);
            context->primary_state = PRIMARY_READ_HIERARCHY;
            context->loadKey.state = LOAD_KEY_WAIT_FOR_PRIMARY;
            return TSS2_FAPI_RC_TRY_AGAIN;
        }
        IFAPI_OBJECT * copyToPush = malloc(sizeof(IFAPI_OBJECT));
        goto_if_null(copyToPush, "Out of memory", TSS2_FAPI_RC_MEMORY, error_cleanup);
        r = ifapi_copy_ifapi_key_object(copyToPush, context->loadKey.key_object);
        goto_if_error(r, "Could not create a copy to push", error_cleanup);
        r = push_object_to_list(copyToPush, &context->loadKey.key_list);
        goto_if_error(r, "Out of memory", error_cleanup);

        ifapi_cleanup_ifapi_object(context->loadKey.key_object);

        *position -= 1;
        context->loadKey.state = LOAD_KEY_GET_PATH;
        return TSS2_FAPI_RC_TRY_AGAIN;

    statecase(context->loadKey.state, LOAD_KEY_LOAD_KEY);
        if (!(context->loadKey.key_list)) {
            LOG_TRACE("All keys loaded.");
            return TSS2_RC_SUCCESS;
        }

        /* if flush_parent is false parent is only flushed if a new parent
           is available */
        if (!flush_parent && context->loadKey.parent_handle != ESYS_TR_NONE) {
            r = Esys_FlushContext(context->esys, context->loadKey.parent_handle);
            goto_if_error_reset_state(r, "Flush object", error_cleanup);
        }
        fallthrough;

    statecase(context->loadKey.state, LOAD_KEY_AUTHORIZE);
        key_object = context->loadKey.key_list->object;
        key = &key_object->misc.key;
        r = ifapi_authorize_object(context, &context->loadKey.auth_object, &auth_session);
        FAPI_SYNC(r, "Authorize key.", error_cleanup);

        /* Store parent handle in context for usage in ChangeAuth if not persistent */
        context->loadKey.parent_handle =  context->loadKey.handle;
        if (context->loadKey.auth_object.misc.key.persistent_handle)
            context->loadKey.parent_handle_persistent = true;
        else
            context->loadKey.parent_handle_persistent = false;

        TPM2B_PRIVATE private;

        private.size = key->private.size;
        memcpy(&private.buffer[0], key->private.buffer, key->private.size);

        r = Esys_Load_Async(context->esys, context->loadKey.handle,
                            auth_session,
                            ESYS_TR_NONE, ESYS_TR_NONE,
                            &private, &key->public);
        goto_if_error(r, "Load async", error_cleanup);
        fallthrough;

    statecase(context->loadKey.state, LOAD_KEY_AUTH);
        r = Esys_Load_Finish(context->esys, &context->loadKey.handle);
        return_try_again(r);
        goto_if_error_reset_state(r, "Load", error_cleanup);

        /* The current parent is flushed if not prohibited by flush parent */
        if (flush_parent && context->loadKey.auth_object.objectType == IFAPI_KEY_OBJ &&
            ! context->loadKey.auth_object.misc.key.persistent_handle) {
            r = Esys_FlushContext(context->esys, context->loadKey.auth_object.handle);
            goto_if_error_reset_state(r, "Flush object", error_cleanup);

        }
        LOG_TRACE("New key used as auth object.");
        ifapi_cleanup_ifapi_object(&context->loadKey.auth_object);
        r = ifapi_copy_ifapi_key_object(&context->loadKey.auth_object,
                context->loadKey.key_list->object);
        goto_if_error(r, "Could not copy loaded key", error_cleanup);
        context->loadKey.auth_object.handle =  context->loadKey.handle;
        IFAPI_OBJECT *top_obj = context->loadKey.key_list->object;
        ifapi_cleanup_ifapi_object(top_obj);
        SAFE_FREE(context->loadKey.key_list->object);
        r = pop_object_from_list(context, &context->loadKey.key_list);
        goto_if_error_reset_state(r, "Pop key failed.", error_cleanup);

        if (context->loadKey.key_list) {
            /* Object can be cleaned if it's not the last */
            ifapi_free_object(context, &top_obj);
        }

        context->loadKey.state = LOAD_KEY_LOAD_KEY;
        return TSS2_FAPI_RC_TRY_AGAIN;

    statecase(context->loadKey.state, LOAD_KEY_WAIT_FOR_PRIMARY);
        r = ifapi_load_primary_finish(context, &context->loadKey.handle);
        return_try_again(r);
        goto_if_error(r, "CreatePrimary", error_cleanup);

        /* Save the primary object for authorization */
        r = ifapi_copy_ifapi_key_object(&context->loadKey.auth_object,
                &context->createPrimary.pkey_object);
        goto_if_error(r, "Could not copy primary key", error_cleanup);

        if (context->loadKey.key_list) {
            context->loadKey.state = LOAD_KEY_LOAD_KEY;
            return TSS2_FAPI_RC_TRY_AGAIN;
        } else {
            LOG_TRACE("success");
            ifapi_cleanup_ifapi_object(context->loadKey.key_object);
            ifapi_cleanup_ifapi_object(&context->loadKey.auth_object);
            return TSS2_RC_SUCCESS;
        }
        break;

    statecasedefault(context->loadKey.state);
    }

error_cleanup:
    ifapi_free_object_list(context->loadKey.key_list);
    ifapi_cleanup_ifapi_object(context->loadKey.key_object);
    SAFE_FREE(context->loadKey.key_path);
    return r;
}

TSS2_RC
get_entities(IFAPI_KEYSTORE *keystore, char *dir_name, NODE_OBJECT_T **list, size_t *n)
{
    DIR *dir;
    struct dirent *entry;
    TSS2_RC r;
    char *path = NULL;
    NODE_OBJECT_T *second;

    if (!(dir = opendir(dir_name))) {
        return TSS2_RC_SUCCESS;
    }

    while ((entry = readdir(dir)) != NULL) {
        path = NULL;
        if (entry->d_type == DT_DIR) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            r = ifapi_asprintf(&path, "%s/%s", dir_name, entry->d_name);
            return_if_error(r, "Out of memory");

            LOG_TRACE("Directory: %s", path);
            r = get_entities(keystore, path, list, n);
            Fapi_Free(path);
            return_if_error(r, "get_entities");

        } else {
            size_t l_dir = strlen(dir_name);
            size_t l_user_dir = strlen(keystore->userdir);
            size_t l_system_dir = strlen(keystore->systemdir);
            if (dir_name[l_dir - 1] == IFAPI_FILE_DELIM_CHAR)
                l_dir -= 1;
            if (keystore->userdir[l_user_dir - 1] == IFAPI_FILE_DELIM_CHAR)
                l_user_dir -= 1;
            if (keystore->systemdir[l_system_dir - 1] == IFAPI_FILE_DELIM_CHAR)
                l_user_dir -= 1;
            if ((strncmp(dir_name, keystore->userdir, l_user_dir) == 0 && l_dir != l_user_dir) ||
                (strncmp(dir_name, keystore->systemdir, l_system_dir) == 0 && l_dir != l_system_dir)) {
                r = ifapi_asprintf(&path, "%s/%s", dir_name, entry->d_name);
                return_if_error(r, "Out of memory");

                NODE_OBJECT_T *file_obj = calloc(sizeof(NODE_OBJECT_T), 1);
                return_if_null(file_obj, "Out of memory.", TSS2_FAPI_RC_MEMORY);
                *n += 1;
                file_obj->object = strdup(path);
                if (file_obj->object == NULL) {
                    LOG_ERROR("%s", "Out of memory.");
                    SAFE_FREE(file_obj);
                    SAFE_FREE(path);
                    closedir(dir);
                    return TSS2_FAPI_RC_MEMORY;
                }
                if (*list != NULL) {
                    second = *list;
                    file_obj->next = second;
                }
                *list = file_obj;
                LOG_TRACE("File: %s", path);
                SAFE_FREE(path);
            }
        }
    }
    closedir(dir);
    return TSS2_RC_SUCCESS;
}

/** Get all object files from key store.
 *
 * @param[in] The search path in key store.
 * @param[out] The array of absolute path names.
 * @retval TSS2_RC_SUCCESS if computation of path array was successful.
 */
TSS2_RC
ifapi_get_entities(
    IFAPI_KEYSTORE *keystore,
    const char *searchPath,
    char ***pathlist,
    size_t *numPaths)
{
    TSS2_RC r;
    NODE_OBJECT_T *file_list = NULL;
    char *dir = keystore->systemdir;
    char *full_search_path = NULL;
    size_t n;
    NODE_OBJECT_T *head;
    char **pathlist2;
    char *expSearchPath = NULL;

    /* Get objects from system directory */
    if (searchPath && strcmp(searchPath,"") != 0 && strcmp(searchPath,"/") != 0) {
        size_t start_pos = 0;
        if (searchPath[0] == IFAPI_FILE_DELIM_CHAR)
            start_pos = 1;
        if ((strncmp(&searchPath[start_pos], "HS", 2) == 0 ||
             strncmp(&searchPath[start_pos], "HE", 2) == 0) &&
            strlen(&searchPath[start_pos]) <= 3) {
            /* Root directory is hierarchy */
            r = ifapi_asprintf(&expSearchPath, "%s/", keystore->defaultprofile,
                               searchPath[start_pos]);
            return_if_error(r, "Out of memory.");

        } else {
            /* Try to expand a key path */
            r = ifapi_expand_path(keystore, searchPath, &expSearchPath);
            return_if_error(r, "Out of memory.");
        }
    } else {
        /* get entities for the complete data store */
        expSearchPath = NULL;
    }
    r = ifapi_asprintf(&full_search_path, "%s%s%s", dir, IFAPI_FILE_DELIM,
                       expSearchPath?expSearchPath:"");
    return_if_error(r, "Out of memory.");


    *numPaths = 0;
    r = get_entities(keystore, full_search_path, &file_list, numPaths);
    goto_if_error(r, "get_entities", error_cleanup);

    /* Get objects from user directory if not equal system directory */
    if (strcmp(keystore->systemdir, keystore->userdir) != 0) {
        SAFE_FREE(full_search_path);
        dir = keystore->userdir;
        if (searchPath) {
            r = ifapi_asprintf(&full_search_path, "%s%s%s", dir, IFAPI_FILE_DELIM,
                               expSearchPath?expSearchPath:"");
            return_if_error(r, "Out of memory.");
        } else {
            /* get entities for the complete data store */
            strdup_check(full_search_path, dir, r, error_cleanup);
        }
        r = get_entities(keystore, full_search_path, &file_list, numPaths);
        goto_if_error(r, "get_entities", error_cleanup);
    }

    if (*numPaths > 0) {
        size_t size_path_list = *numPaths * sizeof(char *);
        pathlist2 = calloc(1, size_path_list);
        goto_if_null(pathlist2, "Out of memory.", TSS2_FAPI_RC_MEMORY,
                error_cleanup);
        n = *numPaths;

        /* Move file names from list to array */
        while (n > 0 && file_list) {
            n -= 1;
            pathlist2[n] = file_list->object;
            head = file_list;
            file_list = file_list->next;
            SAFE_FREE(head);
        }
        *pathlist = pathlist2;
        SAFE_FREE(full_search_path);
        SAFE_FREE(expSearchPath);
        return TSS2_RC_SUCCESS;
    }

error_cleanup:
    while (file_list) {
        head = file_list;
        file_list = file_list->next;
        SAFE_FREE(head->object);
        SAFE_FREE(head);
    }
    SAFE_FREE(expSearchPath);
    SAFE_FREE(full_search_path);
    return r;
}

size_t
get_name_alg(FAPI_CONTEXT *context, IFAPI_OBJECT *object)
{
    switch (object->objectType) {
    case IFAPI_KEY_OBJ:
        return object->misc.key.public.publicArea.nameAlg;
    case IFAPI_NV_OBJ:
        return object->misc.nv.public.nvPublic.nameAlg;
    case IFAPI_HIERARCHY_OBJ:
        return context->profiles.default_profile.nameAlg;
    default:
        return 0;
    }
}

/** Check whether policy session has to be flushed.
 *
 * Policy sessions with cleared continue session flag are not flushed in error
 * cases. Therefore the return code will be checked and if a policy session was
 * used the session will be flushed if the command was not executed successfully.
 *
 * @param[in,out] context for storing all state information.
 * @param[in] session the sessio to be checked wheter flush is needed.
 * @param[in] r The return code of the command using the session.
 */
void
ifapi_flush_policy_session(FAPI_CONTEXT *context, ESYS_TR session, TSS2_RC r)
{
    if (session != context->session1) {
        /* A policy session was used instead auf the default session. */
        if (r != TSS2_RC_SUCCESS) {
            Esys_FlushContext(context->esys, session);
        }
    }
}

/** State machine to authorize a key, a NV object of a hierarchy.
 *
 * @param[in,out] context for storing all state information.
 * @param[in] The FAPI object.
 * @param[out] The session which can be used for object authorization.
 * @retval TSS2_RC_SUCCESS If data can be read.
 * @retval TSS2_FAPI_RC_MEMORY: if not enough memory can be allocated.
 * @retval TSS2_FAPI_RC_BAD_VALUE If wrong values are detected during execution.
 * @retval TSS2_FAPI_RC_IO_ERROR If an error occurs during access to the policy
 *         store.
 * @retval TSS2_FAPI_RC_PATH_NOT_FOUND If a policy for a certain path was not found.
 * @retval TSS2_FAPI_RC_POLICY_UNKNOWN If policy search for a certain policy diges was
           not successful.
 * @retval TSS2_FAPI_RC_BAD_TEMPLATE In a invalid policy is loaded during execution.
 * @retval TPM2_RC_BAD_AUTH If the authentication for an object needed for policy
 *         execution fails.
 */
TSS2_RC
ifapi_authorize_object(FAPI_CONTEXT *context, IFAPI_OBJECT *object, ESYS_TR *session)
{
    TSS2_RC r;
    TPMI_YES_NO auth_required;

    LOG_DEBUG("Authorize object: %x", object->handle);
    switch (object->authorization_state) {
        statecase(object->authorization_state, AUTH_INIT)
            LOG_TRACE("**STATE** AUTH_INIT");

            if (!policy_digest_size(object)) {
                /* No policy used authorization callbacks have to be called if necessary. */
                if (object_with_auth(object)) {
                    r = ifapi_set_auth(context, object, "Authorize object");
                    return_if_error(r, "Set auth value");
                }
                /* No policy session needed current fapi session can be used */
                if (context->session1 && context->session1 != ESYS_TR_NONE)
                    *session = context->session1;
                else
                    /* Use password session if session1 had not been created */
                    *session = ESYS_TR_PASSWORD;
                break;
            }
            r = ifapi_policyutil_execute_prepare(context, get_name_alg(context, object)
                                                 ,object->policy_harness);
            return_if_error(r, "Prepare policy execution.");

            /* Next state will switch from prev context to next context. */
            context->policy.util_current_policy =  context->policy.util_current_policy->prev;
            object->authorization_state = AUTH_EXEC_POLICY;
            fallthrough;

        statecase(object->authorization_state, AUTH_EXEC_POLICY)
            *session = ESYS_TR_NONE;
            r = ifapi_policyutil_execute(context, session);
            if (r == TSS2_FAPI_RC_TRY_AGAIN)
                return r;

            return_if_error(r, "Execute policy.");

            r = Esys_TRSess_GetAuthRequired(context->esys, *session,
                                            &auth_required);
            return_if_error(r, "GetAuthRequired");

            /* Check whether PolicyCommand requiring authorization was executed */
            if (auth_required == TPM2_YES) {
                r = ifapi_set_auth(context, object, "Authorize object");
                goto_if_error(r, "Set auth value", error);
            }
            /* Clear continue session flag, so policy session will be flushed after authorization */
            r = Esys_TRSess_SetAttributes(context->esys, *session, 0, TPMA_SESSION_CONTINUESESSION);
            goto_if_error(r, "Esys_TRSess_SetAttributes", error);
            break;

        statecasedefault(object->authorization_state)
    }

    object->authorization_state = AUTH_INIT;
    return TSS2_RC_SUCCESS;

 error:
    /* No policy call was executed session can be flushed */
    Esys_FlushContext(context->esys, *session);
    return r;
}

/** State machine to write data to the TPM.
 *
 * Context nv_cmd has to be prepared:
 *
 * @param[in,out] context for storing all state information.
 * @param[in] nvPath The fapi path of the NV object.
 * @param[in] param_offset The offset in the NV memory (will be stored in context).
 * @param[in] data The pointer to the data to be written.
 * @param[in] size The number of bytes to be written.
 * @retval TSS2_RC_SUCCESS If data can be read.
 */
TSS2_RC
ifapi_nv_write(
    FAPI_CONTEXT *context,
    char         *nvPath,
    size_t         param_offset,
    uint8_t const *data,
    size_t         size)
{
    TSS2_RC r = TSS2_RC_SUCCESS;
    ESYS_TR auth_index;
    size_t data_idx = context->nv_cmd.data_idx;
    UINT16 bytesRequested = context->nv_cmd.bytesRequested;
    ESYS_TR  offset =  context->nv_cmd.offset;
    ESYS_TR nv_index =  context->nv_cmd.esys_handle;
    IFAPI_OBJECT *object = &context->nv_cmd.nv_object;
    IFAPI_OBJECT *auth_object = &context->nv_cmd.auth_object;
    TPM2B_MAX_NV_BUFFER *aux_data = (TPM2B_MAX_NV_BUFFER *)&context->aux_data;
    char *nv_file_name = NULL;
    ESYS_TR auth_session;

    switch (context->nv_cmd.nv_write_state) {
    statecase(context->nv_cmd.nv_write_state, NV2_WRITE_INIT);
        memset(&context->nv_cmd.nv_object, 0, sizeof(IFAPI_OBJECT));
        context->nv_cmd.nvPath = nvPath;
        context->nv_cmd.offset = param_offset;
        context->nv_cmd.numBytes = size;
        context->nv_cmd.data = data;
        if (context->nv_cmd.numBytes > context->nv_buffer_max)
            aux_data->size = context->nv_buffer_max;
        else
            aux_data->size = context->nv_cmd.numBytes;
        context->nv_cmd.data_idx = aux_data->size;

        /* Use calloc to ensure zero padding for write buffer. */
        context->nv_cmd.write_data = calloc(object->misc.nv.public.nvPublic.dataSize,
                                            1);
        goto_if_null2(context->nv_cmd.write_data, "Out of memory.", r,
                      TSS2_FAPI_RC_MEMORY,
                      error_cleanup);
        memcpy(context->nv_cmd.write_data, data,
               object->misc.nv.public.nvPublic.dataSize);
        memcpy(&aux_data->buffer[0], &context->nv_cmd.data[0], aux_data->size);

        r = ifapi_keystore_load_async(&context->keystore, &context->io,
                                      context->nv_cmd.nvPath);
        return_if_error2(r, "Could not open: %s", context->nv_cmd.nvPath);
        fallthrough;

    statecase(context->nv_cmd.nv_write_state, NV2_WRITE_READ);
        r = ifapi_keystore_load_finish(&context->keystore, &context->io, object);
        return_try_again(r);
        return_if_error(r, "read_finish failed");

        if (object->objectType != IFAPI_NV_OBJ)
            goto_error(r, TSS2_FAPI_RC_BAD_PATH, "%s is no NV object.", error_cleanup,
                       context->nv_cmd.nvPath);

        r = ifapi_initialize_object(context->esys, object);
        goto_if_error_reset_state(r, "Initialize NV object", error_cleanup);

        /* Store object info in context */
        nv_index = context->nv_cmd.nv_object.handle;
        context->nv_cmd.esys_handle = nv_index;
        context->nv_cmd.nv_obj = object->misc.nv;

        if (object->misc.nv.public.nvPublic.attributes & TPMA_NV_PPWRITE) {
            ifapi_init_hierarchy_object(auth_object, ESYS_TR_RH_PLATFORM);
            auth_index = ESYS_TR_RH_PLATFORM;
        } else {
            if (object->misc.nv.public.nvPublic.attributes & TPMA_NV_OWNERWRITE) {
                ifapi_init_hierarchy_object(auth_object, ESYS_TR_RH_OWNER);
                auth_index = ESYS_TR_RH_OWNER;
            } else {
                auth_index = nv_index;
            }
            *auth_object = *object;
        }
        context->nv_cmd.auth_index = auth_index;
        fallthrough;

    statecase(context->nv_cmd.nv_write_state, NV2_WRITE_AUTHORIZE);
        r = ifapi_authorize_object(context, auth_object, &auth_session);
        FAPI_SYNC(r, "Authorize NV object.", error_cleanup);

        r = Esys_NV_Write_Async(context->esys,
                                context->nv_cmd.auth_index,
                                nv_index,
                                auth_session,
                                context->session2,
                                ESYS_TR_NONE,
                                aux_data,
                                offset);
        goto_if_error_reset_state(r, " Fapi_NvWrite_Async", error_cleanup);

        if (!(object->misc.nv.public.nvPublic.attributes & TPMA_NV_NO_DA))
            context->nv_cmd.nv_write_state = NV2_WRITE_AUTH_SENT;
        else
            context->nv_cmd.nv_write_state =  NV2_WRITE_NULL_AUTH_SENT;

        context->nv_cmd.bytesRequested = aux_data->size;

        context->nv_cmd.offset = offset;
        fallthrough;

    case NV2_WRITE_AUTH_SENT:
    case NV2_WRITE_NULL_AUTH_SENT:
        r = Esys_NV_Write_Finish(context->esys);
        return_try_again(r);

        if ((r & ~TPM2_RC_N_MASK) == TPM2_RC_BAD_AUTH) {
            if (context->nv_cmd.nv_write_state ==  NV2_WRITE_NULL_AUTH_SENT) {
                IFAPI_OBJECT *auth_object = &context->nv_cmd.auth_object;
                r = ifapi_set_auth(context, auth_object, "NV Write");
                goto_if_error_reset_state(r, " Fapi_NvWrite_Finish", error_cleanup);

                r = Esys_NV_Write_Async(context->esys,
                                        context->nv_cmd.auth_index,
                                        nv_index,
                                        (!context->policy.session
                                         || context->policy.session == ESYS_TR_NONE) ? context->session1 :
                                        context->policy.session,
                                        context->session2,
                                        ESYS_TR_NONE,
                                        aux_data,
                                        offset);
                goto_if_error_reset_state(r, "FAPI NV_Write_Async", error_cleanup);

                context->nv_cmd.nv_write_state = NV2_WRITE_AUTH_SENT;
                return TSS2_FAPI_RC_TRY_AGAIN;
            }
        }
        goto_if_error_reset_state(r, "FAPI NV_Write_Finish", error_cleanup);

        context->nv_cmd.numBytes -= context->nv_cmd.bytesRequested;

        if (context->nv_cmd.numBytes > 0) {
            if (context->nv_cmd.numBytes > context->nv_buffer_max)
                aux_data->size = context->nv_buffer_max;
            else
                aux_data->size = context->nv_cmd.numBytes;
            memcpy(&aux_data->buffer[0], &context->nv_cmd.write_data[data_idx],
                   aux_data->size);
            offset += bytesRequested;
            r = Esys_NV_Write_Async(context->esys,
                                    context->nv_cmd.auth_index,
                                    nv_index,
                                    context->session1,
                                    context->session2,
                                    ESYS_TR_NONE,
                                    aux_data,
                                    offset);
            goto_if_error_reset_state(r, "FAPI NV_Write", error_cleanup);

            context->nv_cmd.bytesRequested = aux_data->size;
            //context->state =  NV_READ_AUTH_SENT;
            return TSS2_FAPI_RC_TRY_AGAIN;

        }
        fallthrough;

    statecase(context->nv_cmd.nv_write_state, NV2_WRITE_WRITE_PREPARE);
        /* Set written bit in keystore */
        context->nv_cmd.nv_object.misc.nv.public.nvPublic.attributes |= TPMA_NV_WRITTEN;
        /* Perform esys serialization if necessary */
        r = ifapi_esys_serialize_object(context->esys, &context->nv_cmd.nv_object);
        goto_if_error(r, "Prepare serialization", error_cleanup);

        /* Start writing the NV object to the key store */
        r = ifapi_keystore_store_async(&context->keystore, &context->io,
                                       context->nv_cmd.nvPath,
                                       &context->nv_cmd.nv_object);
        goto_if_error_reset_state(r, "Could not open: %sh", error_cleanup,
                                  context->nv_cmd.nvPath);
        context->nv_cmd.nv_write_state = NV2_WRITE_WRITE;
        fallthrough;

    statecase(context->nv_cmd.nv_write_state, NV2_WRITE_WRITE);
        /* Finish writing the NV object to the key store */
        r = ifapi_keystore_store_finish(&context->keystore, &context->io);
        return_try_again(r);
        return_if_error_reset_state(r, "write_finish failed");

        LOG_DEBUG("success");
        r = TSS2_RC_SUCCESS;

        context->nv_cmd.nv_write_state = NV2_WRITE_INIT;
        break;

    statecasedefault(context->nv_cmd.nv_write_state);
    }

error_cleanup:
    SAFE_FREE(nv_file_name);
    SAFE_FREE(context->nv_cmd.write_data);
    return r;
}

/** State machine to read data from TPM.
 *
 * Context nv_cmd has to be prepared:
 * - auth_index
 * - numBytes
 * - esys_handle
 * @param[in,out] context for storing all state information.
 * @param[out] data the data fetched from TPM.
 * @param[in,out] The number of bytes requested and fetched.
 * @retval TSS2_RC_SUCCESS If data can be read.
 */
TSS2_RC
ifapi_nv_read(
    FAPI_CONTEXT *context,
    uint8_t     **data,
    size_t       *size)
{
    TSS2_RC r;
    UINT16 aux_size;
    TPM2B_MAX_NV_BUFFER *aux_data;
    UINT16 bytesRequested = context->nv_cmd.bytesRequested;
    size_t numBytes = context->nv_cmd.numBytes;
    ESYS_TR nv_index =  context->nv_cmd.esys_handle;
    size_t data_idx = context->nv_cmd.data_idx;
    UINT16 offset = context->nv_cmd.offset;
    IFAPI_OBJECT *auth_object = &context->nv_cmd.auth_object;
    ESYS_TR session;

    switch (context->nv_cmd.nv_read_state) {
    statecase(context->nv_cmd.nv_read_state, NV_READ_INIT);
        LOG_TRACE("NV_READ_INIT");
        context->nv_cmd.rdata = NULL;
        fallthrough;

    statecase(context->nv_cmd.nv_read_state, NV_READ_AUTHORIZE);
        LOG_TRACE("NV_READ_AUTHORIZE");
        r = ifapi_authorize_object(context, auth_object, &session);
        FAPI_SYNC(r, "Authorize NV object.", error_cleanup);

        if (context->nv_cmd.numBytes > context->nv_buffer_max)
            aux_size = context->nv_buffer_max;
        else
            aux_size = context->nv_cmd.numBytes;
        r = Esys_NV_Read_Async(context->esys,
                               context->nv_cmd.auth_index,
                               nv_index,
                               session,
                               ESYS_TR_NONE,
                               ESYS_TR_NONE,
                               aux_size,
                               offset);
        goto_if_error_reset_state(r, " Fapi_NvRead_Async", error_cleanup);

        context->nv_cmd.nv_read_state = NV_READ_AUTH_SENT;
        context->nv_cmd.bytesRequested = aux_size;

        return TSS2_FAPI_RC_TRY_AGAIN;

    statecase(context->nv_cmd.nv_read_state, NV_READ_AUTH_SENT);
        LOG_TRACE("NV_READ_NULL_AUTH_SENT");
        if (context->nv_cmd.rdata == NULL) {
            LOG_TRACE("Allocate %zu bytes", context->nv_cmd.numBytes);
            context->nv_cmd.rdata = malloc(context->nv_cmd.numBytes);
        }
        *data = context->nv_cmd.rdata;
        goto_if_null(*data, "Malloc failed", TSS2_FAPI_RC_MEMORY, error_cleanup);

        r = Esys_NV_Read_Finish(context->esys, &aux_data);

        if ((r & ~TSS2_RC_LAYER_MASK) == TSS2_BASE_RC_TRY_AGAIN)
            return TSS2_FAPI_RC_TRY_AGAIN;

        goto_if_error_reset_state(r, "FAPI NV_Read_Finish", error_cleanup);

        if (aux_data->size < bytesRequested)
            numBytes = 0;
        else
            numBytes -= aux_data->size;
        memcpy(*data + data_idx, &aux_data->buffer[0], aux_data->size);
        data_idx += aux_data->size;
        free(aux_data);
        if (numBytes > 0) {
            if (numBytes > context->nv_buffer_max)
                aux_size = context->nv_buffer_max;
            else
                aux_size = numBytes;
            offset += bytesRequested;

            r = Esys_NV_Read_Async(context->esys,
                                   context->nv_cmd.auth_index,
                                   nv_index,
                                   context->session1,
                                   ESYS_TR_NONE,
                                   ESYS_TR_NONE,
                                   aux_size,
                                   offset);
            goto_if_error_reset_state(r, "FAPI NV_Read", error_cleanup);
            context->nv_cmd.bytesRequested = aux_size;
            context->nv_cmd.data_idx = data_idx;
            context->nv_cmd.numBytes = numBytes;
            context->nv_cmd.nv_read_state =  NV_READ_AUTH_SENT;
            return TSS2_FAPI_RC_TRY_AGAIN;
        } else {
            *size = data_idx;
            context->nv_cmd.nv_read_state = NV_READ_INIT;
            LOG_DEBUG("success");
            r = TSS2_RC_SUCCESS;
            break;
        }
    statecasedefault(context->nv_cmd.nv_read_state);
    }

error_cleanup:
    return r;
}

#define min(X,Y) (X>Y)?Y:X

/** State machine to retrieve random data from TPM.
 *
 * @param[in,out] context for storing all state information.
 * @param[in] numBytes Number of random bytes to be computed.
 * @param[out] data The random data.
 * @retval TSS2_RC_SUCCESS If random data can be computed.
 */
TSS2_RC
ifapi_get_random(FAPI_CONTEXT *context, size_t numBytes, uint8_t **data)
{
    TSS2_RC r;
    TPM2B_DIGEST *aux_data = NULL;

    switch (context->get_random_state) {
    statecase(context->get_random_state, GET_RANDOM_INIT);
        context->get_random.numBytes = numBytes;
        context->get_random.data = calloc(context->get_random.numBytes, 1);
        context->get_random.idx = 0;
        return_if_null(context->get_random.data, "FAPI out of memory.",
                       TSS2_FAPI_RC_MEMORY);

        r = Esys_GetRandom_Async(context->esys,
                                 context->session1,
                                 ESYS_TR_NONE, ESYS_TR_NONE,
                                 min(context->get_random.numBytes, sizeof(TPMU_HA)));
        goto_if_error_reset_state(r, "FAPI GetRandom", error_cleanup);
        fallthrough;

    statecase(context->get_random_state, GET_RANDOM_SENT);
        r = Esys_GetRandom_Finish(context->esys, &aux_data);
        return_try_again(r);
        goto_if_error_reset_state(r, "FAPI GetRandom_Finish", error_cleanup);

        if (aux_data -> size > context->get_random.numBytes) {
            goto_error(r, TSS2_FAPI_RC_BAD_VALUE, "TPM returned too many bytes",
                       error_cleanup);
        }

        memcpy(context->get_random.data + context->get_random.idx, &aux_data->buffer[0],
               aux_data->size);
        context->get_random.numBytes -= aux_data->size;
        context->get_random.idx += aux_data->size;
        Esys_Free(aux_data);

        if (context->get_random.numBytes > 0) {
            r = Esys_GetRandom_Async(context->esys, context->session1, ESYS_TR_NONE,
                                     ESYS_TR_NONE, min(context->get_random.numBytes, sizeof(TPMU_HA)));
            goto_if_error_reset_state(r, "FAPI GetRandom", error_cleanup);

            return TSS2_FAPI_RC_TRY_AGAIN;
        }
        break;

    statecasedefault(context->get_random_state);
    }

    *data = context->get_random.data;

    LOG_DEBUG("success");
    context->get_random_state = GET_RANDOM_INIT;
    return TSS2_RC_SUCCESS;

error_cleanup:
    context->get_random_state = GET_RANDOM_INIT;
    if (context->get_random.data != NULL)
        SAFE_FREE(context->get_random.data);
    return r;
}

/** Initialize the context for symmetric encryption decryption.
 *
 * @param[in,out] context The FAPI_CONTEXT.
 * @param[in] in_data The data to encrypt or decrypt, depending on
 *            the decrypt switch.
 * @param[in] size The size of the input data.
 * @param[in] decrypt if 0 encrypt input else decrypt input.
 * @retval TSS2_RC_SUCCESS on success.
 */
TSS2_RC
ifapi_sym_encrypt_decrypt_async(
    FAPI_CONTEXT *context,
    const uint8_t *in_data,
    size_t  size,
    TPMI_YES_NO decrypt)
{
//TODO: Get mode and scheme from crypto data
    context->cmd.Data_EncryptDecrypt.sym_mode = context->profiles.default_profile.sym_mode;
    context->cmd.Data_EncryptDecrypt.rsa_scheme =
        context->profiles.default_profile.rsa_decrypt_scheme;

    context->cmd.Data_EncryptDecrypt.in_data = in_data;
    context->cmd.Data_EncryptDecrypt.decrypt = decrypt;
    context->cmd.Data_EncryptDecrypt.numBytes = size;


    context->sym_encrypt_decrypt_state = ENCRYPT_DECRYPT_INIT;
    context->get_random_state = GET_RANDOM_INIT;

    return TSS2_RC_SUCCESS;
}

/** State machine for symmetric encryption& / decryption.
 *
 * @param[in,out] context for storing all state information.
 * @param[out] data the cipher text or plain text depending on decrypt switch.
 * @param[out] size the size of the output buffer.
 * @retval TSS2_RC_SUCCESS If encryption or decryption was successful.
 */
TSS2_RC
ifapi_sym_encrypt_decrypt_finish(
    FAPI_CONTEXT *context,
    uint8_t     **data,
    size_t       *size,
    TPMI_YES_NO decrypt)
{
    TSS2_RC r;
    TPM2B_MAX_BUFFER *aux_data = (TPM2B_MAX_BUFFER *)&context->aux_data;
    UINT16 bytesRequested = context->cmd.Data_EncryptDecrypt.bytesRequested;
    size_t numBytes = context->cmd.Data_EncryptDecrypt.numBytes;
    size_t data_idx = context->cmd.Data_EncryptDecrypt.data_idx;
    IFAPI_OBJECT *object = context->cmd.Data_EncryptDecrypt.key_object;
    TPMI_ALG_SYM_MODE mode = context->cmd.Data_EncryptDecrypt.sym_mode;
    TPM2B_IV *iv = &context->cmd.Data_EncryptDecrypt.iv;
    TPM2B_IV *tpm_iv;
    uint8_t *iv_rand = NULL;
    const uint8_t *in_data = context->cmd.Data_EncryptDecrypt.in_data;
    TPM2B_MAX_BUFFER *tpm_out_data;

    switch (context->sym_encrypt_decrypt_state) {
    statecase(context->sym_encrypt_decrypt_state, ENCRYPT_DECRYPT_INIT);
//TODO: Get mode and scheme from crypto data
        size_t iv_size = context->profiles.default_profile.sym_block_size;
        /* Received random number will  be encrypted! */
        r = Esys_TRSess_SetAttributes(context->esys, context->session1,
                                      TPMA_SESSION_ENCRYPT,
                                      TPMA_SESSION_ENCRYPT);
        goto_if_error_reset_state(r, "Set session attributes.", error_cleanup);

        r = ifapi_get_random(context, iv_size,  &iv_rand);

        if (r == TSS2_FAPI_RC_TRY_AGAIN)
            return r;

        goto_if_error_reset_state(r, " FAPI GetRandom", error_cleanup);

        iv->size = iv_size;
        memcpy(&context->cmd.Data_EncryptDecrypt.iv.buffer[0], iv_rand, iv_size);
        SAFE_FREE(iv_rand);

        if (context->cmd.Data_EncryptDecrypt.numBytes > context->nv_buffer_max)
            aux_data->size = context->nv_buffer_max;
        else
            aux_data->size = context->cmd.Data_EncryptDecrypt.numBytes;
        memcpy(&aux_data->buffer[0], &in_data[0], aux_data->size);
        context->cmd.Data_EncryptDecrypt.data_idx = 0;

        /* Authorization needed if NO_DA is not set */
        if (!(context->loadKey.auth_object.misc.key.public.publicArea.objectAttributes &
                TPMA_OBJECT_NODA)) {
            r = ifapi_set_auth(context, object, "Fapi_DataEncrypt/Decrypt");
            goto_if_error_reset_state(r, "Fapi_Encrypt/Decrypt", error_cleanup);
        }

        /* Transmitted plain text will not be encrypted! */
        r = Esys_TRSess_SetAttributes(context->esys, context->session1,
                                      TPMA_SESSION_CONTINUESESSION,
                                      0xff);
        goto_if_error_reset_state(r, "Set session attributes.", error_cleanup);

        for (int i = 0; i < 16; i++)
            iv->buffer[i] = i;
        r = Esys_EncryptDecrypt_Async(context->esys,
                                      object->handle,
                                      context->session1,
                                      ESYS_TR_NONE,
                                      ESYS_TR_NONE,
                                      decrypt,
                                      mode,
                                      iv,
                                      aux_data);
        goto_if_error_reset_state(r, " Fapi_Encrypt/Decrypt", error_cleanup);
        context->sym_encrypt_decrypt_state = ENCRYPT_DECRYPT_NULL_AUTH_SENT;

        return TSS2_FAPI_RC_TRY_AGAIN;

        /* This state is used below in an if statement. */
    case ENCRYPT_DECRYPT_NULL_AUTH_SENT:
    case ENCRYPT_DECRYPT_AUTH_SENT:
        LOG_TRACE("**STATE** ENCRYPT_DECRYPT_NULL_AUTH_SENT");

        /* Allocation of the output buffer */
        if (context->cmd.Data_EncryptDecrypt.out_data == NULL)
            context->cmd.Data_EncryptDecrypt.out_data =
                malloc(context->cmd.Data_EncryptDecrypt.numBytes);
        *data = context->cmd.Data_EncryptDecrypt.out_data;
        goto_if_null(*data, "Malloc failed", TSS2_FAPI_RC_MEMORY, error_cleanup);

        r = Esys_EncryptDecrypt_Finish(context->esys, &tpm_out_data, &tpm_iv);

        if ((r & ~TSS2_RC_LAYER_MASK) == TSS2_BASE_RC_TRY_AGAIN)
            return TSS2_FAPI_RC_TRY_AGAIN;
        if ((r & ~TPM2_RC_N_MASK) == TPM2_RC_BAD_AUTH) {
            if (context->sym_encrypt_decrypt_state == ENCRYPT_DECRYPT_NULL_AUTH_SENT) {
                r = ifapi_set_auth(context, object, "Fapi_Encrypt/Decrypt");
                goto_if_error_reset_state(r, " Fapi_NvRead", error_cleanup);

                r = Esys_EncryptDecrypt_Async(context->esys,
                                              object->handle,
                                              context->session1,
                                              ESYS_TR_NONE,
                                              ESYS_TR_NONE,
                                              decrypt,
                                              mode,
                                              iv,
                                              aux_data);
                goto_if_error_reset_state(r, "Fapi_Data/Encrypt/Decrypt", error_cleanup);

                context->sym_encrypt_decrypt_state = ENCRYPT_DECRYPT_AUTH_SENT ;
                return TSS2_FAPI_RC_TRY_AGAIN;
            }
        }
        goto_if_error_reset_state(r, "FAPI Data_EncryptDecrypt", error_cleanup);

        iv->size = tpm_iv->size;
        memcpy(&iv->buffer[0], &tpm_iv->buffer[0], tpm_iv->size);
        free(tpm_iv);

        if (tpm_out_data->size < bytesRequested) {
            goto_error(r, TSS2_FAPI_RC_GENERAL_FAILURE, "Wrong encryption/decryption size",
                       error_cleanup);

        } else {
            numBytes -= tpm_out_data->size;
        }
        memcpy(*data + data_idx, &tpm_out_data->buffer[0], tpm_out_data->size);
        data_idx += aux_data->size;
        free(tpm_out_data);
        if (numBytes > 0) {
            if (numBytes > context->nv_buffer_max)
                aux_data->size = context->nv_buffer_max;
            else
                aux_data->size = numBytes;
            memcpy(&aux_data->buffer[0], &in_data[data_idx], aux_data->size);
            r = Esys_EncryptDecrypt_Async(context->esys,
                                          object->handle,
                                          context->session1,
                                          ESYS_TR_NONE,
                                          ESYS_TR_NONE,
                                          decrypt,
                                          mode,
                                          iv,
                                          aux_data);
            goto_if_error_reset_state(r, "FAPI NV_Read", error_cleanup);
            context->cmd.Data_EncryptDecrypt.bytesRequested = aux_data->size;
            context->cmd.Data_EncryptDecrypt.data_idx = data_idx;
            context->cmd.Data_EncryptDecrypt.numBytes = numBytes;
            context->sym_encrypt_decrypt_state =  ENCRYPT_DECRYPT_AUTH_SENT;
            return TSS2_FAPI_RC_TRY_AGAIN;

        } else {
            *size = data_idx;
            IFAPI_ENCRYPTED_DATA *enc_data = &context->cmd.Data_EncryptDecrypt.enc_data;
            enc_data->type = IFAPI_SYM_BULK_ENCRYPTION;
            enc_data->cipher.size = context->cmd.Data_EncryptDecrypt.in_dataSize;
            enc_data->cipher.buffer = context->cmd.Data_EncryptDecrypt.out_data;
            r = ifapi_get_name(&context->loadKey.auth_object.misc.key.public.publicArea,
                               &enc_data->key_name);
            goto_if_error(r, "Compute key name.", error_cleanup);

            LOG_DEBUG("success");
            r = TSS2_RC_SUCCESS;
            break;
        }
    statecasedefault(context->sym_encrypt_decrypt_state);
    }
    return r;

error_cleanup:
    return r;
}

/** Load a key and initialize profile and session for ESAPI commands
 */
TSS2_RC
ifapi_load_key(
    FAPI_CONTEXT  *context,
    char    const *keyPath,
    IFAPI_OBJECT **key_object)
{
    TSS2_RC r;
    const IFAPI_PROFILE *profile;

    return_if_null(keyPath, "Bad reference for key path.",
                   TSS2_FAPI_RC_BAD_REFERENCE);

    switch (context->Key_Sign.state) {
    statecase(context->Key_Sign.state, SIGN_INIT);
        context->Key_Sign.keyPath = keyPath;

        r = ifapi_get_sessions_async(context,
                                     IFAPI_SESSION_GENEK | IFAPI_SESSION1,
                                     TPMA_SESSION_DECRYPT, 0);
        goto_if_error_reset_state(r, "Create sessions", error_cleanup);
        fallthrough;

    statecase(context->Key_Sign.state, SIGN_WAIT_FOR_SESSION);
        r = ifapi_profiles_get(&context->profiles, context->Key_Sign.keyPath, &profile);
        goto_if_error_reset_state(r, "Reading profile data", error_cleanup);

        r = ifapi_get_sessions_finish(context, profile);
        return_try_again(r);
        goto_if_error_reset_state(r, " FAPI create session", error_cleanup);

        r = ifapi_load_keys_async(context, context->Key_Sign.keyPath);
        goto_if_error(r, "Load keys.", error_cleanup);
        fallthrough;

    statecase(context->Key_Sign.state, SIGN_WAIT_FOR_KEY);
        r = ifapi_load_keys_finish(context, IFAPI_FLUSH_PARENT,
                                   &context->Key_Sign.handle,
                                   key_object);
        return_try_again(r);
        goto_if_error_reset_state(r, " Load key.", error_cleanup);

        context->Key_Sign.state = SIGN_INIT;
        break;

    statecasedefault(context->Key_Sign.state);
        context->state = _FAPI_STATE_INTERNALERROR;
        return_error(TSS2_FAPI_RC_BAD_VALUE, "Invalid state for FAPI load key");
    }

error_cleanup:
    return r;
}

TSS2_RC
ifapi_key_sign(
    FAPI_CONTEXT     *context,
    IFAPI_OBJECT     *sig_key_object,
    char const       *padding,
    TPM2B_DIGEST     *digest,
    TPMT_SIGNATURE  **tpm_signature,
    char            **publicKey,
    char            **certificate)
{
    TSS2_RC r;
    TPMT_SIG_SCHEME *sig_scheme;
    ESYS_TR session;

    TPMT_TK_HASHCHECK hash_validation = {
        .tag = TPM2_ST_HASHCHECK,
        .hierarchy = TPM2_RH_OWNER,
    };
    memset(&hash_validation.digest, 0, sizeof(TPM2B_DIGEST));

    switch (context->Key_Sign.state) {
    statecase(context->Key_Sign.state, SIGN_INIT);
        sig_key_object = context->Key_Sign.key_object;

        r = ifapi_authorize_object(context, sig_key_object, &session);
        FAPI_SYNC(r, "Authorize signature key.", cleanup);

        context->policy.session = session;

        r = ifapi_get_sig_scheme(context, sig_key_object, padding, digest, &sig_scheme);
        goto_if_error(r, "Get signature scheme", cleanup);

        r = Esys_Sign_Async(context->esys,
                            context->Key_Sign.handle,
                            session,
                            ESYS_TR_NONE, ESYS_TR_NONE,
                            digest,
                            sig_scheme,
                            &hash_validation);
        goto_if_error(r, "Error: Sign", cleanup);
        fallthrough;

    statecase(context->Key_Sign.state, SIGN_AUTH_SENT);
        context->Key_Sign.signature = NULL;
        r = Esys_Sign_Finish(context->esys,
                             &context->Key_Sign.signature);
        return_try_again(r);
        ifapi_flush_policy_session(context, context->policy.session, r);
        goto_if_error(r, "Error: Sign", cleanup);

        r = Esys_FlushContext_Async(context->esys, context->Key_Sign.handle);
        goto_if_error(r, "Error: FlushContext", cleanup);
        fallthrough;

    statecase(context->Key_Sign.state, SIGN_WAIT_FOR_FLUSH);
        r = Esys_FlushContext_Finish(context->esys);
        return_try_again(r);
        goto_if_error(r, "Error: Sign", cleanup);

        int pem_size;
        if (publicKey) {
            r = ifapi_pub_pem_key_from_tpm(&sig_key_object->misc.key.public,
                                                publicKey,
                                                &pem_size);
            goto_if_error(r, "Conversion pub key to PEM failed", cleanup);
        }
        context->Key_Sign.handle = ESYS_TR_NONE;
        *tpm_signature = context->Key_Sign.signature;
        if (certificate) {
            *certificate = strdup(context->Key_Sign.key_object->misc.key.certificate);
            goto_if_null(*certificate, "Out of memory.",
                    TSS2_FAPI_RC_MEMORY, cleanup);
        }
        context->Key_Sign.state = SIGN_INIT;
        LOG_TRACE("success");
        r = TSS2_RC_SUCCESS;
        break;

    statecasedefault(context->Key_Sign.state);
    }

cleanup:
    if (context->Key_Sign.handle != ESYS_TR_NONE)
        Esys_FlushContext(context->esys, context->Key_Sign.handle);
    ifapi_cleanup_ifapi_object(context->Key_Sign.key_object);
    return r;
}

/** Get json encoding for FAPI object
 */
TSS2_RC
ifapi_get_json(FAPI_CONTEXT *context, IFAPI_OBJECT *object, char **json_string)
{
    TSS2_RC r = TSS2_RC_SUCCESS;
    json_object *jso = NULL;

    /* Perform esys serialization if necessary */
    r = ifapi_esys_serialize_object(context->esys, object);
    goto_if_error(r, "Prepare serialization", cleanup);

    r = ifapi_json_IFAPI_OBJECT_serialize(object, &jso);
    return_if_error(r, "Serialize duplication object");

    *json_string = strdup(json_object_to_json_string_ext(jso,
                          JSON_C_TO_STRING_PRETTY));
    goto_if_null2(*json_string, "Converting json to string", r, TSS2_FAPI_RC_MEMORY,
                  cleanup);

cleanup:
    if (jso)
        json_object_put(jso);
    return r;
}

/** Serialize persistent objects into buffer of keystore object.
 *
 * NV objects and persistent keys will serialized via the ESYS API to
 * enable reconstruction durinng loading from keystore.
 *
 * @param[object] object  The nv object or the key.
 * @param[out] jso pointer to the json object.
 * @retval TSS2_RC_SUCCESS if the function call was a success.
 * @retval ESYS error code if the serialization fails.
 */
TSS2_RC
ifapi_esys_serialize_object(ESYS_CONTEXT *ectx, IFAPI_OBJECT *object)
{
    TSS2_RC r = TSS2_RC_SUCCESS;
    IFAPI_KEY *key_object = NULL;
    IFAPI_NV *nv_object;

    switch (object->objectType) {
    case IFAPI_NV_OBJ:
        nv_object = &object->misc.nv;
        if (nv_object->serialization.buffer != NULL) {
            Fapi_Free(nv_object->serialization.buffer);
            nv_object->serialization.buffer = NULL;
        }
        r = Esys_TR_Serialize(ectx, object->handle,
                              &nv_object->serialization.buffer,
                              &nv_object->serialization.size);
        return_if_error(r, "Error serialize esys object");
        break;

    case IFAPI_KEY_OBJ:
        key_object = &object->misc.key;
        key_object->serialization.size = 0;
        if (key_object->serialization.buffer != NULL) {
            Fapi_Free(key_object->serialization.buffer);
            key_object->serialization.buffer = NULL;
        }
        if (object->handle != ESYS_TR_NONE && key_object->persistent_handle) {
            key_object->serialization.buffer = NULL;
            r = Esys_TR_Serialize(ectx, object->handle,
                                  &key_object->serialization.buffer,
                                  &key_object->serialization.size);
            return_if_error(r, "Error serialize esys object");
        }
        break;

    default:
        /* Nothing to be done */
        break;
    }
    return TSS2_RC_SUCCESS;
}

 /** Initialize the part of an IFAPI_OBJECT  which is not serialized.
 *
 * For persistent objects the correspodning ESYS object will be created.
 *
 * @param[inout] ectx The ESYS context.
 * @param[in]  jso the json object to be deserialized.
 * @param[out] out the deserialzed binary object.
 * @retval TSS2_RC_SUCCESS if the function call was a success.
 * @retval TSS2_FAPI_RC_BAD_VALUE if the json object can't be deserialized.
 */
TSS2_RC
ifapi_initialize_object(
    ESYS_CONTEXT *ectx,
    IFAPI_OBJECT *object)
{
    TSS2_RC r = TSS2_RC_SUCCESS;
    ESYS_TR handle;

    switch (object->objectType) {
    case IFAPI_NV_OBJ:
        if (object->misc.nv.serialization.size > 0) {
            r = Esys_TR_Deserialize(ectx, &object->misc.nv.serialization.buffer[0],
                                    object->misc.nv.serialization.size, &handle);
            goto_if_error(r, "Error dserialize esys object", cleanup);
        } else {
            handle = ESYS_TR_NONE;
        }
        object->authorization_state = AUTH_INIT;
        object->handle = handle;
        break;

    case IFAPI_KEY_OBJ:
        if (object->misc.key.serialization.size > 0) {
            r = Esys_TR_Deserialize(ectx, &object->misc.key.serialization.buffer[0],
                                    object->misc.key.serialization.size, &handle);
            goto_if_error(r, "Error deserialize esys object", cleanup);
        } else {
            handle = ESYS_TR_NONE;
        }
        object->authorization_state = AUTH_INIT;
        object->handle = handle;
        break;

    default:
        /* Nothing to be done */
        break;
    }

    return r;

cleanup:
    SAFE_FREE(object->policy_harness);
    return r;
}

/** Load a key and initialize profile and session for ESAPI commands
 */
TSS2_RC
ifapi_key_create_prepare_auth(
    FAPI_CONTEXT  *context,
    char   const *keyPath,
    char   const *policyPath,
    char   const *authValue)
{
    TSS2_RC r;

    memset(&context->cmd.Key_Create.inSensitive, 0, sizeof(TPM2B_SENSITIVE_CREATE));
    if (authValue) {
        if (strlen(authValue) > sizeof(TPMU_HA)) {
            return_error(TSS2_FAPI_RC_BAD_VALUE, "Password too long.");
        }
        memcpy(&context->cmd.Key_Create.inSensitive.sensitive.userAuth.buffer,
               authValue, strlen(authValue));
        context->cmd.Key_Create.inSensitive.sensitive.userAuth.size = strlen(authValue);
    }
    context->cmd.Key_Create.inSensitive.sensitive.data.size = 0;
    r = ifapi_key_create_prepare(context, keyPath, policyPath);
    return r;
}

TSS2_RC
ifapi_key_create_prepare_sensitive(
    FAPI_CONTEXT  *context,
    char    const *keyPath,
    char    const *policyPath,
    size_t         dataSize,
    char    const *authValue,
    uint8_t const *data)
{
    TSS2_RC r;

    memset(&context->cmd.Key_Create.inSensitive, 0, sizeof(TPM2B_SENSITIVE_CREATE));
    if (dataSize > sizeof(TPMU_HA) || dataSize == 0) {
        return_error(TSS2_FAPI_RC_BAD_VALUE, "Data to big or equal zero.");
    }
    if (data)
        memcpy(&context->cmd.Key_Create.inSensitive.sensitive.data.buffer,
               data, dataSize);
    context->cmd.Key_Create.inSensitive.sensitive.data.size = dataSize;
    if (authValue) {
        if (strlen(authValue) > sizeof(TPMU_HA)) {
            return_error(TSS2_FAPI_RC_BAD_VALUE, "Password too long.");
        }
        memcpy(&context->cmd.Key_Create.inSensitive.sensitive.userAuth.buffer,
               authValue, strlen(authValue));
        context->cmd.Key_Create.inSensitive.sensitive.userAuth.size = strlen(authValue);
    }
    r = ifapi_key_create_prepare(context, keyPath, policyPath);
    return r;
}

/** Load a key and initialize profile and session for ESAPI commands
 */
TSS2_RC
ifapi_key_create_prepare(
    FAPI_CONTEXT  *context,
    char   const *keyPath,
    char   const *policyPath)
{
    TSS2_RC r;
    IFAPI_OBJECT *object = &context->cmd.Key_Create.object;
    NODE_STR_T *path_list = NULL;

    LOG_TRACE("call");
    r = ifapi_session_init(context);
    return_if_error(r, "Initialize Key_Create");

    /* First check whether an existing object would be overwritten */
    r = ifapi_keystore_check_overwrite(&context->keystore, &context->io,
                                       keyPath);
    return_if_error2(r, "Check overwrite %s", keyPath);

    context->srk_handle = 0;

    /* Clear the memory used for the the new key object */
    memset(&context->cmd.Key_Create.outsideInfo, 0, sizeof(TPM2B_DATA));
    memset(&context->cmd.Key_Create.creationPCR, 0, sizeof(TPML_PCR_SELECTION));
    memset(object, 0, sizeof(IFAPI_OBJECT));

    strdup_check(context->cmd.Key_Create.policyPath, policyPath, r, error);
    strdup_check(context->cmd.Key_Create.keyPath, keyPath, r, error);
    r = get_explicit_key_path(&context->keystore, keyPath, &path_list);
    return_if_error(r, "Compute explicit path.");

    context->loadKey.path_list = path_list;
    char *file;
    r = ifapi_path_string(&file, NULL, path_list, NULL);
    goto_if_error(r, "Compute explicit path.", error);

    LOG_DEBUG("Explicit key path: %s", file);

    free(file);

    context->cmd.Key_Create.state = KEY_CREATE_INIT;

    return TSS2_RC_SUCCESS;

error:
    free_string_list(path_list);
    return r;
}

TSS2_RC
ifapi_key_create(
    FAPI_CONTEXT *context,
    IFAPI_KEY_TEMPLATE *template)
{
    TSS2_RC r;
    size_t path_length;
    NODE_STR_T *path_list = context->loadKey.path_list;
    TPM2B_PUBLIC *outPublic = NULL;
    TPM2B_PRIVATE *outPrivate = NULL;
    TPM2B_CREATION_DATA *creationData = NULL;
    TPM2B_DIGEST *creationHash = NULL;
    TPMT_TK_CREATION *creationTicket = NULL;
    IFAPI_OBJECT *object = &context->cmd.Key_Create.object;
    ESYS_TR auth_session;

    LOG_TRACE("call");

    switch (context->cmd.Key_Create.state) {
    statecase(context->cmd.Key_Create.state, KEY_CREATE_INIT);
        context->cmd.Key_Create.public_templ = *template;

        /* Profile name is first element of the explicit path list */
        char *profile_name = context->loadKey.path_list->str;
        r = ifapi_profiles_get(&context->profiles, profile_name, &context->cmd.Key_Create.profile);
        goto_if_error_reset_state(r, "Retrieving profile data", error_cleanup);

        if (context->cmd.Key_Create.inSensitive.sensitive.data.size > 0) {
            /* A keyed hash object sealing sensitive data will be created */
            context->cmd.Key_Create.public_templ.public.publicArea.type = TPM2_ALG_KEYEDHASH;
            context->cmd.Key_Create.public_templ.public.publicArea.nameAlg =
                    context->cmd.Key_Create.profile->nameAlg;
            context->cmd.Key_Create.public_templ.public.publicArea.parameters.keyedHashDetail.scheme.scheme =
            TPM2_ALG_NULL;
        } else {
            r = ifapi_merge_profile_into_template(context->cmd.Key_Create.profile,
                                                  &context->cmd.Key_Create.public_templ);
            goto_if_error_reset_state(r, "Merge profile", error_cleanup);
        }

        if (context->cmd.Key_Create.policyPath
                && strcmp(context->cmd.Key_Create.policyPath, "") != 0)
            context->cmd.Key_Create.state = KEY_CREATE_CALCULATE_POLICY;
        /* else jump over to KEY_CREATE_WAIT_FOR_SESSION below */
    /* FALLTHRU */

    case KEY_CREATE_CALCULATE_POLICY:
        if (context->cmd.Key_Create.state == KEY_CREATE_CALCULATE_POLICY) {
            r = ifapi_calculate_tree(context, context->cmd.Key_Create.policyPath,
                                     &context->policy.harness,
                                     context->cmd.Key_Create.public_templ.public.publicArea.nameAlg,
                                     &context->policy.digest_idx,
                                     &context->policy.hash_size);
            return_try_again(r);
            goto_if_error2(r, "Calculate policy tree %s", error_cleanup,
                           context->cmd.Key_Create.policyPath);

            /* Store the calculated policy in the key object */
            object->policy_harness = calloc(1, sizeof(TPMS_POLICY_HARNESS));
            return_if_null(object->policy_harness, "Out of memory",
                    TSS2_FAPI_RC_MEMORY);
            *(object->policy_harness) = context->policy.harness;

            context->cmd.Key_Create.public_templ.public.publicArea.authPolicy.size =
                context->policy.hash_size;
            memcpy(&context->cmd.Key_Create.public_templ.public.publicArea.authPolicy.buffer[0],
                   &context->policy.harness.policyDigests.digests[context->policy.digest_idx].digest,
                   context->policy.hash_size);
        }
        r = ifapi_get_sessions_async(context,
                                     IFAPI_SESSION_GENEK | IFAPI_SESSION1,
                                     TPMA_SESSION_DECRYPT, 0);
        goto_if_error_reset_state(r, "Create sessions", error_cleanup);
        fallthrough;

    statecase(context->cmd.Key_Create.state, KEY_CREATE_WAIT_FOR_SESSION);
        LOG_TRACE("KEY_CREATE_WAIT_FOR_SESSION");
        r = ifapi_get_sessions_finish(context, context->cmd.Key_Create.profile);
        return_try_again(r);
        goto_if_error_reset_state(r, " FAPI create session", error_cleanup);

        path_length = ifapi_path_length(path_list);
        r = ifapi_load_key_async(context, path_length - 1);
        goto_if_error(r, "LoadKey async", error_cleanup);
        fallthrough;

    statecase(context->cmd.Key_Create.state, KEY_CREATE_WAIT_FOR_PARENT);
        LOG_TRACE("KEY_CREATE_WAIT_FOR_PARENT");
        r = ifapi_load_key_finish(context, IFAPI_FLUSH_PARENT);
        return_try_again(r);
        goto_if_error(r, "LoadKey finish", error_cleanup);
        fallthrough;

    statecase(context->cmd.Key_Create.state, KEY_CREATE_WAIT_FOR_AUTHORIZATION);
        r = ifapi_authorize_object(context, &context->loadKey.auth_object, &auth_session);
        FAPI_SYNC(r, "Authorize key.", error_cleanup);

        r = Esys_Create_Async(context->esys, context->loadKey.auth_object.handle,
                              auth_session,
                              ESYS_TR_NONE, ESYS_TR_NONE,
                              &context->cmd.Key_Create.inSensitive,
                              &context->cmd.Key_Create.public_templ.public,
                              &context->cmd.Key_Create.outsideInfo,
                              &context->cmd.Key_Create.creationPCR);
        goto_if_error(r, "Create_Async", error_cleanup);
        fallthrough;

    statecase(context->cmd.Key_Create.state, KEY_CREATE_AUTH_SENT);
        r = Esys_Create_Finish(context->esys, &outPrivate, &outPublic, &creationData,
                               &creationHash, &creationTicket);
        try_again_or_error_goto(r, "Key create finish", error_cleanup);

        /* Prepare object for serialization */
        object->system = context->cmd.Key_Create.public_templ.system;
        object->objectType = IFAPI_KEY_OBJ;
        object->misc.key.public = *outPublic;
        object->misc.key.private.size = outPrivate->size;
        object->misc.key.private.buffer = calloc(1, outPrivate->size);
        goto_if_null2( object->misc.key.private.buffer, "Out of memory.", r,
                       TSS2_FAPI_RC_MEMORY, error_cleanup);

        object->misc.key.private.buffer = memcpy(&object->misc.key.private.buffer[0],
                                                 &outPrivate->buffer[0], outPrivate->size);
        object->misc.key.policyInstance = NULL;
        object->misc.key.creationData = *creationData;
        object->misc.key.creationTicket = *creationTicket;
        object->misc.key.description = NULL;
        object->misc.key.certificate = NULL;
        SAFE_FREE(outPrivate);
        SAFE_FREE(creationData);
        SAFE_FREE(creationTicket);
        SAFE_FREE(creationHash);
        if (context->cmd.Key_Create.inSensitive.sensitive.userAuth.size > 0)
            object->misc.key.with_auth = TPM2_YES;
        else
            object->misc.key.with_auth = TPM2_NO;;
        r = ifapi_get_name(&outPublic->publicArea, &object->misc.key.name);
        goto_if_error(r, "Get key name", error_cleanup);

        if (object->misc.key.public.publicArea.type == TPM2_ALG_RSA)
            object->misc.key.signing_scheme = context->cmd.Key_Create.profile->rsa_signing_scheme;
        else
            object->misc.key.signing_scheme = context->cmd.Key_Create.profile->ecc_signing_scheme;
        SAFE_FREE(outPublic);
        fallthrough;

    statecase(context->cmd.Key_Create.state, KEY_CREATE_WRITE_PREPARE);
        /* Perform esys serialization if necessary */
        r = ifapi_esys_serialize_object(context->esys, object);
        goto_if_error(r, "Prepare serialization", error_cleanup);

        /* Start writing the NV object to the key store */
        r = ifapi_keystore_store_async(&context->keystore, &context->io,
                                       context->cmd.Key_Create.keyPath, object);
        goto_if_error_reset_state(r, "Could not open: %sh", error_cleanup,
                                  context->cmd.Key_Create.keyPath);
        ifapi_cleanup_ifapi_object(object);
        fallthrough;

    statecase(context->cmd.Key_Create.state, KEY_CREATE_WRITE);
        /* Finish writing the key to the key store */
        r = ifapi_keystore_store_finish(&context->keystore, &context->io);
        return_try_again(r);
        return_if_error_reset_state(r, "write_finish failed");

        if (context->loadKey.auth_object.misc.key.persistent_handle) {
            context->cmd.Key_Create.state = KEY_CREATE_INIT;
            r = TSS2_RC_SUCCESS;
            break;
        }
        r = Esys_FlushContext_Async(context->esys, context->loadKey.auth_object.handle);
        goto_if_error(r, "Flush parent", error_cleanup);
        fallthrough;

    statecase(context->cmd.Key_Create.state, KEY_CREATE_FLUSH);
        r = Esys_FlushContext_Finish(context->esys);
        try_again_or_error_goto(r, "Flush context", error_cleanup);
        fallthrough;

    statecase(context->cmd.Key_Create.state, KEY_CREATE_CLEANUP);
        r = ifapi_cleanup_session(context);
        try_again_or_error_goto(r, "Cleanup", error_cleanup);

        context->cmd.Key_Create.state = KEY_CREATE_INIT;
        r = TSS2_RC_SUCCESS;
        break;

    statecasedefault(context->cmd.Key_Create.state);
    }
error_cleanup:
    free_string_list(context->loadKey.path_list);
    SAFE_FREE(outPublic);
    SAFE_FREE(outPrivate);
    SAFE_FREE(creationData);
    SAFE_FREE(creationHash);
    SAFE_FREE(creationTicket);
    SAFE_FREE(context->cmd.Key_Create.policyPath);
    SAFE_FREE(context->cmd.Key_Create.keyPath);
    ifapi_cleanup_ifapi_object(object);
    ifapi_session_clean(context);
    return r;
}

/** Get signature scheme for object of if padding compute scheme from padding.
 */
TSS2_RC
ifapi_get_sig_scheme(
    FAPI_CONTEXT *context,
    IFAPI_OBJECT *object,
    char const *padding,
    TPM2B_DIGEST *digest,
    TPMT_SIG_SCHEME **sig_scheme)
{
    TPMI_ALG_HASH hash_alg;
    TSS2_RC r;

    if (padding) {
        /* Get hash algorithm from digest size */
        r = ifapi_get_hash_alg_for_size(digest->size, &hash_alg);
        return_if_error2(r, "Invalid digest size.");

        /* Use scheme object from context */
        if (strcasecmp("RSA_SSA", padding) == 0) {
            context->Key_Sign.scheme.scheme = TPM2_ALG_RSASSA;
            context->Key_Sign.scheme.details.rsassa.hashAlg = hash_alg;
        }
        if (strcasecmp("RSA_PSS", padding) == 0) {
            context->Key_Sign.scheme.scheme = TPM2_ALG_RSAPSS;
            context->Key_Sign.scheme.details.rsapss.hashAlg = hash_alg;
        }
        *sig_scheme =  &context->Key_Sign.scheme;
        return TSS2_RC_SUCCESS;
    } else {
        /* Use scheme defined for object */
        *sig_scheme =  &object->misc.key.signing_scheme;
        return TSS2_RC_SUCCESS;
    }
}

/** State machine for changing the hierarchy authorization
 */
TSS2_RC
ifapi_change_auth_hierarchy(
    FAPI_CONTEXT *context,
    ESYS_TR handle,
    IFAPI_OBJECT *hierarchy_object,
    TPM2B_AUTH *newAuthValue)
{
    TSS2_RC r;

    switch (context->hierarchy_state) {
    statecase(context->hierarchy_state, HIERARCHY_CHANGE_AUTH_INIT);
        if (newAuthValue->size>0)
            hierarchy_object->misc.hierarchy.with_auth = TPM2_YES;
        else
            hierarchy_object->misc.hierarchy.with_auth = TPM2_NO;
        r = Esys_HierarchyChangeAuth_Async(context->esys,
                                           handle,
                                           (context->session1
                                            && context->session1 != ESYS_TR_NONE) ?
                                           context->session1 : ESYS_TR_PASSWORD,
                                           ESYS_TR_NONE, ESYS_TR_NONE,
                                           newAuthValue);
        return_if_error(r, "HierarchyChangeAuth");
        fallthrough;

    statecase(context->hierarchy_state, HIERARCHY_CHANGE_AUTH_NULL_AUTH_SENT);
        r = Esys_HierarchyChangeAuth_Finish(context->esys);
        return_try_again(r);

        if ((r & ~TPM2_RC_N_MASK) != TPM2_RC_BAD_AUTH) {
            return_if_error(r, "Hierarchy change auth.");
            context->hierarchy_state = HIERARCHY_CHANGE_AUTH_INIT;
            LOG_TRACE("success");
            return TSS2_RC_SUCCESS;
        }

        /* Retry after NULL authorization was not successful */
        r = ifapi_set_auth(context, hierarchy_object, "Hierarchy object");
        return_if_error(r, "HierarchyChangeAuth");

        r = Esys_HierarchyChangeAuth_Async(context->esys,
                                           handle,
                                           (context->session1
                                            && context->session1 != ESYS_TR_NONE) ?
                                           context->session1 : ESYS_TR_PASSWORD,
                                           ESYS_TR_NONE, ESYS_TR_NONE,
                                           newAuthValue);
        return_if_error(r, "HierarchyChangeAuth");
        fallthrough;

    statecase(context->hierarchy_state, HIERARCHY_CHANGE_AUTH_AUTH_SENT);
        r = Esys_HierarchyChangeAuth_Finish(context->esys);
        FAPI_SYNC(r, "Hierarchy change auth.", error);

        context->hierarchy_state = HIERARCHY_CHANGE_AUTH_INIT;
        return r;

    statecasedefault(context->hierarchy_state);
    }

error:
    return r;
}

TSS2_RC
ifapi_change_policy_hierarchy(
    FAPI_CONTEXT *context,
    ESYS_TR handle,
    IFAPI_OBJECT *hierarchy_object,
    TPMS_POLICY_HARNESS *policy_harness)
{
    TSS2_RC r;

    switch (context->hierarchy_policy_state) {
    statecase(context->hierarchy_policy_state, HIERARCHY_CHANGE_POLICY_INIT);
        if (! policy_harness || ! policy_harness->policy) {
            /* No policy will be used for hierarchy */
            return TSS2_RC_SUCCESS;
        }

        context->policy.state = POLICY_CALCULATE;

        r = ifapi_calculate_tree(context, NULL, /**< no path needed */
                                 policy_harness,
                                 context->profiles.default_profile.nameAlg,
                                 &context->cmd.Provision.digest_idx,
                                 &context->cmd.Provision.hash_size);
        goto_if_error(r, "Policy calculation", error);


        context->cmd.Provision.policy_digest.size = context->cmd.Provision.hash_size;
        memcpy(&context->cmd.Provision.policy_digest.buffer[0],
               &policy_harness
               ->policyDigests.digests[context->cmd.Provision.digest_idx].digest,
               context->cmd.Provision.hash_size);

        hierarchy_object->policy_harness = policy_harness;
        hierarchy_object->misc.hierarchy.authPolicy = context->cmd.Provision.policy_digest;

        r = Esys_SetPrimaryPolicy_Async(context->esys, handle,
                                        ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
                                        &context->cmd.Provision.policy_digest,
                                        context->profiles.default_profile.nameAlg);
        return_if_error(r, "Esys_SetPrimaryPolicy_Async");
        fallthrough;

    statecase(context->hierarchy_policy_state, HIERARCHY_CHANGE_POLICY_NULL_AUTH_SENT);
        r = Esys_SetPrimaryPolicy_Finish(context->esys);
        return_try_again(r);
        if ((r & ~TPM2_RC_N_MASK) != TPM2_RC_BAD_AUTH) {
            return_if_error(r, "SetPrimaryPolicy_Finish");
            context->hierarchy_policy_state = HIERARCHY_CHANGE_POLICY_INIT;
            return TSS2_RC_SUCCESS;
        }

        /* Retry after NULL authorization was not successful */
        ifapi_init_hierarchy_object(hierarchy_object, handle);
        r = ifapi_set_auth(context, hierarchy_object, "Hierarchy object");
        return_if_error(r, "HierarchyChangePolicy");

        r = Esys_SetPrimaryPolicy_Async(context->esys, handle,
                                        context->session1, ESYS_TR_NONE, ESYS_TR_NONE,
                                        &context->cmd.Provision.policy_digest,
                                        context->profiles.default_profile.nameAlg);
        return_if_error(r, "Esys_SetPrimaryPolicy_Async");

        context->hierarchy_policy_state = HIERARCHY_CHANGE_POLICY_AUTH_SENT;
        return TSS2_FAPI_RC_TRY_AGAIN;

    statecasedefault(context->hierarchy_policy_state);
    }

error:
    return r;
}

/** Allocated ifapi objects will be recorede in the context.
 */
IFAPI_OBJECT
*ifapi_allocate_object(FAPI_CONTEXT *context)
{
    NODE_OBJECT_T *node = calloc(1, sizeof(NODE_OBJECT_T));
    if (!node)
        return NULL;

    node->object = calloc(1, sizeof(IFAPI_OBJECT));
    if (!node->object) {
        free(node);
        return NULL;
    }
    node->next = context->object_list;
    context->object_list = node;
    return (IFAPI_OBJECT *) node->object;
}

/** Free an object stored in the context.
 */
void
ifapi_free_objects(FAPI_CONTEXT *context)
{
    NODE_OBJECT_T *free_node;
    NODE_OBJECT_T *node = context->object_list;
    while (node) {
        free(node->object);
        free_node = node;
        node = node->next;
        free(free_node);
    }
}

/** Free all objects stored in the context.
 */
void
ifapi_free_object(FAPI_CONTEXT *context, IFAPI_OBJECT **object)
{
    NODE_OBJECT_T *node;
    NODE_OBJECT_T **update_ptr;

    for (node = context->object_list,
             update_ptr = &context->object_list;
             node != NULL;
         update_ptr = &node->next, node = node->next) {
        if (node->object == object) {
            *update_ptr = node->next;
            SAFE_FREE(node->object);
            SAFE_FREE(node);
            *object = NULL;
            return;
        }
    }
}

#define ADD_CAPABILITY_INFO(capability, field, subfield, max_count, property_count) \
    if (context->cmd.GetInfo.fetched_data->data.capability.count > max_count - property_count) { \
        context->cmd.GetInfo.fetched_data->data.capability.count = max_count - property_count; \
    } \
\
    memmove(&context->cmd.GetInfo.capability_data->data.capability.field[property_count], \
            context->cmd.GetInfo.fetched_data->data.capability.field, \
            context->cmd.GetInfo.fetched_data->data.capability.count \
            * sizeof(context->cmd.GetInfo.fetched_data->data.capability.field[0]));       \
    property_count += context->cmd.GetInfo.fetched_data->data.capability.count; \
\
    context->cmd.GetInfo.capability_data->data.capability.count = property_count; \
\
    if (more_data && property_count < count \
        && context->cmd.GetInfo.fetched_data->data.capability.count) {  \
        context->cmd.GetInfo.property \
            = context->cmd.GetInfo.capability_data->data. \
            capability.field[property_count - 1]subfield + 1;   \
    } else { \
        more_data = false; \
    }

TPM2_RC
ifapi_capability_init(FAPI_CONTEXT *context)
{
    context->cmd.GetInfo.capability_data = NULL;
    context->cmd.GetInfo.fetched_data = NULL;

    return TSS2_RC_SUCCESS;


}

TPM2_RC
ifapi_capability_get(FAPI_CONTEXT *context, TPM2_CAP capability,
                     UINT32 count, TPMS_CAPABILITY_DATA **capability_data) {

    TPMI_YES_NO more_data;
    TSS2_RC r = TSS2_RC_SUCCESS;
    ESYS_CONTEXT *ectx = context->esys;

    switch (context->state) {
    statecase(context->state, GET_INFO_GET_CAP);
        /* fetch capability info */
        context->cmd.GetInfo.fetched_data = NULL;
        context->cmd.GetInfo.capability_data = NULL;
        fallthrough;

    statecase(context->state, GET_INFO_GET_CAP_MORE);
        r = Esys_GetCapability_Async(ectx, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                                     capability, context->cmd.GetInfo.property,
                                     count - context->cmd.GetInfo.property_count);
        goto_if_error(r, "Error GetCapability", error_cleanup);
        fallthrough;

    statecase(context->state, GET_INFO_WAIT_FOR_CAP);
        r = Esys_GetCapability_Finish(ectx, &more_data, &context->cmd.GetInfo.fetched_data);
        return_try_again(r);
        goto_if_error(r, "Error GetCapability", error_cleanup);

        LOG_TRACE("GetCapability: capability: 0x%x, property: 0x%x", capability,
                  context->cmd.GetInfo.property);

        if (context->cmd.GetInfo.fetched_data->capability != capability) {
            goto_error(r, TSS2_FAPI_RC_GENERAL_FAILURE,
                       "TPM returned different capability than requested: 0x%x != 0x%x",
                       error_cleanup,
                       context->cmd.GetInfo.fetched_data->capability, capability);
        }

        if (context->cmd.GetInfo.capability_data == NULL) {
            /* reuse the TPM's result structure */
            context->cmd.GetInfo.capability_data = context->cmd.GetInfo.fetched_data;

            if (!more_data) {
                /* there won't be another iteration of the loop, just return the result unmodified */
                *capability_data = context->cmd.GetInfo.capability_data;
                return TPM2_RC_SUCCESS;
            }
        }

        /* append the TPM's results to the initial structure, as long as there is still space left */
        switch (capability) {
        case TPM2_CAP_ALGS:
            ADD_CAPABILITY_INFO(algorithms, algProperties, .alg,
                                TPM2_MAX_CAP_ALGS,
                                context->cmd.GetInfo.property_count);
            break;
        case TPM2_CAP_HANDLES:
            ADD_CAPABILITY_INFO(handles, handle,,
                                TPM2_MAX_CAP_HANDLES,
                                context->cmd.GetInfo.property_count);
            break;
        case TPM2_CAP_COMMANDS:
            ADD_CAPABILITY_INFO(command, commandAttributes,,
                                TPM2_MAX_CAP_CC,
                                context->cmd.GetInfo.property_count);
            /* workaround because tpm2-tss does not implement attribute commandIndex for TPMA_CC */
            context->cmd.GetInfo.property &= TPMA_CC_COMMANDINDEX_MASK;
            break;
        case TPM2_CAP_PP_COMMANDS:
            ADD_CAPABILITY_INFO(ppCommands, commandCodes,,
                                TPM2_MAX_CAP_CC,
                                context->cmd.GetInfo.property_count);
            break;
        case TPM2_CAP_AUDIT_COMMANDS:
            ADD_CAPABILITY_INFO(auditCommands, commandCodes,,
                                TPM2_MAX_CAP_CC,
                                context->cmd.GetInfo.property_count);
            break;
        case TPM2_CAP_PCRS:
            ADD_CAPABILITY_INFO(assignedPCR, pcrSelections, .hash,
                                TPM2_NUM_PCR_BANKS,
                                context->cmd.GetInfo.property_count);
            break;
        case TPM2_CAP_TPM_PROPERTIES:
            ADD_CAPABILITY_INFO(tpmProperties, tpmProperty, .property,
                                TPM2_MAX_TPM_PROPERTIES,
                                context->cmd.GetInfo.property_count);
            break;
        case TPM2_CAP_PCR_PROPERTIES:
            ADD_CAPABILITY_INFO(pcrProperties, pcrProperty, .tag,
                                TPM2_MAX_PCR_PROPERTIES,
                                context->cmd.GetInfo.property_count);
            break;
        case TPM2_CAP_ECC_CURVES:
            ADD_CAPABILITY_INFO(eccCurves, eccCurves,,
                                TPM2_MAX_ECC_CURVES,
                                context->cmd.GetInfo.property_count);
            break;
        case TPM2_CAP_VENDOR_PROPERTY:
            ADD_CAPABILITY_INFO(intelPttProperty, property,,
                                TPM2_MAX_PTT_PROPERTIES,
                                context->cmd.GetInfo.property_count);
            break;
        default:
            LOG_ERROR("Unsupported capability: 0x%x\n", capability);
            if (context->cmd.GetInfo.fetched_data != context->cmd.GetInfo.capability_data) {
                free(context->cmd.GetInfo.fetched_data);
            }
            free(context->cmd.GetInfo.capability_data);
            *capability_data = NULL;
            return TSS2_FAPI_RC_NOT_IMPLEMENTED;
        }

        if (context->cmd.GetInfo.fetched_data != context->cmd.GetInfo.capability_data) {
            free(context->cmd.GetInfo.fetched_data);
        }
        *capability_data = context->cmd.GetInfo.capability_data;
        break;

    statecasedefault(context->state);
    }
    if (more_data) {
        context->state = GET_INFO_GET_CAP_MORE;
        return TSS2_FAPI_RC_TRY_AGAIN;
    } else {
        context->state = _FAPI_STATE_INIT;
        return TSS2_RC_SUCCESS;
    }

 error_cleanup:
    context->state = _FAPI_STATE_INIT;
    SAFE_FREE(context->cmd.GetInfo.capability_data);
    SAFE_FREE(context->cmd.GetInfo.fetched_data);
    return r;
}

TSS2_RC
ifapi_get_certificates(
    FAPI_CONTEXT *context,
    UINT32 min_handle,
    UINT32 max_handle,
    NODE_OBJECT_T **cert_list)
{
    TSS2_RC r;
    TPMI_YES_NO moreData;
    TPMS_CAPABILITY_DATA **capabilityData = &context->cmd.Provision.capabilityData;
    TPM2B_NV_PUBLIC *nvPublic;
    TPM2B_NAME *nvName;
    uint8_t *cert_data;
    size_t cert_size;

    context->cmd.Provision.cert_nv_idx = MIN_EK_CERT_HANDLE;
    context->cmd.Provision.capabilityData = NULL;

    switch (context->get_cert_state) {
    statecase(context->get_cert_state, GET_CERT_INIT);
        *cert_list = NULL;
        context->cmd.Provision.cert_idx = 0;
        r = Esys_GetCapability_Async(context->esys,
                                     ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE,
                                     TPM2_CAP_HANDLES, min_handle,
                                     TPM2_MAX_CAP_HANDLES);
        goto_if_error(r, "Esys_GetCapability_Async", error);
        fallthrough;

    statecase(context->get_cert_state, GET_CERT_WAIT_FOR_GET_CAP);
        r = Esys_GetCapability_Finish(context->esys, &moreData, capabilityData);
        return_try_again(r);
        goto_if_error_reset_state(r, "GetCapablity_Finish", error);

        if (!*capabilityData || (*capabilityData)->data.handles.count == 0) {
            *cert_list = NULL;
            return TSS2_RC_SUCCESS;
        }
        context->cmd.Provision.capabilityData = *capabilityData;
        context->cmd.Provision.cert_count = (*capabilityData)->data.handles.count;

        /* Filter out NV handles beyond the EK cert range */
        for (size_t i = 0; i < context->cmd.Provision.cert_count; i++) {
            if (context->cmd.Provision.capabilityData->data.handles.handle[i] > max_handle) {
                context->cmd.Provision.cert_count = i;
                break;
            }
        }
        fallthrough;

    statecase(context->get_cert_state, GET_CERT_GET_CERT_NV);

        context->cmd.Provision.cert_nv_idx
            = context->cmd.Provision.capabilityData
            ->data.handles.handle[context->cmd.Provision.cert_idx];

        ifapi_init_hierarchy_object(&context->nv_cmd.auth_object,
                                    TPM2_RH_OWNER);

        r = Esys_TR_FromTPMPublic_Async(context->esys,
                                        context->cmd.Provision.cert_nv_idx,
                                        ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE);
        goto_if_error_reset_state(r, "Esys_TR_FromTPMPublic_Async", error);
        fallthrough;

    statecase(context->get_cert_state, GET_CERT_GET_CERT_NV_FINISH);
        r = Esys_TR_FromTPMPublic_Finish(context->esys,
                                         &context->cmd.Provision.esys_nv_cert_handle);
        return_try_again(r);
        goto_if_error_reset_state(r, "TR_FromTPMPublic_Finish", error);

        /* Read public to get size of certificate */
        r = Esys_NV_ReadPublic_Async(context->esys,
                                     context->cmd.Provision.esys_nv_cert_handle,
                                     ESYS_TR_NONE,
                                     ESYS_TR_NONE,
                                     ESYS_TR_NONE);
        goto_if_error_reset_state(r, "Esys_NV_ReadPublic_Async", error);
        fallthrough;

    statecase(context->get_cert_state, GET_CERT_GET_CERT_READ_PUBLIC);
        r = Esys_NV_ReadPublic_Finish(context->esys,
                                      &nvPublic,
                                      &nvName);
        return_try_again(r);
        goto_if_error(r, "Error: nv read public", error);

        /* TPMA_NV_NO_DA is set for NV certificate */
        context->nv_cmd.nv_object.misc.nv.public.nvPublic.attributes = TPMA_NV_NO_DA;

        /* Prepare context for nv read */
        context->nv_cmd.data_idx = 0;
        context->nv_cmd.auth_index = ESYS_TR_RH_OWNER;
        context->nv_cmd.numBytes = nvPublic->nvPublic.dataSize;
        context->nv_cmd.esys_handle = context->cmd.Provision.esys_nv_cert_handle;
        context->nv_cmd.offset = 0;
        context->cmd.Provision.pem_cert = NULL;
        context->session1 = ESYS_TR_PASSWORD;
        context->session2 = ESYS_TR_NONE;
        context->nv_cmd.nv_read_state = NV_READ_INIT;
        memset(&context->nv_cmd.nv_object, 0, sizeof(IFAPI_OBJECT));
        fallthrough;

    statecase(context->get_cert_state, GET_CERT_READ_CERT);
        r = ifapi_nv_read(context, &cert_data, &cert_size);
        return_try_again(r);
        goto_if_error_reset_state(r, " FAPI NV_Read", error);

        context->cmd.Provision.cert_idx += 1;

        /* Add cert to list */
        if (context->cmd.Provision.cert_idx == context->cmd.Provision.cert_count) {
            context->get_cert_state = GET_CERT_GET_CERT_NV;

            r = push_object_with_size_to_list(cert_data, cert_size, cert_list);
            goto_if_error(r, "Store certificate in list.", error);

            return TSS2_RC_SUCCESS;
        } else {
            context->get_cert_state = GET_CERT_GET_CERT_NV;
        }
        break;

    statecasedefault(context->get_cert_state);
    }

 error:
    ifapi_free_object_list(*cert_list);
    return r;
}


/** Get description of an internal FAPI object.
 *
 * @parm[in] object The object with the descritpion.
 * @retval The char string of the description.
 * @retval NULL if no description exists.
 */
TSS2_RC
ifapi_get_description(IFAPI_OBJECT *object, char **description)
{
    char *obj_description = NULL;

    switch (object->objectType) {
    case IFAPI_KEY_OBJ:
        obj_description = object->misc.key.description;
        break;
    case IFAPI_NV_OBJ:
        obj_description = object->misc.nv.description;
        break;
    case IFAPI_HIERARCHY_OBJ:
        obj_description = object->misc.hierarchy.description;
        break;
    default:
        *description = NULL;
        return TSS2_RC_SUCCESS;
    }
    if (obj_description) {
        *description = strdup(obj_description);
        check_oom(*description);
    } else {
        *description = NULL;
    }
    return TSS2_RC_SUCCESS;
}

/** Set description of an internal FAPI object.
 *
 * @parm[in,out] object The object with the descritpion.
 * @parm[in] descritpion The description char strint or NULL.
 */
void
ifapi_set_description(IFAPI_OBJECT *object, char *description)
{
    switch (object->objectType) {
    case IFAPI_KEY_OBJ:
        SAFE_FREE(object->misc.key.description);
        object->misc.key.description = description;
        break;
    case IFAPI_NV_OBJ:
        SAFE_FREE(object->misc.nv.description);
        object->misc.nv.description = description;
        break;
    case IFAPI_HIERARCHY_OBJ:
        SAFE_FREE(object->misc.hierarchy.description);
        object->misc.hierarchy.description = description;
        break;
    default:
        LOG_WARNING("Description can't be set");
        break;
    }
}

TSS2_RC
ifapi_expand_path(IFAPI_KEYSTORE *keystore, const char *path, char **file_name)
{
    TSS2_RC r;
    NODE_STR_T *node_list = NULL;
    size_t pos = 0;

    if (ifapi_hierarchy_path_p(path)) {
        if (strncmp(path, "P_", 2) == 0 || strncmp(path, "/P_", 3) == 0) {
            *file_name = strdup(path);
            return_if_null(*file_name, "Out of memory", TSS2_FAPI_RC_MEMORY);
        } else {
            if (strncmp("/", path, 1) == 0)
                pos = 1;
            r  = ifapi_asprintf(file_name, "%s%s%s",  keystore->defaultprofile,
                                IFAPI_FILE_DELIM, &path[pos]);
            return_if_error(r, "Out of memory.");
        }
    } else if (ifapi_path_type_p(path, IFAPI_NV_PATH)
        || ifapi_path_type_p(path, IFAPI_POLICY_PATH)
        || ifapi_path_type_p(path, IFAPI_EXT_PATH)
        || strncmp(path, "/P_", 3) == 0
        || strncmp(path, "P_", 2) == 0) {
        *file_name = strdup(path);
        return_if_null(*file_name, "Out of memory", TSS2_FAPI_RC_MEMORY);

    } else {
        r = get_explicit_key_path(keystore, path, &node_list);
        return_if_error(r, "Out of memory");

        r = ifapi_path_string(file_name, NULL, node_list, NULL);
        goto_if_error(r, "Out of memory", error);

        free_string_list(node_list);
    }
    return TSS2_RC_SUCCESS;

error:
    free_string_list(node_list);
    return r;
}
