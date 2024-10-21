// SPDX-License-Identifier: GPL-2.0

#ifndef ____cacheline_aligned_in_smp
#define ____cacheline_aligned_in_smp __attribute__((aligned(64)))
#endif

//#include <linux/uio.h>
#include <linux/types.h>

#include <errno.h>
#include <error.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>

struct dmabuf_cmsg {
	__u64 frag_offset;	/* offset into the dmabuf where the frag starts.
				 */
	__u32 frag_size;	/* size of the frag. */
	__u32 frag_token;	/* token representing this frag for
				 * DEVMEM_DONTNEED.
				 */
	__u32  dmabuf_id;	/* dmabuf id this frag belongs to. */
	__u32 flags;		/* Currently unused. Reserved for future
				 * uses.
				 */
};

struct dmabuf_token {
	__u32 token_start;
	__u32 token_count;
};

#define LINR_SZ (16*1024*1024-16)

#define CMSG_SZ (LINR_SZ/sizeof(struct dmabuf_cmsg))
#define TOKN_SZ (LINR_SZ/sizeof(struct dmabuf_token))

struct devmem_ring {
	__u32 producer ____cacheline_aligned_in_smp;
	__u32 producer_err ____cacheline_aligned_in_smp;
	__u32 consumer ____cacheline_aligned_in_smp;
	__u32 consumer_err ____cacheline_aligned_in_smp;

	union {
		struct dmabuf_cmsg cmsg[CMSG_SZ];
		struct dmabuf_token token[TOKN_SZ];
		char data[LINR_SZ];
	};
};

#define SOL_TCP         6
#define TCP_MRQ_ALLOC		44
#define TCP_MRQ_ACTIVATE	45

struct tcp_mrq_alloc {
	__u64 size;
};

struct tcp_mrq_activate {
	__u64 skip_xa:1;
	__u64 skip_copy:1;
	__u64 skip_wakeup:1;
	__u64 pad:61;
};

static void enable_mrq(int fd, struct devmem_ring **cmsg, struct devmem_ring **token, struct devmem_ring **linear, int skip_copy)
{
	struct tcp_mrq_alloc alloc = {
		/* TODO: support PAGE_SIZE */
		.size = (sizeof(struct devmem_ring) / 4096 + 1) * 4096,
	};

	struct tcp_mrq_activate act = {
		.skip_xa = 0,
		.skip_copy = skip_copy,
		.skip_wakeup = 1,
	};

	int ret;

	ret = setsockopt(fd, SOL_TCP, TCP_MRQ_ALLOC, &alloc, sizeof(alloc));
	if (ret)
		error(1, errno, "TCP_MRQ_ALLOC\n");

	ret = setsockopt(fd, SOL_TCP, TCP_MRQ_ACTIVATE, &act, sizeof(act));
	if (ret)
		error(1, errno, "TCP_MRQ_ACTIVATE\n");

	*cmsg = mmap(NULL, sizeof(struct devmem_ring), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 1 * 4096);
	if (*cmsg == MAP_FAILED)
		error(1, errno, "mmap(cmsg)\n");

	*token = mmap(NULL, sizeof(struct devmem_ring), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 2 * 4096);
	if (*token == MAP_FAILED)
		error(1, errno, "mmap(token)\n");

	*linear = mmap(NULL, sizeof(struct devmem_ring), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 3 * 4096);
	if (*linear == MAP_FAILED)
		error(1, errno, "mmap(linear)\n");

}

static int enable_reuseaddr(int fd)
{
	int opt = 1;
	int ret;

	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
	if (ret)
		return -errno;

	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	if (ret)
		return -errno;

	return 0;
}

static int parse_address(const char *str, int port, struct sockaddr_in6 *sin6)
{
	int ret;

	sin6->sin6_family = AF_INET6;
	sin6->sin6_port = htons(port);

	ret = inet_pton(sin6->sin6_family, str, &sin6->sin6_addr);
	if (ret < 0)
		return -1;

	return 0;
}


static int do_recv_linear(struct devmem_ring *linear, int skip_copy)
{
	int producer, consumer;
	int ret = 0;

	consumer = __atomic_load_n(&linear->consumer, __ATOMIC_RELAXED);
	producer = __atomic_load_n(&linear->producer, __ATOMIC_ACQUIRE);

	if (producer > consumer) {
		int nr = producer - consumer;
		int off = consumer % LINR_SZ;
		int cap = LINR_SZ - off;

		ret += nr;

		fprintf(stderr, "consuming %d linear\n", nr);

		if (!skip_copy) {
			write(1, linear->data + off, cap < nr ? cap : nr);
			nr -= cap;
			if (nr > 0)
				write(1, linear->data, nr);
		}
	}

	__atomic_store_n(&linear->consumer, producer, __ATOMIC_RELAXED);

	return ret;
}

static int do_recv(int fd, int skip_copy)
{
	struct dmabuf_token *dmabuf_token;
	struct dmabuf_cmsg *dmabuf_cmsg;
	struct devmem_ring *linear;
	struct devmem_ring *token;
	struct devmem_ring *cmsg;
	int fin = 0;
	int ret = 0;
	int err;
	int i;

	enable_mrq(fd, &cmsg, &token, &linear, skip_copy);

	int cmsg_producer, cmsg_consumer;
	int token_producer, token_consumer;
	int nr, token_nr = 0;

	token_producer = __atomic_load_n(&token->producer, __ATOMIC_RELAXED);
	cmsg_consumer = __atomic_load_n(&cmsg->consumer, __ATOMIC_RELAXED);

	while (1) {
		if (__atomic_load_n(&cmsg->producer_err, __ATOMIC_RELAXED)) {
			fprintf(stderr, "got %d producer errors\n", __atomic_load_n(&cmsg->producer_err, __ATOMIC_RELAXED));
			break;
		}

		cmsg_producer = __atomic_load_n(&cmsg->producer, __ATOMIC_ACQUIRE);
		if (cmsg_producer == cmsg_consumer) {
			continue;
		}

		token_nr = 0;
		nr = cmsg_producer - cmsg_consumer;
		fprintf(stderr, "got %d (%d %d) iovecs\n", nr, cmsg_producer, cmsg_consumer);

		for (i = 0; i < nr; i++) {
			dmabuf_cmsg = &cmsg->cmsg[(cmsg_consumer + i) % CMSG_SZ];

			dmabuf_token = &token->token[(token_producer + i) % TOKN_SZ];
			dmabuf_token->token_start = dmabuf_cmsg->frag_token;
			dmabuf_token->token_count = dmabuf_cmsg->flags;
			token_nr++;

			if (!dmabuf_cmsg->dmabuf_id) {
				fin = 1;
			}
		}

		if (token_nr) {
			token_producer += token_nr;

			token_consumer = __atomic_load_n(&token->consumer, __ATOMIC_RELAXED);
			if (token_producer - token_consumer >= TOKN_SZ) {
				fprintf(stderr, "completions ring overflow (%d %d)\n", token_producer, token_consumer);
				break;
			}

			/* TODO: batch token refill */
			__atomic_store_n(&token->producer, token_producer, __ATOMIC_RELEASE);
		}

		cmsg_consumer += nr;

		__atomic_store_n(&cmsg->consumer, cmsg_producer, __ATOMIC_RELEASE); /* TODO: backpressure? */

		ret += do_recv_linear(linear, skip_copy);

		if (fin)
			break;
	}

	sleep(1);

	ret += do_recv_linear(linear, skip_copy);

	err = __atomic_load_n(&cmsg->producer_err, __ATOMIC_RELAXED);
	if (err)
		error(1, 0, "%d producer errors for cmsg ring", err);

	err = __atomic_load_n(&linear->producer_err, __ATOMIC_RELAXED);
	if (err)
		error(1, 0, "%d producer errors for linear ring", err);

	err = __atomic_load_n(&token->consumer_err, __ATOMIC_RELAXED);
	if (err)
		error(1, 0, "%d consumer errors for token ring", err);

	return ret;
}

int main(int argc, char *argv[])
{
	struct sockaddr_in6 sin;
	char *addr = NULL;
	int expected = -1;
	int skip_copy = 0;
	int port = -1;
	int lfd, fd;
	int ret;
	int opt;

	while ((opt = getopt(argc, argv, "a:e:p:tO")) != -1) {
		switch (opt) {
		case 'a':
			addr = optarg;
			break;
		case 'e':
			expected = atoi(optarg);
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 't':
			skip_copy = 1;
			break;
		default:
			error(1, 0, "unknown option %c\n", opt);
		}
	}

	if (addr == NULL)
		error(1, 0, "-a is requred\n");

	if (port < 0)
		error(1, 0, "-p is requred\n");

	ret = parse_address(addr, port, &sin);
	if (ret < 0)
		error(1, 0, "parse server address");

	lfd = socket(AF_INET6, SOCK_STREAM, 0);
	if (socket < 0)
		error(1, errno, "socket\n");

	ret = enable_reuseaddr(lfd);
	if (ret)
		error(1, errno, "reuseaddr\n");

	fprintf(stderr, "binding to address %s:%d\n", addr,
		ntohs(sin.sin6_port));

	ret = bind(lfd, &sin, sizeof(sin));
	if (ret)
		error(1, errno, "bind\n");

	ret = listen(lfd, 1);
	if (ret)
		error(1, errno, "listen\n");

	fd = accept(lfd, NULL, NULL);

	fprintf(stderr, "accepted client\n");
	ret = do_recv(fd, skip_copy);
	fprintf(stderr, "finished client\n");
	
	if (expected > 0) {
		if (ret != expected)
			error(1, 0, "received unexpected number of bytes %d vs %d", ret, expected);
	}

	close(fd);
	close(lfd);
}
