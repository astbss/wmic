/*-
 * Copyright (c) 2005 Doug Rabson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	$FreeBSD: src/lib/libgssapi/gss_canonicalize_name.c,v 1.1 2005/12/29 14:40:20 dfr Exp $
 */

#include "mech_locl.h"
RCSID("$Id: gss_canonicalize_name.c,v 1.2 2006/06/28 09:00:25 lha Exp $");

OM_uint32
gss_canonicalize_name(OM_uint32 *minor_status,
    const gss_name_t input_name,
    const gss_OID mech_type,
    gss_name_t *output_name)
{
	OM_uint32 major_status;
	struct _gss_name *name = (struct _gss_name *) input_name;
	struct _gss_mechanism_name *mn;
	gssapi_mech_interface m = __gss_get_mechanism(mech_type);
	gss_name_t new_canonical_name;

	*minor_status = 0;
	*output_name = 0;

	mn = _gss_find_mn(name, mech_type);
	if (!mn) {
		return (GSS_S_BAD_MECH);
	}

	m = mn->gmn_mech;
	major_status = m->gm_canonicalize_name(minor_status,
	    mn->gmn_name, mech_type, &new_canonical_name);
	if (major_status)
		return (major_status);

	/*
	 * Now we make a new name and mark it as an MN.
	 */
	*minor_status = 0;
	name = malloc(sizeof(struct _gss_name));
	if (!name) {
		m->gm_release_name(minor_status, &new_canonical_name);
		*minor_status = ENOMEM;
		return (GSS_S_FAILURE);
	}
	memset(name, 0, sizeof(struct _gss_name));

	mn = malloc(sizeof(struct _gss_mechanism_name));
	if (!mn) {
		m->gm_release_name(minor_status, &new_canonical_name);
		free(name);
		*minor_status = ENOMEM;
		return (GSS_S_FAILURE);
	}

	SLIST_INIT(&name->gn_mn);
	mn->gmn_mech = m;
	mn->gmn_mech_oid = &m->gm_mech_oid;
	mn->gmn_name = new_canonical_name;
	SLIST_INSERT_HEAD(&name->gn_mn, mn, gmn_link);

	*output_name = (gss_name_t) name;

	return (GSS_S_COMPLETE);
}
