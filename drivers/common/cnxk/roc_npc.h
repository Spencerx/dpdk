/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2021 Marvell.
 */

#ifndef _ROC_NPC_H_
#define _ROC_NPC_H_

#include <sys/queue.h>

enum roc_npc_item_type {
	ROC_NPC_ITEM_TYPE_VOID,
	ROC_NPC_ITEM_TYPE_ANY,
	ROC_NPC_ITEM_TYPE_ETH,
	ROC_NPC_ITEM_TYPE_VLAN,
	ROC_NPC_ITEM_TYPE_E_TAG,
	ROC_NPC_ITEM_TYPE_IPV4,
	ROC_NPC_ITEM_TYPE_IPV6,
	ROC_NPC_ITEM_TYPE_IPV6_FRAG_EXT,
	ROC_NPC_ITEM_TYPE_ARP_ETH_IPV4,
	ROC_NPC_ITEM_TYPE_MPLS,
	ROC_NPC_ITEM_TYPE_ICMP,
	ROC_NPC_ITEM_TYPE_IGMP,
	ROC_NPC_ITEM_TYPE_UDP,
	ROC_NPC_ITEM_TYPE_TCP,
	ROC_NPC_ITEM_TYPE_SCTP,
	ROC_NPC_ITEM_TYPE_ESP,
	ROC_NPC_ITEM_TYPE_GRE,
	ROC_NPC_ITEM_TYPE_NVGRE,
	ROC_NPC_ITEM_TYPE_VXLAN,
	ROC_NPC_ITEM_TYPE_GTPC,
	ROC_NPC_ITEM_TYPE_GTPU,
	ROC_NPC_ITEM_TYPE_GENEVE,
	ROC_NPC_ITEM_TYPE_VXLAN_GPE,
	ROC_NPC_ITEM_TYPE_IPV6_EXT,
	ROC_NPC_ITEM_TYPE_GRE_KEY,
	ROC_NPC_ITEM_TYPE_PPPOES,
	ROC_NPC_ITEM_TYPE_HIGIG2,
	ROC_NPC_ITEM_TYPE_CPT_HDR,
	ROC_NPC_ITEM_TYPE_L3_CUSTOM,
	ROC_NPC_ITEM_TYPE_QINQ,
	ROC_NPC_ITEM_TYPE_RAW,
	ROC_NPC_ITEM_TYPE_MARK,
	ROC_NPC_ITEM_TYPE_TX_QUEUE,
	ROC_NPC_ITEM_TYPE_IPV6_ROUTING_EXT,
	ROC_NPC_ITEM_TYPE_REPRESENTED_PORT,
	ROC_NPC_ITEM_TYPE_END,
};

struct roc_npc_item_info {
	enum roc_npc_item_type type; /* Item type */
	uint32_t size;		     /* item size */
	const void *spec; /**< Pointer to item specification structure. */
	const void *mask; /**< Bit-mask applied to spec and last. */
	const void *last; /* For range */
};

struct roc_npc_flow_item_raw {
	uint32_t relative : 1; /**< Look for pattern after the previous item. */
	uint32_t search : 1;   /**< Search pattern from offset. */
	uint32_t reserved : 30; /**< Reserved, must be set to zero. */
	int32_t offset;		/**< Absolute or relative offset for pattern. */
	uint16_t limit;		/**< Search area limit for start of pattern. */
	uint16_t length;	/**< Pattern length. */
	const uint8_t *pattern; /**< Byte string to look for. */
};

struct roc_ether_addr {
	uint8_t addr_bytes[PLT_ETHER_ADDR_LEN]; /**< Addr bytes in tx order */
} __plt_aligned(2);

struct roc_ether_hdr {
	struct roc_ether_addr d_addr; /**< Destination address. */
	union {
		struct roc_ether_addr s_addr; /**< Source address. */
		struct {
			struct roc_ether_addr S_addr;
		} S_un; /**< Do not use directly; use s_addr instead.*/
	};
	uint16_t ether_type; /**< Frame type. */
} __plt_aligned(2);

struct roc_npc_flow_item_eth {
	union {
		struct {
			/*
			 * These fields are retained
			 * for compatibility.
			 * Please switch to the new header field below.
			 */
			struct roc_ether_addr dst; /**< Destination MAC. */
			struct roc_ether_addr src; /**< Source MAC. */
			uint16_t type;		   /**< EtherType or TPID. */
		};
		struct roc_ether_hdr hdr;
	};
	uint32_t has_vlan : 1; /**< Packet header contains at least one VLAN. */
	uint32_t reserved : 31; /**< Reserved, must be zero. */
};

struct __plt_packed_begin roc_vlan_hdr {
	uint16_t vlan_tci; /**< Priority (3) + CFI (1) + Identifier Code (12) */
	uint16_t eth_proto; /**< Ethernet type of encapsulated frame. */
} __plt_packed_end;

struct roc_npc_flow_item_vlan {
	union {
		struct {
			uint16_t tci;	     /**< Tag control information. */
			uint16_t inner_type; /**< Inner EtherType or TPID. */
		};
		struct roc_vlan_hdr hdr;
	};
	uint32_t has_more_vlan : 1;
	/**< Packet header contains at least one more VLAN, after this VLAN. */
	uint32_t reserved : 31; /**< Reserved, must be zero. */
};

struct __plt_packed_begin roc_ipv6_hdr {
	uint32_t vtc_flow;    /**< IP version, traffic class & flow label. */
	uint16_t payload_len; /**< IP payload size, including ext. headers */
	uint8_t proto;	      /**< Protocol, next header. */
	uint8_t hop_limits;   /**< Hop limits. */
	uint8_t src_addr[16]; /**< IP address of source host. */
	uint8_t dst_addr[16]; /**< IP address of destination host(s). */
} __plt_packed_end;

struct __plt_packed_begin roc_ipv6_fragment_ext {
	uint8_t next_header; /**< Next header type */
	uint8_t reserved;    /**< Reserved */
	uint16_t frag_data;  /**< All fragmentation data */
	uint32_t id;	     /**< Packet ID */
} __plt_packed_end;

struct __plt_packed_begin roc_ipv6_routing_ext {
	uint8_t next_hdr;	/**< Protocol, next header. */
	uint8_t hdr_len;	/**< Header length. */
	uint8_t type;		/**< Extension header type. */
	uint8_t segments_left;	/**< Valid segments number. */
	union {
		uint32_t flags; /**< Packet control data per type. */
		struct {
			uint8_t last_entry; /**< The last_entry field of SRH */
			uint8_t flag;	    /**< Packet flag. */
			uint16_t tag;	    /**< Packet tag. */
		};
	};
	/* Next are 128-bit IPv6 address fields to describe segments. */
} __plt_packed_end;

struct roc_flow_item_ipv6_ext {
	uint8_t next_hdr; /**< Next header. */
};

struct roc_npc_flow_item_ipv6 {
	struct roc_ipv6_hdr hdr; /**< IPv6 header definition. */
	uint32_t has_hop_ext : 1;
	/**< Header contains Hop-by-Hop Options extension header. */
	uint32_t has_route_ext : 1;
	/**< Header contains Routing extension header. */
	uint32_t has_frag_ext : 1;
	/**< Header contains Fragment extension header. */
	uint32_t has_auth_ext : 1;
	/**< Header contains Authentication extension header. */
	uint32_t has_esp_ext : 1;
	/**< Header contains Encapsulation Security Payload extension header. */
	uint32_t has_dest_ext : 1;
	/**< Header contains Destination Options extension header. */
	uint32_t has_mobil_ext : 1;
	/**< Header contains Mobility extension header. */
	uint32_t has_hip_ext : 1;
	/**< Header contains Host Identity Protocol extension header. */
	uint32_t has_shim6_ext : 1;
	/**< Header contains Shim6 Protocol extension header. */
	uint32_t reserved : 23;
	/**< Reserved for future extension headers, must be zero. */
};

#define ROC_NPC_MAX_ACTION_COUNT 19

enum roc_npc_action_type {
	ROC_NPC_ACTION_TYPE_END = (1 << 0),
	ROC_NPC_ACTION_TYPE_VOID = (1 << 1),
	ROC_NPC_ACTION_TYPE_MARK = (1 << 2),
	ROC_NPC_ACTION_TYPE_FLAG = (1 << 3),
	ROC_NPC_ACTION_TYPE_DROP = (1 << 4),
	ROC_NPC_ACTION_TYPE_QUEUE = (1 << 5),
	ROC_NPC_ACTION_TYPE_RSS = (1 << 6),
	ROC_NPC_ACTION_TYPE_DUP = (1 << 7),
	ROC_NPC_ACTION_TYPE_SEC = (1 << 8),
	ROC_NPC_ACTION_TYPE_COUNT = (1 << 9),
	ROC_NPC_ACTION_TYPE_PF = (1 << 10),
	ROC_NPC_ACTION_TYPE_VF = (1 << 11),
	ROC_NPC_ACTION_TYPE_VLAN_STRIP = (1 << 12),
	ROC_NPC_ACTION_TYPE_VLAN_INSERT = (1 << 13),
	ROC_NPC_ACTION_TYPE_VLAN_ETHTYPE_INSERT = (1 << 14),
	ROC_NPC_ACTION_TYPE_VLAN_PCP_INSERT = (1 << 15),
	ROC_NPC_ACTION_TYPE_PORT_ID = (1 << 16),
	ROC_NPC_ACTION_TYPE_METER = (1 << 17),
	ROC_NPC_ACTION_TYPE_AGE = (1 << 18),
	ROC_NPC_ACTION_TYPE_SAMPLE = (1 << 19),
};

struct roc_npc_action {
	enum roc_npc_action_type type; /**< Action type. */
	const void *conf;	       /**< Pointer to action configuration object. */
	uint16_t rss_repte_pf_func;    /**< Per flow tmp var for rss representee pffunc */
};

struct roc_npc_action_sample {
	uint32_t ratio;	      /**< packets sampled equals to '1/ratio'. */
	uint32_t action_type; /* PF or VF or PORT_ID target. */
	uint16_t pf_func;
	uint16_t channel;
};

struct roc_npc_action_mark {
	uint32_t id; /**< Integer value to return with packets. */
};

struct roc_npc_action_vf {
	uint32_t original : 1;	/**< Use original VF ID if possible. */
	uint32_t reserved : 31; /**< Reserved, must be zero. */
	uint32_t id;		/**< VF ID. */
};

struct roc_npc_action_port_id {
	uint32_t original : 1;	/**< Use original port ID if possible. */
	uint32_t reserved : 31; /**< Reserved, must be zero. */
	uint32_t id;		/**< port ID. */
};

struct roc_npc_action_age {
	uint32_t timeout : 24; /**< Time in seconds. */
	uint32_t reserved : 8; /**< Reserved, must be zero. */
	/** The user flow context, NULL means the flow pointer. */
	void *context;
};

/**
 * ESP Header
 */
struct roc_npc_item_esp_hdr {
	uint32_t spi; /**< Security Parameters Index */
	uint32_t seq; /**< packet sequence number */
};

struct roc_npc_action_queue {
	uint16_t index; /**< Queue index to use. */
};

struct roc_npc_action_of_push_vlan {
	uint16_t ethertype; /**< EtherType. */
};

struct roc_npc_action_of_set_vlan_vid {
	uint16_t vlan_vid; /**< VLAN id. */
};

struct roc_npc_action_of_set_vlan_pcp {
	uint8_t vlan_pcp; /**< VLAN priority. */
};

struct roc_npc_action_meter {
	uint32_t mtr_id; /**< Meter id to be applied. > */
};

enum roc_npc_sec_action_alg {
	ROC_NPC_SEC_ACTION_ALG0,
	ROC_NPC_SEC_ACTION_ALG1,
	ROC_NPC_SEC_ACTION_ALG2,
	ROC_NPC_SEC_ACTION_ALG3,
	ROC_NPC_SEC_ACTION_ALG4,
};

struct roc_npc_sec_action {
	/* Used as lookup result for ALG3 */
	uint32_t sa_index;
	/* When true XOR initial SA_INDEX with SA_HI/SA_LO to get SA_MCAM */
	bool sa_xor;
	uint16_t sa_hi, sa_lo;
	/* Determines alg to be applied post SA_MCAM computation with/without
	 * XOR
	 */
	enum roc_npc_sec_action_alg alg;
};

struct roc_npc_attr {
	uint32_t priority;	/**< Rule priority level within group. */
	uint32_t ingress : 1;	/**< Rule applies to ingress traffic. */
	uint32_t egress : 1;	/**< Rule applies to egress traffic. */
	uint32_t reserved : 30; /**< Reserved, must be zero. */
};

struct roc_npc_flow_dump_data {
	uint8_t lid;
	uint16_t ltype;
};

struct roc_npc_spi_to_sa_action_info {
	uint32_t spi;
	uint32_t hash_index;
	uint8_t way;
	bool duplicate;
	bool has_action;
};

struct mbox;
struct roc_npc_flow {
	uint8_t nix_intf;
	uint8_t enable;
	uint8_t use_ctr;
	bool is_validate;
	uint32_t mcam_id;
	uint16_t tx_pf_func;
	bool has_age_action;
	bool rep_act_rep;
	int32_t ctr_id;
	uint32_t priority;
	uint32_t mtr_id;
#if defined(ROC_PLATFORM_CN20K)
#define ROC_NPC_MAX_MCAM_WIDTH_DWORDS 8
#define ROC_NPC_MAX_MCAM_PRIORITY     123
#else
#define ROC_NPC_MAX_MCAM_WIDTH_DWORDS 7
#define ROC_NPC_MAX_MCAM_PRIORITY     32
#endif
	/* Contiguous match string */
	uint64_t mcam_data[ROC_NPC_MAX_MCAM_WIDTH_DWORDS];
	uint64_t mcam_mask[ROC_NPC_MAX_MCAM_WIDTH_DWORDS];
	uint64_t npc_action;
	uint64_t npc_action2;
	uint64_t vtag_action;
	bool vtag_insert_enabled;
	int8_t vtag_insert_count;
#define ROC_NPC_MAX_FLOW_PATTERNS 32
	struct roc_npc_flow_dump_data dump_data[ROC_NPC_MAX_FLOW_PATTERNS];
	uint16_t num_patterns;
	struct roc_npc_spi_to_sa_action_info spi_to_sa_info;
	uint16_t match_id;
	uint8_t is_inline_dev;
	bool use_pre_alloc;
	uint64_t timeout_cycles;
	void *age_context;
	uint16_t rep_act_pf_func;
	bool is_rep_vf;
	bool has_rep;
	bool is_sampling_rule;
	uint16_t rep_pf_func;
#define ROC_NPC_MIRROR_LIST_SIZE 2
	uint16_t mcast_pf_funcs[ROC_NPC_MIRROR_LIST_SIZE];
	uint16_t rep_channel;
	struct mbox *rep_mbox;
	struct npc *rep_npc;
	int port_id;
	uint16_t mcast_channels[ROC_NPC_MIRROR_LIST_SIZE];
	uint32_t recv_queue;
	uint32_t mcast_grp_index;
	uint32_t mce_start_index;
	uint8_t key_type;
	uint32_t timeout;

	TAILQ_ENTRY(roc_npc_flow) next;
};

enum roc_npc_rss_hash_function {
	ROC_NPC_RSS_HASH_FUNCTION_DEFAULT = 0,
	ROC_NPC_RSS_HASH_FUNCTION_TOEPLITZ,   /**< Toeplitz */
	ROC_NPC_RSS_HASH_FUNCTION_SIMPLE_XOR, /**< Simple XOR */
	ROC_NPC_RSS_HASH_FUNCTION_SYMMETRIC_TOEPLITZ,
	ROC_NPC_RSS_HASH_FUNCTION_MAX,
};

struct roc_npc_action_rss {
	enum roc_npc_rss_hash_function func;
	uint32_t level;
	uint64_t types;	       /**< Specific RSS hash types (see ETH_RSS_*). */
	uint32_t key_len;      /**< Hash key length in bytes. */
	uint32_t queue_num;    /**< Number of entries in @p queue. */
	const uint8_t *key;    /**< Hash key. */
	const uint16_t *queue; /**< Queue indices to use. */
};

enum roc_npc_intf {
	ROC_NPC_INTF_RX = 0,
	ROC_NPC_INTF_TX = 1,
	ROC_NPC_INTF_MAX = 2,
};

enum flow_vtag_cfg_dir { VTAG_TX, VTAG_RX };
#define ROC_ETHER_TYPE_VLAN 0x8100 /**< IEEE 802.1Q VLAN tagging. */
#define ROC_ETHER_TYPE_QINQ 0x88A8 /**< IEEE 802.1ad QinQ tagging. */

struct roc_npc_flow_age {
	plt_seqcount_t seq_cnt;
	uint32_t aging_poll_freq;
	uint32_t age_flow_refcnt;
	uint32_t aged_flows_cnt;
	uint32_t start_id;
	uint32_t end_id;
	plt_thread_t aged_flows_poll_thread;
	struct plt_bitmap *aged_flows;
	void *age_mem;
	bool aged_flows_get_thread_exit;
};

struct roc_npc {
	struct roc_nix *roc_nix;
	uint8_t switch_header_type;
	uint8_t pre_l2_size_offset;	 /**< Offset with in header that holds
					  * size of custom header
					  */
	uint8_t pre_l2_size_offset_mask; /**< Offset mask with in header
					  * that holds size of custom header
					  */
	uint8_t pre_l2_size_shift_dir;	 /**< Shift direction to calculate size
					  */
	uint16_t flow_prealloc_size;
	uint16_t flow_max_priority;
	uint16_t channel;
	uint16_t pf_func;
	bool is_sdp_mask_set;
	bool rep_act_rep;
	uint16_t sdp_channel_mask;
	uint64_t kex_capability;
	uint64_t rx_parse_nibble;
	/* Parsed RSS Flowkey cfg for current flow being created */
	uint32_t flowkey_cfg_state;
	uint32_t max_entries;
	uint16_t rep_act_pf_func;
	uint16_t sdp_channel;
	struct roc_npc_flow_age flow_age;
	struct roc_npc *rep_npc;
	uint16_t rep_pf_func;
	uint16_t rep_rx_channel;
	int rep_port_id;

#define ROC_NPC_MEM_SZ (20 * 1024)
	uint8_t reserved[ROC_NPC_MEM_SZ];
} __plt_cache_aligned;

#define ROC_NPC_AGE_POLL_FREQ_MIN 10

int __roc_api roc_npc_init(struct roc_npc *roc_npc);
int __roc_api roc_npc_fini(struct roc_npc *roc_npc);
const char *__roc_api roc_npc_profile_name_get(struct roc_npc *roc_npc);
int __roc_api roc_npc_kex_capa_get(struct roc_nix *roc_nix, uint64_t *kex_capability);

struct roc_npc_flow *__roc_api roc_npc_flow_create(struct roc_npc *roc_npc,
						   const struct roc_npc_attr *attr,
						   const struct roc_npc_item_info pattern[],
						   const struct roc_npc_action actions[],
						   uint16_t dst_pf_func, int *errcode);
int __roc_api roc_npc_flow_destroy(struct roc_npc *roc_npc, struct roc_npc_flow *flow);
int __roc_api roc_npc_mcam_free(struct roc_npc *roc_npc, struct roc_npc_flow *mcam);
int __roc_api roc_npc_mcam_free_entry(struct roc_npc *roc_npc, uint32_t entry);
int __roc_api roc_npc_mcam_enable_all_entries(struct roc_npc *roc_npc, bool enable);
int __roc_api roc_npc_mcam_alloc_entry(struct roc_npc *roc_npc, struct roc_npc_flow *mcam,
				       struct roc_npc_flow *ref_mcam, int prio, int *resp_count);
int __roc_api roc_npc_mcam_alloc_entries(struct roc_npc *roc_npc, int ref_entry, int *alloc_entry,
					 int req_count, int priority, int *resp_count,
					 bool is_conti);
int __roc_api roc_npc_mcam_ena_dis_entry(struct roc_npc *roc_npc, struct roc_npc_flow *mcam,
					 bool enable);
int __roc_api roc_npc_mcam_write_entry(struct roc_npc *roc_npc, struct roc_npc_flow *mcam);
void __roc_api roc_npc_defrag_mcam_banks(struct roc_npc *roc_npc);
uint8_t __roc_api roc_npc_get_key_type(struct roc_npc *roc_npc, struct roc_npc_flow *flow);
uint8_t __roc_api roc_npc_kex_key_type_config_get(struct roc_npc *roc_npc);
int __roc_api roc_npc_flow_parse(struct roc_npc *roc_npc, const struct roc_npc_attr *attr,
				 const struct roc_npc_item_info pattern[],
				 const struct roc_npc_action actions[], struct roc_npc_flow *flow);
int __roc_api roc_npc_get_low_priority_mcam(struct roc_npc *roc_npc);
int __roc_api roc_npc_mcam_free_counter(struct roc_npc *roc_npc, uint16_t ctr_id);
int __roc_api roc_npc_mcam_read_counter(struct roc_npc *roc_npc, uint32_t ctr_id, uint64_t *count);
int __roc_api roc_npc_mcam_clear_counter(struct roc_npc *roc_npc, uint32_t ctr_id);
int __roc_api roc_npc_mcam_alloc_counter(struct roc_npc *roc_npc, uint16_t *ctr_id);
int __roc_api roc_npc_get_free_mcam_entry(struct roc_npc *roc_npc, struct roc_npc_flow *flow);
int __roc_api roc_npc_inl_mcam_read_counter(uint32_t ctr_id, uint64_t *count);
int __roc_api roc_npc_inl_mcam_clear_counter(uint32_t ctr_id);
int __roc_api roc_npc_mcam_free_all_resources(struct roc_npc *roc_npc);
void __roc_api roc_npc_flow_dump(FILE *file, struct roc_npc *roc_npc, int rep_port_id);
void __roc_api roc_npc_flow_mcam_dump(FILE *file, struct roc_npc *roc_npc,
				      struct roc_npc_flow *mcam);
int __roc_api roc_npc_mark_actions_get(struct roc_npc *roc_npc);
int __roc_api roc_npc_mark_actions_sub_return(struct roc_npc *roc_npc, uint32_t count);
int __roc_api roc_npc_vtag_actions_get(struct roc_npc *roc_npc);
int __roc_api roc_npc_vtag_actions_sub_return(struct roc_npc *roc_npc, uint32_t count);
int __roc_api roc_npc_mcam_merge_base_steering_rule(struct roc_npc *roc_npc,
						    struct roc_npc_flow *flow);
int __roc_api roc_npc_validate_portid_action(struct roc_npc *roc_npc_src,
					     struct roc_npc *roc_npc_dst);
int __roc_api roc_npc_mcam_init(struct roc_npc *roc_npc, struct roc_npc_flow *flow, int mcam_id);
int __roc_api roc_npc_mcam_move(struct roc_npc *roc_npc, uint16_t old_ent, uint16_t new_ent);
void *__roc_api roc_npc_aged_flow_ctx_get(struct roc_npc *roc_npc, uint32_t mcam_id);
void __roc_api roc_npc_sdp_channel_get(struct roc_npc *roc_npc, uint16_t *chan_base,
				       uint16_t *chan_mask);
int __roc_api roc_npc_mcam_get_stats(struct roc_npc *roc_npc, struct roc_npc_flow *flow,
				     uint64_t *count);
#endif /* _ROC_NPC_H_ */
