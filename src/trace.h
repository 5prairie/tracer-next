/* trace.h: the tracer as the traced program sees it, and the ring it shares with tracerd.
   A mark is one timestamp and one 16-byte store into the calling thread's ring: no branch,
   no call, no syscall. Valid C and C++. Link trace_attach.c into the program itself. */
#ifndef TRACE_H
#define TRACE_H

#include <stddef.h>
#include <stdint.h>

#include "trace_arch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the ring, as laid out in shared memory ------------------------------------------ */

#define TRACE_RING_MAGIC      (0x3530524543415254UL)  /* "TRACER05" */
#define TRACE_RING_PREFIX     "tracer."               /* the file is <dir>/tracer.<pid>.<tid> */
#define TRACE_RING_TEMP       ".tmp"                  /* its name while it is being created */
#define TRACE_RING_DIR        "/dev/shm"
#define TRACE_RING_EVENTS     (1UL << 16)             /* default capacity */
#define TRACE_RING_EVENTS_MIN (1UL << 10)
#define TRACE_RING_EVENTS_MAX (1UL << 30)
/* The namespaces of the calling thread, by inode. Not /proc/self: that is the main thread,
   whose namespace files are gone once it has exited, though the process lives on. */
#define TRACE_NAMESPACE_FILE  "/proc/thread-self/ns/pid"
#define TRACE_TIME_FILE       "/proc/thread-self/ns/time"
#define TRACE_STAT_FILE       "/proc/thread-self/stat"
#define TRACE_PMU_HOLD_FILE   TRACE_RING_DIR "/" TRACE_RING_PREFIX "pmu-hold"  /* locked if held */
/* A thread's /proc stat after the command name: its state, 18 fields, its start time. */
#define TRACE_STAT_FORMAT \
  " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %lu"
#define TRACE_SITE_MAX        (128)
#define TRACE_COUNTER_MAX     (16)
#define TRACE_NAME_BYTES      (32)
#define TRACE_CHOSEN_BYTES    (64)

#define TRACE_KIND_BEGIN      (0UL)  /* a span opens */
#define TRACE_KIND_END        (1UL)  /* the span of the same site closes */
#define TRACE_KIND_MARK       (2UL)  /* an instant */
#define TRACE_KIND_GAUGE      (3UL)  /* a level: value is the level, tsc is 0 */
#define TRACE_KIND_COUNTER    (4UL)  /* a hardware counter: site is its index, value its count */

#define TRACE_VALUE_BITS      (48)
#define TRACE_VALUE_MASK      ( ( 1UL << TRACE_VALUE_BITS ) - 1UL )
#define TRACE_SITE_SHIFT      (48)
#define TRACE_SITE_MASK       (0xfffUL)
#define TRACE_KIND_SHIFT      (60)

#define TRACE_ROOT            (1U)   /* site flag: a begin of this site opens a new trace */

typedef struct {
  uint64_t tsc;   /* trace_tsc() when it happened; 0 for a gauge */
  uint64_t word;  /* kind (4 bits), site (12), value (48) */
} trace_event_t;

typedef struct {
  uint32_t flags;
  char     name[ TRACE_NAME_BYTES - 4 ];
} trace_site_name_t;

typedef struct {
  char name  [ TRACE_NAME_BYTES ];    /* the metric: "dtlb_walk" */
  char chosen[ TRACE_CHOSEN_BYTES ];  /* the event taken for it on this host */
} trace_counter_name_t;

typedef struct {
  uint64_t             magic;         /* written last: the header is complete */
  uint32_t             header_bytes;  /* offset of the first event */
  uint32_t             pid;
  uint32_t             tid;
  uint32_t             site_cnt;
  uint64_t             event_cnt;       /* capacity, a power of two */
  uint64_t             pid_namespace;   /* inode of the pid namespace of pid and tid; 0: unknown */
  uint64_t             time_namespace;  /* inode of the time namespace start_ticks is told in;
                                           0: the kernel has none */
  uint64_t             start_ticks;     /* when the thread started, as its /proc stat says: tells
                                           it from a later thread of the same ids; 0: unknown */
  uint32_t             is_locked;     /* the ring is locked in memory */
  uint32_t             counter_cnt;   /* 0 unless trace_pmu_attach found some */
  uint32_t             counter_read_ns;    /* what one counter read costs this thread */
  uint32_t             counter_period_us;  /* one counter is read this often */
  char                 counter_status[ TRACE_CHOSEN_BYTES ];  /* why there are none, if so */
  char                 thread_name[ TRACE_NAME_BYTES ];
  trace_site_name_t    site[ TRACE_SITE_MAX ];
  trace_counter_name_t counter[ TRACE_COUNTER_MAX ];
  uint64_t             cursor __attribute__(( aligned( 64 ) ));  /* events ever written */
} trace_ring_t;

/* ---- the calling thread's writer ------------------------------------------------------ */

/* What trace_now calls once per period, with the timestamp it had just taken. It sets
   sample_next_tsc, the end of the period. */
struct trace_writer;
typedef void ( *trace_sampler_t )( struct trace_writer * writer, uint64_t tsc );

/* A thread that never attached writes to a one-slot ring shared by all such threads, so
   that no trace call has to test whether tracing is on. mask and cursor are kept apart on
   purpose: side by side, gcc loads them as one vector and the event costs 1.7 ns more. */
typedef struct trace_writer {
  uint64_t        mask;              /* event_cnt - 1 */
  trace_event_t * event;             /* the ring's first event */
  uint64_t *      cursor_shared;     /* the cursor tracerd reads */
  uint64_t        cursor;            /* private copy of the cursor */
  uint64_t        sample_next_tsc;   /* when the sampler next has work; never, by default */
  trace_sampler_t sampler;
  trace_ring_t *  ring;              /* NULL until attached */
#if TRACE_ARCH_HAS_DEMOTE  /* by architecture, not by TRACE_CLDEMOTE: no flag moves a field */
  uint64_t        demote_next;       /* first slot of the last pair of lines handed down */
#endif
} trace_writer_t;

/* On a cache line of its own: left to the linker it lands wherever the program's other
   thread-local data ends, and 24 bytes of those were enough to split it across two lines. */
extern __thread trace_writer_t trace_writer
  __attribute__(( tls_model( "initial-exec" ), aligned( 64 ) ));

/* ---- cold: once per thread ------------------------------------------------------------ */

typedef struct {
  char const * name;
  uint32_t     flags;  /* TRACE_ROOT or 0 */
} trace_site_t;

/* Create and map the calling thread's ring and publish the site names. Sites are the
   program's own enum: site[ i ] names site i. Every thread attaches before any thread's
   hot loop starts, because mapping memory can stall the other threads of the process.
   Returns 0 or a negative errno; after a failure trace calls still work and record nothing.
   Environment: TRACE_RING_DIR (default /dev/shm), TRACE_RING_EVENTS (default 65536). */
int trace_attach( trace_site_t const * site, int site_cnt, char const * thread_name );

/* ---- hot ------------------------------------------------------------------------------ */

TRACE_INLINE uint64_t trace_word( uint64_t kind, int site, uint64_t value ) {
  uint64_t site_bits = ( (uint64_t)site & TRACE_SITE_MASK ) << TRACE_SITE_SHIFT;
  return ( kind << TRACE_KIND_SHIFT ) | site_bits | ( value & TRACE_VALUE_MASK );
}

TRACE_INLINE void trace_emit( uint64_t tsc, uint64_t word ) {
  trace_writer_t * writer = &trace_writer;
  uint64_t         cursor = writer->cursor;
  trace_event_t *  event  = &writer->event[ cursor & writer->mask ];
  event->tsc     = tsc;
  event->word    = word;
  writer->cursor = cursor + 1UL;
  __atomic_store_n( writer->cursor_shared, cursor + 1UL, __ATOMIC_RELEASE );
}

/* Two events of one timestamp, published together: the writer fields are loaded once and
   the cursor is stored once. tracerd sees both or neither. */
TRACE_INLINE void trace_emit_two( uint64_t tsc, uint64_t first_word, uint64_t second_word ) {
  trace_writer_t * writer = &trace_writer;
  uint64_t         cursor = writer->cursor;
  uint64_t         mask   = writer->mask;
  trace_event_t *  first  = &writer->event[ cursor & mask ];
  trace_event_t *  second = &writer->event[ ( cursor + 1UL ) & mask ];
  first->tsc     = tsc;
  first->word    = first_word;
  second->tsc    = tsc;
  second->word   = second_word;
  writer->cursor = cursor + 2UL;
  __atomic_store_n( writer->cursor_shared, cursor + 2UL, __ATOMIC_RELEASE );
}

/* An event stored to a cache line its core does not hold ready to write waits for the line
   in the store buffer. The program does not wait, until its next atomic, barrier or system
   call, which waits for the store buffer: 20 to 120 ns, the tracer's and not the program's.
   The two functions below ask for the lines before the events come. A line holds 4 events
   and the events start on a page, so slots 4n to 4n+3 share one. Define TRACE_NO_PREFETCH,
   for the program and for trace_attach.c, to leave both out. */

/* The lines of the next 4 to 8 events: the line of slot cursor+3 is the one being written
   or the next. For the end of a span, when the program goes on to something else. A message
   of 7 events followed by an atomic was then up to 57 ns shorter on Granite Rapids after
   work that had evicted the lines, 36 to 97 ns on a Neoverse V2. It costs nothing measurable
   in a tight loop; 1 to 3 ns per message to a program walking a working set the size of its
   L2; and on Granite Rapids 5 to 12 ns per message after work that emptied the TLB, when no
   atomic follows the message. Not in trace_next: there it cost 1 ns per boundary. */
TRACE_INLINE void trace_own_ahead( void ) {
#ifndef TRACE_NO_PREFETCH
  trace_writer_t * writer = &trace_writer;
  uint64_t         cursor = writer->cursor;
  uint64_t         mask   = writer->mask;
  trace_arch_own( &writer->event[ ( cursor + 3UL ) & mask ] );
  trace_arch_own( &writer->event[ ( cursor + 7UL ) & mask ] );
#endif
}

/* The lines tracerd takes from the writer at each drain: the cursor's, and those of the next
   9 to 12 events. The first of these alone is not enough: the burst after a drain then took
   99 ns where it takes 68 with all three, as if the reading core fetched ahead of what it
   copies. Called by trace_now once per TRACE_ARCH_OWN_TICKS, not at every poll: that cost
   each empty poll 1 ns. Returns when to call again; never, where the architecture has no
   period for it. */
TRACE_INLINE uint64_t trace_own_back( trace_writer_t const * writer, uint64_t tsc ) {
#if TRACE_ARCH_OWN_TICKS && !defined(TRACE_NO_PREFETCH)
  trace_event_t * event  = writer->event;
  uint64_t        cursor = writer->cursor;
  uint64_t        mask   = writer->mask;
  trace_arch_own( writer->cursor_shared );
  trace_arch_own( &event[ cursor & mask ] );
  trace_arch_own( &event[ ( cursor + 4UL ) & mask ] );
  trace_arch_own( &event[ ( cursor + 8UL ) & mask ] );
  return tsc + TRACE_ARCH_OWN_TICKS;
#else
  (void)writer;
  (void)tsc;
  return UINT64_MAX;
#endif
}

/* The event that opens a message, after its poll. With TRACE_CLDEMOTE defined for the
   program, on x86, it also hands down to the shared cache (cldemote) the ring's lines the
   events before it filled: else the ring's 1 MiB passes through the core's own caches and
   pushes the program's data out of them. On Granite Rapids, with a working set of 65 to 100%
   of the L2, that took back two thirds to four fifths of what the ring cost the program: 15
   to 75 ns per message of 7 events. Off by default: an atomic or a barrier less than 100 ns
   after this event waits for the hand-down, 83 ns at once, 32 ns after 50 ns of work. Lines
   go a pair at a time, 4 at most, and never twice: a second hand-down of a line waits for
   the first. Here and not as a line fills: there the atomic that ends a message waited. */
TRACE_INLINE void trace_emit_begin( uint64_t tsc, uint64_t word ) {
#if TRACE_ARCH_HAS_DEMOTE && defined(TRACE_CLDEMOTE)
  trace_writer_t * writer = &trace_writer;
  uint64_t         cursor = writer->cursor;
  uint64_t         mask   = writer->mask;
  trace_event_t *  ring   = writer->event;
  trace_event_t *  event  = &ring[ cursor & mask ];
  uint64_t         pair   = cursor & mask & ~7UL;                   /* this event's pair */
  uint64_t         behind = ( pair - writer->demote_next ) & mask;  /* filled, not handed down */
  event->tsc     = tsc;
  event->word    = word;
  writer->cursor = cursor + 1UL;
  __atomic_store_n( writer->cursor_shared, cursor + 1UL, __ATOMIC_RELEASE );
  if( behind ) {  /* never without a ring: mask, pair and demote_next stay 0 */
    writer->demote_next = pair;
    trace_arch_demote( &ring[ ( pair - 8UL ) & mask ] );
    trace_arch_demote( &ring[ ( pair - 4UL ) & mask ] );
    if( behind > 8UL ) {
      trace_arch_demote( &ring[ ( pair - 16UL ) & mask ] );
      trace_arch_demote( &ring[ ( pair - 12UL ) & mask ] );
    }
  }
#else
  trace_emit( tsc, word );
#endif
}

/* The timestamp that closes a span: fenced, so that the span holds the loads it ended on.
   Unfenced, a stage of a few dependent cache misses is recorded as one timestamp read long
   and its time lands after it. Define TRACE_END_UNFENCED for the bare read: 6 to 8 ns less
   per end, for stages that end on no load worth timing. */
TRACE_INLINE uint64_t trace_tsc_end( void ) {
#ifdef TRACE_END_UNFENCED
  return trace_tsc();
#else
  return trace_tsc_fenced();
#endif
}

/* A span of `site` opens now. value is free: a sequence number, an id. */
TRACE_INLINE void trace_begin( int site, uint64_t value ) {
  trace_emit( trace_tsc(), trace_word( TRACE_KIND_BEGIN, site, value ) );
}

/* Same, backdated to a timestamp taken earlier with trace_now(), or to the one returned by
   the trace_end() of the span it follows. */
TRACE_INLINE void trace_begin_at( int site, uint64_t value, uint64_t tsc ) {
  trace_emit_begin( tsc, trace_word( TRACE_KIND_BEGIN, site, value ) );
}

/* The span of `site` closes now, once the work before it has completed. Returns its
   timestamp. The lines of the next events are asked for here: see trace_own_ahead. */
TRACE_INLINE uint64_t trace_end( int site ) {
  uint64_t tsc = trace_tsc_end();
  trace_emit( tsc, trace_word( TRACE_KIND_END, site, 0UL ) );
  trace_own_ahead();
  return tsc;
}

/* A stage boundary: the span of `end_site` closes and the span of `begin_site` opens at the
   same instant, for one timestamp where trace_end then trace_begin take two. Returns it. */
TRACE_INLINE uint64_t trace_next( int end_site, int begin_site, uint64_t value ) {
  uint64_t tsc = trace_tsc_end();
  trace_emit_two( tsc, trace_word( TRACE_KIND_END, end_site, 0UL ),
                  trace_word( TRACE_KIND_BEGIN, begin_site, value ) );
  return tsc;
}

TRACE_INLINE void trace_mark( int site, uint64_t value ) {
  trace_emit( trace_tsc(), trace_word( TRACE_KIND_MARK, site, value ) );
}

/* A level has no time: tracerd keeps the last and the largest, not when. */
TRACE_INLINE void trace_gauge( int site, uint64_t value ) {
  trace_emit( 0UL, trace_word( TRACE_KIND_GAUGE, site, value ) );
}

/* The timestamp for a poll loop: call it before the poll and pass the result to
   trace_begin_at when a message arrived. An empty poll then writes nothing. It is also
   where the periodic work is done: one compare, and once per period a call, which
   therefore sits before a trace and never inside one. The call asks the ring's lines back
   from tracerd (trace_own_back: x86, a thread with a ring; 27 ns on that poll, 40 to 47
   after work that evicted the lines) and reads one counter when counters are on. It is
   given the timestamp just taken, to stamp its event and to set its next period by: such a
   call then costs one more timestamp, the one read after it, not two. */
TRACE_INLINE uint64_t trace_now( void ) {
  trace_writer_t * writer = &trace_writer;
  uint64_t         tsc    = trace_tsc();
  if( __builtin_expect( tsc >= writer->sample_next_tsc, 0 ) ) {
    writer->sampler( writer, tsc );
    tsc = trace_tsc();
  }
  return tsc;
}

/* For a thread without a poll loop: sample the counters here, where a microsecond is cheap. */
TRACE_INLINE void trace_sample( void ) { (void)trace_now(); }

#ifdef __cplusplus
}
#endif

#endif /* TRACE_H */
