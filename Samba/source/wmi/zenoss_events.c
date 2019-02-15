/*
 * zenoss_events.c
 *
 ###########################################################################
 #
 # This program is part of Zenoss Core, an open source monitoring platform.
 # Copyright (C) 2008-2011, Zenoss Inc.
 #
 # This program is free software; you can redistribute it and/or modify it
 # under the terms of the GNU General Public License version 2, or (at your
 # option) any later version, as published by the Free Software Foundation.
 #
 # For complete information please visit: http://www.zenoss.com/oss/
 #
 ###########################################################################
 *
 *  Created on: Aug 19, 2008
 *      Author: cgibbons
 */

#include "includes.h"
#include "lib/events/events.h"
#include "lib/events/events_internal.h"
#include "lib/util/dlinklist.h"

#include "zenoss_events.h"


/* to mark the ev->maxfd invalid
 * this means we need to recalculate it
 */
#define EVENT_INVALID_MAXFD (-1)

/**
 * Our event context will maintain state for all of the outstanding events
 * and file descriptors under our control. This in turn will allow us to
 * provide a set of file descriptors back to Twisted for asynchronous
 * I/O waiting.
 */
struct zenoss_event_context
{
    /* a pointer back to the generic event context */
    struct event_context* ev;

    /* a list of filedescriptor events */
    struct fd_event* fd_events;

    /* a list of timed events */
    struct timed_event* timed_events;

    /* the maximum file descriptor number in fd_events */
    int maxfd;

     /* this is changed by the destructors for the fd event
     type. It is used to detect event destruction by event
     handlers, which means the code that is calling the event
     handler needs to assume that the linked list is no longer
     valid
     */
    uint32_t destruction_count;

    /*
     * callbacks to use the twisted reactor
     */
    struct reactor_functions functions;
};

/**
 * forward reference
 */
static const struct event_ops *local_event_get_ops(void);
static void local_event_loop_timer(struct zenoss_event_context *zenoss_ev);
static int local_event_context_init(struct event_context *ev,
				    void *private_data);
static void local_calc_maxfd(struct zenoss_event_context *zenoss_ev);
static int local_event_timed_destructor(struct timed_event *te);

/*
 create a event_context structure. This must be the first events
 call, and all subsequent calls pass this event_context as the first
 element. Event handlers also receive this as their first argument.
 */
struct event_context *zenoss_event_context_init(TALLOC_CTX *mem_ctx,
						struct reactor_functions *funcs)
{
    DEBUG_FN_ENTER;
    const struct event_ops *ops = local_event_get_ops();

    struct event_context *newContext = event_context_init_ops(mem_ctx, ops, funcs);
    DEBUG_FN_EXIT;
    return newContext;
}


void zenoss_get_next_timeout(struct event_context* event_ctx, struct timeval* timeout)
{
    DEBUG_FN_ENTER;
    struct zenoss_event_context *zenoss_ev =
    	talloc_get_type(event_ctx->additional_data, struct zenoss_event_context);
	if (zenoss_ev == NULL)
	{
	    DEBUG_FN_FAIL("zenoss_ev == NULL: not of type struct zenoss_event_context");
	}

    /* work out the right timeout for all timed events */
    if (zenoss_ev->timed_events)
    {
        struct timeval t = timeval_current();
        *timeout = timeval_until(&t, &zenoss_ev->timed_events->next_event);
        if (timeval_is_zero(timeout))
        {
            local_event_loop_timer(zenoss_ev);
            DEBUG_FN_EXIT_MSG("Exiting after local_event_loop_timer");
            return;
        }
    }
    else
    {
        /* have a default tick time of 30 seconds. This guarantees
         that code that uses its own timeout checking will be
         able to proceed eventually */
        *timeout = timeval_set(30, 0);
    }
    DEBUG_FN_EXIT;
}

/**
 * lib.zenoss_read_ready(event_ctx, self.fd)
 *
 */
void zenoss_read_ready(struct event_context* event_ctx, int fd)
{

    DEBUG_FN_ENTER;
    struct zenoss_event_context *zenoss_ev =
        talloc_get_type(event_ctx->additional_data, struct zenoss_event_context);
    if (zenoss_ev == NULL)
    {
        DEBUG_FN_FAIL("zenoss_ev == NULL: not of type struct zenoss_event_context");
    }

    uint32_t destruction_count = zenoss_ev->destruction_count;

    struct fd_event *fde;

    for (fde = zenoss_ev->fd_events; fde; fde = fde->next)
    {
        if (fde->fd == fd)
        {
            fde->flags |= EVENT_FD_READ;

            fde->handler(event_ctx, fde, EVENT_FD_READ, fde->private_data);
            if (destruction_count != zenoss_ev->destruction_count)
            {
                DEBUG(9, ("fd_event destruction (#%u) detected in zenoss_read_ready: %p\n", zenoss_ev->destruction_count, fde));
                break;
            }
        }
    }

    DEBUG_FN_EXIT;
}

/**
 * lib.zenoss_write_ready(event_ctx, self.fd)
 *
 */
void zenoss_write_ready(struct event_context* event_ctx, int fd)
{
    DEBUG_FN_ENTER;

    struct zenoss_event_context *zenoss_ev =
	    talloc_get_type(event_ctx->additional_data, struct zenoss_event_context);
	if (zenoss_ev == NULL)
	{
	    DEBUG_FN_FAIL("zenoss_ev == NULL: not of type struct zenoss_event_context");
	}
	else if (zenoss_ev->fd_events == NULL)
	{
        DEBUG_FN_FAIL("zenoss_ev->fd_events == NULL");
	}

    uint32_t destruction_count = zenoss_ev->destruction_count;

    struct fd_event *fde;

    for (fde = zenoss_ev->fd_events; fde; fde = fde->next)
    {
        if (fde->fd == fd)
        {
            fde->flags |= EVENT_FD_WRITE;
            fde->handler(event_ctx, fde, EVENT_FD_WRITE, fde->private_data);
            if (destruction_count != zenoss_ev->destruction_count)
            {
                DEBUG(9, ("fd_event destruction detected in zenoss_write_ready: %p\n", fde));
                break;
            }
        }
    }

    DEBUG_FN_EXIT;
}

static int local_event_timed_deny_destructor(struct timed_event *te)
{
    return -1;
}

/*
 a timer has gone off - call it
 */
static void local_event_loop_timer(struct zenoss_event_context *zenoss_ev)
{
    DEBUG_FN_ENTER;

    struct timeval t = timeval_current();
    struct timed_event *te = zenoss_ev->timed_events;

    if (te == NULL)
    {
        DEBUG_FN_FAIL("te == NULL");
        return;
    }

    /* deny the handler to free the event */
    talloc_set_destructor(te, local_event_timed_deny_destructor);

    /* We need to remove the timer from the list before calling the
     * handler because in a semi-async inner event loop called from the
     * handler we don't want to come across this event again -- vl */
    DLIST_REMOVE(zenoss_ev->timed_events, te);
    talloc_steal(NULL, te);

    te->handler(zenoss_ev->ev, te, t, te->private_data);

    /* The destructor isn't necessary anymore, we've already removed the
     * event from the list. */
    talloc_set_destructor(te, NULL);

    talloc_free(te);

    DEBUG_FN_EXIT;
}

static int local_event_context_init(struct event_context *ev,
				    void *private_data)
{
    DEBUG_FN_ENTER;
    struct zenoss_event_context* zenoss_ev;
    struct reactor_functions* funcs = (struct reactor_functions *)private_data;
    zenoss_ev = talloc_zero(ev, struct zenoss_event_context);
    if (!zenoss_ev) {
        DEBUG_FN_FAIL("Out of memory: talloc_zero(zenoss_event_context) failed.");
        return -1;
    }
    zenoss_ev->ev = ev;
    zenoss_ev->functions = *funcs;

    ev->additional_data = zenoss_ev;
    DEBUG_FN_EXIT;
    return 0;
}

/*
 recalculate the maxfd
 */
static void local_calc_maxfd(struct zenoss_event_context *zenoss_ev)
{
    DEBUG_FN_ENTER;

    struct fd_event *fde;

    zenoss_ev->maxfd = 0;
    for (fde = zenoss_ev->fd_events; fde; fde = fde->next)
    {
        if (fde->fd > zenoss_ev->maxfd)
        {
            zenoss_ev->maxfd = fde->fd;
        }
    }

    DEBUG_FN_EXIT;
}

/*
 destroy an fd_event
 */
static int local_event_fd_destructor(struct fd_event *fde)
{
    DEBUG_FN_ENTER;
    struct event_context *ev = fde->event_ctx;
    struct zenoss_event_context *zenoss_ev =
        talloc_get_type(ev->additional_data, struct zenoss_event_context);
    if (zenoss_ev == NULL)
    {
        DEBUG_FN_FAIL("zenoss_ev == NULL: not of type struct zenoss_event_context");
    }

    DEBUG(9, ("event_destructor: fd=%d\n", fde->fd));

    if (zenoss_ev->maxfd == fde->fd)
    {
        zenoss_ev->maxfd = EVENT_INVALID_MAXFD;
    }
    DLIST_REMOVE(zenoss_ev->fd_events, fde);
    zenoss_ev->functions.update_reactor_callback(fde->fd, 0);

    zenoss_ev->destruction_count++;

    DEBUG_FN_EXIT;
    return 0;
}

static struct fd_event *local_event_add_fd(struct event_context *ev,
        TALLOC_CTX *mem_ctx, int fd, uint16_t flags,
        event_fd_handler_t handler, void *private_data)
{
    DEBUG_FN_ENTER;
    DEBUG(9, ("event_add_fd: fd=%d flags=%04x\n", fd, flags));

    struct zenoss_event_context *zenoss_ev =
        talloc_get_type(ev->additional_data, struct zenoss_event_context);
    if (zenoss_ev == NULL)
    {
        DEBUG_FN_FAIL("zenoss_ev == NULL: not of type struct zenoss_event_context");
    }

    struct fd_event *fde;

    fde = talloc(mem_ctx != NULL ? mem_ctx:ev, struct fd_event);
    if (!fde)
    {
	    DEBUG_FN_FAIL("Out of memory: talloc(fd_event) failed.");
        return NULL;
	}

    fde->event_ctx = ev;
    fde->fd = fd;
    fde->flags = flags;
    fde->handler = handler;
    fde->private_data = private_data;
    fde->additional_flags = 0;
    fde->additional_data = NULL;

    DLIST_ADD(zenoss_ev->fd_events, fde);
    if (fde->fd > zenoss_ev->maxfd)
    {
        zenoss_ev->maxfd = fde->fd;
    }
    talloc_set_destructor(fde, local_event_fd_destructor);

    // update the reactor since we might need to update our read or write
    // selector
    zenoss_ev->functions.update_reactor_callback(fde->fd, flags);

    DEBUG_FN_EXIT;
    return fde;
}

static uint16_t local_event_get_fd_flags(struct fd_event *fde)
{
    DEBUG_FN_ENTER;
    DEBUG_FN_EXIT;
    return fde->flags;
}

static void local_event_set_fd_flags(struct fd_event *fde, uint16_t flags)
{
    DEBUG_FN_ENTER;
    DEBUG(9, ("event_set_fd_flags: fde->fd=%d flags new=%04x old=%04x\n", fde->fd, flags, fde->flags));
    if (fde->flags == flags)
    {
        DEBUG_FN_EXIT_MSG("fde->flags already match");
        return;
    }

    DEBUG(9, ("Updating reactor for fd %d\n", fde->fd));
    fde->flags = flags;

    // update the reactor since we might need to update our read or write
    // selector
    struct zenoss_event_context *zenoss_ev = talloc_get_type(
        fde->event_ctx->additional_data, struct zenoss_event_context);
    if (zenoss_ev == NULL)
    {
        DEBUG_FN_FAIL("zenoss_ev == NULL: not of type struct zenoss_event_context");
    }

    zenoss_ev->functions.update_reactor_callback(fde->fd, flags);
    DEBUG_FN_EXIT;
}

static struct timed_event *local_event_add_timed(struct event_context *ev,
        TALLOC_CTX *mem_ctx, struct timeval next_event,
        event_timed_handler_t handler, void *private_data)
{
    DEBUG_FN_ENTER;
    DEBUG(9, ("event_add_timed: handler=%p next_event=%u.%u\n", handler, next_event.tv_sec, next_event.tv_usec));
    struct zenoss_event_context *zenoss_ev =
        talloc_get_type(ev->additional_data, struct zenoss_event_context);
    if (zenoss_ev == NULL) {
        DEBUG_FN_FAIL("zenoss_ev == NULL: not of type struct zenoss_event_context");
    }

    struct timed_event *te, *last_te, *cur_te;

    te = talloc(mem_ctx?mem_ctx:ev, struct timed_event);
    if (te == NULL)
    {
        DEBUG_FN_FAIL("Out of memory: talloc(timed_event) failed.");
        return NULL;
    }

    te->event_ctx = ev;
    te->next_event = next_event;
    te->handler = handler;
    te->private_data = private_data;
    te->additional_data = NULL;

    /* keep the list ordered */
    last_te = NULL;
    for (cur_te = zenoss_ev->timed_events; cur_te; cur_te = cur_te->next)
    {
        /* if the new event comes before the current one break */
        if (!timeval_is_zero(&cur_te->next_event) &&
                timeval_compare(&te->next_event, &cur_te->next_event) < 0)
        {
            break;
        }

        last_te = cur_te;
    }

    DLIST_ADD_AFTER(zenoss_ev->timed_events, te, last_te);

    talloc_set_destructor(te, local_event_timed_destructor);

    DEBUG_FN_EXIT;
    return te;
}

/*
 destroy a timed event
 */
static int local_event_timed_destructor(struct timed_event *te)
{
    DEBUG_FN_ENTER;
    struct zenoss_event_context *zenoss_ev =
        talloc_get_type(te->event_ctx->additional_data, struct zenoss_event_context);
    if (zenoss_ev == NULL)
    {
        DEBUG_FN_FAIL("zenoss_ev == NULL: not of type struct zenoss_event_context");
    }

    DLIST_REMOVE(zenoss_ev->timed_events, te);
    DEBUG_FN_EXIT;
    return 0;
}

static int local_event_loop_once(struct event_context *ev)
{
    DEBUG_FN_ENTER;
    // TODO: callback to python and do reactor.doIteration?
    struct zenoss_event_context *zenoss_ev =
	    talloc_get_type(ev->additional_data, struct zenoss_event_context);
    if (zenoss_ev == NULL)
    {
        DEBUG_FN_FAIL("zenoss_ev == NULL: not of type struct zenoss_event_context");
    }

    DEBUG_FN_EXIT;
    return zenoss_ev->functions.reactor_once();
}


static int local_event_loop_wait(struct event_context *ev)
{
    DEBUG_FN_ENTER;
    // should never be called from anywhere since we're in a 100% asynchronous
    // setup
    DEBUG_FN_FAIL("Function local_event_loop_wait should never be called. Aborting.");
    abort();
}

static const struct event_ops local_event_ops =
{
    .context_init = local_event_context_init,
    .add_fd = local_event_add_fd,
    .get_fd_flags = local_event_get_fd_flags,
    .set_fd_flags = local_event_set_fd_flags,
    .add_timed = local_event_add_timed,
    .loop_once = local_event_loop_once,
    .loop_wait = local_event_loop_wait,
};

static const struct event_ops *local_event_get_ops(void)
{
    return &local_event_ops;
}


