/* td_util.h: the daemon's small tools. A clock, a log-linear histogram, a bounded text
   builder for JSON, file and kernel-list readers, number parsers, a walker of per-CPU
   tables, and the /proc paths several files share. All fixed size; nothing allocates. */
#ifndef TD_UTIL_H
#define TD_UTIL_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TD_NS_PER_US       (1000UL)
#define TD_NS_PER_MS       (1000000UL)
#define TD_MS_PER_SECOND   (1000UL)
#define TD_NS_PER_SECOND   (1000000000UL)
#define TD_COUNT_OF( array ) ( (int)( sizeof( array ) / sizeof( ( array )[ 0 ] ) ) )

#define TD_CPU_MAX         (1024)
#define TD_PATH_BYTES      (256)
#define TD_CPU_LIST_BYTES  (1024)  /* a kernel CPU list such as 1-3,5-7 */
#define TD_TASK_FILE       "/proc/%u/task/%u/%s"  /* pid, tid, leaf: a thread's own files */
#define TD_TASK_PATH_BYTES (64)
#define TD_TASK_STAT       "stat"
#define TD_TASK_STATUS     "status"
#define TD_TASK_SCHEDSTAT  "schedstat"
#define TD_INTERRUPTS_FILE "/proc/interrupts"
#define TD_ISOLATED_FILE   "/sys/devices/system/cpu/isolated"
#define TD_ONLINE_FILE     "/sys/devices/system/cpu/online"
#define TD_LOOPBACK_NAME   "lo"  /* the interface no source shows */
#define TD_NOHZ_FULL_FILE  "/sys/devices/system/cpu/nohz_full"

/* Say something once: for the cold paths that would otherwise repeat it every tick. */
#define TD_LOG_ONCE( ... )                                    \
  do {                                                        \
    static int td_log_once_done;                              \
    if( !td_log_once_done ) fprintf( stderr, __VA_ARGS__ );   \
    td_log_once_done = 1;                                     \
  } while( 0 )

/* ---- clock --------------------------------------------------------------------------- */

static inline uint64_t td_clock_ns( void ) {
  struct timespec now;
  clock_gettime( CLOCK_MONOTONIC, &now );
  return (uint64_t)now.tv_sec * TD_NS_PER_SECOND + (uint64_t)now.tv_nsec;
}

static inline uint64_t td_wall_ms( void ) {
  struct timespec now;
  clock_gettime( CLOCK_REALTIME, &now );
  return (uint64_t)now.tv_sec * TD_MS_PER_SECOND + (uint64_t)now.tv_nsec / TD_NS_PER_MS;
}

/* ---- histogram ------------------------------------------------------------------------
   Eight buckets per power of two: a bucket is 7 to 12.5 % wide, so a percentile is good to
   about 6 %. The maximum is kept exactly. Values are nanoseconds. */

#define TD_HIST_SUB_BITS   (3)
#define TD_HIST_SUB_CNT    (1 << TD_HIST_SUB_BITS)
#define TD_HIST_BUCKET_CNT (44 * TD_HIST_SUB_CNT)  /* up to 2^46 ns, about nineteen hours */

typedef struct {
  uint32_t bucket[ TD_HIST_BUCKET_CNT ];
  uint64_t cnt;
  uint64_t max_ns;
} td_hist_t;

static inline int td_hist_bucket( uint64_t value_ns ) {
  int power, bucket;
  if( value_ns < (uint64_t)TD_HIST_SUB_CNT ) return (int)value_ns;
  power  = 63 - __builtin_clzl( value_ns );
  bucket = ( ( power - TD_HIST_SUB_BITS + 1 ) << TD_HIST_SUB_BITS ) +
           ( (int)( value_ns >> ( power - TD_HIST_SUB_BITS ) ) & ( TD_HIST_SUB_CNT - 1 ) );
  return bucket < TD_HIST_BUCKET_CNT ? bucket : TD_HIST_BUCKET_CNT - 1;
}

/* The middle of a bucket: what a percentile reports. */
static inline uint64_t td_hist_value( int bucket ) {
  int      power;
  uint64_t low, width;
  if( bucket < TD_HIST_SUB_CNT ) return (uint64_t)bucket;
  power = ( bucket >> TD_HIST_SUB_BITS ) + TD_HIST_SUB_BITS - 1;
  width = 1UL << ( power - TD_HIST_SUB_BITS );
  low   = ( 1UL << power ) + (uint64_t)( bucket & ( TD_HIST_SUB_CNT - 1 ) ) * width;
  return low + width / 2UL;
}

static inline void td_hist_add( td_hist_t * hist, uint64_t value_ns ) {
  hist->bucket[ td_hist_bucket( value_ns ) ]++;
  hist->cnt++;
  if( value_ns > hist->max_ns ) hist->max_ns = value_ns;
}

/* The value below which `fraction` of the samples fall; 0 when empty. */
static inline uint64_t td_hist_percentile( td_hist_t const * hist, double fraction ) {
  uint64_t wanted = (uint64_t)( fraction * (double)hist->cnt ), seen = 0UL;
  if( !hist->cnt ) return 0UL;
  for( int i = 0; i < TD_HIST_BUCKET_CNT; i++ ) {
    seen += hist->bucket[ i ];
    if( seen <= wanted ) continue;
    return td_hist_value( i ) < hist->max_ns ? td_hist_value( i ) : hist->max_ns;
  }
  return hist->max_ns;
}

static inline void td_hist_reset( td_hist_t * hist ) { memset( hist, 0, sizeof( *hist ) ); }

/* ---- text ----------------------------------------------------------------------------
   Appends into a caller's buffer. Once full it stops and remembers: the caller checks
   `is_overflow` once at the end instead of after every append. */

typedef struct {
  char * data;
  size_t data_bytes;
  size_t byte_cnt;
  int    is_overflow;
} td_text_t;

static inline void td_text_init( td_text_t * text, char * data, size_t data_bytes ) {
  text->data        = data;
  text->data_bytes  = data_bytes;
  text->byte_cnt    = 0UL;
  text->is_overflow = 0;
  if( data_bytes ) data[ 0 ] = '\0';
}

__attribute__(( format( printf, 2, 3 ) ))
static inline void td_text_printf( td_text_t * text, char const * format, ... ) {
  va_list arg;
  int     byte_cnt;
  size_t  room_bytes = text->data_bytes - text->byte_cnt;
  if( text->is_overflow ) return;
  va_start( arg, format );
  byte_cnt = vsnprintf( text->data + text->byte_cnt, room_bytes, format, arg );
  va_end( arg );
  if( byte_cnt >= 0 && (size_t)byte_cnt < room_bytes ) {
    text->byte_cnt += (size_t)byte_cnt;
    return;
  }
  text->is_overflow = 1;
  text->data[ text->byte_cnt ] = '\0';
}

/* Bytes as they are, and a whole number in decimal: what a frame is mostly made of, without
   a format to parse. */
static inline void td_text_put( td_text_t * text, char const * data, size_t byte_cnt ) {
  if( text->is_overflow ) return;
  if( byte_cnt >= text->data_bytes - text->byte_cnt ) {
    text->is_overflow = 1;
    text->data[ text->byte_cnt ] = '\0';
    return;
  }
  memcpy( text->data + text->byte_cnt, data, byte_cnt );
  text->byte_cnt += byte_cnt;
  text->data[ text->byte_cnt ] = '\0';
}

/* A string literal as it is: the compiler knows its length. The "" refuses a pointer. */
#define TD_TEXT_LITERAL( text, literal ) td_text_put( text, "" literal, sizeof( literal ) - 1UL )

static inline void td_text_uint( td_text_t * text, uint64_t value ) {
  char   digit[ 20 ];  /* 2^64 has twenty */
  size_t first = sizeof( digit );
  do digit[ --first ] = (char)( '0' + value % 10UL ); while( value /= 10UL );
  td_text_put( text, digit + first, sizeof( digit ) - first );
}

/* A JSON string, quoted and escaped. At most string_bytes of `string` are read: it may
   come from shared memory, where nothing guarantees a terminator. */
static inline void td_text_json( td_text_t * text, char const * string, size_t string_bytes ) {
  td_text_put( text, "\"", 1UL );
  for( size_t i = 0UL; i < string_bytes && string[ i ]; i++ ) {
    char byte = string[ i ];
    if( byte == '"' || byte == '\\' ) td_text_put( text, "\\", 1UL );
    if( (unsigned char)byte < 0x20 ) byte = ' ';
    td_text_put( text, &byte, 1UL );
  }
  td_text_put( text, "\"", 1UL );
}

/* ---- files --------------------------------------------------------------------------- */

/* For tests: when set, a file found under this directory is read instead of the real one,
   so that captures of another host's /proc can stand in for it. */
char const * td_file_root __attribute__(( weak ));  /* one copy, whoever includes this */

/* Read a whole file, NUL terminated. Returns the byte count or a negative errno; -EFBIG
   when it did not fit, because half a file is worse than none. */
static inline long td_file_read( char const * path, char * text, size_t text_bytes ) {
  char   rooted[ 2 * TD_PATH_BYTES ];
  size_t byte_cnt = 0UL;
  int    fd       = -1;
  if( td_file_root ) {
    snprintf( rooted, sizeof( rooted ), "%s%s", td_file_root, path );
    fd = open( rooted, O_RDONLY | O_CLOEXEC );
  }
  if( fd < 0 ) fd = open( path, O_RDONLY | O_CLOEXEC );
  if( fd < 0 ) return -errno;
  while( byte_cnt + 1UL < text_bytes ) {
    ssize_t got = read( fd, text + byte_cnt, text_bytes - 1UL - byte_cnt );
    if( got < 0 ) { int err = errno; close( fd ); return -err; }
    if( !got ) break;
    byte_cnt += (size_t)got;
  }
  close( fd );
  text[ byte_cnt ] = '\0';
  if( byte_cnt + 1UL >= text_bytes ) return -EFBIG;
  return (long)byte_cnt;
}

/* A file that is read again and again, through an fd that stays open: no path to walk and
   no file to set up and tear down at each read. Zeroed, it is closed. */
typedef struct {
  int fd_plus1;  /* the fd plus one; 0: not open */
} td_kept_t;

static inline void td_kept_close( td_kept_t * kept ) {
  if( kept->fd_plus1 ) close( kept->fd_plus1 - 1 );
  kept->fd_plus1 = 0;
}

/* Read a whole file as td_file_read does, through its kept fd: a pread from the start,
   which makes the kernel print a /proc file afresh. `path` is used only to open the file
   when it is not open yet. is_single: the file is one record (a thread's stat, status or
   schedstat; /proc/stat), which the kernel hands over whole in one read: no second read to
   learn that it ended. A read that fails closes the fd: the thread of a /proc file has
   exited (ESRCH), and the next read opens whatever the path then names. */
static inline long td_kept_read( td_kept_t * kept, char const * path, char * text,
                                 size_t text_bytes, int is_single ) {
  size_t byte_cnt = 0UL;
  if( !kept->fd_plus1 ) {
    char rooted[ 2 * TD_PATH_BYTES ];
    int  fd = -1;
    if( td_file_root ) {
      snprintf( rooted, sizeof( rooted ), "%s%s", td_file_root, path );
      fd = open( rooted, O_RDONLY | O_CLOEXEC );
    }
    if( fd < 0 ) fd = open( path, O_RDONLY | O_CLOEXEC );
    if( fd < 0 ) return -errno;
    kept->fd_plus1 = fd + 1;
  }
  while( byte_cnt + 1UL < text_bytes ) {
    ssize_t got = pread( kept->fd_plus1 - 1, text + byte_cnt, text_bytes - 1UL - byte_cnt,
                         (off_t)byte_cnt );
    if( got < 0 ) { int err = errno; td_kept_close( kept ); return -err; }
    if( !got ) break;
    byte_cnt += (size_t)got;
    if( is_single && !td_file_root ) break;  /* a capture is a plain file: read to its end */
  }
  text[ byte_cnt ] = '\0';
  if( byte_cnt + 1UL >= text_bytes ) return -EFBIG;
  return (long)byte_cnt;
}

/* The number after "key:" or "key " at the start of a line of a /proc or /sys file, or -1
   when absent. */
static inline long td_file_field( char const * text, char const * key ) {
  size_t       key_bytes = strlen( key );
  char const * line      = text;
  while( line ) {
    char after  = strncmp( line, key, key_bytes ) ? '\0' : line[ key_bytes ];
    int  is_key = after == ':' || after == ' ' || after == '\t';
    if( is_key ) return atol( line + key_bytes + 1UL );
    line = strchr( line, '\n' );
    if( line ) line++;
  }
  return -1L;
}

/* Walk a kernel list such as "0,2-3". Returns `number` when the list holds it, the nth
   entry (from 0) when the list is that long, else -1. Pass -1 or LONG_MAX for the question
   not asked. */
static inline long td_list_scan( char const * list, long nth, long number ) {
  char * cursor = (char *)list;
  while( *cursor && *cursor != '\n' ) {
    char * start = cursor;
    long   first = strtol( cursor, &cursor, 10 ), last = first;
    if( cursor == start ) break;  /* not a number: an empty list reads "(null)" on some kernels */
    if( *cursor == '-' ) last = strtol( cursor + 1, &cursor, 10 );
    if( number >= first && number <= last ) return number;
    if( nth <= last - first ) return first + nth;
    nth -= last - first + 1L;
    if( *cursor != ',' ) break;
    cursor++;
  }
  return -1L;
}

/* The nth number (from 0) of a kernel list, or -1 when it is shorter. */
static inline long td_list_nth( char const * list, long nth ) {
  return td_list_scan( list, nth, -1L );
}

/* Is `number` in a kernel list such as "1-3,5-7"? */
static inline int td_list_has( char const * list, long number ) {
  return number >= 0L && td_list_scan( list, LONG_MAX, number ) >= 0L;
}

/* Read one of a thread's own /proc files, `leaf` being TD_TASK_STAT and the like. Returns
   as td_file_read does. */
static inline long td_task_read( uint32_t pid, uint32_t tid, char const * leaf, char * text,
                                 size_t text_bytes ) {
  char path[ TD_TASK_PATH_BYTES ];
  snprintf( path, sizeof( path ), TD_TASK_FILE, pid, tid, leaf );
  return td_file_read( path, text, text_bytes );
}

/* What the daemon takes from a thread's stat file: one read serves all who want it. */
typedef struct {
  char          state;        /* 'R', 'S', ...; 'Z' or 'X' once it has exited */
  unsigned long minor_cnt;    /* page faults */
  unsigned long major_cnt;
  unsigned long start_ticks;  /* when the thread started */
  int           cpu;          /* where it last ran */
} td_task_stat_t;

/* The fields after the command name, counted from the state, which is 0. */
#define TD_TASK_STAT_MINOR (7)
#define TD_TASK_STAT_MAJOR (9)
#define TD_TASK_STAT_START (19)
#define TD_TASK_STAT_CPU   (36)

/* Parse the text of a thread's stat file. The command name may hold blanks and brackets:
   the fields are counted from its last bracket. Returns 0, or -EINVAL when the text is
   not a stat file's or is cut short. */
static inline int td_task_stat_parse( char const * text, td_task_stat_t * stat ) {
  char const * cursor = strrchr( text, ')' );
  if( !cursor ) return -EINVAL;
  cursor++;
  while( *cursor == ' ' ) cursor++;
  if( !*cursor ) return -EINVAL;
  stat->state = *cursor++;
  for( int field = 1; field <= TD_TASK_STAT_CPU; field++ ) {
    char const *  digit;
    unsigned long value = 0UL;
    while( *cursor == ' ' ) cursor++;
    if( *cursor == '-' ) cursor++;  /* a few are signed; none of those wanted here */
    for( digit = cursor; *digit >= '0' && *digit <= '9'; digit++ )
      value = value * 10UL + (unsigned long)( *digit - '0' );
    if( digit == cursor ) return -EINVAL;
    cursor = digit;
    if( field == TD_TASK_STAT_MINOR ) stat->minor_cnt   = value;
    if( field == TD_TASK_STAT_MAJOR ) stat->major_cnt   = value;
    if( field == TD_TASK_STAT_START ) stat->start_ticks = value;
    if( field == TD_TASK_STAT_CPU   ) stat->cpu         = (int)value;
  }
  return 0;
}

/* A whole number that is not negative, or a finite decimal number, from the command line.
   Returns 0, or -EINVAL when the text is not exactly one such number, plainly written. */
static inline int td_parse_long( char const * text, long * value ) {
  char * end;
  errno  = 0;
  *value = strtol( text, &end, 10 );
  return *text >= '0' && *text <= '9' && !*end && !errno ? 0 : -EINVAL;
}

static inline int td_parse_double( char const * text, double * value ) {
  char * end;
  *value = strtod( text, &end );
  if( strspn( text, "+-.0123456789eE" ) != strlen( text ) ) return -EINVAL;  /* no nan, no 0x */
  return *text && !*end && isfinite( *value ) ? 0 : -EINVAL;
}

/* ---- per-CPU tables --------------------------------------------------------------------
   /proc/interrupts and /proc/softirqs: a header of CPU names, then "label: n n n ... rest"
   rows. The header is what says which column a CPU is: offline CPUs have none. */

#define TD_ROWS_CPU "CPU"  /* what a column's name begins with */

typedef struct {
  char *   line;        /* the row to parse next */
  int      column;      /* the column of the CPU asked for */
  int      column_cnt;  /* columns in the header: a row with fewer is not per CPU */
  char *   label;       /* of the row just parsed: "70" or "LOC", NUL terminated */
  char *   tail;        /* what follows its numbers: the description */
  uint64_t count;       /* its value in the column */
  char *   row_end;     /* where the row just parsed was cut, to be mended: or NULL */
  char *   label_end;
} td_rows_t;

/* Start walking `text` for `cpu`. The row being looked at is cut into strings and mended
   at the next call, so the text can be walked again for another CPU once a walk has
   ended. Returns 0, or -ENOENT when the table has no column for that CPU. */
static inline int td_rows_begin( td_rows_t * rows, char * text, int cpu ) {
  char * header_end = strchr( text, '\n' );
  char * token      = text;
  if( !header_end ) return -ENOENT;
  rows->column     = -1;
  rows->column_cnt = 0;
  rows->count      = 0UL;
  rows->label      = rows->tail = header_end;
  rows->row_end    = rows->label_end = NULL;
  *header_end = '\0';  /* the header alone is searched, not the table after it */
  while( ( token = strstr( token, TD_ROWS_CPU ) ) ) {
    token += sizeof( TD_ROWS_CPU ) - 1UL;
    if( atoi( token ) == cpu && *token >= '0' && *token <= '9' ) rows->column = rows->column_cnt;
    rows->column_cnt++;
  }
  *header_end = '\n';
  rows->line = header_end + 1;
  return rows->column < 0 ? -ENOENT : 0;
}

/* Parse the next per-CPU row. Returns 1, or 0 at the end of the table. Rows that are not
   per CPU, such as ERR, are passed over. */
static inline int td_rows_next( td_rows_t * rows ) {
  while( rows->line && *rows->line ) {
    char * line   = rows->line;
    char * end;
    char * cursor;
    int    column = 0;
    if( rows->row_end )   *rows->row_end   = '\n';
    if( rows->label_end ) *rows->label_end = ':';
    end = strchr( line, '\n' );
    if( end ) *end = '\0';  /* before the colon is looked for: it must be this row's */
    cursor          = strchr( line, ':' );
    rows->line      = end ? end + 1 : NULL;
    rows->row_end   = end;
    rows->label_end = cursor;
    if( !cursor ) continue;
    *cursor++ = '\0';
    for( ; column < rows->column_cnt; column++ ) {
      char *   digit;
      uint64_t count = 0UL;
      while( *cursor == ' ' ) cursor++;
      for( digit = cursor; *digit >= '0' && *digit <= '9'; digit++ )
        count = count * 10UL + (uint64_t)( *digit - '0' );
      if( digit == cursor ) break;
      if( column == rows->column ) rows->count = count;
      cursor = digit;
    }
    if( column < rows->column_cnt ) continue;
    while( *line == ' ' ) line++;
    rows->label = line;
    rows->tail  = cursor;
    return 1;
  }
  if( rows->row_end )   *rows->row_end   = '\n';
  if( rows->label_end ) *rows->label_end = ':';
  rows->row_end = rows->label_end = NULL;
  return 0;
}

#endif /* TD_UTIL_H */
