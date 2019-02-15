/* 
   Unix SMB/CIFS implementation.
   SMB torture tester
   Copyright (C) Andrew Tridgell 1997-2003
   Copyright (C) Jelmer Vernooij 2006
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "includes.h"
#include "system/time.h"
#include "torture/torture.h"
#include "build.h"
#include "lib/util/dlinklist.h"

_PUBLIC_ int torture_numops=10;
_PUBLIC_ int torture_entries=1000;
_PUBLIC_ int torture_failures=1;
_PUBLIC_ int torture_seed=0;
_PUBLIC_ int torture_numasync=100;

struct torture_suite *torture_root = NULL;

bool torture_register_suite(struct torture_suite *suite)
{
	if (!suite)
		return true;

	return torture_suite_add_suite(torture_root, suite);
}

struct torture_context *torture_context_init(TALLOC_CTX *mem_ctx, 
											 const char *known_failures_file, 
											 const struct torture_ui_ops *ui_ops)
{
	int numlines;
	struct torture_context *torture = talloc_zero(mem_ctx, 
												  struct torture_context);
	torture->expected_failures = file_lines_load(known_failures_file, &numlines,
												 torture);
	torture->ui_ops = ui_ops;

	torture->results.returncode = true;

	return torture;
}


int torture_init(void)
{
	init_module_fn static_init[] = STATIC_torture_MODULES;
	init_module_fn *shared_init = load_samba_modules(NULL, "torture");

	torture_root = talloc_zero(talloc_autofree_context(), 
							struct torture_suite);
	
	run_init_functions(static_init);
	run_init_functions(shared_init);

	talloc_free(shared_init);

	return 0;
}
