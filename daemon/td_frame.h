/* td_frame.h: what the page receives. Three kinds of JSON frame: "hello" describes the
   threads, sources and instances; "tick" carries one 50 ms slot; "second" one second.
   A new client gets hello, then the past as the same ticks and seconds, a batch per tick. */
#ifndef TD_FRAME_H
#define TD_FRAME_H

#include <stdint.h>

/* Send hello to every stream client: the set of threads or instances changed. */
void td_frame_hello_all( void );

/* Send the slot or second that just closed to every stream client. is_known 0: it ended
   while the daemon was stopped and holds nothing of its own. No frame is sent for it, now
   or in a replay: an index the page never got is a gap on the page. */
void td_frame_tick( uint64_t slot, int is_known );
void td_frame_second( uint64_t second, int is_known );

/* A new stream client: send hello and start replaying the history to it. The callback
   td_http_poll wants. */
void td_frame_replay_start( int client_idx );

/* Send the next part of the history to the clients still catching up. Call every tick. */
void td_frame_replay_step( void );

#endif /* TD_FRAME_H */
