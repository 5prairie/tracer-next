/* td_source.h: the one interface every data source plugs into, and the store that keeps
   what sources emit. A source is a table of metrics, a probe and a read; it knows nothing
   of HTTP, JSON or the page. To add one: write src_<name>.c, declare and list it in tracerd.c. */
#ifndef TD_SOURCE_H
#define TD_SOURCE_H

#include "td_ring.h"

#define TD_SCOPE_HOST     (0)  /* read once */
#define TD_SCOPE_PROCESS  (1)  /* read once per traced process */
#define TD_SCOPE_THREAD   (2)  /* read once per traced thread */
#define TD_SCOPE_CORE     (3)  /* read once per CPU a traced thread runs on */

#define TD_LANE           (0)  /* a line over time, on the shared axis */
#define TD_TABLE          (1)  /* a row of current values */
#define TD_CHECK          (2)  /* a pass/fail cell: passes while the value is below `red` */

#define TD_LEVEL          (0)  /* emitted value is shown as is */
#define TD_RATE           (1)  /* emitted value is a running count: shown per second */
#define TD_TOTAL          (2)  /* emitted value is a running count: shown since first seen */
#define TD_SINCE          (3)  /* emitted value is already a count since the source began, and
                                  is emitted only once it has moved: shown as is */

#define TD_SOURCE_MAX      (16)
#define TD_METRIC_MAX      (16)    /* per source */
#define TD_INSTANCE_MAX    (1024)
#define TD_KEY_BYTES       (48)    /* a longer key is cut */
#define TD_WHY_BYTES       (128)
#define TD_UNAVAILABLE_MAX (256)
#define TD_SCRATCH_BYTES   (16UL << 20)  /* /proc/interrupts of 1024 CPUs and 1000 interrupts */

typedef struct {
  char const * id;     /* short, stable: "minor" */
  char const * label;  /* as the page shows it: "minor faults" */
  char const * unit;   /* "/s", "bytes", "" */
  int          widget;
  int          kind;
  double       amber;  /* amber at or above; 0 for none */
  double       red;    /* red at or above; 0 for none */
} td_metric_t;

/* What a probe or a read is about. Fields beyond the source's scope are -1 or 0. */
typedef struct {
  int      thread_idx;  /* index into td_thread, or -1 for a host-scope source */
  uint32_t pid;
  uint32_t tid;
  int      cpu;
  uint64_t now_ns;
  int      period_ms;   /* how often the source is read just now: its own period, or slower */
} td_ctx_t;

typedef struct td_out td_out_t;

/* A value of one metric. key names the instance within the scope ("eth0/rx-3", "udp:5000");
   "" when the metric has a single instance. NAN says the instance is there and has no
   value just now: it stays on the page, and its lane shows a gap. */
void td_emit( td_out_t * out, int metric_idx, char const * key, double value );

/* This source (metric_idx -1) or one of its metrics cannot be read here, and why. A probe
   that succeeds may use it as well, to say something worth knowing about what it reads. */
void td_unavailable( td_out_t * out, int metric_idx, char const * why );

/* How fine the values emitted from now on are, when coarser than the source's period (a
   finer one is taken as the period). */
void td_resolution( td_out_t * out, int resolution_ms );

/* The running counts emitted from now on are those of this CPU. When a thread has changed
   core they do not continue the last ones: no rate for one period, and a total goes on from
   where it was. A core-scope source need not call it: all it emits is its CPU's. */
void td_of_cpu( td_out_t * out, int cpu );

typedef struct {
  char const *        name;
  int                 scope;
  td_metric_t const * metric;
  int                 metric_cnt;
  int                 period_ms;
  int                 is_unproven;  /* never run on a host that has what it reads: off
                                       unless asked for with --with-<name>, and said so */
  int  ( *probe )( td_ctx_t const * ctx, td_out_t * out );  /* 0 when usable; else it said why.
                                                               NULL: always usable */
  void ( *read  )( td_ctx_t const * ctx, td_out_t * out );
} td_source_t;

/* ---- the store ------------------------------------------------------------------------- */

typedef struct {
  int      is_live;
  int      source_idx;
  int      metric_idx;
  int      thread_idx;      /* -1: belongs to the host */
  char     key[ TD_KEY_BYTES ];
  double   raw_last;        /* last emitted value, for rates and totals */
  double   raw_first;
  uint64_t raw_ns;
  int      has_raw;
  int      cpu;             /* whose count raw_last was; -1: not a CPU's */
  float    value;           /* as shown: level, rate per second, or total */
  float    second_max;      /* largest value of the second being filled */
  uint64_t seen_ns;         /* when it was last emitted */
  int      resolution_ms;   /* how fine its values are: the larger of the next and the period */
  int      own_ms;          /* the resolution its source stated for it; 0: none, the period */
  int      next_idx;        /* the instance its source emitted after this one, a read ago */
  float    fast[ TD_FAST_CNT ];
  float    slow[ TD_SLOW_CNT ];
} td_instance_t;

typedef struct {
  int  source_idx;
  int  metric_idx;  /* -1: the whole source */
  int  thread_idx;
  char why[ TD_WHY_BYTES ];
} td_unavailable_t;

typedef struct {
  td_source_t const * source;
  int                 is_on;          /* selected on the command line */
  int                 period_ticks;   /* doubles when a read overruns its budget */
  int                 pace_read_cnt;  /* reads since the period was last reconsidered */
  uint64_t            pace_sum_ns;    /* what they cost together */
  uint64_t            pace_worst_ns;  /* the costliest of them */
  uint64_t            due_tick;       /* when it reads next; 0: it has not read yet */
  uint32_t            cost_us;        /* the costliest read of late, all scopes */
  double              amber[ TD_METRIC_MAX ];  /* thresholds: the table's, or --set */
  double              red [ TD_METRIC_MAX ];
} td_source_state_t;

/* Where a source reads a /proc file into. One thread, one read at a time: one buffer. */
extern char td_scratch[ TD_SCRATCH_BYTES ];

extern td_source_state_t td_source_state[ TD_SOURCE_MAX ];
extern int               td_source_cnt;
extern td_instance_t     td_instance[ TD_INSTANCE_MAX ];
extern int               td_instance_end;  /* one past the highest slot in use */
extern td_unavailable_t  td_unavailable_list[ TD_UNAVAILABLE_MAX ];
extern int               td_unavailable_cnt;

/* Register the sources, in page order. */
void td_store_init( td_source_t const * const * source, int source_cnt );

/* Turn the sources named in `list` ("faults,irq") on (is_on 1) or off (0); or, with is_on
   -1, turn them on and every other one off. Returns 0, or -ENOENT for an unknown name. */
int td_store_select( char const * list, int is_on );

/* Override one threshold: "irq.count.red=5" or ".amber=". Returns 0 or -EINVAL. */
int td_store_set( char const * assignment );

/* Probe every source again for the current set of threads. Call when that set changed, and
   now and then. Returns 1 when what is unavailable changed, else 0. */
int td_store_probe( uint64_t now_ns );

/* Run the reads that are due at or before this tick. Returns 1 when the page must be told:
   an instance appeared or left, or a period or a resolution changed. */
int td_store_tick( uint64_t tick, uint64_t now_ns );

/* Close a slot or a second: fills fast[] and slow[] of every instance. is_known 0: the
   slot ended long before it is closed (the daemon was stopped), and no value is its own. */
void td_store_close_slot( uint64_t slot, int is_known );
void td_store_close_second( uint64_t second, int is_known );

/* ---- a thread's own /proc files ----------------------------------------------------------
   td_ring_find.c keeps them open per thread slot, for itself and for the sources, and
   closes them when the thread is gone: a source holds no fd of a thread. */

enum { TD_TASK_LEAF_STAT, TD_TASK_LEAF_STATUS, TD_TASK_LEAF_SCHEDSTAT, TD_TASK_LEAF_CNT };

/* Read one of them whole, NUL terminated, for the thread of slot thread_idx. Returns the
   byte count or a negative errno: -ESRCH or -ENOENT once the thread has exited. */
long td_ring_task_read( int thread_idx, int leaf_idx, char * text, size_t text_bytes );

/* The thread's stat file, parsed. It is read at most once per tick, whoever asks: a read
   made at or after since_ns (the tick's time) is given again. Returns 0 or a negative
   errno, as above; -EINVAL for a text that is no stat file's. */
int td_ring_task_stat( int thread_idx, uint64_t since_ns, td_task_stat_t * stat );

#endif /* TD_SOURCE_H */
