/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2021 Marvell.
 */
#ifndef __CN10K_RX_H__
#define __CN10K_RX_H__

#include <rte_ethdev.h>
#include <rte_security_driver.h>
#include <rte_vect.h>
#include "cn10k_rxtx.h"

#define NSEC_PER_SEC             1000000000L

#define NIX_RX_OFFLOAD_NONE	     (0)
#define NIX_RX_OFFLOAD_RSS_F	     BIT(0)
#define NIX_RX_OFFLOAD_PTYPE_F	     BIT(1)
#define NIX_RX_OFFLOAD_CHECKSUM_F    BIT(2)
#define NIX_RX_OFFLOAD_MARK_UPDATE_F BIT(3)
#define NIX_RX_OFFLOAD_TSTAMP_F	     BIT(4)
#define NIX_RX_OFFLOAD_VLAN_STRIP_F  BIT(5)
#define NIX_RX_OFFLOAD_SECURITY_F    BIT(6)
#define NIX_RX_OFFLOAD_MAX	     (NIX_RX_OFFLOAD_SECURITY_F << 1)

/* Flags to control cqe_to_mbuf conversion function.
 * Defining it from backwards to denote its been
 * not used as offload flags to pick function
 */
#define NIX_RX_REAS_F	   BIT(12)
#define NIX_RX_VWQE_F	   BIT(13)
#define NIX_RX_MULTI_SEG_F BIT(14)

#define CNXK_NIX_CQ_ENTRY_SZ 128
#define NIX_DESCS_PER_LOOP   4
#define CQE_CAST(x)	     ((struct nix_cqe_hdr_s *)(x))
#define CQE_SZ(x)	     ((x) * CNXK_NIX_CQ_ENTRY_SZ)

#define CQE_PTR_OFF(b, i, o, f)                                                \
	(((f) & NIX_RX_VWQE_F) ?                                               \
		       (uint64_t *)(((uintptr_t)((uint64_t *)(b))[i]) + (o)) : \
		       (uint64_t *)(((uintptr_t)(b)) + CQE_SZ(i) + (o)))
#define CQE_PTR_DIFF(b, i, o, f)                                               \
	(((f) & NIX_RX_VWQE_F) ?                                               \
		 (uint64_t *)(((uintptr_t)((uint64_t *)(b))[i]) - (o)) :       \
		       (uint64_t *)(((uintptr_t)(b)) + CQE_SZ(i) - (o)))

#define NIX_RX_SEC_UCC_CONST                                                                       \
	((RTE_MBUF_F_RX_IP_CKSUM_BAD >> 1) |                                                       \
	 ((RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_GOOD) >> 1) << 8 |                 \
	 ((RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_BAD) >> 1) << 16 |                 \
	 ((RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_GOOD) >> 1) << 32 |                \
	 ((RTE_MBUF_F_RX_IP_CKSUM_GOOD | RTE_MBUF_F_RX_L4_CKSUM_GOOD) >> 1) << 48)

#ifdef RTE_LIBRTE_MEMPOOL_DEBUG
static inline void
nix_mbuf_validate_next(struct rte_mbuf *m)
{
	if (m->nb_segs == 1 && m->next) {
		rte_panic("mbuf->next[%p] valid when mbuf->nb_segs is %d",
			m->next, m->nb_segs);
	}
}
#else
static inline void
nix_mbuf_validate_next(struct rte_mbuf *m)
{
	RTE_SET_USED(m);
}
#endif

#define NIX_RX_SEC_REASSEMBLY_F \
	(NIX_RX_REAS_F | NIX_RX_OFFLOAD_SECURITY_F)

static inline rte_eth_ip_reassembly_dynfield_t *
cnxk_ip_reassembly_dynfield(struct rte_mbuf *mbuf,
		int ip_reassembly_dynfield_offset)
{
	return RTE_MBUF_DYNFIELD(mbuf, ip_reassembly_dynfield_offset,
				 rte_eth_ip_reassembly_dynfield_t *);
}

union mbuf_initializer {
	struct {
		uint16_t data_off;
		uint16_t refcnt;
		uint16_t nb_segs;
		uint16_t port;
	} fields;
	uint64_t value;
};

static __rte_always_inline uint64_t
nix_clear_data_off(uint64_t oldval)
{
	union mbuf_initializer mbuf_init = {.value = oldval};

	mbuf_init.fields.data_off = 0;
	return mbuf_init.value;
}

static __rte_always_inline struct rte_mbuf *
nix_get_mbuf_from_cqe(void *cq, const uint64_t data_off)
{
	rte_iova_t buff;

	/* Skip CQE, NIX_RX_PARSE_S and SG HDR(9 DWORDs) and peek buff addr */
	buff = *((rte_iova_t *)((uint64_t *)cq + 9));
	return (struct rte_mbuf *)(buff - data_off);
}

static __rte_always_inline void
nix_sec_flush_meta_burst(uint16_t lmt_id, uint64_t data, uint16_t lnum,
			 uintptr_t aura_handle)
{
	uint64_t pa;

	/* Prepare PA and Data */
	pa = roc_npa_aura_handle_to_base(aura_handle) + NPA_LF_AURA_BATCH_FREE0;
	pa |= ((data & 0x7) << 4);

	data >>= 3;
	data <<= 19;
	data |= (uint64_t)lmt_id;
	data |= (uint64_t)(lnum - 1) << 12;

	roc_lmt_submit_steorl(data, pa);
}

static __rte_always_inline void
nix_sec_flush_meta(uintptr_t laddr, uint16_t lmt_id, uint8_t loff,
		   uintptr_t aura_handle)
{
	uint64_t pa;

	/* laddr is pointing to first pointer */
	laddr -= 8;

	/* Trigger free either on lmtline full or different aura handle */
	pa = roc_npa_aura_handle_to_base(aura_handle) + NPA_LF_AURA_BATCH_FREE0;

	/* Update aura handle */
	*(uint64_t *)laddr = (((uint64_t)(loff & 0x1) << 32) |
			      roc_npa_aura_handle_to_aura(aura_handle));

	pa |= ((uint64_t)(loff >> 1) << 4);
	roc_lmt_submit_steorl(lmt_id, pa);
}

#if defined(RTE_ARCH_ARM64)
static __rte_always_inline uint64_t
nix_sec_reass_frags_get(const struct cpt_cn10k_parse_hdr_s *hdr, struct rte_mbuf **next_mbufs)
{
	const struct cpt_frag_info_s *finfo;
	uint32_t offset = hdr->w2.fi_offset;
	const uint64_t *frag_ptr;
	uint64x2_t frags23;
	uint16x4_t fsz_w1;

	/* offset of 0 implies 256B, otherwise it implies offset*8B */
	offset = (((offset - 1) & 0x1f) + 1) * 8;
	finfo = RTE_PTR_ADD(hdr, offset);
	frag_ptr = (const uint64_t *)(finfo + 1);
	frags23 = vrev64q_u8(vld1q_u64(frag_ptr));

	next_mbufs[0] = ((struct rte_mbuf *)rte_be_to_cpu_64(hdr->frag1_wqe_ptr) - 1);
	next_mbufs[1] = ((struct rte_mbuf *)vgetq_lane_u64(frags23, 0) - 1);
	next_mbufs[2] = ((struct rte_mbuf *)vgetq_lane_u64(frags23, 1) - 1);

	fsz_w1 = vreinterpret_u16_u64(vdup_n_u64(finfo->w1.u64));
	fsz_w1 = vrev16_u8(fsz_w1);
	return vget_lane_u64(vreinterpret_u64_u16(fsz_w1), 0);
}

static __rte_always_inline void
nix_sec_reass_first_frag_update(struct rte_mbuf *head, const uint8_t *m_ipptr,
				uint64_t fsz, uint64_t cq_w1, uint16_t *ihl)
{
	union nix_rx_parse_u *rx = (union nix_rx_parse_u *)((uintptr_t)(head + 1) + 8);
	uint16_t fragx_sum = vaddv_u16(vreinterpret_u16_u64(vdup_n_u64(fsz)));
	uint8_t lcptr = rx->lcptr;
	uint16_t tot_len;
	uint32_t cksum;
	uint8_t *ipptr;

	ipptr = (uint8_t *)head->buf_addr + head->data_off + lcptr;
	/* Find the L3 header length and update inner pkt based on meta lc type */
	if (((cq_w1 >> 40) & 0xF) == NPC_LT_LC_IP) {
		const struct rte_ipv4_hdr *m_hdr = (const struct rte_ipv4_hdr *)m_ipptr;
		struct rte_ipv4_hdr *hdr = (struct rte_ipv4_hdr *)ipptr;

		*ihl = (m_hdr->version_ihl & 0xf) << 2;

		hdr->fragment_offset = 0;
		tot_len = rte_cpu_to_be_16(fragx_sum + *ihl);
		hdr->total_length = tot_len;
		/* Perform incremental checksum based on meta pkt ip hdr */
		cksum = m_hdr->hdr_checksum;
		cksum += m_hdr->fragment_offset;
		cksum += 0xFFFF;
		cksum += m_hdr->total_length;
		cksum += (uint16_t)(~tot_len);
		cksum = (cksum & 0xFFFF) + ((cksum & 0xFFFF0000) >> 16);
		hdr->hdr_checksum = cksum;

		head->pkt_len = lcptr + *ihl + fragx_sum;
	} else {
		struct rte_ipv6_hdr *hdr = (struct rte_ipv6_hdr *)ipptr;
		size_t ext_len = sizeof(struct rte_ipv6_hdr);
		uint8_t *nxt_hdr = (uint8_t *)hdr;
		uint8_t *nxt_proto = &hdr->proto;
		int nh = hdr->proto;

		*ihl = 0;
		tot_len = 0;
		while (nh != -EINVAL) {
			nxt_hdr += ext_len;
			*ihl += ext_len;
			if (nh == IPPROTO_FRAGMENT) {
				*nxt_proto = *nxt_hdr;
				tot_len = *ihl;
			}
			nh = rte_ipv6_get_next_ext(nxt_hdr, nh, &ext_len);
			nxt_proto = nxt_hdr;
		}

		/* Remove the frag header by moving header 8 bytes forward */
		hdr->payload_len = rte_cpu_to_be_16(fragx_sum + *ihl -
					8 - sizeof(struct rte_ipv6_hdr));

		/* tot_len is sum of all IP header's length before fragment header */
		rte_memcpy(rte_pktmbuf_mtod_offset(head, void *, 8),
			   rte_pktmbuf_mtod(head, void *),
			   lcptr + tot_len);

		head->data_len -= 8;
		head->data_off += 8;
		head->pkt_len = lcptr + *ihl - 8 + fragx_sum;
		/* ihl l3hdr size value should be up to fragment header for next frags */
		*ihl = tot_len + 8;
	}
}

#else
static __rte_always_inline uint64_t
nix_sec_reass_frags_get(const struct cpt_cn10k_parse_hdr_s *hdr, struct rte_mbuf **next_mbufs)
{
	RTE_SET_USED(hdr);
	next_mbufs[0] = NULL;
	next_mbufs[1] = NULL;
	next_mbufs[2] = NULL;
	return 0;
}

static __rte_always_inline void
nix_sec_reass_first_frag_update(struct rte_mbuf *head, const uint8_t *m_ipptr,
				uint64_t fsz, uint64_t cq_w1, uint16_t *ihl)
{
	RTE_SET_USED(head);
	RTE_SET_USED(m_ipptr);
	RTE_SET_USED(fsz);
	RTE_SET_USED(cq_w1);
	*ihl = 0;
}
#endif

static struct rte_mbuf *
nix_sec_attach_frags(const struct cpt_cn10k_parse_hdr_s *hdr,
		     struct rte_mbuf *head,
		     struct cn10k_inb_priv_data *inb_priv,
		     const uint64_t mbuf_init)
{
	uint8_t num_frags = hdr->w0.num_frags;
	struct rte_mbuf *next_mbufs[3];
	union nix_rx_parse_u *frag_rx;
	struct rte_mbuf *mbuf;
	uint64_t ol_flags;
	uint16_t frag_size;
	uint8_t frag_i = 0;
	uint16_t rlen;
	uint64_t *wqe;
	int off;

	off = inb_priv->reass_dynfield_off;
	ol_flags = BIT_ULL(inb_priv->reass_dynflag_bit);
	ol_flags |= RTE_MBUF_F_RX_SEC_OFFLOAD;

	/* Get frags list */
	nix_sec_reass_frags_get(hdr, next_mbufs);

	/* Frag-0: */
	wqe = (uint64_t *)(head + 1);
	rlen = ((*(wqe + 10)) >> 16) & 0xFFFF;

	frag_rx = (union nix_rx_parse_u *)(wqe + 1);

	head->ol_flags = ol_flags;
	/* Update dynamic field with userdata */
	*rte_security_dynfield(head) = (uint64_t)inb_priv->userdata;

	num_frags--;
	mbuf = head;

	/* Frag-1+: */
	while (num_frags) {
		cnxk_ip_reassembly_dynfield(mbuf, off)->next_frag = next_mbufs[frag_i];
		cnxk_ip_reassembly_dynfield(mbuf, off)->nb_frags = num_frags;
		mbuf = next_mbufs[frag_i];
		wqe = (uint64_t *)(mbuf + 1);
		rlen = ((*(wqe + 10)) >> 16) & 0xFFFF;

		frag_rx = (union nix_rx_parse_u *)(wqe + 1);
		frag_size = rlen + frag_rx->lcptr - frag_rx->laptr;

		*(uint64_t *)(&mbuf->rearm_data) = mbuf_init;
		mbuf->data_len = frag_size;
		mbuf->pkt_len = frag_size;
		mbuf->ol_flags = ol_flags;

		/* Update dynamic field with userdata */
		*rte_security_dynfield(mbuf) = (uint64_t)inb_priv->userdata;

		/* Mark frag as get */
		RTE_MEMPOOL_CHECK_COOKIES(mbuf->pool, (void **)&mbuf, 1, 1);

		num_frags--;
		frag_i++;
	}
	cnxk_ip_reassembly_dynfield(mbuf, off)->nb_frags = 0;
	cnxk_ip_reassembly_dynfield(mbuf, off)->next_frag = NULL;

	return head;
}

static __rte_always_inline struct rte_mbuf *
nix_sec_reassemble_frags(const struct cpt_cn10k_parse_hdr_s *hdr, struct rte_mbuf *head,
			 uint64_t cq_w1, uint64_t cq_w5, uint64_t mbuf_init)
{
	uint8_t num_frags = hdr->w0.num_frags;
	union nix_rx_parse_u *frag_rx;
	struct rte_mbuf *next_mbufs[3];
	uint16_t data_off, b_off;
	const uint8_t *m_ipptr;
	uint16_t l3_hdr_size;
	struct rte_mbuf *mbuf;
	uint16_t frag_size;
	uint64_t fsz_w1;
	uint64_t *wqe;

	/* Base data offset */
	b_off = mbuf_init & 0xFFFFUL;
	mbuf_init &= ~0xFFFFUL;

	/* Get list of all fragments and frag sizes */
	fsz_w1 = nix_sec_reass_frags_get(hdr, next_mbufs);

	/* Frag-0: */
	wqe = (uint64_t *)(head + 1);

	/* First fragment data len is already update by caller */
	m_ipptr = ((const uint8_t *)hdr + ((cq_w5 >> 16) & 0xFF));
	nix_sec_reass_first_frag_update(head, m_ipptr, fsz_w1, cq_w1, &l3_hdr_size);
	fsz_w1 >>= 16;

	/* Frag-1: */
	head->next = next_mbufs[0];
	mbuf = next_mbufs[0];
	wqe = (uint64_t *)(mbuf + 1);
	frag_rx = (union nix_rx_parse_u *)(wqe + 1);
	frag_size = fsz_w1 & 0xFFFF;
	fsz_w1 >>= 16;

	data_off = b_off + frag_rx->lcptr + l3_hdr_size;
	*(uint64_t *)(&mbuf->rearm_data) = mbuf_init | data_off;
	mbuf->data_len = frag_size;

	/* Mark frag as get */
	RTE_MEMPOOL_CHECK_COOKIES(mbuf->pool, (void **)&mbuf, 1, 1);

	/* Frag-2: */
	if (num_frags > 2) {
		mbuf->next = next_mbufs[1];
		mbuf = next_mbufs[1];
		wqe = (uint64_t *)(mbuf + 1);
		frag_rx = (union nix_rx_parse_u *)(wqe + 1);
		frag_size = fsz_w1 & 0xFFFF;
		fsz_w1 >>= 16;

		data_off = b_off + frag_rx->lcptr + l3_hdr_size;
		*(uint64_t *)(&mbuf->rearm_data) = mbuf_init | data_off;
		mbuf->data_len = frag_size;

		/* Mark frag as get */
		RTE_MEMPOOL_CHECK_COOKIES(mbuf->pool, (void **)&mbuf, 1, 1);
	}

	/* Frag-3: */
	if (num_frags > 3) {
		mbuf->next = next_mbufs[2];
		mbuf = next_mbufs[2];
		wqe = (uint64_t *)(mbuf + 1);
		frag_rx = (union nix_rx_parse_u *)(wqe + 1);
		frag_size = fsz_w1 & 0xFFFF;
		fsz_w1 >>= 16;

		data_off = b_off + frag_rx->lcptr + l3_hdr_size;
		*(uint64_t *)(&mbuf->rearm_data) = mbuf_init | data_off;
		mbuf->data_len = frag_size;

		/* Mark frag as get */
		RTE_MEMPOOL_CHECK_COOKIES(mbuf->pool, (void **)&mbuf, 1, 1);
	}

	head->nb_segs = num_frags;
	return head;
}

static inline struct rte_mbuf *
nix_sec_oop_process(const struct cpt_cn10k_parse_hdr_s *hdr, struct rte_mbuf *mbuf,
		    uint64_t *mbuf_init)
{
	uintptr_t wqe = rte_be_to_cpu_64(hdr->wqe_ptr);
	union nix_rx_parse_u *inner_rx;
	struct rte_mbuf *inner;
	uint16_t data_off;

	inner = ((struct rte_mbuf *)wqe) - 1;

	inner_rx = (union nix_rx_parse_u *)(wqe + 8);
	inner->pkt_len = inner_rx->pkt_lenm1 + 1;
	inner->data_len = inner_rx->pkt_lenm1 + 1;

	/* Mark inner mbuf as get */
	RTE_MEMPOOL_CHECK_COOKIES(inner->pool,
				  (void **)&inner, 1, 1);
	/* Update rearm data for full mbuf as it has
	 * cpt parse header that needs to be skipped.
	 *
	 * Since meta pool will not have private area while
	 * ethdev RQ's first skip would be considering private area
	 * calculate actual data off and update in meta mbuf.
	 */
	data_off = (uintptr_t)hdr - (uintptr_t)mbuf->buf_addr;
	data_off += sizeof(struct cpt_cn10k_parse_hdr_s);
	data_off += hdr->w0.pad_len;
	*mbuf_init &= ~0xFFFFUL;
	*mbuf_init |= (uint64_t)data_off;

	*rte_security_oop_dynfield(mbuf) = inner;
	/* Return outer instead of inner mbuf as inner mbuf would have original encrypted packet */
	return mbuf;
}

static __rte_always_inline struct rte_mbuf *
nix_sec_meta_to_mbuf_sc(uint64_t cq_w1, uint64_t cq_w5, const uint64_t sa_base,
			uintptr_t laddr, uint8_t *loff, struct rte_mbuf *mbuf,
			uint16_t data_off, const uint16_t flags,
			uint64_t mbuf_init)
{
	const void *__p = (void *)((uintptr_t)mbuf + (uint16_t)data_off);
	const struct cpt_cn10k_parse_hdr_s *hdr = (const struct cpt_cn10k_parse_hdr_s *)__p;
	struct cn10k_inb_priv_data *inb_priv;
	struct rte_mbuf *inner = NULL;
	uint32_t sa_idx;
	uint16_t ucc;
	uint32_t len;
	uintptr_t ip;
	void *inb_sa;
	uint64_t w0;

	if (!(cq_w1 & BIT(11)))
		return mbuf;

	if (flags & NIX_RX_REAS_F && hdr->w0.pkt_fmt == ROC_IE_OT_SA_PKT_FMT_FULL) {
		inner = nix_sec_oop_process(hdr, mbuf, &mbuf_init);
	} else {
		inner = (struct rte_mbuf *)(rte_be_to_cpu_64(hdr->wqe_ptr) -
					    sizeof(struct rte_mbuf));

		/* Store meta in lmtline to free
		 * Assume all meta's from same aura.
		 */
		*(uint64_t *)(laddr + (*loff << 3)) = (uint64_t)mbuf;
		*loff = *loff + 1;
	}

	/* Get SPI from CPT_PARSE_S's cookie(already swapped) */
	w0 = hdr->w0.u64;
	sa_idx = w0 >> 32;

	inb_sa = roc_nix_inl_ot_ipsec_inb_sa(sa_base, sa_idx);
	inb_priv = roc_nix_inl_ot_ipsec_inb_sa_sw_rsvd(inb_sa);

	/* Cryptodev injected packet can be identified from SA IDX 0xFFFFFFFF, and
	 * Ethdev injected packet can be identified with match ID 0xFFFF.
	 */
	if (flags & NIX_RX_REAS_F && (sa_idx == 0xFFFFFFFF || hdr->w0.match_id == 0xFFFFU)) {
		*(uint64_t *)(&inner->rearm_data) = (mbuf_init & ~(BIT_ULL(16) - 1)) |
						    inner->data_off;
		if (hdr->w0.match_id == 0xFFFFU)
			*rte_security_dynfield(inner) = (uint64_t)inb_priv->userdata;
	} else {
		/* Update dynamic field with userdata */
		*rte_security_dynfield(inner) = (uint64_t)inb_priv->userdata;
		*(uint64_t *)(&inner->rearm_data) = mbuf_init;
	}

	/* Get ucc from cpt parse header */
	ucc = hdr->w3.hw_ccode;

	/* Calculate inner packet length as IP total len + l2 len */
	ip = (uintptr_t)hdr + ((cq_w5 >> 16) & 0xFF);
	ip += ((cq_w1 >> 40) & 0x6);
	len = rte_be_to_cpu_16(*(uint16_t *)ip);
	len += ((cq_w5 >> 16) & 0xFF) - (cq_w5 & 0xFF);
	len += (cq_w1 & BIT(42)) ? 40 : 0;

	inner->pkt_len = len;
	inner->data_len = len;

	inner->ol_flags = ((CPT_COMP_HWGOOD_MASK & (1U << ucc)) ?
			   RTE_MBUF_F_RX_SEC_OFFLOAD :
			   (RTE_MBUF_F_RX_SEC_OFFLOAD |
			    RTE_MBUF_F_RX_SEC_OFFLOAD_FAILED));

	ucc = hdr->w3.uc_ccode;

	if (ucc && ucc < 0xED) {
		inner->ol_flags |= RTE_MBUF_F_RX_SEC_OFFLOAD_FAILED;
	} else {
		ucc += 3; /* To make codes in 0xFx series except 0 */
		inner->ol_flags |= ((ucc & 0xF0) == 0xF0) ?
			((NIX_RX_SEC_UCC_CONST >> ((ucc & 0xF) << 3))
			 & 0xFF) << 1 : RTE_MBUF_F_RX_IP_CKSUM_GOOD;
	}

	if (!(flags & NIX_RX_REAS_F) || hdr->w0.pkt_fmt != ROC_IE_OT_SA_PKT_FMT_FULL) {
		/* Mark meta mbuf as put */
		RTE_MEMPOOL_CHECK_COOKIES(mbuf->pool, (void **)&mbuf, 1, 0);

		/* Mark inner mbuf as get */
		RTE_MEMPOOL_CHECK_COOKIES(inner->pool, (void **)&inner, 1, 1);
	}

	/* Skip reassembly processing when multi-seg is enabled */
	if (!(flags & NIX_RX_MULTI_SEG_F) && (flags & NIX_RX_REAS_F) && hdr->w0.num_frags) {
		if ((!(hdr->w0.err_sum) || roc_ie_ot_ucc_is_success(hdr->w3.uc_ccode)) &&
		    !(hdr->w0.reas_sts)) {
			/* Reassembly success */
			nix_sec_reassemble_frags(hdr, inner, cq_w1, cq_w5, mbuf_init);

			/* Update dynamic field with userdata */
			*rte_security_dynfield(inner) =
				(uint64_t)inb_priv->userdata;

			/* Assume success */
			inner->ol_flags = RTE_MBUF_F_RX_SEC_OFFLOAD;
		} else {
			/* Reassembly failure */
			nix_sec_attach_frags(hdr, inner, inb_priv, mbuf_init);
		}
	}
	return inner;
}

#if defined(RTE_ARCH_ARM64)

static __rte_always_inline void
nix_sec_meta_to_mbuf(uint64_t cq_w1, uint64_t cq_w5, uintptr_t inb_sa,
		     uintptr_t cpth, struct rte_mbuf *inner,
		     uint8x16_t *rx_desc_field1, uint64_t *ol_flags,
		     const uint16_t flags, uint64x2_t *rearm)
{
	const struct cpt_cn10k_parse_hdr_s *hdr =
		(const struct cpt_cn10k_parse_hdr_s *)cpth;
	uint64_t mbuf_init = vgetq_lane_u64(*rearm, 0);
	struct cn10k_inb_priv_data *inb_priv;
	uintptr_t p;

	/* Clear checksum flags */
	*ol_flags &= ~(RTE_MBUF_F_RX_L4_CKSUM_MASK |
		       RTE_MBUF_F_RX_IP_CKSUM_MASK);


	if (flags & NIX_RX_REAS_F && !inb_sa) {
		/* Clear and update original lower 16 bit of data offset */
		*rearm = (*rearm & ~(BIT_ULL(16) - 1)) | inner->data_off;
	} else {
		/* Get SPI from CPT_PARSE_S's cookie(already swapped) */
		inb_priv = roc_nix_inl_ot_ipsec_inb_sa_sw_rsvd((void *)inb_sa);
		/* Update dynamic field with userdata */
		*rte_security_dynfield(inner) = (uint64_t)inb_priv->userdata;
	}

	/* Clear and update original lower 16 bit of data offset */
	if (flags & NIX_RX_REAS_F && hdr->w0.match_id == 0xFFFFU)
		*rearm = (*rearm & ~(BIT_ULL(16) - 1)) | inner->data_off;

	/* Mark inner mbuf as get */
	if (!(flags & NIX_RX_REAS_F) ||
	    hdr->w0.pkt_fmt != ROC_IE_OT_SA_PKT_FMT_FULL)
		RTE_MEMPOOL_CHECK_COOKIES(inner->pool, (void **)&inner, 1, 1);

	if (!(flags & NIX_RX_MULTI_SEG_F) && flags & NIX_RX_REAS_F && hdr->w0.num_frags) {
		if ((!(hdr->w0.err_sum) || roc_ie_ot_ucc_is_success(hdr->w3.uc_ccode)) &&
		    !(hdr->w0.reas_sts)) {
			/* First frag len */
			inner->pkt_len = vgetq_lane_u16(*rx_desc_field1, 2);
			inner->data_len = vgetq_lane_u16(*rx_desc_field1, 4);
			p = (uintptr_t)&inner->rearm_data;
			*(uint64_t *)p = mbuf_init;

			/* Reassembly success */
			nix_sec_reassemble_frags(hdr, inner, cq_w1, cq_w5, mbuf_init);

			/* Assume success */
			*ol_flags |= RTE_MBUF_F_RX_SEC_OFFLOAD;

			/* Update pkt_len and data_len */
			*rx_desc_field1 = vsetq_lane_u16(inner->pkt_len,
							 *rx_desc_field1, 2);
			*rx_desc_field1 = vsetq_lane_u16(inner->data_len,
							 *rx_desc_field1, 4);

			/* Data offset might be updated */
			mbuf_init = *(uint64_t *)p;
			*rearm = vsetq_lane_u64(mbuf_init, *rearm, 0);
		} else {
			/* Reassembly failure */
			if (inb_sa) {
				nix_sec_attach_frags(hdr, inner, inb_priv, mbuf_init);
				*ol_flags |= inner->ol_flags;
			}
		}
	} else if (flags & NIX_RX_REAS_F) {
		/* Without fragmentation but may have to handle OOP session */
		if (hdr->w0.pkt_fmt == ROC_IE_OT_SA_PKT_FMT_FULL) {
			uint64_t mbuf_init = 0;

			/* Caller has already prepared to return second pass
			 * mbuf and inner mbuf is actually outer.
			 * Store original buffer pointer in dynfield.
			 */
			nix_sec_oop_process(hdr, inner, &mbuf_init);
			/* Clear and update lower 16 bit of data offset */
			*rearm = (*rearm & ~(BIT_ULL(16) - 1)) | mbuf_init;
		}
	}
}
#endif

static __rte_always_inline uint32_t
nix_ptype_get(const void *const lookup_mem, const uint64_t in)
{
	const uint16_t *const ptype = lookup_mem;
	const uint16_t lh_lg_lf = (in & 0xFFF0000000000000) >> 52;
	const uint16_t tu_l2 = ptype[(in & 0x000FFFF000000000) >> 36];
	const uint16_t il4_tu = ptype[PTYPE_NON_TUNNEL_ARRAY_SZ + lh_lg_lf];

	return (il4_tu << PTYPE_NON_TUNNEL_WIDTH) | tu_l2;
}

static __rte_always_inline uint32_t
nix_rx_olflags_get(const void *const lookup_mem, const uint64_t in)
{
	const uint32_t *const ol_flags =
		(const uint32_t *)((const uint8_t *)lookup_mem +
				   PTYPE_ARRAY_SZ);

	return ol_flags[(in & 0xfff00000) >> 20];
}

static inline uint64_t
nix_update_match_id(const uint16_t match_id, uint64_t ol_flags,
		    struct rte_mbuf *mbuf)
{
	/* There is no separate bit to check match_id
	 * is valid or not? and no flag to identify it is an
	 * RTE_FLOW_ACTION_TYPE_FLAG vs RTE_FLOW_ACTION_TYPE_MARK
	 * action. The former case addressed through 0 being invalid
	 * value and inc/dec match_id pair when MARK is activated.
	 * The later case addressed through defining
	 * CNXK_FLOW_MARK_DEFAULT as value for
	 * RTE_FLOW_ACTION_TYPE_MARK.
	 * This would translate to not use
	 * CNXK_FLOW_ACTION_FLAG_DEFAULT - 1 and
	 * CNXK_FLOW_ACTION_FLAG_DEFAULT for match_id.
	 * i.e valid mark_id's are from
	 * 0 to CNXK_FLOW_ACTION_FLAG_DEFAULT - 2
	 */
	if (likely(match_id)) {
		ol_flags |= RTE_MBUF_F_RX_FDIR;
		if (match_id != CNXK_FLOW_ACTION_FLAG_DEFAULT) {
			ol_flags |= RTE_MBUF_F_RX_FDIR_ID;
			mbuf->hash.fdir.hi = match_id - 1;
		}
	}

	return ol_flags;
}

static __rte_always_inline void
nix_cqe_xtract_mseg(const union nix_rx_parse_u *rx, struct rte_mbuf *mbuf,
		    uint64_t rearm, uintptr_t cpth, uintptr_t sa_base, const uint16_t flags)
{
	const struct cpt_cn10k_parse_hdr_s *hdr = (const struct cpt_cn10k_parse_hdr_s *)cpth;
	struct cn10k_inb_priv_data *inb_priv = NULL;
	uint8_t num_frags = 0, frag_i = 0;
	struct rte_mbuf *next_mbufs[3];
	const rte_iova_t *iova_list;
	bool reas_success = false;
	uint16_t later_skip = 0;
	struct rte_mbuf *head;
	const rte_iova_t *eol;
	bool rx_inj = false;
	uint64_t cq_w5 = 0;
	uint16_t ihl = 0;
	uint64_t fsz = 0;
	int dyn_off = 0;
	uint8_t nb_segs;
	uint16_t sg_len;
	uint64_t cq_w1;
	int64_t len;
	uint64_t sg;
	uintptr_t p;

	cq_w1 = *(const uint64_t *)rx;
	if (flags & NIX_RX_REAS_F)
		cq_w5 = *((const uint64_t *)rx + 4);
	/* Use inner rx parse for meta pkts sg list */
	if (cq_w1 & BIT(11) && flags & NIX_RX_OFFLOAD_SECURITY_F) {
		const uint64_t *wqe;
		/* Rx Inject packet must have Match ID 0xFFFF and for this
		 * wqe will get from address stored at mbuf+1 location
		 */
		rx_inj = ((flags & NIX_RX_REAS_F) && ((hdr->w0.match_id == 0xFFFFU) ||
					       (hdr->w0.cookie == 0xFFFFFFFFU)));
		if (rx_inj)
			wqe = (const uint64_t *)*((uint64_t *)(mbuf + 1));
		else
			wqe = (const uint64_t *)(mbuf + 1);

		if (!(flags & NIX_RX_REAS_F) || hdr->w0.pkt_fmt != ROC_IE_OT_SA_PKT_FMT_FULL)
			rx = (const union nix_rx_parse_u *)(wqe + 1);
	}

	sg = *(const uint64_t *)(rx + 1);
	nb_segs = (sg >> 48) & 0x3;

	if (nb_segs == 1 && !(flags & NIX_RX_REAS_F))
		return;

	/* For security we have already updated right pkt_len */
	if (cq_w1 & BIT(11) && flags & NIX_RX_OFFLOAD_SECURITY_F) {
		len = mbuf->pkt_len;

		/* Handle reassembly with multi segs */
		if (flags & NIX_RX_REAS_F && hdr->w0.num_frags) {
			void *inb_sa;

			num_frags = hdr->w0.num_frags;
			inb_sa = roc_nix_inl_ot_ipsec_inb_sa(sa_base, hdr->w0.u64 >> 32);
			inb_priv = roc_nix_inl_ot_ipsec_inb_sa_sw_rsvd(inb_sa);
			ihl = 0;

			dyn_off = inb_priv->reass_dynfield_off;
			fsz = nix_sec_reass_frags_get(hdr, next_mbufs);
			num_frags -= 1;

			if (!(hdr->w0.reas_sts) &&
			    (!(hdr->w0.err_sum) ||
			     roc_ie_ot_ucc_is_success(hdr->w3.uc_ccode)))
				reas_success = true;
		}
	} else {
		len = rx->pkt_lenm1 + 1;
	}

	mbuf->pkt_len = len - (flags & NIX_RX_OFFLOAD_TSTAMP_F ? CNXK_NIX_TIMESYNC_RX_OFFSET : 0);
	mbuf->nb_segs = nb_segs;
	head = mbuf;
	mbuf->data_len =
		(sg & 0xFFFF) - (flags & NIX_RX_OFFLOAD_TSTAMP_F ? CNXK_NIX_TIMESYNC_RX_OFFSET : 0);
	eol = ((const rte_iova_t *)(rx + 1) + ((rx->desc_sizem1 + 1) << 1));
again:
	len -= mbuf->data_len;
	sg = sg >> 16;
	/* Skip SG_S and first IOVA*/
	iova_list = ((const rte_iova_t *)(rx + 1)) + 2;
	nb_segs--;

	later_skip = (uintptr_t)mbuf->buf_addr - (uintptr_t)mbuf;

	while (nb_segs) {
		if (!(flags & NIX_RX_REAS_F) || !rx_inj)
			mbuf->next = (struct rte_mbuf *)(*iova_list - later_skip);
		mbuf = mbuf->next;

		RTE_MEMPOOL_CHECK_COOKIES(mbuf->pool, (void **)&mbuf, 1, 1);

		sg_len = sg & 0XFFFF;
		if (flags & NIX_RX_OFFLOAD_SECURITY_F) {
			/* Adjust last mbuf data length with negative offset for
			 * security pkts if needed.
			 */
			len -= sg_len;
			sg_len = (len > 0) ? sg_len : (sg_len + len);
			len = (len > 0) ? len : 0;
		}

		mbuf->data_len = sg_len;
		sg = sg >> 16;
		p = (uintptr_t)&mbuf->rearm_data;
		if (!(flags & NIX_RX_REAS_F) || !rx_inj)
			*(uint64_t *)p = rearm & ~0xFFFF;
		nb_segs--;
		iova_list++;

		if (!nb_segs && (iova_list + 1 < eol)) {
			sg = *(const uint64_t *)(iova_list);
			nb_segs = (sg >> 48) & 0x3;
			head->nb_segs += nb_segs;
			iova_list = (const rte_iova_t *)(iova_list + 1);
		}
	}

	if ((flags & NIX_RX_REAS_F) && (cq_w1 & BIT(11)) && num_frags) {
		struct rte_mbuf *next_frag = next_mbufs[frag_i];
		uint16_t lcptr, ldptr = 0;

		rx = (const union nix_rx_parse_u *)((uintptr_t)(next_frag + 1) + 8);
		lcptr = (*((const uint64_t *)rx + 4) >> 16) & 0xFF;
		eol = ((const rte_iova_t *)(rx + 1) + ((rx->desc_sizem1 + 1) << 1));
		sg = *(const uint64_t *)(rx + 1);
		nb_segs = (sg >> 48) & 0x3;

		if (reas_success) {
			/* Update first fragment info */
			if (!frag_i) {
				const uint8_t *ipptr;

				ipptr = ((const uint8_t *)hdr + ((cq_w5 >> 16) & 0xFF));
				nix_sec_reass_first_frag_update(head, ipptr, fsz, cq_w1, &ihl);
				fsz >>= 16;
			}
			mbuf->next = next_frag;
			head->nb_segs += nb_segs;
			len = fsz & 0xFFFF;
			fsz >>= 16;
			ldptr = ihl + lcptr;
		} else {
			len = ((eol[0] >> 16) & 0xFFFF) + lcptr;
			head->ol_flags |= BIT_ULL(inb_priv->reass_dynflag_bit) |
				RTE_MBUF_F_RX_SEC_OFFLOAD;
			cnxk_ip_reassembly_dynfield(head, dyn_off)->next_frag = next_frag;
			cnxk_ip_reassembly_dynfield(head, dyn_off)->nb_frags = num_frags;
			/* Update dynamic field with userdata from prev head */
			*rte_security_dynfield(next_frag) = *rte_security_dynfield(head);
			head = next_frag;
			head->pkt_len = len - (flags & NIX_RX_OFFLOAD_TSTAMP_F ?
					       CNXK_NIX_TIMESYNC_RX_OFFSET : 0);
			head->nb_segs = nb_segs;
		}
		mbuf = next_frag;
		p = (uintptr_t)&mbuf->rearm_data;
		*(uint64_t *)p = rearm + ldptr;
		mbuf->data_len = (sg & 0xFFFF) - ldptr -
				 (flags & NIX_RX_OFFLOAD_TSTAMP_F ?
				  CNXK_NIX_TIMESYNC_RX_OFFSET : 0);
		RTE_MEMPOOL_CHECK_COOKIES(mbuf->pool, (void **)&mbuf, 1, 1);
		num_frags--;
		frag_i++;
		goto again;
	} else if ((flags & NIX_RX_REAS_F) && (cq_w1 & BIT(11)) && !reas_success &&
		   hdr->w0.pkt_fmt == ROC_IE_OT_SA_PKT_FMT_FULL) {
		uintptr_t wqe = rte_be_to_cpu_64(hdr->wqe_ptr);

		/* Process OOP packet inner buffer mseg. reas_success flag is used here only
		 * to avoid looping.
		 */
		mbuf = ((struct rte_mbuf *)wqe) - 1;
		rx = (const union nix_rx_parse_u *)(wqe + 8);
		eol = ((const rte_iova_t *)(rx + 1) + ((rx->desc_sizem1 + 1) << 1));
		sg = *(const uint64_t *)(rx + 1);
		nb_segs = (sg >> 48) & 0x3;


		len = mbuf->pkt_len;
		p = (uintptr_t)&mbuf->rearm_data;
		*(uint64_t *)p = rearm;
		mbuf->data_len = (sg & 0xFFFF) -
				 (flags & NIX_RX_OFFLOAD_TSTAMP_F ?
				  CNXK_NIX_TIMESYNC_RX_OFFSET : 0);
		head = mbuf;
		head->nb_segs = nb_segs;
		/* Using this flag to avoid looping in case of OOP */
		reas_success = true;
		goto again;
	}

	/* Update for last failure fragment */
	if ((flags & NIX_RX_REAS_F) && frag_i && !reas_success) {
		cnxk_ip_reassembly_dynfield(head, dyn_off)->next_frag = NULL;
		cnxk_ip_reassembly_dynfield(head, dyn_off)->nb_frags = 0;
	}
}

static __rte_always_inline void
cn10k_nix_cqe_to_mbuf(const struct nix_cqe_hdr_s *cq, const uint32_t tag,
		      struct rte_mbuf *mbuf, const void *lookup_mem,
		      const uint64_t val, const uintptr_t cpth, const uintptr_t sa_base,
		      const uint16_t flag)
{
	const union nix_rx_parse_u *rx =
		(const union nix_rx_parse_u *)((const uint64_t *)cq + 1);
	const uint64_t w1 = *(const uint64_t *)rx;
	uint16_t len = rx->pkt_lenm1 + 1;
	uint64_t ol_flags = 0;
	uintptr_t p;

	if (flag & NIX_RX_OFFLOAD_PTYPE_F)
		mbuf->packet_type = nix_ptype_get(lookup_mem, w1);
	else
		mbuf->packet_type = 0;

	if (flag & NIX_RX_OFFLOAD_RSS_F) {
		mbuf->hash.rss = tag;
		ol_flags |= RTE_MBUF_F_RX_RSS_HASH;
	}

	/* Skip rx ol flags extraction for Security packets */
	if ((!(flag & NIX_RX_SEC_REASSEMBLY_F) || !(w1 & BIT(11))) &&
			flag & NIX_RX_OFFLOAD_CHECKSUM_F)
		ol_flags |= (uint64_t)nix_rx_olflags_get(lookup_mem, w1);

	if (flag & NIX_RX_OFFLOAD_VLAN_STRIP_F) {
		if (rx->vtag0_gone) {
			ol_flags |= RTE_MBUF_F_RX_VLAN | RTE_MBUF_F_RX_VLAN_STRIPPED;
			mbuf->vlan_tci = rx->vtag0_tci;
		}
		if (rx->vtag1_gone) {
			ol_flags |= RTE_MBUF_F_RX_QINQ | RTE_MBUF_F_RX_QINQ_STRIPPED;
			mbuf->vlan_tci_outer = rx->vtag1_tci;
		}
	}

	if (flag & NIX_RX_OFFLOAD_MARK_UPDATE_F)
		ol_flags = nix_update_match_id(rx->match_id, ol_flags, mbuf);

	/* Packet data length and ol flags is already updated for sec */
	if (flag & NIX_RX_SEC_REASSEMBLY_F && w1 & BIT_ULL(11)) {
		mbuf->ol_flags |= ol_flags;
	} else {
		mbuf->ol_flags = ol_flags;
		mbuf->pkt_len = len;
		mbuf->data_len = len;
		p = (uintptr_t)&mbuf->rearm_data;
		*(uint64_t *)p = val;
	}

	if (flag & NIX_RX_MULTI_SEG_F)
		/*
		 * For multi segment packets, mbuf length correction according
		 * to Rx timestamp length will be handled later during
		 * timestamp data process.
		 * Hence, timestamp flag argument is not required.
		 */
		nix_cqe_xtract_mseg(rx, mbuf, val, cpth, sa_base, flag & ~NIX_RX_OFFLOAD_TSTAMP_F);
}

static inline uint16_t
nix_rx_nb_pkts(struct cn10k_eth_rxq *rxq, const uint64_t wdata,
	       const uint16_t pkts, const uint32_t qmask)
{
	uint32_t available = rxq->available;

	/* Update the available count if cached value is not enough */
	if (unlikely(available < pkts)) {
		uint64_t reg, head, tail;

		/* Use LDADDA version to avoid reorder */
		reg = roc_atomic64_add_sync(wdata, rxq->cq_status);
		/* CQ_OP_STATUS operation error */
		if (reg & BIT_ULL(NIX_CQ_OP_STAT_OP_ERR) ||
		    reg & BIT_ULL(NIX_CQ_OP_STAT_CQ_ERR))
			return 0;

		tail = reg & 0xFFFFF;
		head = (reg >> 20) & 0xFFFFF;
		if (tail < head)
			available = tail - head + qmask + 1;
		else
			available = tail - head;

		rxq->available = available;
	}

	return RTE_MIN(pkts, available);
}

static __rte_always_inline void
cn10k_nix_mbuf_to_tstamp(struct rte_mbuf *mbuf,
			struct cnxk_timesync_info *tstamp,
			const uint8_t ts_enable, uint64_t *tstamp_ptr)
{
	if (ts_enable) {
		mbuf->pkt_len -= CNXK_NIX_TIMESYNC_RX_OFFSET;
		mbuf->data_len -= CNXK_NIX_TIMESYNC_RX_OFFSET;

		/* Reading the rx timestamp inserted by CGX, viz at
		 * starting of the packet data.
		 */
		*tstamp_ptr = ((*tstamp_ptr >> 32) * NSEC_PER_SEC) +
			(*tstamp_ptr & 0xFFFFFFFFUL);
		*cnxk_nix_timestamp_dynfield(mbuf, tstamp) =
			rte_be_to_cpu_64(*tstamp_ptr);
		/* RTE_MBUF_F_RX_IEEE1588_TMST flag needs to be set only in case
		 * PTP packets are received.
		 */
		if (mbuf->packet_type == RTE_PTYPE_L2_ETHER_TIMESYNC) {
			tstamp->rx_tstamp =
				*cnxk_nix_timestamp_dynfield(mbuf, tstamp);
			tstamp->rx_ready = 1;
			mbuf->ol_flags |= RTE_MBUF_F_RX_IEEE1588_PTP |
				RTE_MBUF_F_RX_IEEE1588_TMST |
				tstamp->rx_tstamp_dynflag;
		}
	}
}

static __rte_always_inline uint16_t
cn10k_nix_recv_pkts(void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t pkts,
		    const uint16_t flags)
{
	struct cn10k_eth_rxq *rxq = rx_queue;
	const uint64_t mbuf_init = rxq->mbuf_initializer;
	const void *lookup_mem = rxq->lookup_mem;
	const uint64_t data_off = rxq->data_off;
	struct rte_mempool *meta_pool = NULL;
	const uintptr_t desc = rxq->desc;
	const uint64_t wdata = rxq->wdata;
	const uint32_t qmask = rxq->qmask;
	uint64_t lbase = rxq->lmt_base;
	uint16_t packets = 0, nb_pkts;
	uint8_t loff = 0, lnum = 0;
	uint32_t head = rxq->head;
	struct nix_cqe_hdr_s *cq;
	struct rte_mbuf *mbuf;
	uint64_t aura_handle;
	uint64_t sa_base = 0;
	uintptr_t cpth = 0;
	uint16_t lmt_id;
	uint64_t laddr;

	nb_pkts = nix_rx_nb_pkts(rxq, wdata, pkts, qmask);

	if (flags & NIX_RX_OFFLOAD_SECURITY_F) {
		aura_handle = rxq->meta_aura;
		sa_base = rxq->sa_base;
		sa_base &= ~(ROC_NIX_INL_SA_BASE_ALIGN - 1);
		ROC_LMT_BASE_ID_GET(lbase, lmt_id);
		laddr = lbase;
		laddr += 8;
		if (flags & NIX_RX_REAS_F)
			meta_pool = (struct rte_mempool *)rxq->meta_pool;
	}

	while (packets < nb_pkts) {
		/* Prefetch N desc ahead */
		rte_prefetch_non_temporal(
			(void *)(desc + (CQE_SZ((head + 2) & qmask))));
		cq = (struct nix_cqe_hdr_s *)(desc + CQE_SZ(head));

		mbuf = nix_get_mbuf_from_cqe(cq, data_off);

		/* Mark mempool obj as "get" as it is alloc'ed by NIX */
		RTE_MEMPOOL_CHECK_COOKIES(mbuf->pool, (void **)&mbuf, 1, 1);

		/* Translate meta to mbuf */
		if (flags & NIX_RX_OFFLOAD_SECURITY_F) {
			const uint64_t cq_w1 = *((const uint64_t *)cq + 1);
			const uint64_t cq_w5 = *((const uint64_t *)cq + 5);

			cpth = ((uintptr_t)mbuf + (uint16_t)data_off);

			/* Update mempool pointer for full mode pkt */
			if ((flags & NIX_RX_REAS_F) && (cq_w1 & BIT(11)) &&
			    !((*(uint64_t *)cpth) & BIT(15)))
				mbuf->pool = meta_pool;

			mbuf = nix_sec_meta_to_mbuf_sc(cq_w1, cq_w5, sa_base, laddr,
						       &loff, mbuf, data_off,
						       flags, mbuf_init);
		}

		cn10k_nix_cqe_to_mbuf(cq, cq->tag, mbuf, lookup_mem, mbuf_init,
				      cpth, sa_base, flags);
		cn10k_nix_mbuf_to_tstamp(mbuf, rxq->tstamp,
					(flags & NIX_RX_OFFLOAD_TSTAMP_F),
					(uint64_t *)((uint8_t *)mbuf
								+ data_off));
		rx_pkts[packets++] = mbuf;
		roc_prefetch_store_keep(mbuf);
		head++;
		head &= qmask;

		if (flags & NIX_RX_OFFLOAD_SECURITY_F) {
			/* Flush when we don't have space for 4 meta */
			if ((15 - loff) < 1) {
				nix_sec_flush_meta(laddr, lmt_id + lnum, loff,
						   aura_handle);
				lnum++;
				lnum &= BIT_ULL(ROC_LMT_LINES_PER_CORE_LOG2) -
					1;
				/* First pointer starts at 8B offset */
				laddr = (uintptr_t)LMT_OFF(lbase, lnum, 8);
				loff = 0;
			}
		}
	}

	rxq->head = head;
	rxq->available -= nb_pkts;

	/* Free all the CQs that we've processed */
	plt_write64((wdata | nb_pkts), rxq->cq_door);

	/* Free remaining meta buffers if any */
	if (flags & NIX_RX_OFFLOAD_SECURITY_F && loff)
		nix_sec_flush_meta(laddr, lmt_id + lnum, loff, aura_handle);

	if (flags & NIX_RX_OFFLOAD_SECURITY_F)
		rte_io_wmb();

	return nb_pkts;
}

static __rte_always_inline uint16_t
cn10k_nix_flush_recv_pkts(void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t pkts,
			  const uint16_t flags)
{
	struct cn10k_eth_rxq *rxq = rx_queue;
	const uint64_t mbuf_init = rxq->mbuf_initializer;
	const void *lookup_mem = rxq->lookup_mem;
	const uint64_t data_off = rxq->data_off;
	struct rte_mempool *meta_pool = NULL;
	const uint64_t wdata = rxq->wdata;
	const uint32_t qmask = rxq->qmask;
	const uintptr_t desc = rxq->desc;
	uint64_t lbase = rxq->lmt_base;
	uint16_t packets = 0, nb_pkts;
	uint16_t lmt_id __rte_unused;
	uint32_t head = rxq->head;
	struct nix_cqe_hdr_s *cq;
	struct rte_mbuf *mbuf;
	uint64_t sa_base = 0;
	uintptr_t cpth = 0;
	uint8_t loff = 0;
	uint64_t laddr;

	nb_pkts = nix_rx_nb_pkts(rxq, wdata, pkts, qmask);

	if (flags & NIX_RX_OFFLOAD_SECURITY_F) {
		sa_base = rxq->sa_base;
		sa_base &= ~(ROC_NIX_INL_SA_BASE_ALIGN - 1);
		ROC_LMT_BASE_ID_GET(lbase, lmt_id);
		laddr = lbase;
		laddr += 8;
		if (flags & NIX_RX_REAS_F)
			meta_pool = (struct rte_mempool *)rxq->meta_pool;
	}

	while (packets < nb_pkts) {
		/* Prefetch N desc ahead */
		rte_prefetch_non_temporal((void *)(desc + (CQE_SZ((head + 2) & qmask))));
		cq = (struct nix_cqe_hdr_s *)(desc + CQE_SZ(head));

		mbuf = nix_get_mbuf_from_cqe(cq, data_off);

		/* Mark mempool obj as "get" as it is alloc'ed by NIX */
		RTE_MEMPOOL_CHECK_COOKIES(mbuf->pool, (void **)&mbuf, 1, 1);

		/* Translate meta to mbuf */
		if (flags & NIX_RX_OFFLOAD_SECURITY_F) {
			const uint64_t cq_w1 = *((const uint64_t *)cq + 1);
			const uint64_t cq_w5 = *((const uint64_t *)cq + 5);
			struct rte_mbuf *meta_buf = mbuf;

			cpth = ((uintptr_t)meta_buf + (uint16_t)data_off);

			/* Update mempool pointer for full mode pkt */
			if ((flags & NIX_RX_REAS_F) && (cq_w1 & BIT(11)) &&
			    !((*(uint64_t *)cpth) & BIT(15)))
				meta_buf->pool = meta_pool;

			mbuf = nix_sec_meta_to_mbuf_sc(cq_w1, cq_w5, sa_base, laddr, &loff,
						       meta_buf, data_off, flags, mbuf_init);
			/* Free Meta mbuf, not use LMT line for flush as this will be called
			 * from non-datapath i.e. dev_stop case.
			 */
			if (loff) {
				roc_npa_aura_op_free(meta_buf->pool->pool_id, 0,
						     (uint64_t)meta_buf);
				loff = 0;
			}
		}

		cn10k_nix_cqe_to_mbuf(cq, cq->tag, mbuf, lookup_mem, mbuf_init,
				      cpth, sa_base, flags);
		cn10k_nix_mbuf_to_tstamp(mbuf, rxq->tstamp,
					(flags & NIX_RX_OFFLOAD_TSTAMP_F),
					(uint64_t *)((uint8_t *)mbuf + data_off));
		rx_pkts[packets++] = mbuf;
		roc_prefetch_store_keep(mbuf);
		head++;
		head &= qmask;
	}

	rxq->head = head;
	rxq->available -= nb_pkts;

	/* Free all the CQs that we've processed */
	plt_write64((wdata | nb_pkts), rxq->cq_door);

	if (flags & NIX_RX_OFFLOAD_SECURITY_F)
		rte_io_wmb();

	return nb_pkts;
}

#if defined(RTE_ARCH_ARM64)
static __rte_always_inline uint16_t
cn10k_nix_rx_inj_prepare_mseg(struct rte_mbuf *m, uint64_t *cmd)
{
	union nix_send_sg_s *sg, l_sg;
	struct rte_mbuf *m_next;
	uint16_t segdw, nb_segs;
	uint64_t len, dlen;
	uint64_t *slist;

	sg = (union nix_send_sg_s *)cmd;
	l_sg.u = sg->u;
	l_sg.u &= 0xC00000000000000; /* LD_TYPE */
	l_sg.subdc = NIX_SUBDC_SG;
	nb_segs = m->nb_segs;
	len = m->pkt_len;
	slist = &cmd[1];

	/* Fill mbuf segments */
	do {
		*slist = rte_pktmbuf_iova(m);
		dlen = m->data_len;
		len -= dlen;

		/* Set the segment length */
		l_sg.u |= ((uint64_t)dlen << (l_sg.segs << 4));
		l_sg.segs += 1;
		slist++;
		nb_segs--;
		if (l_sg.segs > 2 && nb_segs) {
			sg->u = l_sg.u;
			/* Next SG subdesc */
			sg = (union nix_send_sg_s *)slist;
			l_sg.u = sg->u;
			l_sg.u &= 0xC00000000000000; /* LD_TYPE */
			l_sg.subdc = NIX_SUBDC_SG;
			slist++;
		}
		m_next = m->next;
		m = m_next;
	} while (nb_segs);

	/* Add remaining bytes of data to last seg */
	if (len) {
		uint8_t shft = (l_sg.subdc == NIX_SUBDC_SG) ? ((l_sg.segs - 1) << 4) : 0;
		dlen = ((l_sg.u >> shft) & 0xFFFFULL) + len;
		l_sg.u = l_sg.u & ~(0xFFFFULL << shft);
		l_sg.u |= dlen << shft;
	}
	/* Write the last subdc out */
	sg->u = l_sg.u;

	segdw = (uint64_t *)slist - cmd;
	/* Roundup extra dwords to multiple of 2 */
	segdw = (segdw >> 1) + (segdw & 0x1);
	return segdw;
}

static __rte_always_inline uint16_t
cn10k_nix_inj_pkts(struct rte_security_session **sess, struct cnxk_ethdev_inj_cfg *inj_cfg,
		   struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
	uintptr_t c_lbase = inj_cfg->lmt_base;
	struct cn10k_sec_sess_priv sess_priv;
	uint64_t sa_base = inj_cfg->sa_base;
	uint16_t c_lmt_id, burst, left, i;
	uintptr_t cptres, rxphdr, dptr;
	struct rte_mbuf *m, *last;
	uint8_t lnum, shft, loff;
	uint64x2_t cmd01, cmd23;
	uint64_t ucode_cmd[4];
	rte_iova_t c_io_addr;
	uint64_t *laddr;
	uint64_t sa, w0;
	uint16_t segdw;

	/* Get LMT base address and LMT ID as lcore id */
	ROC_LMT_CPT_BASE_ID_GET(c_lbase, c_lmt_id);
	c_io_addr = inj_cfg->io_addr;

	left = nb_pkts;
again:
	burst = left > 32 ? 32 : left;

	lnum = 0;
	loff = 0;
	shft = 16;

	for (i = 0; i < burst; i++) {
		m = tx_pkts[i];
		sess_priv.u64 = sess[i]->fast_mdata;
		last = rte_pktmbuf_lastseg(m);

		cptres = rte_pktmbuf_mtod_offset(last, uintptr_t, last->data_len);
		cptres += BIT_ULL(7);
		cptres = (cptres - 1) & ~(BIT_ULL(7) - 1);

		if (m->nb_segs > 1) {
			/* Will reserve NIX Rx descriptor with SG list after end of
			 * last mbuf data location. and pointer to this will be
			 * stored at 1st mbuf space for Rx path multi-seg processing.
			 */
			/* Pointer to WQE header */
			*(uint64_t *)(m + 1) = cptres;
			/* Reserve 8 Dwords of WQE Hdr + Rx Parse Hdr */
			rxphdr = cptres + 8;
			dptr = rxphdr + 7 * 8;
			/* Prepare Multiseg SG list */
			segdw = cn10k_nix_rx_inj_prepare_mseg(m, (uint64_t *)(dptr));
			*(uint64_t *)rxphdr = (uint64_t)(segdw - 1) << 12;
			cptres += 64 + segdw * 16;
			ucode_cmd[1] = dptr | ((uint64_t)m->nb_segs << 60);
		} else {
			dptr = (uint64_t)rte_pktmbuf_iova(m);
			ucode_cmd[1] = dptr;
		}

		/* Prepare CPT instruction */
		/* CPT word 0 and 1 */
		cmd01 = vdupq_n_u64(0);
		w0 = inj_cfg->cmd_w0 | ((uint64_t)m->l2_len - 2) << 24 | (uint64_t)m->l2_len << 16;
		cmd01 = vsetq_lane_u64(w0, cmd01, 0);
		cmd01 = vsetq_lane_u64(cptres, cmd01, 1);

		/* CPT word 2 and 3 */
		cmd23 = vdupq_n_u64(0);
		/* Set PF func */
		w0 &= 0xFFFF000000000000UL;
		cmd23 = vsetq_lane_u64(w0, cmd23, 0);
		cmd23 = vsetq_lane_u64(((uint64_t)m + sizeof(struct rte_mbuf)) | 1, cmd23, 1);

		sa_base &= ~0xFFFFUL;
		sa = (uintptr_t)roc_nix_inl_ot_ipsec_inb_sa(sa_base, sess_priv.sa_idx);
		ucode_cmd[0] = (ROC_IE_OT_MAJOR_OP_PROCESS_INBOUND_IPSEC << 48 | 1UL << 54 |
				((uint64_t)sess_priv.chksum) << 32 |
				((uint64_t)sess_priv.dec_ttl) << 34 | m->pkt_len);

		ucode_cmd[2] = 0;
		ucode_cmd[3] = (ROC_LEGACY_CPT_DFLT_ENG_GRP_SE_IE << 61 | 1UL << 60 | sa);

		/* Move to our line */
		laddr = LMT_OFF(c_lbase, lnum, loff ? 64 : 0);

		/* Write CPT instruction to lmt line */
		vst1q_u64(laddr, cmd01);
		vst1q_u64((laddr + 2), cmd23);

		*(__uint128_t *)(laddr + 4) = *(__uint128_t *)ucode_cmd;
		*(__uint128_t *)(laddr + 6) = *(__uint128_t *)(ucode_cmd + 2);

		loff = !loff;
		lnum = lnum + (loff ? 0 : 1);
		shft = shft + (loff ? 0 : 3);
	}

	left -= burst;
	tx_pkts += burst;
	sess += burst;

	cn10k_nix_sec_steorl(c_io_addr, c_lmt_id, lnum, loff, shft);

	rte_io_wmb();
	if (left)
		goto again;

	return nb_pkts;
}
#else
static __rte_always_inline uint16_t
cn10k_nix_inj_pkts(struct rte_security_session **sess, struct cnxk_ethdev_inj_cfg *inj_cfg,
		   struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
	RTE_SET_USED(sess);
	RTE_SET_USED(inj_cfg);
	RTE_SET_USED(tx_pkts);
	RTE_SET_USED(nb_pkts);
	return 0;
}
#endif

#if defined(RTE_ARCH_ARM64)

static __rte_always_inline uint64_t
nix_vlan_update(const uint64_t w2, uint64_t ol_flags, uint8x16_t *f)
{
	if (w2 & BIT_ULL(21) /* vtag0_gone */) {
		ol_flags |= RTE_MBUF_F_RX_VLAN | RTE_MBUF_F_RX_VLAN_STRIPPED;
		*f = vsetq_lane_u16((uint16_t)(w2 >> 32), *f, 5);
	}

	return ol_flags;
}

static __rte_always_inline uint64_t
nix_qinq_update(const uint64_t w2, uint64_t ol_flags, struct rte_mbuf *mbuf)
{
	if (w2 & BIT_ULL(23) /* vtag1_gone */) {
		ol_flags |= RTE_MBUF_F_RX_QINQ | RTE_MBUF_F_RX_QINQ_STRIPPED;
		mbuf->vlan_tci_outer = (uint16_t)(w2 >> 48);
	}

	return ol_flags;
}

#define NIX_PUSH_META_TO_FREE(_mbuf, _laddr, _loff_p)                          \
	do {                                                                   \
		*(uint64_t *)((_laddr) + (*(_loff_p) << 3)) = (uint64_t)_mbuf; \
		*(_loff_p) = *(_loff_p) + 1;                                   \
		/* Mark meta mbuf as put */                                    \
		RTE_MEMPOOL_CHECK_COOKIES(_mbuf->pool, (void **)&_mbuf, 1, 0); \
	} while (0)

static __rte_always_inline uint16_t
cn10k_nix_recv_pkts_vector(void *args, struct rte_mbuf **mbufs, uint16_t pkts,
			   const uint16_t flags, void *lookup_mem,
			   struct cnxk_timesync_info *tstamp,
			   uintptr_t lmt_base, uint64_t meta_aura)
{
	struct cn10k_eth_rxq *rxq = args;
	const uint64_t mbuf_initializer = (flags & NIX_RX_VWQE_F) ?
							*(uint64_t *)args :
							rxq->mbuf_initializer;
	const uint64x2_t data_off = flags & NIX_RX_VWQE_F ?
					vdupq_n_u64(RTE_PKTMBUF_HEADROOM) :
					vdupq_n_u64(rxq->data_off);
	const uint32_t qmask = flags & NIX_RX_VWQE_F ? 0 : rxq->qmask;
	const uint64_t wdata = flags & NIX_RX_VWQE_F ? 0 : rxq->wdata;
	const uintptr_t desc = flags & NIX_RX_VWQE_F ? 0 : rxq->desc;
	uint64x2_t cq0_w8, cq1_w8, cq2_w8, cq3_w8, mbuf01, mbuf23;
	uintptr_t cpth0 = 0, cpth1 = 0, cpth2 = 0, cpth3 = 0;
	uint64_t ol_flags0, ol_flags1, ol_flags2, ol_flags3;
	uint64x2_t rearm0 = vdupq_n_u64(mbuf_initializer);
	uint64x2_t rearm1 = vdupq_n_u64(mbuf_initializer);
	uint64x2_t rearm2 = vdupq_n_u64(mbuf_initializer);
	uint64x2_t rearm3 = vdupq_n_u64(mbuf_initializer);
	struct rte_mbuf *mbuf0, *mbuf1, *mbuf2, *mbuf3;
	uint8_t loff = 0, lnum = 0, shft = 0;
	struct rte_mempool *meta_pool = NULL;
	uint8x16_t f0, f1, f2, f3;
	uint16_t lmt_id, d_off;
	uint64_t lbase, laddr;
	uintptr_t sa_base = 0;
	uint16_t packets = 0;
	uint16_t pkts_left;
	uint32_t head;
	uintptr_t cq0;

	if (!(flags & NIX_RX_VWQE_F)) {
		lookup_mem = rxq->lookup_mem;
		head = rxq->head;

		pkts = nix_rx_nb_pkts(rxq, wdata, pkts, qmask);
		pkts_left = pkts & (NIX_DESCS_PER_LOOP - 1);
		/* Packets has to be floor-aligned to NIX_DESCS_PER_LOOP */
		pkts = RTE_ALIGN_FLOOR(pkts, NIX_DESCS_PER_LOOP);
		if (flags & NIX_RX_OFFLOAD_TSTAMP_F)
			tstamp = rxq->tstamp;

		cq0 = desc + CQE_SZ(head);
		rte_prefetch0(CQE_PTR_OFF(cq0, 0, 64, flags));
		rte_prefetch0(CQE_PTR_OFF(cq0, 1, 64, flags));
		rte_prefetch0(CQE_PTR_OFF(cq0, 2, 64, flags));
		rte_prefetch0(CQE_PTR_OFF(cq0, 3, 64, flags));
	} else {
		RTE_SET_USED(head);
	}

	if (flags & NIX_RX_OFFLOAD_SECURITY_F) {
		if (flags & NIX_RX_VWQE_F) {
			uint64_t sg_w1;
			uint16_t port;

			mbuf0 = (struct rte_mbuf *)((uintptr_t)mbufs[0] -
						    sizeof(struct rte_mbuf));
			/* Pick first mbuf's aura handle assuming all
			 * mbufs are from a vec and are from same RQ.
			 */
			if (!meta_aura)
				meta_aura = mbuf0->pool->pool_id;
			/* Calculate offset from mbuf to actual data area */
			/* Zero aura's first skip i.e mbuf setup might not match the actual
			 * offset as first skip is taken from second pass RQ. So compute
			 * using diff b/w first SG pointer and mbuf addr.
			 */
			sg_w1 = *(uint64_t *)((uintptr_t)mbufs[0] + 72);
			d_off = (sg_w1 - (uint64_t)mbuf0);

			/* Get SA Base from lookup tbl using port_id */
			port = mbuf_initializer >> 48;
			sa_base = cnxk_nix_sa_base_get(port, lookup_mem);
			if (flags & NIX_RX_REAS_F)
				meta_pool = (struct rte_mempool *)cnxk_nix_inl_metapool_get(port,
											lookup_mem);

			lbase = lmt_base;
		} else {
			meta_aura = rxq->meta_aura;
			d_off = rxq->data_off;
			sa_base = rxq->sa_base;
			lbase = rxq->lmt_base;
			if (flags & NIX_RX_REAS_F)
				meta_pool = (struct rte_mempool *)rxq->meta_pool;
		}
		sa_base &= ~(ROC_NIX_INL_SA_BASE_ALIGN - 1);
		ROC_LMT_BASE_ID_GET(lbase, lmt_id);
		lnum = 0;
		laddr = lbase;
		laddr += 8;
	}

	while (packets < pkts) {
		if (!(flags & NIX_RX_VWQE_F)) {
			/* Exit loop if head is about to wrap and become
			 * unaligned.
			 */
			if (((head + NIX_DESCS_PER_LOOP - 1) & qmask) <
			    NIX_DESCS_PER_LOOP) {
				pkts_left += (pkts - packets);
				break;
			}

			cq0 = desc + CQE_SZ(head);
		} else {
			cq0 = (uintptr_t)&mbufs[packets];
		}

		if (flags & NIX_RX_VWQE_F) {
			if (pkts - packets > 4) {
				rte_prefetch_non_temporal(CQE_PTR_OFF(cq0,
					4, 0, flags));
				rte_prefetch_non_temporal(CQE_PTR_OFF(cq0,
					5, 0, flags));
				rte_prefetch_non_temporal(CQE_PTR_OFF(cq0,
					6, 0, flags));
				rte_prefetch_non_temporal(CQE_PTR_OFF(cq0,
					7, 0, flags));

				if (likely(pkts - packets > 8)) {
					rte_prefetch1(CQE_PTR_OFF(cq0,
						8, 0, flags));
					rte_prefetch1(CQE_PTR_OFF(cq0,
						9, 0, flags));
					rte_prefetch1(CQE_PTR_OFF(cq0,
						10, 0, flags));
					rte_prefetch1(CQE_PTR_OFF(cq0,
						11, 0, flags));
					if (pkts - packets > 12) {
						rte_prefetch1(CQE_PTR_OFF(cq0,
							12, 0, flags));
						rte_prefetch1(CQE_PTR_OFF(cq0,
							13, 0, flags));
						rte_prefetch1(CQE_PTR_OFF(cq0,
							14, 0, flags));
						rte_prefetch1(CQE_PTR_OFF(cq0,
							15, 0, flags));
					}
				}

				rte_prefetch0(CQE_PTR_DIFF(cq0,
					4, RTE_PKTMBUF_HEADROOM, flags));
				rte_prefetch0(CQE_PTR_DIFF(cq0,
					5, RTE_PKTMBUF_HEADROOM, flags));
				rte_prefetch0(CQE_PTR_DIFF(cq0,
					6, RTE_PKTMBUF_HEADROOM, flags));
				rte_prefetch0(CQE_PTR_DIFF(cq0,
					7, RTE_PKTMBUF_HEADROOM, flags));

				if (likely(pkts - packets > 8)) {
					rte_prefetch0(CQE_PTR_DIFF(cq0,
						8, RTE_PKTMBUF_HEADROOM, flags));
					rte_prefetch0(CQE_PTR_DIFF(cq0,
						9, RTE_PKTMBUF_HEADROOM, flags));
					rte_prefetch0(CQE_PTR_DIFF(cq0,
						10, RTE_PKTMBUF_HEADROOM, flags));
					rte_prefetch0(CQE_PTR_DIFF(cq0,
						11, RTE_PKTMBUF_HEADROOM, flags));
				}
			}
		} else {
			if (flags & NIX_RX_OFFLOAD_SECURITY_F &&
			    pkts - packets > 4) {
				/* Fetch cpt parse header */
				void *p0 =
					(void *)*CQE_PTR_OFF(cq0, 4, 72, flags);
				void *p1 =
					(void *)*CQE_PTR_OFF(cq0, 5, 72, flags);
				void *p2 =
					(void *)*CQE_PTR_OFF(cq0, 6, 72, flags);
				void *p3 =
					(void *)*CQE_PTR_OFF(cq0, 7, 72, flags);
				rte_prefetch0(p0);
				rte_prefetch0(p1);
				rte_prefetch0(p2);
				rte_prefetch0(p3);
			}

			if (pkts - packets > 8) {
				if (flags) {
					rte_prefetch0(CQE_PTR_OFF(cq0, 8, 0, flags));
					rte_prefetch0(CQE_PTR_OFF(cq0, 9, 0, flags));
					rte_prefetch0(CQE_PTR_OFF(cq0, 10, 0, flags));
					rte_prefetch0(CQE_PTR_OFF(cq0, 11, 0, flags));
				}
				rte_prefetch0(CQE_PTR_OFF(cq0, 8, 64, flags));
				rte_prefetch0(CQE_PTR_OFF(cq0, 9, 64, flags));
				rte_prefetch0(CQE_PTR_OFF(cq0, 10, 64, flags));
				rte_prefetch0(CQE_PTR_OFF(cq0, 11, 64, flags));
			}
		}

		if (!(flags & NIX_RX_VWQE_F)) {
			/* Get NIX_RX_SG_S for size and buffer pointer */
			cq0_w8 = vld1q_u64(CQE_PTR_OFF(cq0, 0, 64, flags));
			cq1_w8 = vld1q_u64(CQE_PTR_OFF(cq0, 1, 64, flags));
			cq2_w8 = vld1q_u64(CQE_PTR_OFF(cq0, 2, 64, flags));
			cq3_w8 = vld1q_u64(CQE_PTR_OFF(cq0, 3, 64, flags));

			/* Extract mbuf from NIX_RX_SG_S */
			mbuf01 = vzip2q_u64(cq0_w8, cq1_w8);
			mbuf23 = vzip2q_u64(cq2_w8, cq3_w8);
			mbuf01 = vqsubq_u64(mbuf01, data_off);
			mbuf23 = vqsubq_u64(mbuf23, data_off);
		} else {
			mbuf01 =
				vsubq_u64(vld1q_u64((uint64_t *)cq0),
					  vdupq_n_u64(sizeof(struct rte_mbuf)));
			mbuf23 =
				vsubq_u64(vld1q_u64((uint64_t *)(cq0 + 16)),
					  vdupq_n_u64(sizeof(struct rte_mbuf)));
		}

		/* Move mbufs to scalar registers for future use */
		mbuf0 = (struct rte_mbuf *)vgetq_lane_u64(mbuf01, 0);
		mbuf1 = (struct rte_mbuf *)vgetq_lane_u64(mbuf01, 1);
		mbuf2 = (struct rte_mbuf *)vgetq_lane_u64(mbuf23, 0);
		mbuf3 = (struct rte_mbuf *)vgetq_lane_u64(mbuf23, 1);

		/* Mark mempool obj as "get" as it is alloc'ed by NIX */
		RTE_MEMPOOL_CHECK_COOKIES(mbuf0->pool, (void **)&mbuf0, 1, 1);
		RTE_MEMPOOL_CHECK_COOKIES(mbuf1->pool, (void **)&mbuf1, 1, 1);
		RTE_MEMPOOL_CHECK_COOKIES(mbuf2->pool, (void **)&mbuf2, 1, 1);
		RTE_MEMPOOL_CHECK_COOKIES(mbuf3->pool, (void **)&mbuf3, 1, 1);

		if (!(flags & NIX_RX_VWQE_F)) {
			/* Mask to get packet len from NIX_RX_SG_S */
			const uint8x16_t shuf_msk = {
				0xFF, 0xFF, /* pkt_type set as unknown */
				0xFF, 0xFF, /* pkt_type set as unknown */
				0,    1,    /* octet 1~0, low 16 bits pkt_len */
				0xFF, 0xFF, /* skip high 16it pkt_len, zero out */
				0,    1,    /* octet 1~0, 16 bits data_len */
				0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

			/* Form the rx_descriptor_fields1 with pkt_len and data_len */
			f0 = vqtbl1q_u8(cq0_w8, shuf_msk);
			f1 = vqtbl1q_u8(cq1_w8, shuf_msk);
			f2 = vqtbl1q_u8(cq2_w8, shuf_msk);
			f3 = vqtbl1q_u8(cq3_w8, shuf_msk);
		}

		/* Load CQE word0 and word 1 */
		const uint64_t cq0_w0 = *CQE_PTR_OFF(cq0, 0, 0, flags);
		const uint64_t cq0_w1 = *CQE_PTR_OFF(cq0, 0, 8, flags);
		const uint64_t cq0_w2 = *CQE_PTR_OFF(cq0, 0, 16, flags);
		const uint64_t cq1_w0 = *CQE_PTR_OFF(cq0, 1, 0, flags);
		const uint64_t cq1_w1 = *CQE_PTR_OFF(cq0, 1, 8, flags);
		const uint64_t cq1_w2 = *CQE_PTR_OFF(cq0, 1, 16, flags);
		const uint64_t cq2_w0 = *CQE_PTR_OFF(cq0, 2, 0, flags);
		const uint64_t cq2_w1 = *CQE_PTR_OFF(cq0, 2, 8, flags);
		const uint64_t cq2_w2 = *CQE_PTR_OFF(cq0, 2, 16, flags);
		const uint64_t cq3_w0 = *CQE_PTR_OFF(cq0, 3, 0, flags);
		const uint64_t cq3_w1 = *CQE_PTR_OFF(cq0, 3, 8, flags);
		const uint64_t cq3_w2 = *CQE_PTR_OFF(cq0, 3, 16, flags);

		if (flags & NIX_RX_VWQE_F) {
			uint16_t psize0, psize1, psize2, psize3;

			psize0 = (cq0_w2 & 0xFFFF) + 1;
			psize1 = (cq1_w2 & 0xFFFF) + 1;
			psize2 = (cq2_w2 & 0xFFFF) + 1;
			psize3 = (cq3_w2 & 0xFFFF) + 1;

			f0 = vdupq_n_u64(0);
			f1 = vdupq_n_u64(0);
			f2 = vdupq_n_u64(0);
			f3 = vdupq_n_u64(0);

			f0 = vsetq_lane_u16(psize0, f0, 2);
			f0 = vsetq_lane_u16(psize0, f0, 4);

			f1 = vsetq_lane_u16(psize1, f1, 2);
			f1 = vsetq_lane_u16(psize1, f1, 4);

			f2 = vsetq_lane_u16(psize2, f2, 2);
			f2 = vsetq_lane_u16(psize2, f2, 4);

			f3 = vsetq_lane_u16(psize3, f3, 2);
			f3 = vsetq_lane_u16(psize3, f3, 4);
		}

		if (flags & NIX_RX_OFFLOAD_RSS_F) {
			/* Fill rss in the rx_descriptor_fields1 */
			f0 = vsetq_lane_u32(cq0_w0, f0, 3);
			f1 = vsetq_lane_u32(cq1_w0, f1, 3);
			f2 = vsetq_lane_u32(cq2_w0, f2, 3);
			f3 = vsetq_lane_u32(cq3_w0, f3, 3);
			ol_flags0 = RTE_MBUF_F_RX_RSS_HASH;
			ol_flags1 = RTE_MBUF_F_RX_RSS_HASH;
			ol_flags2 = RTE_MBUF_F_RX_RSS_HASH;
			ol_flags3 = RTE_MBUF_F_RX_RSS_HASH;
		} else {
			ol_flags0 = 0;
			ol_flags1 = 0;
			ol_flags2 = 0;
			ol_flags3 = 0;
		}

		if (flags & NIX_RX_OFFLOAD_PTYPE_F) {
			/* Fill packet_type in the rx_descriptor_fields1 */
			f0 = vsetq_lane_u32(nix_ptype_get(lookup_mem, cq0_w1),
					    f0, 0);
			f1 = vsetq_lane_u32(nix_ptype_get(lookup_mem, cq1_w1),
					    f1, 0);
			f2 = vsetq_lane_u32(nix_ptype_get(lookup_mem, cq2_w1),
					    f2, 0);
			f3 = vsetq_lane_u32(nix_ptype_get(lookup_mem, cq3_w1),
					    f3, 0);
		}

		if (flags & NIX_RX_OFFLOAD_CHECKSUM_F) {
			ol_flags0 |= (uint64_t)nix_rx_olflags_get(lookup_mem, cq0_w1);
			ol_flags1 |= (uint64_t)nix_rx_olflags_get(lookup_mem, cq1_w1);
			ol_flags2 |= (uint64_t)nix_rx_olflags_get(lookup_mem, cq2_w1);
			ol_flags3 |= (uint64_t)nix_rx_olflags_get(lookup_mem, cq3_w1);
		}

		/* Translate meta to mbuf */
		if (flags & NIX_RX_OFFLOAD_SECURITY_F) {
			uint64_t cq0_w5 = *CQE_PTR_OFF(cq0, 0, 40, flags);
			uint64_t cq1_w5 = *CQE_PTR_OFF(cq0, 1, 40, flags);
			uint64_t cq2_w5 = *CQE_PTR_OFF(cq0, 2, 40, flags);
			uint64_t cq3_w5 = *CQE_PTR_OFF(cq0, 3, 40, flags);
			uint8_t code;

			uint64x2_t inner0, inner1, inner2, inner3;
			uint64x2_t wqe01, wqe23, sa01, sa23;
			uint16x4_t lens, l2lens, ltypes;
			uint64x2_t mask01, mask23;
			uint8x8_t ucc;

			cpth0 = (uintptr_t)mbuf0 + d_off;
			cpth1 = (uintptr_t)mbuf1 + d_off;
			cpth2 = (uintptr_t)mbuf2 + d_off;
			cpth3 = (uintptr_t)mbuf3 + d_off;

			inner0 = vld1q_u64((const uint64_t *)cpth0);
			inner1 = vld1q_u64((const uint64_t *)cpth1);
			inner2 = vld1q_u64((const uint64_t *)cpth2);
			inner3 = vld1q_u64((const uint64_t *)cpth3);

			/* Extract and reverse wqe pointers */
			wqe01 = vzip2q_u64(inner0, inner1);
			wqe23 = vzip2q_u64(inner2, inner3);
			wqe01 = vrev64q_u8(wqe01);
			wqe23 = vrev64q_u8(wqe23);
			/* Adjust wqe pointers to point to mbuf */
			wqe01 = vsubq_u64(wqe01,
					  vdupq_n_u64(sizeof(struct rte_mbuf)));
			wqe23 = vsubq_u64(wqe23,
					  vdupq_n_u64(sizeof(struct rte_mbuf)));

			/* Extract sa idx from cookie area and add to sa_base */
			sa01 = vzip1q_u64(inner0, inner1);
			sa23 = vzip1q_u64(inner2, inner3);

			sa01 = vshrq_n_u64(sa01, 32);
			sa23 = vshrq_n_u64(sa23, 32);

			/* Crypto Look-aside Rx Inject case */
			mask01 = vceqq_u64(sa01, vdupq_n_u64(0xFFFFFFFF));
			mask23 = vceqq_u64(sa23, vdupq_n_u64(0xFFFFFFFF));

			sa01 = vshlq_n_u64(sa01,
					   ROC_NIX_INL_OT_IPSEC_INB_SA_SZ_LOG2);
			sa23 = vshlq_n_u64(sa23,
					   ROC_NIX_INL_OT_IPSEC_INB_SA_SZ_LOG2);
			sa01 = vaddq_u64(sa01, vdupq_n_u64(sa_base));
			sa23 = vaddq_u64(sa23, vdupq_n_u64(sa_base));

			if (flags & NIX_RX_REAS_F) {
				sa01 = vbicq_u64(sa01, mask01);
				sa23 = vbicq_u64(sa23, mask23);
			}

			const uint8x16x2_t tbl = {{
				{
					/* ROC_IE_OT_UCC_SUCCESS_PKT_IP_BADCSUM */
					RTE_MBUF_F_RX_IP_CKSUM_BAD >> 1,
					/* ROC_IE_OT_UCC_SUCCESS_PKT_L4_GOODCSUM */
					(RTE_MBUF_F_RX_IP_CKSUM_GOOD |
					 RTE_MBUF_F_RX_L4_CKSUM_GOOD) >> 1,
					/* ROC_IE_OT_UCC_SUCCESS_PKT_L4_BADCSUM */
					(RTE_MBUF_F_RX_IP_CKSUM_GOOD |
					 RTE_MBUF_F_RX_L4_CKSUM_BAD) >> 1,
					1,
					/* ROC_IE_OT_UCC_SUCCESS_PKT_UDPESP_NZCSUM */
					(RTE_MBUF_F_RX_IP_CKSUM_GOOD |
					 RTE_MBUF_F_RX_L4_CKSUM_GOOD) >> 1,
					1,
					/* ROC_IE_OT_UCC_SUCCESS_PKT_UDP_ZEROCSUM */
					(RTE_MBUF_F_RX_IP_CKSUM_GOOD |
					RTE_MBUF_F_RX_L4_CKSUM_GOOD) >> 1,
					3, 1, 3, 3, 3, 3, 1, 3, 1,
				},
				{
					1, 1, 1,
					/* ROC_IE_OT_UCC_SUCCESS_PKT_IP_GOODCSUM */
					RTE_MBUF_F_RX_IP_CKSUM_GOOD >> 1,
					/* Rest 0 to indicate RTE_MBUF_F_RX_SEC_OFFLOAD_FAILED */
					0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
				},
			}};

			const uint8x8_t err_off = {
				/* UCC */
				0xED,
				/* HW_CCODE 0:6 -> 7:D */
				-7,
				0xED,
				-7,
				0xED,
				-7,
				0xED,
				-7,
			};

			ucc = vdup_n_u8(0);
			ucc = vset_lane_u16(*(uint16_t *)(cpth0 + 30), ucc, 0);
			ucc = vset_lane_u16(*(uint16_t *)(cpth1 + 30), ucc, 1);
			ucc = vset_lane_u16(*(uint16_t *)(cpth2 + 30), ucc, 2);
			ucc = vset_lane_u16(*(uint16_t *)(cpth3 + 30), ucc, 3);
			ucc = vsub_u8(ucc, err_off);

			/* Table lookup to get the corresponding flags, Out of the range
			 * from this lookup will have value 0 and consider as
			 * RTE_MBUF_F_RX_SEC_OFFLOAD_FAILED.
			 */
			ucc = vqtbl2_u8(tbl, ucc);

			RTE_BUILD_BUG_ON(NPC_LT_LC_IP != 2);
			RTE_BUILD_BUG_ON(NPC_LT_LC_IP_OPT != 3);
			RTE_BUILD_BUG_ON(NPC_LT_LC_IP6 != 4);
			RTE_BUILD_BUG_ON(NPC_LT_LC_IP6_EXT != 5);

			ltypes = vdup_n_u16(0);
			ltypes = vset_lane_u16((cq0_w1 >> 40) & 0x6, ltypes, 0);
			ltypes = vset_lane_u16((cq1_w1 >> 40) & 0x6, ltypes, 1);
			ltypes = vset_lane_u16((cq2_w1 >> 40) & 0x6, ltypes, 2);
			ltypes = vset_lane_u16((cq3_w1 >> 40) & 0x6, ltypes, 3);

			/* Extract and reverse l3 length from IPv4/IPv6 hdr
			 * that is in same cacheline most probably as cpth.
			 */
			cpth0 += ((cq0_w5 >> 16) & 0xFF) +
				 vget_lane_u16(ltypes, 0);
			cpth1 += ((cq1_w5 >> 16) & 0xFF) +
				 vget_lane_u16(ltypes, 1);
			cpth2 += ((cq2_w5 >> 16) & 0xFF) +
				 vget_lane_u16(ltypes, 2);
			cpth3 += ((cq3_w5 >> 16) & 0xFF) +
				 vget_lane_u16(ltypes, 3);
			lens = vdup_n_u16(0);
			lens = vset_lane_u16(*(uint16_t *)cpth0, lens, 0);
			lens = vset_lane_u16(*(uint16_t *)cpth1, lens, 1);
			lens = vset_lane_u16(*(uint16_t *)cpth2, lens, 2);
			lens = vset_lane_u16(*(uint16_t *)cpth3, lens, 3);
			lens = vrev16_u8(lens);

			/* Add l2 length to l3 lengths */
			l2lens = vdup_n_u16(0);
			l2lens = vset_lane_u16(((cq0_w5 >> 16) & 0xFF) -
						       (cq0_w5 & 0xFF),
					       l2lens, 0);
			l2lens = vset_lane_u16(((cq1_w5 >> 16) & 0xFF) -
						       (cq1_w5 & 0xFF),
					       l2lens, 1);
			l2lens = vset_lane_u16(((cq2_w5 >> 16) & 0xFF) -
						       (cq2_w5 & 0xFF),
					       l2lens, 2);
			l2lens = vset_lane_u16(((cq3_w5 >> 16) & 0xFF) -
						       (cq3_w5 & 0xFF),
					       l2lens, 3);
			lens = vadd_u16(lens, l2lens);

			/* L3 header adjust */
			const int8x8_t l3adj = {
				0, 0, 0, 0, 40, 0, 0, 0,
			};
			lens = vadd_u16(lens, vtbl1_u8(l3adj, ltypes));

			/* Initialize rearm data when reassembly is enabled as
			 * data offset might change.
			 */
			if (flags & NIX_RX_REAS_F) {
				rearm0 = vdupq_n_u64(mbuf_initializer);
				rearm1 = vdupq_n_u64(mbuf_initializer);
				rearm2 = vdupq_n_u64(mbuf_initializer);
				rearm3 = vdupq_n_u64(mbuf_initializer);
			}

			/* Checksum ol_flags will be cleared if mbuf is meta */
			if (cq0_w1 & BIT(11)) {
				uintptr_t wqe = vgetq_lane_u64(wqe01, 0);
				uintptr_t sa = vgetq_lane_u64(sa01, 0);
				uint16_t len = vget_lane_u16(lens, 0);

				cpth0 = (uintptr_t)mbuf0 + d_off;

				/* Free meta to aura */
				if (!(flags & NIX_RX_REAS_F) ||
				    *(uint64_t *)cpth0 & BIT_ULL(15)) {
					/* Free meta to aura */
					NIX_PUSH_META_TO_FREE(mbuf0, laddr,
							      &loff);
					mbuf01 = vsetq_lane_u64(wqe, mbuf01, 0);
					mbuf0 = (struct rte_mbuf *)wqe;
				} else if (flags & NIX_RX_REAS_F) {
					/* Update meta pool for full mode pkts */
					mbuf0->pool = meta_pool;
				}

				/* Update pkt_len and data_len */
				f0 = vsetq_lane_u16(len, f0, 2);
				f0 = vsetq_lane_u16(len, f0, 4);

				nix_sec_meta_to_mbuf(cq0_w1, cq0_w5, sa, cpth0,
						     mbuf0, &f0, &ol_flags0,
						     flags, &rearm0);
				code = vget_lane_u8(ucc, 0);
				ol_flags0 |= code ? (code > 1 ? ((uint64_t)code) << 1 : 0) :
						    RTE_MBUF_F_RX_SEC_OFFLOAD_FAILED;

				ol_flags0 |= ((uint64_t)(vget_lane_u8(ucc, 1)) << 18);
			}

			if (cq1_w1 & BIT(11)) {
				uintptr_t wqe = vgetq_lane_u64(wqe01, 1);
				uintptr_t sa = vgetq_lane_u64(sa01, 1);
				uint16_t len = vget_lane_u16(lens, 1);

				cpth1 = (uintptr_t)mbuf1 + d_off;

				/* Free meta to aura */
				if (!(flags & NIX_RX_REAS_F) ||
				    *(uint64_t *)cpth1 & BIT_ULL(15)) {
					NIX_PUSH_META_TO_FREE(mbuf1, laddr,
							      &loff);
					mbuf01 = vsetq_lane_u64(wqe, mbuf01, 1);
					mbuf1 = (struct rte_mbuf *)wqe;
				} else if (flags & NIX_RX_REAS_F) {
					/* Update meta pool for full mode pkts */
					mbuf1->pool = meta_pool;
				}

				/* Update pkt_len and data_len */
				f1 = vsetq_lane_u16(len, f1, 2);
				f1 = vsetq_lane_u16(len, f1, 4);

				nix_sec_meta_to_mbuf(cq1_w1, cq1_w5, sa, cpth1,
						     mbuf1, &f1, &ol_flags1,
						     flags, &rearm1);
				code = vget_lane_u8(ucc, 2);
				ol_flags1 |= code ? (code > 1 ? ((uint64_t)code) << 1 : 0) :
						    RTE_MBUF_F_RX_SEC_OFFLOAD_FAILED;
				ol_flags1 |= ((uint64_t)(vget_lane_u8(ucc, 3)) << 18);
			}

			if (cq2_w1 & BIT(11)) {
				uintptr_t wqe = vgetq_lane_u64(wqe23, 0);
				uintptr_t sa = vgetq_lane_u64(sa23, 0);
				uint16_t len = vget_lane_u16(lens, 2);

				cpth2 = (uintptr_t)mbuf2 + d_off;

				/* Free meta to aura */
				if (!(flags & NIX_RX_REAS_F) ||
				    *(uint64_t *)cpth2 & BIT_ULL(15)) {
					NIX_PUSH_META_TO_FREE(mbuf2, laddr,
							      &loff);
					mbuf23 = vsetq_lane_u64(wqe, mbuf23, 0);
					mbuf2 = (struct rte_mbuf *)wqe;
				} else if (flags & NIX_RX_REAS_F) {
					/* Update meta pool for full mode pkts */
					mbuf2->pool = meta_pool;
				}

				/* Update pkt_len and data_len */
				f2 = vsetq_lane_u16(len, f2, 2);
				f2 = vsetq_lane_u16(len, f2, 4);

				nix_sec_meta_to_mbuf(cq2_w1, cq2_w5, sa, cpth2,
						     mbuf2, &f2, &ol_flags2,
						     flags, &rearm2);
				code = vget_lane_u8(ucc, 4);
				ol_flags2 |= code ? (code > 1 ? ((uint64_t)code) << 1 : 0) :
						    RTE_MBUF_F_RX_SEC_OFFLOAD_FAILED;
				ol_flags2 |= ((uint64_t)(vget_lane_u8(ucc, 5)) << 18);
			}

			if (cq3_w1 & BIT(11)) {
				uintptr_t wqe = vgetq_lane_u64(wqe23, 1);
				uintptr_t sa = vgetq_lane_u64(sa23, 1);
				uint16_t len = vget_lane_u16(lens, 3);

				cpth3 = (uintptr_t)mbuf3 + d_off;

				/* Free meta to aura */
				if (!(flags & NIX_RX_REAS_F) ||
				    *(uint64_t *)cpth3 & BIT_ULL(15)) {
					NIX_PUSH_META_TO_FREE(mbuf3, laddr,
							      &loff);
					mbuf23 = vsetq_lane_u64(wqe, mbuf23, 1);
					mbuf3 = (struct rte_mbuf *)wqe;
				} else if (flags & NIX_RX_REAS_F) {
					/* Update meta pool for full mode pkts */
					mbuf3->pool = meta_pool;
				}

				/* Update pkt_len and data_len */
				f3 = vsetq_lane_u16(len, f3, 2);
				f3 = vsetq_lane_u16(len, f3, 4);

				nix_sec_meta_to_mbuf(cq3_w1, cq3_w5, sa, cpth3,
						     mbuf3, &f3, &ol_flags3,
						     flags, &rearm3);
				code = vget_lane_u8(ucc, 6);
				ol_flags3 |= code ? (code > 1 ? ((uint64_t)code) << 1 : 0) :
						    RTE_MBUF_F_RX_SEC_OFFLOAD_FAILED;
				ol_flags3 |= ((uint64_t)(vget_lane_u8(ucc, 7)) << 18);
			}
		}

		if (flags & NIX_RX_OFFLOAD_VLAN_STRIP_F) {

			ol_flags0 = nix_vlan_update(cq0_w2, ol_flags0, &f0);
			ol_flags1 = nix_vlan_update(cq1_w2, ol_flags1, &f1);
			ol_flags2 = nix_vlan_update(cq2_w2, ol_flags2, &f2);
			ol_flags3 = nix_vlan_update(cq3_w2, ol_flags3, &f3);

			ol_flags0 = nix_qinq_update(cq0_w2, ol_flags0, mbuf0);
			ol_flags1 = nix_qinq_update(cq1_w2, ol_flags1, mbuf1);
			ol_flags2 = nix_qinq_update(cq2_w2, ol_flags2, mbuf2);
			ol_flags3 = nix_qinq_update(cq3_w2, ol_flags3, mbuf3);
		}

		if (flags & NIX_RX_OFFLOAD_MARK_UPDATE_F) {
			ol_flags0 = nix_update_match_id(
				*(uint16_t *)CQE_PTR_OFF(cq0, 0, 38, flags),
				ol_flags0, mbuf0);
			ol_flags1 = nix_update_match_id(
				*(uint16_t *)CQE_PTR_OFF(cq0, 1, 38, flags),
				ol_flags1, mbuf1);
			ol_flags2 = nix_update_match_id(
				*(uint16_t *)CQE_PTR_OFF(cq0, 2, 38, flags),
				ol_flags2, mbuf2);
			ol_flags3 = nix_update_match_id(
				*(uint16_t *)CQE_PTR_OFF(cq0, 3, 38, flags),
				ol_flags3, mbuf3);
		}

		if ((flags & NIX_RX_OFFLOAD_TSTAMP_F) &&
		    ((flags & NIX_RX_VWQE_F) && tstamp)) {
			const uint16x8_t len_off = {
				0,			     /* ptype   0:15 */
				0,			     /* ptype  16:32 */
				CNXK_NIX_TIMESYNC_RX_OFFSET, /* pktlen  0:15*/
				0,			     /* pktlen 16:32 */
				CNXK_NIX_TIMESYNC_RX_OFFSET, /* datalen 0:15 */
				0,
				0,
				0};
			const uint32x4_t ptype = {RTE_PTYPE_L2_ETHER_TIMESYNC,
						  RTE_PTYPE_L2_ETHER_TIMESYNC,
						  RTE_PTYPE_L2_ETHER_TIMESYNC,
						  RTE_PTYPE_L2_ETHER_TIMESYNC};
			const uint64_t ts_olf = RTE_MBUF_F_RX_IEEE1588_PTP |
						RTE_MBUF_F_RX_IEEE1588_TMST |
						tstamp->rx_tstamp_dynflag;
			const uint32x4_t and_mask = {0x1, 0x2, 0x4, 0x8};
			uint64x2_t ts01, ts23, mask;
			uint64_t ts[4];
			uint8_t res;

			/* Subtract timesync length from total pkt length. */
			f0 = vsubq_u16(f0, len_off);
			f1 = vsubq_u16(f1, len_off);
			f2 = vsubq_u16(f2, len_off);
			f3 = vsubq_u16(f3, len_off);

			/* Get the address of actual timestamp. */
			ts01 = vaddq_u64(mbuf01, data_off);
			ts23 = vaddq_u64(mbuf23, data_off);
			/* Load timestamp from address. */
			ts01 = vsetq_lane_u64(*(uint64_t *)vgetq_lane_u64(ts01,
									  0),
					      ts01, 0);
			ts01 = vsetq_lane_u64(*(uint64_t *)vgetq_lane_u64(ts01,
									  1),
					      ts01, 1);
			ts23 = vsetq_lane_u64(*(uint64_t *)vgetq_lane_u64(ts23,
									  0),
					      ts23, 0);
			ts23 = vsetq_lane_u64(*(uint64_t *)vgetq_lane_u64(ts23,
									  1),
					      ts23, 1);
			/* Convert from be to cpu byteorder. */
			ts01 = vrev64q_u8(ts01);
			ts23 = vrev64q_u8(ts23);
			/* Store timestamp into scalar for later use. */
			ts[0] = vgetq_lane_u64(ts01, 0);
			ts[1] = vgetq_lane_u64(ts01, 1);
			ts[2] = vgetq_lane_u64(ts23, 0);
			ts[3] = vgetq_lane_u64(ts23, 1);

			/* Store timestamp into dynfield. */
			*cnxk_nix_timestamp_dynfield(mbuf0, tstamp) = ts[0];
			*cnxk_nix_timestamp_dynfield(mbuf1, tstamp) = ts[1];
			*cnxk_nix_timestamp_dynfield(mbuf2, tstamp) = ts[2];
			*cnxk_nix_timestamp_dynfield(mbuf3, tstamp) = ts[3];

			/* Generate ptype mask to filter L2 ether timesync */
			mask = vdupq_n_u32(vgetq_lane_u32(f0, 0));
			mask = vsetq_lane_u32(vgetq_lane_u32(f1, 0), mask, 1);
			mask = vsetq_lane_u32(vgetq_lane_u32(f2, 0), mask, 2);
			mask = vsetq_lane_u32(vgetq_lane_u32(f3, 0), mask, 3);

			/* Match against L2 ether timesync. */
			mask = vceqq_u32(mask, ptype);
			/* Convert from vector from scalar mask */
			res = vaddvq_u32(vandq_u32(mask, and_mask));
			res &= 0xF;

			if (res) {
				/* Fill in the ol_flags for any packets that
				 * matched.
				 */
				ol_flags0 |= ((res & 0x1) ? ts_olf : 0);
				ol_flags1 |= ((res & 0x2) ? ts_olf : 0);
				ol_flags2 |= ((res & 0x4) ? ts_olf : 0);
				ol_flags3 |= ((res & 0x8) ? ts_olf : 0);

				/* Update Rxq timestamp with the latest
				 * timestamp.
				 */
				tstamp->rx_ready = 1;
				tstamp->rx_tstamp = ts[31 - rte_clz32(res)];
			}
		}

		/* Form rearm_data with ol_flags */
		rearm0 = vsetq_lane_u64(ol_flags0, rearm0, 1);
		rearm1 = vsetq_lane_u64(ol_flags1, rearm1, 1);
		rearm2 = vsetq_lane_u64(ol_flags2, rearm2, 1);
		rearm3 = vsetq_lane_u64(ol_flags3, rearm3, 1);

		/* Update rx_descriptor_fields1 */
		vst1q_u64((uint64_t *)mbuf0->rx_descriptor_fields1, f0);
		vst1q_u64((uint64_t *)mbuf1->rx_descriptor_fields1, f1);
		vst1q_u64((uint64_t *)mbuf2->rx_descriptor_fields1, f2);
		vst1q_u64((uint64_t *)mbuf3->rx_descriptor_fields1, f3);

		/* Update rearm_data */
		vst1q_u64((uint64_t *)mbuf0->rearm_data, rearm0);
		vst1q_u64((uint64_t *)mbuf1->rearm_data, rearm1);
		vst1q_u64((uint64_t *)mbuf2->rearm_data, rearm2);
		vst1q_u64((uint64_t *)mbuf3->rearm_data, rearm3);

		if (flags & NIX_RX_MULTI_SEG_F) {
			/* Multi segment is enable build mseg list for
			 * individual mbufs in scalar mode.
			 */
			nix_cqe_xtract_mseg((union nix_rx_parse_u *)
					    (CQE_PTR_OFF(cq0, 0, 8, flags)),
					    mbuf0, mbuf_initializer, cpth0, sa_base, flags);
			nix_cqe_xtract_mseg((union nix_rx_parse_u *)
					    (CQE_PTR_OFF(cq0, 1, 8, flags)),
					    mbuf1, mbuf_initializer, cpth1, sa_base, flags);
			nix_cqe_xtract_mseg((union nix_rx_parse_u *)
					    (CQE_PTR_OFF(cq0, 2, 8, flags)),
					    mbuf2, mbuf_initializer, cpth2, sa_base, flags);
			nix_cqe_xtract_mseg((union nix_rx_parse_u *)
					    (CQE_PTR_OFF(cq0, 3, 8, flags)),
					    mbuf3, mbuf_initializer, cpth3, sa_base, flags);
		}

		/* Store the mbufs to rx_pkts */
		vst1q_u64((uint64_t *)&mbufs[packets], mbuf01);
		vst1q_u64((uint64_t *)&mbufs[packets + 2], mbuf23);

		nix_mbuf_validate_next(mbuf0);
		nix_mbuf_validate_next(mbuf1);
		nix_mbuf_validate_next(mbuf2);
		nix_mbuf_validate_next(mbuf3);

		packets += NIX_DESCS_PER_LOOP;

		if (!(flags & NIX_RX_VWQE_F)) {
			/* Advance head pointer and packets */
			head += NIX_DESCS_PER_LOOP;
			head &= qmask;
		}

		if (flags & NIX_RX_OFFLOAD_SECURITY_F) {
			/* Check if lmtline border is crossed and adjust lnum */
			if (loff > 15) {
				/* Update aura handle */
				*(uint64_t *)(laddr - 8) =
					(((uint64_t)(15 & 0x1) << 32) |
				    roc_npa_aura_handle_to_aura(meta_aura));
				loff = loff - 15;
				shft += 3;

				lnum++;
				laddr = (uintptr_t)LMT_OFF(lbase, lnum, 8);
				/* Pick the pointer from 16th index and put it
				 * at end of this new line.
				 */
				*(uint64_t *)(laddr + (loff << 3) - 8) =
					*(uint64_t *)(laddr - 8);
			}

			/* Flush it when we are in 16th line and might
			 * overflow it
			 */
			if (lnum >= 15 && loff >= 12) {
				/* 16 LMT Line size m1 */
				uint64_t data = BIT_ULL(48) - 1;

				/* Update aura handle */
				*(uint64_t *)(laddr - 8) =
					(((uint64_t)(loff & 0x1) << 32) |
				    roc_npa_aura_handle_to_aura(meta_aura));

				data = (data & ~(0x7UL << shft)) |
				       (((uint64_t)loff >> 1) << shft);

				/* Send up to 16 lmt lines of pointers */
				nix_sec_flush_meta_burst(lmt_id, data, lnum + 1,
							 meta_aura);
				rte_io_wmb();
				lnum = 0;
				loff = 0;
				shft = 0;
				/* First pointer starts at 8B offset */
				laddr = (uintptr_t)LMT_OFF(lbase, lnum, 8);
			}
		}
	}

	if (flags & NIX_RX_OFFLOAD_SECURITY_F && loff) {
		/* 16 LMT Line size m1 */
		uint64_t data = BIT_ULL(48) - 1;

		/* Update aura handle */
		*(uint64_t *)(laddr - 8) =
			(((uint64_t)(loff & 0x1) << 32) |
			 roc_npa_aura_handle_to_aura(meta_aura));

		data = (data & ~(0x7UL << shft)) |
		       (((uint64_t)loff >> 1) << shft);

		/* Send up to 16 lmt lines of pointers */
		nix_sec_flush_meta_burst(lmt_id, data, lnum + 1, meta_aura);
		if (flags & NIX_RX_VWQE_F)
			plt_io_wmb();
	}

	if (flags & NIX_RX_VWQE_F)
		return packets;

	rxq->head = head;
	rxq->available -= packets;

	rte_io_wmb();
	/* Free all the CQs that we've processed */
	plt_write64((rxq->wdata | packets), rxq->cq_door);

	if (unlikely(pkts_left))
		packets += cn10k_nix_recv_pkts(args, &mbufs[packets], pkts_left,
					       flags);

	return packets;
}

#else

static inline uint16_t
cn10k_nix_recv_pkts_vector(void *args, struct rte_mbuf **mbufs, uint16_t pkts,
			   const uint16_t flags, void *lookup_mem,
			   struct cnxk_timesync_info *tstamp,
			   uintptr_t lmt_base, uint64_t meta_aura)
{
	RTE_SET_USED(args);
	RTE_SET_USED(mbufs);
	RTE_SET_USED(pkts);
	RTE_SET_USED(flags);
	RTE_SET_USED(lookup_mem);
	RTE_SET_USED(tstamp);
	RTE_SET_USED(lmt_base);
	RTE_SET_USED(meta_aura);

	return 0;
}

#endif


#define RSS_F	  NIX_RX_OFFLOAD_RSS_F
#define PTYPE_F	  NIX_RX_OFFLOAD_PTYPE_F
#define CKSUM_F	  NIX_RX_OFFLOAD_CHECKSUM_F
#define MARK_F	  NIX_RX_OFFLOAD_MARK_UPDATE_F
#define TS_F      NIX_RX_OFFLOAD_TSTAMP_F
#define RX_VLAN_F NIX_RX_OFFLOAD_VLAN_STRIP_F
#define R_SEC_F   NIX_RX_OFFLOAD_SECURITY_F

/* [R_SEC_F] [RX_VLAN_F] [TS] [MARK] [CKSUM] [PTYPE] [RSS] */
#define NIX_RX_FASTPATH_MODES_0_15                                             \
	R(no_offload, NIX_RX_OFFLOAD_NONE)                                     \
	R(rss, RSS_F)                                                          \
	R(ptype, PTYPE_F)                                                      \
	R(ptype_rss, PTYPE_F | RSS_F)                                          \
	R(cksum, CKSUM_F)                                                      \
	R(cksum_rss, CKSUM_F | RSS_F)                                          \
	R(cksum_ptype, CKSUM_F | PTYPE_F)                                      \
	R(cksum_ptype_rss, CKSUM_F | PTYPE_F | RSS_F)                          \
	R(mark, MARK_F)                                                        \
	R(mark_rss, MARK_F | RSS_F)                                            \
	R(mark_ptype, MARK_F | PTYPE_F)                                        \
	R(mark_ptype_rss, MARK_F | PTYPE_F | RSS_F)                            \
	R(mark_cksum, MARK_F | CKSUM_F)                                        \
	R(mark_cksum_rss, MARK_F | CKSUM_F | RSS_F)                            \
	R(mark_cksum_ptype, MARK_F | CKSUM_F | PTYPE_F)                        \
	R(mark_cksum_ptype_rss, MARK_F | CKSUM_F | PTYPE_F | RSS_F)

#define NIX_RX_FASTPATH_MODES_16_31                                            \
	R(ts, TS_F)                                                            \
	R(ts_rss, TS_F | RSS_F)                                                \
	R(ts_ptype, TS_F | PTYPE_F)                                            \
	R(ts_ptype_rss, TS_F | PTYPE_F | RSS_F)                                \
	R(ts_cksum, TS_F | CKSUM_F)                                            \
	R(ts_cksum_rss, TS_F | CKSUM_F | RSS_F)                                \
	R(ts_cksum_ptype, TS_F | CKSUM_F | PTYPE_F)                            \
	R(ts_cksum_ptype_rss, TS_F | CKSUM_F | PTYPE_F | RSS_F)                \
	R(ts_mark, TS_F | MARK_F)                                              \
	R(ts_mark_rss, TS_F | MARK_F | RSS_F)                                  \
	R(ts_mark_ptype, TS_F | MARK_F | PTYPE_F)                              \
	R(ts_mark_ptype_rss, TS_F | MARK_F | PTYPE_F | RSS_F)                  \
	R(ts_mark_cksum, TS_F | MARK_F | CKSUM_F)                              \
	R(ts_mark_cksum_rss, TS_F | MARK_F | CKSUM_F | RSS_F)                  \
	R(ts_mark_cksum_ptype, TS_F | MARK_F | CKSUM_F | PTYPE_F)              \
	R(ts_mark_cksum_ptype_rss, TS_F | MARK_F | CKSUM_F | PTYPE_F | RSS_F)

#define NIX_RX_FASTPATH_MODES_32_47                                            \
	R(vlan, RX_VLAN_F)                                                     \
	R(vlan_rss, RX_VLAN_F | RSS_F)                                         \
	R(vlan_ptype, RX_VLAN_F | PTYPE_F)                                     \
	R(vlan_ptype_rss, RX_VLAN_F | PTYPE_F | RSS_F)                         \
	R(vlan_cksum, RX_VLAN_F | CKSUM_F)                                     \
	R(vlan_cksum_rss, RX_VLAN_F | CKSUM_F | RSS_F)                         \
	R(vlan_cksum_ptype, RX_VLAN_F | CKSUM_F | PTYPE_F)                     \
	R(vlan_cksum_ptype_rss, RX_VLAN_F | CKSUM_F | PTYPE_F | RSS_F)         \
	R(vlan_mark, RX_VLAN_F | MARK_F)                                       \
	R(vlan_mark_rss, RX_VLAN_F | MARK_F | RSS_F)                           \
	R(vlan_mark_ptype, RX_VLAN_F | MARK_F | PTYPE_F)                       \
	R(vlan_mark_ptype_rss, RX_VLAN_F | MARK_F | PTYPE_F | RSS_F)           \
	R(vlan_mark_cksum, RX_VLAN_F | MARK_F | CKSUM_F)                       \
	R(vlan_mark_cksum_rss, RX_VLAN_F | MARK_F | CKSUM_F | RSS_F)           \
	R(vlan_mark_cksum_ptype, RX_VLAN_F | MARK_F | CKSUM_F | PTYPE_F)       \
	R(vlan_mark_cksum_ptype_rss,                                           \
	  RX_VLAN_F | MARK_F | CKSUM_F | PTYPE_F | RSS_F)

#define NIX_RX_FASTPATH_MODES_48_63                                            \
	R(vlan_ts, RX_VLAN_F | TS_F)                                           \
	R(vlan_ts_rss, RX_VLAN_F | TS_F | RSS_F)                               \
	R(vlan_ts_ptype, RX_VLAN_F | TS_F | PTYPE_F)                           \
	R(vlan_ts_ptype_rss, RX_VLAN_F | TS_F | PTYPE_F | RSS_F)               \
	R(vlan_ts_cksum, RX_VLAN_F | TS_F | CKSUM_F)                           \
	R(vlan_ts_cksum_rss, RX_VLAN_F | TS_F | CKSUM_F | RSS_F)               \
	R(vlan_ts_cksum_ptype, RX_VLAN_F | TS_F | CKSUM_F | PTYPE_F)           \
	R(vlan_ts_cksum_ptype_rss,                                             \
	  RX_VLAN_F | TS_F | CKSUM_F | PTYPE_F | RSS_F)                        \
	R(vlan_ts_mark, RX_VLAN_F | TS_F | MARK_F)                             \
	R(vlan_ts_mark_rss, RX_VLAN_F | TS_F | MARK_F | RSS_F)                 \
	R(vlan_ts_mark_ptype, RX_VLAN_F | TS_F | MARK_F | PTYPE_F)             \
	R(vlan_ts_mark_ptype_rss, RX_VLAN_F | TS_F | MARK_F | PTYPE_F | RSS_F) \
	R(vlan_ts_mark_cksum, RX_VLAN_F | TS_F | MARK_F | CKSUM_F)             \
	R(vlan_ts_mark_cksum_rss, RX_VLAN_F | TS_F | MARK_F | CKSUM_F | RSS_F) \
	R(vlan_ts_mark_cksum_ptype,                                            \
	  RX_VLAN_F | TS_F | MARK_F | CKSUM_F | PTYPE_F)                       \
	R(vlan_ts_mark_cksum_ptype_rss,                                        \
	  RX_VLAN_F | TS_F | MARK_F | CKSUM_F | PTYPE_F | RSS_F)

#define NIX_RX_FASTPATH_MODES_64_79                                            \
	R(sec, R_SEC_F)                                                        \
	R(sec_rss, R_SEC_F | RSS_F)                                            \
	R(sec_ptype, R_SEC_F | PTYPE_F)                                        \
	R(sec_ptype_rss, R_SEC_F | PTYPE_F | RSS_F)                            \
	R(sec_cksum, R_SEC_F | CKSUM_F)                                        \
	R(sec_cksum_rss, R_SEC_F | CKSUM_F | RSS_F)                            \
	R(sec_cksum_ptype, R_SEC_F | CKSUM_F | PTYPE_F)                        \
	R(sec_cksum_ptype_rss, R_SEC_F | CKSUM_F | PTYPE_F | RSS_F)            \
	R(sec_mark, R_SEC_F | MARK_F)                                          \
	R(sec_mark_rss, R_SEC_F | MARK_F | RSS_F)                              \
	R(sec_mark_ptype, R_SEC_F | MARK_F | PTYPE_F)                          \
	R(sec_mark_ptype_rss, R_SEC_F | MARK_F | PTYPE_F | RSS_F)              \
	R(sec_mark_cksum, R_SEC_F | MARK_F | CKSUM_F)                          \
	R(sec_mark_cksum_rss, R_SEC_F | MARK_F | CKSUM_F | RSS_F)              \
	R(sec_mark_cksum_ptype, R_SEC_F | MARK_F | CKSUM_F | PTYPE_F)          \
	R(sec_mark_cksum_ptype_rss,                                            \
	  R_SEC_F | MARK_F | CKSUM_F | PTYPE_F | RSS_F)

#define NIX_RX_FASTPATH_MODES_80_95                                            \
	R(sec_ts, R_SEC_F | TS_F)                                              \
	R(sec_ts_rss, R_SEC_F | TS_F | RSS_F)                                  \
	R(sec_ts_ptype, R_SEC_F | TS_F | PTYPE_F)                              \
	R(sec_ts_ptype_rss, R_SEC_F | TS_F | PTYPE_F | RSS_F)                  \
	R(sec_ts_cksum, R_SEC_F | TS_F | CKSUM_F)                              \
	R(sec_ts_cksum_rss, R_SEC_F | TS_F | CKSUM_F | RSS_F)                  \
	R(sec_ts_cksum_ptype, R_SEC_F | TS_F | CKSUM_F | PTYPE_F)              \
	R(sec_ts_cksum_ptype_rss, R_SEC_F | TS_F | CKSUM_F | PTYPE_F | RSS_F)  \
	R(sec_ts_mark, R_SEC_F | TS_F | MARK_F)                                \
	R(sec_ts_mark_rss, R_SEC_F | TS_F | MARK_F | RSS_F)                    \
	R(sec_ts_mark_ptype, R_SEC_F | TS_F | MARK_F | PTYPE_F)                \
	R(sec_ts_mark_ptype_rss, R_SEC_F | TS_F | MARK_F | PTYPE_F | RSS_F)    \
	R(sec_ts_mark_cksum, R_SEC_F | TS_F | MARK_F | CKSUM_F)                \
	R(sec_ts_mark_cksum_rss, R_SEC_F | TS_F | MARK_F | CKSUM_F | RSS_F)    \
	R(sec_ts_mark_cksum_ptype,                                             \
	  R_SEC_F | TS_F | MARK_F | CKSUM_F | PTYPE_F)                         \
	R(sec_ts_mark_cksum_ptype_rss,                                         \
	  R_SEC_F | TS_F | MARK_F | CKSUM_F | PTYPE_F | RSS_F)

#define NIX_RX_FASTPATH_MODES_96_111                                           \
	R(sec_vlan, R_SEC_F | RX_VLAN_F)                                       \
	R(sec_vlan_rss, R_SEC_F | RX_VLAN_F | RSS_F)                           \
	R(sec_vlan_ptype, R_SEC_F | RX_VLAN_F | PTYPE_F)                       \
	R(sec_vlan_ptype_rss, R_SEC_F | RX_VLAN_F | PTYPE_F | RSS_F)           \
	R(sec_vlan_cksum, R_SEC_F | RX_VLAN_F | CKSUM_F)                       \
	R(sec_vlan_cksum_rss, R_SEC_F | RX_VLAN_F | CKSUM_F | RSS_F)           \
	R(sec_vlan_cksum_ptype, R_SEC_F | RX_VLAN_F | CKSUM_F | PTYPE_F)       \
	R(sec_vlan_cksum_ptype_rss,                                            \
	  R_SEC_F | RX_VLAN_F | CKSUM_F | PTYPE_F | RSS_F)                     \
	R(sec_vlan_mark, R_SEC_F | RX_VLAN_F | MARK_F)                         \
	R(sec_vlan_mark_rss, R_SEC_F | RX_VLAN_F | MARK_F | RSS_F)             \
	R(sec_vlan_mark_ptype, R_SEC_F | RX_VLAN_F | MARK_F | PTYPE_F)         \
	R(sec_vlan_mark_ptype_rss,                                             \
	  R_SEC_F | RX_VLAN_F | MARK_F | PTYPE_F | RSS_F)                      \
	R(sec_vlan_mark_cksum, R_SEC_F | RX_VLAN_F | MARK_F | CKSUM_F)         \
	R(sec_vlan_mark_cksum_rss,                                             \
	  R_SEC_F | RX_VLAN_F | MARK_F | CKSUM_F | RSS_F)                      \
	R(sec_vlan_mark_cksum_ptype,                                           \
	  R_SEC_F | RX_VLAN_F | MARK_F | CKSUM_F | PTYPE_F)                    \
	R(sec_vlan_mark_cksum_ptype_rss,                                       \
	  R_SEC_F | RX_VLAN_F | MARK_F | CKSUM_F | PTYPE_F | RSS_F)

#define NIX_RX_FASTPATH_MODES_112_127                                          \
	R(sec_vlan_ts, R_SEC_F | RX_VLAN_F | TS_F)                             \
	R(sec_vlan_ts_rss, R_SEC_F | RX_VLAN_F | TS_F | RSS_F)                 \
	R(sec_vlan_ts_ptype, R_SEC_F | RX_VLAN_F | TS_F | PTYPE_F)             \
	R(sec_vlan_ts_ptype_rss, R_SEC_F | RX_VLAN_F | TS_F | PTYPE_F | RSS_F) \
	R(sec_vlan_ts_cksum, R_SEC_F | RX_VLAN_F | TS_F | CKSUM_F)             \
	R(sec_vlan_ts_cksum_rss, R_SEC_F | RX_VLAN_F | TS_F | CKSUM_F | RSS_F) \
	R(sec_vlan_ts_cksum_ptype,                                             \
	  R_SEC_F | RX_VLAN_F | TS_F | CKSUM_F | PTYPE_F)                      \
	R(sec_vlan_ts_cksum_ptype_rss,                                         \
	  R_SEC_F | RX_VLAN_F | TS_F | CKSUM_F | PTYPE_F | RSS_F)              \
	R(sec_vlan_ts_mark, R_SEC_F | RX_VLAN_F | TS_F | MARK_F)               \
	R(sec_vlan_ts_mark_rss, R_SEC_F | RX_VLAN_F | TS_F | MARK_F | RSS_F)   \
	R(sec_vlan_ts_mark_ptype,                                              \
	  R_SEC_F | RX_VLAN_F | TS_F | MARK_F | PTYPE_F)                       \
	R(sec_vlan_ts_mark_ptype_rss,                                          \
	  R_SEC_F | RX_VLAN_F | TS_F | MARK_F | PTYPE_F | RSS_F)               \
	R(sec_vlan_ts_mark_cksum,                                              \
	  R_SEC_F | RX_VLAN_F | TS_F | MARK_F | CKSUM_F)                       \
	R(sec_vlan_ts_mark_cksum_rss,                                          \
	  R_SEC_F | RX_VLAN_F | TS_F | MARK_F | CKSUM_F | RSS_F)               \
	R(sec_vlan_ts_mark_cksum_ptype,                                        \
	  R_SEC_F | RX_VLAN_F | TS_F | MARK_F | CKSUM_F | PTYPE_F)             \
	R(sec_vlan_ts_mark_cksum_ptype_rss,                                    \
	  R_SEC_F | RX_VLAN_F | TS_F | MARK_F | CKSUM_F | PTYPE_F | RSS_F)


#define NIX_RX_FASTPATH_MODES                                                  \
	NIX_RX_FASTPATH_MODES_0_15                                             \
	NIX_RX_FASTPATH_MODES_16_31                                            \
	NIX_RX_FASTPATH_MODES_32_47                                            \
	NIX_RX_FASTPATH_MODES_48_63                                            \
	NIX_RX_FASTPATH_MODES_64_79                                            \
	NIX_RX_FASTPATH_MODES_80_95                                            \
	NIX_RX_FASTPATH_MODES_96_111                                           \
	NIX_RX_FASTPATH_MODES_112_127                                          \

#define R(name, flags)                                                         \
	uint16_t __rte_noinline __rte_hot cn10k_nix_recv_pkts_##name(          \
		void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t pkts);     \
	uint16_t __rte_noinline __rte_hot cn10k_nix_recv_pkts_mseg_##name(     \
		void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t pkts);     \
	uint16_t __rte_noinline __rte_hot cn10k_nix_recv_pkts_vec_##name(      \
		void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t pkts);     \
	uint16_t __rte_noinline __rte_hot cn10k_nix_recv_pkts_vec_mseg_##name( \
		void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t pkts);     \
	uint16_t __rte_noinline __rte_hot cn10k_nix_recv_pkts_reas_##name(     \
		void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t pkts);     \
	uint16_t __rte_noinline __rte_hot cn10k_nix_recv_pkts_reas_mseg_##name(\
		void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t pkts);     \
	uint16_t __rte_noinline __rte_hot cn10k_nix_recv_pkts_reas_vec_##name( \
		void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t pkts);     \
	uint16_t __rte_noinline __rte_hot cn10k_nix_recv_pkts_reas_vec_mseg_##name( \
		void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t pkts);

NIX_RX_FASTPATH_MODES
#undef R

#define NIX_RX_RECV(fn, flags)                                                 \
	uint16_t __rte_noinline __rte_hot fn(                                  \
		void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t pkts)      \
	{                                                                      \
		return cn10k_nix_recv_pkts(rx_queue, rx_pkts, pkts, (flags));  \
	}

#define NIX_RX_RECV_MSEG(fn, flags) NIX_RX_RECV(fn, flags | NIX_RX_MULTI_SEG_F)

#define NIX_RX_RECV_VEC(fn, flags)                                             \
	uint16_t __rte_noinline __rte_hot fn(                                  \
		void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t pkts)      \
	{                                                                      \
		return cn10k_nix_recv_pkts_vector(rx_queue, rx_pkts, pkts,     \
						  (flags), NULL, NULL, 0, 0);  \
	}

#define NIX_RX_RECV_VEC_MSEG(fn, flags)                                        \
	NIX_RX_RECV_VEC(fn, flags | NIX_RX_MULTI_SEG_F)

uint16_t __rte_noinline __rte_hot cn10k_nix_recv_pkts_all_offload(void *rx_queue,
								  struct rte_mbuf **rx_pkts,
								  uint16_t pkts);

uint16_t __rte_noinline __rte_hot cn10k_nix_recv_pkts_vec_all_offload(void *rx_queue,
								      struct rte_mbuf **rx_pkts,
								      uint16_t pkts);

uint16_t __rte_noinline __rte_hot cn10k_nix_recv_pkts_all_offload_tst(void *rx_queue,
								      struct rte_mbuf **rx_pkts,
								      uint16_t pkts);

uint16_t __rte_noinline __rte_hot cn10k_nix_recv_pkts_vec_all_offload_tst(void *rx_queue,
									  struct rte_mbuf **rx_pkts,
									  uint16_t pkts);

#endif /* __CN10K_RX_H__ */
