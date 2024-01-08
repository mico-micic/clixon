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

 *
 * Regular logging and debugging. Syslog using levels.
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/types.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_debug.h"
#include "clixon_xml_io.h"
#include "clixon_yang_module.h"
#include "clixon_plugin.h"

/*
 * Local Variables
 */

/* Cache handle since debug calls do not have handle parameter */
static clixon_handle _debug_clixon_h    = NULL;

/*! The global debug level. 0 means no debug 
 *
 * @note There are pros and cons in having the debug state as a global variable. The 
 * alternative to bind it to the clicon handle (h) was considered but it limits its
 * usefulness, since not all functions have h
 */
static int _debug_level = 0;

/*! Initialize debug messages. Set debug level.
 *
 * Initialize debug module. The level is used together with clixon_debug(dbglevel) calls as follows: 
 * print message if level >= dbglevel.
 * Example: clixon_debug_init(1) -> debug(1) is printed, but not debug(2).
 * Normally, debug messages are sent to clixon_log() which in turn can be sent to syslog and/or stderr.
 * But you can also override this with a specific debug file so that debug messages are written on the file
 * independently of log or errors. This is to ensure that a syslog of normal logs is unpolluted by extensive
 * debugging.
 *
 * @param[in] h         Clixon handle
 * @param[in] dbglevel  0 is show no debug messages, 1 is normal, 2.. is high debug. 
 *                      Note this is _not_ level from syslog(3)
 * @param[in] f         Debug-file. Open file where debug messages are directed. 
 * @see clixon_log_file For specifying a debug-file
 */
int
clixon_debug_init(clixon_handle h,
                  int           dbglevel)
{
    _debug_clixon_h = h;
    _debug_level = dbglevel; /* Global variable */
    return 0;
}

/*! Get debug level
 */
int
clixon_debug_get(void)
{
    return _debug_level;
}

/*! Print a debug message with debug-level. Settings determine where msg appears.
 *
 * Do not use this fn directly, use the clixon_debug() macro
 * If the dbglevel passed in the function is equal to or lower than the one set by 
 * clixon_debug_init(level).  That is, only print debug messages <= than what you want:
 *      print message if level >= dbglevel.
 * The message is sent to clixon_log. EIther to syslog, stderr or both, depending on 
 * clixon_log_init() setting
 * @param[in] h         Clixon handle
 * @param[in] dbglevel  Mask of CLIXON_DBG_DEFAULT and other masks
 * @param[in] x         XML tree logged without prettyprint
 * @param[in] format    Message to print as argv.
 * @retval    0         OK
 * @retval   -1         Error
 * @see CLIXON_DBG_DEFAULT and other flags
 */
int
clixon_debug_fn(clixon_handle h,
                int           dbglevel,
                cxobj        *x,
                const char   *format, ...)
{
    int     retval = -1;
    va_list ap;
    size_t  trunc;
    cbuf   *cb = NULL;

    /* Mask debug level with global dbg variable */
    if ((dbglevel & clixon_debug_get()) == 0)
        return 0;
    if (h == NULL)     /* Accept NULL, use saved clixon handle */
        h = _debug_clixon_h;
    va_start(ap, format);
    if (clixon_plugin_errmsg_all(h, NULL, 0, LOG_TYPE_DEBUG,
                                 NULL, NULL, x, format, ap, &cb) < 0)
        goto done;
    va_end(ap);
    if (cb != NULL){ /* Customized: expand clixon_err_args */
        clixon_log_str(LOG_DEBUG, cbuf_get(cb));
        goto ok;
    }
    if ((cb = cbuf_new()) == NULL){
        fprintf(stderr, "cbuf_new: %s\n", strerror(errno));
        goto done;
    }
    va_start(ap, format);
    vcprintf(cb, format, ap);
    va_end(ap);    
    if (x){
        cprintf(cb, ": ");
        if (clixon_xml2cbuf(cb, x, 0, 0, NULL, -1, 0) < 0)
            goto done;
    }
    /* Truncate long debug strings */
    if ((trunc = clixon_log_string_limit_get()) && trunc < cbuf_len(cb))
        cbuf_trunc(cb, trunc);
    clixon_log_str(LOG_DEBUG, cbuf_get(cb));
 ok:
    retval = 0;
 done:
    if (cb)
        cbuf_free(cb);
    return retval;
}
