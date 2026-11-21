/* src_irq.c: interrupts and softirqs on the cores traced threads run on. The total as a
   lane, and one table row per kind that has ever fired there, so the culprit has a name.
   Reads /proc/interrupts and /proc/softirqs once per period and walks each once, whatever
   the number of cores. */
#include "td_source.h"

enum { SRC_IRQ_COUNT, SRC_IRQ_KIND, SRC_IRQ_SOFT, SRC_IRQ_METRIC_CNT };

/* One row per metric, in the order of the enum above. */
static td_metric_t const src_irq_metric[ SRC_IRQ_METRIC_CNT ] = {
  { "count", "interrupts on core", "/s", TD_LANE,  TD_RATE, 5, 100 },
  { "kind",  "interrupt",          "/s", TD_TABLE, TD_RATE, 5, 100 },
  { "soft",  "softirq",            "/s", TD_TABLE, TD_RATE, 5, 100 },
};

#define SRC_IRQ_SOFT_FILE    "/proc/softirqs"
#define SRC_IRQ_SOFT_BYTES   (1UL << 20)  /* ten rows of 1024 CPUs */
#define SRC_IRQ_ROW_MAX      (1024)       /* rows kept of /proc/interrupts: all the store holds */
#define SRC_IRQ_SOFT_ROW_MAX (32)
#define SRC_IRQ_CPU_NAME     "CPU"        /* what a column's name begins with */

/* A row of a per-CPU table, as kept from one walk: its key, and its count on each of the
   CPUs asked for. A device interrupt is keyed by its number and device ("70
   eth0-Tx-Rx-0"): devices repeat, numbers do not. */
typedef struct {
  char     key[ TD_KEY_BYTES ];
  uint8_t  is_numbered;  /* a device interrupt, "70", not a named row such as "LOC" */
  uint64_t count[ TD_THREAD_MAX ];
} src_irq_row_t;

typedef struct {
  src_irq_row_t * row;
  int             row_max, row_cnt;
  uint8_t         has_column[ TD_THREAD_MAX ];  /* the table has a column for that CPU */
  double          total     [ TD_THREAD_MAX ];  /* of the rows that are interrupts of their own */
} src_irq_table_t;

/* The two tables as last read, and when: every core of one tick is served from one read
   and one walk. Nothing is sent to a core; printing a device interrupt's row holds that
   interrupt's lock for a moment (docs/reading-the-page.md, "What can reach a traced core"). */
static char      src_irq_text[ TD_SCRATCH_BYTES ];
static char      src_irq_soft_text[ SRC_IRQ_SOFT_BYTES ];
static long      src_irq_bytes, src_irq_soft_bytes;
static uint64_t  src_irq_read_ns = UINT64_MAX;  /* no tick yet */
static td_kept_t src_irq_kept, src_irq_soft_kept;

/* The CPUs the last walk kept counts for: those traced threads run on. */
static int src_irq_cpu[ TD_THREAD_MAX ];
static int src_irq_cpu_cnt;

static src_irq_row_t   src_irq_row     [ SRC_IRQ_ROW_MAX ];
static src_irq_row_t   src_irq_soft_row[ SRC_IRQ_SOFT_ROW_MAX ];
static src_irq_table_t src_irq_hard = { .row = src_irq_row,      .row_max = SRC_IRQ_ROW_MAX };
static src_irq_table_t src_irq_soft = { .row = src_irq_soft_row, .row_max = SRC_IRQ_SOFT_ROW_MAX };

/* Rows of x86 that are not interrupts of their own, left out of the total. A TLB shootdown
   arrives as a function call interrupt: it is in CAL and again in TLB. A performance
   monitoring interrupt is an NMI: in NMI and again in PMI. MCP counts polls for machine
   checks, which run from a timer; RTR counts retries of a read of the interrupt controller. */
static char const * const src_irq_subset_row[] = { "TLB", "PMI", "MCP", "RTR" };
#define SRC_IRQ_SUBSET_CNT TD_COUNT_OF( src_irq_subset_row )

static int src_irq_is_subset( char const * label, size_t label_bytes ) {
  for( int i = 0; i < SRC_IRQ_SUBSET_CNT; i++ )
    if( strlen( src_irq_subset_row[ i ] ) == label_bytes &&
        !memcmp( label, src_irq_subset_row[ i ], label_bytes ) ) return 1;
  return 0;
}

/* Which of the CPUs asked for is `cpu`: its index in src_irq_cpu, or -1. */
static int src_irq_cpu_idx( int cpu ) {
  for( int i = 0; i < src_irq_cpu_cnt; i++ ) if( src_irq_cpu[ i ] == cpu ) return i;
  return -1;
}

/* Name a row: its label, and for a device interrupt the last word of its description with
   the blank before it. `tail` is what follows the row's numbers, up to `end`. */
static void src_irq_name( src_irq_row_t * row, char const * label, size_t label_bytes,
                          char const * tail, char const * end ) {
  char const * device       = end;
  size_t       device_bytes = 0UL;
  if( row->is_numbered && end > tail && end[ -1 ] != ' ' ) {  /* a description, and a last word */
    while( device > tail && device[ -1 ] != ' ' ) device--;
    if( device > tail ) device_bytes = (size_t)( end - --device );  /* a blank precedes it */
  }
  if( label_bytes > TD_KEY_BYTES - 1UL ) label_bytes = TD_KEY_BYTES - 1UL;  /* a long key is cut */
  if( device_bytes > TD_KEY_BYTES - 1UL - label_bytes )
    device_bytes = TD_KEY_BYTES - 1UL - label_bytes;
  memcpy( row->key, label, label_bytes );
  memcpy( row->key + label_bytes, device, device_bytes );
  row->key[ label_bytes + device_bytes ] = '\0';
}

/* Walk a per-CPU table once: a header of CPU names, which says which column a CPU is
   (offline CPUs have none), then "label: n n n ... description" rows. Keeps, for the CPUs
   asked for, every named row and every device interrupt that has fired on one of them:
   one that never did is left out, there can be hundreds. A row with fewer numbers than
   the header has columns, such as ERR, is not per CPU and is passed over. The text is
   left as it was. */
static void src_irq_walk( char const * text, src_irq_table_t * table ) {
  int          column_of[ TD_THREAD_MAX ], column_cnt = 0, is_full = 0;
  char const * header_end = strchr( text, '\n' );
  char const * line       = header_end ? header_end + 1 : "";

  table->row_cnt = 0;
  for( int i = 0; i < TD_THREAD_MAX; i++ ) {
    column_of[ i ]         = -1;
    table->has_column[ i ] = 0;
    table->total[ i ]      = 0.0;
  }
  for( char const * name = text;
       header_end && name + sizeof( SRC_IRQ_CPU_NAME ) - 1UL <= header_end; ) {
    if( memcmp( name, SRC_IRQ_CPU_NAME, sizeof( SRC_IRQ_CPU_NAME ) - 1UL ) ) { name++; continue; }
    name += sizeof( SRC_IRQ_CPU_NAME ) - 1UL;
    if( *name >= '0' && *name <= '9' ) {
      int cpu_idx = src_irq_cpu_idx( atoi( name ) );
      if( cpu_idx >= 0 ) { column_of[ cpu_idx ] = column_cnt; table->has_column[ cpu_idx ] = 1; }
    }
    column_cnt++;
  }

  while( *line ) {
    char const *    end    = strchr( line, '\n' );
    char const *    colon, * cursor;
    src_irq_row_t * row    = &table->row[ table->row_cnt < table->row_max ? table->row_cnt : 0 ];
    uint64_t        count[ TD_THREAD_MAX ] = { 0UL };
    int             column = 0, is_numbered, is_subset, has_fired = 0;
    if( !end ) end = line + strlen( line );
    colon = memchr( line, ':', (size_t)( end - line ) );  /* the first: it must be this row's */
    for( cursor = colon ? colon + 1 : end; colon && column < column_cnt; column++ ) {
      char const * digit;
      uint64_t     value = 0UL;
      while( *cursor == ' ' ) cursor++;
      for( digit = cursor; *digit >= '0' && *digit <= '9'; digit++ )
        value = value * 10UL + (uint64_t)( *digit - '0' );
      if( digit == cursor ) break;
      for( int i = 0; i < src_irq_cpu_cnt; i++ ) if( column_of[ i ] == column ) count[ i ] = value;
      cursor = digit;
    }
    if( colon && column == column_cnt ) {  /* a per-CPU row */
      while( *line == ' ' ) line++;
      is_numbered = line[ 0 ] >= '0' && line[ 0 ] <= '9';
      is_subset   = src_irq_is_subset( line, (size_t)( colon - line ) );
      for( int i = 0; i < src_irq_cpu_cnt; i++ ) {
        if( !is_subset ) table->total[ i ] += (double)count[ i ];
        has_fired |= count[ i ] != 0UL;
      }
      if( ( has_fired || !is_numbered ) && table->row_cnt < table->row_max ) {
        row->is_numbered = (uint8_t)is_numbered;
        memcpy( row->count, count, sizeof( row->count ) );
        src_irq_name( row, line, (size_t)( colon - line ), cursor, end );
        table->row_cnt++;
      } else if( has_fired || !is_numbered ) is_full = 1;
    }
    line = *end ? end + 1 : end;
  }
  if( is_full )
    TD_LOG_ONCE( "tracerd: more than %d interrupts have fired on traced cores: the rest are "
                 "in the total, and have no row\n", table->row_max );
}

/* Walk both tables for the CPUs traced threads run on now. */
static void src_irq_walk_all( void ) {
  src_irq_cpu_cnt = 0;
  for( int i = 0; i < TD_THREAD_MAX; i++ ) {
    td_thread_t const * thread = &td_thread[ i ];
    if( !thread->is_live || thread->is_gone || thread->cpu < 0 ) continue;
    if( src_irq_cpu_idx( thread->cpu ) < 0 ) src_irq_cpu[ src_irq_cpu_cnt++ ] = thread->cpu;
  }
  src_irq_walk( src_irq_bytes      > 0L ? src_irq_text      : "", &src_irq_hard );
  src_irq_walk( src_irq_soft_bytes > 0L ? src_irq_soft_text : "", &src_irq_soft );
}

/* Read and walk both tables, unless this tick already has: a tick's probes and reads share
   them. Returns which of the CPUs walked for `cpu` is, or -1 when it is none of them. */
static int src_irq_refresh( uint64_t now_ns, int cpu ) {
  if( now_ns != src_irq_read_ns ) {
    src_irq_bytes      = td_kept_read( &src_irq_kept, TD_INTERRUPTS_FILE, src_irq_text,
                                       sizeof( src_irq_text ), 0 );
    src_irq_soft_bytes = td_kept_read( &src_irq_soft_kept, SRC_IRQ_SOFT_FILE, src_irq_soft_text,
                                       sizeof( src_irq_soft_text ), 0 );
    src_irq_read_ns    = now_ns;
    src_irq_walk_all();
  }
  /* A CPU that no thread ran on when the tables were walked: walk them again for it. */
  if( cpu >= 0 && src_irq_cpu_idx( cpu ) < 0 ) src_irq_walk_all();
  return cpu >= 0 ? src_irq_cpu_idx( cpu ) : -1;
}

/* Emit a CPU's count of every row kept of a table. A device interrupt that never fired on
   this CPU is left out; named rows are emitted at zero too, so that the first one to move
   has a rate. Returns the sum, or -1 when the table has no column for the CPU. */
static double src_irq_emit( src_irq_table_t const * table, int cpu_idx, td_out_t * out,
                            int metric_idx ) {
  if( cpu_idx < 0 || !table->has_column[ cpu_idx ] ) return -1.0;
  for( int i = 0; i < table->row_cnt; i++ ) {
    src_irq_row_t const * row   = &table->row[ i ];
    uint64_t              count = row->count[ cpu_idx ];
    if( !count && row->is_numbered ) continue;
    td_emit( out, metric_idx, row->key, (double)count );
  }
  return table->total[ cpu_idx ];
}

static int src_irq_probe( td_ctx_t const * ctx, td_out_t * out ) {
  int cpu_idx = src_irq_refresh( ctx->now_ns, ctx->cpu );
  if( src_irq_bytes <= 0L ) {
    td_unavailable( out, -1, src_irq_bytes == -EFBIG
                             ? TD_INTERRUPTS_FILE " is larger than tracerd reads"
                             : "cannot read " TD_INTERRUPTS_FILE );
    return -ENOENT;
  }
  if( cpu_idx >= 0 && src_irq_hard.has_column[ cpu_idx ] ) return 0;
  td_unavailable( out, -1, TD_INTERRUPTS_FILE " has no column for the thread's CPU" );
  return -ENOENT;
}

static void src_irq_read( td_ctx_t const * ctx, td_out_t * out ) {
  int    cpu_idx = src_irq_refresh( ctx->now_ns, ctx->cpu );
  double total;
  if( src_irq_bytes > 0L ) {
    total = src_irq_emit( &src_irq_hard, cpu_idx, out, SRC_IRQ_KIND );
    if( total >= 0.0 ) td_emit( out, SRC_IRQ_COUNT, "", total );
  }
  if( src_irq_soft_bytes > 0L ) src_irq_emit( &src_irq_soft, cpu_idx, out, SRC_IRQ_SOFT );
}

td_source_t const td_src_irq = {
  .name = "irq", .scope = TD_SCOPE_CORE, .period_ms = 100,
  .metric = src_irq_metric, .metric_cnt = SRC_IRQ_METRIC_CNT,
  .probe = src_irq_probe, .read = src_irq_read,
};
