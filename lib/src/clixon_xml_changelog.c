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

 * YANG module revision change management. 
 * See draft-wang-netmod-module-revision-management-01
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syslog.h>
#include <fcntl.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_string.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_debug.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_yang_module.h"
#include "clixon_yang_parse_lib.h"
#include "clixon_netconf_lib.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xml_map.h"
#include "clixon_xml_io.h"
#include "clixon_validate.h"
#include "clixon_xml_changelog.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"

static int
changelog_rename(clixon_handle h,
                 cxobj        *xt,
                 cxobj        *xw,
                 cvec         *nsc,
                 char         *tag)
{
    int     retval = -1;
    xp_ctx *xctx = NULL;
    char   *str = NULL;

    if (tag == NULL){
        clixon_err(OE_XML, 0, "tag required");
        goto done;
    }
    if (xpath_vec_ctx(xw, nsc, tag, 0, &xctx) < 0)
        goto done;
    if (ctx2string(xctx, &str) < 0)
        goto done;
    if (!strlen(str)){
        clixon_err(OE_XML, 0, "invalid rename tag: \"%s\"", str);
        goto done;
    }
    if (xml_name_set(xw, str) < 0)
        goto done;
    // ok:
    retval = 1;
 done:
    if (xctx)
        ctx_free(xctx);
    if (str)
        free(str);
    return retval;
    // fail:
    retval = 0;
    goto done;
}

/* replace target XML */
static int
changelog_replace(clixon_handle h,
                  cxobj        *xt,
                  cxobj        *xw,
                  cxobj        *xnew)
{
    int    retval = -1;
    cxobj *x;

    /* create a new node by parsing fttransform string and insert it at 
       target */
    if (xnew == NULL){
        clixon_err(OE_XML, 0, "new required");
        goto done;
    }
    /* replace: remove all children of target */
    while ((x = xml_child_i(xw, 0)) != NULL)
        if (xml_purge(x) < 0)
            goto done;
    /* replace: first single node under <new> */
    if (xml_child_nr(xnew) != 1){
        clixon_err(OE_XML, 0, "Single child to <new> required");
        goto done;
    }
    x = xml_child_i(xnew, 0);
    /* Copy from xnew to (now) empty target */
    if (xml_copy(x, xw) < 0)
        goto done;
    retval = 1;
 done:
    return retval;
}

/* create a new node by parsing "new" and insert it at 
   target */
static int
changelog_insert(clixon_handle h,
                 cxobj        *xt,
                 cxobj        *xw,
                 cxobj        *xnew)
{
    int    retval = -1;
    cxobj *x;

    if (xnew == NULL){
        clixon_err(OE_XML, 0, "new required");
        goto done;
    }
    /* replace: add all new children to target */
    while ((x = xml_child_i(xnew, 0)) != NULL)
        if (xml_addsub(xw, x) < 0)
            goto done;
    // ok:
    retval = 1;
 done:
    return retval;
    // fail:
    retval = 0;
    goto done;
}

/* delete target */
static int
changelog_delete(clixon_handle h,
                 cxobj        *xt,
                 cxobj        *xw)
{
    int retval = -1;

    if (xml_purge(xw) < 0)
        goto done;
    retval = 1;
 done:
    return retval;
}

/* Move target node to location */
static int
changelog_move(clixon_handle h,
               cxobj        *xt,
               cxobj        *xw,
               cvec         *nsc,
               char         *dst)
{
    int    retval = -1;
    cxobj *xp;       /* destination parent node */

    if ((xp = xpath_first(xt, nsc, "%s", dst)) == NULL){
        clixon_err(OE_XML, 0, "path required");
        goto done;
    }
    if (xml_addsub(xp, xw) < 0)
        goto done;
    retval = 1;
 done:
    return retval;
}

/*! Perform a changelog operation
 *
 * @param[in]  h   Clixon handle
 * @param[in]  xt  XML to upgrade
 * @param[in]  xi  Changelog item
 * @retval     0   OK
 * @retval    -1   Error
 * @note XXX error handling!
 * @note XXX xn --> xt  xpath may not match
*/
static int
changelog_op(clixon_handle h,
             cxobj        *xt,
             cxobj        *xi)

{
    int     retval = -1;
    char   *op;
    char   *whenxpath;   /* xpath to when */
    char   *tag;         /* xpath to extra path (move) */
    char   *dst;         /* xpath to extra path (move) */
    cxobj  *xnew;        /* new xml (insert, replace) */
    char   *wxpath;      /* xpath to where (target-node) */
    cxobj **wvec = NULL; /* Vector of where(target) nodes */
    size_t  wlen;
    cxobj  *xw;
    int     ret;
    xp_ctx *xctx = NULL;
    int     i;
    cvec   *nsc = NULL;

    /* Get namespace context from changelog item */
    if (xml_nsctx_node(xi, &nsc) < 0)
        goto done;
    if ((op = xml_find_body(xi, "op")) == NULL)
        goto ok;
    /* get common variables that may be used in the operations below */
    tag = xml_find_body(xi, "tag");
    dst = xml_find_body(xi, "dst");
    xnew = xml_find(xi, "new");
    whenxpath = xml_find_body(xi, "when");
    if ((wxpath = xml_find_body(xi, "where")) == NULL)
        goto ok;
    /* Get vector of target nodes meeting the where requirement */
    if (xpath_vec(xt, nsc, "%s", &wvec, &wlen, wxpath) < 0)
       goto done;
   for (i=0; i<wlen; i++){
       xw = wvec[i];
       /* If 'when' exists and is false, skip this target */
       if (whenxpath){
           if (xpath_vec_ctx(xw, nsc, whenxpath, 0, &xctx) < 0)
               goto done;
           if ((ret = ctx2boolean(xctx)) < 0)
               goto done;
           if (xctx){
               ctx_free(xctx);
               xctx = NULL;
           }
           if (ret == 0)
               continue;
       }
       /* Now switch on operation */
       if (strcmp(op, "rename") == 0){
           ret = changelog_rename(h, xt, xw, nsc, tag);
       }
       else if (strcmp(op, "replace") == 0){
           ret = changelog_replace(h, xt, xw, xnew);
       }
       else if (strcmp(op, "insert") == 0){
           ret = changelog_insert(h, xt, xw, xnew);
       }
       else if (strcmp(op, "delete") == 0){
           ret = changelog_delete(h, xt, xw);
       }
       else if (strcmp(op, "move") == 0){
           ret = changelog_move(h, xt, xw, nsc, dst);
       }
       else{
           clixon_err(OE_XML, 0, "Unknown operation: %s", op);
           goto done;
       }
       if (ret < 0)
           goto done;
       if (ret == 0)
           goto fail;
   }
 ok:
    retval = 1;
 done:
    if (nsc)
        xml_nsctx_free(nsc);
    if (wvec)
        free(wvec);
    if (xctx)
        ctx_free(xctx);
    return retval;
 fail:
    retval = 0;
    clixon_debug(CLIXON_DBG_XML, "fail op:%s", op);
    goto done;
}
    
/*! Iterate through one changelog item
 *
 * @param[in]  h   Clixon handle
 * @param[in]  xt  Changelog list
 * @param[in]  xn  XML to upgrade
 * @retval     0   OK
 * @retval    -1   Error
 */
static int
changelog_iterate(clixon_handle h,
                  cxobj        *xt,
                  cxobj        *xch)

{
    int        retval = -1;
    cxobj    **vec = NULL;
    size_t     veclen;
    int        ret;
    int        i;

    if (xpath_vec(xch, NULL, "step", &vec, &veclen) < 0)
        goto done;
    /* Iterate through changelog items */
    for (i=0; i<veclen; i++){
        if ((ret = changelog_op(h, xt, vec[i])) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
    retval = 1;
 done:
    clixon_debug(CLIXON_DBG_XML, "retval: %d", retval);
    if (vec)
        free(vec);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Automatic upgrade using changelog
 *
 * @param[in]  h       Clixon handle 
 * @param[in]  xt      Top-level XML tree to be updated (includes other ns as well)
 * @param[in]  ns      Namespace of module (for info)
 * @param[in]  op      One of XML_FLAG_ADD, _DEL, _CHANGE
 * @param[in]  from    From revision on the form YYYYMMDD
 * @param[in]  to      To revision on the form YYYYMMDD (0 not in system)
 * @param[in]  arg     User argument given at rpc_callback_register() 
 * @param[out] cbret   Return xml tree, eg <rpc-reply>..., <rpc-error.. 
 * @retval     1       OK
 * @retval     0       Invalid
 * @retval    -1       Error
 * @see upgrade_callback_register  where this function should be registered
 */
int
xml_changelog_upgrade(clixon_handle h,
                      cxobj        *xt,
                      char         *ns,
                      uint16_t      op,
                      uint32_t      from,
                      uint32_t      to,
                      void         *arg,
                      cbuf         *cbret)
{
    int        retval = -1;
    cxobj     *xchlog; /* changelog */
    cxobj    **vec = NULL;
    cxobj     *xch;
    size_t     veclen;
    char      *b;
    int        ret;
    int        i;
    uint32_t   f;
    uint32_t   t;

    /* Check if changelog enabled */
    if (!clicon_option_bool(h, "CLICON_XML_CHANGELOG"))
        goto ok;
    /* Get changelog */
    if ((xchlog = clicon_xml_changelog_get(h)) == NULL)
        goto ok;

    /* Iterate and find relevant changelog entries in the interval:
     * - find all changelogs in the interval: [from, to]
     * - note it t=0 then no changelog is applied
     */
    if (xpath_vec(xchlog, NULL, "changelog[namespace=\"%s\"]",
                  &vec, &veclen, ns) < 0)
        goto done;
    /* Get all changelogs in the interval [from,to]*/
    for (i=0; i<veclen; i++){
        xch = vec[i];
        f = t = 0;
        if ((b = xml_find_body(xch, "revfrom")) != NULL)
            if (ys_parse_date_arg(b, &f) < 0)
                goto done;
        if ((b = xml_find_body(xch, "revision")) != NULL)
            if (ys_parse_date_arg(b, &t) < 0)
                goto done;
        if ((f && from>f) || to<t)
            continue;
        if ((ret = changelog_iterate(h, xt, xch)) < 0)
            goto done;
        if (ret == 0)
            goto fail;
    }
 ok:
    retval = 1;
 done:
    if (vec)
        free(vec);
    return retval;
 fail:
    retval = 0;
    goto done;
}

/*! Initialize module revision. read changelog, etc
 */
int
clixon_xml_changelog_init(clixon_handle h)
{
    int        retval = -1;
    char      *filename;
    FILE      *fp = NULL;
    cxobj     *xt = NULL;
    yang_stmt *yspec;
    int        ret;
    cxobj     *xret = NULL;
    cbuf      *cbret = NULL;

    yspec = clicon_dbspec_yang(h);
    if ((filename = clicon_option_str(h, "CLICON_XML_CHANGELOG_FILE")) != NULL){
        if ((fp = fopen(filename, "r")) == NULL){
            clixon_err(OE_UNIX, errno, "fopen(%s)", filename);
            goto done;
        }
        if (clixon_xml_parse_file(fp, YB_MODULE, yspec, &xt, NULL) < 0)
            goto done;
        if (xml_rootchild(xt, 0, &xt) < 0)
            goto done;
        if ((ret = xml_yang_validate_all(h, xt, &xret)) < 0)
            goto done;
        if (ret==1 && (ret = xml_yang_validate_add(h, xt, &xret)) < 0)
            goto done;
        if (ret == 0){ /* validation failed */
            if ((cbret = cbuf_new()) ==NULL){
                clixon_err(OE_XML, errno, "cbuf_new");
                goto done;
            }
            clixon_err_netconf(h, OE_YANG, 0, xret, "validation failed");
        }
        if (clicon_xml_changelog_set(h, xt) < 0)
            goto done;
        xt = NULL;
    }
    retval = 0;
 done:
    if (cbret)
        cbuf_free(cbret);
    if (xret)
        xml_free(xret);
    if (fp)
        fclose(fp);
    if (xt)
        xml_free(xt);
    return retval;
}

/*! Given a top-level XML tree and a namespace, return a vector of matching XML nodes
 *
 * @param[in]  h         Clixon handle
 * @param[in]  xt        Top-level XML tree, with children marked with namespaces
 * @param[in]  ns        The namespace to select
 * @param[out] vecp      Vector containining XML nodes w namespace. Null-terminated.
 * @param[out] veclenp   Length of vector
 * @retval     0         OK
 * @retval    -1         Error
 * @note  Need to free vec after use with free()
 * Example 
 *   xt ::= <config><a xmlns="urn:example:a"/><aaa xmlns="urn:example:a"/><a xmlns="urn:example:b"/></config
 *   namespace ::= urn:example:a
 * out: 
 *   vec ::= [<a xmlns="urn:example:a"/>, <aaa xmlns="urn:example:a"/>, NULL]
 */
int
xml_namespace_vec(clixon_handle h,
                  cxobj        *xt,
                  char         *ns,
                  cxobj      ***vecp,
                  size_t       *veclenp)
{
    int     retval = -1;
    cxobj **xvec = NULL;
    size_t  xlen;
    cxobj  *xc;
    char   *ns0;
    int     i;

    /* Allocate upper bound on length (ie could be too large) + a NULL element
     * (event though we use veclen)
     */
    xlen = xml_child_nr_type(xt, CX_ELMNT)+1;
    if ((xvec = calloc(xlen, sizeof(cxobj*))) == NULL){
        clixon_err(OE_UNIX, errno, "calloc");
        goto done;
    }
    /* Iterate and find xml nodes with assoctaed namespace */
    xc = NULL;
    i = 0;
    while ((xc = xml_child_each(xt, xc, CX_ELMNT)) != NULL) {
        if (xml2ns(xc, NULL, &ns0) < 0) /* Get namespace of XML */
            goto done;
        if (strcmp(ns, ns0))
            continue; /* no match */
        xvec[i++] = xc;
    }
    *vecp = xvec;
    xvec = NULL;
    *veclenp = i;
    retval = 0;
 done:
    if (xvec)
        free(xvec);
    return retval;
}
