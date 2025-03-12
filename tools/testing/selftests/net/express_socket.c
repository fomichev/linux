// SPDX-License-Identifier: GPL-2.0

#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <net/if.h>

#include "netdev-user.h"
#include <ynl.h>

#include "network_helpers.h"

static int create_netdevsim(int id, const char *addr)
{
	char buf[256];
	FILE *f;

	f = fopen("/sys/bus/netdevsim/new_device", "w");
	if (!f)
		error(1, -errno, "Failed to open /sys/bus/netdevsim/new_device");
	fprintf(f, "%d 1\n", id);
	fclose(f);

	if (system("ip link set dev eth0 name netdevsim0") < 0)
		error(1, -errno, "Failed to rename netdevsim");

	sprintf(buf, "ip -6 addr add %s dev netdevsim0 nodad", addr);
	if (system(buf) < 0)
		error(1, -errno, "Failed to set netdevsim address");

	if (system("ip link set dev netdevsim0 up") < 0)
		error(1, -errno, "Failed to up netdevsim");

	return if_nametoindex("netdevsim0");
}

static void connect_netdevsims(int ns0_fd, int ns0_ifindex, int ns1_fd, int ns1_ifindex)
{
	FILE *f;

	f = fopen("/sys/bus/netdevsim/link_device", "w");
	if (!f)
		error(1, -errno, "Failed to open /sys/bus/netdevsim/link_device");
	fprintf(f, "%d:%d %d:%d\n", ns0_fd, ns0_ifindex, ns1_fd, ns1_ifindex);
	fclose(f);
}

static int lookup_napi_id(int ifindex, int id)
{
	struct netdev_queue_get_req *req;
	struct netdev_queue_get_rsp *rsp;
	struct ynl_error yerr;
	struct ynl_sock *ys;
	int napi_id = -1;

	ys = ynl_sock_create(&ynl_netdev_family, &yerr);
	if (!ys) {
		fprintf(stderr, "YNL: %s\n", yerr.msg);
		return -1;
	}

	req = netdev_queue_get_req_alloc();
	if (!req)
		goto out;
	netdev_queue_get_req_set_ifindex(req, ifindex);
	netdev_queue_get_req_set_type(req, NETDEV_QUEUE_TYPE_RX);
	netdev_queue_get_req_set_id(req, id);

	rsp = netdev_queue_get(ys, req);
	if (rsp)
		napi_id = rsp->napi_id;
	netdev_queue_get_req_free(req);
	netdev_queue_get_rsp_free(rsp);

out:
	ynl_sock_destroy(ys);
	return napi_id;
}

static int add_express_socket(int napi_id, int fd)
{
	struct netdev_express_socket_add_req *req;
	struct ynl_error yerr;
	struct ynl_sock *ys;
	int ret = -1;

	ys = ynl_sock_create(&ynl_netdev_family, &yerr);
	if (!ys) {
		fprintf(stderr, "YNL: %s\n", yerr.msg);
		return -1;
	}

	req = netdev_express_socket_add_req_alloc();
	if (!req)
		goto out;

	netdev_express_socket_add_req_set_napi_id(req, napi_id);
	netdev_express_socket_add_req_set_socket_fd(req, fd);

	ret = netdev_express_socket_add(ys, req);

out:
	ynl_sock_destroy(ys);
	return ret;
}

#define NS0 "express_socket_ns0"
#define NS1 "express_socket_ns1"

int main(int argc, char **argv)
{
	struct nstoken *ns0_token, *ns1_token;
	const char *ifname = "netdevsim0";
	int ns0_ifindex, ns1_ifindex;
	char buf[10000] = {};
	int ns0_fd, ns1_fd;
	int rx_fd, tx_fd;
	int server_fd;
	int napi_id;
	int ret;

	if (make_netns(NS0) < 0)
		error(1, -errno, "Failed to create %s netns", NS0);
	if (make_netns(NS1) < 0)
		error(1, -errno, "Failed to create %s netns", NS1);

	/* listener namespace */

	ns0_token = open_netns(NS0);
	if (!ns0_token)
		error(1, -errno, "Failed to open %s netns", NS0);

	ns0_ifindex = create_netdevsim(0, "fd00::/8");
	ns0_fd = open("/proc/self/ns/net", O_RDONLY);

	server_fd = start_server(AF_INET6, SOCK_STREAM, "fd00::", 0, 1000);
	if (server_fd <0)
		error(1, -errno, "Failed to start server");

	ret = setsockopt(server_fd, SOL_SOCKET, SO_BINDTODEVICE, ifname,
			 strlen(ifname) + 1);
	if (ret < 0)
		error(1, -errno, "Failed to SO_BINDTODEVICE");

	close_netns(ns0_token);
	ns0_token = NULL;

	/* client namespace */

	ns1_token = open_netns(NS1);
	if (!ns1_token)
		error(1, -errno, "Failed to open %s netns", NS1);

	ns1_ifindex = create_netdevsim(1, "fd00::1/8");
	ns1_fd = open("/proc/self/ns/net", O_RDONLY);

	connect_netdevsims(ns0_fd, ns0_ifindex, ns1_fd, ns1_ifindex);

	tx_fd = connect_to_fd(server_fd, 0);
	if (tx_fd < 0)
		error(1, -errno, "Failed to connect server");

	close_netns(ns1_token);
	ns1_token = NULL;

	/* listener namespace */

	ns0_token = open_netns(NS0);
	if (!ns0_token)
		error(1, -errno, "Failed to open %s netns", NS0);

	napi_id = lookup_napi_id(ns0_ifindex, 0);
	if (napi_id < 0)
		error(1, -errno, "Failed to lookup NAPI id");

	rx_fd = accept(server_fd, NULL, NULL);
	if (rx_fd < 0)
		error(1, -errno, "Failed to accept client");

	if (add_express_socket(napi_id, rx_fd) < 0)
		error(1, -errno, "Failed to add express socket");

	ret = send(tx_fd, buf, sizeof(buf), 0);
	if (ret != sizeof(buf)) {
		error(1, -errno, "Failed to send");
	}

	ret = recv(rx_fd, buf, sizeof(buf), 0);
	if (ret != sizeof(buf)) {
		error(1, -errno, "Failed to recv");
	}

	/* TODO: make sure the payload is correct */

	close_netns(ns0_token);
	ns0_token = NULL;

	return 0;
}
