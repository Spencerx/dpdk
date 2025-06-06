/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2021 Marvell.
 */

#include "roc_api.h"
#include "roc_priv.h"

#include <unistd.h>

#define NIX_INL_DEV_CPT_LF_QSZ 8192

#define NIX_AURA_DROP_PC_DFLT 40

/* Default Rx Config for Inline NIX LF */
#define NIX_INL_LF_RX_CFG                                                      \
	(ROC_NIX_LF_RX_CFG_DROP_RE | ROC_NIX_LF_RX_CFG_L2_LEN_ERR |            \
	 ROC_NIX_LF_RX_CFG_IP6_UDP_OPT | ROC_NIX_LF_RX_CFG_DIS_APAD |          \
	 ROC_NIX_LF_RX_CFG_LEN_IL3 | ROC_NIX_LF_RX_CFG_LEN_OL3)

#define INL_NIX_RX_STATS(val) plt_read64(inl_dev->nix_base + NIX_LF_RX_STATX(val))

extern uint32_t soft_exp_consumer_cnt;
static bool soft_exp_poll_thread_exit = true;

uint16_t
nix_inl_dev_pffunc_get(void)
{
	struct idev_cfg *idev = idev_get_cfg();
	struct nix_inl_dev *inl_dev;

	if (idev != NULL) {
		inl_dev = idev->nix_inl_dev;
		if (inl_dev)
			return inl_dev->dev.pf_func;
	}
	return 0;
}

static void
nix_inl_selftest_work_cb(uint64_t *gw, void *args, uint32_t soft_exp_event)
{
	uintptr_t work = gw[1];

	(void)soft_exp_event;
	*((uintptr_t *)args + (gw[0] & 0x1)) = work;

	plt_atomic_thread_fence(__ATOMIC_ACQ_REL);
}

static int
nix_inl_selftest(void)
{
	struct idev_cfg *idev = idev_get_cfg();
	roc_nix_inl_sso_work_cb_t save_cb;
	static uintptr_t work_arr[2];
	struct nix_inl_dev *inl_dev;
	void *save_cb_args;
	uint64_t add_work0;
	int rc = 0;

	if (idev == NULL)
		return -ENOTSUP;

	inl_dev = idev->nix_inl_dev;
	if (inl_dev == NULL)
		return -ENOTSUP;

	plt_info("Performing nix inl self test");

	/* Save and update cb to test cb */
	save_cb = inl_dev->work_cb;
	save_cb_args = inl_dev->cb_args;
	inl_dev->work_cb = nix_inl_selftest_work_cb;
	inl_dev->cb_args = work_arr;

	plt_atomic_thread_fence(__ATOMIC_ACQ_REL);

#define WORK_MAGIC1 0x335577ff0
#define WORK_MAGIC2 0xdeadbeef0

	/* Add work */
	add_work0 = ((uint64_t)(SSO_TT_ORDERED) << 32) | 0x0;
	roc_store_pair(add_work0, WORK_MAGIC1, inl_dev->sso_base);
	add_work0 = ((uint64_t)(SSO_TT_ORDERED) << 32) | 0x1;
	roc_store_pair(add_work0, WORK_MAGIC2, inl_dev->sso_base);

	plt_delay_ms(10000);

	/* Check if we got expected work */
	if (work_arr[0] != WORK_MAGIC1 || work_arr[1] != WORK_MAGIC2) {
		plt_err("Failed to get expected work, [0]=%p [1]=%p",
			(void *)work_arr[0], (void *)work_arr[1]);
		rc = -EFAULT;
		goto exit;
	}

	plt_info("Work, [0]=%p [1]=%p", (void *)work_arr[0],
		 (void *)work_arr[1]);

exit:
	/* Restore state */
	inl_dev->work_cb = save_cb;
	inl_dev->cb_args = save_cb_args;
	return rc;
}

int
nix_inl_setup_dflt_ipsec_profile(struct dev *dev, uint16_t *prof_id)
{
	struct mbox *mbox = mbox_get(dev->mbox);
	struct nix_rx_inl_profile_cfg_req *req;
	struct nix_rx_inl_profile_cfg_rsp *rsp;
	int rc;

	req = mbox_alloc_msg_nix_rx_inl_profile_cfg(mbox);
	if (req == NULL) {
		mbox_put(mbox);
		return -ENOSPC;
	}

	/* Prepare NIXX_AF_RX_DEF_INLINE to match ESP, IPv4/IPv6 and extract l2_len */
	req->def_cfg = NIX_INL_DFLT_IPSEC_DEF_CFG;

	/* Extract 32 bit from bit pos 0 */
	req->extract_cfg = NIX_INL_DFLT_IPSEC_EXTRACT_CFG;

	/* Gen config */
	req->gen_cfg = NIX_INL_DFLT_IPSEC_GEN_CFG;

	rc = mbox_process_msg(mbox, (void **)&rsp);
	if (rc)
		goto exit;

	*prof_id = rsp->profile_id;
exit:
	mbox_put(mbox);
	return rc;
}

int
nix_inl_setup_reass_profile(struct dev *dev, uint8_t *prof_id)
{
	struct mbox *mbox = mbox_get(dev->mbox);
	struct nix_rx_inl_profile_cfg_req *req;
	struct nix_rx_inl_profile_cfg_rsp *rsp;
	int rc;

	req = mbox_alloc_msg_nix_rx_inl_profile_cfg(mbox);
	if (req == NULL) {
		mbox_put(mbox);
		return -ENOSPC;
	}

	req->def_cfg = NIX_INL_REASS_DEF_CFG;
	req->gen_cfg = NIX_INL_REASS_GEN_CFG;

	rc = mbox_process_msg(mbox, (void **)&rsp);
	if (rc)
		goto exit;

	*prof_id = rsp->profile_id;
exit:
	mbox_put(mbox);
	return rc;
}

static int
nix_inl_inb_queue_setup(struct nix_inl_dev *inl_dev, uint8_t slot_id)
{
	struct roc_cpt_lf *lf = &inl_dev->cpt_lf[slot_id];
	struct nix_rx_inline_qcfg_req *nix_req;
	struct cpt_rx_inline_qcfg_req *cpt_req;
	struct cpt_rx_inline_qalloc_rsp *rsp;
	struct msg_req *req;
	struct mbox *mbox;
	uint16_t bpid, qid;
	int rc;

	/* Allocate BPID if not allocated */
	if (inl_dev->nix_inb_q_bpid < 0) {
		rc = nix_bpids_alloc(&inl_dev->dev, ROC_NIX_INTF_TYPE_CPT_NIX, 1, &bpid);
		if (rc <= 0)
			plt_warn("Failed to allocate BPID for inbound queue, rc=%d", rc);
		else
			inl_dev->nix_inb_q_bpid = bpid;
	}

	mbox = mbox_get((&inl_dev->dev)->mbox);
	/* Allocate inline queue */
	rc = -ENOSPC;
	req = mbox_alloc_msg_cpt_rx_inline_qalloc(mbox);
	if (!req)
		goto exit;

	rc = mbox_process_msg(mbox, (void **)&rsp);
	if (rc) {
		plt_err("Failed to alloc inline q, rc=%d", rc);
		goto exit;
	}

	qid = rsp->rx_queue_id;

	/* Configure CPT LF dedicated for inline processing */
	cpt_req = mbox_alloc_msg_cpt_rx_inl_queue_cfg(mbox);
	if (!cpt_req)
		goto cpt_cfg_fail;

	cpt_req->enable = 1;
	cpt_req->slot = slot_id;
	cpt_req->rx_queue_id = qid;
	cpt_req->eng_grpmsk = inl_dev->eng_grpmask;
	rc = mbox_process(mbox);
	if (rc) {
		plt_err("Failed to configure CPT LF for inline processing, rc=%d", rc);
		goto cpt_cfg_fail;
	}

	/* Setup NIX AF to CPT LF mapping for inline queue */
	rc = -ENOSPC;
	nix_req = mbox_alloc_msg_nix_rx_inl_queue_cfg(mbox);
	if (!nix_req)
		goto nix_cfg_fail;
	nix_req->cpt_pf_func = inl_dev->dev.pf_func;
	nix_req->cpt_slot = slot_id;
	nix_req->cpt_credit = lf->nb_desc;
	nix_req->rx_queue_id = qid;
	nix_req->enable = 1;
	if (inl_dev->nix_inb_q_bpid >= 0) {
		nix_req->bpid = inl_dev->nix_inb_q_bpid;
		nix_req->credit_th = nix_req->cpt_credit - 1;
	}

	rc = mbox_process(mbox);
	if (rc) {
		plt_err("Failed to enable inbound queue on slot %u, rc=%d", slot_id, rc);
		goto nix_cfg_fail;
	}

	inl_dev->nix_inb_qids[slot_id] = qid;
	mbox_put(mbox);
	return 0;
nix_cfg_fail:
	cpt_req = mbox_alloc_msg_cpt_rx_inl_queue_cfg(mbox);
	if (!cpt_req) {
		rc |= -ENOSPC;
	} else {
		nix_req->enable = false;
		rc |= mbox_process(mbox);
	}
cpt_cfg_fail:
	/* TODO: Free QID */
exit:
	mbox_put(mbox);
	return rc;
}

static int
nix_inl_inb_queue_release(struct nix_inl_dev *inl_dev, uint8_t slot_id)
{
	struct nix_rx_inline_qcfg_req *nix_req;
	struct cpt_rx_inline_qcfg_req *cpt_req;
	struct mbox *mbox;
	int rc, ret = 0;
	int qid;

	qid = inl_dev->nix_inb_qids[slot_id];
	if (qid < 0)
		return 0;

	mbox = mbox_get((&inl_dev->dev)->mbox);

	/* Cleanup NIX AF to CPT LF mapping for inline queue */
	rc = -ENOSPC;
	nix_req = mbox_alloc_msg_nix_rx_inl_queue_cfg(mbox);
	if (!nix_req) {
		ret |= rc;
		goto exit;
	}
	nix_req->rx_queue_id = qid;
	nix_req->enable = 0;

	rc = mbox_process(mbox);
	if (rc)
		plt_err("Failed to cleanup inbound queue %u, rc=%d", qid, rc);
	ret |= rc;

	/* Configure CPT LF dedicated for inline processing */
	cpt_req = mbox_alloc_msg_cpt_rx_inl_queue_cfg(mbox);
	if (!cpt_req) {
		rc = -ENOSPC;
		goto exit;
	}

	cpt_req->enable = 0;
	cpt_req->rx_queue_id = qid;
	cpt_req->slot = slot_id;

	rc = mbox_process(mbox);
	if (rc)
		plt_err("Failed to disable CPT LF for inline processing, rc=%d", rc);
	ret |= rc;

	/* TODO: Free inline queue */

	inl_dev->nix_inb_qids[slot_id] = -1;
	mbox_put(mbox);
	return 0;
exit:
	mbox_put(mbox);
	return ret;
}

static int
nix_inl_cpt_ctx_cache_sync(struct nix_inl_dev *inl_dev)
{
	struct mbox *mbox = mbox_get((&inl_dev->dev)->mbox);
	struct msg_req *req;
	int rc;

	req = mbox_alloc_msg_cpt_ctx_cache_sync(mbox);
	if (req == NULL) {
		rc = -ENOSPC;
		goto exit;
	}

	rc = mbox_process(mbox);
exit:
	mbox_put(mbox);
	return rc;
}

static int
nix_inl_nix_ipsec_cfg(struct nix_inl_dev *inl_dev, bool ena)
{
	struct mbox *mbox = mbox_get((&inl_dev->dev)->mbox);
	uint64_t max_sa, sa_w, sa_pow2_sz, lenm1_max;
	uint8_t profile_id = inl_dev->ipsec_prof_id;
	int rc;

	max_sa = inl_dev->inb_spi_mask + 1;
	sa_w = plt_log2_u32(max_sa);
	sa_pow2_sz = plt_log2_u32(inl_dev->inb_sa_sz[profile_id]);
	/* CN9K SA size is different */
	if (roc_model_is_cn9k())
		lenm1_max = NIX_CN9K_MAX_HW_FRS - 1;
	else
		lenm1_max = NIX_RPM_MAX_HW_FRS - 1;

	if (!roc_model_is_cn20k()) {
		struct nix_inline_ipsec_lf_cfg *lf_cfg;

		lf_cfg = mbox_alloc_msg_nix_inline_ipsec_lf_cfg(mbox);
		if (lf_cfg == NULL) {
			rc = -ENOSPC;
			goto exit;
		}

		if (ena) {
			lf_cfg->enable = 1;
			lf_cfg->sa_base_addr = (uintptr_t)inl_dev->inb_sa_base[profile_id];
			lf_cfg->ipsec_cfg1.sa_idx_w = sa_w;
			lf_cfg->ipsec_cfg0.lenm1_max = lenm1_max;
			lf_cfg->ipsec_cfg1.sa_idx_max = max_sa - 1;
			lf_cfg->ipsec_cfg0.sa_pow2_size = sa_pow2_sz;

			lf_cfg->ipsec_cfg0.tag_const = 0;
			lf_cfg->ipsec_cfg0.tt = SSO_TT_ORDERED;
			if (inl_dev->res_addr_offset) {
				lf_cfg->ipsec_cfg0_ext.res_addr_offset_valid = 1;
				lf_cfg->ipsec_cfg0_ext.res_addr_offset =
					(inl_dev->res_addr_offset & 0x80) |
					abs(inl_dev->res_addr_offset);
			}
		} else {
			lf_cfg->enable = 0;
		}
	} else {
		struct nix_rx_inl_lf_cfg_req *lf_cfg;
		uint64_t res_addr_offset;
		uint64_t def_cptq;

		lf_cfg = mbox_alloc_msg_nix_rx_inl_lf_cfg(mbox);
		if (lf_cfg == NULL) {
			rc = -ENOSPC;
			goto exit;
		}

		if (!inl_dev->nb_inb_cptlfs)
			def_cptq = 0;
		else
			def_cptq = inl_dev->nix_inb_qids[inl_dev->inb_cpt_lf_id];

		res_addr_offset = (uint64_t)(inl_dev->res_addr_offset & 0xFF) << 48;
		if (res_addr_offset)
			res_addr_offset |= (1UL << 56);

		lf_cfg->profile_id = inl_dev->ipsec_prof_id;
		if (ena) {
			lf_cfg->enable = 1;
			lf_cfg->rx_inline_sa_base = (uintptr_t)inl_dev->inb_sa_base[profile_id];
			lf_cfg->rx_inline_cfg0 =
				((def_cptq << 57) | res_addr_offset |
				 ((uint64_t)SSO_TT_ORDERED << 44) | (sa_pow2_sz << 16) | lenm1_max);
			lf_cfg->rx_inline_cfg1 = (max_sa - 1) | (sa_w << 32);
		} else {
			lf_cfg->enable = 0;
		}
	}

	rc = mbox_process(mbox);
exit:
	mbox_put(mbox);
	return rc;
}

static int
nix_inl_cpt_setup(struct nix_inl_dev *inl_dev, bool inl_dev_sso)
{
	struct roc_nix_inl_dev_q *q_info;
	struct dev *dev = &inl_dev->dev;
	bool ctx_ilen_valid = false;
	struct roc_cpt_lf *lf;
	uint8_t eng_grpmask;
	uint8_t ctx_ilen = 0;
	int rc, i;

	if (!inl_dev->attach_cptlf)
		return 0;

	eng_grpmask = inl_dev->eng_grpmask;

	if (roc_errata_cpt_has_ctx_fetch_issue()) {
		ctx_ilen = (ROC_NIX_INL_OT_IPSEC_INB_HW_SZ / 128) - 1;
		ctx_ilen_valid = true;
	}

	/* Alloc CPT LF */
	rc = cpt_lfs_alloc(dev, eng_grpmask, RVU_BLOCK_ADDR_CPT0, inl_dev_sso, ctx_ilen_valid,
			   ctx_ilen, inl_dev->rx_inj_ena, 1);
	if (rc) {
		plt_err("Failed to alloc CPT LF resources, rc=%d", rc);
		return rc;
	}

	for (i = 0; i < inl_dev->nb_cptlf; i++) {
		/* Setup CPT LF for submitting control opcode */
		lf = &inl_dev->cpt_lf[i];
		lf->lf_id = i;
		lf->nb_desc = NIX_INL_DEV_CPT_LF_QSZ; /* Set to default */
		lf->dev = &inl_dev->dev;
		lf->msixoff = inl_dev->cpt_msixoff[i];
		lf->pci_dev = inl_dev->pci_dev;

		rc = cpt_lf_init(lf);
		if (rc) {
			plt_err("Failed to initialize CPT LF, rc=%d", rc);
			goto lf_free;
		}

		q_info = &inl_dev->q_info[i];
		q_info->nb_desc = lf->nb_desc;
		q_info->fc_addr = (uint64_t __plt_atomic *)lf->fc_addr;
		q_info->io_addr = lf->io_addr;
		q_info->lmt_base = lf->lmt_base;
		q_info->rbase = lf->rbase;

		roc_cpt_iq_enable(lf);
	}

	/* Configure NIX inline inbound queue resource */
	for (i = 0; i < inl_dev->nb_inb_cptlfs; i++) {
		rc = nix_inl_inb_queue_setup(inl_dev, inl_dev->inb_cpt_lf_id + i);
		if (rc)
			goto lf_fini;
	}

	return 0;
lf_fini:
	for (i = 0; i < inl_dev->nb_cptlf; i++)
		cpt_lf_fini(&inl_dev->cpt_lf[i]);
lf_free:
	rc |= cpt_lfs_free(dev);
	return rc;
}

static int
nix_inl_cpt_release(struct nix_inl_dev *inl_dev)
{
	struct dev *dev = &inl_dev->dev;
	int rc = 0, i, ret;

	if (!inl_dev->attach_cptlf)
		return 0;

	/* Release NIX inline inbound queue resource */
	for (i = 0; i < inl_dev->nb_inb_cptlfs; i++)
		rc |= nix_inl_inb_queue_release(inl_dev, inl_dev->inb_cpt_lf_id + i);
	ret = rc;

	/* TODO: Wait for CPT/RXC queue to drain */

	/* Cleanup CPT LF queue */
	for (i = 0; i < inl_dev->nb_cptlf; i++)
		cpt_lf_fini(&inl_dev->cpt_lf[i]);

	/* Free LF resources */
	rc = cpt_lfs_free(dev);
	if (!rc) {
		for (i = 0; i < inl_dev->nb_cptlf; i++)
			inl_dev->cpt_lf[i].dev = NULL;
	} else
		plt_err("Failed to free CPT LF resources, rc=%d", rc);
	ret |= rc;
	return ret;
}

static int
nix_inl_sso_setup(struct nix_inl_dev *inl_dev)
{
	struct sso_lf_alloc_rsp *sso_rsp;
	struct dev *dev = &inl_dev->dev;
	uint16_t hwgrp[1] = {0};
	int rc;

	/* Alloc SSOW LF */
	rc = sso_lf_alloc(dev, SSO_LF_TYPE_HWS, 1, NULL);
	if (rc) {
		plt_err("Failed to alloc SSO HWS, rc=%d", rc);
		return rc;
	}

	/* Alloc HWGRP LF */
	rc = sso_lf_alloc(dev, SSO_LF_TYPE_HWGRP, 1, (void **)&sso_rsp);
	if (rc) {
		plt_err("Failed to alloc SSO HWGRP, rc=%d", rc);
		goto free_ssow;
	}

	inl_dev->xaq_buf_size = sso_rsp->xaq_buf_size;
	inl_dev->xae_waes = sso_rsp->xaq_wq_entries;
	inl_dev->iue = sso_rsp->in_unit_entries;

	inl_dev->nb_xae = inl_dev->iue;
	rc = sso_hwgrp_init_xaq_aura(dev, &inl_dev->xaq, inl_dev->nb_xae,
				     inl_dev->xae_waes, inl_dev->xaq_buf_size,
				     1);
	if (rc) {
		plt_err("Failed to alloc SSO XAQ aura, rc=%d", rc);
		goto free_sso;
	}

	/* Setup xaq for hwgrps */
	rc = sso_hwgrp_alloc_xaq(dev, roc_npa_aura_handle_to_aura(inl_dev->xaq.aura_handle), 1);
	if (rc) {
		plt_err("Failed to setup hwgrp xaq aura, rc=%d", rc);
		goto destroy_pool;
	}

	/* Register SSO, SSOW error and work irq's */
	rc = nix_inl_sso_register_irqs(inl_dev);
	if (rc) {
		plt_err("Failed to register sso irq's, rc=%d", rc);
		goto release_xaq;
	}

	/* Setup hwgrp->hws link */
	sso_hws_link_modify(0, inl_dev->ssow_base, NULL, hwgrp, 1, 0, true);

	/* Enable HWGRP */
	plt_write64(0x1, inl_dev->sso_base + SSO_LF_GGRP_QCTL);

	return 0;

release_xaq:
	sso_hwgrp_release_xaq(&inl_dev->dev, 1);
destroy_pool:
	sso_hwgrp_free_xaq_aura(dev, &inl_dev->xaq, 0);
free_sso:
	sso_lf_free(dev, SSO_LF_TYPE_HWGRP, 1);
free_ssow:
	sso_lf_free(dev, SSO_LF_TYPE_HWS, 1);
	return rc;
}

static int
nix_inl_sso_release(struct nix_inl_dev *inl_dev)
{
	uint16_t hwgrp[1] = {0};

	/* Disable HWGRP */
	plt_write64(0, inl_dev->sso_base + SSO_LF_GGRP_QCTL);

	/* Unregister SSO/SSOW IRQ's */
	nix_inl_sso_unregister_irqs(inl_dev);

	/* Unlink hws */
	sso_hws_link_modify(0, inl_dev->ssow_base, NULL, hwgrp, 1, 0, false);

	/* Release XAQ aura */
	sso_hwgrp_release_xaq(&inl_dev->dev, 1);

	/* Free SSO, SSOW LF's */
	sso_lf_free(&inl_dev->dev, SSO_LF_TYPE_HWS, 1);
	sso_lf_free(&inl_dev->dev, SSO_LF_TYPE_HWGRP, 1);

	/* Free the XAQ aura */
	sso_hwgrp_free_xaq_aura(&inl_dev->dev, &inl_dev->xaq, 0);

	return 0;
}

static int
nix_inl_nix_profile_config(struct nix_inl_dev *inl_dev, uint8_t profile_id)
{
	struct mbox *mbox = mbox_get((&inl_dev->dev)->mbox);
	uint64_t max_sa, sa_w, sa_pow2_sz, lenm1_max;
	struct nix_rx_inl_lf_cfg_req *lf_cfg;
	uint64_t res_addr_offset;
	uint64_t def_cptq;
	size_t inb_sa_sz;
	void *sa;
	int rc;

	/* Alloc contiguous memory for Inbound SA's */
	inb_sa_sz = ROC_NIX_INL_OW_IPSEC_INB_SA_SZ;
	max_sa = inl_dev->inb_sa_max[profile_id];
	inl_dev->inb_sa_sz[profile_id] = inb_sa_sz;
	inl_dev->inb_sa_base[profile_id] =
		plt_zmalloc(inb_sa_sz * max_sa, ROC_NIX_INL_SA_BASE_ALIGN);
	if (!inl_dev->inb_sa_base[profile_id]) {
		plt_err("Failed to allocate memory for Inbound SA for profile %u", profile_id);
		rc = -ENOMEM;
		goto exit;
	}

	sa = ((uint8_t *)inl_dev->inb_sa_base[profile_id]);
	roc_ow_reass_inb_sa_init(sa);
	lf_cfg = mbox_alloc_msg_nix_rx_inl_lf_cfg(mbox);
	if (lf_cfg == NULL) {
		rc = -ENOSPC;
		goto free_mem;
	}

	lenm1_max = NIX_RPM_MAX_HW_FRS - 1;
	sa_w = plt_log2_u32(max_sa);
	sa_pow2_sz = plt_log2_u32(inb_sa_sz);

	if (!inl_dev->nb_inb_cptlfs)
		def_cptq = 0;
	else
		def_cptq = inl_dev->nix_inb_qids[inl_dev->inb_cpt_lf_id];

	res_addr_offset = (uint64_t)(inl_dev->res_addr_offset & 0xFF) << 48;
	if (res_addr_offset)
		res_addr_offset |= (1UL << 56);

	lf_cfg->enable = 1;
	lf_cfg->profile_id = profile_id;
	lf_cfg->rx_inline_sa_base = (uintptr_t)inl_dev->inb_sa_base[profile_id];
	lf_cfg->rx_inline_cfg0 =
		((def_cptq << 57) | res_addr_offset | ((uint64_t)SSO_TT_ORDERED << 44) |
		 (sa_pow2_sz << 16) | lenm1_max);
	lf_cfg->rx_inline_cfg1 = (max_sa - 1) | (sa_w << 32);

	rc = mbox_process(mbox);
	if (rc) {
		plt_err("Failed to setup NIX Inbound SA conf of profile=%u, rc=%d", profile_id, rc);
		goto free_mem;
	}

	mbox_put(mbox);
	return 0;

free_mem:
	plt_free(inl_dev->inb_sa_base[profile_id]);
	inl_dev->inb_sa_base[profile_id] = NULL;
exit:
	mbox_put(mbox);
	return rc;
}

static int
nix_inl_nix_profile_release(struct nix_inl_dev *inl_dev, uint8_t profile_id)
{
	struct mbox *mbox = mbox_get((&inl_dev->dev)->mbox);
	struct nix_rx_inl_lf_cfg_req *lf_cfg;
	int rc;

	lf_cfg = mbox_alloc_msg_nix_rx_inl_lf_cfg(mbox);
	if (!lf_cfg) {
		rc = -ENOSPC;
		goto exit;
	}

	lf_cfg->enable = 0;
	lf_cfg->profile_id = profile_id;
	rc = mbox_process(mbox);
	if (rc) {
		plt_err("Failed to cleanup NIX Inbound profile=%u SA conf, rc=%d", profile_id, rc);
		goto exit;
	}

	plt_free(inl_dev->inb_sa_base[profile_id]);
	inl_dev->inb_sa_base[profile_id] = NULL;
exit:
	mbox_put(mbox);
	return rc;
}

static int
nix_inl_nix_reass_setup(struct nix_inl_dev *inl_dev)
{
	int rc;

	if (!inl_dev->reass_ena)
		return 0;

	rc = nix_inl_setup_reass_profile(&inl_dev->dev, &inl_dev->reass_prof_id);
	if (rc) {
		plt_err("Failed to setup reassembly profile, rc=%d", rc);
		return rc;
	}

	inl_dev->inb_sa_max[inl_dev->reass_prof_id] = 1;
	return nix_inl_nix_profile_config(inl_dev, inl_dev->reass_prof_id);
}

static int
nix_inl_nix_reass_cleanup(struct nix_inl_dev *inl_dev)
{
	int rc;

	if (!inl_dev->reass_ena)
		return 0;

	rc = nix_inl_nix_profile_release(inl_dev, inl_dev->reass_prof_id);
	if (rc) {
		plt_err("Failed to cleanup reassembly profile, rc=%d", rc);
		return rc;
	}

	return rc;
}

static int
nix_inl_nix_setup(struct nix_inl_dev *inl_dev)
{
	uint32_t ipsec_in_min_spi = inl_dev->ipsec_in_min_spi;
	uint32_t ipsec_in_max_spi = inl_dev->ipsec_in_max_spi;
	struct dev *dev = &inl_dev->dev;
	struct mbox *mbox = dev->mbox;
	struct nix_lf_alloc_rsp *rsp;
	struct nix_lf_alloc_req *req;
	struct nix_hw_info *hw_info;
	struct roc_nix_rq *rqs;
	uint64_t max_sa, i;
	uint8_t profile_id;
	size_t inb_sa_sz;
	int rc = -ENOSPC;
	void *sa;

	/* Setup default IPsec profile */
	if (roc_feature_nix_has_inl_profile()) {
		rc = nix_inl_setup_dflt_ipsec_profile(&inl_dev->dev, &inl_dev->ipsec_prof_id);
		if (rc)
			return rc;
	}

	profile_id = inl_dev->ipsec_prof_id;
	max_sa = plt_align32pow2(ipsec_in_max_spi - ipsec_in_min_spi + 1);

	/* Alloc NIX LF needed for single RQ */
	req = mbox_alloc_msg_nix_lf_alloc(mbox_get(mbox));
	if (req == NULL) {
		mbox_put(mbox);
		return rc;
	}
	/* We will have per-port RQ if it is not with channel masking */
	req->rq_cnt = inl_dev->nb_rqs;
	req->sq_cnt = 1;
	req->cq_cnt = 1;
	/* XQESZ is W16 */
	req->xqe_sz = NIX_XQESZ_W16;
	/* RSS size does not matter as this RQ is only for UCAST_IPSEC action */
	req->rss_sz = ROC_NIX_RSS_RETA_SZ_64;
	req->rss_grps = ROC_NIX_RSS_GRPS;
	req->npa_func = idev_npa_pffunc_get();
	req->sso_func = dev->pf_func;
	req->rx_cfg = NIX_INL_LF_RX_CFG;
	req->flags = NIX_LF_RSS_TAG_LSB_AS_ADDER;

	if (roc_errata_nix_has_no_drop_re())
		req->rx_cfg &= ~ROC_NIX_LF_RX_CFG_DROP_RE;

	rc = mbox_process_msg(mbox, (void *)&rsp);
	if (rc) {
		plt_err("Failed to alloc lf, rc=%d", rc);
		mbox_put(mbox);
		return rc;
	}

	inl_dev->lf_tx_stats = rsp->lf_tx_stats;
	inl_dev->lf_rx_stats = rsp->lf_rx_stats;
	inl_dev->qints = rsp->qints;
	inl_dev->cints = rsp->cints;
	mbox_put(mbox);

	/* Get VWQE info if supported */
	if (!roc_model_is_cn9k()) {
		mbox_alloc_msg_nix_get_hw_info(mbox_get(mbox));
		rc = mbox_process_msg(mbox, (void *)&hw_info);
		if (rc) {
			plt_err("Failed to get HW info, rc=%d", rc);
			mbox_put(mbox);
			goto lf_free;
		}
		inl_dev->vwqe_interval = hw_info->vwqe_delay;
		mbox_put(mbox);
	}

	/* Register nix interrupts */
	rc = nix_inl_nix_register_irqs(inl_dev);
	if (rc) {
		plt_err("Failed to register nix irq's, rc=%d", rc);
		goto lf_free;
	}

	/* CN9K SA is different */
	if (inl_dev->custom_inb_sa)
		inb_sa_sz = ROC_NIX_INL_INB_CUSTOM_SA_SZ;
	else if (roc_model_is_cn9k())
		inb_sa_sz = ROC_NIX_INL_ON_IPSEC_INB_SA_SZ;
	else if (roc_model_is_cn10k())
		inb_sa_sz = ROC_NIX_INL_OT_IPSEC_INB_SA_SZ;
	else
		inb_sa_sz = ROC_NIX_INL_OW_IPSEC_INB_SA_SZ;

	/* Alloc contiguous memory for Inbound SA's */
	inl_dev->inb_sa_sz[profile_id] = inb_sa_sz;
	inl_dev->inb_sa_max[profile_id] = max_sa;
	inl_dev->inb_spi_mask = max_sa - 1;
	inl_dev->inb_sa_base[profile_id] =
		plt_zmalloc(inb_sa_sz * max_sa, ROC_NIX_INL_SA_BASE_ALIGN);
	if (!inl_dev->inb_sa_base[profile_id]) {
		plt_err("Failed to allocate memory for Inbound SA");
		rc = -ENOMEM;
		goto unregister_irqs;
	}

	if (!roc_model_is_cn9k()) {
		for (i = 0; i < max_sa; i++) {
			sa = ((uint8_t *)inl_dev->inb_sa_base[profile_id]) + (i * inb_sa_sz);
			if (roc_model_is_cn10k())
				roc_ot_ipsec_inb_sa_init(sa);
			else
				roc_ow_ipsec_inb_sa_init(sa);
		}
	}

	/* Allocate memory for RQ's */
	rqs = plt_zmalloc(sizeof(struct roc_nix_rq) * PLT_MAX_ETHPORTS, 0);
	if (!rqs) {
		plt_err("Failed to allocate memory for RQ's");
		goto free_mem;
	}
	inl_dev->rqs = rqs;

	return 0;
free_mem:
	plt_free(inl_dev->inb_sa_base[profile_id]);
	inl_dev->inb_sa_base[profile_id] = NULL;
unregister_irqs:
	nix_inl_nix_unregister_irqs(inl_dev);
lf_free:
	mbox_alloc_msg_nix_lf_free(mbox_get(mbox));
	rc |= mbox_process(mbox);
	mbox_put(mbox);
	return rc;
}

static int
nix_inl_nix_release(struct nix_inl_dev *inl_dev)
{
	struct dev *dev = &inl_dev->dev;
	struct mbox *mbox = dev->mbox;
	struct nix_lf_free_req *req;
	struct ndc_sync_op *ndc_req;
	int rc = -ENOSPC;

	/* Disable Inbound processing */
	rc = nix_inl_nix_ipsec_cfg(inl_dev, false);
	if (rc)
		plt_err("Failed to disable Inbound IPSec, rc=%d", rc);

	/* Cleanup reassembly profile */
	rc = nix_inl_nix_reass_cleanup(inl_dev);

	/* Sync NDC-NIX for LF */
	ndc_req = mbox_alloc_msg_ndc_sync_op(mbox_get(mbox));
	if (ndc_req == NULL) {
		mbox_put(mbox);
		return rc;
	}
	ndc_req->nix_lf_rx_sync = 1;
	rc = mbox_process(mbox);
	if (rc)
		plt_err("Error on NDC-NIX-RX LF sync, rc %d", rc);
	mbox_put(mbox);

	/* Unregister IRQs */
	nix_inl_nix_unregister_irqs(inl_dev);

	/* By default all associated mcam rules are deleted */
	req = mbox_alloc_msg_nix_lf_free(mbox_get(mbox));
	if (req == NULL) {
		mbox_put(mbox);
		return -ENOSPC;
	}

	rc = mbox_process(mbox);
	if (rc) {
		mbox_put(mbox);
		return rc;
	}
	mbox_put(mbox);

	plt_free(inl_dev->rqs);
	plt_free(inl_dev->inb_sa_base[inl_dev->ipsec_prof_id]);
	inl_dev->rqs = NULL;
	inl_dev->inb_sa_base[inl_dev->ipsec_prof_id] = NULL;
	return 0;
}

static int
nix_inl_lf_attach(struct nix_inl_dev *inl_dev)
{
	struct msix_offset_rsp *msix_rsp;
	struct dev *dev = &inl_dev->dev;
	struct mbox *mbox = mbox_get(dev->mbox);
	struct rsrc_attach_req *req;
	uint64_t nix_blkaddr;
	int rc = -ENOSPC;

	req = mbox_alloc_msg_attach_resources(mbox);
	if (req == NULL)
		goto exit;
	req->modify = true;
	/* Attach 1 NIXLF, SSO HWS and SSO HWGRP */
	req->nixlf = true;
	req->ssow = 1;
	req->sso = 1;
	if (inl_dev->attach_cptlf) {
		req->cptlfs = inl_dev->nb_cptlf;
		req->cpt_blkaddr = RVU_BLOCK_ADDR_CPT0;
	}

	rc = mbox_process(dev->mbox);
	if (rc)
		goto exit;

	/* Get MSIX vector offsets */
	mbox_alloc_msg_msix_offset(mbox);
	rc = mbox_process_msg(dev->mbox, (void **)&msix_rsp);
	if (rc)
		goto exit;

	inl_dev->nix_msixoff = msix_rsp->nix_msixoff;
	inl_dev->ssow_msixoff = msix_rsp->ssow_msixoff[0];
	inl_dev->sso_msixoff = msix_rsp->sso_msixoff[0];

	for (int i = 0; i < inl_dev->nb_cptlf; i++)
		inl_dev->cpt_msixoff[i] = msix_rsp->cptlf_msixoff[i];

	nix_blkaddr = nix_get_blkaddr(dev);
	inl_dev->is_nix1 = (nix_blkaddr == RVU_BLOCK_ADDR_NIX1);

	/* Update base addresses for LF's */
	inl_dev->nix_base = dev->bar2 + (nix_blkaddr << 20);
	inl_dev->ssow_base = dev->bar2 + (RVU_BLOCK_ADDR_SSOW << 20);
	inl_dev->sso_base = dev->bar2 + (RVU_BLOCK_ADDR_SSO << 20);
	inl_dev->cpt_base = dev->bar2 + (RVU_BLOCK_ADDR_CPT0 << 20);

	rc = 0;
exit:
	mbox_put(mbox);
	return rc;
}

static int
nix_inl_lf_detach(struct nix_inl_dev *inl_dev)
{
	struct dev *dev = &inl_dev->dev;
	struct mbox *mbox = mbox_get(dev->mbox);
	struct rsrc_detach_req *req;
	int rc = -ENOSPC;

	req = mbox_alloc_msg_detach_resources(mbox);
	if (req == NULL)
		goto exit;
	req->partial = true;
	req->nixlf = true;
	req->ssow = true;
	req->sso = true;
	req->cptlfs = !!inl_dev->attach_cptlf;

	rc = mbox_process(dev->mbox);
exit:
	mbox_put(mbox);
	return rc;
}

static int
nix_inl_dev_wait_for_sso_empty(struct nix_inl_dev *inl_dev)
{
	uintptr_t sso_base = inl_dev->sso_base;
	int wait_ms = 3000;

	while (wait_ms > 0) {
		/* Break when empty */
		if (!plt_read64(sso_base + SSO_LF_GGRP_XAQ_CNT) &&
		    !plt_read64(sso_base + SSO_LF_GGRP_AQ_CNT))
			return 0;

		plt_delay_us(1000);
		wait_ms -= 1;
	}

	return -ETIMEDOUT;
}

int
roc_nix_inl_dev_xaq_realloc(uint64_t aura_handle)
{
	struct idev_cfg *idev = idev_get_cfg();
	struct nix_inl_dev *inl_dev;
	int rc, i;

	if (idev == NULL)
		return 0;

	inl_dev = idev->nix_inl_dev;
	/* Nothing to do if no inline device */
	if (!inl_dev)
		return 0;

	if (!aura_handle) {
		inl_dev->nb_xae = inl_dev->iue;
		goto no_pool;
	}

	/* Check if aura is already considered */
	for (i = 0; i < inl_dev->pkt_pools_cnt; i++) {
		if (inl_dev->pkt_pools[i] == aura_handle)
			return 0;
	}

no_pool:
	/* Disable RQ if enabled */
	for (i = 0; i < inl_dev->nb_rqs; i++) {
		if (!inl_dev->rqs[i].inl_dev_refs)
			continue;
		rc = nix_rq_ena_dis(&inl_dev->dev, &inl_dev->rqs[i], false);
		if (rc) {
			plt_err("Failed to disable inline dev RQ %d, rc=%d", i,
				rc);
			return rc;
		}
	}

	/* Wait for events to be removed */
	rc = nix_inl_dev_wait_for_sso_empty(inl_dev);
	if (rc) {
		plt_err("Timeout waiting for inline device event cleanup");
		goto exit;
	}

	/* Disable HWGRP */
	plt_write64(0, inl_dev->sso_base + SSO_LF_GGRP_QCTL);

	inl_dev->pkt_pools_cnt++;
	inl_dev->pkt_pools =
		plt_realloc(inl_dev->pkt_pools,
			    sizeof(uint64_t) * inl_dev->pkt_pools_cnt, 0);
	if (!inl_dev->pkt_pools)
		inl_dev->pkt_pools_cnt = 0;
	else
		inl_dev->pkt_pools[inl_dev->pkt_pools_cnt - 1] = aura_handle;
	inl_dev->nb_xae += roc_npa_aura_op_limit_get(aura_handle);

	/* Realloc XAQ aura */
	rc = sso_hwgrp_init_xaq_aura(&inl_dev->dev, &inl_dev->xaq,
				     inl_dev->nb_xae, inl_dev->xae_waes,
				     inl_dev->xaq_buf_size, 1);
	if (rc) {
		plt_err("Failed to reinitialize xaq aura, rc=%d", rc);
		return rc;
	}

	/* Setup xaq for hwgrps */
	rc = sso_hwgrp_alloc_xaq(&inl_dev->dev,
				 roc_npa_aura_handle_to_aura(inl_dev->xaq.aura_handle), 1);
	if (rc) {
		plt_err("Failed to setup hwgrp xaq aura, rc=%d", rc);
		return rc;
	}

	/* Enable HWGRP */
	plt_write64(0x1, inl_dev->sso_base + SSO_LF_GGRP_QCTL);

exit:
	/* Renable RQ */
	for (i = 0; i < inl_dev->nb_rqs; i++) {
		if (!inl_dev->rqs[i].inl_dev_refs)
			continue;

		rc = nix_rq_ena_dis(&inl_dev->dev, &inl_dev->rqs[i], true);
		if (rc)
			plt_err("Failed to enable inline dev RQ %d, rc=%d", i,
				rc);
	}

	return rc;
}

static void
inl_outb_soft_exp_poll(struct nix_inl_dev *inl_dev, uint32_t ring_idx)
{
	union roc_ot_ipsec_err_ring_head head;
	struct roc_ot_ipsec_outb_sa *sa;
	uint16_t head_l, tail_l;
	uint64_t *ring_base;
	uint32_t port_id;

	port_id = ring_idx / ROC_NIX_SOFT_EXP_PER_PORT_MAX_RINGS;
	ring_base = PLT_PTR_CAST(inl_dev->sa_soft_exp_ring[ring_idx]);
	if (!ring_base) {
		plt_err("Invalid soft exp ring base");
		return;
	}

	head.u64 = __atomic_load_n(ring_base, __ATOMIC_ACQUIRE);
	head_l = head.s.head_pos;
	tail_l = head.s.tail_pos;

	while (tail_l != head_l) {
		union roc_ot_ipsec_err_ring_entry entry;
		int poll_counter = 0;

		while (poll_counter++ <
		       ROC_NIX_INL_SA_SOFT_EXP_ERR_MAX_POLL_COUNT) {
			plt_delay_us(20);
			entry.u64 = __atomic_load_n(ring_base + tail_l + 1,
						    __ATOMIC_ACQUIRE);
			if (likely(entry.u64))
				break;
		}

		entry.u64 = plt_be_to_cpu_64(entry.u64);
		sa = (struct roc_ot_ipsec_outb_sa *)(((uint64_t)entry.s.data1
						      << 51) |
						     (entry.s.data0 << 7));

		if (sa != NULL) {
			uint64_t tmp = ~(uint32_t)0x0;
			inl_dev->work_cb(&tmp, sa, (port_id << 8) | 0x1);
			__atomic_store_n(ring_base + tail_l + 1, 0ULL,
					 __ATOMIC_RELAXED);
			__atomic_fetch_add((uint32_t *)ring_base, 1,
					   __ATOMIC_ACQ_REL);
		} else
			plt_err("Invalid SA");

		tail_l++;
	}
}

static uint32_t
nix_inl_outb_poll_thread(void *args)
{
	struct nix_inl_dev *inl_dev = args;
	uint32_t poll_freq;
	uint32_t i;
	bool bit;

	poll_freq = inl_dev->soft_exp_poll_freq;

	while (!soft_exp_poll_thread_exit) {
		if (soft_exp_consumer_cnt) {
			for (i = 0; i < ROC_NIX_INL_MAX_SOFT_EXP_RNGS; i++) {
				bit = plt_bitmap_get(
					inl_dev->soft_exp_ring_bmap, i);
				if (bit)
					inl_outb_soft_exp_poll(inl_dev, i);
			}
		}
		usleep(poll_freq);
	}

	return 0;
}

static int
nix_inl_outb_poll_thread_setup(struct nix_inl_dev *inl_dev)
{
	struct plt_bitmap *bmap;
	size_t bmap_sz;
	uint32_t i;
	void *mem;
	int rc;

	/* Allocate a bitmap that pool thread uses to get the port_id
	 * that's corresponding to the inl_outb_soft_exp_ring
	 */
	bmap_sz =
		plt_bitmap_get_memory_footprint(ROC_NIX_INL_MAX_SOFT_EXP_RNGS);
	mem = plt_zmalloc(bmap_sz, PLT_CACHE_LINE_SIZE);
	if (mem == NULL) {
		plt_err("soft expiry ring bmap alloc failed");
		rc = -ENOMEM;
		goto exit;
	}

	bmap = plt_bitmap_init(ROC_NIX_INL_MAX_SOFT_EXP_RNGS, mem, bmap_sz);
	if (!bmap) {
		plt_err("soft expiry ring bmap init failed");
		plt_free(mem);
		rc = -ENOMEM;
		goto exit;
	}

	inl_dev->soft_exp_ring_bmap_mem = mem;
	inl_dev->soft_exp_ring_bmap = bmap;
	inl_dev->sa_soft_exp_ring = plt_zmalloc(
		ROC_NIX_INL_MAX_SOFT_EXP_RNGS * sizeof(uint64_t), 0);
	if (!inl_dev->sa_soft_exp_ring) {
		plt_err("soft expiry ring pointer array alloc failed");
		plt_free(mem);
		rc = -ENOMEM;
		goto exit;
	}

	for (i = 0; i < ROC_NIX_INL_MAX_SOFT_EXP_RNGS; i++)
		plt_bitmap_clear(inl_dev->soft_exp_ring_bmap, i);

	soft_exp_consumer_cnt = 0;
	soft_exp_poll_thread_exit = false;
	rc = plt_thread_create_control(&inl_dev->soft_exp_poll_thread,
			"outb-poll", nix_inl_outb_poll_thread, inl_dev);
	if (rc) {
		plt_bitmap_free(inl_dev->soft_exp_ring_bmap);
		plt_free(inl_dev->soft_exp_ring_bmap_mem);
	}

exit:
	return rc;
}

void *
roc_nix_inl_dev_qptr_get(uint8_t qid)
{
	struct idev_cfg *idev = idev_get_cfg();
	struct nix_inl_dev *inl_dev = NULL;

	if (idev)
		inl_dev = idev->nix_inl_dev;

	if (!inl_dev) {
		plt_nix_dbg("Inline Device could not be detected");
		return NULL;
	}
	if (!inl_dev->attach_cptlf) {
		plt_err("No CPT LFs are attached to Inline Device");
		return NULL;
	}
	if (qid >= inl_dev->nb_cptlf) {
		plt_err("Invalid qid: %u total queues: %d", qid, inl_dev->nb_cptlf);
		return NULL;
	}
	return &inl_dev->q_info[qid];
}

int
roc_nix_inl_dev_stats_get(struct roc_nix_stats *stats)
{
	struct idev_cfg *idev = idev_get_cfg();
	struct nix_inl_dev *inl_dev = NULL;

	if (stats == NULL)
		return NIX_ERR_PARAM;

	if (idev && idev->nix_inl_dev)
		inl_dev = idev->nix_inl_dev;

	if (!inl_dev)
		return -EINVAL;

	stats->rx_octs = INL_NIX_RX_STATS(NIX_STAT_LF_RX_RX_OCTS);
	stats->rx_ucast = INL_NIX_RX_STATS(NIX_STAT_LF_RX_RX_UCAST);
	stats->rx_bcast = INL_NIX_RX_STATS(NIX_STAT_LF_RX_RX_BCAST);
	stats->rx_mcast = INL_NIX_RX_STATS(NIX_STAT_LF_RX_RX_MCAST);
	stats->rx_drop = INL_NIX_RX_STATS(NIX_STAT_LF_RX_RX_DROP);
	stats->rx_drop_octs = INL_NIX_RX_STATS(NIX_STAT_LF_RX_RX_DROP_OCTS);
	stats->rx_fcs = INL_NIX_RX_STATS(NIX_STAT_LF_RX_RX_FCS);
	stats->rx_err = INL_NIX_RX_STATS(NIX_STAT_LF_RX_RX_ERR);
	stats->rx_drop_bcast = INL_NIX_RX_STATS(NIX_STAT_LF_RX_RX_DRP_BCAST);
	stats->rx_drop_mcast = INL_NIX_RX_STATS(NIX_STAT_LF_RX_RX_DRP_MCAST);
	stats->rx_drop_l3_bcast = INL_NIX_RX_STATS(NIX_STAT_LF_RX_RX_DRP_L3BCAST);
	stats->rx_drop_l3_mcast = INL_NIX_RX_STATS(NIX_STAT_LF_RX_RX_DRP_L3MCAST);

	return 0;
}

int
roc_nix_inl_dev_stats_reset(void)
{
	struct idev_cfg *idev = idev_get_cfg();
	struct nix_inl_dev *inl_dev = NULL;
	struct mbox *mbox;
	int rc;

	if (idev && idev->nix_inl_dev)
		inl_dev = idev->nix_inl_dev;

	if (!inl_dev)
		return -EINVAL;

	mbox = mbox_get((&inl_dev->dev)->mbox);

	if (mbox_alloc_msg_nix_stats_rst(mbox) == NULL) {
		rc = -ENOMEM;
		goto exit;
	}

	rc = mbox_process(mbox);
exit:
	mbox_put(mbox);
	return rc;
}

int
roc_nix_inl_dev_init(struct roc_nix_inl_dev *roc_inl_dev)
{
	struct plt_pci_device *pci_dev;
	struct nix_inl_dev *inl_dev;
	struct idev_cfg *idev;
	int start_index;
	int resp_count;
	int rc, i;

	pci_dev = roc_inl_dev->pci_dev;

	/* Skip probe if already done */
	idev = idev_get_cfg();
	if (idev == NULL)
		return -ENOTSUP;

	if (idev->nix_inl_dev) {
		plt_info("Skipping device %s, inline device already probed",
			 pci_dev->name);
		return -EEXIST;
	}

	PLT_STATIC_ASSERT(sizeof(struct nix_inl_dev) <= ROC_NIX_INL_MEM_SZ);

	inl_dev = (struct nix_inl_dev *)roc_inl_dev->reserved;
	memset(inl_dev, 0, sizeof(*inl_dev));

	inl_dev->pci_dev = pci_dev;
	inl_dev->ipsec_in_min_spi = roc_inl_dev->ipsec_in_min_spi;
	inl_dev->ipsec_in_max_spi = roc_inl_dev->ipsec_in_max_spi;
	inl_dev->selftest = roc_inl_dev->selftest;
	inl_dev->is_multi_channel = roc_inl_dev->is_multi_channel;
	inl_dev->channel = roc_inl_dev->channel;
	inl_dev->chan_mask = roc_inl_dev->chan_mask;
	inl_dev->attach_cptlf = true;
	inl_dev->wqe_skip = roc_inl_dev->wqe_skip;
	inl_dev->spb_drop_pc = NIX_AURA_DROP_PC_DFLT;
	inl_dev->lpb_drop_pc = NIX_AURA_DROP_PC_DFLT;
	inl_dev->set_soft_exp_poll = !!roc_inl_dev->soft_exp_poll_freq;
	inl_dev->nb_rqs = inl_dev->is_multi_channel ? 1 : PLT_MAX_ETHPORTS;
	inl_dev->nb_meta_bufs = roc_inl_dev->nb_meta_bufs;
	inl_dev->meta_buf_sz = roc_inl_dev->meta_buf_sz;
	inl_dev->soft_exp_poll_freq = roc_inl_dev->soft_exp_poll_freq;
	inl_dev->custom_inb_sa = roc_inl_dev->custom_inb_sa;
	inl_dev->nix_inb_q_bpid = -1;
	inl_dev->nb_cptlf = 1;
	inl_dev->ipsec_prof_id = 0;
	inl_dev->res_addr_offset = roc_inl_dev->res_addr_offset;

	if (roc_model_is_cn9k() || roc_model_is_cn10k())
		inl_dev->eng_grpmask = (1ULL << ROC_LEGACY_CPT_DFLT_ENG_GRP_SE |
					1ULL << ROC_LEGACY_CPT_DFLT_ENG_GRP_SE_IE |
					1ULL << ROC_LEGACY_CPT_DFLT_ENG_GRP_AE);
	else
		inl_dev->eng_grpmask =
			(1ULL << ROC_CPT_DFLT_ENG_GRP_SE | 1ULL << ROC_CPT_DFLT_ENG_GRP_AE);

	/* RXC inject uses extra CPT LF */
	if (roc_inl_dev->rx_inj_ena) {
		inl_dev->rx_inj_ena = 1;
		inl_dev->nb_cptlf++;
	}

	/* Attach inline inbound CPT LF to NIX has multi queue support */
	if (roc_feature_nix_has_inl_multi_queue() && roc_inl_dev->nb_inb_cptlfs) {
		inl_dev->nb_inb_cptlfs = roc_inl_dev->nb_inb_cptlfs;

		inl_dev->inb_cpt_lf_id = inl_dev->nb_cptlf;
		inl_dev->nb_cptlf += inl_dev->nb_inb_cptlfs;
	}

	if (roc_inl_dev->spb_drop_pc)
		inl_dev->spb_drop_pc = roc_inl_dev->spb_drop_pc;
	if (roc_inl_dev->lpb_drop_pc)
		inl_dev->lpb_drop_pc = roc_inl_dev->lpb_drop_pc;

	/* Initialize base device */
	rc = dev_init(&inl_dev->dev, pci_dev);
	if (rc) {
		plt_err("Failed to init roc device");
		goto error;
	}

	/* Attach LF resources */
	rc = nix_inl_lf_attach(inl_dev);
	if (rc) {
		plt_err("Failed to attach LF resources, rc=%d", rc);
		goto dev_cleanup;
	}

	/* Setup NIX LF */
	rc = nix_inl_nix_setup(inl_dev);
	if (rc)
		goto lf_detach;

	/* Setup SSO LF */
	rc = nix_inl_sso_setup(inl_dev);
	if (rc)
		goto nix_release;

	/* Setup CPT LF */
	rc = nix_inl_cpt_setup(inl_dev, false);
	if (rc)
		goto sso_release;

	/* Setup device specific inb SA table */
	rc = nix_inl_nix_ipsec_cfg(inl_dev, true);
	if (rc) {
		plt_err("Failed to setup NIX Inbound SA conf, rc=%d", rc);
		goto cpt_release;
	}

	/* Setup Reassembly */
	if (roc_feature_nix_has_plain_pkt_reassembly()) {
		inl_dev->reass_ena = 1;

		rc = nix_inl_nix_reass_setup(inl_dev);
		if (rc)
			goto cpt_release;
	}

	if (inl_dev->set_soft_exp_poll) {
		rc = nix_inl_outb_poll_thread_setup(inl_dev);
		if (rc)
			goto cpt_release;
	}

	/* Perform selftest if asked for */
	if (inl_dev->selftest) {
		rc = nix_inl_selftest();
		if (rc)
			goto cpt_release;
	}
	inl_dev->max_ipsec_rules = roc_inl_dev->max_ipsec_rules;

	if (inl_dev->max_ipsec_rules && roc_inl_dev->is_multi_channel) {
		inl_dev->ipsec_index =
			plt_zmalloc(sizeof(int) * inl_dev->max_ipsec_rules, PLT_CACHE_LINE_SIZE);
		if (inl_dev->ipsec_index == NULL) {
			rc = NPC_ERR_NO_MEM;
			goto cpt_release;
		}
		rc = npc_mcam_alloc_entries(inl_dev->dev.mbox, inl_dev->max_ipsec_rules,
					    inl_dev->ipsec_index, inl_dev->max_ipsec_rules,
					    NPC_MCAM_HIGHER_PRIO, &resp_count, 1);
		if (rc) {
			plt_free(inl_dev->ipsec_index);
			goto cpt_release;
		}

		start_index = inl_dev->ipsec_index[0];
		for (i = 0; i < resp_count; i++)
			inl_dev->ipsec_index[i] = start_index + i;

		inl_dev->curr_ipsec_idx = 0;
		inl_dev->alloc_ipsec_rules = resp_count;
	}

	idev->nix_inl_dev = inl_dev;

	return 0;
cpt_release:
	rc |= nix_inl_cpt_release(inl_dev);
sso_release:
	rc |= nix_inl_sso_release(inl_dev);
nix_release:
	rc |= nix_inl_nix_release(inl_dev);
lf_detach:
	rc |= nix_inl_lf_detach(inl_dev);
dev_cleanup:
	rc |= dev_fini(&inl_dev->dev, pci_dev);
error:
	return rc;
}

int
roc_nix_inl_dev_fini(struct roc_nix_inl_dev *roc_inl_dev)
{
	struct plt_pci_device *pci_dev;
	struct nix_inl_dev *inl_dev;
	struct idev_cfg *idev;
	uint32_t i;
	int rc;

	idev = idev_get_cfg();
	if (idev == NULL)
		return 0;

	if (!idev->nix_inl_dev ||
	    PLT_PTR_DIFF(roc_inl_dev->reserved, idev->nix_inl_dev))
		return 0;

	inl_dev = idev->nix_inl_dev;
	pci_dev = inl_dev->pci_dev;

	if (inl_dev->ipsec_index && roc_inl_dev->is_multi_channel) {
		for (i = inl_dev->curr_ipsec_idx; i < inl_dev->alloc_ipsec_rules; i++)
			npc_mcam_free_entry(inl_dev->dev.mbox, inl_dev->ipsec_index[i]);
		plt_free(inl_dev->ipsec_index);
	}

	if (inl_dev->set_soft_exp_poll) {
		soft_exp_poll_thread_exit = true;
		plt_thread_join(inl_dev->soft_exp_poll_thread, NULL);
		plt_bitmap_free(inl_dev->soft_exp_ring_bmap);
		plt_free(inl_dev->soft_exp_ring_bmap_mem);
		plt_free(inl_dev->sa_soft_exp_ring);
	}

	/* Flush Inbound CTX cache entries */
	nix_inl_cpt_ctx_cache_sync(inl_dev);

	/* Release CPT */
	rc = nix_inl_cpt_release(inl_dev);

	/* Release SSO */
	rc |= nix_inl_sso_release(inl_dev);

	/* Release NIX */
	rc |= nix_inl_nix_release(inl_dev);

	/* Detach LF's */
	rc |= nix_inl_lf_detach(inl_dev);

	/* Cleanup mbox */
	rc |= dev_fini(&inl_dev->dev, pci_dev);
	if (rc)
		return rc;

	idev->nix_inl_dev = NULL;
	return 0;
}

int
roc_nix_inl_dev_cpt_setup(bool use_inl_dev_sso)
{
	struct idev_cfg *idev = idev_get_cfg();
	struct nix_inl_dev *inl_dev = NULL;

	if (!idev || !idev->nix_inl_dev)
		return -ENOENT;
	inl_dev = idev->nix_inl_dev;

	if (inl_dev->cpt_lf[0].dev != NULL)
		return -EBUSY;

	return nix_inl_cpt_setup(inl_dev, use_inl_dev_sso);
}

int
roc_nix_inl_dev_cpt_release(void)
{
	struct idev_cfg *idev = idev_get_cfg();
	struct nix_inl_dev *inl_dev = NULL;

	if (!idev || !idev->nix_inl_dev)
		return -ENOENT;
	inl_dev = idev->nix_inl_dev;

	if (inl_dev->cpt_lf[0].dev == NULL)
		return 0;

	return nix_inl_cpt_release(inl_dev);
}
