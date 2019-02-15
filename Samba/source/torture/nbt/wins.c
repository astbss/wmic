/* 
   Unix SMB/CIFS implementation.

   NBT WINS server testing

   Copyright (C) Andrew Tridgell 2005
   
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
#include "lib/socket/socket.h"
#include "libcli/resolve/resolve.h"
#include "system/network.h"
#include "lib/socket/netif.h"
#include "librpc/gen_ndr/ndr_nbt.h"
#include "torture/torture.h"
#include "torture/nbt/proto.h"

#define CHECK_VALUE(tctx, v, correct) \
	torture_assert_int_equal(tctx, v, correct, "Incorrect value")

#define CHECK_STRING(tctx, v, correct) \
	torture_assert_casestr_equal(tctx, v, correct, "Incorrect value")

#define CHECK_NAME(tctx, _name, correct) do { \
	CHECK_STRING(tctx, (_name).name, (correct).name); \
	CHECK_VALUE(tctx, (uint8_t)(_name).type, (uint8_t)(correct).type); \
	CHECK_STRING(tctx, (_name).scope, (correct).scope); \
} while (0)


/*
  test operations against a WINS server
*/
static bool nbt_test_wins_name(struct torture_context *tctx, const char *address,
			       struct nbt_name *name, uint16_t nb_flags)
{
	struct nbt_name_register_wins io;
	struct nbt_name_query query;
	struct nbt_name_refresh_wins refresh;
	struct nbt_name_release release;
	NTSTATUS status;
	struct nbt_name_socket *nbtsock = nbt_name_socket_init(tctx, NULL);
	const char *myaddress = talloc_strdup(tctx, iface_best_ip(address));
	struct socket_address *socket_address;

	socket_address = socket_address_from_strings(tctx, 
						     nbtsock->sock->backend_name,
						     myaddress, 0);
	torture_assert(tctx, socket_address != NULL, 
				   "Error getting address");

	/* we do the listen here to ensure the WINS server receives the packets from
	   the right IP */
	status = socket_listen(nbtsock->sock, socket_address, 0, 0);
	torture_assert_ntstatus_ok(tctx, status, 
							   "socket_listen for WINS failed");
	talloc_free(socket_address);

	torture_comment(tctx, "Testing name registration to WINS with name %s at %s nb_flags=0x%x\n", 
	       nbt_name_string(tctx, name), myaddress, nb_flags);

	torture_comment(tctx, "release the name\n");
	release.in.name = *name;
	release.in.dest_addr = address;
	release.in.address = myaddress;
	release.in.nb_flags = nb_flags;
	release.in.broadcast = False;
	release.in.timeout = 3;
	release.in.retries = 0;

	status = nbt_name_release(nbtsock, tctx, &release);
	torture_assert_ntstatus_ok(tctx, status, talloc_asprintf(tctx, "Bad response from %s for name query", address));
	CHECK_VALUE(tctx, release.out.rcode, 0);

	torture_comment(tctx, "register the name\n");
	io.in.name = *name;
	io.in.wins_servers = str_list_make(tctx, address, NULL);
	io.in.addresses = str_list_make(tctx, myaddress, NULL);
	io.in.nb_flags = nb_flags;
	io.in.ttl = 300000;
	
	status = nbt_name_register_wins(nbtsock, tctx, &io);
	torture_assert_ntstatus_ok(tctx, status, talloc_asprintf(tctx, "Bad response from %s for name register", address));
	
	CHECK_STRING(tctx, io.out.wins_server, address);
	CHECK_VALUE(tctx, io.out.rcode, 0);

	if (name->type != NBT_NAME_MASTER &&
	    name->type != NBT_NAME_LOGON && 
	    name->type != NBT_NAME_BROWSER && 
	    (nb_flags & NBT_NM_GROUP)) {
		torture_comment(tctx, "Try to register as non-group\n");
		io.in.nb_flags &= ~NBT_NM_GROUP;
		status = nbt_name_register_wins(nbtsock, tctx, &io);
		torture_assert_ntstatus_ok(tctx, status, talloc_asprintf(tctx, "Bad response from %s for name register\n",
			address));
		CHECK_VALUE(tctx, io.out.rcode, NBT_RCODE_ACT);
	}

	torture_comment(tctx, "query the name to make sure its there\n");
	query.in.name = *name;
	query.in.dest_addr = address;
	query.in.broadcast = False;
	query.in.wins_lookup = True;
	query.in.timeout = 3;
	query.in.retries = 0;

	status = nbt_name_query(nbtsock, tctx, &query);
	if (name->type == NBT_NAME_MASTER) {
		torture_assert_ntstatus_equal(
			  tctx, status, NT_STATUS_OBJECT_NAME_NOT_FOUND, 
			  talloc_asprintf(tctx, "Bad response from %s for name query", address));
		return true;
	}
	torture_assert_ntstatus_ok(tctx, status, talloc_asprintf(tctx, "Bad response from %s for name query", address));
	
	CHECK_NAME(tctx, query.out.name, *name);
	CHECK_VALUE(tctx, query.out.num_addrs, 1);
	if (name->type != NBT_NAME_LOGON &&
	    (nb_flags & NBT_NM_GROUP)) {
		CHECK_STRING(tctx, query.out.reply_addrs[0], "255.255.255.255");
	} else {
		CHECK_STRING(tctx, query.out.reply_addrs[0], myaddress);
	}


	query.in.name.name = strupper_talloc(tctx, name->name);
	if (query.in.name.name &&
	    strcmp(query.in.name.name, name->name) != 0) {
		torture_comment(tctx, "check case sensitivity\n");
		status = nbt_name_query(nbtsock, tctx, &query);
		torture_assert_ntstatus_equal(tctx, status, NT_STATUS_OBJECT_NAME_NOT_FOUND, talloc_asprintf(tctx, "Bad response from %s for name query", address));
	}

	query.in.name = *name;
	if (name->scope) {
		query.in.name.scope = strupper_talloc(tctx, name->scope);
	}
	if (query.in.name.scope &&
	    strcmp(query.in.name.scope, name->scope) != 0) {
		torture_comment(tctx, "check case sensitivity on scope\n");
		status = nbt_name_query(nbtsock, tctx, &query);
		torture_assert_ntstatus_equal(tctx, status, NT_STATUS_OBJECT_NAME_NOT_FOUND, talloc_asprintf(tctx, "Bad response from %s for name query", address));
	}

	torture_comment(tctx, "refresh the name\n");
	refresh.in.name = *name;
	refresh.in.wins_servers = str_list_make(tctx, address, NULL);
	refresh.in.addresses = str_list_make(tctx, myaddress, NULL);
	refresh.in.nb_flags = nb_flags;
	refresh.in.ttl = 12345;
	
	status = nbt_name_refresh_wins(nbtsock, tctx, &refresh);
	torture_assert_ntstatus_ok(tctx, status, talloc_asprintf(tctx, "Bad response from %s for name refresh", address));
	
	CHECK_STRING(tctx, refresh.out.wins_server, address);
	CHECK_VALUE(tctx, refresh.out.rcode, 0);

	torture_comment(tctx, "release the name\n");
	release.in.name = *name;
	release.in.dest_addr = address;
	release.in.address = myaddress;
	release.in.nb_flags = nb_flags;
	release.in.broadcast = False;
	release.in.timeout = 3;
	release.in.retries = 0;

	status = nbt_name_release(nbtsock, tctx, &release);
	torture_assert_ntstatus_ok(tctx, status, talloc_asprintf(tctx, "Bad response from %s for name query", address));
	
	CHECK_NAME(tctx, release.out.name, *name);
	CHECK_VALUE(tctx, release.out.rcode, 0);

	torture_comment(tctx, "release again\n");
	status = nbt_name_release(nbtsock, tctx, &release);
	torture_assert_ntstatus_ok(tctx, status, 
				talloc_asprintf(tctx, "Bad response from %s for name query",
		       address));
	
	CHECK_NAME(tctx, release.out.name, *name);
	CHECK_VALUE(tctx, release.out.rcode, 0);


	torture_comment(tctx, "query the name to make sure its gone\n");
	query.in.name = *name;
	status = nbt_name_query(nbtsock, tctx, &query);
	if (name->type != NBT_NAME_LOGON &&
	    (nb_flags & NBT_NM_GROUP)) {
		torture_assert_ntstatus_ok(tctx, status, 
				"ERROR: Name query failed after group release");
	} else {
		torture_assert_ntstatus_equal(tctx, status, 
									  NT_STATUS_OBJECT_NAME_NOT_FOUND,
				"Incorrect response to name query");
	}
	
	return true;
}



/*
  test operations against a WINS server
*/
static bool nbt_test_wins(struct torture_context *tctx)
{
	struct nbt_name name;
	uint32_t r = (uint32_t)(random() % (100000));
	const char *address;
	bool ret = true;

	if (!torture_nbt_get_name(tctx, &name, &address))
		return false;

	name.name = talloc_asprintf(tctx, "_TORTURE-%5u", r);

	name.type = NBT_NAME_CLIENT;
	name.scope = NULL;
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H);

	name.type = NBT_NAME_MASTER;
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H);

	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H | NBT_NM_GROUP);

	name.type = NBT_NAME_SERVER;
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H);

	name.type = NBT_NAME_LOGON;
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H | NBT_NM_GROUP);

	name.type = NBT_NAME_BROWSER;
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H | NBT_NM_GROUP);

	name.type = NBT_NAME_PDC;
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H);

	name.type = 0xBF;
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H);

	name.type = 0xBE;
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H);

	name.scope = "example";
	name.type = 0x72;
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H);

	name.scope = "example";
	name.type = 0x71;
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H | NBT_NM_GROUP);

	name.scope = "foo.example.com";
	name.type = 0x72;
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H);

	name.name = talloc_asprintf(tctx, "_T\01-%5u.foo", r);
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H);

	name.name = "";
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H);

	name.name = talloc_asprintf(tctx, ".");
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H);

	name.name = talloc_asprintf(tctx, "%5u-\377\200\300FOO", r);
	ret &= nbt_test_wins_name(tctx, address, &name, NBT_NODE_H);

	return ret;
}

/*
  test WINS operations
*/
struct torture_suite *torture_nbt_wins(void)
{
	struct torture_suite *suite = torture_suite_create(talloc_autofree_context(), 
													   "WINS");

	torture_suite_add_simple_test(suite, "wins", nbt_test_wins);

	return suite;
}
