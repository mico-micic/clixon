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

 * Clixon XML object parse and print functions
 * @see https://www.w3.org/TR/2008/REC-xml-20081126
 *      https://www.w3.org/TR/2009/REC-xml-names-20091208
 * Canonical XML version (just for info)
 *      https://www.w3.org/TR/xml-c14n
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <limits.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_string.h"
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_log.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_options.h" 
#include "clixon_yang_module.h"
#include "clixon_xml_bind.h"
#include "clixon_xml_vec.h"
#include "clixon_xml_sort.h"
#include "clixon_xml_nsctx.h"
#include "clixon_xml_parse.h"
#include "clixon_xml_io.h"

/*
 * Constants
 */
/* Size of xml read buffer */
#define BUFLEN 1024  

/*------------------------------------------------------------------------
 * XML printing functions. Output a parse tree to file, string cligen buf
 *------------------------------------------------------------------------*/

/*! Print an XML tree structure to an output stream and encode chars "<>&"
 *
 * @param[in]   f          UNIX output stream
 * @param[in]   xn         Clicon xml tree
 * @param[in]   level      How many spaces to insert before each line
 * @param[in]   pretty     Insert \n and spaces to make the xml more readable.
 * @param[in]   prefix     Add string to beginning of each line (if pretty)
 * @param[in]   fn         Callback to make print function
 * @param[in]   autocliext How to handle autocli extensions: 0: ignore 1: follow
 * @retval      0          OK
 * @retval     -1          Error
 * @see clixon_xml2cbuf
 * One can use clixon_xml2cbuf to get common code, but using fprintf is
 * much faster than using cbuf and then printing that,...
 *
 */
static int
xml2file_recurse(FILE             *f, 
                 cxobj            *x, 
                 int               level, 
                 int               pretty,
                 char             *prefix,
                 clicon_output_cb *fn,
                 int               autocliext)
{
    int        retval = -1;
    char      *name;
    char      *namespace;
    cxobj     *xc;
    int        hasbody;
    int        haselement;
    char      *val;
    char      *encstr = NULL; /* xml encoded string */
    int        exist = 0;
    yang_stmt *y;
    int        level1;
        
    if (x == NULL)
        goto ok;
    level1 = level*PRETTYPRINT_INDENT;
    if (prefix)
        level1 -= strlen(prefix);
    if (autocliext &&
        (y = xml_spec(x)) != NULL){
        if (yang_extension_value(y, "hide-show", CLIXON_AUTOCLI_NS, &exist, NULL) < 0)
            goto done;
        if (exist)
            goto ok;
    }
    name = xml_name(x);
    namespace = xml_prefix(x);
    switch(xml_type(x)){
    case CX_BODY:
        if ((val = xml_value(x)) == NULL) /* incomplete tree */
            break;
        if (xml_chardata_encode(&encstr, "%s", val) < 0)
            goto done;
        (*fn)(f, "%s", encstr);
        break;
    case CX_ATTR:
        (*fn)(f, " ");
        if (namespace)
            (*fn)(f, "%s:", namespace);
        (*fn)(f, "%s=\"%s\"", name, xml_value(x));
        break;
    case CX_ELMNT:
        if (pretty && prefix)
            (*fn)(f, "%s", prefix);
        (*fn)(f, "%*s<", pretty?level1:0, "");
        if (namespace)
            (*fn)(f, "%s:", namespace);
        (*fn)(f, "%s", name);
        hasbody = 0;
        haselement = 0;
        xc = NULL;
        /* print attributes only */
        while ((xc = xml_child_each(x, xc, -1)) != NULL) {
            switch (xml_type(xc)){
            case CX_ATTR:
                if (xml2file_recurse(f, xc, level+1, pretty, prefix, fn, autocliext) <0)
                    goto done;
                break;
            case CX_BODY:
                hasbody=1;
                break;
            case CX_ELMNT:
                haselement=1;
                break;
            default:
                break;
            }
        }
        /* Check for special case <a/> instead of <a></a>:
         * Ie, no CX_BODY or CX_ELMNT child.
         */
        if (hasbody==0 && haselement==0) 
            (*fn)(f, "/>");
        else{
            (*fn)(f, ">");
            if (pretty && hasbody == 0){
                    (*fn)(f, "\n");
            }
            xc = NULL;
            while ((xc = xml_child_each(x, xc, -1)) != NULL) {
                if (xml_type(xc) != CX_ATTR)
                    if (xml2file_recurse(f, xc, level+1, pretty, prefix, fn, autocliext) <0)
                        goto done;
            }
            if (pretty && hasbody==0){
                if (pretty && prefix)
                    (*fn)(f, "%s", prefix);
                (*fn)(f, "%*s", level1, "");
            }
            (*fn)(f, "</");
            if (namespace)
                (*fn)(f, "%s:", namespace);
            (*fn)(f, "%s>", name);
        }
        if (pretty){
            (*fn)(f, "\n");
        }
        break;
    default:
        break;
    }/* switch */
 ok:
    retval = 0;
 done:
    if (encstr)
        free(encstr);
    return retval;
}

/*! Print an XML tree structure to an output stream and encode chars "<>&"
 *
 * @param[in]  f       Output file
 * @param[in]  xn      XML tree
 * @param[in]  level   How many spaces to insert before each line
 * @param[in]  pretty  Insert \n and spaces to make the xml more readable.
 * @param[in]  prefix  Add string to beginning of each line (if pretty)
 * @param[in]  fn      File print function (if NULL, use fprintf)
 * @param[in]  skiptop 0: Include top object 1: Skip top-object, only children, 
 * @param[in]  autocliext How to handle autocli extensions: 0: ignore 1: follow
 * @retval     0       OK
 * @retval    -1       Error
 * @see clixon_xml2cbuf print to a cbuf string
 * @note There is a slight "layer violation" with the autocli parameter: it should normally be set
 *       for CLI calls, but not for others.
 */
int
clixon_xml2file(FILE             *f, 
                cxobj            *xn, 
                int               level, 
                int               pretty,
                char             *prefix,
                clicon_output_cb *fn,
                int               skiptop,
                int               autocliext)
{
    int   retval = 1;
    cxobj *xc;

    if (fn == NULL)
        fn = fprintf;
    if (skiptop){
        xc = NULL;
        while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL)
            if (xml2file_recurse(f, xc, level, pretty, prefix, fn, autocliext) < 0)
                goto done;
    }
    else {
        if (xml2file_recurse(f, xn, level, pretty, prefix, fn, autocliext) < 0)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Print an XML tree structure to an output stream
 *
 * Utility function eg in gdb. For code, use clixon_xml2file
 * @param[in]   f           UNIX output stream
 * @param[in]   xn          clicon xml tree
 * @see clixon_xml2cbuf
 * @see clixon_xml2cbuf_cb print using a callback
 */
int
xml_print(FILE  *f, 
          cxobj *x)
{
    return xml2file_recurse(f, x, 0, 1, NULL, fprintf, 0);
}

/*! Dump cxobj structure with pointers and flags for debugging, internal function
 */
static int
xml_dump1(FILE  *f, 
          cxobj *x,
          int    indent)
{
    cxobj *xc;
    
    if (xml_type(x) != CX_ELMNT)
        return 0;
    fprintf(stderr, "%*s %s(%s)",
            indent*3, "",
            //      x,
            xml_name(x),
            xml_type2str(xml_type(x)));
    if (xml_flag(x, XML_FLAG_ADD))
        fprintf(stderr, " add");
    if (xml_flag(x, XML_FLAG_DEL))
        fprintf(stderr, " delete");
    if (xml_flag(x, XML_FLAG_CHANGE))
        fprintf(stderr, " change");
    if (xml_flag(x, XML_FLAG_MARK))
        fprintf(stderr, " mark");
    fprintf(stderr, "\n");
    xc = NULL;
    while ((xc = xml_child_each(x, xc, -1)) != NULL) {
        xml_dump1(f, xc, indent+1);
    }
    return 0;
}
         
/*! Dump cxobj structure with pointers and flags for debugging
 *
 * @param[in]   f    UNIX output stream
 * @param[in]   x    Clixon xml tree
 * @see xml_print
 */
int
xml_dump(FILE  *f, 
         cxobj *x)
{
    return xml_dump1(f, x, 0);
}

/*! Internal: print  XML tree structure to a cligen buffer and encode chars "<>&"
 *
 * @param[in,out] cb       Cligen buffer to write to
 * @param[in]     xn       Clixon xml tree
 * @param[in]     level    Indentation level for prettyprint
 * @param[in]     pretty   Insert \n and spaces to make the xml more readable.
 * @param[in]     prefix   Add string to beginning of each line (if pretty)
 * @param[in]     depth    Limit levels of child resources: -1 is all, 0 is none, 1 is node itself
 */
static int
clixon_xml2cbuf1(cbuf   *cb, 
                 cxobj  *x, 
                 int     level,
                 int     pretty,
                 char   *prefix,
                 int32_t depth)
{
    int    retval = -1;
    cxobj *xc;
    char  *name;
    int    hasbody;
    int    haselement;
    char  *namespace;
    char  *val;
    int    level1;
    
    if (depth == 0)
        goto ok;
    level1 = level*PRETTYPRINT_INDENT;
    if (prefix)
        level1 -= strlen(prefix);
    name = xml_name(x);
    namespace = xml_prefix(x);
    switch(xml_type(x)){
    case CX_BODY:
        if ((val = xml_value(x)) == NULL) /* incomplete tree */
            break;
        if (xml_chardata_cbuf_append(cb, val) < 0)
            goto done;
        break;
    case CX_ATTR:
        cbuf_append_str(cb, " ");
        if (namespace){
            cbuf_append_str(cb, namespace);
            cbuf_append_str(cb, ":");
        }
        cprintf(cb, "%s=\"%s\"", name, xml_value(x));
        break;
    case CX_ELMNT:
        if (pretty){
            if (prefix)
                cprintf(cb, "%s", prefix);
            cprintf(cb, "%*s<", level1, "");
        }
        else
            cbuf_append_str(cb, "<");
        if (namespace){
            cbuf_append_str(cb, namespace);
            cbuf_append_str(cb, ":");
        }
        cbuf_append_str(cb, name);
        hasbody = 0;
        haselement = 0;
        xc = NULL;
        /* print attributes only */
        while ((xc = xml_child_each(x, xc, -1)) != NULL) 
            switch (xml_type(xc)){
            case CX_ATTR:
                if (clixon_xml2cbuf1(cb, xc, level+1, pretty, prefix, -1) < 0)
                    goto done;
                break;
            case CX_BODY:
                hasbody=1;
                break;
            case CX_ELMNT:
                haselement=1;
                break;
            default:
                break;
            }
        /* Check for special case <a/> instead of <a></a> */
        if (hasbody==0 && haselement==0) 
            cbuf_append_str(cb, "/>");
        else{
            cbuf_append_str(cb, ">");
            if (pretty && hasbody == 0)
                cbuf_append_str(cb, "\n");
            xc = NULL;
            while ((xc = xml_child_each(x, xc, -1)) != NULL) 
                if (xml_type(xc) != CX_ATTR)
                    if (clixon_xml2cbuf1(cb, xc, level+1, pretty, prefix, depth-1) < 0)
                        goto done;
            if (pretty && hasbody == 0){
                if (prefix)
                    cprintf(cb, "%s", prefix);
                cprintf(cb, "%*s", level1, "");
            }
            cbuf_append_str(cb, "</");
            if (namespace){
                cbuf_append_str(cb, namespace);
                cbuf_append_str(cb, ":");
            }
            cbuf_append_str(cb, name);
            cbuf_append_str(cb, ">");
        }
        if (pretty)
            cbuf_append_str(cb, "\n");
        break;
    default:
        break;
    }/* switch */
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Print an XML tree structure to a cligen buffer and encode chars "<>&"
 *
 * @param[in,out] cb      Cligen buffer to write to
 * @param[in]     xn      Top-level xml object
 * @param[in]     level   Indentation level for pretty
 * @param[in]     pretty  Insert \n and spaces to make the xml more readable.
 * @param[in]     prefix  Add string to beginning of each line (if pretty)
 * @param[in]     depth   Limit levels of child resources: -1: all, 0: none, 1: node itself
 * @param[in]     skiptop 0: Include top object 1: Skip top-object, only children, 
 * @retval        0       OK
 * @retval        -1      Error
 * Depth is used in NACM
 * @code
 *   cbuf *cb = cbuf_new();
 *   if (clixon_xml2cbuf(cb, xn, 0, 1, NULL, -1, 0) < 0)
 *     goto err;
 *   fprintf(stderr, "%s", cbuf_get(cb));
 *   cbuf_free(cb);
 * @endcode
 * @see  clixon_xml2file
 */
int
clixon_xml2cbuf(cbuf   *cb,
                cxobj  *xn,
                int     level,
                int     pretty,
                char   *prefix,
                int32_t depth,
                int     skiptop)
{
    int    retval = -1;
    cxobj *xc;
    
    if (skiptop){
        xc = NULL;
        while ((xc = xml_child_each(xn, xc, CX_ELMNT)) != NULL)
            if (clixon_xml2cbuf1(cb, xc, level, pretty, prefix, depth) < 0)
                goto done;
    }
    else {
        if (clixon_xml2cbuf1(cb, xn, level, pretty, prefix, depth) < 0)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Print actual xml tree datastructures (not xml), mainly for debugging
 * @param[in,out] cb          Cligen buffer to write to
 * @param[in]     xn          Clicon xml tree
 * @param[in]     level       Indentation level
 */
int
xmltree2cbuf(cbuf  *cb, 
             cxobj *x,
             int    level)
{
    cxobj *xc;
    int    i;

    for (i=0; i<level*PRETTYPRINT_INDENT; i++)
        cprintf(cb, " ");
    if (xml_type(x) != CX_BODY)
        cprintf(cb, "%s", xml_type2str(xml_type(x)));
    if (xml_prefix(x)==NULL)
        cprintf(cb, " %s", xml_name(x));
    else
        cprintf(cb, " %s:%s", xml_prefix(x), xml_name(x));
    if (xml_value(x))
        cprintf(cb, " value:\"%s\"", xml_value(x));
    if (xml_flag(x, 0xff))
        cprintf(cb, " flags:0x%x", xml_flag(x, 0xff));
    if (xml_child_nr(x))
        cprintf(cb, " {");
    cprintf(cb, "\n");
    xc = NULL;
    while ((xc = xml_child_each(x, xc, -1)) != NULL) 
        xmltree2cbuf(cb, xc, level+1);
    if (xml_child_nr(x)){
        for (i=0; i<level*PRETTYPRINT_INDENT; i++)
            cprintf(cb, " ");
        cprintf(cb, "}\n");
    }
    return 0;
}

/*--------------------------------------------------------------------
 * XML parsing functions. Create XML parse tree from string and file.
 *--------------------------------------------------------------------*/
/*! Common internal xml parsing function string to parse-tree
 *
 * Given a string containing XML, parse into existing XML tree and return
 * @param[in]     str   Pointer to string containing XML definition. 
 * @param[in]     yb    How to bind yang to XML top-level when parsing
 * @param[in]     yspec Yang specification (only if bind is TOP or CONFIG)
 * @param[in,out] xtop  Top of XML parse tree. Assume created. Holds new tree.
 * @param[out]    xerr  Reason for failure (yang assignment not made)
 * @retval        1     Parse OK and all yang assignment made
 * @retval        0     Parse OK but yang assigment not made (or only partial) and xerr set
 * @retval       -1     Error with clicon_err called. Includes parse error
 * @see clixon_xml_parse_file
 * @see clixon_xml_parse_string
 * @see _json_parse
 * @note special case is empty XML where the parser is not invoked.
 * It is questionable empty XML is legal. From https://www.w3.org/TR/2008/REC-xml-20081126 Sec 2.1:
 *    A well-formed document ... contains one or more elements.
 * But in clixon one can invoke a parser on a sub-part of a document where it makes sense to accept
 * an empty XML. For example where an empty config: <config></config> is parsed.
 * In other cases, such as receiving netconf ]]>]]> it should represent a complete document and 
 * therefore not well-formed.
 * Therefore checking for empty XML must be done by a calling function which knows wether the 
 * the XML represents a full document or not.
 * @note may be called recursively, some yang-bind (eg rpc) semantic checks may trigger error message
 * @note yang-binding over schema mount-points do not work, you need to make a separate bind call
 */
static int 
_xml_parse(const char *str, 
           yang_bind   yb,
           yang_stmt  *yspec,
           cxobj      *xt,
           cxobj     **xerr)
{
    int             retval = -1;
    clixon_xml_yacc xy = {0,};
    cxobj          *x;
    int             ret;
    int             failed = 0; /* yang assignment */
    int             i;

    clicon_debug(CLIXON_DBG_DETAIL, "%s", __FUNCTION__);
    if (strlen(str) == 0){
        return 1; /* OK */
    }
    if (xt == NULL){
        clicon_err(OE_XML, errno, "Unexpected NULL XML");
        return -1;      
    }
    if ((xy.xy_parse_string = strdup(str)) == NULL){
        clicon_err(OE_XML, errno, "strdup");
        return -1;
    }
    xy.xy_xtop = xt;
    xy.xy_xparent = xt;
    xy.xy_yspec = yspec;
    if (clixon_xml_parsel_init(&xy) < 0)
        goto done;    
    if (clixon_xml_parseparse(&xy) != 0)  /* yacc returns 1 on error */
        goto done;
    /* Purge all top-level body objects */
    x = NULL;
    while ((x = xml_find_type(xt, NULL, "body", CX_BODY)) != NULL)
        xml_purge(x);
    /* Traverse new objects */
    for (i = 0; i < xy.xy_xlen; i++) {
        x = xy.xy_xvec[i];
        /* Verify namespaces after parsing */
        if (xml2ns_recurse(x) < 0)
            goto done;
        /* Populate, ie associate xml nodes with yang specs 
         */
        switch (yb){
        case YB_NONE:
            break;
        case YB_PARENT:
            /* xt:n         Has spec
             * x:   <a> <-- populate from parent
             */
            if ((ret = xml_bind_yang0(NULL, x, YB_PARENT, NULL, xerr)) < 0)
                goto done;
            if (ret == 0)
                failed++;
            break;

        case YB_MODULE_NEXT:
            if ((ret = xml_bind_yang(NULL, x, YB_MODULE, yspec, xerr)) < 0)
                goto done;
            if (ret == 0)
                failed++;
            break;
        case YB_MODULE:
            /* xt:<top>     nospec
             * x:   <a> <-- populate from modules
             */
            if ((ret = xml_bind_yang0(NULL, x, YB_MODULE, yspec, xerr)) < 0)
                goto done;
            if (ret == 0)
                failed++;
            break;
        case YB_RPC:
            if ((ret = xml_bind_yang_rpc(NULL, x, yspec, xerr)) < 0)
                goto done;
            if (ret == 0){ /* Add message-id */
                if (*xerr && clixon_xml_attr_copy(x, *xerr, "message-id") < 0)
                    goto done;
                failed++;
            }
            break;
        } /* switch */
    }
    if (failed)
        goto fail;
    /* Sort the complete tree after parsing. Sorting is not really meaningful if Yang
       not bound */
    if (yb != YB_NONE)
        if (xml_sort_recurse(xt) < 0)
            goto done;
    retval = 1;
  done:
    clixon_xml_parsel_exit(&xy);
    if (xy.xy_parse_string != NULL)
        free(xy.xy_parse_string);
    if (xy.xy_xvec)
        free(xy.xy_xvec);
    return retval; 
 fail: /* invalid */
    retval = 0;
    goto done;
}

/*! Read an XML definition from file and parse it into a parse-tree, advanced API
 *
 * @param[in]     fd    A file descriptor containing the XML file (as ASCII characters)
 * @param[in]     yb    How to bind yang to XML top-level when parsing
 * @param[in]     yspec Yang specification (only if bind is TOP or CONFIG)
 * @param[in,out] xt    Pointer to XML parse tree. If empty, create.
 * @param[out]    xerr  Pointer to XML error tree, if retval is 0
 * @retval        1     Parse OK and all yang assignment made
 * @retval        0     Parse OK but yang assigment not made (or only partial) and xerr set
 * @retval       -1     Error with clicon_err called. Includes parse error
 *
 * @code
 *  cxobj *xt = NULL;
 *  cxobj *xerr = NULL;
 *  FILE  *f;
 *  if ((f = fopen(filename, "r")) == NULL)
 *    err;
 *  if ((ret = clixon_xml_parse_file(f, YB_MODULE, yspec, &xt, &xerr)) < 0)
 *    err;
 *  xml_free(xt);
 * @endcode
 * @see clixon_xml_parse_string
 * @see clixon_json_parse_file
 * @note, If xt empty, a top-level symbol will be added so that <tree../> will be:  <top><tree.../></tree></top>
 * @note May block on file I/O
 */
int 
clixon_xml_parse_file(FILE      *fp, 
                      yang_bind  yb,
                      yang_stmt *yspec,
                      cxobj    **xt,
                      cxobj    **xerr)
{
    int   retval = -1;
    int   ret;
    int   len = 0;
    char  ch;
    char *xmlbuf = NULL;
    char *ptr;
    int   xmlbuflen = BUFLEN; /* start size */
    int   oldxmlbuflen;
    int   failed = 0;

    if (xt==NULL || fp == NULL){
        clicon_err(OE_XML, EINVAL, "arg is NULL");
        return -1;
    }
    if (yb == YB_MODULE && yspec == NULL){
        clicon_err(OE_XML, EINVAL, "yspec is required if yb == YB_MODULE");
        return -1;
    }
    if ((xmlbuf = malloc(xmlbuflen)) == NULL){
        clicon_err(OE_XML, errno, "malloc");
        goto done;
    }
    memset(xmlbuf, 0, xmlbuflen);
    ptr = xmlbuf;
    while (1){
        if ((ret = fread(&ch, 1, 1, fp)) < 0){
            clicon_err(OE_XML, errno, "read");
            break;
        }
        if (ret != 0){
            xmlbuf[len++] = ch;
        }
        if (ret == 0) {
            if (*xt == NULL)
                if ((*xt = xml_new(XML_TOP_SYMBOL, NULL, CX_ELMNT)) == NULL)
                    goto done;
            if ((ret = _xml_parse(ptr, yb, yspec, *xt, xerr)) < 0)
                goto done;
            if (ret == 0)
                failed++;
            break;
        }
        if (len >= xmlbuflen-1){ /* Space: one for the null character */
            oldxmlbuflen = xmlbuflen;
            xmlbuflen *= 2;
            if ((xmlbuf = realloc(xmlbuf, xmlbuflen)) == NULL){
                clicon_err(OE_XML, errno, "realloc");
                goto done;
            }
            memset(xmlbuf+oldxmlbuflen, 0, xmlbuflen-oldxmlbuflen);
            ptr = xmlbuf;
        }
    } /* while */
    retval = (failed==0) ? 1 : 0;
 done:
    if (retval < 0 && *xt){
        free(*xt);
        *xt = NULL;
    }
    if (xmlbuf)
        free(xmlbuf);
    return retval;
}

/*! Read an XML definition from string and parse it into a parse-tree, advanced API
 *
 * @param[in]     str   String containing XML definition. 
 * @param[in]     yb    How to bind yang to XML top-level when parsing
 * @param[in]     yspec Yang specification, or NULL
 * @param[in,out] xt    Pointer to XML parse tree. If empty will be created.
 * @param[out]    xerr  Reason for failure (yang assignment not made) if retval = 0
 * @retval        1     Parse OK and all yang assignment made
 * @retval        0     Parse OK but yang assigment not made (or only partial), xerr is set
 * @retval       -1     Error with clicon_err called. Includes parse error
 *
 * @code
 *  cxobj *xt = NULL;
 *  cxobj *xerr = NULL;
 *  if ((ret = clixon_xml_parse_string(str, YB_MODULE, yspec, &xt, &xerr)) < 0)
 *    err;
 *  if (ret == 0)
 *    // use xerr
 *  if (xml_rootchild(xt, 0, &xt) < 0) # If you want to remove TOP
 *    err;
 * @endcode
 * @see clixon_xml_parse_file
 * @see clixon_xml_parse_va
 * @note You need to free the xml parse tree after use, using xml_free()
 * @note If empty on entry, a new TOP xml will be created named "top"
 */
int 
clixon_xml_parse_string(const char *str, 
                        yang_bind   yb,
                        yang_stmt  *yspec,
                        cxobj     **xt,
                        cxobj     **xerr)
{
    if (xt==NULL){
        clicon_err(OE_XML, EINVAL, "xt is NULL");
        return -1;
    }
    if (yb == YB_MODULE && yspec == NULL){
        clicon_err(OE_XML, EINVAL, "yspec is required if yb == YB_MODULE");
        return -1;
    }
    if (*xt == NULL){
        if ((*xt = xml_new(XML_TOP_SYMBOL, NULL, CX_ELMNT)) == NULL)
            return -1;
    }
    return _xml_parse(str, yb, yspec, *xt, xerr);
}

/*! Read XML from var-arg list and parse it into xml tree
 *
 * Utility function using stdarg instead of static string.

 * @param[in]     yb     How to bind yang to XML top-level when parsing
 * @param[in]     yspec  Yang specification, or NULL
 * @param[in,out] xtop   Top of XML parse tree. If it is NULL, top element 
 *                       called 'top' will be created. Call xml_free() after use
 * @param[out]    xerr   Reason for failure (yang assignment not made)
 * @param[in]     format Format string for stdarg according to printf(3)
 * @retval        1      Parse OK and all yang assignment made
 * @retval        0      Parse OK but yang assigment not made (or only partial)
 * @retval       -1      Error with clicon_err called. Includes parse error
 *
 * @code
 *  cxobj *xt = NULL;
 *  if (clixon_xml_parse_va(YB_NONE, NULL, &xt, NULL, "<xml>%d</xml>", 22) < 0)
 *    err;
 *  xml_free(xt);
 * @endcode
 * @see clixon_xml_parse_string
 * @see clixon_xml_parse_file
 * @note If vararg list is empty, consider using clixon_xml_parse_string()
 */
int 
clixon_xml_parse_va(yang_bind   yb,
                    yang_stmt  *yspec,           
                    cxobj     **xtop,
                    cxobj     **xerr,
                    const char *format, ...)
{
    int     retval = -1;
    va_list args;
    char   *str = NULL;
    int     len;

    va_start(args, format);
    len = vsnprintf(NULL, 0, format, args) + 1;
    va_end(args);
    if ((str = malloc(len)) == NULL){
        clicon_err(OE_UNIX, errno, "malloc");
        goto done;
    }
    memset(str, 0, len);
    va_start(args, format);
    len = vsnprintf(str, len, format, args) + 1;
    va_end(args);
    retval = clixon_xml_parse_string(str, yb, yspec, xtop, xerr); 
 done:
    if (str)
        free(str);
    return retval;
}

/*! Copy an attribute value(eg message-id) from one xml (eg rpc input) to another xml (eg rpc outgoing)
 * @param[in]  xin   Get attr value from this XML
 * @param[in]  xout  Set attr value to this XML
 * @param[in]  name  Attribute name
 * @retval     0     OK
 * @retval    -1     Error
 * Alternative is to use: char *val = xml_find_value(x, name);
 * @code
 *  if (clixon_xml_attr_copy(xin, xout, "message-id") < 0)
 *    err;
 * @endcode
 */
int
clixon_xml_attr_copy(cxobj *xin,
                     cxobj *xout,
                     char  *name)
{
    int    retval = -1;
    char  *msgid;
    cxobj *xa;

    if (xin == NULL || xout == NULL){
        clicon_err(OE_XML, EINVAL, "xin or xout NULL");
        goto done;
    }
    if ((msgid = xml_find_value(xin, name)) != NULL){
        if ((xa = xml_new(name, xout, CX_ATTR)) == NULL)
            goto done;
        if (xml_value_set(xa, msgid) < 0)
            goto done;
    }
    retval = 0;
 done:
    return retval;
}
