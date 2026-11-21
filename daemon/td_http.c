/* td_http.c: a web server reduced to what one page needs. GET of an embedded file, and
   GET /stream, which stays open and carries server-sent events. One thread, ppoll(), no
   allocation: each client owns a fixed output buffer. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include "td_http.h"
#include "td_util.h"

#define TD_HTTP_REQUEST_BYTES (16384)      /* a browser sends its cookies for localhost too */
#define TD_HTTP_HEAD_BYTES    (256)
#define TD_HTTP_URL_BYTES     (64)
#define TD_HTTP_URL_FORMAT    "GET %63s"   /* the 63 is TD_HTTP_URL_BYTES - 1: checked below */
#define TD_HTTP_BACKLOG_MAX   (32)         /* connections that wait: a browser opens six at once */
#define TD_HTTP_METHOD        "GET "
#define TD_HTTP_HOST_HEADER   "\nHost:"
#define TD_HTTP_LOOPBACK      "127."
#define TD_HTTP_LOCALHOST     "localhost"
#define TD_HTTP_STREAM_URL    "/stream"
#define TD_HTTP_EVENT_HEAD    "data: "
#define TD_HTTP_EVENT_TAIL    "\n\n"
#define TD_HTTP_IDLE_NS       (5UL * TD_NS_PER_SECOND)  /* a client that asks nothing is cut */

#define TD_HTTP_HEAD_STREAM   "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n" \
                              "Cache-Control: no-cache\r\n\r\n"
#define TD_HTTP_HEAD_FILE     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n" \
                              "Cache-Control: no-cache\r\nConnection: close\r\n\r\n"
#define TD_HTTP_HEAD_MISSING  "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n" \
                              "Connection: close\r\n\r\n"
#define TD_HTTP_HEAD_BUSY     "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n" \
                              "Connection: close\r\n\r\n"
#define TD_HTTP_HEAD_METHOD   "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET\r\n" \
                              "Content-Length: 0\r\nConnection: close\r\n\r\n"

/* The page's files, embedded at build time. Paths are relative to the repository root. */
#define TD_HTTP_ASSET( symbol, file )                                            \
  __asm__( ".pushsection .rodata\n"                                              \
           ".global " #symbol "\n" #symbol ":\n.incbin \"" file "\"\n"           \
           ".global " #symbol "_end\n" #symbol "_end:\n.byte 0\n.popsection\n" ); \
  extern char const symbol[], symbol##_end[];

TD_HTTP_ASSET( td_asset_index,   "ui/index.html" )
TD_HTTP_ASSET( td_asset_css,     "ui/ui.css"     )
TD_HTTP_ASSET( td_asset_main,    "ui/main.js"    )
TD_HTTP_ASSET( td_asset_state,   "ui/state.js"   )
TD_HTTP_ASSET( td_asset_stream,  "ui/stream.js"  )
TD_HTTP_ASSET( td_asset_draw,    "ui/draw.js"    )
TD_HTTP_ASSET( td_asset_latency, "ui/latency.js" )
TD_HTTP_ASSET( td_asset_traces,  "ui/traces.js"  )
TD_HTTP_ASSET( td_asset_lane,    "ui/lane.js"    )
TD_HTTP_ASSET( td_asset_table,   "ui/table.js"   )
TD_HTTP_ASSET( td_asset_check,   "ui/check.js"   )

_Static_assert( TD_HTTP_URL_BYTES == 64, "TD_HTTP_URL_FORMAT holds this size" );

typedef struct {
  char const * url;
  char const * type;
  char const * start;
  char const * end;
} td_http_asset_t;

static td_http_asset_t const td_http_asset[] = {
  { "/",           "text/html",       td_asset_index,   td_asset_index_end   },
  { "/ui.css",     "text/css",        td_asset_css,     td_asset_css_end     },
  { "/main.js",    "text/javascript", td_asset_main,    td_asset_main_end    },
  { "/state.js",   "text/javascript", td_asset_state,   td_asset_state_end   },
  { "/stream.js",  "text/javascript", td_asset_stream,  td_asset_stream_end  },
  { "/draw.js",    "text/javascript", td_asset_draw,    td_asset_draw_end    },
  { "/latency.js", "text/javascript", td_asset_latency, td_asset_latency_end },
  { "/traces.js",  "text/javascript", td_asset_traces,  td_asset_traces_end  },
  { "/lane.js",    "text/javascript", td_asset_lane,    td_asset_lane_end    },
  { "/table.js",   "text/javascript", td_asset_table,   td_asset_table_end   },
  { "/check.js",   "text/javascript", td_asset_check,   td_asset_check_end   },
};
#define TD_HTTP_ASSET_CNT TD_COUNT_OF( td_http_asset )

typedef struct {
  int      fd;          /* -1: free */
  int      is_stream;   /* stays open and receives events */
  int      is_closing;  /* close once the output is flushed */
  uint64_t accept_ns;
  size_t   request_bytes;
  size_t   out_bytes;   /* queued in `out`... */
  size_t   sent_bytes;  /* ...of which this many are sent */
  char     request[ TD_HTTP_REQUEST_BYTES ];
  char     out[ TD_HTTP_OUT_BYTES ];
} td_http_client_t;

static td_http_client_t td_http_client[ TD_HTTP_CLIENT_MAX ];
static int              td_http_listen_fd = -1;
static char             td_http_address[ TD_HTTP_URL_BYTES ];  /* as given to td_http_init */

int td_http_init( char const * address, int port ) {
  struct sockaddr_in where = { .sin_family = AF_INET, .sin_port = htons( (uint16_t)port ) };
  int                one   = 1, err;
  for( int i = 0; i < TD_HTTP_CLIENT_MAX; i++ ) td_http_client[ i ].fd = -1;
  if( inet_pton( AF_INET, address, &where.sin_addr ) != 1 ) return -EINVAL;
  snprintf( td_http_address, sizeof( td_http_address ), "%s", address );
  td_http_listen_fd = socket( AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 );
  if( td_http_listen_fd < 0 ) return -errno;
  setsockopt( td_http_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof( one ) );
  if( bind( td_http_listen_fd, (struct sockaddr *)&where, sizeof( where ) ) ||
      listen( td_http_listen_fd, TD_HTTP_BACKLOG_MAX ) ) {
    err = -errno;
    close( td_http_listen_fd );
    td_http_listen_fd = -1;
    return err;
  }
  return 0;
}

static void td_http_close( td_http_client_t * client ) {
  close( client->fd );
  client->fd = -1;
}

static void td_http_queue( td_http_client_t * client, char const * data, size_t byte_cnt ) {
  if( client->fd < 0 ) return;
  if( client->sent_bytes == client->out_bytes ) client->out_bytes = client->sent_bytes = 0UL;
  else if( client->sent_bytes && client->out_bytes + byte_cnt > TD_HTTP_OUT_BYTES ) {
    /* The tail is full: move what is left to the front, the room is then one piece. */
    client->out_bytes -= client->sent_bytes;
    memmove( client->out, client->out + client->sent_bytes, client->out_bytes );
    client->sent_bytes = 0UL;
  }
  if( client->out_bytes + byte_cnt > TD_HTTP_OUT_BYTES ) {
    TD_LOG_ONCE( "tracerd: a page client could not keep up and was cut\n" );
    td_http_close( client );
    return;
  }
  memcpy( client->out + client->out_bytes, data, byte_cnt );
  client->out_bytes += byte_cnt;
}

static void td_http_flush( td_http_client_t * client ) {
  while( client->fd >= 0 && client->sent_bytes < client->out_bytes ) {
    size_t  left_bytes  = client->out_bytes - client->sent_bytes;
    ssize_t wrote_bytes = send( client->fd, client->out + client->sent_bytes, left_bytes,
                                MSG_NOSIGNAL );
    if( wrote_bytes < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) ) return;
    if( wrote_bytes <= 0 ) { td_http_close( client ); return; }
    client->sent_bytes += (size_t)wrote_bytes;
  }
  if( client->fd >= 0 && client->is_closing ) td_http_close( client );
}

static td_http_client_t * td_http_free_client( void ) {
  for( int i = 0; i < TD_HTTP_CLIENT_MAX; i++ )
    if( td_http_client[ i ].fd < 0 ) return &td_http_client[ i ];
  return NULL;
}

/* Called only while a client slot is free: a connection that finds none waits in the
   listen backlog until one is, instead of being accepted and dropped. */
static void td_http_accept( void ) {
  td_http_client_t * client = td_http_free_client();
  int                one    = 1, fd;
  if( !client ) return;
  fd = accept4( td_http_listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC );
  if( fd < 0 ) return;
  setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof( one ) );
  client->fd            = fd;
  client->is_stream     = client->is_closing = 0;
  client->accept_ns     = td_clock_ns();
  client->request_bytes = client->out_bytes = client->sent_bytes = 0UL;
}

static int td_http_stream_cnt( void ) {
  int stream_cnt = 0;
  for( int i = 0; i < TD_HTTP_CLIENT_MAX; i++ )
    stream_cnt += td_http_client[ i ].fd >= 0 && td_http_client[ i ].is_stream;
  return stream_cnt;
}

/* A browser names the host it thinks it talks to. When the page is served on loopback only,
   a page elsewhere that points a name of its own at 127.0.0.1 (DNS rebinding) names that
   instead, and is refused. Served on another address, any name may be the right one. */
static int td_http_host_is_ours( char const * request ) {
  char const * host = strcasestr( request, TD_HTTP_HOST_HEADER );
  size_t       host_bytes;
  if( strncmp( td_http_address, TD_HTTP_LOOPBACK, sizeof( TD_HTTP_LOOPBACK ) - 1UL ) ) return 1;
  if( !host ) return 1;  /* not a browser: only a browser can be made to rebind */
  host += sizeof( TD_HTTP_HOST_HEADER ) - 1UL;
  while( *host == ' ' || *host == '\t' ) host++;
  host_bytes = strcspn( host, ":\r\n \t" );
  if( host_bytes == strlen( td_http_address ) && !strncmp( host, td_http_address, host_bytes ) )
    return 1;
  return host_bytes == sizeof( TD_HTTP_LOCALHOST ) - 1UL &&
         !strncasecmp( host, TD_HTTP_LOCALHOST, host_bytes );
}

static void td_http_answer( int client_idx, void ( *on_stream )( int ) ) {
  td_http_client_t * client = &td_http_client[ client_idx ];
  char               head[ TD_HTTP_HEAD_BYTES ], url[ TD_HTTP_URL_BYTES ] = "";
  size_t             asset_bytes;
  int                head_bytes;

  sscanf( client->request, TD_HTTP_URL_FORMAT, url );
  url[ strcspn( url, "?" ) ] = '\0';
  if( !td_http_host_is_ours( client->request ) ) url[ 0 ] = '\0';  /* answered as not found */
  if( !strcmp( url, TD_HTTP_STREAM_URL ) && td_http_stream_cnt() >= TD_HTTP_STREAM_MAX ) {
    client->is_closing = 1;  /* the slots left are for fetching the page's files */
    td_http_queue( client, TD_HTTP_HEAD_BUSY, sizeof( TD_HTTP_HEAD_BUSY ) - 1UL );
    return;
  }
  if( !strcmp( url, TD_HTTP_STREAM_URL ) ) {
    td_http_queue( client, TD_HTTP_HEAD_STREAM, sizeof( TD_HTTP_HEAD_STREAM ) - 1UL );
    client->is_stream = 1;
    on_stream( client_idx );
    td_http_flush( client );  /* the page's first frame now, not with the next tick's */
    return;
  }
  client->is_closing = 1;
  if( strncmp( client->request, TD_HTTP_METHOD, sizeof( TD_HTTP_METHOD ) - 1UL ) ) {
    td_http_queue( client, TD_HTTP_HEAD_METHOD, sizeof( TD_HTTP_HEAD_METHOD ) - 1UL );
    return;
  }
  for( int i = 0; i < TD_HTTP_ASSET_CNT; i++ ) {
    td_http_asset_t const * asset = &td_http_asset[ i ];
    if( strcmp( url, asset->url ) ) continue;
    asset_bytes = (size_t)( asset->end - asset->start );
    head_bytes  = snprintf( head, sizeof( head ), TD_HTTP_HEAD_FILE, asset->type, asset_bytes );
    td_http_queue( client, head, (size_t)head_bytes );
    td_http_queue( client, asset->start, asset_bytes );
    return;
  }
  td_http_queue( client, TD_HTTP_HEAD_MISSING, sizeof( TD_HTTP_HEAD_MISSING ) - 1UL );
}

static void td_http_read( int client_idx, void ( *on_stream )( int ) ) {
  td_http_client_t * client    = &td_http_client[ client_idx ];
  ssize_t            got_bytes = recv( client->fd, client->request + client->request_bytes,
                                       sizeof( client->request ) - 1UL - client->request_bytes, 0 );
  if( got_bytes < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) ) return;
  if( got_bytes <= 0 ) { td_http_close( client ); return; }
  if( client->is_stream ) return;  /* nothing more is expected from it */
  client->request_bytes += (size_t)got_bytes;
  client->request[ client->request_bytes ] = '\0';
  if( strstr( client->request, "\r\n\r\n" ) ) td_http_answer( client_idx, on_stream );
  else if( client->request_bytes + 1UL >= sizeof( client->request ) ) td_http_close( client );
}

void td_http_poll( int64_t timeout_ns, void ( *on_stream )( int client_idx ) ) {
  struct pollfd   wait[ TD_HTTP_CLIENT_MAX + 1 ];
  int             client_of[ TD_HTTP_CLIENT_MAX + 1 ], wait_cnt = 1;
  uint64_t        now_ns  = td_clock_ns();
  /* To the nanosecond: poll()'s whole milliseconds, rounded up, woke the loop 0.6 ms late. */
  struct timespec timeout = { .tv_sec  = (time_t)( timeout_ns / (int64_t)TD_NS_PER_SECOND ),
                              .tv_nsec = (long)( timeout_ns % (int64_t)TD_NS_PER_SECOND ) };

  wait[ 0 ].fd      = td_http_listen_fd;
  wait[ 0 ].events  = 0;  /* POLLIN below, if a slot is free once the idle clients are cut */
  wait[ 0 ].revents = 0;
  for( int i = 0; i < TD_HTTP_CLIENT_MAX; i++ ) {
    td_http_client_t * client = &td_http_client[ i ];
    if( client->fd >= 0 && !client->is_stream && now_ns - client->accept_ns > TD_HTTP_IDLE_NS )
      td_http_close( client );
    if( client->fd < 0 ) continue;
    wait[ wait_cnt ].fd      = client->fd;
    /* One that is being answered is no longer listened to: it may have closed its sending
       side already, and must get its answer all the same. */
    wait[ wait_cnt ].events  = client->is_closing ? POLLOUT : POLLIN;
    if( client->sent_bytes < client->out_bytes ) wait[ wait_cnt ].events |= POLLOUT;
    wait[ wait_cnt ].revents = 0;
    client_of[ wait_cnt++ ]  = i;
  }
  if( td_http_free_client() ) wait[ 0 ].events = POLLIN;
  if( ppoll( wait, (nfds_t)wait_cnt, &timeout, NULL ) <= 0 ) return;
  if( wait[ 0 ].revents & POLLIN ) td_http_accept();
  for( int i = 1; i < wait_cnt; i++ ) {
    td_http_client_t * client = &td_http_client[ client_of[ i ] ];
    short              events = wait[ i ].revents;
    if( client->fd >= 0 && ( events & ( POLLIN | POLLHUP | POLLERR ) ) )
      td_http_read( client_of[ i ], on_stream );
    if( client->fd >= 0 && ( events & POLLOUT ) ) td_http_flush( client );
  }
}

void td_http_send( int client_idx, char const * data, size_t byte_cnt ) {
  td_http_client_t * client = &td_http_client[ client_idx ];
  if( client->fd < 0 || !client->is_stream ) return;
  td_http_queue( client, TD_HTTP_EVENT_HEAD, sizeof( TD_HTTP_EVENT_HEAD ) - 1UL );
  td_http_queue( client, data, byte_cnt );
  td_http_queue( client, TD_HTTP_EVENT_TAIL, sizeof( TD_HTTP_EVENT_TAIL ) - 1UL );
}

void td_http_flush_all( void ) {
  for( int i = 0; i < TD_HTTP_CLIENT_MAX; i++ )
    if( td_http_client[ i ].is_stream ) td_http_flush( &td_http_client[ i ] );
}

int td_http_has_stream( void ) { return td_http_stream_cnt() > 0; }

int td_http_is_stream( int client_idx, size_t * room_bytes ) {
  td_http_client_t const * client = &td_http_client[ client_idx ];
  if( client->fd < 0 || !client->is_stream ) return 0;
  *room_bytes = TD_HTTP_OUT_BYTES - ( client->out_bytes - client->sent_bytes );
  return 1;
}

void td_http_broadcast( char const * data, size_t byte_cnt ) {
  for( int i = 0; i < TD_HTTP_CLIENT_MAX; i++ ) td_http_send( i, data, byte_cnt );
}
