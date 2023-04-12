/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2022 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
  Commit and validate
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <pwd.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "clixon_backend_transaction.h"
#include "clixon_backend_plugin.h"
#include "clixon_backend_client.h"
#include "backend_handle.h"
#include "clixon_backend_commit.h"
#include "backend_client.h"

/*! Key values are checked for validity independent of user-defined callbacks
 *
 * Key values are checked as follows:
 * 1. If no value and default value defined, add it.
 * 2. If no value and mandatory flag set in spec, report error.
 * 3. Validate value versus spec, and report error if no match. Currently only int ranges and
 *    string regexp checked.
 * See also db_lv_set() where defaults are also filled in. The case here for defaults
 * are if code comes via XML/NETCONF.
 * @param[in]   h       Clixon handle
 * @param[in]   yspec   Yang spec
 * @param[in]   td      Transaction data
 * @param[out]  xret    Error XML tree. Free with xml_free after use
 * @retval      1       Validation OK       
 * @retval      0       Validation failed (with cbret set)
 * @retval     -1       Error
 */
static int
generic_validate(clicon_handle       h,
                 yang_stmt          *yspec,
                 transaction_data_t *td,
                 cxobj             **xret)
{
    int        retval = -1;
    cxobj     *x2;
    int        i;
    int        ret;
    cbuf      *cb = NULL;

    /* All entries */
    if ((ret = xml_yang_validate_all_top(h, td->td_target, xret)) < 0) 
        goto done;
    if (ret == 0)
        goto fail;
    /* changed entries */
    for (i=0; i<td->td_clen; i++){
        x2 = td->td_tcvec[i]; /* target changed */
        /* Should this be recursive? */
        if ((ret = xml_yang_validate_add(h, x2, xret)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
    /* added entries */
    for (i=0; i<td->td_alen; i++){
        x2 = td->td_avec[i];
        if ((ret = xml_yang_validate_add(h, x2, xret)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
    // ok:
    retval = 1;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Common startup validation
 * Get db, upgrade it w potential transformed XML, populate it w yang spec,
 * sort it, validate it by triggering a transaction
 * and call application callback validations.
 * @param[in]  h       Clicon handle
 * @param[in]  db      The startup database. The wanted backend state
 * @param[in]  td      Transaction data
 * @param[out] cbret   CLIgen buffer w error stmt if retval = 0
 * @retval     1       Validation OK       
 * @retval     0       Validation failed (with cbret set)
 * @retval    -1       Error - or validation failed (but cbret not set)
 * @note Need to differentiate between error and validation fail 
 *
 * 1. Parse startup XML (or JSON)
 * 2. If syntax failure, call startup-cb(ERROR), copy failsafe db to 
 * candidate and commit. Done
 * 3. Check yang module versions between backend and init config XML. (msdiff)
 * 4. Validate startup db. (valid)
 * 5. If valid fails, call startup-cb(Invalid, msdiff), keep startup in candidate and commit failsafe db. Done.
 * 6. Call startup-cb(OK, msdiff) and commit.
 * @see validate_common   for incoming validate/commit
 */
static int
startup_common(clicon_handle       h, 
               char               *db,
               transaction_data_t *td,
               cbuf               *cbret)
{
    int                 retval = -1;
    yang_stmt          *yspec;
    int                 ret;
    modstate_diff_t    *msdiff = NULL;
    cxobj              *xt = NULL;
    cxobj              *x;
    cxobj              *xret = NULL;
    cxobj              *xerr = NULL;

    /* If CLICON_XMLDB_MODSTATE is enabled, then get the db XML with 
     * potentially non-matching module-state in msdiff
     */
    if (clicon_option_bool(h, "CLICON_XMLDB_MODSTATE"))
        if ((msdiff = modstate_diff_new()) == NULL)
            goto done;
    clicon_debug(1, "Reading initial config from %s", db);
    /* Get the startup datastore WITHOUT binding to YANG, sorting and default setting. 
     * It is done below, later in this function
     */
    if (clicon_option_bool(h, "CLICON_XMLDB_UPGRADE_CHECKOLD")){
        if ((ret = xmldb_get0(h, db, YB_MODULE, NULL, "/", 0, 0, &xt, msdiff, &xerr)) < 0)
            goto done;
        if (ret == 0){     /* ret should not be 0 */
            /* Print upgraded db: -q backend switch for debugging/ showing upgraded config only */
            if (clicon_quit_upgrade_get(h) == 1){
                xml_print(stderr, xerr);
                clicon_err(OE_XML, 0, "invalid configuration before upgrade");
                exit(0);  /* This is fairly abrupt , but need to avoid side-effects of rewinding 
                           * See similar clause below
                           */
            }
            if (clixon_xml2cbuf(cbret, xerr, 0, 0, NULL, -1, 0) < 0)
                goto done;
            goto fail;
        }
    }
    else {
        if (xmldb_get0(h, db, YB_NONE, NULL, "/", 0, 0, &xt, msdiff, &xerr) < 0)
            goto done;
    }
    clicon_debug_xml(CLIXON_DBG_DETAIL, xt, "startup");
    if (msdiff && msdiff->md_status == 0){ // Possibly check for CLICON_XMLDB_MODSTATE
        clicon_log(LOG_WARNING, "Modstate expected in startup datastore but not found\n"
                   "This may indicate that the datastore is not initialized corrrectly, such as copy/pasted.\n"
                   "It may also be normal bootstrapping since module state will be written on next datastore save");
    }
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
        clicon_err(OE_YANG, 0, "Yang spec not set");
        goto done;
    }
    clicon_debug(1, "Reading startup config done");
    /* Clear flags xpath for get */
    xml_apply0(xt, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset,
               (void*)(XML_FLAG_MARK|XML_FLAG_CHANGE));
    /* Here xt is old syntax */
    /* General purpose datastore upgrade */
    if (clixon_plugin_datastore_upgrade_all(h, db, xt, msdiff) < 0)
       goto done;
    /* Module-specific upgrade callbacks */
    if (msdiff){
        if ((ret = clixon_module_upgrade(h, xt, msdiff, cbret)) < 0)
            goto done;
        if (ret == 0){
            if (cbuf_len(cbret) == 0)
                cprintf(cbret, "Module-set upgrade function returned failure but lacks reason (cbret is not set)");
            goto fail;
        }
    }
    /* Print upgraded db: -q backend switch for debugging/ showing upgraded config only */
    if (clicon_quit_upgrade_get(h) == 1){
        /* bind yang */
        if ((ret = xml_bind_yang(h, xt, YB_MODULE, yspec, &xret)) < 1){
            if (ret == 0){
                /* invalid */
                clicon_err(OE_XML, EFAULT, "invalid configuration");
            }
            else {
                /* error */
                xml_print(stderr, xret);
                clicon_err(OE_XML, 0, "%s: YANG binding error", __func__);
            }
        }       /* sort yang */
        else if (xml_sort_recurse(xt) < 0) {
            clicon_err(OE_XML, EFAULT, "Yang sort error");
        }
        if (xmldb_dump(h, stdout, xt) < 0)
            goto done;
        exit(0);  /* This is fairly abrupt , but need to avoid side-effects of rewinding
                     stack. Alternative is to make a separate function stack for this. */
    }
    /* If empty skip. Note upgrading can add children, so it may be empty before that. */
    if (xml_child_nr(xt) == 0){     
        td->td_target = xt;
        xt = NULL;
        goto ok;
    }
    /* After upgrading, XML tree needs to be sorted and yang spec populated */
    if ((ret = xml_bind_yang(h, xt, YB_MODULE, yspec, &xret)) < 0)
        goto done;
    if (ret == 0){
        if (clixon_xml2cbuf(cbret, xret, 0, 0, NULL, -1, 0) < 0)
            goto done;
        goto fail; 
    }
    /* After upgrade check no state data */
    if ((ret = xml_non_config_data(xt, &xret)) < 0)
        goto done;
    if (ret == 0){
        if (clixon_xml2cbuf(cbret, xret, 0, 0, NULL, -1, 0) < 0)
            goto done;
        goto fail; 
    }
    /* Sort xml */
    if (xml_sort_recurse(xt) < 0)
        goto done;
    /* Add global defaults. */
    if (xml_global_defaults(h, xt, NULL, NULL, yspec, 0) < 0)
        goto done;
    /* Apply default values (removed in clear function) */
    if (xml_default_recurse(xt, 0) < 0)
        goto done;
    
    /* Handcraft transition with with only add tree */
    td->td_target = xt;
    xt = NULL;
    x = NULL;
    while ((x = xml_child_each(td->td_target, x, CX_ELMNT)) != NULL){
        xml_flag_set(x, XML_FLAG_ADD); /* Also down */
        xml_apply(x, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_ADD);
        if (cxvec_append(x, &td->td_avec, &td->td_alen) < 0) 
            goto done;
    }

    /* 4. Call plugin transaction start callbacks */
    if (plugin_transaction_begin_all(h, td) < 0)
        goto done;

    /* 5. Make generic validation on all new or changed data.
       Note this is only call that uses 3-values */
    clicon_debug(1, "Validating startup %s", db);
    if ((ret = generic_validate(h, yspec, td, &xret)) < 0)
        goto done;
    if (ret == 0){
        if (clixon_xml2cbuf(cbret, xret, 0, 0, NULL, -1, 0) < 0)
            goto done;
        goto fail; /* STARTUP_INVALID */
    }
    /* 6. Call plugin transaction validate callbacks */
    if (plugin_transaction_validate_all(h, td) < 0)
        goto done;

    /* 7. Call plugin transaction complete callbacks */
    if (plugin_transaction_complete_all(h, td) < 0)
        goto done;
 ok:
    retval = 1;
 done:
    if (xerr)
        xml_free(xerr);
    if (xret)
        xml_free(xret);
    if (xt)
        xml_free(xt);
    if (msdiff)
        modstate_diff_free(msdiff);
    return retval;
 fail: 
    retval = 0;
    goto done;
}

/*! Read startup db, check upgrades and validate it, return upgraded XML
 *
 * @param[in]  h       Clicon handle
 * @param[in]  db      The startup database. The wanted backend state
 * @param[out] xtr     (Potentially) transformed XML
 * @param[out] cbret   CLIgen buffer w error stmt if retval = 0
 * @retval     1       Validation OK       
 * @retval     0       Validation failed (with cbret set)
 * @retval    -1       Error - or validation failed (but cbret not set)
 */
int
startup_validate(clicon_handle  h, 
                 char          *db,
                 cxobj        **xtr,
                 cbuf          *cbret)
{
    int                 retval = -1;
    int                 ret;
    transaction_data_t *td = NULL;

    /* Handcraft a transition with only target and add trees */
    if ((td = transaction_new()) == NULL)
        goto done;
    if ((ret = startup_common(h, db, td, cbret)) < 0){
        plugin_transaction_abort_all(h, td);
        goto done;
    }
    if (ret == 0){
        plugin_transaction_abort_all(h, td);
        goto fail;
    }
    plugin_transaction_end_all(h, td);
     /* Clear cached trees from default values and marking */
    if (xmldb_get0_clear(h, td->td_target) < 0)
         goto done;
    if (xtr){
        *xtr = td->td_target; 
        td->td_target = NULL;
    }
    retval = 1;
 done:
     if (td){
         xmldb_get0_free(h, &td->td_target);
         transaction_free(td);
     }
    return retval;
 fail: /* cbret should be set */
    retval = 0;
    goto done;
}

/*! Read startup db, check upgrades and commit it
 *
 * @param[in]  h       Clicon handle
 * @param[in]  db      The startup database. The wanted backend state
 * @param[out] cbret   CLIgen buffer w error stmt if retval = 0
 * @retval     1       Validation OK       
 * @retval     0       Validation failed (with cbret set)
 * @retval    -1       Error - or validation failed (but cbret not set)
 * Only called from startup_mode_startup
 */
int
startup_commit(clicon_handle  h, 
               char          *db,
               cbuf          *cbret)
{
    int                 retval = -1;
    int                 ret;
    transaction_data_t *td = NULL;

    if (strcmp(db,"running")==0){
        clicon_err(OE_FATAL, 0, "Invalid startup db: %s", db);
        goto done;
    }
    /* Handcraft a transition with only target and add trees */
    if ((td = transaction_new()) == NULL)
        goto done;
    if ((ret = startup_common(h, db, td, cbret)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    /* 8. Call plugin transaction commit callbacks */
    if (plugin_transaction_commit_all(h, td) < 0)
        goto done;
    /* After commit, make a post-commit call (sure that all plugins have committed) */
    if (plugin_transaction_commit_done_all(h, td) < 0)
        goto done;
    /* Clear cached trees from default values and marking */
    if (xmldb_get0_clear(h, td->td_target) < 0)
        goto done;

    /* [Delete and] create running db */
    if (xmldb_exists(h, "running") == 1){
        if (xmldb_delete(h, "running") != 0 && errno != ENOENT) 
            goto done;;
    }
    if (xmldb_create(h, "running") < 0)
        goto done;
    /* 9, write (potentially modified) tree to running
     * XXX note here startup is copied to candidate, which may confuse everything
     * XXX default values are overwritten
     */
    if (td->td_target)
        /* target is datastore, but is here transformed to mimic an incoming 
         * edit-config
         */
        xml_name_set(td->td_target,  NETCONF_INPUT_CONFIG);

    if ((ret = xmldb_put(h, "running", OP_REPLACE, td->td_target,
                         clicon_username_get(h), cbret)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    /* 10. Call plugin transaction end callbacks */
    plugin_transaction_end_all(h, td);
    retval = 1;
 done:
    if (td){
        if (retval < 1)
            plugin_transaction_abort_all(h, td);
        xmldb_get0_free(h, &td->td_target);
        transaction_free(td);
    }
    return retval;
 fail: /* cbret should be set */
    retval = 0;
    goto done;
}

/*! Validate a candidate db and comnpare to running
 * Get both source and dest datastore, validate target, compute diffs
 * and call application callback validations.
 * @param[in]  h       Clicon handle
 * @param[in]  db      The (candidate) database. The wanted backend state
 * @param[in]  td      Transaction data
 * @param[out] xret    Error XML tree, if retval is 0. Free with xml_free after use
 * @retval     1       Validation OK       
 * @retval     0       Validation failed (with xret set)
 * @retval    -1       Error - or validation failed (but cbret not set)
 * @note Need to differentiate between error and validation fail 
 *       (only done for generic_validate)
 * @see startup_common  for startup scenario
 */
static int
validate_common(clicon_handle       h, 
                char               *db,
                transaction_data_t *td,
                cxobj             **xret)
{
    int         retval = -1;
    yang_stmt  *yspec;
    int         i;
    cxobj      *xn;
    int         ret;
    
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
        clicon_err(OE_FATAL, 0, "No DB_SPEC");
        goto done;
    }   
    /* This is the state we are going to */
    if ((ret = xmldb_get0(h, db, YB_MODULE, NULL, "/", 0, 0, &td->td_target, NULL, xret)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    /* Clear flags xpath for get */
    xml_apply0(td->td_target, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset,
               (void*)(XML_FLAG_MARK|XML_FLAG_CHANGE));
    /* 2. Parse xml trees 
     * This is the state we are going from */
    if ((ret = xmldb_get0(h, "running", YB_MODULE, NULL, "/", 0, 0, &td->td_src, NULL, xret)) < 0)
        goto done;
    if (ret == 0)
        goto fail;
    /* Clear flags xpath for get */
    xml_apply0(td->td_src, CX_ELMNT, (xml_applyfn_t*)xml_flag_reset,
               (void*)(XML_FLAG_MARK|XML_FLAG_CHANGE));
    /* 3. Compute differences */
    if (xml_diff(td->td_src,
                 td->td_target,
                 &td->td_dvec,      /* removed: only in running */
                 &td->td_dlen,
                 &td->td_avec,      /* added: only in candidate */
                 &td->td_alen,
                 &td->td_scvec,     /* changed: original values */
                 &td->td_tcvec,     /* changed: wanted values */
                 &td->td_clen) < 0)
        goto done;
    transaction_dbg(h, CLIXON_DBG_DETAIL, td, __FUNCTION__);
    /* Mark as changed in tree */
    for (i=0; i<td->td_dlen; i++){ /* Also down */
        xn = td->td_dvec[i];
        xml_flag_set(xn, XML_FLAG_DEL);
        xml_apply(xn, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_DEL);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    for (i=0; i<td->td_alen; i++){ /* Also down */
        xn = td->td_avec[i];
        xml_flag_set(xn, XML_FLAG_ADD);
        xml_apply(xn, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_ADD);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    for (i=0; i<td->td_clen; i++){ /* Also up */
        xn = td->td_scvec[i];
        xml_flag_set(xn, XML_FLAG_CHANGE);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
        xn = td->td_tcvec[i];
        xml_flag_set(xn, XML_FLAG_CHANGE);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    /* 4. Call plugin transaction start callbacks */
    if (plugin_transaction_begin_all(h, td) < 0)
        goto done;

    /* 5. Make generic validation on all new or changed data.
       Note this is only call that uses 3-values */
    if ((ret = generic_validate(h, yspec, td, xret)) < 0)
        goto done;
    if (ret == 0)
        goto fail;

    /* 6. Call plugin transaction validate callbacks */
    if (plugin_transaction_validate_all(h, td) < 0)
        goto done;

    /* 7. Call plugin transaction complete callbacks */
    if (plugin_transaction_complete_all(h, td) < 0)
        goto done;
    retval = 1;
 done:
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Start a validate transaction
 *
 * @param[in]  h      Clicon handle
 * @param[in]  db     A candidate database, typically "candidate" but not necessarily so
 * @param[out] cbret  CLIgen buffer w error stmt if retval = 0
 * @retval     1      Validation OK       
 * @retval     0      Validation failed (with cbret set)
 * @retval    -1      Error - or validation failed 
 */
int
candidate_validate(clicon_handle h, 
                   char         *db,
                   cbuf         *cbret)
{
    int                 retval = -1;
    transaction_data_t *td = NULL;
    cxobj              *xret = NULL;
    int                 ret;
    
    clicon_debug(1, "%s", __FUNCTION__);
    if (db == NULL || cbret == NULL){
        clicon_err(OE_CFG, EINVAL, "db or cbret is NULL");
        goto done;
    }
    /* 1. Start transaction */
    if ((td = transaction_new()) == NULL)
        goto done;
        /* Common steps (with commit) */
    if ((ret = validate_common(h, db, td, &xret)) < 0){
        /* A little complex due to several sources of validation fails or errors.
         * (1) xerr is set -> translate to cbret; (2) cbret set use that; otherwise
         * use clicon_err. 
         * TODO: -1 return should be fatal error, not failed validation
         */
        if (!cbuf_len(cbret) &&
            netconf_operation_failed(cbret, "application", clicon_err_reason)< 0)
            goto done;
        goto fail;
    }
    if (ret == 0){
        if (xret == NULL){
            clicon_err(OE_CFG, EINVAL, "xret is NULL");
            goto done;
        }
        if (clixon_xml2cbuf(cbret, xret, 0, 0, NULL, -1, 0) < 0)
            goto done;
        if (!cbuf_len(cbret) &&
            netconf_operation_failed(cbret, "application", clicon_err_reason)< 0)
            goto done;
        goto fail;
    }
    if (xmldb_get0_clear(h, td->td_src) < 0 ||
        xmldb_get0_clear(h, td->td_target) < 0)
        goto done;

    plugin_transaction_end_all(h, td);
    retval = 1;
 done:
    if (xret)
        xml_free(xret);
     if (td){
         if (retval < 1)
             plugin_transaction_abort_all(h, td);
         xmldb_get0_free(h, &td->td_target);
         xmldb_get0_free(h, &td->td_src);
         transaction_free(td);
     }
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Do a diff between candidate and running, then start a commit transaction
 *
 * The code reverts changes if the commit fails. But if the revert
 * fails, we just ignore the errors and proceed. Maybe we should
 * do something more drastic?
 * @param[in]  h          Clicon handle
 * @param[in]  xe         Request: <rpc><xn></rpc>  (or NULL)
 * @param[in]  db         A candidate database, not necessarily "candidate"
 * @param[in]  myid       Client id of triggering incoming message (or 0)
 * @param[in]  vlev       Validation level (0: full validation) // obsolete
 * @param[out] cbret      Return xml tree, eg <rpc-reply>..., <rpc-error.. (if retval = 0)
 * @retval     1          Validation OK       
 * @retval     0          Validation failed (with cbret set)
 * @retval    -1          Error - or validation failed 
 */
int
candidate_commit(clicon_handle h, 
                 cxobj        *xe,
                 char         *db,
                 uint32_t      myid,
                 validate_level vlev, // obsolete
                 cbuf         *cbret)
{
    int                 retval = -1;
    transaction_data_t *td = NULL;
    int                 ret;
    cxobj              *xret = NULL;
    yang_stmt          *yspec;

    /* 1. Start transaction */
    if ((td = transaction_new()) == NULL)
        goto done;

    /* Common steps (with validate). Load candidate and running and compute diffs
     * Note this is only call that uses 3-values
     */
    if ((ret = validate_common(h, db, td, &xret)) < 0)
        goto done;

    /* If the confirmed-commit feature is enabled, execute phase 2:
     *  - If a valid confirming-commit, cancel the rollback event
     *  - If a new confirmed-commit, schedule a new rollback event, otherwise
     *  - delete the rollback database
     *
     * Unless, however, this invocation of candidate_commit() was by way of a
     * rollback event, in which case the timers are already cancelled and the
     * caller will cleanup the rollback database.  All that must be done here is
     * to activate it.
     */
    if ((yspec = clicon_dbspec_yang(h)) == NULL) {
        clicon_err(OE_YANG, ENOENT, "No yang spec");
        goto done;
    }

    if (if_feature(yspec, "ietf-netconf", "confirmed-commit")
        && confirmed_commit_state_get(h) != ROLLBACK
        && xe != NULL){
        if (handle_confirmed_commit(h, xe, myid) < 0)
            goto done;
    }
    if (ret == 0){
        if (clixon_xml2cbuf(cbret, xret, 0, 0, NULL, -1, 0) < 0)
            goto done;
        goto fail;
    }
    /* 7. Call plugin transaction commit callbacks */
    if (plugin_transaction_commit_all(h, td) < 0)
        goto done;
    /* After commit, make a post-commit call (sure that all plugins have committed) */
    if (plugin_transaction_commit_done_all(h, td) < 0)
        goto done;
     
    /* Clear cached trees from default values and marking */
    if (xmldb_get0_clear(h, td->td_target) < 0)
        goto done;
    if (xmldb_get0_clear(h, td->td_src) < 0)
        goto done;

    /* 8. Success: Copy candidate to running 
     */
    if (xmldb_copy(h, db, "running") < 0)
        goto done;
    xmldb_modified_set(h, db, 0); /* reset dirty bit */
    /* Here pointers to old (source) tree are obsolete */
    if (td->td_dvec){
        td->td_dlen = 0;
        free(td->td_dvec);
        td->td_dvec = NULL;
    }
    if (td->td_scvec){
        free(td->td_scvec);
        td->td_scvec = NULL;
    }

    /* 9. Call plugin transaction end callbacks */
    plugin_transaction_end_all(h, td);
    
    retval = 1;
 done:
    /* In case of failure (or error), call plugin transaction termination callbacks */
    if (td){
        if (retval < 1)
            plugin_transaction_abort_all(h, td);
        xmldb_get0_free(h, &td->td_target);
        xmldb_get0_free(h, &td->td_src);
        transaction_free(td);
    }
    if (xret)
        xml_free(xret);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Commit the candidate configuration as the device's new current configuration
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK
 * @retval    -1       Error
 * @note NACM:    The server MUST determine the exact nodes in the running
 *  configuration datastore that are actually different and only check
 *  "create", "update", and "delete" access permissions for this set of
 *  nodes, which could be empty.
 *
 * Handling of the first phase of confirmed-commit:
 * First, it must be determined if the given <commit> RPC constitutes a "confirming-commit", roughly meaning:
 *   1) it was issued in the same session as a prior confirmed-commit
 *   2) it bears a <persist-id> element matching the <persist> element that accompanied the prior confirmed-commit
 *
 *   If it is a valid "confirming-commit" and this RPC does not bear another <confirmed/> element, then the
 *   confirmed-commit is complete, the rollback event can be cancelled and the rollback database deleted.
 *
 *   No further action is necessary as the candidate configuration was already copied to the running configuration.
 *
 *   If the RPC does bear another <confirmed/> element, that will be handled in phase two, from within the
 *   candidate_commit() method.
 */
int
from_client_commit(clicon_handle h,
                   cxobj        *xe,
                   cbuf         *cbret,
                   void         *arg,
                   void         *regarg)
{
    int                  retval = -1;
    struct client_entry *ce = (struct client_entry *)arg;
    uint32_t             myid = ce->ce_id;
    uint32_t             iddb;
    cbuf                *cbx = NULL; /* Assist cbuf */
    int                  ret;
    yang_stmt           *yspec;

    if ((yspec = clicon_dbspec_yang(h)) == NULL) {
        clicon_err(OE_YANG, ENOENT, "No yang spec");
        goto done;
    }
    if (if_feature(yspec, "ietf-netconf", "confirmed-commit")) {
        if ((ret = from_client_confirmed_commit(h, xe, myid, cbret)) < 0)
            goto done;
        if (ret == 0)
            goto ok;
    }
    /* Check if target locked by other client */
    iddb = xmldb_islocked(h, "running");
    if (iddb && myid != iddb){
        if ((cbx = cbuf_new()) == NULL){
            clicon_err(OE_XML, errno, "cbuf_new");
            goto done;
        }       
        if (netconf_in_use(cbret, "protocol", "Operation failed, lock is already held") < 0)
            goto done;
        goto ok;
    }
    if ((ret = candidate_commit(h, xe, "candidate", myid, 0, cbret)) < 0){ /* Assume validation fail, nofatal */
        clicon_debug(1, "Commit candidate failed");
        if (ret < 0)
            if (netconf_operation_failed(cbret, "application", clicon_err_reason)< 0)
                goto done;
        goto ok;
    }
    if (ret == 1)
        cprintf(cbret, "<rpc-reply xmlns=\"%s\"><ok/></rpc-reply>", NETCONF_BASE_NAMESPACE);
 ok:
    retval = 0;
 done:
    if (cbx)
        cbuf_free(cbx);
    return retval; /* may be zero if we ignoring errors from commit */
} /* from_client_commit */

/*! Revert the candidate configuration to the current running configuration.
 *
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval  0  OK. This may indicate both ok and err msg back to client
 * @retval     0       OK
 * @retval    -1       Error
 * NACM: No datastore permissions are needed.
 */
int
from_client_discard_changes(clicon_handle h,
                            cxobj        *xe,
                            cbuf         *cbret,
                            void         *arg,
                            void         *regarg)
{
    int                  retval = -1;
    struct client_entry *ce = (struct client_entry *)arg;
    uint32_t             myid = ce->ce_id;
    uint32_t             iddb;
    cbuf                *cbx = NULL; /* Assist cbuf */
    
    /* Check if target locked by other client */
    iddb = xmldb_islocked(h, "candidate");
    if (iddb && myid != iddb){
        if ((cbx = cbuf_new()) == NULL){
            clicon_err(OE_XML, errno, "cbuf_new");
            goto done;
        }       
        cprintf(cbx, "<session-id>%u</session-id>", iddb);
        if (netconf_lock_denied(cbret, cbuf_get(cbx), "Operation failed, lock is already held") < 0)
            goto done;
        goto ok;
    }
    if (xmldb_copy(h, "running", "candidate") < 0){
        if (netconf_operation_failed(cbret, "application", clicon_err_reason)< 0)
            goto done;
        goto ok;
    }
    xmldb_modified_set(h, "candidate", 0); /* reset dirty bit */
    cprintf(cbret, "<rpc-reply xmlns=\"%s\"><ok/></rpc-reply>", NETCONF_BASE_NAMESPACE);
 ok:
    retval = 0;
 done:
    if (cbx)
        cbuf_free(cbx);
    return retval; /* may be zero if we ignoring errors from commit */
}

/*! Validates the contents of the specified configuration.
 * @param[in]  h       Clicon handle 
 * @param[in]  xe      Request: <rpc><xn></rpc> 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @param[in]  arg     client-entry
 * @param[in]  regarg  User argument given at rpc_callback_register() 
 * @retval     0       OK. This may indicate both ok and err msg back to client 
 *                     (eg invalid)
 * @retval    -1       Error
 */
int
from_client_validate(clicon_handle h,
                     cxobj        *xe,
                     cbuf         *cbret,
                     void         *arg,
                     void         *regarg)
{
    int   retval = -1;
    int   ret;
    char *db;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((db = netconf_db_find(xe, "source")) == NULL){
        if (netconf_missing_element(cbret, "protocol", "source", NULL) < 0)
            goto done;
        goto ok;
    }
    if ((ret = candidate_validate(h, db, cbret)) < 0)
        goto done;
    if (ret == 1)
        cprintf(cbret, "<rpc-reply xmlns=\"%s\"><ok/></rpc-reply>", NETCONF_BASE_NAMESPACE);
 ok:
    retval = 0;
 done:
    return retval;
} /* from_client_validate */

/*! Restart specific backend plugins without full backend restart
 * Note, depending on plugin callbacks, there may be other dependencies which may make this
 * difficult in the general case.
 */
int
from_client_restart_one(clicon_handle h,
                        clixon_plugin_t *cp,
                        cbuf         *cbret)
{
    int                 retval = -1;
    char               *db = "tmp";
    transaction_data_t *td = NULL;
    plgreset_t         *resetfn;          /* Plugin auth */
    int                 ret;
    cxobj              *xerr = NULL;
    yang_stmt          *yspec;
    int                 i;
    cxobj              *xn;
    void               *wh = NULL;
    
    yspec =  clicon_dbspec_yang(h);
    if (xmldb_db_reset(h, db) < 0)
        goto done;
    /* Application may define extra xml in its reset function*/
    if ((resetfn = clixon_plugin_api_get(cp)->ca_reset) != NULL){
        wh = NULL;
        if (plugin_context_check(h, &wh, clixon_plugin_name_get(cp), __FUNCTION__) < 0)
            goto done;
        if ((retval = resetfn(h, db)) < 0) {
            clicon_debug(1, "plugin_start() failed");
            goto done;
        }
        if (plugin_context_check(h, &wh, clixon_plugin_name_get(cp), __FUNCTION__) < 0)
            goto done;
    }
    /* 1. Start transaction */
    if ((td = transaction_new()) == NULL)
        goto done;
    /* This is the state we are going to */
    if (xmldb_get0(h, "running", YB_MODULE, NULL, "/", 0, 0, &td->td_target, NULL, NULL) < 0)
        goto done;
    if ((ret = xml_yang_validate_all_top(h, td->td_target, &xerr)) < 0)
        goto done;
    if (ret == 0){
        if (clixon_xml2cbuf(cbret, xerr, 0, 0, NULL, -1, 0) < 0)
            goto done;
        goto fail;
    }
    /* This is the state we are going from */
    if (xmldb_get0(h, db, YB_MODULE, NULL, "/", 0, 0, &td->td_src, NULL, NULL) < 0)
        goto done;

    /* 3. Compute differences */
    if (xml_diff(td->td_src,
                 td->td_target,
                 &td->td_dvec,      /* removed: only in running */
                 &td->td_dlen,
                 &td->td_avec,      /* added: only in candidate */
                 &td->td_alen,
                 &td->td_scvec,     /* changed: original values */
                 &td->td_tcvec,     /* changed: wanted values */
                 &td->td_clen) < 0)
        goto done;

    /* Mark as changed in tree */
    for (i=0; i<td->td_dlen; i++){ /* Also down */
        xn = td->td_dvec[i];
        xml_flag_set(xn, XML_FLAG_DEL);
        xml_apply(xn, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_DEL);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    for (i=0; i<td->td_alen; i++){ /* Also down */
        xn = td->td_avec[i];
        xml_flag_set(xn, XML_FLAG_ADD);
        xml_apply(xn, CX_ELMNT, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_ADD);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    for (i=0; i<td->td_clen; i++){ /* Also up */
        xn = td->td_scvec[i];
        xml_flag_set(xn, XML_FLAG_CHANGE);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
        xn = td->td_tcvec[i];
        xml_flag_set(xn, XML_FLAG_CHANGE);
        xml_apply_ancestor(xn, (xml_applyfn_t*)xml_flag_set, (void*)XML_FLAG_CHANGE);
    }
    /* Call plugin transaction start callbacks */
    if (plugin_transaction_begin_one(cp, h, td) < 0)
        goto fail;
    /* Make generic validation on all new or changed data.
       Note this is only call that uses 3-values */
    if ((ret = generic_validate(h, yspec, td, &xerr)) < 0)
        goto done;
    if (ret == 0){
        if (clixon_xml2cbuf(cbret, xerr, 0, 0, NULL, -1, 0) < 0)
            goto done;
        goto fail;
    }
    /* Call validate callback in this plugin */
    if (plugin_transaction_validate_one(cp, h, td) < 0)
        goto fail;
    if (plugin_transaction_complete_one(cp, h, td) < 0)
        goto fail;
    /* Call commit callback in this plugin */
    if (plugin_transaction_commit_one(cp, h, td) < 0)
        goto fail;
    if (plugin_transaction_commit_done_one(cp, h, td) < 0)
        goto fail;
    /* Finalize */
    if (plugin_transaction_end_one(cp, h, td) < 0)
        goto fail;
    retval = 1;
 done:
    if (td){
         xmldb_get0_free(h, &td->td_target);
         transaction_free(td);
     }
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Reset running and start in failsafe mode. If no failsafe then quit.
 * 
 * param[in] h     Clixon handle
 * param[in] phase Debug string
  Typically done when startup status is not OK so

failsafe      ----------------------+
                            reset    \ commit
running                   ----|-------+---------------> RUNNING FAILSAFE
                           \
tmp                         |---------------------->
 */
int
load_failsafe(clicon_handle h,
              char         *phase)
{
    int   retval = -1;
    int   ret;
    char *db = "failsafe";
    cbuf *cbret = NULL;

    phase = phase == NULL ? "(unknown)" : phase;

    if ((cbret = cbuf_new()) == NULL){
        clicon_err(OE_XML, errno, "cbuf_new");
        goto done;
    }
    if ((ret = xmldb_exists(h, db)) < 0)
        goto done;
    if (ret == 0){ /* No it does not exist, fail */
        clicon_err(OE_DB, 0, "%s failed and no Failsafe database found, exiting", phase);
        goto done;
    }
    /* Copy original running to tmp as backup (restore if error) */
    if (xmldb_copy(h, "running", "tmp") < 0)
        goto done;
    if (xmldb_db_reset(h, "running") < 0)
        goto done;
    ret = candidate_commit(h, NULL, db, 0, 0, cbret);
    if (ret != 1)
        if (xmldb_copy(h, "tmp", "running") < 0)
            goto done;
    if (ret < 0)
        goto done;
    if (ret == 0){
        clicon_err(OE_DB, 0, "%s failed, Failsafe database validation failed %s", phase, cbuf_get(cbret));
        goto done;
    }
    clicon_log(LOG_NOTICE, "%s failed, Failsafe database loaded ", phase);
    retval = 0;
    done:
    if (cbret)
        cbuf_free(cbret);
    return retval;
}
