/* src_net.c: the kernel's network path, from counters every host has. Two sources. "net":
   per interface, packets, bytes, errors and drops. "softnet": per CPU, packets the network
   stack processed there, dropped or put off. Reads /proc/net/dev and softnet_stat. */
#include "td_source.h"

enum {
  SRC_NET_RX_PACKETS, SRC_NET_RX_BYTES, SRC_NET_RX_BAD, SRC_NET_TX_PACKETS, SRC_NET_TX_BYTES,
  SRC_NET_TX_BAD, SRC_NET_METRIC_CNT
};
enum { SRC_SOFTNET_PROCESSED, SRC_SOFTNET_DROPPED, SRC_SOFTNET_SQUEEZED, SRC_SOFTNET_METRIC_CNT };

/* One row per metric, in the order of the enums above. */
static td_metric_t const src_net_metric[ SRC_NET_METRIC_CNT ] = {
  { "rx_packets", "rx packets",   "/s", TD_TABLE, TD_RATE,  0, 0 },
  { "rx_bytes",   "rx bytes",     "/s", TD_TABLE, TD_RATE,  0, 0 },
  { "rx_bad",     "rx err, drop", "",   TD_TABLE, TD_TOTAL, 0, 1 },
  { "tx_packets", "tx packets",   "/s", TD_TABLE, TD_RATE,  0, 0 },
  { "tx_bytes",   "tx bytes",     "/s", TD_TABLE, TD_RATE,  0, 0 },
  { "tx_bad",     "tx err, drop", "",   TD_TABLE, TD_TOTAL, 0, 1 },
};
static td_metric_t const src_softnet_metric[ SRC_SOFTNET_METRIC_CNT ] = {
  { "processed", "packets processed", "/s", TD_TABLE, TD_RATE,  0, 0 },
  { "dropped",   "dropped",           "",   TD_TABLE, TD_SINCE, 0, 1 },
  { "squeezed",  "out of budget",     "",   TD_TABLE, TD_SINCE, 1, 0 },
};

#define SRC_NET_DEV_FILE     "/proc/net/dev"
#define SRC_NET_SOFTNET_FILE "/proc/net/softnet_stat"

/* After "name:": rx bytes, packets, errs, drop, fifo, 3 more; tx bytes, packets, errs, drop. */
#define SRC_NET_DEV_FORMAT "%lu %lu %lu %lu %lu %*u %*u %*u %lu %lu %lu %lu"

/* A softnet row, in hex: processed, dropped, out of budget, nine more, then the CPU. */
#define SRC_NET_SOFTNET_FIELDS (13)
#define SRC_NET_SOFTNET_CPU    (12)  /* the field that says the CPU */

/* The first value read of each softnet count, per CPU: what is shown is the growth since. */
static unsigned long src_softnet_first[ SRC_SOFTNET_METRIC_CNT ][ TD_CPU_MAX ];
static uint8_t       src_softnet_has_first[ SRC_SOFTNET_METRIC_CNT ][ TD_CPU_MAX ];

/* The two files, kept open: a read is a pread from the start, into td_scratch. */
static td_kept_t src_net_dev_kept, src_net_softnet_kept;

static long src_net_dev_text( void ) {
  return td_kept_read( &src_net_dev_kept, SRC_NET_DEV_FILE, td_scratch, TD_SCRATCH_BYTES, 0 );
}

static long src_net_softnet_text( void ) {
  return td_kept_read( &src_net_softnet_kept, SRC_NET_SOFTNET_FILE, td_scratch,
                       TD_SCRATCH_BYTES, 0 );
}

static void src_net_read( td_ctx_t const * ctx, td_out_t * out ) {
  char * line = td_scratch;
  (void)ctx;
  if( src_net_dev_text() <= 0L ) return;
  while( ( line = strchr( line, '\n' ) ) && *++line ) {
    unsigned long rx_bytes, rx_packets, rx_errs, rx_drop, rx_fifo;
    unsigned long tx_bytes, tx_packets, tx_errs, tx_drop;
    char          name[ TD_KEY_BYTES ];
    char *        colon = strchr( line, ':' );
    char *        end   = strchr( line, '\n' );
    if( !colon || ( end && colon > end ) ) continue;  /* one of the two header lines */
    while( *line == ' ' ) line++;
    snprintf( name, sizeof( name ), "%.*s", (int)( colon - line ), line );
    if( !strcmp( name, TD_LOOPBACK_NAME ) ) continue;
    if( sscanf( colon + 1, SRC_NET_DEV_FORMAT, &rx_bytes, &rx_packets, &rx_errs, &rx_drop,
                &rx_fifo, &tx_bytes, &tx_packets, &tx_errs, &tx_drop ) != 9 ) continue;
    td_emit( out, SRC_NET_RX_PACKETS, name, (double)rx_packets );
    td_emit( out, SRC_NET_RX_BYTES,   name, (double)rx_bytes );
    td_emit( out, SRC_NET_RX_BAD,     name, (double)( rx_errs + rx_drop + rx_fifo ) );
    td_emit( out, SRC_NET_TX_PACKETS, name, (double)tx_packets );
    td_emit( out, SRC_NET_TX_BYTES,   name, (double)tx_bytes );
    td_emit( out, SRC_NET_TX_BAD,     name, (double)( tx_errs + tx_drop ) );
  }
}

/* What a count has grown by since this source first read it, emitted only once it has:
   hosts have hundreds of CPUs and most never drop. The first value is kept here, zero
   included, so that the first drops on a clean CPU show. */
static void src_softnet_since( td_out_t * out, int metric_idx, char const * key, unsigned long cpu,
                               unsigned long count ) {
  unsigned long * first;
  uint8_t *       has_first;
  if( cpu >= TD_CPU_MAX ) return;
  first     = &src_softnet_first    [ metric_idx ][ cpu ];
  has_first = &src_softnet_has_first[ metric_idx ][ cpu ];
  if( !*has_first || count < *first ) *first = count;  /* the first read, or it restarted */
  *has_first = 1;
  if( count > *first ) td_emit( out, metric_idx, key, (double)( count - *first ) );
}

/* One row per online CPU; its 13th column says which (kernels since 5.10, the oldest this
   tracer runs on). Only CPUs the stack has run on get an instance, and drops only where
   there are some: hosts have hundreds of CPUs. */
static void src_softnet_read( td_ctx_t const * ctx, td_out_t * out ) {
  char * line = td_scratch;
  (void)ctx;
  if( src_net_softnet_text() <= 0L ) return;
  while( line && *line ) {
    unsigned long field[ SRC_NET_SOFTNET_FIELDS ] = { 0UL };
    char          key[ TD_KEY_BYTES ];
    char *        line_end  = strchr( line, '\n' );
    char *        cursor    = line;
    int           field_cnt = 0;
    if( line_end ) *line_end = '\0';  /* a short row must not borrow fields from the next */
    for( ; field_cnt < SRC_NET_SOFTNET_FIELDS; field_cnt++ ) {  /* every field is in hex */
      char * field_end;
      field[ field_cnt ] = strtoul( cursor, &field_end, 16 );
      if( field_end == cursor ) break;
      cursor = field_end;
    }
    if( field_cnt == SRC_NET_SOFTNET_FIELDS && field[ 0 ] ) {  /* the stack has run there */
      unsigned long cpu = field[ SRC_NET_SOFTNET_CPU ];
      snprintf( key, sizeof( key ), "cpu%lu", cpu );
      td_emit( out, SRC_SOFTNET_PROCESSED, key, (double)field[ 0 ] );
      src_softnet_since( out, SRC_SOFTNET_DROPPED,  key, cpu, field[ 1 ] );
      src_softnet_since( out, SRC_SOFTNET_SQUEEZED, key, cpu, field[ 2 ] );
    }
    line = line_end ? line_end + 1 : NULL;
  }
}

static int src_net_probe( td_ctx_t const * ctx, td_out_t * out ) {
  (void)ctx;
  if( src_net_dev_text() > 0L ) return 0;
  td_unavailable( out, -1, "cannot read " SRC_NET_DEV_FILE );
  return -ENOENT;
}

static int src_softnet_probe( td_ctx_t const * ctx, td_out_t * out ) {
  (void)ctx;
  if( src_net_softnet_text() > 0L ) return 0;
  td_unavailable( out, -1, "cannot read " SRC_NET_SOFTNET_FILE );
  return -ENOENT;
}

td_source_t const td_src_net = {
  .name = "net", .scope = TD_SCOPE_HOST, .period_ms = 100,
  .metric = src_net_metric, .metric_cnt = SRC_NET_METRIC_CNT,
  .probe = src_net_probe, .read = src_net_read,
};

td_source_t const td_src_softnet = {
  .name = "softnet", .scope = TD_SCOPE_HOST, .period_ms = 100,
  .metric = src_softnet_metric, .metric_cnt = SRC_SOFTNET_METRIC_CNT,
  .probe = src_softnet_probe, .read = src_softnet_read,
};
