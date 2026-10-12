/* linux/poll.h -- Kestrel's Linux KPI: poll() for character devices
 * Kestrel's poll re-asks the device at every wakeup (and every 100 ms),
 * so poll_wait() need not register anything. */
#ifndef _LKPI_LINUX_POLL_H
#define _LKPI_LINUX_POLL_H
#include <linux/types.h>
#include <linux/wait.h>
#define EPOLLIN         0x00000001
#define EPOLLPRI        0x00000002
#define EPOLLOUT        0x00000004
#define EPOLLERR        0x00000008
#define EPOLLHUP        0x00000010
#define EPOLLNVAL       0x00000020
#define EPOLLRDNORM     0x00000040
#define EPOLLRDBAND     0x00000080
#define EPOLLWRNORM     0x00000100
#define EPOLLWRBAND     0x00000200
#define EPOLLMSG        0x00000400
#define EPOLLRDHUP      0x00002000
#define POLLIN          EPOLLIN
#define POLLPRI         EPOLLPRI
#define POLLOUT         EPOLLOUT
#define POLLERR         EPOLLERR
#define POLLHUP         EPOLLHUP
#define POLLRDNORM      EPOLLRDNORM
#define POLLWRNORM      EPOLLWRNORM
struct file;
typedef struct poll_table_struct { __poll_t _key; } poll_table;
static inline void poll_wait(struct file *filp, wait_queue_head_t *wq, poll_table *p) { }
static inline __poll_t poll_requested_events(const poll_table *p) { return p ? p->_key : ~(__poll_t)0; }
#endif
