#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <hiredis.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>

#define RDS_CHANNEL_VERSION "0.3.5"

#define if_err_ret(err, message, code)                                  \
  if ((err)) {                                                          \
    fprintf(stderr, "err (line=%i, file=%s): %s\n", __LINE__, __FILE__, \
            (message));                                                 \
    fflush(stderr);                                                     \
    return (code);                                                      \
  }

#define defargs(pos, def_value) (((pos) < argc) ? argv[(pos)] : (def_value))
#define defargi(pos, def_value) \
  (((pos) < argc) ? atoi(argv[(pos)]) : (def_value))
#define HEART_BEAT_INTERVAL 10
#define HEART_BEAT_CHANNEL "heartbeat"
typedef struct address_t {
  const char *host;  // IP or host name
  int port;          // Valid AF_INET port (0<port<65535)
} address_t;

typedef struct channel_t {
  address_t source_ad,
      target_ad;         // Addresses of source and target REDIS databases
  redisContext *source,  // Connection context to source REDIS database
      *notification,     // Same as source, but used for notifications listener
      *target,           // Connection context to target REDIS database
      *heartbeat;  // Connection context to source REDIS for pushing heartbeats
  const char *prefix;      // New prefix for each key
  size_t prefix_len;       // Len of prefix
  pthread_t heartbeat_th;  // Thread of heartbeating
} channel_t;

/**
* Start processing keys: create -> dump -> enable -> catch -> cleanup.
* Expects ch will be filled by zero
*/
int start_channel(channel_t *ch);

/**
* Clean all allocation resources during channel (automatically invoked by
* start_channel)
*/
void clean_up(channel_t *ch);

/**
* Open REDIS connection to remote side
**/
int create_source_connection(channel_t *ch);

/**
* Open REDIS connection to remote side for notification listener
*/
int create_notification_connection(channel_t *ch);

/**
* Open REDIS connection to remote side for heartbeats listener
*/
int create_heartbeat_connection(channel_t *ch);

/**
* Setup remote REDIS for keyevent space notification
*/
int enable_event_notifications(channel_t *ch);

/**
* Begin listen for events from remote REDIS
*/
int subscribe_for_events(channel_t *ch);

/**
* Open connection to target REDIS
**/
int create_target_connection(channel_t *ch);

/**
* Copy all keys from remote REDIS to local with new prefix for each key
**/
int dump_data(channel_t *ch);

/**
* Subscribe to any changes of remote REDIS and copy changed values
**/
int catch_notifications(channel_t *ch);

/**
* Start thread for heartbeats
*/
int start_heartbeats(channel_t *ch);

/**
* DUMP and restore single key
*/
int copy_key(redisContext *local, redisContext *remote, const char *key,
             size_t key_len, const char *prefix, size_t prefix_len);

/**
* Returns true if getReply success
*/
bool check_next_reply(redisContext *ctx, const char *err_msg);

/**
* Publish heartbeat to REDIS
*/
bool push_heartbeat(redisContext *ctx);

/**
* Thread routing for heartbeats
*/
void *heartbeat_routing(void *ctx);

int main(int argc, char **argv) {
  // 1 - host
  // 2 - port
  // 3 - host dest
  // 4 - port dest
  // 5 - prefix (optional)
  if (argc < 2) {
    fprintf(stderr,
            "usage: <host src | --version> [port-src = 6379] [host-dest = "
            "127.0.0.1] "
            "[port-dest = 6379] [prefix = '']\n");
    return 1;
  }
  if (strcmp(argv[1], "--version") == 0) {
    printf("%s\n", RDS_CHANNEL_VERSION);
    return 0;
  }
  const char *host = argv[1];
  int port = defargi(2, 6379);
  const char *local_host = defargs(3, "127.0.0.1");
  int local_port = defargi(4, 6379);
  const char *prefix = defargs(5, "");

  if (port <= 0 || port > 65535) {
    fprintf(stderr, "source port out of range\n");
    return 1;
  }
  if (local_port <= 0 || local_port > 65535) {
    fprintf(stderr, "destination port out of range\n");
    return 1;
  }

  printf("from: %s:%i\n", host, port);

  printf("  to: %s:%i\n", local_host, local_port);
  printf("pref: %s\n", prefix);
  fflush(stdout);
  channel_t ch;
  memset(&ch, 0, sizeof(ch));
  ch.source_ad.host = host;
  ch.source_ad.port = port;
  ch.target_ad.host = local_host;
  ch.target_ad.port = local_port;
  ch.prefix = prefix;
  ch.prefix_len = strlen(prefix);
  return start_channel(&ch);
}

int start_channel(channel_t *ch) {
  assert(ch->target_ad.host);
  assert(ch->source_ad.host);
  assert(ch->source_ad.port > 0 && ch->source_ad.port < 65535);
  assert(ch->target_ad.port > 0 && ch->target_ad.port < 65535);
  assert(ch->prefix || ch->prefix_len == 0);
  ch->source = NULL;
  ch->target = NULL;
  ch->notification = NULL;
  ch->heartbeat = NULL;
  bool ret = create_source_connection(ch) == 0 &&        //
             create_notification_connection(ch) == 0 &&  //
             create_target_connection(ch) == 0 &&        //
             create_heartbeat_connection(ch) == 0 &&     //
             enable_event_notifications(ch) == 0 &&      //
             subscribe_for_events(ch) == 0 &&            //
             dump_data(ch) == 0 &&                       //
             start_heartbeats(ch) == 0 &&                //
             catch_notifications(ch) == 0;               //
  clean_up(ch);
  return ret ? 0 : 1;
}

void clean_up(channel_t *ch) {
  pthread_cancel(ch->heartbeat_th);
  if (ch->source) redisFree(ch->source);
  if (ch->target) redisFree(ch->target);
  if (ch->notification) redisFree(ch->notification);
  if (ch->heartbeat) redisFree(ch->heartbeat);
  pthread_join(ch->heartbeat_th, NULL);
}

int create_source_connection(channel_t *ch) {
  redisContext *c = redisConnect(ch->source_ad.host, ch->source_ad.port);
  if_err_ret(!c, "failed allocate source redis context", -1);
  if_err_ret(c->err, c->errstr, -2);
  ch->source = c;
  return 0;
}

int create_notification_connection(channel_t *ch) {
  redisContext *c = redisConnect(ch->source_ad.host, ch->source_ad.port);
  if_err_ret(!c, "failed allocate notification redis context", -1);
  if_err_ret(c->err, c->errstr, -2);
  ch->notification = c;
  return 0;
}

int create_target_connection(channel_t *ch) {
  redisContext *c = redisConnect(ch->target_ad.host, ch->target_ad.port);
  if_err_ret(!c, "failed allocate target redis context", -1);
  if_err_ret(c->err, c->errstr, -2);
  ch->target = c;
  return 0;
}

int create_heartbeat_connection(channel_t *ch) {
  redisContext *c = redisConnect(ch->source_ad.host, ch->source_ad.port);
  if_err_ret(!c, "failed allocate heartbeat redis context", -1);
  if_err_ret(c->err, c->errstr, -2);
  ch->heartbeat = c;
  return 0;
}

void *heartbeat_routing(void *ctx) {
  redisContext *redis = (redisContext *)ctx;
  while (push_heartbeat(redis)) {
    sleep(HEART_BEAT_INTERVAL);
  }
  return NULL;
}

int copy_key(redisContext *local, redisContext *remote, const char *key,
             size_t key_len, const char *prefix, size_t prefix_len) {
  redisReply *src = redisCommand(remote, "DUMP %b", key, (size_t)key_len);
  if_err_ret(!src, remote->errstr, -30);
  int ret = 0;
  if (src->type == REDIS_REPLY_NIL) {
    fprintf(stderr, "key miss\n");
    fflush(stderr);
  } else {
    size_t temp_len = prefix_len + key_len;
    char *temp = (char *)malloc(temp_len + 1);
    memcpy(temp, prefix, prefix_len);
    memcpy(temp + prefix_len, key, key_len);
    temp[temp_len] = '\0';
    printf("------------------\n");
    printf("%s => %s\n", key, temp);
    printf("source k-len: %zu\n", key_len);
    printf("target k-len: %zu\n", temp_len);
    printf("    data-len: %zu\n", src->len);
    printf("        data:");
    for (size_t i = 0; i < src->len; ++i) {
      printf(" %02x", (unsigned char)src->str[i]);
    }
    printf("\n");
    redisAppendCommand(local, "MULTI");
    redisAppendCommand(local, "DEL %b", temp, temp_len);
    redisAppendCommand(local, "RESTORE %b 0 %b", temp, temp_len, src->str,
                       (size_t)src->len);
    redisAppendCommand(local, "EXEC");
    bool multi =
        check_next_reply(local, "failed open transaction on local side");
    bool del =
        check_next_reply(local, "failed remove target key on local side");
    bool rest =
        check_next_reply(local, "failed restore target data on local side");
    bool exec =
        check_next_reply(local, "failed finish transaction on local side");

    ret = (multi && del && rest && exec) ? 0 : 1;
    free(temp);
  }
  freeReplyObject(src);
  fflush(stdout);
  return ret;
}

int start_heartbeats(channel_t *ch) {
  return pthread_create(&(ch->heartbeat_th), NULL, heartbeat_routing,
                        (void *)(ch->heartbeat));
}

int dump_data(channel_t *ch) {
  int iterator = 0;
  int ret = 0;
  do {
    redisReply *reply = redisCommand(ch->source, "SCAN %i", iterator);
    if_err_ret(!reply, ch->source->errstr, -20);
    iterator = reply->element[0]->integer;
    printf("batch size: %zu\n", reply->element[1]->elements);
    fflush(stdout);
    for (size_t i = 0; i < reply->element[1]->elements; ++i) {
      const char *key = reply->element[1]->element[i]->str;
      size_t key_len = reply->element[1]->element[i]->len;
      if (copy_key(ch->target, ch->source,  //
                   key, key_len,            //
                   ch->prefix, ch->prefix_len) != 0) {
        break;
      }
    }
    freeReplyObject(reply);
  } while (iterator);
  return ret;
}

int enable_event_notifications(channel_t *ch) {
  redisReply *reply =
      redisCommand(ch->notification, "CONFIG SET notify-keyspace-events EA");
  if_err_ret(!reply, ch->notification->errstr, -50);
  freeReplyObject(reply);
  return 0;
}

int subscribe_for_events(channel_t *ch) {
  redisReply *reply = redisCommand(ch->notification, "PSUBSCRIBE __key*__:*");
  if_err_ret(!reply, ch->notification->errstr, -30);
  freeReplyObject(reply);
  reply = redisCommand(ch->notification, "PSUBSCRIBE %s", HEART_BEAT_CHANNEL);
  if_err_ret(!reply, ch->notification->errstr, -30);
  freeReplyObject(reply);
  return 0;
}

int catch_notifications(channel_t *ch) {
  struct timeval timeout = {HEART_BEAT_INTERVAL * 2, 0};
  if_err_ret(redisSetTimeout(ch->notification, timeout) != REDIS_OK,
             ch->notification->errstr, -20);
  redisReply *reply;
  int ret = 0;
  while (ret == 0) {
    reply = NULL;
    int reply_code = redisGetReply(ch->notification, (void **)&reply);
    if (reply_code == REDIS_ERR && ch->notification->err == REDIS_ERR_IO &&
        errno == EAGAIN) {
      puts("Heartbeat timeout");
      fflush(stdout);
      ret = 1;
    } else if (reply_code != REDIS_OK) {
      ret = 1;
    } else if (strcmp(reply->element[2]->str, HEART_BEAT_CHANNEL) == 0) {
      puts("Heartbeat catched");
      fflush(stdout);
    } else if (strcmp(rindex(reply->element[2]->str, ':') + 1, "del") != 0) {
      printf("catch %s\n", reply->element[3]->str);
      ret = copy_key(ch->target, ch->source,                          //
                     reply->element[3]->str, reply->element[3]->len,  //
                     ch->prefix, ch->prefix_len);
    }
    if (reply) freeReplyObject(reply);
  }
  return ret;
}

bool push_heartbeat(redisContext *conn) {
  redisReply *reply =
      redisCommand(conn, "PUBLISH %s \"1\"", HEART_BEAT_CHANNEL);
  if_err_ret(!reply, conn->errstr, false);
  freeReplyObject(reply);
  return true;
}

bool check_next_reply(redisContext *ctx, const char *err_msg) {
  assert(ctx);
  bool ret = true;
  redisReply *res;
  redisGetReply(ctx, (void **)&res);
  if (!res) {
    ret = false;
    fprintf(stderr, "err: %s: %s\n", err_msg, ctx->errstr);
    fflush(stderr);
  }
  freeReplyObject(res);
  return ret;
}
