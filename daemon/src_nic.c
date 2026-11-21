/* src_nic.c: what the NIC driver itself counts. Per queue: packets, from the kernel's
   netdev netlink family where the driver gives them there (newer drivers no longer do
   through ethtool), else from ethtool's counters, whose names are classified by pattern,
   not by driver. Through ethtool as well: every counter whose name says pressure (drops,
   refill failures, shaping, queue stops), once it moves; and the ring sizes. Read every 10 s. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <linux/ethtool.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "td_source.h"

enum { SRC_NIC_RX_QUEUE, SRC_NIC_TX_QUEUE, SRC_NIC_PRESSURE, SRC_NIC_RING, SRC_NIC_METRIC_CNT };

/* One row per metric, in the order of the enum above. */
static td_metric_t const src_nic_metric[ SRC_NIC_METRIC_CNT ] = {
  { "rx_queue", "rx packets",     "/s", TD_TABLE, TD_RATE,  0, 0 },
  { "tx_queue", "tx packets",     "/s", TD_TABLE, TD_RATE,  0, 0 },
  { "pressure", "driver counter", "",   TD_TABLE, TD_SINCE, 0, 1 },
  { "ring",     "ring size",      "",   TD_TABLE, TD_LEVEL, 0, 0 },
};

#define SRC_NIC_DIR           "/sys/class/net"
#define SRC_NIC_INTERFACE_MAX (8)
#define SRC_NIC_COUNTER_MAX   (8192)  /* an interface whose driver has more is left out */
/* Room for twice the names accepted: the kernel writes as many as the driver has when it
   is asked, and a driver can grow between the count and the names (an ethtool -L). */
#define SRC_NIC_BLOCK_BYTES \
  ( sizeof( struct ethtool_gstrings ) + 2UL * SRC_NIC_COUNTER_MAX * ETH_GSTRING_LEN )
/* Slow on purpose. Each GSTATS makes the kernel vmalloc and vfree a block. Freed blocks pile
   up until a purge flushes kernel TLB entries on every CPU, isolated ones too: an IPI on
   x86. docs/reading-the-page.md says how rarely at this period. */
#define SRC_NIC_PERIOD_MS     (10000)

/* The kernel's "netdev" generic netlink family, and of its per-queue statistics (kernel
   6.9) what is asked here. The numbers are the kernel's, of linux/netdev.h; written out so
   that this builds against headers older than they are. On a kernel without the family,
   or without the command, the answer is an error and ethtool's counters serve as before. */
#define SRC_NIC_FAMILY_NAME   "netdev"
#define SRC_NIC_QSTATS_GET    (12)  /* the command: a dump, one row per queue */
#define SRC_NIC_A_IFINDEX     (1)   /* attributes of a row: whose queue, */
#define SRC_NIC_A_QUEUE_TYPE  (2)   /* receive or transmit, */
#define SRC_NIC_A_QUEUE_ID    (3)   /* which, */
#define SRC_NIC_A_SCOPE       (4)   /* (of the request: per queue, not per device) */
#define SRC_NIC_A_RX_PACKETS  (8)   /* and its packets */
#define SRC_NIC_A_TX_PACKETS  (10)
#define SRC_NIC_SCOPE_QUEUE   (1)
#define SRC_NIC_QUEUE_RX      (0)
#define SRC_NIC_QUEUE_TX      (1)
#define SRC_NIC_ANSWER_BYTES  (1UL << 15)  /* the most the kernel puts in one datagram of a dump */
#define SRC_NIC_REQUEST_BYTES (64)
#define SRC_NIC_TIMEOUT_US    (50000)      /* the kernel always answers; if not, move on */

/* What each counter of an interface is. */
enum { SRC_NIC_ROLE_IGNORED, SRC_NIC_ROLE_RX, SRC_NIC_ROLE_TX, SRC_NIC_ROLE_PRESSURE };

typedef struct {
  char const * shape;  /* a counter name with every run of digits written as '#' */
  int          role;
} src_nic_shape_t;

/* The names drivers give a queue's packet counter. */
static src_nic_shape_t const src_nic_queue_shape[] = {
  { "queue_#_rx_cnt",     SRC_NIC_ROLE_RX }, { "queue_#_tx_cnt",     SRC_NIC_ROLE_TX }, /* ena */
  { "rx_queue_#_packets", SRC_NIC_ROLE_RX }, { "tx_queue_#_packets", SRC_NIC_ROLE_TX }, /* igb */
  { "rx-#.packets",       SRC_NIC_ROLE_RX }, { "tx-#.packets",       SRC_NIC_ROLE_TX }, /* i40e */
  { "rx#_packets",        SRC_NIC_ROLE_RX }, { "tx#_packets",        SRC_NIC_ROLE_TX }, /* mlx5 */
  { "rx-#.rx_packets",    SRC_NIC_ROLE_RX }, { "tx-#.tx_packets",    SRC_NIC_ROLE_TX }, /* sfc */
};
#define SRC_NIC_SHAPE_CNT TD_COUNT_OF( src_nic_queue_shape )

/* A counter whose name holds one of these says the NIC or its driver is under pressure... */
static char const * const src_nic_pressure_word[] = {
  "drop", "discard", "miss", "overrun", "alloc_fail", "failed", "no_buf", "out_of_buffer",
  "empty", "queue_stop", "stopped", "tx_busy", "restart", "allowance_exceeded", "timeout",
  "refil_partial", "bad_", "_err", "fifo",
};
#define SRC_NIC_WORD_CNT TD_COUNT_OF( src_nic_pressure_word )

/* ...unless it holds one of these: counters that move in normal operation. */
static char const * const src_nic_benign_word[] = { "fdir_miss", "cache_empty", "pp_alloc_empty" };
#define SRC_NIC_BENIGN_CNT TD_COUNT_OF( src_nic_benign_word )

typedef struct {
  char     name[ IFNAMSIZ ];
  uint32_t counter_cnt;
  uint32_t ifindex;      /* the kernel's number for it: what a row of queue statistics names */
  int      is_learning;  /* learnt at this probe: whether it has queue statistics is not known */
  int      has_qstats;   /* its queues' packets come from the netdev family, not from ethtool */
  uint8_t  role [ SRC_NIC_COUNTER_MAX ];
  uint16_t queue[ SRC_NIC_COUNTER_MAX ];  /* the queue number, for a queue counter */
  uint64_t first[ SRC_NIC_COUNTER_MAX ];  /* the first value read: only what moves is shown */
  char     label[ SRC_NIC_COUNTER_MAX ][ ETH_GSTRING_LEN ];
} src_nic_interface_t;

static src_nic_interface_t src_nic_interface[ SRC_NIC_INTERFACE_MAX ];  /* name "": free */
static int                 src_nic_fd = -1;
static int                 src_nic_genl_fd = -1;  /* the generic netlink socket */
static int                 src_nic_is_asked;      /* the netdev family has been looked for */
static uint16_t            src_nic_family;        /* its id; 0: this kernel has none */
static uint32_t            src_nic_sequence;
static char                src_nic_answer[ SRC_NIC_ANSWER_BYTES ];

/* The ioctl argument block. The kernel fills in as many counters as the driver has,
   whatever the caller asked for: it is sized for the most this source accepts. */
static union {
  struct ethtool_gstrings  strings;
  struct ethtool_stats     stats;
  struct ethtool_sset_info set;
  char                     bytes[ SRC_NIC_BLOCK_BYTES ];
} src_nic_block;

static int src_nic_ioctl( char const * interface_name, void * block ) {
  struct ifreq request;
  memset( &request, 0, sizeof( request ) );
  snprintf( request.ifr_name, sizeof( request.ifr_name ), "%.*s", IFNAMSIZ - 1, interface_name );
  request.ifr_data = (char *)block;
  return ioctl( src_nic_fd, SIOCETHTOOL, &request ) ? -errno : 0;
}

/* How many counters the interface's driver has; 0 when it has none or cannot say. */
static uint32_t src_nic_counter_cnt( char const * interface_name ) {
  memset( &src_nic_block, 0, sizeof( src_nic_block.set ) + sizeof( uint32_t ) );
  src_nic_block.set.cmd       = ETHTOOL_GSSET_INFO;
  src_nic_block.set.sset_mask = 1UL << ETH_SS_STATS;
  if( src_nic_ioctl( interface_name, &src_nic_block ) || !src_nic_block.set.sset_mask ) return 0U;
  return src_nic_block.set.data[ 0 ];
}

static int src_nic_has_word( char const * name, char const * const * word, int word_cnt ) {
  for( int i = 0; i < word_cnt; i++ ) if( strstr( name, word[ i ] ) ) return 1;
  return 0;
}

/* Decide what a counter is from its name. Returns the role; *queue gets the queue number. */
static int src_nic_classify( char const * label, uint16_t * queue ) {
  char shape[ ETH_GSTRING_LEN + 1 ], lower[ ETH_GSTRING_LEN + 1 ] = "";
  int  shape_bytes = 0, has_number = 0, is_pressure;
  for( int i = 0; i < ETH_GSTRING_LEN && label[ i ]; i++ ) {
    unsigned char letter    = (unsigned char)label[ i ];
    int           in_number = i && isdigit( (unsigned char)label[ i - 1 ] );
    lower[ i ]     = (char)tolower( letter );
    lower[ i + 1 ] = '\0';
    if( !isdigit( letter ) ) { shape[ shape_bytes++ ] = lower[ i ]; continue; }
    if( !has_number ) *queue = (uint16_t)atoi( label + i );
    if( !in_number ) shape[ shape_bytes++ ] = '#';
    has_number = 1;
  }
  shape[ shape_bytes ] = '\0';
  for( int i = 0; i < SRC_NIC_SHAPE_CNT; i++ )
    if( !strcmp( shape, src_nic_queue_shape[ i ].shape ) ) return src_nic_queue_shape[ i ].role;
  is_pressure = !src_nic_has_word( lower, src_nic_benign_word, SRC_NIC_BENIGN_CNT ) &&
                src_nic_has_word( lower, src_nic_pressure_word, SRC_NIC_WORD_CNT );
  return is_pressure ? SRC_NIC_ROLE_PRESSURE : SRC_NIC_ROLE_IGNORED;
}

/* The kernel's number for an interface; 0 when it has none by that name. */
static uint32_t src_nic_ifindex( char const * interface_name ) {
  struct ifreq request;
  memset( &request, 0, sizeof( request ) );
  snprintf( request.ifr_name, sizeof( request.ifr_name ), "%.*s", IFNAMSIZ - 1, interface_name );
  return ioctl( src_nic_fd, SIOCGIFINDEX, &request ) ? 0U : (uint32_t)request.ifr_ifindex;
}

/* Learn an interface: read its counter names, once, and decide what each is. The names are
   asked for only here: each such call makes the kernel allocate and free a large block. */
static void src_nic_learn( src_nic_interface_t * interface, char const * interface_name,
                           uint32_t counter_cnt ) {
  memset( &src_nic_block, 0, sizeof( src_nic_block ) );
  src_nic_block.strings.cmd        = ETHTOOL_GSTRINGS;
  src_nic_block.strings.string_set = ETH_SS_STATS;
  src_nic_block.strings.len        = counter_cnt;
  memset( interface, 0, sizeof( *interface ) );
  if( src_nic_ioctl( interface_name, &src_nic_block ) ) return;
  if( src_nic_block.strings.len != counter_cnt ) return;  /* it changed meanwhile: next probe */

  snprintf( interface->name, sizeof( interface->name ), "%.*s", IFNAMSIZ - 1, interface_name );
  interface->counter_cnt = counter_cnt;
  interface->ifindex     = src_nic_ifindex( interface_name );
  interface->is_learning = 1;  /* the probe asks next whether its queues have statistics */
  for( uint32_t i = 0U; i < counter_cnt; i++ ) {
    char * label = interface->label[ i ];
    memcpy( label, src_nic_block.strings.data + (size_t)i * ETH_GSTRING_LEN, ETH_GSTRING_LEN );
    label[ ETH_GSTRING_LEN - 1 ] = '\0';
    interface->role[ i ]  = (uint8_t)src_nic_classify( label, &interface->queue[ i ] );
    interface->first[ i ] = UINT64_MAX;
  }
}

/* The slot of a known interface, or a free one, or NULL. */
static src_nic_interface_t * src_nic_slot( char const * interface_name ) {
  src_nic_interface_t * free_slot = NULL;
  for( int i = 0; i < SRC_NIC_INTERFACE_MAX; i++ ) {
    src_nic_interface_t * interface = &src_nic_interface[ i ];
    if( !strcmp( interface->name, interface_name ) ) return interface;
    if( !interface->name[ 0 ] && !free_slot ) free_slot = interface;
  }
  return free_slot;
}

/* ---- queue statistics, from the netdev family ------------------------------------------ */

typedef struct {
  struct nlmsghdr   header;
  struct genlmsghdr family;
  char              attr[ SRC_NIC_REQUEST_BYTES ];
} src_nic_request_t;

/* What is done with the attributes of one answer. */
typedef void ( *src_nic_row_t )( struct nlattr const * attr, int attr_bytes, void * arg );

/* Begin a generic netlink request; then append its attributes, one call each. */
static void src_nic_request( src_nic_request_t * request, uint16_t family, uint8_t command,
                             int is_dump ) {
  memset( request, 0, sizeof( *request ) );
  request->header.nlmsg_len   = NLMSG_LENGTH( GENL_HDRLEN );
  request->header.nlmsg_type  = family;
  request->header.nlmsg_flags = (uint16_t)( NLM_F_REQUEST | ( is_dump ? NLM_F_DUMP : 0 ) );
  request->header.nlmsg_seq   = ++src_nic_sequence;
  request->family.cmd         = command;
  request->family.version     = 1;
}

static void src_nic_request_attr( src_nic_request_t * request, uint16_t type, void const * data,
                                  size_t byte_cnt ) {
  size_t          start = NLMSG_ALIGN( request->header.nlmsg_len );
  struct nlattr * attr  = (struct nlattr *)( (char *)request + start );
  if( start + NLA_ALIGN( NLA_HDRLEN + byte_cnt ) > sizeof( *request ) ) return;  /* no room */
  attr->nla_type = type;
  attr->nla_len  = (uint16_t)( NLA_HDRLEN + byte_cnt );
  memcpy( (char *)attr + NLA_HDRLEN, data, byte_cnt );
  request->header.nlmsg_len = (uint32_t)( start + NLA_ALIGN( attr->nla_len ) );
}

/* The attribute after this one, with what is left of the answer; and whether one is there. */
static struct nlattr const * src_nic_attr_next( struct nlattr const * attr, int * attr_bytes ) {
  *attr_bytes -= (int)NLA_ALIGN( attr->nla_len );
  return (struct nlattr const *)( (char const *)attr + NLA_ALIGN( attr->nla_len ) );
}

static int src_nic_attr_ok( struct nlattr const * attr, int attr_bytes ) {
  return attr_bytes >= (int)NLA_HDRLEN && attr->nla_len >= NLA_HDRLEN &&
         (int)attr->nla_len <= attr_bytes;
}

/* The whole number an attribute holds, in 2, 4 or 8 bytes. */
static uint64_t src_nic_attr_uint( struct nlattr const * attr ) {
  char const * data     = (char const *)attr + NLA_HDRLEN;
  size_t       byte_cnt = (size_t)attr->nla_len - NLA_HDRLEN;
  uint64_t     wide     = 0UL;
  uint32_t     word     = 0U;
  uint16_t     half     = 0U;
  if( byte_cnt >= sizeof( wide ) ) { memcpy( &wide, data, sizeof( wide ) ); return wide; }
  if( byte_cnt >= sizeof( word ) ) { memcpy( &word, data, sizeof( word ) ); return word; }
  if( byte_cnt >= sizeof( half ) ) { memcpy( &half, data, sizeof( half ) ); return half; }
  return 0UL;
}

/* Send a request and hand the attributes of each answer to `row`. Returns 0 once the kernel
   has answered in full, else a negative errno: the kernel's own (no such family, no such
   command: an older kernel), or -ETIMEDOUT when nothing came. */
static int src_nic_ask( src_nic_request_t const * request, src_nic_row_t row, void * arg ) {
  uint32_t sequence = request->header.nlmsg_seq;
  int      is_dump  = ( request->header.nlmsg_flags & NLM_F_DUMP ) == NLM_F_DUMP;
  if( send( src_nic_genl_fd, request, request->header.nlmsg_len, 0 ) < 0 ) return -errno;
  for(;;) {
    ssize_t  got  = recv( src_nic_genl_fd, src_nic_answer, sizeof( src_nic_answer ), 0 );
    uint32_t left = got > 0 ? (uint32_t)got : 0U;
    if( !left ) return -ETIMEDOUT;
    for( struct nlmsghdr * header = (struct nlmsghdr *)src_nic_answer; NLMSG_OK( header, left );
         header = NLMSG_NEXT( header, left ) ) {
      char const * payload    = (char const *)NLMSG_DATA( header );
      int          attr_bytes = (int)header->nlmsg_len - (int)NLMSG_LENGTH( GENL_HDRLEN );
      if( header->nlmsg_seq != sequence ) continue;  /* the tail of an earlier request */
      if( header->nlmsg_type == NLMSG_DONE ) return 0;
      if( header->nlmsg_type == NLMSG_ERROR )
        return header->nlmsg_len >= NLMSG_LENGTH( sizeof( struct nlmsgerr ) )
               ? ( (struct nlmsgerr const *)payload )->error : -EBADMSG;
      if( attr_bytes < 0 ) continue;
      row( (struct nlattr const *)( payload + GENL_HDRLEN ), attr_bytes, arg );
      if( !is_dump ) return 0;
    }
  }
}

/* One family of the kernel's list: keep its id when it is the netdev family. */
static void src_nic_family_row( struct nlattr const * attr, int attr_bytes, void * arg ) {
  uint16_t id      = 0U;
  int      is_ours = 0;
  for( ; src_nic_attr_ok( attr, attr_bytes ); attr = src_nic_attr_next( attr, &attr_bytes ) ) {
    int type = attr->nla_type & NLA_TYPE_MASK;
    if( type == CTRL_ATTR_FAMILY_ID ) id = (uint16_t)src_nic_attr_uint( attr );
    if( type == CTRL_ATTR_FAMILY_NAME )
      is_ours = attr->nla_len == NLA_HDRLEN + sizeof( SRC_NIC_FAMILY_NAME ) &&
                !memcmp( (char const *)attr + NLA_HDRLEN, SRC_NIC_FAMILY_NAME,
                         sizeof( SRC_NIC_FAMILY_NAME ) );
  }
  if( is_ours ) *(uint16_t *)arg = id;
}

/* Look for the netdev family, once: its id, which requests to it are sent under. The
   kernel's families are listed and the name looked for among them: asked for by name, a
   family the kernel lacks makes it try to load a module for it. */
static void src_nic_family_find( void ) {
  struct timeval    timeout = { .tv_usec = SRC_NIC_TIMEOUT_US };
  src_nic_request_t request;
  if( src_nic_is_asked ) return;
  src_nic_is_asked = 1;
  src_nic_genl_fd  = socket( AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC );
  if( src_nic_genl_fd < 0 ) return;
  setsockopt( src_nic_genl_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof( timeout ) );
  src_nic_request( &request, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 1 );
  if( src_nic_ask( &request, src_nic_family_row, &src_nic_family ) ) src_nic_family = 0U;
}

/* One queue's row. arg is the td_out_t to emit its packets to; or NULL at a probe, when
   the row only says that its interface, if it is being learnt, has such statistics. */
static void src_nic_qstats_row( struct nlattr const * attr, int attr_bytes, void * arg ) {
  td_out_t *            out       = (td_out_t *)arg;
  src_nic_interface_t * interface = NULL;
  uint64_t              ifindex = 0UL, queue_type = UINT64_MAX, queue_id = 0UL, packet_cnt = 0UL;
  int                   has_rx = 0, has_tx = 0;
  char                  key[ TD_KEY_BYTES ];
  for( ; src_nic_attr_ok( attr, attr_bytes ); attr = src_nic_attr_next( attr, &attr_bytes ) ) {
    int      type  = attr->nla_type & NLA_TYPE_MASK;
    uint64_t value = src_nic_attr_uint( attr );
    if( type == SRC_NIC_A_IFINDEX )    ifindex    = value;
    if( type == SRC_NIC_A_QUEUE_TYPE ) queue_type = value;
    if( type == SRC_NIC_A_QUEUE_ID )   queue_id   = value;
    if( type == SRC_NIC_A_RX_PACKETS ) {
      has_rx = 1;
      if( queue_type != SRC_NIC_QUEUE_TX ) packet_cnt = value;
    }
    if( type == SRC_NIC_A_TX_PACKETS ) {
      has_tx = 1;
      if( queue_type != SRC_NIC_QUEUE_RX ) packet_cnt = value;
    }
  }
  if( queue_type == SRC_NIC_QUEUE_RX ? !has_rx : queue_type == SRC_NIC_QUEUE_TX ? !has_tx : 1 )
    return;  /* a row that gives no packets of its own kind says nothing here */
  for( int i = 0; i < SRC_NIC_INTERFACE_MAX; i++ )
    if( src_nic_interface[ i ].name[ 0 ] && src_nic_interface[ i ].ifindex == ifindex )
      interface = &src_nic_interface[ i ];
  if( !interface || !ifindex ) return;
  if( !out ) { interface->has_qstats |= interface->is_learning; return; }
  if( !interface->has_qstats ) return;
  snprintf( key, sizeof( key ), "%.*s/%u", IFNAMSIZ, interface->name, (unsigned)queue_id );
  td_emit( out, queue_type == SRC_NIC_QUEUE_RX ? SRC_NIC_RX_QUEUE : SRC_NIC_TX_QUEUE, key,
           (double)packet_cnt );
}

/* Ask for the packets of every queue of every device, in one dump; see the row above for
   what `out` is. Nothing is asked on a kernel without the family. */
static void src_nic_qstats( td_out_t * out ) {
  src_nic_request_t request;
  uint32_t          scope = SRC_NIC_SCOPE_QUEUE;
  if( !src_nic_family ) return;
  src_nic_request( &request, src_nic_family, SRC_NIC_QSTATS_GET, 1 );
  src_nic_request_attr( &request, SRC_NIC_A_SCOPE, &scope, sizeof( scope ) );
  src_nic_ask( &request, src_nic_qstats_row, out );
}

/* Look for interfaces not yet known, or whose driver changed its counters; forget those
   that are gone. An interface that is known and unchanged keeps what was learnt, first
   values included. */
static int src_nic_probe( td_ctx_t const * ctx, td_out_t * out ) {
  uint8_t         is_present[ SRC_NIC_INTERFACE_MAX ] = { 0 };
  char            why[ TD_WHY_BYTES ];
  DIR *           dir;
  struct dirent * entry;
  int             learnt_cnt = 0, learning_cnt = 0;
  (void)ctx;
  if( src_nic_fd < 0 ) src_nic_fd = socket( AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0 );
  if( src_nic_fd < 0 ) {
    td_unavailable( out, -1, "no socket to ask the drivers through" );
    return -ENOENT;
  }
  src_nic_family_find();
  dir = opendir( SRC_NIC_DIR );
  while( dir && ( entry = readdir( dir ) ) ) {
    char const *          name = entry->d_name;
    src_nic_interface_t * interface;
    uint32_t              counter_cnt;
    if( name[ 0 ] == '.' || !strcmp( name, TD_LOOPBACK_NAME ) ) continue;
    if( entry->d_type != DT_LNK ) continue;  /* a file such as bonding_masters: not a device */
    counter_cnt = src_nic_counter_cnt( name );
    if( counter_cnt > SRC_NIC_COUNTER_MAX ) {
      snprintf( why, sizeof( why ), "%.*s: its driver has %u counters, more than %d",
                IFNAMSIZ, name, counter_cnt, SRC_NIC_COUNTER_MAX );
      td_unavailable( out, SRC_NIC_PRESSURE, why );
    }
    interface = counter_cnt && counter_cnt <= SRC_NIC_COUNTER_MAX ? src_nic_slot( name ) : NULL;
    if( !interface && counter_cnt && counter_cnt <= SRC_NIC_COUNTER_MAX ) {
      snprintf( why, sizeof( why ), "%.*s: not shown, more than %d interfaces", IFNAMSIZ, name,
                SRC_NIC_INTERFACE_MAX );
      td_unavailable( out, SRC_NIC_PRESSURE, why );
    }
    if( !interface ) continue;
    /* A free slot keeps the counts of the interface it held: its name is what says free.
       Another device under a known name (another number) is learnt as a new one. */
    if( !interface->name[ 0 ] || interface->counter_cnt != counter_cnt ||
        interface->ifindex != src_nic_ifindex( name ) )
      src_nic_learn( interface, name, counter_cnt );
    is_present[ interface - src_nic_interface ] = 1;
  }
  if( dir ) closedir( dir );
  for( int i = 0; i < SRC_NIC_INTERFACE_MAX; i++ ) {
    if( !is_present[ i ] ) src_nic_interface[ i ].name[ 0 ] = '\0';
    learnt_cnt   += src_nic_interface[ i ].name[ 0 ] != '\0';
    learning_cnt += src_nic_interface[ i ].name[ 0 ] && src_nic_interface[ i ].is_learning;
  }
  /* Where the packets of an interface's queues come from is settled once, when it is
     learnt: a counter that changed hands later would show a rate that never was. */
  if( learning_cnt ) src_nic_qstats( NULL );
  for( int i = 0; i < SRC_NIC_INTERFACE_MAX; i++ ) src_nic_interface[ i ].is_learning = 0;
  if( learnt_cnt ) return 0;
  td_unavailable( out, -1, "no interface whose driver gives counters through ethtool" );
  return -ENOENT;
}

static void src_nic_read_one( src_nic_interface_t * interface, td_out_t * out ) {
  struct ethtool_ringparam ring = { .cmd = ETHTOOL_GRINGPARAM };
  char                     key[ TD_KEY_BYTES ];

  memset( &src_nic_block.stats, 0, sizeof( src_nic_block.stats ) );
  src_nic_block.stats.cmd     = ETHTOOL_GSTATS;
  src_nic_block.stats.n_stats = interface->counter_cnt;
  if( src_nic_ioctl( interface->name, &src_nic_block ) ) return;
  if( src_nic_block.stats.n_stats != interface->counter_cnt ) {  /* the driver was reloaded */
    interface->counter_cnt = 0U;  /* the next probe learns it again */
    return;
  }
  for( uint32_t i = 0U; i < interface->counter_cnt; i++ ) {
    uint64_t value = src_nic_block.stats.data[ i ];
    int      role  = interface->role[ i ], metric_idx;
    if( role == SRC_NIC_ROLE_IGNORED ) continue;
    if( role == SRC_NIC_ROLE_PRESSURE ) {
      if( interface->first[ i ] == UINT64_MAX || value < interface->first[ i ] )
        interface->first[ i ] = value;  /* the first read, or the driver started over */
      if( value == interface->first[ i ] ) continue;  /* it has never moved: not worth a row */
      snprintf( key, sizeof( key ), "%.*s %s", IFNAMSIZ, interface->name, interface->label[ i ] );
      td_emit( out, SRC_NIC_PRESSURE, key, (double)( value - interface->first[ i ] ) );
      continue;
    }
    if( interface->has_qstats ) continue;  /* its queues were emitted from the netdev family */
    metric_idx = role == SRC_NIC_ROLE_RX ? SRC_NIC_RX_QUEUE : SRC_NIC_TX_QUEUE;
    snprintf( key, sizeof( key ), "%.*s/%u", IFNAMSIZ, interface->name, interface->queue[ i ] );
    td_emit( out, metric_idx, key, (double)value );
  }
  if( src_nic_ioctl( interface->name, &ring ) ) return;
  snprintf( key, sizeof( key ), "%.*s rx", IFNAMSIZ, interface->name );
  td_emit( out, SRC_NIC_RING, key, (double)ring.rx_pending );
  snprintf( key, sizeof( key ), "%.*s tx", IFNAMSIZ, interface->name );
  td_emit( out, SRC_NIC_RING, key, (double)ring.tx_pending );
}

static void src_nic_read( td_ctx_t const * ctx, td_out_t * out ) {
  int has_qstats = 0;
  (void)ctx;
  for( int i = 0; i < SRC_NIC_INTERFACE_MAX; i++ )
    has_qstats |= src_nic_interface[ i ].name[ 0 ] && src_nic_interface[ i ].has_qstats;
  if( has_qstats ) src_nic_qstats( out );  /* one dump for every interface: no block is allocated */
  for( int i = 0; i < SRC_NIC_INTERFACE_MAX; i++ ) {
    src_nic_interface_t * interface = &src_nic_interface[ i ];
    if( interface->name[ 0 ] && interface->counter_cnt ) src_nic_read_one( interface, out );
  }
}

td_source_t const td_src_nic = {
  .name = "nic", .scope = TD_SCOPE_HOST, .period_ms = SRC_NIC_PERIOD_MS,
  .metric = src_nic_metric, .metric_cnt = SRC_NIC_METRIC_CNT,
  .probe = src_nic_probe, .read = src_nic_read,
};
