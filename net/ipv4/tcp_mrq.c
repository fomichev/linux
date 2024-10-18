// SPDX-License-Identifier: GPL-2.0-or-later

#include <net/tcp.h>
#include <linux/skbuff_ref.h>

#include "../core/devmem.h"

static void tcp_mrq_update_producer(struct mrq_ring *r)
{
	r->cached_producer = READ_ONCE(r->ring->producer);
}

static void tcp_mrq_update_consumer(struct mrq_ring *r)
{
	r->cached_consumer = READ_ONCE(r->ring->consumer);
}

static void tcp_mrq_consume(struct mrq_ring *r, u32 nr)
{
	r->cached_consumer += nr;
}

static void tcp_mrq_consume_err(struct mrq_ring *r, u32 nr)
{
	if (unlikely(nr > 0))
		WRITE_ONCE(r->ring->consumer_err, READ_ONCE(r->ring->consumer_err) + nr);
}

static void tcp_mrq_produce(struct mrq_ring *r, u32 nr)
{
	r->cached_producer += nr;
}

static void tcp_mrq_produce_err(struct mrq_ring *r)
{
	WRITE_ONCE(r->ring->producer_err, READ_ONCE(r->ring->producer_err) + 1);
}

static void tcp_mrq_flush_producer(struct mrq_ring *r)
{
	if (r->flushed_producer == r->cached_producer)
		return;

	WRITE_ONCE(r->ring->producer, r->cached_producer);
	r->flushed_producer = r->cached_producer;
}

static void tcp_mrq_flush_consumer(struct mrq_ring *r)
{
	if (r->flushed_consumer == r->cached_consumer)
		return;

	WRITE_ONCE(r->ring->consumer, r->cached_consumer);
	r->flushed_consumer = r->cached_consumer;
}

int tcp_mrq_alloc(struct sock *sk, struct tcp_mrq_alloc *opt)
{
	if (sk->sk_mrq.cmsg.ring)
		return -EBUSY;

	sk->sk_mrq.cmsg.ring = vmalloc_user(opt->size);
	sk->sk_mrq.token.ring = vmalloc_user(opt->size);
	sk->sk_mrq.linear.ring = vmalloc_user(opt->size);
	sk->sk_mrq.size = opt->size;

	if (!sk->sk_mrq.cmsg.ring || !sk->sk_mrq.token.ring || !sk->sk_mrq.linear.ring) {
		vfree(sk->sk_mrq.cmsg.ring);
		vfree(sk->sk_mrq.token.ring);
		vfree(sk->sk_mrq.linear.ring);
		sk->sk_mrq.cmsg.ring = NULL;
		sk->sk_mrq.token.ring = NULL;
		sk->sk_mrq.linear.ring = NULL;
		sk->sk_mrq.size = 0;
		return -ENOMEM;
	}

	return 0;
}

int tcp_mrq_activate(struct sock *sk, struct tcp_mrq_activate *opt)
{
	int err;

	if (sk->sk_mrq.active) {
		/* only allow flipping msg_trunc for now */
		WRITE_ONCE(sk->sk_mrq.skip_copy, opt->skip_copy);
		return 0;
	}

	if (opt->pad)
		return -EINVAL;

	sk->sk_mrq.skip_xa = opt->skip_xa;
	sk->sk_mrq.skip_copy = opt->skip_copy;
	sk->sk_mrq.skip_wakeup = opt->skip_wakeup;
	sk->sk_mrq.active = true;

	err = tcp_mrq_recv(sk);
	if (err < 0)
		return err;

	return 0;
}

void tcp_mrq_shutdown(struct sock *sk)
{
#if 0
	struct mrq_rings *r = &sk->sk_mrq;

	if (r->cmsg.ring) {
		vfree(r->cmsg.ring);
		vfree(r->token.ring);
		vfree(r->linear.ring);
		r->linear.ring = NULL;
		r->cmsg.ring = NULL;
		r->token.ring = NULL;
		r->size = 0;
		r->active = false;
	}
#endif
}

int tcp_mrq_mmap(struct file *file, struct socket *sock, struct vm_area_struct *vma)
{
	struct mrq_rings *r = &sock->sk->sk_mrq;
	void *addr;

	if (!r->size)
		return -ENOENT;

	if (!IS_ALIGNED(r->size, PAGE_SIZE))
		return -ERANGE;

	if (r->size != vma->vm_end - vma->vm_start)
		return -EINVAL;

	if (vma->vm_pgoff == 1)
		addr = r->cmsg.ring;
	else if (vma->vm_pgoff == 2)
		addr = r->token.ring;
	else if (vma->vm_pgoff == 3)
		addr = r->linear.ring;
	else
		return -EINVAL;

	return remap_vmalloc_range(vma, addr, 0);
}

static bool tcp_mrq_post_linear(struct sock *sk, void *from, int nr, bool skip_copy)
{
	struct mrq_ring *r = &sk->sk_mrq.linear;
	int producer, consumer;
	void *to = r->ring->data;
	int cap, len, off;

	consumer = r->cached_consumer;
	producer = r->cached_producer;

	if (!skip_copy && LINR_SZ - (producer - consumer) < nr) {
		tcp_mrq_produce_err(r);
		return false;
	}

	off = producer % LINR_SZ;
	cap = LINR_SZ - off;
	len = min_t(int, cap, nr);
	tcp_mrq_produce(r, nr);

	if (!skip_copy)
		memcpy(to + off, from, len);
	nr -= len;
	if (nr > 0)
		if (!skip_copy)
			memcpy(to, from + len, nr);

	return true;
}

static void tcp_mrq_fin(struct sock *sk)
{
	struct mrq_ring *r = &sk->sk_mrq.cmsg;
	struct dmabuf_cmsg *dmabuf_cmsg;
	int producer, consumer;

	if (r->fin)
		return;

	consumer = r->cached_consumer;
	producer = r->cached_producer;

	if (producer - consumer >= CMSG_SZ) {
		tcp_mrq_produce_err(r);
		return;
	}

	dmabuf_cmsg = &r->ring->cmsg[producer % CMSG_SZ];
	memset(dmabuf_cmsg, 0, sizeof(*dmabuf_cmsg));
	tcp_mrq_produce(r, 1);

	r->fin = true;
}

static bool tcp_mrq_post(struct sock *sk, struct sk_buff *skb)
{
	struct mrq_ring *r = &sk->sk_mrq.cmsg;
	bool skip_copy = READ_ONCE(sk->sk_mrq.skip_copy);
	bool skip_xa = sk->sk_mrq.skip_xa;
	struct dmabuf_cmsg *dmabuf_cmsg;
	struct tcp_xa_pool tcp_xa_pool;
	int producer, consumer;
	struct sk_buff *iter;
	int nr = 0;
	int i;

	/* TODO: make sure errors are cleared before proceeding further */

	consumer = r->cached_consumer;
	producer = r->cached_producer;

	tcp_xa_pool.max = 0;
	tcp_xa_pool.idx = 0;

	if (skb_headlen(skb) > 0) {
		if (!tcp_mrq_post_linear(sk, skb->data, skb_headlen(skb), skip_copy))
			goto err;
	}

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		struct net_iov *niov;
		u64 frag_offset;

		if (producer - consumer >= CMSG_SZ) {
			tcp_mrq_produce_err(r);
			goto err;
		}

		if (!skb_frag_net_iov(frag)) {
			if (!tcp_mrq_post_linear(
				    sk,
				    skb_frag_address(frag) +
					    skb_frag_off(frag),
				    skb_frag_size(frag), skip_copy))
				goto err;
			continue;
		}
		niov = skb_frag_net_iov(frag);
		frag_offset = net_iov_virtual_addr(niov) + skb_frag_off(frag);

		dmabuf_cmsg = &r->ring->cmsg[(producer + nr++) % CMSG_SZ];
		dmabuf_cmsg->frag_offset = frag_offset;
		dmabuf_cmsg->frag_size = skb_frag_size(frag);
		dmabuf_cmsg->dmabuf_id = net_iov_binding_id(niov);

		if (skip_xa) {
			netmem_ref addr = net_iov_to_netmem(niov);

			dmabuf_cmsg->frag_token = addr; /* yikes */
			dmabuf_cmsg->flags = addr >> 32;
		} else {
			if (tcp_xa_pool_refill(sk, &tcp_xa_pool, skb_shinfo(skb)->nr_frags - i))
				goto err;
			dmabuf_cmsg->frag_token = tcp_xa_pool.tokens[tcp_xa_pool.idx];
			tcp_xa_pool.netmems[tcp_xa_pool.idx++] = skb_frag_netmem(frag);
		}

		atomic_long_inc(&niov->pp_ref_count);
	}

	tcp_xa_pool_commit(sk, &tcp_xa_pool);

	skb_walk_frags(skb, iter)
		if (!tcp_mrq_post(sk, iter))
			return false;

	tcp_mrq_produce(r, nr);

	return true;

err:
	tcp_xa_pool_commit(sk, &tcp_xa_pool);

	sk->sk_mrq.active = false;
	return false;
}

static int tcp_mrq_copy(read_descriptor_t *desc, struct sk_buff *in_skb,
			unsigned int offset, size_t in_len)
{
	struct sock *sk = desc->arg.data;

	/* TODO: support offset to interop with recvmsg */
	if (tcp_mrq_post(sk, in_skb))
		return in_len;

	return -ENOSPC;
}

static bool tcp_mrq_active(struct sock *sk)
{
	return sk->sk_mrq.active;
}

int tcp_mrq_recv(struct sock *sk)
{
	read_descriptor_t desc = {
		.count = 1,
		.arg.data = sk,
	};
	int err;

	if (!tcp_mrq_active(sk))
		return -EBADFD;

	tcp_mrq_update_consumer(&sk->sk_mrq.cmsg);
	tcp_mrq_update_producer(&sk->sk_mrq.token);
	tcp_mrq_update_consumer(&sk->sk_mrq.linear);

	__smp_mb();

	err = tcp_read_sock(sk, &desc, tcp_mrq_copy);
	if (err < 0)
		return err;

	switch (sk->sk_state) {
		case TCP_CLOSE_WAIT:
		case TCP_CLOSING:
		case TCP_FIN_WAIT1:
			tcp_mrq_fin(sk);
			break;
	}

	tcp_mrq_comp(sk);

	__smp_mb();

	tcp_mrq_flush_producer(&sk->sk_mrq.cmsg);
	tcp_mrq_flush_consumer(&sk->sk_mrq.token);
	tcp_mrq_flush_producer(&sk->sk_mrq.linear);

	return sk->sk_mrq.skip_wakeup ? 1 : 0;
}

void tcp_mrq_comp(struct sock *sk)
{
	struct mrq_ring *r = &sk->sk_mrq.token;
	bool skip_xa = sk->sk_mrq.skip_xa;
	unsigned int i, k, netmem_num = 0;
	struct dmabuf_token *token;
	int producer, consumer;
	netmem_ref netmems[16];
	int err = 0;
	int nr;

	if (!tcp_mrq_active(sk))
		return;

	consumer = r->cached_consumer;
	producer = r->cached_producer;

	nr = producer - consumer;
	if (!nr)
		return;

	if (!skip_xa)
		xa_lock_bh(&sk->sk_user_frags);
	for (i = 0; i < nr; i++) {
		netmem_ref netmem;

		token = &r->ring->token[(consumer + i) % TOKN_SZ];

		if (skip_xa) {
			u64 addr = ((u64)token->token_start) | (((u64)token->token_count) << 32);

			netmem = (__force netmem_ref)addr;
		} else {
			netmem = (__force netmem_ref)__xa_erase(
				&sk->sk_user_frags, token->token_start);
		}

		if (netmem &&
		    !WARN_ON_ONCE(!netmem_is_net_iov(netmem))) {
			netmems[netmem_num++] = netmem;
			if (netmem_num == ARRAY_SIZE(netmems)) {
				if (!skip_xa)
					xa_unlock_bh(&sk->sk_user_frags);
				for (k = 0; k < netmem_num; k++)
					if (!napi_pp_put_page(netmems[k]))
						err++;
				netmem_num = 0;
				if (!skip_xa)
					xa_lock_bh(&sk->sk_user_frags);
			}
		}
	}
	if (!skip_xa)
		xa_unlock_bh(&sk->sk_user_frags);

	for (k = 0; k < netmem_num; k++)
		if (!napi_pp_put_page(netmems[k]))
			err++;

	tcp_mrq_consume_err(r, err);
	tcp_mrq_consume(r, nr);
}
