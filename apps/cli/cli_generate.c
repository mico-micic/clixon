/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

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

 *
 * Translation between database specs
 *   yang_spec                   CLIgen parse_tree
 *  +-------------+   yang2cli    +-------------+
 *  |             | ------------> | cli         |
 *  | list{key A;}|               | syntax      |
 *  +-------------+               +-------------+
 */
#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/param.h>
#include <math.h> /* For pow() kludge in cvtype_max2str_dup2 */

/* cligen */
#include <cligen/cligen.h>

/* Clicon */
#include <clixon/clixon.h>

#include "clixon_cli_api.h"
#include "cli_plugin.h"
#include "cli_generate.h"

/* This is the default callback function. But this is typically overwritten */
#define GENERATE_CALLBACK "overwrite_me"

/* variable expand function */
#define GENERATE_EXPAND_XMLDB "expand_dbvar"

/*=====================================================================
 * YANG generate CLI
 *=====================================================================*/
/*
 This is an example yang module:
module m {
  container x {
    list m1 {
      key "a";
      leaf a {
        type string;
      }
      leaf b {
        type string;
      }
    }
  }
}

You can see which CLISPEC it generates via clixon_cli -D 1:
Jan  2 11:17:58: yang2cli: buf
}   x,cli_set("/x");{
      m1         (<a:string>|<a:string expand_dbvar("candidate /x/m1/%s/a")>),cli_set("/x/m1/%s");
{
         b (<b:string>|<b:string expand_dbvar("candidate /x/m1/%s/b")>),cli_set("/x/m1/%s/b");
      }
   }
*/

/*! Create cligen variable expand entry with xmlkey format string as argument
 * @param[in]  h      clicon handle
 * @param[in]  ys     yang_stmt of the node at hand
 * @param[in]  cvtype Type of the cligen variable
 * @param[in]  options 
 * @param[in]  fraction_digits
 * @param[out] cb     The string where the result format string is inserted.

 * @see expand_dbvar  This is where the expand string is used
 * @note XXX only fraction_digits handled,should also have mincv, maxcv, pattern
 */
static int
cli_expand_var_generate(clicon_handle h, 
			yang_stmt    *ys, 
			enum cv_type  cvtype,
			int           options,
			uint8_t       fraction_digits,
			cbuf         *cb)
{
    int   retval = -1;
    char *api_path_fmt = NULL;

    if (yang2api_path_fmt(ys, 1, &api_path_fmt) < 0)
	goto done;
    cprintf(cb, "|<%s:%s",  ys->ys_argument, 
	    cv_type2str(cvtype));
    if (options & YANG_OPTIONS_FRACTION_DIGITS)
	cprintf(cb, " fraction-digits:%u", fraction_digits);
    cprintf(cb, " %s(\"candidate\",\"%s\")>",
	    GENERATE_EXPAND_XMLDB,
	    api_path_fmt);
    retval = 0;
 done:
    if (api_path_fmt)
	free(api_path_fmt);
    return retval;
}

/*! Create callback with api_path format string as argument
 * @param[in]  h   clicon handle
 * @param[in]  ys  yang_stmt of the node at hand
 * @param[out] cb  The string where the result format string is inserted.
 * @see cli_dbxml  This is where the xmlkeyfmt string is used
 * @see pt_callback_reference  in CLIgen where the actual callback overwrites the template
 */
static int
cli_callback_generate(clicon_handle h, 
		      yang_stmt    *ys, 
		      cbuf         *cb)
{
    int        retval = -1;
    char      *api_path_fmt = NULL;

    if (yang2api_path_fmt(ys, 0, &api_path_fmt) < 0)
	goto done;
    cprintf(cb, ",%s(\"%s\")", GENERATE_CALLBACK, 
	    api_path_fmt);
    retval = 0;
 done:
    if (api_path_fmt)
	free(api_path_fmt);
    return retval;
}


/*! Generate identityref statements for CLI variables
 * @param[in]  ys        Yang statement
 * @param[in]  ytype     Yang union type being resolved
 * @param[in]  helptext  CLI help text
 * @param[out] cb        Buffer where cligen code is written
 * @see yang2cli_var_sub  Its sub-function
 */
static int
yang2cli_var_identityref(yang_stmt *ys,
			 yang_stmt *ytype,
			 char      *cvtypestr,
			 char      *helptext,
			 cbuf      *cb)
{
    int     retval = -1;
    yang_stmt *ybaseref;
    yang_stmt *ybaseid;
    cg_var    *cv = NULL;
    char      *name;
    char      *id;
    int        i;
    
    /* Add a wildchar string first -let validate take it for default prefix */
    cprintf(cb, ">");
    if (helptext)
	cprintf(cb, "(\"%s\")", helptext);
    if ((ybaseref = yang_find((yang_node*)ytype, Y_BASE, NULL)) != NULL &&
	(ybaseid = yang_find_identity(ys, ybaseref->ys_argument)) != NULL){
	if (cvec_len(ybaseid->ys_cvec) > 0){
	    cprintf(cb, "|<%s:%s choice:", ys->ys_argument, cvtypestr);
	    i = 0;
	    while ((cv = cvec_each(ybaseid->ys_cvec, cv)) != NULL){
		if (i++)
		    cprintf(cb, "|"); 
		name = strdup(cv_name_get(cv));
		if ((id=strchr(name, ':')) != NULL)
		    *id = '\0';
		cprintf(cb, "%s:%s", name, id+1);
		if (name)
		    free(name);
	    }
	}
    }
    retval = 0;
    // done:
    return retval;
}
    
/*! Generate range check statements for CLI variables
 * @param[in]  ys      Yang statement
 * @param[in]  options Flags field of optional values, eg YANG_OPTIONS_RANGE
 * @param[in]  cvv     Cvec with array of range_min/range_max cv:s (if YANG_OPTIONS_RANGE is set in options)
 * @param[out] cb      Buffer where cligen code is written
 * @see yang2cli_var_sub   which is the main function
 * In yang ranges are given as range 1 or range 1 .. 16, encoded in a cvv
 * 0 : range_min = x
 * and
 * 0 : range_min = x
 * 1 : range_max = y
 * Multiple ranges are given as: range x..y | x1..y1
 * This is encode in clixon as a cvec as:
 * 0 : range_min = x
 * 1 : range_max = y
 * 0 : range_min = x1
 * 1 : range_max = y1
 *
 * Generation of cli code
 * Single range is made by eg:
 *   <n:uint8 range[1:16]>
 * Multiple ranges is made by generating code eg:
 *   <n:uint8 range[1:16] range[32:64]>
 */
static int
yang2cli_var_range(yang_stmt *ys,
		   int        options,
		   cvec      *cvv,
		   cbuf      *cb)
{
    int     retval = -1;
    int     i;
    cg_var *cv1; /* lower limit */
    cg_var *cv2; /* upper limit */
    
    /* Loop through range_min and range_min..range_max */
    i = 0;
    while (i<cvec_len(cvv)){
	cv1 = cvec_i(cvv, i++);
	if (strcmp(cv_name_get(cv1),"range_min") == 0){
	    cprintf(cb, " %s[", (options&YANG_OPTIONS_RANGE)?"range":"length");
	    cv2cbuf(cv1, cb);
	    cprintf(cb,":");
	    /* probe next */
	    if (i<cvec_len(cvv) &&
		(cv2 = cvec_i(cvv, i)) != NULL &&
		strcmp(cv_name_get(cv2),"range_max") == 0){
		i++;
		cv2cbuf(cv2, cb);
	    }
	    else /* If not, it is a single number range [x:x]*/
		cv2cbuf(cv1, cb);
	    cprintf(cb,"]");
	}
    }
    retval = 0;
    // done:
    return retval;
}

/* Forward */
static int yang2cli_stmt(clicon_handle h, yang_stmt *ys, 
			 enum genmodel_type gt, int level, cbuf *cb);

static int yang2cli_var_union(clicon_handle h, yang_stmt *ys, char *origtype,
			      yang_stmt *ytype, char *helptext, cbuf *cb);

/*! Generate CLI code for Yang leaf statement to CLIgen variable of specific type
 * Check for completion (of already existent values), ranges (eg range[min:max]) and
 * patterns, (eg regexp:"[0.9]*").
 * @param[in]  h        Clixon handle
 * @param[in]  ys       Yang statement
 * @param[in]  ytype    Yang union type being resolved
 * @param[in]  helptext CLI help text
 * @param[in]  cvtype
 * @param[in]  options  Flags field of optional values, see YANG_OPTIONS_*
 * @param[in]  cvv      Cvec with array of range_min/range_max cv:s
 * @param[in]  pattern  String of POSIX regexp pattern
 * @param[in]  fraction for decimal64, how many digits after period
 * @param[out] cb       Buffer where cligen code is written
 * @see yang_type_resolve for options and other arguments
 */
static int
yang2cli_var_sub(clicon_handle h,
		 yang_stmt    *ys, 
		 yang_stmt    *ytype,  /* resolved type */
		 char         *helptext,
		 enum cv_type  cvtype,
		 int           options,
		 cvec         *cvv,
		 char         *pattern,
		 uint8_t       fraction_digits,
		 cbuf         *cb
    )
{
    int           retval = -1;
    char         *type;
    yang_stmt    *yi = NULL;
    int           i = 0;
    char         *cvtypestr;

    if (cvtype == CGV_VOID){
	retval = 0;
	goto done;
    }
    type = ytype?ytype->ys_argument:NULL;
    cvtypestr = cv_type2str(cvtype);
    
    if (type && strcmp(type, "identityref") == 0)
	cprintf(cb, "(");
    cprintf(cb, "<%s:%s", ys->ys_argument, cvtypestr);
    /* enumeration special case completion */
    if (type){
	if (strcmp(type, "enumeration") == 0 || strcmp(type, "bits") == 0){
	    cprintf(cb, " choice:"); 
	    i = 0;
	    yi = NULL;
	    while ((yi = yn_each((yang_node*)ytype, yi)) != NULL){
		if (yi->ys_keyword != Y_ENUM && yi->ys_keyword != Y_BIT)
		    continue;
		if (i)
		    cprintf(cb, "|"); 
		cprintf(cb, "%s", yi->ys_argument); 
		i++;
	    }
	}
	else if (strcmp(type, "identityref") == 0){
	    if (yang2cli_var_identityref(ys, ytype, cvtypestr, helptext, cb) < 0)
		goto done;
	}
    }
    if (options & YANG_OPTIONS_FRACTION_DIGITS)
	cprintf(cb, " fraction-digits:%u", fraction_digits);

    if (options & (YANG_OPTIONS_RANGE|YANG_OPTIONS_LENGTH)){
	if (yang2cli_var_range(ys, options, cvv, cb) < 0)
	    goto done;
    }
    if (options & YANG_OPTIONS_PATTERN){
	char *posix = NULL;
	if (regexp_xsd2posix(pattern, &posix) < 0)
	    goto done;
	cprintf(cb, " regexp:\"%s\"", posix);
	if (posix)
	    free(posix);
    }
    cprintf(cb, ">");
    if (helptext)
	cprintf(cb, "(\"%s\")", helptext);
    if (type && strcmp(type, "identityref") == 0)
	cprintf(cb, ")");
    retval = 0;
  done:
    return retval;
}

/*! Resolve a single Yang union and generate code
 * Part of generating CLI code for Yang leaf statement to CLIgen variable
 * @param[in]  h     Clixon handle
 * @param[in]  ys    Yang statement (caller of type)
 * @param[in]  origtype Name of original type in the call
 * @param[in]  ytsub Yang type invocation, a sub-type of a resolved union type
 * @param[in]  cb    Buffer where cligen code is written
 * @param[in]  helptext  CLI help text
 */
static int
yang2cli_var_union_one(clicon_handle h,
		       yang_stmt    *ys, 
		       char         *origtype,
		       yang_stmt    *ytsub,
		       char         *helptext,
		       cbuf         *cb)
{
    int          retval = -1;
    int          options = 0;
    cvec        *cvv = NULL; 
    char        *pattern = NULL;
    uint8_t      fraction_digits = 0;
    enum cv_type cvtype;
    yang_stmt   *ytype; /* resolved type */
    char        *restype;

    /* Resolve the sub-union type to a resolved type */
    if (yang_type_resolve(ys, ys, ytsub, /* in */
			  &ytype, &options, /* resolved type */
			  &cvv, &pattern, &fraction_digits) < 0)
	goto done;
    restype = ytype?ytype->ys_argument:NULL;

    if (restype && strcmp(restype, "union") == 0){      /* recursive union */
	if (yang2cli_var_union(h, ys, origtype, ytype, helptext, cb) < 0)
	    goto done;
    }
    else {
	if (clicon_type2cv(origtype, restype, ys, &cvtype) < 0)
	    goto done;
	if ((retval = yang2cli_var_sub(h, ys, ytype, helptext, cvtype, 
				       options, cvv, pattern, fraction_digits, cb)) < 0)
	    goto done;
    }
    retval = 0;
 done:
    return retval;
}

/*! Loop over all sub-types of a Yang union 
 * Part of generating CLI code for Yang leaf statement to CLIgen variable
 * @param[in]  h     Clixon handle
 * @param[in]  ys    Yang statement (caller)
 * @param[in]  origtype Name of original type in the call
 * @param[in]  ytype Yang resolved type (a union in this case)
 * @param[in]  helptext  CLI help text
 * @param[out]  cb    Buffer where cligen code is written
 */
static int
yang2cli_var_union(clicon_handle h,
		   yang_stmt    *ys, 
		   char         *origtype,
		   yang_stmt    *ytype,
		   char         *helptext,
		   cbuf         *cb)
{
    int        retval = -1;
    yang_stmt *ytsub = NULL;
    int        i;

    i = 0;
    /* Loop over all sub-types in the resolved union type, note these are
     * not resolved types (unless they are built-in, but the resolve call is
     * made in the union_one call.
     */
    while ((ytsub = yn_each((yang_node*)ytype, ytsub)) != NULL){
	if (ytsub->ys_keyword != Y_TYPE)
	    continue;
	if (i++)
	    cprintf(cb, "|");
	if (yang2cli_var_union_one(h, ys, origtype, ytsub, helptext, cb) < 0)
	    goto done;
    }
    retval = 0;
  done:
    return retval;
}

/*! Generate CLI code for Yang leaf statement to CLIgen variable
 * @param[in]  h        Clixon handle
 * @param[in]  ys       Yang statement
 * @param[in]  helptext CLI help text
 * @param[out] cb       Buffer where cligen code is written

 *
 * Make a type lookup and complete a cligen variable expression such as <a:string>.
 * One complication is yang union, that needs a recursion since it consists of 
 * sub-types.
 * eg type union{ type int32; type string } --> (<x:int32>| <x:string>)
 * Another is multiple ranges
 */
static int
yang2cli_var(clicon_handle h,
	     yang_stmt    *ys, 
	     char         *helptext,
	     cbuf         *cb)
{
    int           retval = -1;
    char         *origtype;
    yang_stmt    *yrestype; /* resolved type */
    char         *restype; /* resolved type */
    cvec         *cvv = NULL; 
    char         *pattern = NULL;
    uint8_t       fraction_digits = 0;
    enum cv_type  cvtype;
    int           options = 0;
    int           completionp;
    char         *type;

    if (yang_type_get(ys, &origtype, &yrestype, 
		      &options, &cvv, &pattern, &fraction_digits) < 0)
	goto done;
    restype = yrestype?yrestype->ys_argument:NULL;

    if (restype && strcmp(restype, "empty") == 0){
	retval = 0;
	goto done;
    }
    if (clicon_type2cv(origtype, restype, ys, &cvtype) < 0)
	goto done;
    /* Note restype can be NULL here for example with unresolved hardcoded uuid */
    if (restype && strcmp(restype, "union") == 0){ 
	/* Union: loop over resolved type's sub-types (can also be recursive unions) */
	cprintf(cb, "(");
	if (yang2cli_var_union(h, ys, origtype, yrestype, helptext, cb) < 0)
	    goto done;
	if (clicon_cli_genmodel_completion(h)){
	    if (cli_expand_var_generate(h, ys, cvtype, 
					options, fraction_digits, cb) < 0)
		goto done;
	    if (helptext)
		cprintf(cb, "(\"%s\")", helptext);
	}
	cprintf(cb, ")");
    }
    else{
	type = yrestype?yrestype->ys_argument:NULL;
	if (type)
	    completionp = clicon_cli_genmodel_completion(h) &&
		strcmp(type, "enumeration") != 0 &&
		strcmp(type, "identityref") != 0 && 
		strcmp(type, "bits") != 0;
	else
	    completionp = clicon_cli_genmodel_completion(h);
	if (completionp)
	    cprintf(cb, "(");
	if ((retval = yang2cli_var_sub(h, ys, yrestype, helptext, cvtype, 
				       options, cvv, pattern, fraction_digits, cb)) < 0)
	    goto done;
	if (completionp){
	    if (cli_expand_var_generate(h, ys, cvtype, 
					options, fraction_digits, cb) < 0)
		goto done;
	    if (helptext)
		cprintf(cb, "(\"%s\")", helptext);
	    cprintf(cb, ")");
	}
    }
    retval = 0;
  done:
    return retval;
}

/*! Generate CLI code for Yang leaf statement
 * @param[in]  h     Clixon handle
 * @param[in]  ys    Yang statement
 * @param[in]  gt    CLI Generate style 
 * @param[in]  level Indentation level
 * @param[in]  callback  If set, include a "; cli_set()" callback, otherwise not
 * @param[out] cb  Buffer where cligen code is written
 */
static int
yang2cli_leaf(clicon_handle h, 
	      yang_stmt    *ys, 
	      enum genmodel_type gt,
	      int           level,
	      int           callback,
	      cbuf         *cb)
{
    yang_stmt    *yd;  /* description */
    int           retval = -1;
    char         *helptext = NULL;
    char         *s;

    /* description */
    if ((yd = yang_find((yang_node*)ys, Y_DESCRIPTION, NULL)) != NULL){
	if ((helptext = strdup(yd->ys_argument)) == NULL){
	    clicon_err(OE_UNIX, errno, "strdup");
	    goto done;
	}
	if ((s = strstr(helptext, "\n\n")) != NULL)
	    *s = '\0';
    }
    cprintf(cb, "%*s", level*3, "");
    if (gt == GT_VARS|| gt == GT_ALL){
	cprintf(cb, "%s", ys->ys_argument);
	if (helptext)
	    cprintf(cb, "(\"%s\")", helptext);
	cprintf(cb, " ");
	if (yang2cli_var(h, ys, helptext, cb) < 0)
	    goto done;
    }
    else
	if (yang2cli_var(h, ys, helptext, cb) < 0)
	    goto done;
    if (callback){
	if (cli_callback_generate(h, ys, cb) < 0)
	    goto done;
	cprintf(cb, ";\n");
    }

    retval = 0;
  done:
    if (helptext)
	free(helptext);
    return retval;
}

/*! Generate CLI code for Yang container statement
 * @param[in]  h     Clixon handle
 * @param[in]  ys    Yang statement
 * @param[in]  gt    CLI Generate style 
 * @param[in]  level Indentation level
 * @param[out] cb  Buffer where cligen code is written
 */
static int
yang2cli_container(clicon_handle h, 
		   yang_stmt    *ys, 
		   enum genmodel_type gt,
		   int           level,
		   cbuf         *cb)
{
    yang_stmt    *yc;
    yang_stmt    *yd;
    int           i;
    int           retval = -1;
    char         *helptext = NULL;
    char         *s;

    cprintf(cb, "%*s%s", level*3, "", ys->ys_argument);
    if ((yd = yang_find((yang_node*)ys, Y_DESCRIPTION, NULL)) != NULL){
	if ((helptext = strdup(yd->ys_argument)) == NULL){
	    clicon_err(OE_UNIX, errno, "strdup");
	    goto done;
	}
	if ((s = strstr(helptext, "\n\n")) != NULL)
	    *s = '\0';
	cprintf(cb, "(\"%s\")", helptext);
    }
    if (cli_callback_generate(h, ys, cb) < 0)
	goto done;
    cprintf(cb, ";{\n");
    for (i=0; i<ys->ys_len; i++)
	if ((yc = ys->ys_stmt[i]) != NULL)
	    if (yang2cli_stmt(h, yc, gt, level+1, cb) < 0)
		goto done;
    cprintf(cb, "%*s}\n", level*3, "");
    retval = 0;
  done:
    if (helptext)
	free(helptext);
    return retval;
}

/*! Generate CLI code for Yang list statement
 * @param[in]  h     Clixon handle
 * @param[in]  ys    Yang statement
 * @param[in]  gt    CLI Generate style 
 * @param[in]  level Indentation level
 * @param[out] cb    Buffer where cligen code is written
 */
static int
yang2cli_list(clicon_handle      h, 
	      yang_stmt         *ys, 
	      enum genmodel_type gt,
	      int                level,
	      cbuf              *cb)
{
    yang_stmt    *yc;
    yang_stmt    *yd;
    yang_stmt    *yleaf;
    int           i;
    cg_var       *cvi;
    char         *keyname;
    cvec         *cvk = NULL; /* vector of index keys */
    int           retval = -1;
    char         *helptext = NULL;
    char         *s;

    cprintf(cb, "%*s%s", level*3, "", ys->ys_argument);
    if ((yd = yang_find((yang_node*)ys, Y_DESCRIPTION, NULL)) != NULL){
	if ((helptext = strdup(yd->ys_argument)) == NULL){
	    clicon_err(OE_UNIX, errno, "strdup");
	    goto done;
	}
	if ((s = strstr(helptext, "\n\n")) != NULL)
	    *s = '\0';
	cprintf(cb, "(\"%s\")", helptext);
    }
    /* Loop over all key variables */
    cvk = ys->ys_cvec; /* Use Y_LIST cache, see ys_populate_list() */
    cvi = NULL;
    /* Iterate over individual keys  */
    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
	keyname = cv_string_get(cvi);
	if ((yleaf = yang_find((yang_node*)ys, Y_LEAF, keyname)) == NULL){
	    clicon_err(OE_XML, 0, "List statement \"%s\" has no key leaf \"%s\"", 
		       ys->ys_argument, keyname);
	    goto done;
	}
	/* Print key variable now, and skip it in loop below 
	   Note, only print callback on last statement
	 */
	if (yang2cli_leaf(h, yleaf, gt==GT_VARS?GT_NONE:gt, level+1, 
			  cvec_next(cvk, cvi)?0:1, cb) < 0)
	    goto done;
    }

    cprintf(cb, "{\n");
    for (i=0; i<ys->ys_len; i++)
	if ((yc = ys->ys_stmt[i]) != NULL){
	    /*  cvk is a cvec of strings containing variable names
		yc is a leaf that may match one of the values of cvk.
	     */
	    cvi = NULL;
	    while ((cvi = cvec_each(cvk, cvi)) != NULL) {
		keyname = cv_string_get(cvi);
		if (strcmp(keyname, yc->ys_argument) == 0)
		    break;
	    }
	    if (cvi != NULL)
		continue;
	    if (yang2cli_stmt(h, yc, gt, level+1, cb) < 0)
		goto done;
	}
    cprintf(cb, "%*s}\n", level*3, "");
    retval = 0;
  done:
    if (helptext)
	free(helptext);
    return retval;
}

/*! Generate CLI code for Yang choice statement
 *
 * @param[in]  h     Clixon handle
 * @param[in]  ys    Yang statement
 * @param[in]  gt    CLI Generate style 
 * @param[in]  level Indentation level
 * @param[out] cb    Buffer where cligen code is written
@example
  choice interface-type {
         container ethernet { ... }
         container fddi { ... }
  }
@example.end
  @Note Removes 'meta-syntax' from cli syntax. They are not shown when xml is 
  translated to cli. and therefore input-syntax != output syntax. Which is bad
 */
static int
yang2cli_choice(clicon_handle h, 
		yang_stmt    *ys, 
		enum genmodel_type gt,
		int           level,
    		cbuf         *cb)
{
    int           retval = -1;
    yang_stmt    *yc;
    int           i;

    for (i=0; i<ys->ys_len; i++)
	if ((yc = ys->ys_stmt[i]) != NULL){
	    switch (yc->ys_keyword){
	    case Y_CASE:
		if (yang2cli_stmt(h, yc, gt, level+2, cb) < 0)
		    goto done;
		break;
	    case Y_CONTAINER:
	    case Y_LEAF:
	    case Y_LEAF_LIST:
	    case Y_LIST:
	    default:
		if (yang2cli_stmt(h, yc, gt, level+1, cb) < 0)
		    goto done;
		break;
	    }
	}
    retval = 0;
  done:
    return retval;
}

/*! Generate CLI code for Yang statement
 * @param[in]  h     Clixon handle
 * @param[in]  ys    Yang statement
 * @param[out] cb    Buffer where cligen code is written
 * @param[in]  gt    CLI Generate style 
 * @param[in]  level Indentation level
 */
static int
yang2cli_stmt(clicon_handle h, 
	      yang_stmt    *ys, 
	      enum genmodel_type gt,
	      int           level, /* indentation level for pretty-print */
	      cbuf         *cb)
{
    yang_stmt    *yc;
    int           retval = -1;
    int           i;

    if (yang_config(ys)){
	switch (ys->ys_keyword){
	case Y_CONTAINER:
	    if (yang2cli_container(h, ys, gt, level, cb) < 0)
		goto done;
	    break;
	case Y_LIST:
	    if (yang2cli_list(h, ys, gt, level, cb) < 0)
		goto done;
	    break;
	case Y_CHOICE:
	    if (yang2cli_choice(h, ys, gt, level, cb) < 0)
		goto done;
	    break;
	case Y_LEAF_LIST:
	case Y_LEAF:
	    if (yang2cli_leaf(h, ys, gt, level, 1, cb) < 0)
		goto done;
	    break;
	case Y_CASE:
	case Y_SUBMODULE:
	case Y_MODULE:
	    for (i=0; i<ys->ys_len; i++)
		if ((yc = ys->ys_stmt[i]) != NULL)
		    if (yang2cli_stmt(h, yc, gt, level+1, cb) < 0)
			goto done;
	    break;
	default: /* skip */
	    break;
	}
    }
    retval = 0;
  done:
    return retval;
}

/*! Generate CLI code for Yang specification
 * @param[in]  h     Clixon handle
 * @param[in]  yspec Yang specification
 * @param[out] ptnew CLIgen parse-tree
 * @param[in]  gt    CLI Generate style
 *
 * Code generation styles:
 *    VARS: generate keywords for regular vars only not index
 *    ALL:  generate keywords for all variables including index
 */
int
yang2cli(clicon_handle      h, 
	 yang_spec         *yspec, 
	 parse_tree        *ptnew, 
	 enum genmodel_type gt)
{
    cbuf           *cb = NULL;
    int             i;
    int             retval = -1;
    yang_stmt      *ymod = NULL;
    cvec           *globals;       /* global variables from syntax */

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    /* Traverse YANG, loop through all modules and generate CLI */
    for (i=0; i<yspec->yp_len; i++)
	if ((ymod = yspec->yp_stmt[i]) != NULL){
	    if (yang2cli_stmt(h, ymod, gt, 0, cb) < 0)
		goto done;
	}
    clicon_debug(2, "%s: buf\n%s\n", __FUNCTION__, cbuf_get(cb));
    /* Parse the buffer using cligen parser. XXX why this?*/
    if ((globals = cvec_new(0)) == NULL)
	goto done;
    /* load cli syntax */
    if (cligen_parse_str(cli_cligen(h), cbuf_get(cb), 
			 "yang2cli", ptnew, globals) < 0)
	goto done;
    cvec_free(globals);
    /* Resolve the expand callback functions in the generated syntax.
       This "should" only be GENERATE_EXPAND_XMLDB
       handle=NULL for global namespace, this means expand callbacks must be in
       CLICON namespace, not in a cli frontend plugin.
    */
    if (cligen_expandv_str2fn(*ptnew, (expandv_str2fn_t*)clixon_str2fn, NULL) < 0)     
	goto done;

    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    return retval;
}
