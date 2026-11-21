/* td_ring.h: the traced threads as the daemon holds them. One td_thread_t per ring found:
   the mapping, the state that pairs events into spans and traces, and the latency history
   the page draws. td_ring_find.c finds the rings and follows their threads; td_ring.c drains. */
#ifndef TD_RING_H
#define TD_RING_H

#include <signal.h>

#include "../src/trace.h"
#include "td_util.h"

#define TD_TICK_MS             (10)    /* the daemon's loop: one drain per tick */
#define TD_SLOT_TICKS          (5)     /* a slot is 50 ms: the resolution of the charts */
#define TD_SECOND_TICKS        (100)
#define TD_FAST_CNT            (1200)  /* slots kept: one minute */
#define TD_SLOW_CNT            (600)   /* seconds kept: ten minutes */
#define TD_THREAD_MAX          (16)
#define TD_SITE_MAX            (32)    /* sites followed per thread; more are ignored */
#define TD_SPAN_MAX            (32)    /* spans kept of one trace */
#define TD_DRAIN_BATCH_EVENTS  (4096)  /* events copied at a time */
#define TD_DRAIN_MARGIN_DIV    (4UL)   /* the drain stays this fraction of a ring off the writer */
#define TD_DRAIN_SLACK_EVENTS  (8UL)   /* events a writer may have begun beyond its cursor: a
                                          margin that holds in practice, not a bound of aarch64 */
/* A trace ends at the next root, or after this long with no begin, end or mark and no span
   open. Spans that come later, before the next root, belong to no trace. */
#define TD_IDLE_CLOSE_NS       (20UL * TD_NS_PER_MS)
#define TD_GONE_KEEP_NS        (600UL * TD_NS_PER_SECOND)  /* a gone thread is forgotten then */

/* Latencies as the page gets them. 32 bits each: one over 4.29 s is shown as that. */
typedef struct {
  uint32_t p50_ns;
  uint32_t p99_ns;
  uint32_t max_ns;
  uint32_t cnt;
} td_stat_t;

typedef struct {
  uint16_t site;
  uint16_t kind;      /* TRACE_KIND_END for a span, TRACE_KIND_MARK for an instant */
  uint32_t start_ns;  /* from the root */
  uint32_t length_ns;
} td_span_t;

typedef struct {
  uint64_t  wall_ms;    /* when the trace began */
  uint64_t  length_ns;  /* root to last end */
  uint64_t  value;      /* the root's value */
  uint32_t  span_cnt;
  td_span_t span[ TD_SPAN_MAX ];
} td_trace_t;

typedef struct {
  uint64_t last_value;      /* last count seen, 48 bits */
  uint64_t last_trace_cnt;  /* traces completed when it was seen */
  uint64_t last_tsc;
  uint64_t delta_sum;       /* counts since src_pmu last took them */
  uint64_t trace_sum;       /* traces completed over the same samples */
  uint64_t tsc_sum;         /* time covered by the same samples */
  int      has_last;
  int      has_counted;     /* it has moved at least once: worth showing */
  int      has_waited;      /* its sums span a wait for a trace to complete */
} td_counter_t;

typedef struct {
  int                   is_live;         /* 0: the slot is free */
  int                   is_new;          /* not drained yet: its backlog is history, not lag */
  int                   is_gone;         /* the thread exited; its history stays for a while */
  uint64_t              gone_ns;
  char                  path[ TD_PATH_BYTES ];  /* "" once the file is no longer this ring's */
  uint64_t              inode;           /* of the ring file: a new file is a new ring */
  uint64_t              device;          /* the filesystem the inode is of */
  uint32_t              pid;             /* as the header said when the ring was found */
  uint32_t              tid;
  uint64_t              start_ticks;     /* when the thread started; 0: the header does not say */
  int                   is_locked;       /* the program could lock its ring in memory */
  char                  name[ TRACE_NAME_BYTES ];  /* the thread's and its sites' names and
                                                      flags, copied once: the ring can change */
  trace_site_name_t     site[ TD_SITE_MAX ];
  volatile sig_atomic_t is_cut;          /* the file was cut short: the mapping is now zeroes */
  trace_ring_t const *  ring;
  trace_event_t const * event;
  uint64_t              ring_bytes;
  uint64_t              mask;
  uint64_t              seen_cnt;        /* events consumed so far */
  uint64_t              lost_cnt;        /* events never read: overwritten first, or intact
                                            but within the quarter ring left to the writer */
  uint64_t              backlog_max_cnt; /* largest backlog since src_ring last read it */
  int                   site_cnt;        /* min( ring->site_cnt, TD_SITE_MAX ) */
  int                   cpu;             /* where the thread last ran, from /proc; -1: not known */
  uint64_t              move_cnt;        /* times it was found on another CPU than before */
  uint64_t              found_ns;        /* the daemon's clock when it found the ring */

  /* pairing */
  uint64_t              begin_tsc[ TD_SITE_MAX ];  /* 0: no span of the site is open */
  uint64_t              root_tsc;
  uint64_t              last_end_tsc;
  uint64_t              last_trace_tsc;  /* of the last begin, end or mark */
  uint64_t              last_event_ns;   /* the daemon's clock when it read one of those */
  int                   in_trace;
  td_trace_t            open_trace;
  uint64_t              trace_cnt;       /* traces completed, ever */

  /* latency: one histogram per site; index TD_SITE_MAX is the whole trace. A sample goes
     to slot_hist; second_hist is the sum of the slots closed since the last second was */
  td_hist_t             slot_hist  [ TD_SITE_MAX + 1 ];
  td_hist_t             second_hist[ TD_SITE_MAX + 1 ];
  td_stat_t             fast[ TD_FAST_CNT ][ TD_SITE_MAX + 1 ];
  td_stat_t             slow[ TD_SLOW_CNT ][ TD_SITE_MAX + 1 ];
  td_trace_t            slowest[ TD_SLOW_CNT ];  /* the slowest trace of each second */
  td_trace_t            slowest_open;            /* of the second being filled */

  /* gauges and counters: read by the sources of src_ring.c and src_pmu.c */
  uint64_t              gauge_last[ TD_SITE_MAX ];
  uint64_t              gauge_max [ TD_SITE_MAX ];  /* since the gauge source last read it */
  uint8_t               is_gauge  [ TD_SITE_MAX ];  /* the site has carried a gauge event */
  td_counter_t          counter[ TRACE_COUNTER_MAX ];
} td_thread_t;

extern td_thread_t td_thread[ TD_THREAD_MAX ];

/* Rings found since the caller last zeroed it that got no slot: more threads than followed. */
extern int td_ring_unfollowed_cnt;

/* Set the conversion from timestamp counts to nanoseconds. Call once, before any drain. */
void td_ring_init( double ns_per_tsc );

/* From now on remove no file: for --probe, which only looks. */
void td_ring_keep_files( void );

/* Timestamp counts as milliseconds; and the length of one count in nanoseconds. */
double td_ring_tsc_to_ms( uint64_t tsc_cnt );
double td_ring_tsc_ns( void );

/* Look for new rings in `dir`. Then look at the known threads: which have exited, and where
   each runs. Each returns 1 when something the page shows changed, else 0. */
int td_ring_discover( char const * dir );
int td_ring_check_all( void );

/* The thread is gone: consume what its ring still holds and close its trace. is_dead: the
   writer no longer writes, so nothing needs to be left alone for it. */
void td_ring_finish( td_thread_t * thread, int is_dead );

/* A slot has become a thread's: said by whoever fills one, once it is live. From the first
   call on, the drain and the closes look only at the slots they were told of, not at all
   TD_THREAD_MAX every time. Never called, every slot is looked at. */
void td_ring_follow( td_thread_t const * thread );

/* Consume the new events of every ring. now_ns is td_clock_ns(). */
void td_ring_drain( uint64_t now_ns );

/* Close the 50 ms slot `slot`: turn the working histograms into fast[ slot % TD_FAST_CNT ],
   and add them to the second's. */
void td_ring_close_slot( uint64_t slot );

/* Close the second `second`: fills slow[] and slowest[] at second % TD_SLOW_CNT. Call it
   after the slots of that second are closed: what a slot still holds is not in it. */
void td_ring_close_second( uint64_t second );

#endif /* TD_RING_H */
