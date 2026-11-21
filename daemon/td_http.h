/* td_http.h: the page and its stream. Serves the files embedded in the binary and keeps
   server-sent-event clients fed. Sockets never block: a client that cannot keep up, or
   that connects and says nothing, is cut. */
#ifndef TD_HTTP_H
#define TD_HTTP_H

#include <stddef.h>
#include <stdint.h>

#define TD_HTTP_CLIENT_MAX (8)  /* connections served at once; more wait their turn */
#define TD_HTTP_STREAM_MAX (6)  /* pages open at once: the other slots fetch the page's files */
#define TD_HTTP_OUT_BYTES  (8UL << 20)  /* queued per client; the replay paces itself to fit */

/* Listen on address:port. Returns 0 or a negative errno. */
int td_http_init( char const * address, int port );

/* Wait up to timeout_ns for socket work, then do it: accept, answer requests, flush. When a
   client asks for the stream, on_stream( client_idx ) is called once to give it the past;
   what that queues is sent at once. */
void td_http_poll( int64_t timeout_ns, void ( *on_stream )( int client_idx ) );

/* Queue one event for one stream client, or for all of them. Nothing is sent before
   td_http_flush_all: a tick's events leave in one send per client, not one each. */
void td_http_send( int client_idx, char const * data, size_t byte_cnt );
void td_http_broadcast( char const * data, size_t byte_cnt );

/* Send what is queued for the stream clients. Call once a tick, after its last event. */
void td_http_flush_all( void );

/* 1 while anyone is listening to the stream: frames are built for nobody otherwise. */
int td_http_has_stream( void );

/* 1 while client_idx is a stream client. *room_bytes: what its output buffer can still take. */
int td_http_is_stream( int client_idx, size_t * room_bytes );

#endif /* TD_HTTP_H */
