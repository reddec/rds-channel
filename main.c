#include <stdio.h>
#include <stdlib.h>
#include <hiredis.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdbool.h>

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
/**
* Open REDIS connection to remote side then open local connection.
**/
int open_remote_connection(const char *local_host, int local_port,
                           const char *remote_host, int remote_port,
                           const char *prefix, size_t prefix_len);

/**
* Setup remote REDIS for keyevent space notification
*/
int enable_event_notifications(redisContext *remote);

/**
* Open connection to local REDIS then dump data and then catch notification of
*any changes
**/
int open_local_connection(redisContext *remote, redisContext *remote2,
                          const char *local_host, int local_port,
                          const char *prefix, size_t prefix_len);
/**
* Copy all keys from remote REDIS to local with new prefix for each key
**/
int dump_data(redisContext *local_ctx, redisContext *remote_ctx,
              const char *prefix, size_t prefix_len);

/**
* Subscribe to any changes of remote REDIS and copy changed values
**/
int catch_notifications(redisContext *local_ctx, redisContext *remote_ctx,
                        redisContext *remote2_ctx, const char *prefix,
                        size_t prefix_len);

/**
* DUMP and restore single key
*/
int copy_key(redisContext *local, redisContext *remote, const char *key,
             size_t key_len, const char *prefix, size_t prefix_len);

/**
* Returns true if getReply success
*/
bool check_next_reply(redisContext *ctx, const char *err_msg);

int main(int argc, char **argv) {
  // 1 - host
  // 2 - port
  // 3 - host dest
  // 4 - port dest
  // 5 - prefix (optional)
  if (argc < 2) {
    fprintf(stderr,
            "usage: <host src> [port-src = 6379] [host-dest = 127.0.0.1] "
            "[port-dest = 6379] [prefix = '']\n");
    return 1;
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
  return open_remote_connection(local_host, local_port, host, port, prefix,
                                strlen(prefix));
}

int open_remote_connection(const char *local_host, int local_port,
                           const char *remote_host, int remote_port,
                           const char *prefix, size_t prefix_len) {
  redisContext *c = redisConnect(remote_host, remote_port);
  if_err_ret(!c, "failed allocate redis context", -1);
  if_err_ret(c->err, c->errstr, -2);
  redisContext *c2 = redisConnect(remote_host, remote_port);
  if (!c2 || c2->err) {
    redisFree(c);
    return -1;
  }
  int ret =
      open_local_connection(c, c2, local_host, local_port, prefix, prefix_len);
  redisFree(c);
  return ret;
}

int open_local_connection(redisContext *remote, redisContext *remote2,
                          const char *host, int port, const char *prefix,
                          size_t prefix_len) {
  assert(remote);
  assert(remote2);
  assert(remote != remote2);
  redisContext *lc = redisConnect(host, port);
  if_err_ret(!lc, "failed allocate local redis context", -1);
  if_err_ret(lc->err, lc->errstr, -2);
  int ret = dump_data(lc, remote, prefix, prefix_len);
  if (ret == 0) {
    ret = enable_event_notifications(remote);
  }
  if (ret == 0) {
    ret = catch_notifications(lc, remote, remote2, prefix, prefix_len);
  }
  redisFree(lc);
  return ret;
}

int copy_key(redisContext *local, redisContext *remote, const char *key,
             size_t key_len, const char *prefix, size_t prefix_len) {
  assert(local);
  assert(remote);
  assert(local != remote);
  assert(!local->err);
  assert(!remote->err);
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
  return ret;
}

int dump_data(redisContext *local, redisContext *remote, const char *prefix,
              size_t prefix_len) {
  int iterator = 0;
  int ret = 0;
  assert(local);
  assert(remote);
  assert(local != remote);
  assert(prefix || prefix_len == 0);
  do {
    redisReply *reply = redisCommand(remote, "SCAN %i", iterator);
    if_err_ret(!reply, remote->errstr, -20);
    iterator = reply->element[0]->integer;
    printf("batch size: %zu\n", reply->element[1]->elements);
    for (size_t i = 0; i < reply->element[1]->elements; ++i) {
      const char *key = reply->element[1]->element[i]->str;
      size_t key_len = reply->element[1]->element[i]->len;
      if (copy_key(local, remote, key, key_len, prefix, prefix_len) != 0) {
        break;
      }
    }
    freeReplyObject(reply);
  } while (iterator);
  return ret;
}

int enable_event_notifications(redisContext *remote) {
  redisReply *reply =
      redisCommand(remote, "CONFIG SET notify-keyspace-events EA");
  if_err_ret(!reply, remote->errstr, -50);
  freeReplyObject(reply);
  return 0;
}

int catch_notifications(redisContext *local, redisContext *remote,
                        redisContext *remote2, const char *prefix,
                        size_t prefix_len) {
  assert(local);
  assert(remote);
  assert(remote2);
  assert(local != remote);
  assert(remote != remote2);
  redisReply *reply = redisCommand(remote, "PSUBSCRIBE __key*__:*");
  if_err_ret(!reply, remote->errstr, -30);
  freeReplyObject(reply);
  int ret = 0;
  while (ret == 0 && redisGetReply(remote, (void **)&reply) == REDIS_OK) {
    // consume message
    printf("catch %s\n", reply->element[3]->str);
    if (strcmp(rindex(reply->element[2]->str, ':') + 1, "del") != 0) {
      ret = copy_key(local, remote2, reply->element[3]->str,
                     reply->element[3]->len, prefix, prefix_len);
    }
    freeReplyObject(reply);
  }
  return 1;
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
