/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2002 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 *
 * Portions of this software are based upon public domain software
 * originally written at the National Center for Supercomputing Applications,
 * University of Illinois, Urbana-Champaign.
 */

/*
 * mod_isapi.c - Internet Server Application (ISA) module for Apache
 * by Alexei Kosut <akosut@apache.org>
 *
 * This module implements Microsoft's ISAPI, allowing Apache (when running
 * under Windows) to load Internet Server Applications (ISAPI extensions).
 * It implements all of the ISAPI 2.0 specification, except for the 
 * "Microsoft-only" extensions dealing with asynchronous I/O. All ISAPI
 * extensions that use only synchronous I/O and are compatible with the
 * ISAPI 2.0 specification should work (most ISAPI 1.0 extensions should
 * function as well).
 *
 * To load, simply place the ISA in a location in the document tree.
 * Then add an "AddHandler isapi-isa dll" into your config file.
 * You should now be able to load ISAPI DLLs just be reffering to their
 * URLs. Make sure the ExecCGI option is active in the directory
 * the ISA is in.
 */

#include "apr_strings.h"
#include "apr_portable.h"
#include "apr_buckets.h"
#include "ap_config.h"
#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_log.h"
#include "util_script.h"
#include "mod_core.h"

/* We use the exact same header file as the original */
#include <HttpExt.h>

#if !defined(HSE_REQ_MAP_URL_TO_PATH_EX) \
 || !defined(HSE_REQ_SEND_RESPONSE_HEADER_EX)
#pragma message("WARNING: This build of Apache is missing the recent changes")
#pragma message("in the Microsoft Win32 Platform SDK; some mod_isapi features")
#pragma message("will be disabled.  To obtain the latest Platform SDK files,")
#pragma message("please refer to:")
#pragma message("http://msdn.microsoft.com/downloads/sdks/platform/platform.asp")
#endif

/**********************************************************
 *
 *  ISAPI Module Configuration
 *
 **********************************************************/

module AP_MODULE_DECLARE_DATA isapi_module;

#define ISAPI_UNDEF -1

/* Our isapi global config values */
static struct isapi_global_conf {
    apr_pool_t         *pool;
    apr_array_header_t *modules;
} loaded;

/* Our isapi per-dir config structure */
typedef struct isapi_dir_conf {
    int read_ahead_buflen;
    int log_unsupported;
    int log_to_errlog;
    int log_to_query;
} isapi_dir_conf;

typedef struct isapi_loaded isapi_loaded;

static apr_status_t isapi_load(apr_pool_t *p, request_rec *r, 
                               const char *fpath, isapi_loaded** isa);

static void *create_isapi_dir_config(apr_pool_t *p, char *dummy)
{
    isapi_dir_conf *dir = apr_palloc(p, sizeof(isapi_dir_conf));
    
    dir->read_ahead_buflen = ISAPI_UNDEF;
    dir->log_unsupported   = ISAPI_UNDEF;
    dir->log_to_errlog     = ISAPI_UNDEF;
    dir->log_to_query      = ISAPI_UNDEF;

    return dir;
}

static void *merge_isapi_dir_configs(apr_pool_t *p, void *base_, void *add_)
{
    isapi_dir_conf *base = (isapi_dir_conf *) base_;
    isapi_dir_conf *add = (isapi_dir_conf *) add_;
    isapi_dir_conf *dir = apr_palloc(p, sizeof(isapi_dir_conf));
    
    dir->read_ahead_buflen = (add->read_ahead_buflen == ISAPI_UNDEF)
                                ? base->read_ahead_buflen
                                 : add->read_ahead_buflen;
    dir->log_unsupported   = (add->log_unsupported == ISAPI_UNDEF)
                                ? base->log_unsupported
                                 : add->log_unsupported;
    dir->log_to_errlog     = (add->log_to_errlog == ISAPI_UNDEF)
                                ? base->log_to_errlog
                                 : add->log_to_errlog;
    dir->log_to_query       = (add->log_to_query == ISAPI_UNDEF)
                                ? base->log_to_query
                                 : add->log_to_query;
    
    return dir;
}

static const char *isapi_cmd_cachefile(cmd_parms *cmd, void *dummy, 
                                       const char *filename)
{
    isapi_loaded *isa, **newisa;
    apr_finfo_t tmp;
    apr_status_t rv;
    char *fspec;
    
    fspec = ap_server_root_relative(cmd->pool, filename);
    if (!fspec) {
	ap_log_error(APLOG_MARK, APLOG_WARNING, APR_EBADPATH, cmd->server,
	             "ISAPI: Invalid module path %s, skipping", filename);
	return NULL;
    }
    if ((rv = apr_stat(&tmp, fspec, APR_FINFO_TYPE, 
                      cmd->temp_pool)) != APR_SUCCESS) { 
	ap_log_error(APLOG_MARK, APLOG_WARNING, rv, cmd->server,
	    "ISAPI: unable to stat(%s), skipping", fspec);
	return NULL;
    }
    if (tmp.filetype != APR_REG) {
	ap_log_error(APLOG_MARK, APLOG_WARNING|APLOG_NOERRNO, 0, cmd->server,
	    "ISAPI: %s isn't a regular file, skipping", fspec);
	return NULL;
    }

    /* Load the extention as cached (passing NULL for r) */
    rv = isapi_load(loaded.pool, NULL, fspec, &isa); 
    if (rv != APR_SUCCESS) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, rv, cmd->server,
                     "ISAPI: unable to cache %s, skipping", fspec);
	return NULL;
    }

    /* Add to cached list of loaded modules */
    newisa = apr_array_push(loaded.modules);
    *newisa = isa;
    
    return NULL;
}

static const command_rec isapi_cmds[] = {
    AP_INIT_TAKE1("ISAPIReadAheadBuffer", ap_set_int_slot,
        (void *) APR_XtOffsetOf(isapi_dir_conf, read_ahead_buflen), 
        OR_FILEINFO, "Maximum bytes to initially pass to the ISAPI handler"),
    AP_INIT_FLAG("ISAPILogNotSupported", ap_set_int_slot,
        (void *) APR_XtOffsetOf(isapi_dir_conf, log_unsupported), 
        OR_FILEINFO, "Log requests not supported by the ISAPI server"),
    AP_INIT_FLAG("ISAPIAppendLogToErrors", ap_set_flag_slot,
        (void *) APR_XtOffsetOf(isapi_dir_conf, log_to_errlog), 
        OR_FILEINFO, "Send all Append Log requests to the error log"),
    AP_INIT_FLAG("ISAPIAppendLogToQuery", ap_set_flag_slot,
        (void *) APR_XtOffsetOf(isapi_dir_conf, log_to_query), 
        OR_FILEINFO, "Append Log requests are concatinated to the query args"),
    AP_INIT_ITERATE("ISAPICacheFile", isapi_cmd_cachefile, NULL, 
        RSRC_CONF, "Cache the specified ISAPI extension in-process"),
    {NULL}
};

/**********************************************************
 *
 *  ISAPI Module Cache handling section
 *
 **********************************************************/

/* Our loaded isapi module description structure */
struct isapi_loaded {
    const char *filename;
    apr_dso_handle_t *handle;
    HSE_VERSION_INFO *pVer;
    PFN_GETEXTENSIONVERSION GetExtensionVersion;
    PFN_HTTPEXTENSIONPROC   HttpExtensionProc;
    PFN_TERMINATEEXTENSION  TerminateExtension;
    int   refcount;
    DWORD timeout;
    BOOL  fakeasync;
    DWORD report_version;
};

static apr_status_t isapi_unload(isapi_loaded* isa, int force)
{
    /* All done with the DLL... get rid of it...
     *
     * If optionally cached, pass HSE_TERM_ADVISORY_UNLOAD,
     * and if it returns TRUE, unload, otherwise, cache it.
     */
    if (((--isa->refcount > 0) && !force) || !isa->handle)
        return APR_SUCCESS;
    if (isa->TerminateExtension) {
        if (force)
            (*isa->TerminateExtension)(HSE_TERM_MUST_UNLOAD);
        else if (!(*isa->TerminateExtension)(HSE_TERM_ADVISORY_UNLOAD))
            return APR_EGENERAL;
    }
    apr_dso_unload(isa->handle);
    isa->handle = NULL;
    return APR_SUCCESS;
}

static apr_status_t cleanup_isapi(void *isa)
{
    return isapi_unload((isapi_loaded*) isa, TRUE);
}

static apr_status_t isapi_load(apr_pool_t *p, request_rec *r, 
                               const char *fpath, isapi_loaded** isa)
{
    isapi_loaded **found = (isapi_loaded **)loaded.modules->elts;
    apr_status_t rv;
    int n;

    for (n = 0; n < loaded.modules->nelts; ++n) {
        if (strcasecmp(fpath, (*found)->filename) == 0) {
            break;
        }
        ++found;
    }
    
    if (n < loaded.modules->nelts) 
    {
        *isa = *found;
        if ((*isa)->handle) 
        {
            ++(*isa)->refcount;
            return APR_SUCCESS;
        }
        /* Otherwise we fall through and have to reload the resource
         * into this existing mod_isapi cache bucket.
         */
    }
    else
    {
        *isa = apr_pcalloc(p, sizeof(isapi_module));
        (*isa)->filename = fpath;
        (*isa)->pVer = apr_pcalloc(p, sizeof(HSE_VERSION_INFO));
    
        /* TODO: These need to become overrideable, so that we
         * assure a given isapi can be fooled into behaving well.
         */
        (*isa)->timeout = INFINITE; /* microsecs */
        (*isa)->fakeasync = TRUE;
        (*isa)->report_version = MAKELONG(0, 5); /* Revision 5.0 */
    }
    
    rv = apr_dso_load(&(*isa)->handle, fpath, p);
    if (rv)
    {
        ap_log_rerror(APLOG_MARK, APLOG_ALERT, rv, r,
                      "ISAPI: %s failed to load", fpath);
        (*isa)->handle = NULL;
        return rv;
    }

    rv = apr_dso_sym((void**)&(*isa)->GetExtensionVersion, (*isa)->handle,
                     "GetExtensionVersion");
    if (rv)
    {
        ap_log_rerror(APLOG_MARK, APLOG_ALERT, rv, r,
                      "ISAPI: %s is missing GetExtensionVersion()",
                      fpath);
        apr_dso_unload((*isa)->handle);
        (*isa)->handle = NULL;
        return rv;
    }

    rv = apr_dso_sym((void**)&(*isa)->HttpExtensionProc, (*isa)->handle,
                     "HttpExtensionProc");
    if (rv)
    {
        ap_log_rerror(APLOG_MARK, APLOG_ALERT, rv, r,
                      "ISAPI: %s is missing HttpExtensionProc()",
                      fpath);
        apr_dso_unload((*isa)->handle);
        (*isa)->handle = NULL;
        return rv;
    }

    /* TerminateExtension() is an optional interface */
    rv = apr_dso_sym((void**)&(*isa)->TerminateExtension, (*isa)->handle,
                     "TerminateExtension");
    SetLastError(0);

    /* Run GetExtensionVersion() */
    if (!((*isa)->GetExtensionVersion)((*isa)->pVer)) {
        apr_status_t rv = apr_get_os_error();
        ap_log_rerror(APLOG_MARK, APLOG_ALERT, rv, r,
                      "ISAPI: %s call GetExtensionVersion() failed", 
                      fpath);
        apr_dso_unload((*isa)->handle);
        (*isa)->handle = NULL;
        return rv;
    }

    ++(*isa)->refcount;

    apr_pool_cleanup_register(p, *isa, cleanup_isapi, 
                                   apr_pool_cleanup_null);

    return APR_SUCCESS;
}

/**********************************************************
 *
 *  ISAPI Module request callbacks section
 *
 **********************************************************/

/* Our "Connection ID" structure */
typedef struct isapi_cid {
    LPEXTENSION_CONTROL_BLOCK ecb;
    isapi_dir_conf dconf;
    isapi_loaded *isa;
    request_rec  *r;
    PFN_HSE_IO_COMPLETION completion;
    PVOID  completion_arg;
    HANDLE complete;
} isapi_cid;

BOOL WINAPI GetServerVariable (HCONN hConn, LPSTR lpszVariableName,
                               LPVOID lpvBuffer, LPDWORD lpdwSizeofBuffer)
{
    request_rec *r = ((isapi_cid *)hConn)->r;
    const char *result;
    DWORD len;

    if (!strcmp(lpszVariableName, "ALL_HTTP")) 
    {
        /* lf delimited, colon split, comma seperated and 
         * null terminated list of HTTP_ vars 
         */
        const apr_array_header_t *arr = apr_table_elts(r->subprocess_env);
        const apr_table_entry_t *elts = (const apr_table_entry_t *)arr->elts;
        int i;

        for (len = 0, i = 0; i < arr->nelts; i++) {
            if (!strncmp(elts[i].key, "HTTP_", 5)) {
                len += strlen(elts[i].key) + strlen(elts[i].val) + 2;
            }
        }
  
        if (*lpdwSizeofBuffer < len + 1) {
            *lpdwSizeofBuffer = len + 1;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
    
        for (i = 0; i < arr->nelts; i++) {
            if (!strncmp(elts[i].key, "HTTP_", 5)) {
                strcpy(lpvBuffer, elts[i].key);
                ((char*)lpvBuffer) += strlen(elts[i].key);
                *(((char*)lpvBuffer)++) = ':';
                strcpy(lpvBuffer, elts[i].val);
                ((char*)lpvBuffer) += strlen(elts[i].val);
                *(((char*)lpvBuffer)++) = '\n';
            }
        }

        *(((char*)lpvBuffer)++) = '\0';
        *lpdwSizeofBuffer = len;
        return TRUE;
    }
    
    if (!strcmp(lpszVariableName, "ALL_RAW")) 
    {
        /* lf delimited, colon split, comma seperated and 
         * null terminated list of the raw request header
         */
        const apr_array_header_t *arr = apr_table_elts(r->headers_in);
        const apr_table_entry_t *elts = (const apr_table_entry_t *)arr->elts;
        int i;

        for (len = 0, i = 0; i < arr->nelts; i++) {
            len += strlen(elts[i].key) + strlen(elts[i].val) + 2;
        }
  
        if (*lpdwSizeofBuffer < len + 1) {
            *lpdwSizeofBuffer = len + 1;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
    
        for (i = 0; i < arr->nelts; i++) {
            strcpy(lpvBuffer, elts[i].key);
            ((char*)lpvBuffer) += strlen(elts[i].key);
            *(((char*)lpvBuffer)++) = ':';
            *(((char*)lpvBuffer)++) = ' ';
            strcpy(lpvBuffer, elts[i].val);
            ((char*)lpvBuffer) += strlen(elts[i].val);
            *(((char*)lpvBuffer)++) = '\n';
        }
        *(((char*)lpvBuffer)++) = '\0';
        *lpdwSizeofBuffer = len;
        return TRUE;
    }
    
    /* Not a special case */
    result = apr_table_get(r->subprocess_env, lpszVariableName);

    if (result) {
        len = strlen(result);
        if (*lpdwSizeofBuffer < len + 1) {
            *lpdwSizeofBuffer = len + 1;
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        strcpy(lpvBuffer, result);
        *lpdwSizeofBuffer = len;
        return TRUE;
    }

    /* Not Found */
    SetLastError(ERROR_INVALID_INDEX);
    return FALSE;
}

BOOL WINAPI WriteClient (HCONN ConnID, LPVOID Buffer, LPDWORD lpdwBytes,
                         DWORD dwReserved)
{
    request_rec *r = ((isapi_cid *)ConnID)->r;
    conn_rec *c = r->connection;
    apr_bucket_brigade *bb;
    apr_bucket *b;

    if (dwReserved == HSE_IO_SYNC)
        ; /* XXX: Fake it */

    bb = apr_brigade_create(r->pool, c->bucket_alloc);
    b = apr_bucket_transient_create(Buffer, *lpdwBytes, c->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(bb, b);
    b = apr_bucket_flush_create(c->bucket_alloc);
    APR_BRIGADE_INSERT_TAIL(bb, b);
    ap_pass_brigade(r->output_filters, bb);

    return TRUE;
}

BOOL WINAPI ReadClient (HCONN ConnID, LPVOID lpvBuffer, LPDWORD lpdwSize)
{
    request_rec *r = ((isapi_cid *)ConnID)->r;
    DWORD read = 0;
    int res;

    if (r->remaining < *lpdwSize) {
        *lpdwSize = (apr_size_t)r->remaining;
    }

    while (read < *lpdwSize &&
           ((res = ap_get_client_block(r, (char*)lpvBuffer + read,
                                       *lpdwSize - read)) > 0)) {
        read += res;
    }

    *lpdwSize = read;
    return TRUE;
}

static apr_ssize_t send_response_header(isapi_cid *cid, const char *stat,
                                        const char *head, apr_size_t statlen,
                                        apr_size_t headlen)
{
    int termarg;
    char *termch;

    if (!stat || statlen == 0 || !*stat) {
        stat = "Status: 200 OK";
    }
    else {
        char *newstat;
        newstat = apr_palloc(cid->r->pool, statlen + 9);
        strcpy(newstat, "Status: ");
        apr_cpystrn(newstat + 8, stat, statlen + 1);
        stat = newstat;
    }

    if (!head || headlen == 0 || !*head) {
        head = "\r\n";
    }
    else
    {
        if (head[headlen]) {
            /* Whoops... not NULL terminated */
            head = apr_pstrndup(cid->r->pool, head, headlen);
        }
    }
 
    /* Parse them out, or die trying */
    cid->r->status= ap_scan_script_header_err_strs(cid->r, NULL, &termch,
                                                  &termarg, stat, head, NULL);
    cid->ecb->dwHttpStatusCode = cid->r->status;
    if (cid->r->status == HTTP_INTERNAL_SERVER_ERROR)
        return -1;
    
    /* Headers will actually go when they are good and ready */

    /* If all went well, tell the caller we consumed the headers complete */
    if (!termch)
        return(headlen);

    /* Any data left is sent directly by the caller, all we
     * give back is the size of the headers we consumed
     */
    if (termch && (termarg == 1) && head + headlen > termch) {
        return termch - head;
    }
    return 0;
}

BOOL WINAPI ServerSupportFunction(HCONN hConn, DWORD dwHSERequest,
                                  LPVOID lpvBuffer, LPDWORD lpdwSize,
                                  LPDWORD lpdwDataType)
{
    isapi_cid *cid = (isapi_cid *)hConn;
    request_rec *r = cid->r;
    conn_rec *c = r->connection;
    request_rec *subreq;

    switch (dwHSERequest) {
    case 1: /* HSE_REQ_SEND_URL_REDIRECT_RESP */
        /* Set the status to be returned when the HttpExtensionProc()
         * is done.
         * WARNING: Microsoft now advertises HSE_REQ_SEND_URL_REDIRECT_RESP
         *          and HSE_REQ_SEND_URL as equivalant per the Jan 2000 SDK.
         *          They most definately are not, even in their own samples.
         */
        apr_table_set (r->headers_out, "Location", lpvBuffer);
        cid->r->status = cid->ecb->dwHttpStatusCode 
                                               = HTTP_MOVED_TEMPORARILY;
        return TRUE;

    case 2: /* HSE_REQ_SEND_URL */
        /* Soak up remaining input */
        if (r->remaining > 0) {
            char argsbuffer[HUGE_STRING_LEN];
            while (ap_get_client_block(r, argsbuffer, HUGE_STRING_LEN));
        }

        /* Reset the method to GET */
        r->method = apr_pstrdup(r->pool, "GET");
        r->method_number = M_GET;

        /* Don't let anyone think there's still data */
        apr_table_unset(r->headers_in, "Content-Length");

        /* AV fault per PR3598 - redirected path is lost! */
        (char*)lpvBuffer = apr_pstrdup(r->pool, (char*)lpvBuffer);
        ap_internal_redirect((char*)lpvBuffer, r);
        return TRUE;

    case 3: /* HSE_REQ_SEND_RESPONSE_HEADER */
    {
        /* Parse them out, or die trying */
        apr_size_t statlen = 0, headlen = 0;
        apr_ssize_t ate;
        if (lpvBuffer)
            statlen = strlen((char*) lpvBuffer);
        if (lpdwDataType)
            headlen = strlen((char*) lpdwDataType);
        ate = send_response_header(cid, (char*) lpvBuffer,
                                   (char*) lpdwDataType,
                                   statlen, headlen);
        if (ate < 0) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        else if ((apr_size_t)ate < headlen) {
            apr_bucket_brigade *bb;
            apr_bucket *b;
            bb = apr_brigade_create(cid->r->pool, c->bucket_alloc);
	    b = apr_bucket_transient_create((char*) lpdwDataType + ate, 
                                           headlen - ate, c->bucket_alloc);
	    APR_BRIGADE_INSERT_TAIL(bb, b);
            b = apr_bucket_flush_create(c->bucket_alloc);
	    APR_BRIGADE_INSERT_TAIL(bb, b);
	    ap_pass_brigade(cid->r->output_filters, bb);
        }
        return TRUE;
    }

    case 4: /* HSE_REQ_DONE_WITH_SESSION */
        /* Signal to resume the thread completing this request
         */
        if (cid->complete)
            SetEvent(cid->complete);            
        return TRUE;

    case 1001: /* HSE_REQ_MAP_URL_TO_PATH */
    {
        /* Map a URL to a filename */
        char *file = (char *)lpvBuffer;
        DWORD len;
        subreq = ap_sub_req_lookup_uri(apr_pstrndup(r->pool, file, *lpdwSize),
                                       r, NULL);

        len = apr_cpystrn(file, subreq->filename, *lpdwSize) - file;


        /* IIS puts a trailing slash on directories, Apache doesn't */
        if (subreq->finfo.filetype == APR_DIR) {
            if (len < *lpdwSize - 1) {
                file[len++] = '\\';
                file[len] = '\0';
            }
        }
        *lpdwSize = len;
        return TRUE;
    }

    case 1002: /* HSE_REQ_GET_SSPI_INFO */
        if (cid->dconf.log_unsupported)
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                           "ISAPI: ServerSupportFunction HSE_REQ_GET_SSPI_INFO "
                           "is not supported: %s", r->filename);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
        
    case 1003: /* HSE_APPEND_LOG_PARAMETER */
        /* Log lpvBuffer, of lpdwSize bytes, in the URI Query (cs-uri-query) field
         */
        apr_table_set(r->notes, "isapi-parameter", (char*) lpvBuffer);
        if (cid->dconf.log_to_query) {
            if (r->args)
                r->args = apr_pstrcat(r->pool, r->args, (char*) lpvBuffer, NULL);
            else
                r->args = apr_pstrdup(r->pool, (char*) lpvBuffer);
        }
        if (cid->dconf.log_to_errlog)
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_INFO, 0, r,
                          "ISAPI: %s: %s", cid->r->filename,
                          (char*) lpvBuffer);
        return TRUE;
        
    case 1005: /* HSE_REQ_IO_COMPLETION */
        /* Emulates a completion port...  Record callback address and 
         * user defined arg, we will call this after any async request 
         * (e.g. transmitfile) as if the request executed async.
         * Per MS docs... HSE_REQ_IO_COMPLETION replaces any prior call
         * to HSE_REQ_IO_COMPLETION, and lpvBuffer may be set to NULL.
         */
        if (!cid->isa->fakeasync) {
            if (cid->dconf.log_unsupported)
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                          "ISAPI: ServerSupportFunction HSE_REQ_IO_COMPLETION "
                          "is not supported: %s", r->filename);
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        cid->completion = (PFN_HSE_IO_COMPLETION) lpvBuffer;
        cid->completion_arg = (PVOID) lpdwDataType;
        return TRUE;

    case 1006: /* HSE_REQ_TRANSMIT_FILE */
    {
        HSE_TF_INFO *tf = (HSE_TF_INFO*)lpvBuffer;
        apr_status_t rv;
        apr_bucket_brigade *bb;
        apr_bucket *b;
        apr_file_t *fd;

        if (!cid->isa->fakeasync && (tf->dwFlags & HSE_IO_ASYNC)) {
            if (cid->dconf.log_unsupported)
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                         "ISAPI: ServerSupportFunction HSE_REQ_TRANSMIT_FILE "
                         "as HSE_IO_ASYNC is not supported: %s", r->filename);
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        
        if ((rv = apr_os_file_put(&fd, tf->hFile, 0, r->pool)) != APR_SUCCESS) {
            return FALSE;
        }
        
        /* apr_dupfile_oshandle (&fd, tf->hFile, r->pool); */
        bb = apr_brigade_create(r->pool, c->bucket_alloc);

        if (tf->dwFlags & HSE_IO_SEND_HEADERS) 
        {
            /* According to MS: if calling HSE_REQ_TRANSMIT_FILE with the
             * HSE_IO_SEND_HEADERS flag, then you can't otherwise call any
             * HSE_SEND_RESPONSE_HEADERS* fn, but if you don't use the flag,
             * you must have done so.  They document that the pHead headers
             * option is valid only for HSE_IO_SEND_HEADERS - we are a bit
             * more flexible and assume with the flag, pHead are the
             * response headers, and without, pHead simply contains text
             * (handled after this case).
             */
            apr_ssize_t ate = send_response_header(cid, tf->pszStatusCode, 
                                                   (char*)tf->pHead,
                                                   strlen(tf->pszStatusCode), 
                                                   tf->HeadLength);
            if (ate < 0)
            {
                apr_brigade_destroy(bb);
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if ((apr_size_t)ate < tf->HeadLength)
            {
                b = apr_bucket_transient_create((char*)tf->pHead + ate, 
                                                tf->HeadLength - ate,
                                                c->bucket_alloc);
                APR_BRIGADE_INSERT_TAIL(bb, b);
            }
        }
        else if (tf->pHead && tf->HeadLength) {
            b = apr_bucket_transient_create((char*)tf->pHead, 
                                            tf->HeadLength,
                                            c->bucket_alloc);
            APR_BRIGADE_INSERT_TAIL(bb, b);
        }

        b = apr_bucket_file_create(fd, tf->Offset, 
                                   tf->BytesToWrite, r->pool, c->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(bb, b);
        
        if (tf->pTail && tf->TailLength) {
            b = apr_bucket_transient_create((char*)tf->pTail, 
                                            tf->TailLength, c->bucket_alloc);
            APR_BRIGADE_INSERT_TAIL(bb, b);
        }
        
        b = apr_bucket_flush_create(c->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(bb, b);
        ap_pass_brigade(r->output_filters, bb);

        /* we do nothing with (tf->dwFlags & HSE_DISCONNECT_AFTER_SEND)
         */

        if (tf->dwFlags & HSE_IO_ASYNC) {
            /* XXX: Fake async response,
             * use tf->pfnHseIO, or if NULL, then use cid->fnIOComplete
             * pass pContect to the HseIO callback.
             */
        }
        return TRUE;
    }

    case 1007: /* HSE_REQ_REFRESH_ISAPI_ACL */
        if (cid->dconf.log_unsupported)
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                          "ISAPI: ServerSupportFunction "
                          "HSE_REQ_REFRESH_ISAPI_ACL "
                          "is not supported: %s", r->filename);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;

    case 1008: /* HSE_REQ_IS_KEEP_CONN */
        *((LPBOOL) lpvBuffer) = (r->connection->keepalive == 1);
        return TRUE;

    case 1010: /* XXX: Fake it : HSE_REQ_ASYNC_READ_CLIENT */
        if (cid->dconf.log_unsupported)
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                          "ISAPI: asynchronous I/O not supported: %s", 
                          r->filename);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;

    case 1011: /* HSE_REQ_GET_IMPERSONATION_TOKEN  Added in ISAPI 4.0 */
        if (cid->dconf.log_unsupported)
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                          "ISAPI: ServerSupportFunction "
                          "HSE_REQ_GET_IMPERSONATION_TOKEN "
                          "is not supported: %s", r->filename);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;

#ifdef HSE_REQ_MAP_URL_TO_PATH_EX
    case 1012: /* HSE_REQ_MAP_URL_TO_PATH_EX */
    {
        /* Map a URL to a filename */
        LPHSE_URL_MAPEX_INFO info = (LPHSE_URL_MAPEX_INFO) lpdwDataType;
        char* test_uri = apr_pstrndup(r->pool, (char *)lpvBuffer, *lpdwSize);

        subreq = ap_sub_req_lookup_uri(test_uri, r, NULL);
        info->cchMatchingURL = strlen(test_uri);        
        info->cchMatchingPath = apr_cpystrn(info->lpszPath, subreq->filename, 
                                            MAX_PATH) - info->lpszPath;

        /* Mapping started with assuming both strings matched.
         * Now roll on the path_info as a mismatch and handle
         * terminating slashes for directory matches.
         */
        if (subreq->path_info && *subreq->path_info) {
            apr_cpystrn(info->lpszPath + info->cchMatchingPath, 
                        subreq->path_info, MAX_PATH - info->cchMatchingPath);
            info->cchMatchingURL -= strlen(subreq->path_info);
            if (subreq->finfo.filetype == APR_DIR
                 && info->cchMatchingPath < MAX_PATH - 1) {
                /* roll forward over path_info's first slash */
                ++info->cchMatchingPath;
                ++info->cchMatchingURL;
            }
        }
        else if (subreq->finfo.filetype == APR_DIR
                 && info->cchMatchingPath < MAX_PATH - 1) {
            /* Add a trailing slash for directory */
            info->lpszPath[info->cchMatchingPath++] = '/';
            info->lpszPath[info->cchMatchingPath] = '\0';
        }

        /* If the matched isn't a file, roll match back to the prior slash */
        if (subreq->finfo.filetype == APR_NOFILE) {
            while (info->cchMatchingPath && info->cchMatchingURL) {
                if (info->lpszPath[info->cchMatchingPath - 1] == '/') 
                    break;
                --info->cchMatchingPath;
                --info->cchMatchingURL;
            }
        }
        
        /* Paths returned with back slashes */
        for (test_uri = info->lpszPath; *test_uri; ++test_uri)
            if (*test_uri == '/')
                *test_uri = '\\';
        
        /* is a combination of:
         * HSE_URL_FLAGS_READ         0x001 Allow read
         * HSE_URL_FLAGS_WRITE        0x002 Allow write
         * HSE_URL_FLAGS_EXECUTE      0x004 Allow execute
         * HSE_URL_FLAGS_SSL          0x008 Require SSL
         * HSE_URL_FLAGS_DONT_CACHE   0x010 Don't cache (VRoot only)
         * HSE_URL_FLAGS_NEGO_CERT    0x020 Allow client SSL cert
         * HSE_URL_FLAGS_REQUIRE_CERT 0x040 Require client SSL cert
         * HSE_URL_FLAGS_MAP_CERT     0x080 Map client SSL cert to account
         * HSE_URL_FLAGS_SSL128       0x100 Require 128-bit SSL cert
         * HSE_URL_FLAGS_SCRIPT       0x200 Allow script execution
         *
         * XxX: As everywhere, EXEC flags could use some work...
         *      and this could go further with more flags, as desired.
         */ 
        info->dwFlags = (subreq->finfo.protection & APR_UREAD    ? 0x001 : 0)
                      | (subreq->finfo.protection & APR_UWRITE   ? 0x002 : 0)
                      | (subreq->finfo.protection & APR_UEXECUTE ? 0x204 : 0);
        return TRUE;
    }
#endif

    case 1014: /* HSE_REQ_ABORTIVE_CLOSE */
        if (cid->dconf.log_unsupported)
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                          "ISAPI: ServerSupportFunction HSE_REQ_ABORTIVE_CLOSE"
                          " is not supported: %s", r->filename);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;

    case 1015: /* HSE_REQ_GET_CERT_INFO_EX  Added in ISAPI 4.0 */
        if (cid->dconf.log_unsupported)
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                          "ISAPI: ServerSupportFunction "
                          "HSE_REQ_GET_CERT_INFO_EX "
                          "is not supported: %s", r->filename);        
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;

#ifdef HSE_REQ_SEND_RESPONSE_HEADER_EX
    case 1016: /* HSE_REQ_SEND_RESPONSE_HEADER_EX  Added in ISAPI 4.0 */
    {
        LPHSE_SEND_HEADER_EX_INFO shi
                                  = (LPHSE_SEND_HEADER_EX_INFO) lpvBuffer;
        /* XXX: ignore shi->fKeepConn?  We shouldn't need the advise */
        /* r->connection->keepalive = shi->fKeepConn; */
        apr_ssize_t ate = send_response_header(cid, shi->pszStatus, 
                                               shi->pszHeader,
                                               shi->cchStatus, 
                                               shi->cchHeader);
        if (ate < 0) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        else if ((apr_size_t)ate < shi->cchHeader) {
            apr_bucket_brigade *bb;
            apr_bucket *b;
            bb = apr_brigade_create(cid->r->pool, c->bucket_alloc);
	    b = apr_bucket_transient_create(shi->pszHeader + ate, 
                                            shi->cchHeader - ate,
                                            c->bucket_alloc);
	    APR_BRIGADE_INSERT_TAIL(bb, b);
            b = apr_bucket_flush_create(c->bucket_alloc);
	    APR_BRIGADE_INSERT_TAIL(bb, b);
	    ap_pass_brigade(cid->r->output_filters, bb);
        }
        return TRUE;

    }
#endif

    case 1017: /* HSE_REQ_CLOSE_CONNECTION  Added after ISAPI 4.0 */
        if (cid->dconf.log_unsupported)
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                          "ISAPI: ServerSupportFunction "
                          "HSE_REQ_CLOSE_CONNECTION "
                          "is not supported: %s", r->filename);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;

    case 1018: /* HSE_REQ_IS_CONNECTED  Added after ISAPI 4.0 */
        /* Returns True if client is connected c.f. MSKB Q188346
         * assuming the identical return mechanism as HSE_REQ_IS_KEEP_CONN
         */
        *((LPBOOL) lpvBuffer) = (r->connection->aborted == 0);
        return TRUE;

    case 1020: /* HSE_REQ_EXTENSION_TRIGGER  Added after ISAPI 4.0 */
        /*  Undocumented - defined by the Microsoft Jan '00 Platform SDK
         */
        if (cid->dconf.log_unsupported)
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                          "ISAPI: ServerSupportFunction "
                          "HSE_REQ_EXTENSION_TRIGGER "
                          "is not supported: %s", r->filename);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;

    default:
        if (cid->dconf.log_unsupported)
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                          "ISAPI: ServerSupportFunction (%d) not supported: "
                          "%s", dwHSERequest, r->filename);
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
}

/**********************************************************
 *
 *  ISAPI Module request invocation section
 *
 **********************************************************/

apr_status_t isapi_handler (request_rec *r)
{
    isapi_dir_conf *dconf;
    apr_table_t *e;
    apr_status_t rv;
    isapi_loaded *isa;
    isapi_cid *cid;
    const char *val;
    DWORD read;
    int res;
    
    if(strcmp(r->handler, "isapi-isa"))
        return DECLINED;    

    dconf = ap_get_module_config(r->per_dir_config, &isapi_module);
    e = r->subprocess_env;

    /* Use similar restrictions as CGIs
     *
     * If this fails, it's pointless to load the isapi dll.
     */
    if (!(ap_allow_options(r) & OPT_EXECCGI))
        return HTTP_FORBIDDEN;

    if (r->finfo.filetype == APR_NOFILE)
        return HTTP_NOT_FOUND;

    if (r->finfo.filetype != APR_REG)
        return HTTP_FORBIDDEN;

    if ((r->used_path_info == AP_REQ_REJECT_PATH_INFO) &&
        r->path_info && *r->path_info)
    {
        /* default to accept */
        return HTTP_NOT_FOUND;
    }

    /* Load the isapi extention without caching (pass r value) 
     * but note that we will recover an existing cached module.
     */
    if (isapi_load(loaded.pool, r, r->filename, &isa) != APR_SUCCESS)
        return HTTP_INTERNAL_SERVER_ERROR;
        
    /* Set up variables */
    ap_add_common_vars(r);
    ap_add_cgi_vars(r);
    apr_table_setn(e, "UNMAPPED_REMOTE_USER", "REMOTE_USER");
    if ((val = apr_table_get(e, "HTTPS")) && strcmp(val, "on"))
        apr_table_setn(e, "SERVER_PORT_SECURE", "1");
    else
        apr_table_setn(e, "SERVER_PORT_SECURE", "0");
    apr_table_setn(e, "URL", r->uri);

    /* Set up connection structure and ecb */
    cid = apr_pcalloc(r->pool, sizeof(isapi_cid));

    /* Fixup defaults for dconf */
    cid->dconf.read_ahead_buflen = (dconf->read_ahead_buflen == ISAPI_UNDEF)
                                     ? 48192 : dconf->read_ahead_buflen;
    cid->dconf.log_unsupported   = (dconf->log_unsupported == ISAPI_UNDEF)
                                     ? 0 : dconf->log_unsupported;
    cid->dconf.log_to_errlog     = (dconf->log_to_errlog == ISAPI_UNDEF)
                                     ? 1 : dconf->log_to_errlog;
    cid->dconf.log_to_query      = (dconf->log_to_query == ISAPI_UNDEF)
                                     ? 1 : dconf->log_to_query;

    cid->ecb = apr_pcalloc(r->pool, sizeof(struct _EXTENSION_CONTROL_BLOCK));
    cid->ecb->ConnID = (HCONN)cid;
    cid->isa = isa;
    cid->r = r;
    cid->r->status = 0;
    cid->complete = NULL;
    cid->completion = NULL;
    
    cid->ecb->cbSize = sizeof(EXTENSION_CONTROL_BLOCK);
    cid->ecb->dwVersion = isa->report_version;
    cid->ecb->dwHttpStatusCode = 0;
    strcpy(cid->ecb->lpszLogData, "");
    // TODO: are copies really needed here?
    cid->ecb->lpszMethod = apr_pstrdup(r->pool, (char*) r->method);
    cid->ecb->lpszQueryString = apr_pstrdup(r->pool, 
                                (char*) apr_table_get(e, "QUERY_STRING"));
    cid->ecb->lpszPathInfo = apr_pstrdup(r->pool, 
                             (char*) apr_table_get(e, "PATH_INFO"));
    cid->ecb->lpszPathTranslated = apr_pstrdup(r->pool, 
                                   (char*) apr_table_get(e, "PATH_TRANSLATED"));
    cid->ecb->lpszContentType = apr_pstrdup(r->pool, 
                                (char*) apr_table_get(e, "CONTENT_TYPE"));
    /* Set up the callbacks */
    cid->ecb->GetServerVariable = GetServerVariable;
    cid->ecb->WriteClient = WriteClient;
    cid->ecb->ReadClient = ReadClient;
    cid->ecb->ServerSupportFunction = ServerSupportFunction;

    
    /* Set up client input */
    res = ap_setup_client_block(r, REQUEST_CHUNKED_ERROR);
    if (res) {
        isapi_unload(isa, FALSE);
        return res;
    }

    if (ap_should_client_block(r)) {
        /* Time to start reading the appropriate amount of data,
         * and allow the administrator to tweak the number
         * TODO: add the httpd.conf option for read_ahead_buflen.
         */
        if (r->remaining) {
            cid->ecb->cbTotalBytes = (apr_size_t)r->remaining;
            if (cid->ecb->cbTotalBytes > (DWORD)cid->dconf.read_ahead_buflen)
                cid->ecb->cbAvailable = cid->dconf.read_ahead_buflen;
            else
                cid->ecb->cbAvailable = cid->ecb->cbTotalBytes;
        }
        else
        {
            cid->ecb->cbTotalBytes = 0xffffffff;
            cid->ecb->cbAvailable = cid->dconf.read_ahead_buflen;
        }

        cid->ecb->lpbData = apr_pcalloc(r->pool, cid->ecb->cbAvailable + 1);

        read = 0;
        while (read < cid->ecb->cbAvailable &&
               ((res = ap_get_client_block(r, cid->ecb->lpbData + read,
                                        cid->ecb->cbAvailable - read)) > 0)) {
            read += res;
        }

        if (res < 0) {
            isapi_unload(isa, FALSE);
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        /* Although it's not to spec, IIS seems to null-terminate
         * its lpdData string. So we will too.
         */
        if (res == 0)
            cid->ecb->cbAvailable = cid->ecb->cbTotalBytes = read;
        else
            cid->ecb->cbAvailable = read;
        cid->ecb->lpbData[read] = '\0';
    }
    else {
        cid->ecb->cbTotalBytes = 0;
        cid->ecb->cbAvailable = 0;
        cid->ecb->lpbData = NULL;
    }

    /* All right... try and run the sucker */
    rv = (*isa->HttpExtensionProc)(cid->ecb);

    /* Check for a log message - and log it */
    if (cid->ecb->lpszLogData && *cid->ecb->lpszLogData)
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_INFO, 0, r,
                      "ISAPI: %s: %s", r->filename, cid->ecb->lpszLogData);

    switch(rv) {
        case 0:  /* Strange, but MS isapi accepts this as success */
        case HSE_STATUS_SUCCESS:
        case HSE_STATUS_SUCCESS_AND_KEEP_CONN:
            /* Ignore the keepalive stuff; Apache handles it just fine without
             * the ISA's "advice".
             * Per Microsoft: "In IIS versions 4.0 and later, the return
             * values HSE_STATUS_SUCCESS and HSE_STATUS_SUCCESS_AND_KEEP_CONN
             * are functionally identical: Keep-Alive connections are
             * maintained, if supported by the client."
             * ... so we were pat all this time
             */
            break;

        case HSE_STATUS_PENDING:    
            /* emulating async behavior...
             *
             * Create a cid->completed event and wait on it for some timeout
             * so that the app thinks is it running async.
             *
             * All async ServerSupportFunction calls will be handled through
             * the registered IO_COMPLETION hook.
             */
            
            if (!isa->fakeasync) {
                if (cid->dconf.log_unsupported)
                {
                     ap_log_rerror(APLOG_MARK, APLOG_NOERRNO|APLOG_WARNING, 0, r,
                                   "ISAPI: %s asynch I/O request refused", 
                                   r->filename);
                     cid->r->status = HTTP_INTERNAL_SERVER_ERROR;
                }
            }
            else {
                cid->complete = CreateEvent(NULL, FALSE, FALSE, NULL);
                if (WaitForSingleObject(cid->complete, isa->timeout)
                        == WAIT_TIMEOUT) {
                    /* TODO: Now what... if this hung, then do we kill our own
                     * thread to force its death?  For now leave timeout = -1
                     */
                }
            }
            break;

        case HSE_STATUS_ERROR:    
            /* end response if we have yet to do so.
             */
            cid->r->status = HTTP_INTERNAL_SERVER_ERROR;
            break;

        default:
            /* TODO: log unrecognized retval for debugging 
             */
            cid->r->status = HTTP_INTERNAL_SERVER_ERROR;
            break;
    }

    /* Set the status (for logging) */
    if (cid->ecb->dwHttpStatusCode) {
        cid->r->status = cid->ecb->dwHttpStatusCode;
    }

    /* All done with the DLL... get rid of it... */
    isapi_unload(isa, FALSE);
    
    return OK;		/* NOT r->status, even if it has changed. */
}

/**********************************************************
 *
 *  ISAPI Module Setup Hooks
 *
 **********************************************************/

static int isapi_pre_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp)
{
    apr_pool_sub_make(&loaded.pool, pconf, NULL);
    if (!loaded.pool) {
	ap_log_error(APLOG_MARK, APLOG_ERR, APR_EGENERAL, NULL,
	             "ISAPI: could not create the isapi cache pool");
        return APR_EGENERAL;
    }
    
    loaded.modules = apr_array_make(loaded.pool, 20, sizeof(isapi_loaded*));
    if (!loaded.modules) {
	ap_log_error(APLOG_MARK, APLOG_ERR, APR_EGENERAL, NULL,
                     "ISAPI: could not create the isapi cache");
        return APR_EGENERAL;
    }

    return OK;
}

static int compare_loaded(const void *av, const void *bv)
{
    const isapi_loaded **a = av;
    const isapi_loaded **b = bv;

    return strcmp((*a)->filename, (*b)->filename);
}

static int isapi_post_config(apr_pool_t *p, apr_pool_t *plog,
                             apr_pool_t *ptemp, server_rec *s)
{
    isapi_loaded **elts = (isapi_loaded **)loaded.modules->elts;
    int nelts = loaded.modules->nelts;

    /* sort the elements of the main_server, by filename */
    qsort(elts, nelts, sizeof(isapi_loaded*), compare_loaded);

    return OK;
}

static void isapi_hooks(apr_pool_t *cont)
{
    ap_hook_pre_config(isapi_pre_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_post_config(isapi_post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(isapi_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA isapi_module = {
   STANDARD20_MODULE_STUFF,
   create_isapi_dir_config,     /* create per-dir config */
   merge_isapi_dir_configs,     /* merge per-dir config */
   NULL,                        /* server config */
   NULL,                        /* merge server config */
   isapi_cmds,                  /* command apr_table_t */
   isapi_hooks                  /* register hooks */
};
