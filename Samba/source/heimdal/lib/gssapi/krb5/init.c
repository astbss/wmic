/*
 * Copyright (c) 1997 - 2001, 2003 Kungliga Tekniska H�gskolan
 * (Royal Institute of Technology, Stockholm, Sweden). 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 *
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *
 * 3. Neither the name of the Institute nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE 
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE. 
 */

#include "krb5/gsskrb5_locl.h"

RCSID("$Id: init.c,v 1.9 2006/10/07 22:14:58 lha Exp $");

static HEIMDAL_MUTEX _gsskrb5_context_mutex = HEIMDAL_MUTEX_INITIALIZER;
static int created_key;
static HEIMDAL_thread_key gssapi_context_key;

static void
gssapi_destroy_thread_context(void *ptr)
{
    struct gssapi_thr_context *ctx = ptr;

    if (ctx == NULL)
	return;
    if (ctx->error_string)
	free(ctx->error_string);
    HEIMDAL_MUTEX_destroy(&ctx->mutex);
    free(ctx);
}


struct gssapi_thr_context *
_gsskrb5_get_thread_context(int createp)
{
    struct gssapi_thr_context *ctx;
    int ret;

    HEIMDAL_MUTEX_lock(&_gsskrb5_context_mutex);

    if (!created_key)
	abort();
    ctx = HEIMDAL_getspecific(gssapi_context_key);
    if (ctx == NULL) {
	if (!createp)
	    goto fail;
	ctx = malloc(sizeof(*ctx));
	if (ctx == NULL)
	    goto fail;
	ctx->error_string = NULL;
	HEIMDAL_MUTEX_init(&ctx->mutex);
	HEIMDAL_setspecific(gssapi_context_key, ctx, ret);
	if (ret)
	    goto fail;
    }
    HEIMDAL_MUTEX_unlock(&_gsskrb5_context_mutex);
    return ctx;
 fail:
    HEIMDAL_MUTEX_unlock(&_gsskrb5_context_mutex);
    if (ctx)
	free(ctx);
    return NULL;
}

krb5_error_code
_gsskrb5_init (void)
{
    krb5_error_code ret = 0;

    HEIMDAL_MUTEX_lock(&_gsskrb5_context_mutex);

    if(_gsskrb5_context == NULL)
	ret = krb5_init_context (&_gsskrb5_context);
    if (ret == 0 && !created_key) {
	HEIMDAL_key_create(&gssapi_context_key, 
			   gssapi_destroy_thread_context,
			   ret);
	if (ret) {
	    krb5_free_context(_gsskrb5_context);
	    _gsskrb5_context = NULL;
	} else
	    created_key = 1;
    }

    HEIMDAL_MUTEX_unlock(&_gsskrb5_context_mutex);

    return ret;
}
