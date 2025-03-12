// SPDX-License-Identifier: GPL-2.0-or-later

#include "net/netdev_lock.h"
#include "net/tcp_states.h"
#include <linux/if_vlan.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/express_socket.h>
#include <net/genetlink.h>
#include <net/inet_hashtables.h>
#include <net/inet6_hashtables.h>
#include <net/tcp.h>

#define FOR_REAL 1
#define DIRECT_INJECT 0

struct sx_cb {
	struct express_socket_flow *sxf;
};

#define SX_CB(skb) ((struct sx_cb *)(skb)->cb)

static struct express_socket_flow *sx_lookup(struct napi_struct *napi,
					     __u16 sport, __be16 dport,
					     const struct in6_addr *saddr,
					     const struct in6_addr *daddr,
					     int dif, int sdif)
{
	unsigned int hash = inet6_ehashfn(dev_net(napi->dev), saddr, sport, daddr, dport);
	__portpair ports = INET_COMBINED_PORTS(dport, sport);

	if (!napi->sx)
		return NULL;

	//pr_emerg("sx_lookup hash=%x sport=%x dport=%x\n", hash, sport, dport);

	for (unsigned int i = 0; i < EXPRESS_SOCKET_HT_LEN; i++) {
		struct express_socket_flow *sxf;

		sxf = &napi->sx->ht[(hash + i) % EXPRESS_SOCKET_HT_LEN];
		//pr_emerg("sx_lookup slot=%d sxf=%px\n", (hash + i) % EXPRESS_SOCKET_HT_LEN, sxf);
		if (!sxf->sk)
			break;

		if (inet6_match(dev_net(napi->dev), sxf->sk, daddr, saddr, ports, dif, sdif))
			return sxf;
	}

	return NULL;
}

static struct express_socket_flow *sx_lookup_skb(struct napi_struct *napi,
						 struct sk_buff *skb)
{
	const struct in6_addr *saddr = NULL;
	const struct in6_addr *daddr = NULL;
	struct ipv6hdr *hdr;
	struct tcphdr *th;
	int dif, sdif;
	__be16 dport;
	__u16 sport;

	skb_reset_network_header(skb);
	hdr = ipv6_hdr(skb);

	if (hdr->version != 6)
		return NULL;
	/* TODO: support BIG TCP on RX? */
	if (hdr->nexthdr != IPPROTO_TCP)
		return NULL;

	skb_set_transport_header(skb, sizeof(*hdr));
	__skb_pull(skb, skb_network_header_len(skb));
	th = tcp_hdr(skb);

	if (!pskb_may_pull(skb, skb_transport_offset(skb) + sizeof(struct tcphdr)))
		return NULL;

	if (th->doff < sizeof(struct tcphdr) / 4)
		return NULL;

	sport = ntohs(th->dest);
	dport = th->source;
	daddr = &hdr->saddr;
	saddr = &hdr->daddr;
	dif = skb->dev->ifindex; /* inet6_iif(skb); */
	sdif = inet6_sdif(skb);

	return sx_lookup(napi, sport, dport, saddr, daddr, dif, sdif);
}

static struct express_socket_flow *sx_lookup_sk6(struct napi_struct *napi,
						 struct sock *sk)
{
	const struct in6_addr *saddr = &sk->sk_v6_rcv_saddr;
	const struct in6_addr *daddr = &sk->sk_v6_daddr;
	__be16 dport = sk->sk_dport;
	__u16 sport = sk->sk_num;

	return sx_lookup(napi, sport, dport, saddr, daddr, 0, 0);
}

static int sx_insert_sk6(struct napi_struct *napi, struct sock *sk)
{
	const struct in6_addr *saddr = &sk->sk_v6_rcv_saddr;
	const struct in6_addr *daddr = &sk->sk_v6_daddr;
	__be16 dport = sk->sk_dport;
	__u16 sport = sk->sk_num;
	unsigned int hash;

	hash = inet6_ehashfn(dev_net(napi->dev), saddr, sport, daddr, dport);
	//pr_emerg("sx_insert_sk6 hash=%x sport=%x dport=%x\n", hash, sport, dport);

	for (unsigned int i = 0; i < EXPRESS_SOCKET_HT_LEN; i++) {
		struct express_socket_flow *sxf;

		sxf = &napi->sx->ht[(hash + i) % EXPRESS_SOCKET_HT_LEN];
		if (sxf->sk)
			continue;

		sock_hold(sk);
		sxf->sk = sk;
		//pr_emerg("sx_insert_sk6 slot=%d\n", (hash + i) % EXPRESS_SOCKET_HT_LEN);
		return 0;
	}

	return -E2BIG;
}

int sx_add(struct napi_struct *napi, struct socket *sock)
{
	struct express_socket_flow *sxf;
	int err = 0;

	if (sock->sk->sk_family != AF_INET6)
		return -EINVAL;

	netdev_assert_locked(napi->dev);

	if (!napi->sx) {
		napi->sx = kzalloc(sizeof(struct express_socket), GFP_KERNEL);
		if (!napi->sx)
			return -ENOMEM;

		INIT_LIST_HEAD(&napi->sx->pending);
	}

	sxf = sx_lookup_sk6(napi, sock->sk);
	if (sxf)
		return -EEXIST;

	err = sx_insert_sk6(napi, sock->sk);
	//pr_emerg("sx_add err=%d sk_portpair=%x\n", err, sock->sk->sk_portpair);

	return err;
}

void sx_cleanup(struct napi_struct *napi)
{
	if (!napi->sx)
		return;

	for (unsigned int i = 0; i < EXPRESS_SOCKET_HT_LEN; i++) {
		struct express_socket_flow *sxf;

		sxf = &napi->sx->ht[i];
		if (!sxf->sk)
			continue;

		sock_put(sxf->sk);
	}

	kfree(napi->sx);
}

static void sx_rcv(struct sock *sk, struct sk_buff *skb)
{
	struct ipv6hdr *hdr;
	struct tcphdr *th;

	hdr = ipv6_hdr(skb);
	th = tcp_hdr(skb);
#if 0
	u32 pkt_len = ntohs(hdr->payload_len);
	if (pskb_trim_rcsum(skb, pkt_len + sizeof(struct ipv6hdr)))
		return false;
	hdr = ipv6_hdr(skb);
#endif
	memset(IP6CB(skb), 0, sizeof(struct inet6_skb_parm));
	IP6CB(skb)->iif = skb->dev->ifindex;
	IP6CB(skb)->nhoff = offsetof(struct ipv6hdr, nexthdr);
	/* We do memmove for ipv6 parts, why not reorder? */
	tcp_v6_fill_cb(skb, hdr, th);
	sk_backlog_rcv(sk, skb);
}

#if FOR_REAL
static void sx_link_flow(struct express_socket *sx, struct express_socket_flow *sxf, struct sk_buff *skb)
{
	SX_CB(skb)->sxf = sxf;
	sxf->skb = skb;
	list_add(&skb->list, &sx->pending);
}

static void sx_unlink_flow(struct express_socket_flow *sxf, struct sk_buff *skb)
{
	list_del(&skb->list);
}

static bool sx_can_merge(struct sk_buff *lhs, struct sk_buff *rhs)
{
	int lhs_frags = skb_shinfo(lhs)->nr_frags;
	int rhs_frags = skb_shinfo(rhs)->nr_frags;
	struct tcphdr *lth, *rth;
	u32 end_seq;

	if (lhs->len + rhs->len > netif_get_gro_max_size(lhs->dev, lhs))
		return false;


	lth = tcp_hdr(lhs);
	end_seq = lth->seq + lth->syn + lth->fin + lhs->len - lth->doff*4;

	// TODO
	rth = tcp_hdr(rhs);
	pr_emerg("BEFORE %x %x %x\n", lth->seq, end_seq, rth->seq);
	if (rth->seq != end_seq)
		return false;
	pr_emerg("AFTER\n");

	if (lhs_frags == 0 || rhs_frags == 0)
		return false;

	if (lhs_frags + rhs_frags >= MAX_SKB_FRAGS)
		return false;

	return true;
}

static bool sx_merge_flow(struct express_socket_flow *sxf, struct sk_buff *skb)
{
	struct skb_shared_info *to, *from;

	if (!sx_can_merge(sxf->skb, skb))
		return false;

	to = skb_shinfo(sxf->skb);
	from = skb_shinfo(skb);

	/* using frags and have enough space to move the frags over */

	for (int i = 0; i < from->nr_frags; i++) {
		to->frags[to->nr_frags].netmem = from->frags[i].netmem;

		skb->len += from->frags[i].len;
		skb->data_len += from->frags[i].len;
		skb->truesize += PAGE_SIZE;

		from->frags[i].netmem = 0;
		to->nr_frags++;
	}
	from->nr_frags = 0;

	if (skb->fclone != SKB_FCLONE_UNAVAILABLE)
		__kfree_skb(skb);
	else
		__napi_kfree_skb(skb, SKB_CONSUMED);

	/* TODO: properly merge csum lhs_csum + rhs_csum - pseudo header */
	sxf->skb->ip_summed = CHECKSUM_UNNECESSARY;
	return true;
}

static void sx_flush_flow(struct express_socket_flow *sxf, struct sk_buff *skb, bool try_merge)
{
	bh_lock_sock(sxf->sk);

	if (sxf->skb) {
		/* Already have something in the queue, try to merge and
		 * flush. If can't merge, flush two skbs independently.
		 */

		if (try_merge && sx_merge_flow(sxf, skb)) {
			//pr_emerg("%px: flush %px (consumed %px)\n", sxf, sxf->skb, skb);
			sx_unlink_flow(sxf, sxf->skb);
			sx_rcv(sxf->sk, sxf->skb);
			sxf->skb = NULL;
			goto out;
		}

		//pr_emerg("%px: flush old %px\n", sxf, sxf->skb);
		sx_unlink_flow(sxf, sxf->skb);
		sx_rcv(sxf->sk, sxf->skb);
		sxf->skb = NULL;
	}

	if (skb) {
		//pr_emerg("%px: flush new %px\n", sxf, skb);
		sx_rcv(sxf->sk, skb);
	}

out:
	bh_unlock_sock(sxf->sk);
}
#endif

void sx_flush(struct napi_struct *napi, bool flush_old)
{
#if FOR_REAL
	struct list_head *p, *n;
#endif

	if (!napi->sx)
		return;

	//pr_emerg("sx_flush napi=%px\n", napi);

#if FOR_REAL
	list_for_each_safe(p, n, &napi->sx->pending) {
		struct sk_buff *skb;

		skb = list_entry(p, struct sk_buff, list);
		sx_flush_flow(SX_CB(skb)->sxf, NULL, false);
	}
#endif
}

bool sx_rx(struct napi_struct *napi, struct sk_buff *skb)
{
	struct express_socket_flow *sxf;
	struct tcphdr *th;
	__be32 flags;

	/* TODO: add counters on every condition! why and when flushed! */

	/* TODO: use static branch instead */
	if (!napi->sx)
		return false;

	if (netif_elide_gro(napi->dev))
		return false;

	if (eth_type_vlan(skb->protocol) || skb_vlan_tag_present(skb))
		return false;

	if (skb->pkt_type != PACKET_HOST)
		return false;

	sxf = sx_lookup_skb(napi, skb);
	if (!sxf)
		return false;

	if (sxf->sk->sk_state != TCP_ESTABLISHED)
		return false;

	pr_emerg("sx_rx napi=%px skb=%px len=%d frags=%d head_frag=%d\n", napi, skb, skb->len, skb_shinfo(skb)->nr_frags, skb->head_frag);

	th = tcp_hdr(skb);
	flags = tcp_flag_word(th);

#if DIRECT_INJECT
	bh_lock_sock(sxf->sk);
	sx_rcv(sxf->sk, skb);
	bh_unlock_sock(sxf->sk);
	return true;
#endif

#if FOR_REAL
	if (flags & (TCP_FLAG_CWR | TCP_FLAG_FIN | TCP_FLAG_PSH | TCP_FLAG_URG | TCP_FLAG_RST | TCP_FLAG_SYN)) {
		/*
		pr_emerg("%px: flush (flags %d %d %d %d %d %d)\n", sxf,
			 !!(flags & TCP_FLAG_CWR), !!(flags & TCP_FLAG_FIN),
			 !!(flags & TCP_FLAG_PSH), !!(flags & TCP_FLAG_URG),
			 !!(flags & TCP_FLAG_RST), !!(flags & TCP_FLAG_SYN));
		*/
		sx_flush_flow(sxf, skb, true);
		return true;
	}

	if (!sxf->skb) {
		//pr_emerg("%px: hold %px\n", sxf, skb);
		sx_link_flow(napi->sx, sxf, skb);
		return true;
	}

	if (sx_merge_flow(sxf, skb)) {
		//pr_emerg("%px: merge ok\n", sxf);
		return true;
	}

	/* can't merge, might be a gap - flush */
	//pr_emerg("%px: merge failed\n", sxf);
	sx_flush_flow(sxf, skb, true);
	return true;
#else
	return false;
#endif
}
