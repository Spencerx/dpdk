/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2016-2017 Intel Corporation
 */
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_crypto.h>
#include <rte_cryptodev.h>

#include "cperf_test_latency.h"
#include "cperf_ops.h"
#include "cperf_test_common.h"

struct cperf_op_result {
	uint64_t tsc_start;
	uint64_t tsc_end;
	enum rte_crypto_op_status status;
};

struct cperf_latency_ctx {
	uint8_t dev_id;
	uint16_t qp_id;
	uint8_t lcore_id;

	struct rte_mempool *pool;

	void *sess;
	uint8_t sess_owner;

	cperf_populate_ops_t populate_ops;

	uint32_t src_buf_offset;
	uint32_t dst_buf_offset;

	const struct cperf_options *options;
	const struct cperf_test_vector *test_vector;
	struct cperf_op_result *res;
};

struct priv_op_data {
	struct cperf_op_result *result;
};

static void
cperf_latency_test_free(struct cperf_latency_ctx *ctx)
{
	if (ctx == NULL)
		return;

	if (ctx->sess != NULL && ctx->sess_owner) {
		if (cperf_is_asym_test(ctx->options))
			rte_cryptodev_asym_session_free(ctx->dev_id, ctx->sess);
#ifdef RTE_LIB_SECURITY
		else if (ctx->options->op_type == CPERF_PDCP ||
			 ctx->options->op_type == CPERF_DOCSIS ||
			 ctx->options->op_type == CPERF_TLS ||
			 ctx->options->op_type == CPERF_IPSEC) {
			void *sec_ctx = rte_cryptodev_get_sec_ctx(ctx->dev_id);
			rte_security_session_destroy(sec_ctx, ctx->sess);
		}
#endif
		else
			rte_cryptodev_sym_session_free(ctx->dev_id, ctx->sess);
	}

	rte_mempool_free(ctx->pool);
	rte_free(ctx->res);
	rte_free(ctx);
}

void *
cperf_latency_test_constructor(struct rte_mempool *sess_mp,
		uint8_t dev_id, uint16_t qp_id,
		const struct cperf_options *options,
		const struct cperf_test_vector *test_vector,
		const struct cperf_op_fns *op_fns,
		void **sess)
{
	struct cperf_latency_ctx *ctx = NULL;
	size_t extra_op_priv_size = sizeof(struct priv_op_data);

	ctx = rte_malloc(NULL, sizeof(struct cperf_latency_ctx), 0);
	if (ctx == NULL)
		goto err;

	ctx->dev_id = dev_id;
	ctx->qp_id = qp_id;

	ctx->populate_ops = op_fns->populate_ops;
	ctx->options = options;
	ctx->test_vector = test_vector;

	/* IV goes at the end of the crypto operation */
	uint16_t iv_offset = sizeof(struct rte_crypto_op) +
		sizeof(struct rte_crypto_sym_op) +
		sizeof(struct cperf_op_result *);


	if (*sess != NULL) {
		ctx->sess = *sess;
		ctx->sess_owner = false;
	} else {
		ctx->sess = op_fns->sess_create(sess_mp, dev_id, options, test_vector,
			iv_offset);
		if (ctx->sess == NULL)
			goto err;
		*sess = ctx->sess;
		ctx->sess_owner = true;
	}

	if (cperf_alloc_common_memory(options, test_vector, dev_id, qp_id,
			extra_op_priv_size,
			&ctx->src_buf_offset, &ctx->dst_buf_offset,
			&ctx->pool) < 0)
		goto err;

	ctx->res = rte_malloc(NULL, sizeof(struct cperf_op_result) *
			ctx->options->total_ops, 0);

	if (ctx->res == NULL)
		goto err;

	return ctx;
err:
	cperf_latency_test_free(ctx);

	return NULL;
}

static inline void
store_timestamp(struct rte_crypto_op *op, uint64_t timestamp)
{
	struct priv_op_data *priv_data;

	if (op->type == RTE_CRYPTO_OP_TYPE_SYMMETRIC)
		priv_data = (struct priv_op_data *) (op->sym + 1);
	else
		priv_data = (struct priv_op_data *) (op->asym + 1);

	priv_data->result->status = op->status;
	priv_data->result->tsc_end = timestamp;
}

static int
cperf_check_single_op(struct cperf_latency_ctx *ctx, uint16_t iv_offset)
{
	struct rte_crypto_op *ops[1];
	uint32_t imix_idx = 0;
	uint64_t tsc_start = 0;
	uint64_t ops_enqd = 0, ops_deqd = 0;

	/* Allocate object containing crypto operations and mbufs */
	if (rte_mempool_get(ctx->pool, (void **)&ops[0]) != 0) {
		RTE_LOG(ERR, USER1,
			"Failed to allocate crypto operation "
			"from the crypto operation pool.\n"
			"Consider increasing the pool size "
			"with --pool-sz\n");
		return -1;
	}

	/* Setup crypto op, attach mbuf etc */
	(ctx->populate_ops)(ops, ctx->src_buf_offset,
		ctx->dst_buf_offset,
		1, ctx->sess, ctx->options,
		ctx->test_vector, iv_offset,
		&imix_idx, &tsc_start);

	ops_enqd = rte_cryptodev_enqueue_burst(ctx->dev_id, ctx->qp_id, ops, 1);
	if (ops_enqd != 1) {
		RTE_LOG(ERR, USER1, "PMD cannot process the packet.\n");
		return -1;
	}

	/* Dequeue processed burst of ops from crypto device */
	tsc_start = rte_rdtsc_precise();
	while (1) {
		ops_deqd = rte_cryptodev_dequeue_burst(ctx->dev_id, ctx->qp_id,
				ops, 1);

		if (ops_deqd == 1) {
			rte_mempool_put(ctx->pool, ops[0]);
			return 1;
		}

		/* Check if 1 second timeout has been reached */
		if ((rte_rdtsc_precise() - tsc_start) > rte_get_tsc_hz()) {
			RTE_LOG(ERR, USER1, "Dequeue operation timed out.\n");
			return -1;
		}
	}
}

int
cperf_latency_test_runner(void *arg)
{
	struct cperf_latency_ctx *ctx = arg;
	uint16_t test_burst_size;
	uint8_t burst_size_idx = 0;
	uint32_t imix_idx = 0;
	int ret = 0;

	static RTE_ATOMIC(uint16_t) display_once;

	if (ctx == NULL)
		return 0;

	struct rte_crypto_op *ops[ctx->options->max_burst_size];
	struct rte_crypto_op *ops_processed[ctx->options->max_burst_size];
	uint64_t i;
	struct priv_op_data *priv_data;

	uint32_t lcore = rte_lcore_id();

#ifdef CPERF_LINEARIZATION_ENABLE
	struct rte_cryptodev_info dev_info;
	int linearize = 0;

	/* Check if source mbufs require coalescing */
	if (ctx->options->segment_sz < ctx->options->max_buffer_size) {
		rte_cryptodev_info_get(ctx->dev_id, &dev_info);
		if ((dev_info.feature_flags &
				RTE_CRYPTODEV_FF_MBUF_SCATTER_GATHER) == 0)
			linearize = 1;
	}
#endif /* CPERF_LINEARIZATION_ENABLE */

	ctx->lcore_id = lcore;

	/* Warm up the host CPU before starting the test */
	for (i = 0; i < ctx->options->total_ops; i++)
		rte_cryptodev_enqueue_burst(ctx->dev_id, ctx->qp_id, NULL, 0);

	/* Get first size from range or list */
	if (ctx->options->inc_burst_size != 0)
		test_burst_size = ctx->options->min_burst_size;
	else
		test_burst_size = ctx->options->burst_size_list[0];

	uint16_t iv_offset = sizeof(struct rte_crypto_op) +
		sizeof(struct rte_crypto_sym_op) +
		sizeof(struct cperf_op_result *);

	/* Enqueue just one operation to check whether PMD returns error */
	if (cperf_check_single_op(ctx, iv_offset) < 1)
		return -1;

	while (test_burst_size <= ctx->options->max_burst_size) {
		uint64_t ops_enqd = 0, ops_deqd = 0;
		uint64_t b_idx = 0;

		uint64_t tsc_val, tsc_end, tsc_start;
		uint64_t tsc_max = 0, tsc_min = ~0UL, tsc_tot = 0, tsc_idx = 0;
		uint64_t enqd_max = 0, enqd_min = ~0UL, enqd_tot = 0;
		uint64_t deqd_max = 0, deqd_min = ~0UL, deqd_tot = 0;

		while (enqd_tot < ctx->options->total_ops) {

			uint16_t burst_size = ((enqd_tot + test_burst_size)
					<= ctx->options->total_ops) ?
							test_burst_size :
							ctx->options->total_ops -
							enqd_tot;

			/* Allocate objects containing crypto operations and mbufs */
			if (rte_mempool_get_bulk(ctx->pool, (void **)ops,
						burst_size) != 0) {
				RTE_LOG(ERR, USER1,
					"Failed to allocate more crypto operations "
					"from the crypto operation pool.\n"
					"Consider increasing the pool size "
					"with --pool-sz\n");
				return -1;
			}

			/* Setup crypto op, attach mbuf etc */
			(ctx->populate_ops)(ops, ctx->src_buf_offset,
					ctx->dst_buf_offset,
					burst_size, ctx->sess, ctx->options,
					ctx->test_vector, iv_offset,
					&imix_idx, &tsc_start);

			/* Populate the mbuf with the test vector */
			if (!cperf_is_asym_test(ctx->options))
				for (i = 0; i < burst_size; i++)
					cperf_mbuf_set(ops[i]->sym->m_src,
						ctx->options,
						ctx->test_vector);

			tsc_start = rte_rdtsc_precise();

#ifdef CPERF_LINEARIZATION_ENABLE
			if (linearize) {
				/* PMD doesn't support scatter-gather and source buffer
				 * is segmented.
				 * We need to linearize it before enqueuing.
				 */
				for (i = 0; i < burst_size; i++)
					rte_pktmbuf_linearize(ops[i]->sym->m_src);
			}
#endif /* CPERF_LINEARIZATION_ENABLE */

			/* Enqueue burst of ops on crypto device */
			ops_enqd = rte_cryptodev_enqueue_burst(ctx->dev_id, ctx->qp_id,
					ops, burst_size);

			/* Dequeue processed burst of ops from crypto device */
			ops_deqd = rte_cryptodev_dequeue_burst(ctx->dev_id, ctx->qp_id,
					ops_processed, test_burst_size);

			tsc_end = rte_rdtsc_precise();

			/* Free memory for not enqueued operations */
			if (ops_enqd != burst_size)
				rte_mempool_put_bulk(ctx->pool,
						(void **)&ops[ops_enqd],
						burst_size - ops_enqd);

			for (i = 0; i < ops_enqd; i++) {
				ctx->res[tsc_idx].tsc_start = tsc_start;
				/*
				 * Private data structure starts after the end of the
				 * rte_crypto_sym_op (or rte_crypto_asym_op) structure.
				 */
				if (ops[i]->type == RTE_CRYPTO_OP_TYPE_SYMMETRIC)
					priv_data = (struct priv_op_data *) (ops[i]->sym + 1);
				else
					priv_data = (struct priv_op_data *) (ops[i]->asym + 1);

				priv_data->result = (void *)&ctx->res[tsc_idx];
				tsc_idx++;
			}

			if (likely(ops_deqd))  {
				for (i = 0; i < ops_deqd; i++) {
					struct rte_crypto_op *op = ops_processed[i];

					if (op->status != RTE_CRYPTO_OP_STATUS_SUCCESS)
						ret = -1;

					store_timestamp(ops_processed[i], tsc_end);
				}

				/* Free crypto ops so they can be reused. */
				rte_mempool_put_bulk(ctx->pool,
						(void **)ops_processed, ops_deqd);

				deqd_tot += ops_deqd;
				deqd_max = RTE_MAX(ops_deqd, deqd_max);
				deqd_min = RTE_MIN(ops_deqd, deqd_min);
			}

			enqd_tot += ops_enqd;
			enqd_max = RTE_MAX(ops_enqd, enqd_max);
			enqd_min = RTE_MIN(ops_enqd, enqd_min);

			b_idx++;
		}

		/* Dequeue any operations still in the crypto device */
		while (deqd_tot < ctx->options->total_ops) {
			/* Sending 0 length burst to flush sw crypto device */
			rte_cryptodev_enqueue_burst(ctx->dev_id, ctx->qp_id, NULL, 0);

			/* dequeue burst */
			ops_deqd = rte_cryptodev_dequeue_burst(ctx->dev_id, ctx->qp_id,
					ops_processed, test_burst_size);

			tsc_end = rte_rdtsc_precise();

			if (ops_deqd != 0) {
				for (i = 0; i < ops_deqd; i++) {
					struct rte_crypto_op *op = ops_processed[i];

					if (op->status != RTE_CRYPTO_OP_STATUS_SUCCESS)
						ret = -1;

					store_timestamp(ops_processed[i], tsc_end);
				}

				rte_mempool_put_bulk(ctx->pool,
						(void **)ops_processed, ops_deqd);

				deqd_tot += ops_deqd;
				deqd_max = RTE_MAX(ops_deqd, deqd_max);
				deqd_min = RTE_MIN(ops_deqd, deqd_min);
			}
		}

		/* If there was any failure in crypto op, exit */
		if (ret)
			return ret;

		for (i = 0; i < tsc_idx; i++) {
			tsc_val = ctx->res[i].tsc_end - ctx->res[i].tsc_start;
			tsc_max = RTE_MAX(tsc_val, tsc_max);
			tsc_min = RTE_MIN(tsc_val, tsc_min);
			tsc_tot += tsc_val;
		}

		double time_tot, time_avg, time_max, time_min;

		const uint64_t tunit = 1000000; /* us */
		const uint64_t tsc_hz = rte_get_tsc_hz();

		uint64_t enqd_avg = enqd_tot / b_idx;
		uint64_t deqd_avg = deqd_tot / b_idx;
		uint64_t tsc_avg = tsc_tot / tsc_idx;

		time_tot = tunit*(double)(tsc_tot) / tsc_hz;
		time_avg = tunit*(double)(tsc_avg) / tsc_hz;
		time_max = tunit*(double)(tsc_max) / tsc_hz;
		time_min = tunit*(double)(tsc_min) / tsc_hz;

		uint16_t exp = 0;
		if (ctx->options->csv) {
			if (rte_atomic_compare_exchange_strong_explicit(&display_once, &exp, 1,
					rte_memory_order_relaxed, rte_memory_order_relaxed))
				printf("\n# lcore, Buffer Size, Burst Size, Pakt Seq #, "
						"cycles, time (us)");

			for (i = 0; i < ctx->options->total_ops; i++) {

				printf("\n%u,%u,%u,%"PRIu64",%"PRIu64",%.3f",
					ctx->lcore_id, ctx->options->test_buffer_size,
					test_burst_size, i + 1,
					ctx->res[i].tsc_end - ctx->res[i].tsc_start,
					tunit * (double) (ctx->res[i].tsc_end
							- ctx->res[i].tsc_start)
						/ tsc_hz);

			}
		} else {
			printf("\n# Device %d on lcore %u\n", ctx->dev_id,
				ctx->lcore_id);
			printf("\n# total operations: %u", ctx->options->total_ops);
			printf("\n# Buffer size: %u", ctx->options->test_buffer_size);
			printf("\n# Burst size: %u", test_burst_size);
			printf("\n#     Number of bursts: %"PRIu64,
					b_idx);

			printf("\n#");
			printf("\n#          \t       Total\t   Average\t   "
					"Maximum\t   Minimum");
			printf("\n#  enqueued\t%12"PRIu64"\t%10"PRIu64"\t"
					"%10"PRIu64"\t%10"PRIu64, enqd_tot,
					enqd_avg, enqd_max, enqd_min);
			printf("\n#  dequeued\t%12"PRIu64"\t%10"PRIu64"\t"
					"%10"PRIu64"\t%10"PRIu64, deqd_tot,
					deqd_avg, deqd_max, deqd_min);
			printf("\n#    cycles\t%12"PRIu64"\t%10"PRIu64"\t"
					"%10"PRIu64"\t%10"PRIu64, tsc_tot,
					tsc_avg, tsc_max, tsc_min);
			printf("\n# time [us]\t%12.0f\t%10.3f\t%10.3f\t%10.3f",
					time_tot, time_avg, time_max, time_min);
			printf("\n\n");

		}

		/* Get next size from range or list */
		if (ctx->options->inc_burst_size != 0)
			test_burst_size += ctx->options->inc_burst_size;
		else {
			if (++burst_size_idx == ctx->options->burst_size_count)
				break;
			test_burst_size =
				ctx->options->burst_size_list[burst_size_idx];
		}
	}

	return 0;
}

void
cperf_latency_test_destructor(void *arg)
{
	struct cperf_latency_ctx *ctx = arg;

	if (ctx == NULL)
		return;

	cperf_latency_test_free(ctx);
}
