/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2018 Intel Corporation
 * Copyright(c) 2017 Red Hat, Inc.
 * Copyright(c) 2019 Arrikto Inc.
 */

#include <stdint.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include "rte_vhost.h"
#ifdef RTE_LIBRTE_VHOST_POSTCOPY
#include <linux/userfaultfd.h>
#endif
#include <fcntl.h>

#include <rte_log.h>

#include "fd_man.h"
#include "rte_common.h"
#include "vhost.h"
#include "vhost_user.h"

#define MAX_VIRTIO_BACKLOG 128

static struct fdset af_unix_fdset = {
	.fd = { [0 ... MAX_FDS - 1] = {-1, NULL, NULL, NULL, 0} },
	.fd_mutex = PTHREAD_MUTEX_INITIALIZER,
	.fd_pooling_mutex = PTHREAD_MUTEX_INITIALIZER,
	.num = 0
};

TAILQ_HEAD(vhost_user_connection_list, vhost_user_connection);

struct vhost_user_connection {
	struct virtio_net device; /* must be the first field! */
	struct vhost_user_socket *vsocket;
	int connfd;
	int slave_req_fd;
	rte_spinlock_t slave_req_lock;

	TAILQ_ENTRY(vhost_user_connection) next;
};

struct af_unix_socket {
	struct vhost_user_socket socket; /* must be the first field! */
	struct vhost_user_connection_list conn_list;
	pthread_mutex_t conn_mutex;
	int socket_fd;
	struct sockaddr_un un;
};

static int read_vhost_message(int sockfd, struct VhostUserMsg *msg);
static int create_unix_socket(struct vhost_user_socket *vsocket);
static int vhost_user_start_server(struct vhost_user_socket *vsocket);
static int vhost_user_start_client(struct vhost_user_socket *vsocket);
static void vhost_user_read_cb(int connfd, void *dat, int *remove);

/*
 * return bytes# of read on success or negative val on failure. Update fdnum
 * with number of fds read.
 */
static int
read_fd_message(int sockfd, char *buf, int buflen, int *fds, int max_fds,
		int *fd_num)
{
	struct iovec iov;
	struct msghdr msgh;
	char control[CMSG_SPACE(max_fds * sizeof(int))];
	struct cmsghdr *cmsg;
	int got_fds = 0;
	int ret;

	*fd_num = 0;

	memset(&msgh, 0, sizeof(msgh));
	iov.iov_base = buf;
	iov.iov_len  = buflen;

	msgh.msg_iov = &iov;
	msgh.msg_iovlen = 1;
	msgh.msg_control = control;
	msgh.msg_controllen = sizeof(control);

	ret = recvmsg(sockfd, &msgh, 0);
	if (ret <= 0) {
		if (ret)
			VHOST_LOG_CONFIG(ERR, "recvmsg failed\n");
		return ret;
	}

	if (msgh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
		VHOST_LOG_CONFIG(ERR, "truncated msg\n");
		return -1;
	}

	for (cmsg = CMSG_FIRSTHDR(&msgh); cmsg != NULL;
		cmsg = CMSG_NXTHDR(&msgh, cmsg)) {
		if ((cmsg->cmsg_level == SOL_SOCKET) &&
			(cmsg->cmsg_type == SCM_RIGHTS)) {
			got_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			*fd_num = got_fds;
			memcpy(fds, CMSG_DATA(cmsg), got_fds * sizeof(int));
			break;
		}
	}

	/* Clear out unused file descriptors */
	while (got_fds < max_fds)
		fds[got_fds++] = -1;

	return ret;
}

static int
send_fd_message(int sockfd, void *buf, int buflen, int *fds, int fd_num)
{

	struct iovec iov;
	struct msghdr msgh;
	size_t fdsize = fd_num * sizeof(int);
	char control[CMSG_SPACE(fdsize)];
	struct cmsghdr *cmsg;
	int ret;

	memset(&msgh, 0, sizeof(msgh));
	iov.iov_base = buf;
	iov.iov_len = buflen;

	msgh.msg_iov = &iov;
	msgh.msg_iovlen = 1;

	if (fds && fd_num > 0) {
		msgh.msg_control = control;
		msgh.msg_controllen = sizeof(control);
		cmsg = CMSG_FIRSTHDR(&msgh);
		if (cmsg == NULL) {
			VHOST_LOG_CONFIG(ERR, "cmsg == NULL\n");
			errno = EINVAL;
			return -1;
		}
		cmsg->cmsg_len = CMSG_LEN(fdsize);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		memcpy(CMSG_DATA(cmsg), fds, fdsize);
	} else {
		msgh.msg_control = NULL;
		msgh.msg_controllen = 0;
	}

	do {
		ret = sendmsg(sockfd, &msgh, MSG_NOSIGNAL);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		VHOST_LOG_CONFIG(ERR,  "sendmsg error\n");
		return ret;
	}

	return ret;
}

static int
af_unix_send_reply(struct virtio_net *dev, struct VhostUserMsg *msg)
{
	struct vhost_user_connection *conn =
		container_of(dev, struct vhost_user_connection, device);

	return send_fd_message(conn->connfd, msg,
			       VHOST_USER_HDR_SIZE + msg->size, msg->fds, msg->fd_num);
}

static int
af_unix_send_slave_req(struct virtio_net *dev, struct VhostUserMsg *msg)
{
	struct vhost_user_connection *conn =
		container_of(dev, struct vhost_user_connection, device);
	int ret;

	if (msg->flags & VHOST_USER_NEED_REPLY)
		rte_spinlock_lock(&conn->slave_req_lock);

	ret = send_fd_message(conn->slave_req_fd, msg,
			VHOST_USER_HDR_SIZE + msg->size, msg->fds, msg->fd_num);

	if (ret < 0 && (msg->flags & VHOST_USER_NEED_REPLY))
		rte_spinlock_unlock(&conn->slave_req_lock);

	return ret;
}

static int
af_unix_process_slave_message_reply(struct virtio_net *dev,
				    const struct VhostUserMsg *msg)
{
	struct vhost_user_connection *conn =
		container_of(dev, struct vhost_user_connection, device);
	struct VhostUserMsg msg_reply;
	int ret;

	if ((msg->flags & VHOST_USER_NEED_REPLY) == 0)
		return 0;

	if (read_vhost_message(conn->slave_req_fd, &msg_reply) < 0) {
		ret = -1;
		goto out;
	}

	if (msg_reply.request.slave != msg->request.slave) {
		VHOST_LOG_CONFIG(ERR,
				 "Received unexpected msg type (%u), expected %u\n",
				 msg_reply.request.slave, msg->request.slave);
		ret = -1;
		goto out;
	}

	ret = msg_reply.payload.u64 ? -1 : 0;

out:
	rte_spinlock_unlock(&conn->slave_req_lock);
	return ret;
}

static int
af_unix_set_slave_req_fd(struct virtio_net *dev, struct VhostUserMsg *msg)
{
	struct vhost_user_connection *conn =
		container_of(dev, struct vhost_user_connection, device);
	int fd = msg->fds[0];

	if (fd < 0) {
		VHOST_LOG_CONFIG(ERR,
				 "Invalid file descriptor for slave channel (%d)\n",
				fd);
		return -1;
	}

	conn->slave_req_fd = fd;

	return 0;
}

static void
vhost_user_add_connection(int fd, struct vhost_user_socket *vsocket)
{
	struct af_unix_socket *af_vsocket =
		container_of(vsocket, struct af_unix_socket, socket);
	struct virtio_net *dev;
	size_t size;
	struct vhost_user_connection *conn;
	int ret;

	if (vsocket == NULL)
		return;

	dev = vhost_new_device(vsocket->trans_ops);
	if (!dev) {
		return;
	}

	conn = container_of(dev, struct vhost_user_connection, device);
	conn->connfd = fd;
	conn->slave_req_fd = -1;
	conn->vsocket = vsocket;
	rte_spinlock_init(&conn->slave_req_lock);

	size = strnlen(vsocket->path, PATH_MAX);
	vhost_set_ifname(dev->vid, vsocket->path, size);

	vhost_setup_virtio_net(dev->vid, vsocket->use_builtin_virtio_net,
		vsocket->net_compliant_ol_flags);

	vhost_attach_vdpa_device(dev->vid, vsocket->vdpa_dev);

	if (vsocket->extbuf)
		vhost_enable_extbuf(dev->vid);

	if (vsocket->linearbuf)
		vhost_enable_linearbuf(dev->vid);

	if (vsocket->async_copy) {
		dev = get_device(dev->vid);

		if (dev)
			dev->async_copy = 1;
	}

	VHOST_LOG_CONFIG(INFO, "new device, handle is %d\n", dev->vid);

	if (vsocket->notify_ops->new_connection) {
		ret = vsocket->notify_ops->new_connection(dev->vid);
		if (ret < 0) {
			VHOST_LOG_CONFIG(ERR,
				"failed to add vhost user connection with fd %d\n",
				fd);
			goto err;
		}
	}

	ret = fdset_add(&af_unix_fdset, fd, vhost_user_read_cb,
			NULL, conn);
	if (ret < 0) {
		VHOST_LOG_CONFIG(ERR,
			"failed to add fd %d into vhost server fdset\n",
			fd);

		if (vsocket->notify_ops->destroy_connection)
			vsocket->notify_ops->destroy_connection(dev->vid);

		goto err;
	}

	pthread_mutex_lock(&af_vsocket->conn_mutex);
	TAILQ_INSERT_TAIL(&af_vsocket->conn_list, conn, next);
	pthread_mutex_unlock(&af_vsocket->conn_mutex);

	fdset_pipe_notify(&af_unix_fdset);
	return;

err:
	close(conn->connfd);
	vhost_destroy_device(dev->vid);
}

/* call back when there is new vhost-user connection from client  */
static void
vhost_user_server_new_connection(int fd, void *dat, int *remove __rte_unused)
{
	struct vhost_user_socket *vsocket = dat;

	fd = accept(fd, NULL, NULL);
	if (fd < 0)
		return;

	VHOST_LOG_CONFIG(INFO, "new vhost user connection is %d\n", fd);
	vhost_user_add_connection(fd, vsocket);
}

/* return bytes# of read on success or negative val on failure. */
static int
read_vhost_message(int sockfd, struct VhostUserMsg *msg)
{
	int ret;

	ret = read_fd_message(sockfd, (char *)msg, VHOST_USER_HDR_SIZE,
		msg->fds, VHOST_MEMORY_MAX_NREGIONS, &msg->fd_num);
	if (ret <= 0)
		return ret;

	if (msg->size) {
		if (msg->size > sizeof(msg->payload)) {
			VHOST_LOG_CONFIG(ERR, "invalid msg size: %d\n",
					 msg->size);
			return -1;
		}
		ret = read(sockfd, &msg->payload, msg->size);
		if (ret <= 0)
			return ret;
		if (ret != (int)msg->size) {
			VHOST_LOG_CONFIG(ERR, "read control message failed\n");
                  return -1;
		}
	}

	return ret;
}

static void
vhost_user_read_cb(int connfd, void *dat, int *remove)
{
	struct vhost_user_connection *conn = dat;
	struct vhost_user_socket *vsocket = conn->vsocket;
	struct af_unix_socket *af_vsocket =
		container_of(vsocket, struct af_unix_socket, socket);
	struct VhostUserMsg msg;
	int ret;

	ret = read_vhost_message(connfd, &msg);
	if (ret <= 0) {
		if (ret < 0)
			VHOST_LOG_CONFIG(ERR,
					 "vhost read message failed\n");
		else if (ret == 0)
			VHOST_LOG_CONFIG(ERR,
				"vhost peer closed\n");
		goto err;
	}

	ret = vhost_user_msg_handler(conn->device.vid, connfd, &msg);
	if (ret < 0) {
err:
		close(connfd);
		*remove = 1;

		if (vsocket->notify_ops->destroy_connection)
			vsocket->notify_ops->destroy_connection(conn->device.vid);

		if (vsocket->reconnect) {
			create_unix_socket(vsocket);
			vhost_user_start_client(vsocket);
		}

		pthread_mutex_lock(&af_vsocket->conn_mutex);
		TAILQ_REMOVE(&af_vsocket->conn_list, conn, next);
		pthread_mutex_unlock(&af_vsocket->conn_mutex);

		vhost_destroy_device(conn->device.vid);
	}
}

static int
create_unix_socket(struct vhost_user_socket *vsocket)
{
	struct af_unix_socket *af_vsocket =
		container_of(vsocket, struct af_unix_socket, socket);
	int fd;
	struct sockaddr_un *un = &af_vsocket->un;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	VHOST_LOG_CONFIG(INFO, "vhost-user %s: socket created, fd: %d\n",
		vsocket->is_server ? "server" : "client", fd);

	if (!vsocket->is_server && fcntl(fd, F_SETFL, O_NONBLOCK)) {
		VHOST_LOG_CONFIG(ERR,
			"vhost-user: can't set nonblocking mode for socket, fd: "
			"%d (%s)\n", fd, strerror(errno));
		close(fd);
		return -1;
	}

	memset(un, 0, sizeof(*un));
	un->sun_family = AF_UNIX;
	strncpy(un->sun_path, vsocket->path, sizeof(un->sun_path));
	un->sun_path[sizeof(un->sun_path) - 1] = '\0';

	af_vsocket->socket_fd = fd;
	return 0;
}

static int
vhost_user_start_server(struct vhost_user_socket *vsocket)
{
	struct af_unix_socket *af_vsocket =
		container_of(vsocket, struct af_unix_socket, socket);
	int ret;
	int fd = af_vsocket->socket_fd;
	const char *path = vsocket->path;

	/*
	 * bind () may fail if the socket file with the same name already
	 * exists. But the library obviously should not delete the file
	 * provided by the user, since we can not be sure that it is not
	 * being used by other applications. Moreover, many applications form
	 * socket names based on user input, which is prone to errors.
	 *
	 * The user must ensure that the socket does not exist before
	 * registering the vhost driver in server mode.
	 */
	ret = bind(fd, (struct sockaddr *)&af_vsocket->un, sizeof(af_vsocket->un));
	if (ret < 0) {
		VHOST_LOG_CONFIG(ERR,
			"failed to bind to %s: %s; remove it and try again\n",
			path, strerror(errno));
		goto err;
	}
	VHOST_LOG_CONFIG(INFO, "bind to %s\n", path);

	ret = listen(fd, MAX_VIRTIO_BACKLOG);
	if (ret < 0)
		goto err;

	ret = fdset_add(&af_unix_fdset, fd, vhost_user_server_new_connection,
		  NULL, vsocket);
	if (ret < 0) {
		VHOST_LOG_CONFIG(ERR,
			"failed to add listen fd %d to vhost server fdset\n",
			fd);
		goto err;
	}

	return 0;

err:
	close(fd);
	return -1;
}

struct vhost_user_reconnect {
	struct sockaddr_un un;
	int fd;
	struct vhost_user_socket *vsocket;

	TAILQ_ENTRY(vhost_user_reconnect) next;
};

TAILQ_HEAD(vhost_user_reconnect_tailq_list, vhost_user_reconnect);
struct vhost_user_reconnect_list {
	struct vhost_user_reconnect_tailq_list head;
	pthread_mutex_t mutex;
};

static struct vhost_user_reconnect_list reconn_list;
static pthread_t reconn_tid;

static int
vhost_user_connect_nonblock(int fd, struct sockaddr *un, size_t sz)
{
	int ret, flags;

	ret = connect(fd, un, sz);
	if (ret < 0 && errno != EISCONN)
		return -1;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		VHOST_LOG_CONFIG(ERR,
			"can't get flags for connfd %d\n", fd);
		return -2;
	}
	if ((flags & O_NONBLOCK) && fcntl(fd, F_SETFL, flags & ~O_NONBLOCK)) {
		VHOST_LOG_CONFIG(ERR,
				"can't disable nonblocking on fd %d\n", fd);
		return -2;
	}
	return 0;
}

static void *
vhost_user_client_reconnect(void *arg __rte_unused)
{
	int ret;
	struct vhost_user_reconnect *reconn, *next;

	while (1) {
		pthread_mutex_lock(&reconn_list.mutex);

		/*
		 * An equal implementation of TAILQ_FOREACH_SAFE,
		 * which does not exist on all platforms.
		 */
		for (reconn = TAILQ_FIRST(&reconn_list.head);
		     reconn != NULL; reconn = next) {
			next = TAILQ_NEXT(reconn, next);

			ret = vhost_user_connect_nonblock(reconn->fd,
						(struct sockaddr *)&reconn->un,
						sizeof(reconn->un));
			if (ret == -2) {
				close(reconn->fd);
				VHOST_LOG_CONFIG(ERR,
					"reconnection for fd %d failed\n",
					reconn->fd);
				goto remove_fd;
			}
			if (ret == -1)
				continue;

			VHOST_LOG_CONFIG(INFO,
				"%s: connected\n", reconn->vsocket->path);
			vhost_user_add_connection(reconn->fd, reconn->vsocket);
remove_fd:
			TAILQ_REMOVE(&reconn_list.head, reconn, next);
			free(reconn);
		}

		pthread_mutex_unlock(&reconn_list.mutex);
		sleep(1);
	}

	return NULL;
}

static int
vhost_user_reconnect_init(void)
{
	int ret;

	ret = pthread_mutex_init(&reconn_list.mutex, NULL);
	if (ret < 0) {
		VHOST_LOG_CONFIG(ERR, "failed to initialize mutex");
		return ret;
	}
	TAILQ_INIT(&reconn_list.head);

	ret = rte_ctrl_thread_create(&reconn_tid, "vhost_reconn", NULL,
			     vhost_user_client_reconnect, NULL);
	if (ret != 0) {
		VHOST_LOG_CONFIG(ERR, "failed to create reconnect thread");
		if (pthread_mutex_destroy(&reconn_list.mutex)) {
			VHOST_LOG_CONFIG(ERR,
				"failed to destroy reconnect mutex");
		}
	}

	return ret;
}

static int
vhost_user_start_client(struct vhost_user_socket *vsocket)
{
	struct af_unix_socket *af_vsocket =
		container_of(vsocket, struct af_unix_socket, socket);
	int ret;
	int fd = af_vsocket->socket_fd;
	const char *path = vsocket->path;
	struct vhost_user_reconnect *reconn;

	ret = vhost_user_connect_nonblock(fd, (struct sockaddr *)&af_vsocket->un,
					  sizeof(af_vsocket->un));
	if (ret == 0) {
		vhost_user_add_connection(fd, vsocket);
		return 0;
	}

	VHOST_LOG_CONFIG(WARNING,
		"failed to connect to %s: %s\n",
		path, strerror(errno));

	if (ret == -2 || !vsocket->reconnect) {
		close(fd);
		return -1;
	}

	VHOST_LOG_CONFIG(INFO, "%s: reconnecting...\n", path);
	reconn = malloc(sizeof(*reconn));
	if (reconn == NULL) {
		VHOST_LOG_CONFIG(ERR,
			"failed to allocate memory for reconnect\n");
		close(fd);
		return -1;
	}
	reconn->un = af_vsocket->un;
	reconn->fd = fd;
	reconn->vsocket = vsocket;
	pthread_mutex_lock(&reconn_list.mutex);
	TAILQ_INSERT_TAIL(&reconn_list.head, reconn, next);
	pthread_mutex_unlock(&reconn_list.mutex);

	return 0;
}

static bool
vhost_user_remove_reconnect(struct vhost_user_socket *vsocket)
{
	int found = false;
	struct vhost_user_reconnect *reconn, *next;

	pthread_mutex_lock(&reconn_list.mutex);

	for (reconn = TAILQ_FIRST(&reconn_list.head);
	     reconn != NULL; reconn = next) {
		next = TAILQ_NEXT(reconn, next);

		if (reconn->vsocket == vsocket) {
			TAILQ_REMOVE(&reconn_list.head, reconn, next);
			close(reconn->fd);
			free(reconn);
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&reconn_list.mutex);
	return found;
}

static int
af_unix_socket_init(struct vhost_user_socket *vsocket,
		    uint64_t flags __rte_unused)
{
	struct af_unix_socket *af_vsocket =
		container_of(vsocket, struct af_unix_socket, socket);
	int ret;

	if (vsocket->reconnect && reconn_tid == 0) {
		if (vhost_user_reconnect_init() != 0)
			return -1;
	}

	TAILQ_INIT(&af_vsocket->conn_list);
	ret = pthread_mutex_init(&af_vsocket->conn_mutex, NULL);
	if (ret) {
		VHOST_LOG_CONFIG(ERR, "failed to init connection mutex\n");
		return -1;
	}

	return create_unix_socket(vsocket);

}

static void
af_unix_socket_cleanup(struct vhost_user_socket *vsocket)
{
	struct af_unix_socket *af_vsocket =
		container_of(vsocket, struct af_unix_socket, socket);
	struct vhost_user_connection *conn, *next;

	if (vsocket->is_server) {
		fdset_del(&af_unix_fdset, af_vsocket->socket_fd);
		close(af_vsocket->socket_fd);
		unlink(vsocket->path);
	} else if (vsocket->reconnect) {
		vhost_user_remove_reconnect(vsocket);
	}

again:
	pthread_mutex_lock(&af_vsocket->conn_mutex);
	for (conn = TAILQ_FIRST(&af_vsocket->conn_list);
	     conn != NULL;
	     conn = next) {
		next = TAILQ_NEXT(conn, next);

		/*
		 * If r/wcb is executing, release the
		 * conn_mutex lock, and try again since
		 * the r/wcb may use the conn_mutex lock.
		 */
		if (fdset_try_del(&af_unix_fdset,
				  conn->connfd) == -1) {
			pthread_mutex_unlock(
					&af_vsocket->conn_mutex);
			goto again;
		}

		VHOST_LOG_CONFIG(INFO, "free connfd = %d for device '%s'\n",
				 conn->connfd, vsocket->path);
		close(conn->connfd);
		TAILQ_REMOVE(&af_vsocket->conn_list, conn, next);
		vhost_destroy_device(conn->device.vid);
	}
	pthread_mutex_unlock(&af_vsocket->conn_mutex);

	pthread_mutex_destroy(&af_vsocket->conn_mutex);
}

static int
af_unix_socket_start(struct vhost_user_socket *vsocket)
{
	static pthread_t fdset_tid;

	if (fdset_tid == 0) {
		/**
		 * create a pipe which will be waited by poll and notified to
		 * rebuild the wait list of poll.
		 */
		if (fdset_pipe_init(&af_unix_fdset) < 0) {
			VHOST_LOG_CONFIG(ERR, "failed to create pipe for vhost fdset\n");
			return -1;
		}

		int ret = rte_ctrl_thread_create(&fdset_tid,
			"vhost-events", NULL, fdset_event_dispatch,
			&af_unix_fdset);
		if (ret != 0) {
			VHOST_LOG_CONFIG(ERR, "failed to create fdset handling thread");
			fdset_pipe_uninit(&af_unix_fdset);
			return -1;
		}
	}

	if (vsocket->is_server)
		return vhost_user_start_server(vsocket);
	else
		return vhost_user_start_client(vsocket);
}

static void
af_unix_cleanup_device(struct virtio_net *dev, int destroy __rte_unused)
{
	struct vhost_user_connection *conn =
		container_of(dev, struct vhost_user_connection, device);

	if (dev->log_addr) {
		munmap((void *)(uintptr_t)dev->log_addr, dev->log_size);
		dev->log_addr = 0;
	}

	if (conn->slave_req_fd >= 0) {
		close(conn->slave_req_fd);
		conn->slave_req_fd = -1;
	}
}

static int
af_unix_vring_call(struct virtio_net *dev __rte_unused,
		   struct vhost_virtqueue *vq)
{
	if (vq->callfd >= 0) eventfd_write(vq->callfd, (eventfd_t)1);
	return 0;
}

static uint64_t
get_blk_size(int fd)
{
	struct stat stat;
	int ret;

	ret = fstat(fd, &stat);
	return ret == -1 ? (uint64_t)-1 : (uint64_t)stat.st_blksize;
}

#ifdef RTE_LIBRTE_VHOST_POSTCOPY
static int
af_unix_postcopy_region_register(struct virtio_net *dev,
		struct rte_vhost_mem_region *reg)
{
	struct uffdio_register reg_struct;

	/*
	 * Let's register all the mmap'ed area to ensure
	 * alignment on page boundary.
	 */
	reg_struct.range.start = (uint64_t)(uintptr_t)reg->mmap_addr;
	reg_struct.range.len = reg->mmap_size;
	reg_struct.mode = UFFDIO_REGISTER_MODE_MISSING;

	if (ioctl(dev->postcopy_ufd, UFFDIO_REGISTER,
				&reg_struct)) {
		VHOST_LOG_CONFIG(ERR, "Failed to register ufd for region "
				"%" PRIx64 " - %" PRIx64 " (ufd = %d) %s\n",
				(uint64_t)reg_struct.range.start,
				(uint64_t)reg_struct.range.start +
				(uint64_t)reg_struct.range.len - 1,
				dev->postcopy_ufd,
				strerror(errno));
		return -1;
	}

	VHOST_LOG_CONFIG(INFO, "\t userfaultfd registered for range : %" PRIx64 " - %" PRIx64 "\n",
			(uint64_t)reg_struct.range.start,
			(uint64_t)reg_struct.range.start +
			(uint64_t)reg_struct.range.len - 1);

	return 0;
}
#else
static int
af_unix_postcopy_region_register(struct virtio_net *dev __rte_unused,
		struct rte_vhost_mem_region *reg __rte_unused)
{
	return -1;
}
#endif

static int
af_unix_postcopy_register(struct virtio_net *dev, int main_fd,
		struct VhostUserMsg *msg)
{
	struct VhostUserMemory *memory;
	struct rte_vhost_mem_region *reg;
	VhostUserMsg ack_msg;
	uint32_t i;

	if (!dev->postcopy_listening)
		return 0;

	/*
	 * We haven't a better way right now than sharing
	 * DPDK's virtual address with Qemu, so that Qemu can
	 * retrieve the region offset when handling userfaults.
	 */
	memory = &msg->payload.memory;
	for (i = 0; i < memory->nregions; i++) {
		reg = &dev->mem->regions[i];
		memory->regions[i].userspace_addr = reg->host_user_addr;
	}

	/* Send the addresses back to qemu */
	msg->fd_num = 0;
	af_unix_send_reply(dev, msg);

	/* Wait for qemu to acknolwedge it's got the addresses
	 * we've got to wait before we're allowed to generate faults.
	 */
	if (read_vhost_message(main_fd, &ack_msg) <= 0) {
		VHOST_LOG_CONFIG(ERR,
				"Failed to read qemu ack on postcopy set-mem-table\n");
		return -1;
	}

	if (validate_msg_fds(&ack_msg, 0) != 0)
		return -1;

	if (ack_msg.request.master != VHOST_USER_SET_MEM_TABLE) {
		VHOST_LOG_CONFIG(ERR,
				"Bad qemu ack on postcopy set-mem-table (%d)\n",
				ack_msg.request.master);
		return -1;
	}

	/* Now userfault register and we can use the memory */
	for (i = 0; i < memory->nregions; i++) {
		reg = &dev->mem->regions[i];
		if (af_unix_postcopy_region_register(dev, reg) < 0)
			return -1;
	}

	return 0;
}

static int
af_unix_add_one_guest_page(struct virtio_net *dev, uint64_t guest_phys_addr,
		   uint64_t host_phys_addr, uint64_t size)
{
	struct guest_page *page, *last_page;
	struct guest_page *old_pages;

	if (dev->nr_guest_pages == dev->max_guest_pages) {
		dev->max_guest_pages *= 2;
		old_pages = dev->guest_pages;
		dev->guest_pages = rte_realloc(dev->guest_pages,
					dev->max_guest_pages * sizeof(*page),
					RTE_CACHE_LINE_SIZE);
		if (dev->guest_pages == NULL) {
			VHOST_LOG_CONFIG(ERR, "cannot realloc guest_pages\n");
			rte_free(old_pages);
			return -1;
		}
	}

	if (dev->nr_guest_pages > 0) {
		last_page = &dev->guest_pages[dev->nr_guest_pages - 1];
		/* merge if the two pages are continuous */
		if (host_phys_addr == last_page->host_phys_addr +
				      last_page->size) {
			last_page->size += size;
			return 0;
		}
	}

	page = &dev->guest_pages[dev->nr_guest_pages++];
	page->guest_phys_addr = guest_phys_addr;
	page->host_phys_addr  = host_phys_addr;
	page->size = size;

	return 0;
}

static int
af_unix_add_guest_pages(struct virtio_net *dev, struct rte_vhost_mem_region *reg,
		uint64_t page_size)
{
	uint64_t reg_size = reg->size;
	uint64_t host_user_addr  = reg->host_user_addr;
	uint64_t guest_phys_addr = reg->guest_phys_addr;
	uint64_t host_phys_addr;
	uint64_t size;

	host_phys_addr = rte_mem_virt2iova((void *)(uintptr_t)host_user_addr);
	size = page_size - (guest_phys_addr & (page_size - 1));
	size = RTE_MIN(size, reg_size);

	if (af_unix_add_one_guest_page(dev, guest_phys_addr, host_phys_addr, size) < 0)
		return -1;

	host_user_addr  += size;
	guest_phys_addr += size;
	reg_size -= size;

	while (reg_size > 0) {
		size = RTE_MIN(reg_size, page_size);
		host_phys_addr = rte_mem_virt2iova((void *)(uintptr_t)
						  host_user_addr);
		if (af_unix_add_one_guest_page(dev, guest_phys_addr, host_phys_addr,
				size) < 0)
			return -1;

		host_user_addr  += size;
		guest_phys_addr += size;
		reg_size -= size;
	}

	/* sort guest page array if over binary search threshold */
	if (dev->nr_guest_pages >= VHOST_BINARY_SEARCH_THRESH) {
		qsort((void *)dev->guest_pages, dev->nr_guest_pages,
			sizeof(struct guest_page), guest_page_addrcmp);
	}

	return 0;
}

static int
af_unix_mmap_region(struct virtio_net *dev,
		struct rte_vhost_mem_region *region,
		uint64_t mmap_offset)
{
	void *mmap_addr;
	uint64_t mmap_size;
	uint64_t alignment;
	int populate;

	/* Check for memory_size + mmap_offset overflow */
	if (mmap_offset >= -region->size) {
		VHOST_LOG_CONFIG(ERR,
				"mmap_offset (%#"PRIx64") and memory_size "
				"(%#"PRIx64") overflow\n",
				mmap_offset, region->size);
		return -1;
	}

	mmap_size = region->size + mmap_offset;

	/* mmap() without flag of MAP_ANONYMOUS, should be called with length
	 * argument aligned with hugepagesz at older longterm version Linux,
	 * like 2.6.32 and 3.2.72, or mmap() will fail with EINVAL.
	 *
	 * To avoid failure, make sure in caller to keep length aligned.
	 */
	alignment = get_blk_size(region->fd);
	if (alignment == (uint64_t)-1) {
		VHOST_LOG_CONFIG(ERR,
				"couldn't get hugepage size through fstat\n");
		return -1;
	}
	mmap_size = RTE_ALIGN_CEIL(mmap_size, alignment);
	if (mmap_size == 0) {
		/*
		 * It could happen if initial mmap_size + alignment overflows
		 * the sizeof uint64, which could happen if either mmap_size or
		 * alignment value is wrong.
		 *
		 * mmap() kernel implementation would return an error, but
		 * better catch it before and provide useful info in the logs.
		 */
		VHOST_LOG_CONFIG(ERR, "mmap size (0x%" PRIx64 ") "
				"or alignment (0x%" PRIx64 ") is invalid\n",
				region->size + mmap_offset, alignment);
		return -1;
	}

	populate = dev->async_copy ? MAP_POPULATE : 0;
	mmap_addr = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
			MAP_SHARED | populate, region->fd, 0);

	if (mmap_addr == MAP_FAILED) {
		VHOST_LOG_CONFIG(ERR, "mmap failed (%s).\n", strerror(errno));
		return -1;
	}

	region->mmap_addr = mmap_addr;
	region->mmap_size = mmap_size;
	region->host_user_addr = (uint64_t)(uintptr_t)mmap_addr + mmap_offset;

	if (dev->async_copy)
		if (af_unix_add_guest_pages(dev, region, alignment) < 0) {
			VHOST_LOG_CONFIG(ERR,
					"adding guest pages to region failed.\n");
			return -1;
		}

	VHOST_LOG_CONFIG(INFO,
			"guest memory region size: 0x%" PRIx64 "\n"
			"\t guest physical addr: 0x%" PRIx64 "\n"
			"\t guest virtual  addr: 0x%" PRIx64 "\n"
			"\t host  virtual  addr: 0x%" PRIx64 "\n"
			"\t mmap addr : 0x%" PRIx64 "\n"
			"\t mmap size : 0x%" PRIx64 "\n"
			"\t mmap align: 0x%" PRIx64 "\n"
			"\t mmap off  : 0x%" PRIx64 "\n",
			region->size,
			region->guest_phys_addr,
			region->guest_user_addr,
			region->host_user_addr,
			(uint64_t)(uintptr_t)mmap_addr,
			mmap_size,
			alignment,
			mmap_offset);

	return 0;
}

static int
af_unix_map_mem_regions(struct virtio_net *dev, struct VhostUserMsg *msg)
{
	uint32_t i;
	struct VhostUserMemory *memory = &msg->payload.memory;
	struct vhost_user_connection *conn =
		container_of(dev, struct vhost_user_connection, device);

	for (i = 0; i < memory->nregions; i++) {
		struct rte_vhost_mem_region *reg = &dev->mem->regions[i];
		uint64_t mmap_offset;

		reg->guest_phys_addr = memory->regions[i].guest_phys_addr;
		reg->guest_user_addr = memory->regions[i].userspace_addr;
		reg->size = memory->regions[i].memory_size;
		reg->fd = msg->fds[i];

		/*
		 * Assign invalid file descriptor value to avoid double
		 * closing on error path.
		 */
		msg->fds[i] = -1;

		mmap_offset = memory->regions[i].mmap_offset;

		if (af_unix_mmap_region(dev, reg, mmap_offset) < 0) {
			VHOST_LOG_CONFIG(ERR, "Failed to mmap region %u\n", i);
			return -1;
		}

		dev->mem->nregions++;
	}

	if (af_unix_postcopy_register(dev, conn->connfd, msg) < 0)
		return -1;

	return 0;
}

static void
af_unix_unmap_mem_regions(struct virtio_net *dev)
{
	uint32_t i;
	struct rte_vhost_mem_region *reg;

	for (i = 0; i < dev->mem->nregions; i++) {
		reg = &dev->mem->regions[i];
		if (reg->host_user_addr) {
			munmap(reg->mmap_addr, reg->mmap_size);
			close(reg->fd);
		}
	}
}

static int
af_unix_set_log_base(struct virtio_net *dev, const struct VhostUserMsg *msg)
{
	int fd = msg->fds[0];
	uint64_t size, off;
	void *addr;

	size = msg->payload.log.mmap_size;
	off  = msg->payload.log.mmap_offset;

	/*
	 * mmap from 0 to workaround a hugepage mmap bug: mmap will
	 * fail when offset is not page size aligned.
	 */
	addr = mmap(0, size + off, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (addr == MAP_FAILED) {
		VHOST_LOG_CONFIG(ERR, "mmap log base failed!\n");
		return -1;
	}

	/*
	 * Free previously mapped log memory on occasionally
	 * multiple VHOST_USER_SET_LOG_BASE.
	 */
	if (dev->log_addr) {
		munmap((void *)(uintptr_t)dev->log_addr, dev->log_size);
	}
	dev->log_addr = (uint64_t)(uintptr_t)addr;
	dev->log_base = dev->log_addr + off;
	dev->log_size = size;

	return 0;
}

const struct vhost_transport_ops af_unix_trans_ops = {
	.socket_size = sizeof(struct af_unix_socket),
	.device_size = sizeof(struct vhost_user_connection),
	.socket_init = af_unix_socket_init,
	.socket_cleanup = af_unix_socket_cleanup,
	.socket_start = af_unix_socket_start,
	.cleanup_device = af_unix_cleanup_device,
	.vring_call = af_unix_vring_call,
	.send_reply = af_unix_send_reply,
	.send_slave_req = af_unix_send_slave_req,
	.process_slave_message_reply = af_unix_process_slave_message_reply,
	.set_slave_req_fd = af_unix_set_slave_req_fd,
	.map_mem_regions = af_unix_map_mem_regions,
	.unmap_mem_regions = af_unix_unmap_mem_regions,
	.set_log_base = af_unix_set_log_base,
};
