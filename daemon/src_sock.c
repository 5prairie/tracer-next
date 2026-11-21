/* src_sock.c: the sockets of each traced process, as queues: what waits to be read
   against the buffer's size, backlog, drops. Asks sock_diag for memory info only, which
   takes no socket lock. The process's sockets are found by inode through /proc/<pid>/fd. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/utsname.h>

#include "td_source.h"

enum { SRC_SOCK_QUEUE, SRC_SOCK_FILL, SRC_SOCK_BACKLOG, SRC_SOCK_DROPS, SRC_SOCK_METRIC_CNT };

/* One row per metric, in the order of the enum above. */
static td_metric_t const src_sock_metric[ SRC_SOCK_METRIC_CNT ] = {
  { "queue",   "receive queue",        "bytes", TD_LANE,  TD_LEVEL, 0,  0  },
  { "fill",    "receive queue filled", "%",     TD_TABLE, TD_LEVEL, 25, 75 },
  { "backlog", "backlog",              "bytes", TD_TABLE, TD_LEVEL, 0,  0  },
  { "drops",   "drops",                "",      TD_TABLE, TD_TOTAL, 0,  1  },
};

#define SRC_SOCK_FD_DIR          "/proc/%u/fd"
#define SRC_SOCK_MODULES_FILE    "/proc/modules"
#define SRC_SOCK_BUILTIN_FILE    "/lib/modules/%s/modules.builtin"
#define SRC_SOCK_LINK_FORMAT     "socket:[%lu]"  /* what an fd of a socket links to */
#define SRC_SOCK_MAX             (64)            /* sockets followed per process */
#define SRC_SOCK_ANSWER_BYTES    (1UL << 16)
#define SRC_SOCK_TIMEOUT_US      (50000)         /* the kernel always answers; if not, move on */
#define SRC_SOCK_FD_MAX          (1024)          /* fds of a process looked at; more are not */
/* A process's fds are looked at once in this many reads of the source, each process at a
   read of its own: no fewer than there are thread slots, so that no two looks share a read. */
#define SRC_SOCK_LOOK_READS      (16)
#define SRC_SOCK_WALK_LOOKS      (300)           /* an unfound socket is walked for this often... */
#define SRC_SOCK_WALK_LOST_LOOKS (33)            /* ...once past its early walks: see the table */
#define SRC_SOCK_WALK_GAP_LOOKS  (6)             /* looks between two walks of one process */
#define SRC_SOCK_WALK_SPACE_NS   (2UL * TD_NS_PER_SECOND)  /* between two walk dumps, of anyone */
/* The states a walk asks for: every state of a TCP socket but the two the kernel keeps for
   its own use. One of them, "bound, inactive" (kernel 6.8), is a socket that has no
   connection to ask for alone: found, it would only be lost again. */
#define SRC_SOCK_WALK_STATES     ( ( 1U << ( TCP_CLOSING + 1 ) ) - 1U )

_Static_assert( SRC_SOCK_LOOK_READS >= TD_THREAD_MAX, "each process looks at a read of its own" );

typedef struct {
  uint64_t                inode;
  int                     family, protocol;
  struct inet_diag_sockid id;        /* as the kernel reported it: sport and src are local */
  char                    key[ TD_KEY_BYTES ];
  int                     is_found;  /* a dump has seen it: the fields below are set */
  int                     is_exact;  /* asking for it alone works; else it waits for dumps */
  int                     is_seen;   /* a dump of this look answered for it */
  int                     kind_idx;  /* the cheapest dump that sees it */
  int                     miss_cnt;  /* looks that did not find it: unix, netlink, unbound */
  double                  queue_bytes, buffer_bytes, backlog_bytes, drop_cnt;
} src_sock_t;

typedef struct {
  uint32_t   pid;            /* whose sockets these are, and since when its thread holds the */
  uint64_t   found_ns;       /* slot: a slot can come to hold another process or thread */
  src_sock_t sock[ SRC_SOCK_MAX ];
  int        sock_cnt;
  int        is_walk_due;    /* a socket waits for a walk of every TCP connection */
  int        walk_kind_idx;  /* the walk kind to dump at the next look of a walk; 0: none begun */
  int        walk_wait_cnt;  /* looks until the next walk may begin */
  int        is_fd_cut;      /* the process has more fds than are looked at */
  int        is_sock_cut;    /* ...or more sockets than are followed */
} src_sock_process_t;

typedef struct {
  int      family, protocol;
  uint32_t states;
} src_sock_kind_t;

/* What a dump can be asked for. The first four are cheap, tens of microseconds: UDP, and
   the TCP sockets that listen. The last two each walk every TCP connection of the host,
   which takes milliseconds: asked only for a socket the cheap ones did not find, and one
   of the two per look. */
static src_sock_kind_t const src_sock_kind[] = {
  { AF_INET, IPPROTO_UDP, ~0U },                  { AF_INET6, IPPROTO_UDP, ~0U },
  { AF_INET, IPPROTO_TCP, 1U << TCP_LISTEN },     { AF_INET6, IPPROTO_TCP, 1U << TCP_LISTEN },
  { AF_INET, IPPROTO_TCP, SRC_SOCK_WALK_STATES }, { AF_INET6, IPPROTO_TCP, SRC_SOCK_WALK_STATES },
};
#define SRC_SOCK_KIND_CNT   TD_COUNT_OF( src_sock_kind )
#define SRC_SOCK_CHEAP_MASK (0x0fU)  /* the dumps of the first four kinds */
#define SRC_SOCK_WALK_FIRST (4)      /* the index of the first walk kind */

/* An unfound socket is walked for at its first look, then at these, then every
   SRC_SOCK_WALK_LOOKS: one made early and connected later is found within seconds, one
   that is no inet socket costs little. */
static int const src_sock_walk_look[] = { 0, 2, 8, 32 };
_Static_assert( SRC_SOCK_WALK_LOST_LOOKS > 32, "a lost socket is past the early walks" );

/* The kernel code a request needs: asked for only once all of it is there. */
static char const * const src_sock_module[] = { "inet_diag", "tcp_diag", "udp_diag" };

static src_sock_process_t src_sock_process[ TD_THREAD_MAX ];  /* by the process's first thread */
static src_sock_t         src_sock_kept[ SRC_SOCK_MAX ];      /* scratch of src_sock_refresh */
static int                src_sock_netlink_fd = -1;
static uint32_t           src_sock_sequence;
static uint64_t           src_sock_walk_ns;   /* when the last walk dump ran, whoever's */
static uint64_t           src_sock_read_ns;   /* the tick of the read being done... */
static uint64_t           src_sock_read_cnt;  /* ...and how many reads there have been */
static char               src_sock_answer[ SRC_SOCK_ANSWER_BYTES ];

/* A request as it is sent. Several can go in one send, one after the other. */
typedef struct {
  struct nlmsghdr         header;
  struct inet_diag_req_v2 request;
} src_sock_message_t;

/* The answer for one socket asked for alone: a few hundred bytes. The answers of a batch
   share src_sock_answer, this much each. */
#define SRC_SOCK_EXACT_BYTES ( SRC_SOCK_ANSWER_BYTES / SRC_SOCK_MAX )

/* The inodes of the process's open sockets, among the first SRC_SOCK_FD_MAX entries of
   its fd directory: each costs a readlink, and a process may hold a hundred thousand.
   Returns how many, or a negative errno; *is_cut says whether there were more entries. */
static int src_sock_inodes( uint32_t pid, uint64_t * inode, int inode_max, int * is_cut ) {
  char            path[ TD_PATH_BYTES ], link_target[ TD_KEY_BYTES ];
  DIR *           dir;
  struct dirent * entry = NULL;
  int             inode_cnt = 0, entry_cnt = 0, known_idx;
  snprintf( path, sizeof( path ), SRC_SOCK_FD_DIR, pid );  /* at most inode_max are followed */
  dir = opendir( path );
  if( !dir ) return -errno;
  while( inode_cnt < inode_max && entry_cnt++ < SRC_SOCK_FD_MAX && ( entry = readdir( dir ) ) ) {
    unsigned long socket_inode;
    ssize_t       link_bytes;
    link_bytes = readlinkat( dirfd( dir ), entry->d_name, link_target, TD_KEY_BYTES - 1 );
    if( link_bytes <= 0 ) continue;
    link_target[ link_bytes ] = '\0';
    if( sscanf( link_target, SRC_SOCK_LINK_FORMAT, &socket_inode ) != 1 ) continue;
    known_idx = 0;  /* an fd may be a duplicate of another: one row per socket */
    while( known_idx < inode_cnt && inode[ known_idx ] != socket_inode ) known_idx++;
    if( known_idx == inode_cnt ) inode[ inode_cnt++ ] = socket_inode;
  }
  *is_cut = entry_cnt > SRC_SOCK_FD_MAX && readdir( dir ) != NULL;
  closedir( dir );
  return inode_cnt;
}

/* Keep what one answer says of a socket. The queue of a TCP socket is bytes left to read;
   of a UDP socket, the memory its packets are charged, overhead included, which is what
   the kernel holds against the buffer's size and drops on. */
static void src_sock_keep( src_sock_t * sock, struct inet_diag_msg const * answer,
                           int attr_bytes ) {
  /* For a listening TCP socket the kernel puts connections waiting there, not bytes. */
  sock->queue_bytes = sock->protocol == IPPROTO_TCP && answer->idiag_state == TCP_LISTEN
                      ? 0.0 : (double)answer->idiag_rqueue;
  for( struct rtattr * attr = (struct rtattr *)( answer + 1 ); RTA_OK( attr, attr_bytes );
       attr = RTA_NEXT( attr, attr_bytes ) ) {
    uint32_t const * memory = (uint32_t const *)RTA_DATA( attr );
    if( attr->rta_type != INET_DIAG_SKMEMINFO ) continue;
    if( RTA_PAYLOAD( attr ) < ( SK_MEMINFO_DROPS + 1UL ) * sizeof( uint32_t ) ) continue;
    sock->buffer_bytes  = (double)memory[ SK_MEMINFO_RCVBUF ];
    sock->backlog_bytes = (double)memory[ SK_MEMINFO_BACKLOG ];
    sock->drop_cnt      = (double)memory[ SK_MEMINFO_DROPS ];
  }
}

/* A socket's row on the page: protocol and local port, the remote port of a connection,
   and the inode when another socket of the process would have the same name. */
static void src_sock_name( src_sock_process_t const * process, src_sock_t * sock ) {
  char const * protocol = sock->protocol == IPPROTO_UDP ? "udp" : "tcp";
  char const * version  = sock->family == AF_INET6 ? "6" : "";
  int          key_bytes;
  key_bytes = snprintf( sock->key, sizeof( sock->key ), "%s%s:%u", protocol, version,
                        ntohs( sock->id.idiag_sport ) );
  if( sock->id.idiag_dport )
    key_bytes += snprintf( sock->key + key_bytes, sizeof( sock->key ) - (size_t)key_bytes, ">%u",
                           ntohs( sock->id.idiag_dport ) );
  for( int i = 0; i < process->sock_cnt; i++ ) {
    src_sock_t const * other = &process->sock[ i ];
    if( other == sock || !other->is_found || strcmp( other->key, sock->key ) ) continue;
    snprintf( sock->key + key_bytes, sizeof( sock->key ) - (size_t)key_bytes, " #%lu",
              (unsigned long)sock->inode );
    return;
  }
}

/* Fill a request for `want` and give it the next sequence number, which is returned. */
static uint32_t src_sock_message( src_sock_message_t * message,
                                  struct inet_diag_req_v2 const * want, int is_dump ) {
  memset( message, 0, sizeof( *message ) );
  message->header.nlmsg_len   = sizeof( *message );
  message->header.nlmsg_type  = SOCK_DIAG_BY_FAMILY;
  message->header.nlmsg_flags = (uint16_t)( NLM_F_REQUEST | ( is_dump ? NLM_F_DUMP : 0 ) );
  message->header.nlmsg_seq   = ++src_sock_sequence;
  message->request            = *want;
  return message->header.nlmsg_seq;
}

/* Send one sock_diag dump and walk the answers that carry its sequence number. A dump
   (kind_idx says which) walks the sockets of a family, protocol and state: answers are
   matched to the process's sockets by inode. */
static void src_sock_ask( src_sock_process_t * process, struct inet_diag_req_v2 const * want,
                          int kind_idx ) {
  src_sock_message_t message;
  uint32_t           sequence = src_sock_message( &message, want, 1 );
  if( send( src_sock_netlink_fd, &message, sizeof( message ), 0 ) < 0 ) return;

  for(;;) {
    ssize_t  got  = recv( src_sock_netlink_fd, src_sock_answer, sizeof( src_sock_answer ), 0 );
    uint32_t left = got > 0 ? (uint32_t)got : 0U;
    if( !left ) return;  /* nothing came within the timeout: a hiccup, nothing is concluded */
    for( struct nlmsghdr * header = (struct nlmsghdr *)src_sock_answer; NLMSG_OK( header, left );
         header = NLMSG_NEXT( header, left ) ) {
      struct inet_diag_msg const * answer = (struct inet_diag_msg const *)NLMSG_DATA( header );
      int attr_bytes = (int)header->nlmsg_len - (int)NLMSG_LENGTH( sizeof( *answer ) );
      if( header->nlmsg_seq != sequence ) continue;  /* the tail of an earlier request */
      if( header->nlmsg_type != SOCK_DIAG_BY_FAMILY ) return;  /* done, or an error */
      if( attr_bytes < 0 ) return;  /* shorter than an answer is: not one */
      for( int i = 0; i < process->sock_cnt; i++ ) {
        src_sock_t * sock = &process->sock[ i ];
        if( sock->inode != answer->idiag_inode ) continue;
        /* Bound or connected since it was named: name it again, ask for it alone again. */
        if( sock->is_found && memcmp( &sock->id, &answer->id, sizeof( sock->id ) ) )
          sock->key[ 0 ] = '\0';
        if( !sock->is_found || kind_idx < sock->kind_idx ) sock->kind_idx = kind_idx;
        sock->family   = want->sdiag_family;
        sock->protocol = want->sdiag_protocol;
        sock->id       = answer->id;
        sock->is_found = sock->is_seen = 1;
        src_sock_keep( sock, answer, attr_bytes );
      }
    }
  }
}

static void src_sock_want( struct inet_diag_req_v2 * want, int family, int protocol,
                           uint32_t states ) {
  memset( want, 0, sizeof( *want ) );
  want->sdiag_family   = (uint8_t)family;
  want->sdiag_protocol = (uint8_t)protocol;
  want->idiag_ext      = 1U << ( INET_DIAG_SKMEMINFO - 1 );
  want->idiag_states   = states;
}

/* Run the dumps of kind_mask, to find the process's sockets among the host's and to read
   those that only a dump reads. Then name what was found for the first time. */
static void src_sock_dump( src_sock_process_t * process, uint32_t kind_mask ) {
  struct inet_diag_req_v2 want;
  for( int i = 0; i < SRC_SOCK_KIND_CNT; i++ ) {
    src_sock_kind_t const * kind = &src_sock_kind[ i ];
    if( !( kind_mask & ( 1U << i ) ) ) continue;
    src_sock_want( &want, kind->family, kind->protocol, kind->states );
    src_sock_ask( process, &want, i );
  }
  for( int i = 0; i < process->sock_cnt; i++ ) {
    src_sock_t * sock = &process->sock[ i ];
    if( sock->is_found && !sock->key[ 0 ] ) { src_sock_name( process, sock ); sock->is_exact = 1; }
  }
}

/* Is a walk of every TCP connection due for a socket the cheap dumps did not find? */
static int src_sock_walk_is_due( src_sock_process_t const * process ) {
  for( int i = 0; i < process->sock_cnt; i++ ) {
    src_sock_t const * sock = &process->sock[ i ];
    if( sock->is_found ) continue;
    if( sock->miss_cnt >= SRC_SOCK_WALK_LOST_LOOKS && !( sock->miss_cnt % SRC_SOCK_WALK_LOOKS ) )
      return 1;
    for( int look = 0; look < TD_COUNT_OF( src_sock_walk_look ); look++ )
      if( sock->miss_cnt == src_sock_walk_look[ look ] ) return 1;
  }
  return 0;
}

static int src_sock_has_unfound( src_sock_process_t const * process ) {
  for( int i = 0; i < process->sock_cnt; i++ ) if( !process->sock[ i ].is_found ) return 1;
  return 0;
}

/* A socket the kernel no longer lists (reset, shut down, its fd still held) is forgotten:
   it leaves the page, and is not asked or walked for at every look. */
static void src_sock_forget( src_sock_t * sock ) {
  sock->is_found = 0;
  sock->key[ 0 ] = '\0';
  sock->miss_cnt = SRC_SOCK_WALK_LOST_LOOKS;
}

/* Look at the process's fds again. Sockets it still has keep what is known of them.
   Returns the cheap dumps that are due: the kind that reads a socket only a dump reads,
   and all four while some socket is not found, which may be bound later. A connection
   that only a walk would read is forgotten. */
static uint32_t src_sock_refresh( src_sock_process_t * process, uint32_t pid ) {
  uint64_t inode[ SRC_SOCK_MAX + 1 ];  /* one more, to know that there are more */
  uint32_t kind_mask = 0U;
  int      inode_cnt = src_sock_inodes( pid, inode, SRC_SOCK_MAX + 1, &process->is_fd_cut );
  process->is_sock_cut = inode_cnt > SRC_SOCK_MAX;
  if( inode_cnt < 0 )            inode_cnt = 0;
  if( inode_cnt > SRC_SOCK_MAX ) inode_cnt = SRC_SOCK_MAX;
  for( int i = 0; i < inode_cnt; i++ ) {
    src_sock_t * kept      = &src_sock_kept[ i ];
    int          known_idx = 0;
    while( known_idx < process->sock_cnt && process->sock[ known_idx ].inode != inode[ i ] )
      known_idx++;
    if( known_idx < process->sock_cnt ) { *kept = process->sock[ known_idx ]; }
    else { memset( kept, 0, sizeof( *kept ) ); kept->inode = inode[ i ]; }
    kept->is_seen = 0;
    if( kept->is_found && !kept->is_exact && kept->kind_idx >= SRC_SOCK_WALK_FIRST )
      src_sock_forget( kept );
    if( !kept->is_found )      kind_mask |= SRC_SOCK_CHEAP_MASK;
    else if( !kept->is_exact ) kind_mask |= 1U << kept->kind_idx;
  }
  memcpy( process->sock, src_sock_kept, (size_t)inode_cnt * sizeof( src_sock_t ) );
  process->sock_cnt = inode_cnt;
  return kind_mask;
}

/* One step of a walk: one of its two dumps, each of which takes milliseconds. Begun when a
   socket waits for one and the last walk of this process is long enough ago; the second
   dump follows at the next look. Never two walk dumps, of any process, within two seconds:
   several in one pacing window of the store would slow the whole source. */
static void src_sock_walk( src_sock_process_t * process, uint64_t now_ns ) {
  if( !src_sock_has_unfound( process ) ) process->is_walk_due = process->walk_kind_idx = 0;
  if( process->walk_wait_cnt ) process->walk_wait_cnt--;
  if( !process->is_walk_due || now_ns - src_sock_walk_ns < SRC_SOCK_WALK_SPACE_NS ) return;
  if( !process->walk_kind_idx && process->walk_wait_cnt ) return;
  if( !process->walk_kind_idx ) process->walk_kind_idx = SRC_SOCK_WALK_FIRST;
  src_sock_dump( process, 1U << process->walk_kind_idx );
  src_sock_walk_ns = now_ns;
  if( ++process->walk_kind_idx < SRC_SOCK_KIND_CNT ) return;  /* the other half: next look */
  process->is_walk_due   = process->walk_kind_idx = 0;
  process->walk_wait_cnt = SRC_SOCK_WALK_GAP_LOOKS;
}

/* The look, once every SRC_SOCK_LOOK_READS reads: the fds, the cheap dumps that are
   due, and a step of a walk. A socket read by dump that its dump no longer sees is gone. */
static void src_sock_look( src_sock_process_t * process, uint32_t pid, uint64_t now_ns ) {
  uint32_t kind_mask = src_sock_refresh( process, pid );
  if( kind_mask ) src_sock_dump( process, kind_mask );
  for( int i = 0; i < process->sock_cnt; i++ ) {
    src_sock_t * sock = &process->sock[ i ];
    if( sock->is_found && !sock->is_exact && !sock->is_seen ) src_sock_forget( sock );
  }
  process->is_walk_due |= src_sock_walk_is_due( process );
  src_sock_walk( process, now_ns );
  for( int i = 0; i < process->sock_cnt; i++ )
    process->sock[ i ].miss_cnt += !process->sock[ i ].is_found;
}

/* What to ask for one known socket by its identity. For UDP the kernel's lookup takes the
   request's destination as the local end, the reverse of what a dump reports; for TCP it
   does not. */
static void src_sock_exact_want( src_sock_t const * sock, struct inet_diag_req_v2 * want ) {
  src_sock_want( want, sock->family, sock->protocol, ~0U );
  want->id = sock->id;
  if( sock->protocol == IPPROTO_UDP ) {
    want->id.idiag_sport = sock->id.idiag_dport;
    want->id.idiag_dport = sock->id.idiag_sport;
    memcpy( want->id.idiag_src, sock->id.idiag_dst, sizeof( want->id.idiag_src ) );
    memcpy( want->id.idiag_dst, sock->id.idiag_src, sizeof( want->id.idiag_dst ) );
  }
}

/* What the kernel said of one socket asked for alone: its answer, kept; or an error (the
   socket is no more, or is no longer found so), or another socket's answer, which leave
   it to the dumps. */
static void src_sock_exact_answer( src_sock_t * sock, struct nlmsghdr const * header ) {
  struct inet_diag_msg const * answer = (struct inet_diag_msg const *)NLMSG_DATA( header );
  int attr_bytes = (int)header->nlmsg_len - (int)NLMSG_LENGTH( sizeof( *answer ) );
  if( header->nlmsg_type == NLMSG_ERROR ) { sock->is_exact = 0; return; }
  if( header->nlmsg_type != SOCK_DIAG_BY_FAMILY || attr_bytes < 0 ) return;  /* not an answer */
  sock->is_exact = answer->idiag_inode == sock->inode;
  if( sock->is_exact ) src_sock_keep( sock, answer, attr_bytes );
}

/* Ask for every socket of the process that is asked for alone, in one send: a request
   each, one after the other, each with its sequence number. The kernel answers them in
   turn, each with an answer or an error of the request's number, whatever became of the
   others: a socket closed since the last look gets its error, and those after it their
   answers. One recvmmsg takes what has come; a single answer is taken with a plain recv,
   the cheaper call. is_asked[ i ] says socket i was asked for. A socket with no answer
   within the timeout stays as it was: a hiccup concludes nothing. */
static void src_sock_exact_all( src_sock_process_t * process, uint8_t * is_asked ) {
  src_sock_message_t message[ SRC_SOCK_MAX ];
  struct mmsghdr     datagram[ SRC_SOCK_MAX ];
  struct iovec       room[ SRC_SOCK_MAX ];
  src_sock_t *       asked[ SRC_SOCK_MAX ];
  uint32_t           first_sequence = src_sock_sequence + 1U;
  int                asked_cnt = 0, answered_cnt = 0;

  for( int i = 0; i < process->sock_cnt; i++ ) {
    struct inet_diag_req_v2 want;
    src_sock_t *            sock = &process->sock[ i ];
    is_asked[ i ] = sock->is_found && sock->is_exact;
    if( !is_asked[ i ] ) continue;
    src_sock_exact_want( sock, &want );
    src_sock_message( &message[ asked_cnt ], &want, 0 );
    asked[ asked_cnt++ ] = sock;
  }
  if( !asked_cnt ) return;
  if( send( src_sock_netlink_fd, message, (size_t)asked_cnt * sizeof( message[ 0 ] ), 0 ) < 0 )
    return;
  while( answered_cnt < asked_cnt ) {
    int datagram_cnt = 1;
    for( int i = 0; i < asked_cnt - answered_cnt; i++ ) {
      room[ i ].iov_base = src_sock_answer + (size_t)i * SRC_SOCK_EXACT_BYTES;
      room[ i ].iov_len  = SRC_SOCK_EXACT_BYTES;
      memset( &datagram[ i ], 0, sizeof( datagram[ i ] ) );
      datagram[ i ].msg_hdr.msg_iov    = &room[ i ];
      datagram[ i ].msg_hdr.msg_iovlen = 1;
    }
    if( asked_cnt - answered_cnt == 1 ) {
      ssize_t got = recv( src_sock_netlink_fd, room[ 0 ].iov_base, room[ 0 ].iov_len, MSG_TRUNC );
      if( got <= 0 ) return;
      datagram[ 0 ].msg_len = (unsigned)got;  /* its full length, MSG_TRUNC says: cut if longer */
      if( (size_t)got > room[ 0 ].iov_len ) datagram[ 0 ].msg_hdr.msg_flags = MSG_TRUNC;
    } else {
      /* Waits for the first, within the socket's timeout; takes the others if they are there. */
      datagram_cnt = recvmmsg( src_sock_netlink_fd, datagram,
                               (unsigned)( asked_cnt - answered_cnt ), MSG_WAITFORONE, NULL );
      if( datagram_cnt <= 0 ) return;
    }
    for( int i = 0; i < datagram_cnt; i++ ) {
      uint32_t left = datagram[ i ].msg_len;
      /* One that was cut short is not read; counted, so that it is not waited for again. */
      if( datagram[ i ].msg_hdr.msg_flags & MSG_TRUNC ) { answered_cnt++; continue; }
      for( struct nlmsghdr * header = (struct nlmsghdr *)room[ i ].iov_base;
           NLMSG_OK( header, left ); header = NLMSG_NEXT( header, left ) ) {
        uint32_t asked_idx = header->nlmsg_seq - first_sequence;
        if( asked_idx >= (uint32_t)asked_cnt ) continue;  /* the tail of an earlier request */
        src_sock_exact_answer( asked[ asked_idx ], header );
        answered_cnt++;
      }
    }
  }
}

/* Is the kernel code a request needs already there? The first sock_diag request on a host
   where it is a module not yet loaded makes the kernel load it, and a module load reaches
   every CPU. So the source asks for nothing until all of it is loaded or built in. */
static int src_sock_is_loaded( void ) {
  struct utsname kernel;
  char           path[ TD_PATH_BYTES ], name[ TD_KEY_BYTES ];
  uname( &kernel );
  snprintf( path, sizeof( path ), SRC_SOCK_BUILTIN_FILE, kernel.release );
  for( int i = 0; i < TD_COUNT_OF( src_sock_module ); i++ ) {
    int is_there = 0;
    snprintf( name, sizeof( name ), "\n%s ", src_sock_module[ i ] );  /* a line of /proc/modules */
    td_scratch[ 0 ] = '\n';  /* so that the first line matches like the others */
    if( td_file_read( SRC_SOCK_MODULES_FILE, td_scratch + 1, TD_SCRATCH_BYTES - 1UL ) > 0L )
      is_there = !!strstr( td_scratch, name );
    snprintf( name, sizeof( name ), "/%s.ko", src_sock_module[ i ] );  /* of modules.builtin */
    if( !is_there && td_file_read( path, td_scratch, TD_SCRATCH_BYTES ) > 0L )
      is_there = !!strstr( td_scratch, name );
    if( !is_there ) return 0;
  }
  return 1;
}

/* Usable when sock_diag is there and the process's fds can be listed. The listing itself
   is the look's, in the read path, where its cost is paced: here only what it found. */
static int src_sock_probe( td_ctx_t const * ctx, td_out_t * out ) {
  src_sock_process_t const * process = &src_sock_process[ ctx->thread_idx ];
  struct timeval             timeout = { .tv_usec = SRC_SOCK_TIMEOUT_US };
  char                       path[ TD_PATH_BYTES ];
  if( src_sock_netlink_fd < 0 && !src_sock_is_loaded() ) {
    td_unavailable( out, -1, "sock_diag is not loaded: modprobe tcp_diag udp_diag at boot" );
    return -ENOENT;
  }
  if( src_sock_netlink_fd < 0 ) {
    src_sock_netlink_fd = socket( AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG );
    setsockopt( src_sock_netlink_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof( timeout ) );
  }
  if( src_sock_netlink_fd < 0 ) {
    td_unavailable( out, -1, "no sock_diag netlink socket" );
    return -ENOENT;
  }
  snprintf( path, sizeof( path ), SRC_SOCK_FD_DIR, ctx->pid );
  if( access( path, R_OK | X_OK ) ) {
    td_unavailable( out, -1, "cannot list the process's fds: another user, or not dumpable" );
    return -EACCES;
  }
  if( process->pid != ctx->pid ) return 0;  /* not looked at yet */
  if( process->is_sock_cut )
    td_unavailable( out, SRC_SOCK_QUEUE, "the process has more sockets than are followed" );
  if( process->is_fd_cut )
    td_unavailable( out, SRC_SOCK_QUEUE, "the process has more fds than are looked at" );
  return 0;
}

static void src_sock_emit( td_out_t * out, src_sock_t const * sock ) {
  td_emit( out, SRC_SOCK_QUEUE,   sock->key, sock->queue_bytes );
  td_emit( out, SRC_SOCK_BACKLOG, sock->key, sock->backlog_bytes );
  td_emit( out, SRC_SOCK_DROPS,   sock->key, sock->drop_cnt );
  if( sock->buffer_bytes > 0.0 )
    td_emit( out, SRC_SOCK_FILL, sock->key, 100.0 * sock->queue_bytes / sock->buffer_bytes );
}

static void src_sock_read( td_ctx_t const * ctx, td_out_t * out ) {
  src_sock_process_t * process = &src_sock_process[ ctx->thread_idx ];
  uint8_t              is_dumped[ SRC_SOCK_MAX ], is_asked[ SRC_SOCK_MAX ];
  uint64_t             found_ns = td_thread[ ctx->thread_idx ].found_ns;
  if( process->pid != ctx->pid || process->found_ns != found_ns ) {  /* the slot has changed */
    memset( process, 0, sizeof( *process ) );                        /* hands: start afresh */
    process->pid      = ctx->pid;
    process->found_ns = found_ns;
  }
  if( ctx->now_ns != src_sock_read_ns ) {  /* the first process of this read */
    src_sock_read_ns = ctx->now_ns;
    src_sock_read_cnt++;
  }
  if( !( ( src_sock_read_cnt + (uint64_t)ctx->thread_idx ) % SRC_SOCK_LOOK_READS ) )
    src_sock_look( process, ctx->pid, ctx->now_ns );
  /* Sockets asked for alone first; then those only a dump reads, which are that much older. */
  for( int i = 0; i < process->sock_cnt; i++ )
    is_dumped[ i ] = process->sock[ i ].is_found && !process->sock[ i ].is_exact;
  src_sock_exact_all( process, is_asked );
  for( int i = 0; i < process->sock_cnt; i++ )
    if( is_asked[ i ] ) src_sock_emit( out, &process->sock[ i ] );
  td_resolution( out, ctx->period_ms * SRC_SOCK_LOOK_READS );
  for( int i = 0; i < process->sock_cnt; i++ )
    if( is_dumped[ i ] ) src_sock_emit( out, &process->sock[ i ] );
}

td_source_t const td_src_sock = {
  .name = "sock", .scope = TD_SCOPE_PROCESS, .period_ms = 100,
  .metric = src_sock_metric, .metric_cnt = SRC_SOCK_METRIC_CNT,
  .probe = src_sock_probe, .read = src_sock_read,
};
