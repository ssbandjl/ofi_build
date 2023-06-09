/*
 * Copyright (c) 2014 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include "sock.h"
#include "sock_util.h"

#define SOCK_LOG_DBG(...) _SOCK_LOG_DBG(FI_LOG_CORE, __VA_ARGS__)
#define SOCK_LOG_ERROR(...) _SOCK_LOG_ERROR(FI_LOG_CORE, __VA_ARGS__)

static int sock_poll_add(struct fid_poll *pollset, struct fid *event_fid,
			 uint64_t flags)
{
	struct sock_poll *poll;
	struct sock_fid_list *list_item;
	struct sock_cq *cq;
	struct sock_cntr *cntr;

	poll = container_of(pollset, struct sock_poll, poll_fid.fid);
	list_item = calloc(1, sizeof(*list_item));
	if (!list_item)
		return -FI_ENOMEM;

	list_item->fid = event_fid;
	dlist_init(&list_item->entry);
	dlist_insert_after(&list_item->entry, &poll->fid_list);

	switch (list_item->fid->fclass) {
	case FI_CLASS_CQ:
		cq = container_of(list_item->fid, struct sock_cq, cq_fid);
		ofi_atomic_inc32(&cq->ref);
		break;
	case FI_CLASS_CNTR:
		cntr = container_of(list_item->fid, struct sock_cntr, cntr_fid);
		ofi_atomic_inc32(&cntr->ref);
		break;
	default:
		SOCK_LOG_ERROR("Invalid fid class\n");
		return -FI_EINVAL;
	}
	return 0;
}

static int sock_poll_del(struct fid_poll *pollset, struct fid *event_fid,
			 uint64_t flags)
{
	struct sock_poll *poll;
	struct sock_fid_list *list_item;
	struct dlist_entry *p, *head;
	struct sock_cq *cq;
	struct sock_cntr *cntr;

	poll = container_of(pollset, struct sock_poll, poll_fid.fid);
	head = &poll->fid_list;
	for (p = head->next; p != head; p = p->next) {
		list_item = container_of(p, struct sock_fid_list, entry);
		if (list_item->fid == event_fid) {
			dlist_remove(p);
			switch (list_item->fid->fclass) {
			case FI_CLASS_CQ:
				cq = container_of(list_item->fid, struct sock_cq, cq_fid);
				ofi_atomic_dec32(&cq->ref);
				break;
			case FI_CLASS_CNTR:
				cntr = container_of(list_item->fid, struct sock_cntr, cntr_fid);
				ofi_atomic_dec32(&cntr->ref);
				break;
			default:
				SOCK_LOG_ERROR("Invalid fid class\n");
				break;
			}
			free(list_item);
			break;
		}
	}
	return 0;
}

static int sock_poll_poll(struct fid_poll *pollset, void **context, int count)
{
	struct sock_poll *poll;
	struct sock_cq *cq;
	struct sock_eq *eq;
	struct sock_cntr *cntr;
	struct sock_fid_list *list_item;
	struct dlist_entry *p, *head;
	int ret_count = 0;

	poll = container_of(pollset, struct sock_poll, poll_fid.fid);
	head = &poll->fid_list;

	for (p = head->next; p != head && ret_count < count; p = p->next) {
		list_item = container_of(p, struct sock_fid_list, entry);
		switch (list_item->fid->fclass) {
		case FI_CLASS_CQ:
			cq = container_of(list_item->fid, struct sock_cq,
						cq_fid);
			sock_cq_progress(cq);
			pthread_mutex_lock(&cq->lock);
			if (ofi_rbfdused(&cq->cq_rbfd) || ofi_rbused(&cq->cqerr_rb)) {
				*context++ = cq->cq_fid.fid.context;
				ret_count++;
			}
			pthread_mutex_unlock(&cq->lock);
			break;

		case FI_CLASS_CNTR:
			cntr = container_of(list_item->fid, struct sock_cntr,
						cntr_fid);
			sock_cntr_progress(cntr);
			pthread_mutex_lock(&cntr->mut);
			if (ofi_atomic_get32(&cntr->value) !=
			    ofi_atomic_get32(&cntr->last_read_val)) {
				ofi_atomic_set32(&cntr->last_read_val,
					   ofi_atomic_get32(&cntr->value));
				*context++ = cntr->cntr_fid.fid.context;
				ret_count++;
			}
			pthread_mutex_unlock(&cntr->mut);
			break;

		case FI_CLASS_EQ:
			eq = container_of(list_item->fid, struct sock_eq, eq);
			ofi_mutex_lock(&eq->lock);
			if (!dlistfd_empty(&eq->list) ||
				!dlistfd_empty(&eq->err_list)) {
				*context++ = eq->eq.fid.context;
				ret_count++;
			}
			ofi_mutex_unlock(&eq->lock);
			break;

		default:
			break;
		}
	}

	return ret_count;
}

static int sock_poll_close(fid_t fid)
{
	struct sock_poll *poll;
	struct sock_fid_list *list_item;
	struct dlist_entry *p, *head;

	poll = container_of(fid, struct sock_poll, poll_fid.fid);

	head = &poll->fid_list;
	while (!dlist_empty(head)) {
		p = head->next;
		list_item = container_of(p, struct sock_fid_list, entry);
		sock_poll_del(&poll->poll_fid, list_item->fid, 0);
	}

	ofi_atomic_dec32(&poll->domain->ref);
	free(poll);
	return 0;
}

static struct fi_ops sock_poll_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = sock_poll_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

static struct fi_ops_poll sock_poll_ops = {
	.size = sizeof(struct fi_ops_poll),
	.poll = sock_poll_poll,
	.poll_add = sock_poll_add,
	.poll_del = sock_poll_del,
};

static int sock_poll_verify_attr(struct fi_poll_attr *attr)
{
	if (attr->flags)
		return -FI_ENODATA;
	return 0;
}

int sock_poll_open(struct fid_domain *domain, struct fi_poll_attr *attr,
		   struct fid_poll **pollset)
{
	struct sock_domain *dom;
	struct sock_poll *poll;

	if (attr && sock_poll_verify_attr(attr))
		return -FI_EINVAL;

	dom = container_of(domain, struct sock_domain, dom_fid);
	poll = calloc(1, sizeof(*poll));
	if (!poll)
		return -FI_ENOMEM;

	dlist_init(&poll->fid_list);
	poll->poll_fid.fid.fclass = FI_CLASS_POLL;
	poll->poll_fid.fid.context = 0;
	poll->poll_fid.fid.ops = &sock_poll_fi_ops;
	poll->poll_fid.ops = &sock_poll_ops;
	poll->domain = dom;
	ofi_atomic_inc32(&dom->ref);

	*pollset = &poll->poll_fid;
	return 0;
}
