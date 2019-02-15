/* 
   Unix SMB/CIFS implementation.

   local test for irpc code

   Copyright (C) Andrew Tridgell 2004
   
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
#include "lib/events/events.h"
#include "lib/messaging/irpc.h"
#include "librpc/gen_ndr/ndr_echo.h"
#include "torture/torture.h"

const uint32_t MSG_ID1 = 1, MSG_ID2 = 2;

static BOOL test_debug;

struct irpc_test_data
{
	struct messaging_context *msg_ctx1, *msg_ctx2;
	struct event_context *ev;
};

/*
  serve up AddOne over the irpc system
*/
static NTSTATUS irpc_AddOne(struct irpc_message *irpc, struct echo_AddOne *r)
{
	*r->out.out_data = r->in.in_data + 1;
	if (test_debug) {
		printf("irpc_AddOne: in=%u in+1=%u out=%u\n", 
			r->in.in_data, r->in.in_data+1, *r->out.out_data);
	}
	return NT_STATUS_OK;
}

/*
  a deferred reply to echodata
*/
static void deferred_echodata(struct event_context *ev, struct timed_event *te, 
			      struct timeval t, void *private)
{
	struct irpc_message *irpc = talloc_get_type(private, struct irpc_message);
	struct echo_EchoData *r = irpc->data;
	r->out.out_data = talloc_memdup(r, r->in.in_data, r->in.len);
	if (r->out.out_data == NULL) {
		irpc_send_reply(irpc, NT_STATUS_NO_MEMORY);
	}
	printf("sending deferred reply\n");
	irpc_send_reply(irpc, NT_STATUS_OK);
}


/*
  serve up EchoData over the irpc system
*/
static NTSTATUS irpc_EchoData(struct irpc_message *irpc, struct echo_EchoData *r)
{
	irpc->defer_reply = True;
	event_add_timed(irpc->ev, irpc, timeval_zero(), deferred_echodata, irpc);
	return NT_STATUS_OK;
}


/*
  test a addone call over the internal messaging system
*/
static bool test_addone(struct torture_context *test, const void *_data,
			const void *_value)
{
	struct echo_AddOne r;
	NTSTATUS status;
	const struct irpc_test_data *data = _data;
	uint32_t value = (uint32_t)_value;

	/* make the call */
	r.in.in_data = value;

	test_debug = True;
	status = IRPC_CALL(data->msg_ctx1, MSG_ID2, rpcecho, ECHO_ADDONE, &r, test);
	test_debug = False;
	torture_assert_ntstatus_ok(test, status, "AddOne failed");

	/* check the answer */
	torture_assert(test, *r.out.out_data == r.in.in_data + 1, 
				   "AddOne wrong answer");

	torture_comment(test, "%u + 1 = %u\n", r.in.in_data, *r.out.out_data);
	return true;
}

/*
  test a echodata call over the internal messaging system
*/
static bool test_echodata(struct torture_context *tctx,
						  const void *tcase_data,
						  const void *test_data)
{
	struct echo_EchoData r;
	NTSTATUS status;
	const struct irpc_test_data *data = tcase_data;
	TALLOC_CTX *mem_ctx = tctx;

	/* make the call */
	r.in.in_data = (unsigned char *)talloc_strdup(mem_ctx, "0123456789");
	r.in.len = strlen((char *)r.in.in_data);

	status = IRPC_CALL(data->msg_ctx1, MSG_ID2, rpcecho, ECHO_ECHODATA, &r, 
					   mem_ctx);
	torture_assert_ntstatus_ok(tctx, status, "EchoData failed");

	/* check the answer */
	if (memcmp(r.out.out_data, r.in.in_data, r.in.len) != 0) {
		NDR_PRINT_OUT_DEBUG(echo_EchoData, &r);
		torture_fail(tctx, "EchoData wrong answer");
	}

	torture_comment(tctx, "Echo '%*.*s' -> '%*.*s'\n", 
	       r.in.len, r.in.len,
	       r.in.in_data,
	       r.in.len, r.in.len,
	       r.out.out_data);
	return true;
}


static void irpc_callback(struct irpc_request *irpc)
{
	struct echo_AddOne *r = irpc->r;
	int *pong_count = (int *)irpc->async.private;
	NTSTATUS status = irpc_call_recv(irpc);
	if (!NT_STATUS_IS_OK(status)) {
		printf("irpc call failed - %s\n", nt_errstr(status));
	}
	if (*r->out.out_data != r->in.in_data + 1) {
		printf("AddOne wrong answer - %u + 1 = %u should be %u\n", 
		       r->in.in_data, *r->out.out_data, r->in.in_data+1);
	}
	(*pong_count)++;
}

/*
  test echo speed
*/
static bool test_speed(struct torture_context *tctx,
					   const void *tcase_data,
					   const void *test_data)
{
	int ping_count = 0;
	int pong_count = 0;
	const struct irpc_test_data *data = tcase_data;
	struct timeval tv;
	struct echo_AddOne r;
	TALLOC_CTX *mem_ctx = tctx;
	int timelimit = torture_setting_int(tctx, "timelimit", 10);

	tv = timeval_current();

	r.in.in_data = 0;

	torture_comment(tctx, "Sending echo for %d seconds\n", timelimit);
	while (timeval_elapsed(&tv) < timelimit) {
		struct irpc_request *irpc;

		irpc = IRPC_CALL_SEND(data->msg_ctx1, MSG_ID2, rpcecho, ECHO_ADDONE, 
							  &r, mem_ctx);
		torture_assert(tctx, irpc != NULL, "AddOne send failed");

		irpc->async.fn = irpc_callback;
		irpc->async.private = &pong_count;

		ping_count++;

		while (ping_count > pong_count + 20) {
			event_loop_once(data->ev);
		}
	}

	torture_comment(tctx, "waiting for %d remaining replies (done %d)\n", 
	       ping_count - pong_count, pong_count);
	while (timeval_elapsed(&tv) < 30 && pong_count < ping_count) {
		event_loop_once(data->ev);
	}

	torture_assert_int_equal(tctx, ping_count, pong_count, "ping test failed");

	torture_comment(tctx, "echo rate of %.0f messages/sec\n", 
	       (ping_count+pong_count)/timeval_elapsed(&tv));
	return true;
}


static BOOL irpc_setup(struct torture_context *tctx, void **_data)
{
	struct irpc_test_data *data;

	*_data = data = talloc(tctx, struct irpc_test_data);

	lp_set_cmdline("lock dir", "lockdir.tmp");

	data->ev = event_context_init(tctx);
	torture_assert(tctx, data->msg_ctx1 = messaging_init(tctx, MSG_ID1, data->ev),
				   "Failed to init first messaging context");

	torture_assert(tctx, data->msg_ctx2 = messaging_init(tctx, MSG_ID2, data->ev),
				   "Failed to init second messaging context");

	/* register the server side function */
	IRPC_REGISTER(data->msg_ctx1, rpcecho, ECHO_ADDONE, irpc_AddOne, NULL);
	IRPC_REGISTER(data->msg_ctx2, rpcecho, ECHO_ADDONE, irpc_AddOne, NULL);

	IRPC_REGISTER(data->msg_ctx1, rpcecho, ECHO_ECHODATA, irpc_EchoData, NULL);
	IRPC_REGISTER(data->msg_ctx2, rpcecho, ECHO_ECHODATA, irpc_EchoData, NULL);

	return True;
}

struct torture_suite *torture_local_irpc(TALLOC_CTX *mem_ctx)
{
	struct torture_suite *suite = torture_suite_create(mem_ctx, "IRPC");
	struct torture_tcase *tcase = torture_suite_add_tcase(suite, "irpc");
	int i;
	uint32_t *values = talloc_array(tcase, uint32_t, 5);

	values[0] = 0;
	values[1] = 0x7FFFFFFE;
	values[2] = 0xFFFFFFFE;
	values[3] = 0xFFFFFFFF;
	values[4] = random() & 0xFFFFFFFF;

	tcase->setup = irpc_setup;

	for (i = 0; i < 5; i++) {
		torture_tcase_add_test(tcase, "addone", test_addone, (void *)values[i]);
	}
						   
	torture_tcase_add_test(tcase, "echodata", test_echodata, NULL);
	torture_tcase_add_test(tcase, "speed", test_speed, NULL);

	return suite;
}
