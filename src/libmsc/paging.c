/*
 * (C) 2019 by sysmocom - s.m.f.c. GmbH <info@sysmocom.de>
 * All Rights Reserved
 *
 * SPDX-License-Identifier: AGPL-3.0+
 *
 * Author: Neels Hofmeyr
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <osmocom/msc/gsm_data.h>
#include <osmocom/msc/paging.h>
#include <osmocom/msc/vlr.h>
#include <osmocom/msc/ran_peer.h>
#include <osmocom/msc/sgs_iface.h>
#include <osmocom/msc/signal.h>
#include <osmocom/msc/msc_a.h>
#include <osmocom/msc/transaction.h>

#define LOG_PAGING(vsub, paging_request, level, fmt, args ...) \
	LOGP(DPAG, level, "Paging: %s%s%s: " fmt, \
	     vlr_subscr_name(vsub), paging_request ? " for " : "", paging_request ? (paging_request)->label : "", ## args)

#define VSUB_USE_PAGING "Paging"

const struct value_string paging_cause_names[] = {
	{ PAGING_CAUSE_CALL_CONVERSATIONAL, "CALL_CONVERSATIONAL" },
	{ PAGING_CAUSE_CALL_STREAMING, "CALL_STREAMING" },
	{ PAGING_CAUSE_CALL_INTERACTIVE, "CALL_INTERACTIVE" },
	{ PAGING_CAUSE_CALL_BACKGROUND, "CALL_BACKGROUND" },
	{ PAGING_CAUSE_SIGNALLING_LOW_PRIO, "SIGNALLING_LOW_PRIO" },
	{ PAGING_CAUSE_SIGNALLING_HIGH_PRIO, "SIGNALLING_HIGH_PRIO" },
	{ PAGING_CAUSE_UNSPECIFIED, "UNSPECIFIED" },
	{}
};

static void paging_response_timer_cb(void *data)
{
	struct vlr_subscr *vsub = data;

	if (vsub->cs.attached_via_ran == OSMO_RAT_EUTRAN_SGS)
		sgs_iface_tx_serv_abrt(vsub);

	paging_expired(vsub);
}

/* Execute a paging on the currently active RAN. Returns the number of
 * delivered paging requests or -EINVAL in case of failure. */
static int msc_paging_request(struct paging_request *pr, struct vlr_subscr *vsub)
{
	struct gsm_network *net = vsub->vlr->user_ctx;

	/* The subscriber was last seen in subscr->lac. Find out which
	 * BSCs/RNCs are responsible and send them a paging request via open
	 * SCCP connections (if any). */
	switch (vsub->cs.attached_via_ran) {
	case OSMO_RAT_GERAN_A:
		return ran_peers_down_paging(net->a.sri, CELL_IDENT_LAC, vsub, pr->cause);
	case OSMO_RAT_UTRAN_IU:
		return ran_peers_down_paging(net->iu.sri, CELL_IDENT_LAC, vsub, pr->cause);
	case OSMO_RAT_EUTRAN_SGS:
		return sgs_iface_tx_paging(vsub, sgs_serv_ind_from_paging_cause(pr->cause));
	default:
		LOG_PAGING(vsub, pr, LOGL_ERROR, "Cannot page, subscriber not attached\n");
		return -EINVAL;
	}
}

struct paging_request *paging_request_start(struct vlr_subscr *vsub, enum paging_cause cause,
					    paging_cb_t paging_cb, struct gsm_trans *trans,
					    const char *label)
{
	int rc;
	struct paging_request *pr;
	struct gsm_network *net = vsub->vlr->user_ctx;

	pr = talloc(vsub, struct paging_request);
	OSMO_ASSERT(pr);
	*pr = (struct paging_request){
		.label = label,
		.cause = cause,
		.paging_cb = paging_cb,
		.trans = trans,
	};

	if (vsub->cs.is_paging) {
		LOG_PAGING(vsub, pr, LOGL_DEBUG, "Already paging, not starting another request\n");
	} else {
		LOG_PAGING(vsub, pr, LOGL_DEBUG, "Starting paging\n");

		rc = msc_paging_request(pr, vsub);
		if (rc <= 0) {
			LOG_PAGING(vsub, pr, LOGL_ERROR, "Starting paging failed (rc=%d)\n", rc);
			talloc_free(pr);
			return NULL;
		}

		/* reduced on the first paging callback */
		vlr_subscr_get(vsub, VSUB_USE_PAGING);
		vsub->cs.is_paging = true;
		osmo_timer_setup(&vsub->cs.paging_response_timer, paging_response_timer_cb, vsub);
		osmo_timer_schedule(&vsub->cs.paging_response_timer, net->paging_response_timer, 0);
	}

	llist_add_tail(&pr->entry, &vsub->cs.requests);

	return pr;
}

void paging_request_remove(struct paging_request *pr)
{
	struct gsm_trans *trans = pr->trans;
	struct vlr_subscr *vsub = trans ? trans->vsub : NULL;
	LOG_PAGING(vsub, pr, LOGL_DEBUG, "Removing Paging Request\n");

	if (pr->trans && pr->trans->paging_request == pr)
		pr->trans->paging_request = NULL;

	llist_del(&pr->entry);
	talloc_free(pr);
}

static void paging_concludes(struct vlr_subscr *vsub, struct msc_a *msc_a)
{
	struct paging_request *pr, *pr_next;
	struct paging_signal_data sig_data;

	osmo_timer_del(&vsub->cs.paging_response_timer);

	llist_for_each_entry_safe(pr, pr_next, &vsub->cs.requests, entry) {
		struct gsm_trans *trans = pr->trans;
		paging_cb_t paging_cb = pr->paging_cb;

		LOG_PAGING(vsub, pr, LOGL_DEBUG, "Paging Response action (%s)%s\n",
			   msc_a ? "success" : "expired",
			   paging_cb ? "" : " (no action defined)");

		/* Remove the paging request before the paging_cb could deallocate e.g. the trans */
		paging_request_remove(pr);
		pr = NULL;

		if (paging_cb)
			paging_cb(msc_a, trans);
	}

	/* Inform parts of the system we don't know */
	sig_data = (struct paging_signal_data){
		.vsub = vsub,
		.msc_a = msc_a,
	};
	osmo_signal_dispatch(SS_PAGING, msc_a ? S_PAGING_SUCCEEDED : S_PAGING_EXPIRED, &sig_data);

	/* balanced with the moment we start paging */
	if (vsub->cs.is_paging) {
		vsub->cs.is_paging = false;
		vlr_subscr_put(vsub, VSUB_USE_PAGING);
	}

	/* Handling of the paging requests has usually added transactions, which keep the msc_a connection active. If
	 * there are none, then this probably marks release of the connection. */
	if (msc_a)
		msc_a_put(msc_a, MSC_A_USE_PAGING_RESPONSE);
}

void paging_response(struct msc_a *msc_a)
{
	paging_concludes(msc_a_vsub(msc_a), msc_a);
}

void paging_expired(struct vlr_subscr *vsub)
{
	paging_concludes(vsub, NULL);
}
