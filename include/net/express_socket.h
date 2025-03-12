/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _NET_EXPRESS_SOCKET_H
#define _NET_EXPRESS_SOCKET_H

#include <linux/list.h>

struct napi_struct;
struct sk_buff;
struct socket;

struct express_socket_flow {
	struct sock *sk;
    struct sk_buff *skb;
};

#define EXPRESS_SOCKET_HT_LEN 1024

struct express_socket {
	/* TODO: make size configurable - rhashtable? */
	struct express_socket_flow ht[1024];
    struct list_head pending;
};

int sx_add(struct napi_struct *napi, struct socket *sock);
void sx_cleanup(struct napi_struct *napi);
void sx_flush(struct napi_struct *napi, bool flush_old);
bool sx_rx(struct napi_struct *napi, struct sk_buff *skb);

#endif
