/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 * Copyright (C) Björn Stenberg, <bjorn@haxx.se>
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/

#include "curl_setup.h"

#ifndef CURL_DISABLE_MQTT

#include "urldata.h"
#include <curl/curl.h>
#include "transfer.h"
#include "sendf.h"
#include "progress.h"
#include "mqtt.h"
#include "select.h"
#include "strdup.h"
#include "url.h"
#include "escape.h"
#include "curlx/warnless.h"
#include "curl_printf.h"
#include "curl_memory.h"
#include "multiif.h"
#include "rand.h"

/* The last #include file should be: */
#include "memdebug.h"

/* first byte is command.
   second byte is for flags. */
#define MQTT_MSG_CONNECT    0x10
/* #define MQTT_MSG_CONNACK    0x20 */
#define MQTT_MSG_PUBLISH    0x30
#define MQTT_MSG_SUBSCRIBE  0x82
#define MQTT_MSG_SUBACK     0x90
#define MQTT_MSG_DISCONNECT 0xe0
#define MQTT_MSG_PINGREQ    0xC0
#define MQTT_MSG_PINGRESP   0xD0

#define MQTT_CONNACK_LEN 2
#define MQTT_SUBACK_LEN 3
#define MQTT_CLIENTID_LEN 12 /* "curl0123abcd" */

/* meta key for storing protocol meta at easy handle */
#define CURL_META_MQTT_EASY   "meta:proto:mqtt:easy"
/* meta key for storing protocol meta at connection */
#define CURL_META_MQTT_CONN   "meta:proto:mqtt:conn"

enum mqttstate {
  MQTT_FIRST,             /* 0 */
  MQTT_REMAINING_LENGTH,  /* 1 */
  MQTT_CONNACK,           /* 2 */
  MQTT_SUBACK,            /* 3 */
  MQTT_SUBACK_COMING,     /* 4 - the SUBACK remainder */
  MQTT_PUBWAIT,    /* 5 - wait for publish */
  MQTT_PUB_REMAIN,  /* 6 - wait for the remainder of the publish */

  MQTT_NOSTATE /* 7 - never used an actual state */
};

struct mqtt_conn {
  enum mqttstate state;
  enum mqttstate nextstate; /* switch to this after remaining length is
                               done */
  unsigned int packetid;
};

/* protocol-specific transfer-related data */
struct MQTT {
  struct dynbuf sendbuf;
  /* when receiving */
  struct dynbuf recvbuf;
  size_t npacket; /* byte counter */
  size_t remaining_length;
  unsigned char pkt_hd[4]; /* for decoding the arriving packet length */
  struct curltime lastTime; /* last time we sent or received data */
  unsigned char firstbyte;
  BIT(pingsent); /* 1 while we wait for ping response */
};


/*
 * Forward declarations.
 */

static CURLcode mqtt_do(struct Curl_easy *data, bool *done);
static CURLcode mqtt_done(struct Curl_easy *data,
                          CURLcode status, bool premature);
static CURLcode mqtt_doing(struct Curl_easy *data, bool *done);
static int mqtt_getsock(struct Curl_easy *data, struct connectdata *conn,
                        curl_socket_t *sock);
static CURLcode mqtt_setup_conn(struct Curl_easy *data,
                                struct connectdata *conn);

/*
 * MQTT protocol handler.
 */

const struct Curl_handler Curl_handler_mqtt = {
  "mqtt",                             /* scheme */
  mqtt_setup_conn,                    /* setup_connection */
  mqtt_do,                            /* do_it */
  mqtt_done,                          /* done */
  ZERO_NULL,                          /* do_more */
  ZERO_NULL,                          /* connect_it */
  ZERO_NULL,                          /* connecting */
  mqtt_doing,                         /* doing */
  ZERO_NULL,                          /* proto_getsock */
  mqtt_getsock,                       /* doing_getsock */
  ZERO_NULL,                          /* domore_getsock */
  ZERO_NULL,                          /* perform_getsock */
  ZERO_NULL,                          /* disconnect */
  ZERO_NULL,                          /* write_resp */
  ZERO_NULL,                          /* write_resp_hd */
  ZERO_NULL,                          /* connection_check */
  ZERO_NULL,                          /* attach connection */
  ZERO_NULL,                          /* follow */
  PORT_MQTT,                          /* defport */
  CURLPROTO_MQTT,                     /* protocol */
  CURLPROTO_MQTT,                     /* family */
  PROTOPT_NONE                        /* flags */
};

static void mqtt_easy_dtor(void *key, size_t klen, void *entry)
{
  struct MQTT *mq = entry;
  (void)key;
  (void)klen;
  curlx_dyn_free(&mq->sendbuf);
  curlx_dyn_free(&mq->recvbuf);
  free(mq);
}

static void mqtt_conn_dtor(void *key, size_t klen, void *entry)
{
  (void)key;
  (void)klen;
  free(entry);
}

static CURLcode mqtt_setup_conn(struct Curl_easy *data,
                                struct connectdata *conn)
{
  /* setup MQTT specific meta data at easy handle and connection */
  struct mqtt_conn *mqtt;
  struct MQTT *mq;

  mqtt = calloc(1, sizeof(*mqtt));
  if(!mqtt ||
     Curl_conn_meta_set(conn, CURL_META_MQTT_CONN, mqtt, mqtt_conn_dtor))
    return CURLE_OUT_OF_MEMORY;

  mq = calloc(1, sizeof(struct MQTT));
  if(!mq)
    return CURLE_OUT_OF_MEMORY;
  curlx_dyn_init(&mq->recvbuf, DYN_MQTT_RECV);
  curlx_dyn_init(&mq->sendbuf, DYN_MQTT_SEND);
  if(Curl_meta_set(data, CURL_META_MQTT_EASY, mq, mqtt_easy_dtor))
    return CURLE_OUT_OF_MEMORY;
  return CURLE_OK;
}

static CURLcode mqtt_send(struct Curl_easy *data,
                          const char *buf, size_t len)
{
  size_t n;
  CURLcode result;
  struct MQTT *mq = Curl_meta_get(data, CURL_META_MQTT_EASY);

  if(!mq)
    return CURLE_FAILED_INIT;

  result = Curl_xfer_send(data, buf, len, FALSE, &n);
  if(result)
    return result;
  mq->lastTime = curlx_now();
  Curl_debug(data, CURLINFO_HEADER_OUT, buf, (size_t)n);
  if(len != n) {
    size_t nsend = len - n;
    if(curlx_dyn_len(&mq->sendbuf)) {
      DEBUGASSERT(curlx_dyn_len(&mq->sendbuf) >= nsend);
      result = curlx_dyn_tail(&mq->sendbuf, nsend); /* keep this much */
    }
    else {
      result = curlx_dyn_addn(&mq->sendbuf, &buf[n], nsend);
    }
  }
  else
    curlx_dyn_reset(&mq->sendbuf);
  return result;
}

/* Generic function called by the multi interface to figure out what socket(s)
   to wait for and for what actions during the DOING and PROTOCONNECT
   states */
static int mqtt_getsock(struct Curl_easy *data,
                        struct connectdata *conn,
                        curl_socket_t *sock)
{
  (void)data;
  sock[0] = conn->sock[FIRSTSOCKET];
  return GETSOCK_READSOCK(FIRSTSOCKET);
}

static int mqtt_encode_len(char *buf, size_t len)
{
  int i;

  for(i = 0; (len > 0) && (i < 4); i++) {
    unsigned char encoded;
    encoded = len % 0x80;
    len /= 0x80;
    if(len)
      encoded |= 0x80;
    buf[i] = (char)encoded;
  }

  return i;
}

/* add the passwd to the CONNECT packet */
static int add_passwd(const char *passwd, const size_t plen,
                      char *pkt, const size_t start, int remain_pos)
{
  /* magic number that need to be set properly */
  const size_t conn_flags_pos = remain_pos + 8;
  if(plen > 0xffff)
    return 1;

  /* set password flag */
  pkt[conn_flags_pos] |= 0x40;

  /* length of password provided */
  pkt[start] = (char)((plen >> 8) & 0xFF);
  pkt[start + 1] = (char)(plen & 0xFF);
  memcpy(&pkt[start + 2], passwd, plen);
  return 0;
}

/* add user to the CONNECT packet */
static int add_user(const char *username, const size_t ulen,
                    unsigned char *pkt, const size_t start, int remain_pos)
{
  /* magic number that need to be set properly */
  const size_t conn_flags_pos = remain_pos + 8;
  if(ulen > 0xffff)
    return 1;

  /* set username flag */
  pkt[conn_flags_pos] |= 0x80;
  /* length of username provided */
  pkt[start] = (unsigned char)((ulen >> 8) & 0xFF);
  pkt[start + 1] = (unsigned char)(ulen & 0xFF);
  memcpy(&pkt[start + 2], username, ulen);
  return 0;
}

/* add client ID to the CONNECT packet */
static int add_client_id(const char *client_id, const size_t client_id_len,
                         char *pkt, const size_t start)
{
  if(client_id_len != MQTT_CLIENTID_LEN)
    return 1;
  pkt[start] = 0x00;
  pkt[start + 1] = MQTT_CLIENTID_LEN;
  memcpy(&pkt[start + 2], client_id, MQTT_CLIENTID_LEN);
  return 0;
}

/* Set initial values of CONNECT packet */
static int init_connpack(char *packet, char *remain, int remain_pos)
{
  /* Fixed header starts */
  /* packet type */
  packet[0] = MQTT_MSG_CONNECT;
  /* remaining length field */
  memcpy(&packet[1], remain, remain_pos);
  /* Fixed header ends */

  /* Variable header starts */
  /* protocol length */
  packet[remain_pos + 1] = 0x00;
  packet[remain_pos + 2] = 0x04;
  /* protocol name */
  packet[remain_pos + 3] = 'M';
  packet[remain_pos + 4] = 'Q';
  packet[remain_pos + 5] = 'T';
  packet[remain_pos + 6] = 'T';
  /* protocol level */
  packet[remain_pos + 7] = 0x04;
  /* CONNECT flag: CleanSession */
  packet[remain_pos + 8] = 0x02;
  /* keep-alive 0 = disabled */
  packet[remain_pos + 9] = 0x00;
  packet[remain_pos + 10] = 0x3c;
  /* end of variable header */
  return remain_pos + 10;
}

static CURLcode mqtt_connect(struct Curl_easy *data)
{
  CURLcode result = CURLE_OK;
  int pos = 0;
  int rc = 0;
  /* remain length */
  int remain_pos = 0;
  char remain[4] = {0};
  size_t packetlen = 0;
  size_t start_user = 0;
  size_t start_pwd = 0;
  char client_id[MQTT_CLIENTID_LEN + 1] = "curl";
  const size_t clen = strlen("curl");
  char *packet = NULL;

  /* extracting username from request */
  const char *username = data->state.aptr.user ?
    data->state.aptr.user : "";
  const size_t ulen = strlen(username);
  /* extracting password from request */
  const char *passwd = data->state.aptr.passwd ?
    data->state.aptr.passwd : "";
  const size_t plen = strlen(passwd);
  const size_t payloadlen = ulen + plen + MQTT_CLIENTID_LEN + 2 +
  /* The plus 2s below are for the MSB and LSB describing the length of the
     string to be added on the payload. Refer to spec 1.5.2 and 1.5.4 */
    (ulen ? 2 : 0) +
    (plen ? 2 : 0);

  /* getting how much occupy the remain length */
  remain_pos = mqtt_encode_len(remain, payloadlen + 10);

  /* 10 length of variable header and 1 the first byte of the fixed header */
  packetlen = payloadlen + 10 + remain_pos + 1;

  /* allocating packet */
  if(packetlen > 0xFFFFFFF)
    return CURLE_WEIRD_SERVER_REPLY;
  packet = calloc(1, packetlen);
  if(!packet)
    return CURLE_OUT_OF_MEMORY;

  /* set initial values for the CONNECT packet */
  pos = init_connpack(packet, remain, remain_pos);

  result = Curl_rand_alnum(data, (unsigned char *)&client_id[clen],
                           MQTT_CLIENTID_LEN - clen + 1);
  /* add client id */
  rc = add_client_id(client_id, strlen(client_id), packet, pos + 1);
  if(rc) {
    failf(data, "Client ID length mismatched: [%zu]", strlen(client_id));
    result = CURLE_WEIRD_SERVER_REPLY;
    goto end;
  }
  infof(data, "Using client id '%s'", client_id);

  /* position where the user payload starts */
  start_user = pos + 3 + MQTT_CLIENTID_LEN;
  /* position where the password payload starts */
  start_pwd = start_user + ulen;
  /* if username was provided, add it to the packet */
  if(ulen) {
    start_pwd += 2;

    rc = add_user(username, ulen,
                  (unsigned char *)packet, start_user, remain_pos);
    if(rc) {
      failf(data, "Username too long: [%zu]", ulen);
      result = CURLE_WEIRD_SERVER_REPLY;
      goto end;
    }
  }

  /* if passwd was provided, add it to the packet */
  if(plen) {
    rc = add_passwd(passwd, plen, packet, start_pwd, remain_pos);
    if(rc) {
      failf(data, "Password too long: [%zu]", plen);
      result = CURLE_WEIRD_SERVER_REPLY;
      goto end;
    }
  }

  if(!result)
    result = mqtt_send(data, packet, packetlen);

end:
  if(packet)
    free(packet);
  Curl_safefree(data->state.aptr.user);
  Curl_safefree(data->state.aptr.passwd);
  return result;
}

static CURLcode mqtt_disconnect(struct Curl_easy *data)
{
  return mqtt_send(data, "\xe0\x00", 2);
}

static CURLcode mqtt_recv_atleast(struct Curl_easy *data, size_t nbytes)
{
  struct MQTT *mq = Curl_meta_get(data, CURL_META_MQTT_EASY);
  size_t rlen;
  CURLcode result;

  if(!mq)
    return CURLE_FAILED_INIT;
  rlen = curlx_dyn_len(&mq->recvbuf);

  if(rlen < nbytes) {
    unsigned char readbuf[1024];
    size_t nread;

    DEBUGASSERT(nbytes - rlen < sizeof(readbuf));
    result = Curl_xfer_recv(data, (char *)readbuf, nbytes - rlen, &nread);
    if(result)
      return result;
    if(curlx_dyn_addn(&mq->recvbuf, readbuf, nread))
      return CURLE_OUT_OF_MEMORY;
    rlen = curlx_dyn_len(&mq->recvbuf);
  }
  return (rlen >= nbytes) ? CURLE_OK : CURLE_AGAIN;
}

static void mqtt_recv_consume(struct Curl_easy *data, size_t nbytes)
{
  struct MQTT *mq = Curl_meta_get(data, CURL_META_MQTT_EASY);
  DEBUGASSERT(mq);
  if(mq) {
    size_t rlen = curlx_dyn_len(&mq->recvbuf);
    if(rlen <= nbytes)
      curlx_dyn_reset(&mq->recvbuf);
    else
      curlx_dyn_tail(&mq->recvbuf, rlen - nbytes);
  }
}

static CURLcode mqtt_verify_connack(struct Curl_easy *data)
{
  struct MQTT *mq = Curl_meta_get(data, CURL_META_MQTT_EASY);
  CURLcode result;
  char *ptr;

  DEBUGASSERT(mq);
  if(!mq)
    return CURLE_FAILED_INIT;

  result = mqtt_recv_atleast(data, MQTT_CONNACK_LEN);
  if(result)
    goto fail;

  /* verify CONNACK */
  DEBUGASSERT(curlx_dyn_len(&mq->recvbuf) >= MQTT_CONNACK_LEN);
  ptr = curlx_dyn_ptr(&mq->recvbuf);
  Curl_debug(data, CURLINFO_HEADER_IN, ptr, MQTT_CONNACK_LEN);

  if(ptr[0] != 0x00 || ptr[1] != 0x00) {
    failf(data, "Expected %02x%02x but got %02x%02x",
          0x00, 0x00, ptr[0], ptr[1]);
    curlx_dyn_reset(&mq->recvbuf);
    result = CURLE_WEIRD_SERVER_REPLY;
    goto fail;
  }
  mqtt_recv_consume(data, MQTT_CONNACK_LEN);
fail:
  return result;
}

static CURLcode mqtt_get_topic(struct Curl_easy *data,
                               char **topic, size_t *topiclen)
{
  char *path = data->state.up.path;
  CURLcode result = CURLE_URL_MALFORMAT;
  if(strlen(path) > 1) {
    result = Curl_urldecode(path + 1, 0, topic, topiclen, REJECT_NADA);
    if(!result && (*topiclen > 0xffff)) {
      failf(data, "Too long MQTT topic");
      result = CURLE_URL_MALFORMAT;
    }
  }
  else
    failf(data, "No MQTT topic found. Forgot to URL encode it?");

  return result;
}

static CURLcode mqtt_subscribe(struct Curl_easy *data)
{
  CURLcode result = CURLE_OK;
  char *topic = NULL;
  size_t topiclen;
  unsigned char *packet = NULL;
  size_t packetlen;
  char encodedsize[4];
  size_t n;
  struct connectdata *conn = data->conn;
  struct mqtt_conn *mqtt = Curl_conn_meta_get(conn, CURL_META_MQTT_CONN);

  if(!mqtt)
    return CURLE_FAILED_INIT;

  result = mqtt_get_topic(data, &topic, &topiclen);
  if(result)
    goto fail;

  mqtt->packetid++;

  packetlen = topiclen + 5; /* packetid + topic (has a two byte length field)
                               + 2 bytes topic length + QoS byte */
  n = mqtt_encode_len((char *)encodedsize, packetlen);
  packetlen += n + 1; /* add one for the control packet type byte */

  packet = malloc(packetlen);
  if(!packet) {
    result = CURLE_OUT_OF_MEMORY;
    goto fail;
  }

  packet[0] = MQTT_MSG_SUBSCRIBE;
  memcpy(&packet[1], encodedsize, n);
  packet[1 + n] = (mqtt->packetid >> 8) & 0xff;
  packet[2 + n] = mqtt->packetid & 0xff;
  packet[3 + n] = (topiclen >> 8) & 0xff;
  packet[4 + n ] = topiclen & 0xff;
  memcpy(&packet[5 + n], topic, topiclen);
  packet[5 + n + topiclen] = 0; /* QoS zero */

  result = mqtt_send(data, (const char *)packet, packetlen);

fail:
  free(topic);
  free(packet);
  return result;
}

/*
 * Called when the first byte was already read.
 */
static CURLcode mqtt_verify_suback(struct Curl_easy *data)
{
  struct MQTT *mq = Curl_meta_get(data, CURL_META_MQTT_EASY);
  struct connectdata *conn = data->conn;
  struct mqtt_conn *mqtt = Curl_conn_meta_get(conn, CURL_META_MQTT_CONN);
  CURLcode result;
  char *ptr;

  if(!mqtt || !mq)
    return CURLE_FAILED_INIT;

  result = mqtt_recv_atleast(data, MQTT_SUBACK_LEN);
  if(result)
    goto fail;

  /* verify SUBACK */
  DEBUGASSERT(curlx_dyn_len(&mq->recvbuf) >= MQTT_SUBACK_LEN);
  ptr = curlx_dyn_ptr(&mq->recvbuf);
  Curl_debug(data, CURLINFO_HEADER_IN, ptr, MQTT_SUBACK_LEN);

  if(((unsigned char)ptr[0]) != ((mqtt->packetid >> 8) & 0xff) ||
     ((unsigned char)ptr[1]) != (mqtt->packetid & 0xff) ||
     ptr[2] != 0x00) {
    curlx_dyn_reset(&mq->recvbuf);
    result = CURLE_WEIRD_SERVER_REPLY;
    goto fail;
  }
  mqtt_recv_consume(data, MQTT_SUBACK_LEN);
fail:
  return result;
}

static CURLcode mqtt_publish(struct Curl_easy *data)
{
  CURLcode result;
  char *payload = data->set.postfields;
  size_t payloadlen;
  char *topic = NULL;
  size_t topiclen;
  unsigned char *pkt = NULL;
  size_t i = 0;
  size_t remaininglength;
  size_t encodelen;
  char encodedbytes[4];
  curl_off_t postfieldsize = data->set.postfieldsize;

  if(!payload) {
    DEBUGF(infof(data, "mqtt_publish without payload, return bad arg"));
    return CURLE_BAD_FUNCTION_ARGUMENT;
  }
  if(postfieldsize < 0)
    payloadlen = strlen(payload);
  else
    payloadlen = (size_t)postfieldsize;

  result = mqtt_get_topic(data, &topic, &topiclen);
  if(result)
    goto fail;

  remaininglength = payloadlen + 2 + topiclen;
  encodelen = mqtt_encode_len(encodedbytes, remaininglength);

  /* add the control byte and the encoded remaining length */
  pkt = malloc(remaininglength + 1 + encodelen);
  if(!pkt) {
    result = CURLE_OUT_OF_MEMORY;
    goto fail;
  }

  /* assemble packet */
  pkt[i++] = MQTT_MSG_PUBLISH;
  memcpy(&pkt[i], encodedbytes, encodelen);
  i += encodelen;
  pkt[i++] = (topiclen >> 8) & 0xff;
  pkt[i++] = (topiclen & 0xff);
  memcpy(&pkt[i], topic, topiclen);
  i += topiclen;
  memcpy(&pkt[i], payload, payloadlen);
  i += payloadlen;
  result = mqtt_send(data, (const char *)pkt, i);

fail:
  free(pkt);
  free(topic);
  return result;
}

static size_t mqtt_decode_len(unsigned char *buf,
                              size_t buflen, size_t *lenbytes)
{
  size_t len = 0;
  size_t mult = 1;
  size_t i;
  unsigned char encoded = 128;

  for(i = 0; (i < buflen) && (encoded & 128); i++) {
    encoded = buf[i];
    len += (encoded & 127) * mult;
    mult *= 128;
  }

  if(lenbytes)
    *lenbytes = i;

  return len;
}

#ifdef DEBUGBUILD
static const char *statenames[]={
  "MQTT_FIRST",
  "MQTT_REMAINING_LENGTH",
  "MQTT_CONNACK",
  "MQTT_SUBACK",
  "MQTT_SUBACK_COMING",
  "MQTT_PUBWAIT",
  "MQTT_PUB_REMAIN",

  "NOT A STATE"
};
#endif

/* The only way to change state */
static void mqstate(struct Curl_easy *data,
                    enum mqttstate state,
                    enum mqttstate nextstate) /* used if state == FIRST */
{
  struct connectdata *conn = data->conn;
  struct mqtt_conn *mqtt = Curl_conn_meta_get(conn, CURL_META_MQTT_CONN);
  DEBUGASSERT(mqtt);
  if(!mqtt)
    return;
#ifdef DEBUGBUILD
  infof(data, "%s (from %s) (next is %s)",
        statenames[state],
        statenames[mqtt->state],
        (state == MQTT_FIRST) ? statenames[nextstate] : "");
#endif
  mqtt->state = state;
  if(state == MQTT_FIRST)
    mqtt->nextstate = nextstate;
}


static CURLcode mqtt_read_publish(struct Curl_easy *data, bool *done)
{
  CURLcode result = CURLE_OK;
  struct connectdata *conn = data->conn;
  size_t nread;
  size_t remlen;
  struct mqtt_conn *mqtt = Curl_conn_meta_get(conn, CURL_META_MQTT_CONN);
  struct MQTT *mq = Curl_meta_get(data, CURL_META_MQTT_EASY);
  unsigned char packet;

  DEBUGASSERT(mqtt);
  if(!mqtt || !mq)
    return CURLE_FAILED_INIT;

  switch(mqtt->state) {
MQTT_SUBACK_COMING:
  case MQTT_SUBACK_COMING:
    result = mqtt_verify_suback(data);
    if(result)
      break;

    mqstate(data, MQTT_FIRST, MQTT_PUBWAIT);
    break;

  case MQTT_SUBACK:
  case MQTT_PUBWAIT:
    /* we are expecting PUBLISH or SUBACK */
    packet = mq->firstbyte & 0xf0;
    if(packet == MQTT_MSG_PUBLISH)
      mqstate(data, MQTT_PUB_REMAIN, MQTT_NOSTATE);
    else if(packet == MQTT_MSG_SUBACK) {
      mqstate(data, MQTT_SUBACK_COMING, MQTT_NOSTATE);
      goto MQTT_SUBACK_COMING;
    }
    else if(packet == MQTT_MSG_DISCONNECT) {
      infof(data, "Got DISCONNECT");
      *done = TRUE;
      goto end;
    }
    else {
      result = CURLE_WEIRD_SERVER_REPLY;
      goto end;
    }

    /* -- switched state -- */
    remlen = mq->remaining_length;
    infof(data, "Remaining length: %zu bytes", remlen);
    if(data->set.max_filesize &&
       (curl_off_t)remlen > data->set.max_filesize) {
      failf(data, "Maximum file size exceeded");
      result = CURLE_FILESIZE_EXCEEDED;
      goto end;
    }
    Curl_pgrsSetDownloadSize(data, remlen);
    data->req.bytecount = 0;
    data->req.size = remlen;
    mq->npacket = remlen; /* get this many bytes */
    FALLTHROUGH();
  case MQTT_PUB_REMAIN: {
    /* read rest of packet, but no more. Cap to buffer size */
    char buffer[4*1024];
    size_t rest = mq->npacket;
    if(rest > sizeof(buffer))
      rest = sizeof(buffer);
    result = Curl_xfer_recv(data, buffer, rest, &nread);
    if(result) {
      if(CURLE_AGAIN == result) {
        infof(data, "EEEE AAAAGAIN");
      }
      goto end;
    }
    if(!nread) {
      infof(data, "server disconnected");
      result = CURLE_PARTIAL_FILE;
      goto end;
    }

    /* we received something */
    mq->lastTime = curlx_now();

    /* if QoS is set, message contains packet id */
    result = Curl_client_write(data, CLIENTWRITE_BODY, buffer, nread);
    if(result)
      goto end;

    mq->npacket -= nread;
    if(!mq->npacket)
      /* no more PUBLISH payload, back to subscribe wait state */
      mqstate(data, MQTT_FIRST, MQTT_PUBWAIT);
    break;
  }
  default:
    DEBUGASSERT(NULL); /* illegal state */
    result = CURLE_WEIRD_SERVER_REPLY;
    goto end;
  }
end:
  return result;
}

static CURLcode mqtt_do(struct Curl_easy *data, bool *done)
{
  struct MQTT *mq = Curl_meta_get(data, CURL_META_MQTT_EASY);
  CURLcode result = CURLE_OK;
  *done = FALSE; /* unconditionally */

  if(!mq)
    return CURLE_FAILED_INIT;
  mq->lastTime = curlx_now();
  mq->pingsent = FALSE;

  result = mqtt_connect(data);
  if(result) {
    failf(data, "Error %d sending MQTT CONNECT request", result);
    return result;
  }
  mqstate(data, MQTT_FIRST, MQTT_CONNACK);
  return CURLE_OK;
}

static CURLcode mqtt_done(struct Curl_easy *data,
                          CURLcode status, bool premature)
{
  struct MQTT *mq = Curl_meta_get(data, CURL_META_MQTT_EASY);
  (void)status;
  (void)premature;
  if(mq) {
    curlx_dyn_free(&mq->sendbuf);
    curlx_dyn_free(&mq->recvbuf);
  }
  return CURLE_OK;
}

/* we ping regularly to avoid being disconnected by the server */
static CURLcode mqtt_ping(struct Curl_easy *data)
{
  struct MQTT *mq = Curl_meta_get(data, CURL_META_MQTT_EASY);
  CURLcode result = CURLE_OK;
  struct connectdata *conn = data->conn;
  struct mqtt_conn *mqtt = Curl_conn_meta_get(conn, CURL_META_MQTT_CONN);

  if(!mqtt || !mq)
    return CURLE_FAILED_INIT;

  if(mqtt->state == MQTT_FIRST &&
     !mq->pingsent &&
     data->set.upkeep_interval_ms > 0) {
    struct curltime t = curlx_now();
    timediff_t diff = curlx_timediff(t, mq->lastTime);

    if(diff > data->set.upkeep_interval_ms) {
      /* 0xC0 is PINGREQ, and 0x00 is remaining length */
      unsigned char packet[2] = { 0xC0, 0x00 };
      size_t packetlen = sizeof(packet);

      result = mqtt_send(data, (char *)packet, packetlen);
      if(!result) {
        mq->pingsent = TRUE;
      }
      infof(data, "mqtt_ping: sent ping request.");
    }
  }
  return result;
}

static CURLcode mqtt_doing(struct Curl_easy *data, bool *done)
{
  struct MQTT *mq = Curl_meta_get(data, CURL_META_MQTT_EASY);
  CURLcode result = CURLE_OK;
  size_t nread;
  unsigned char recvbyte;
  struct mqtt_conn *mqtt = Curl_conn_meta_get(data->conn, CURL_META_MQTT_CONN);

  if(!mqtt || !mq)
    return CURLE_FAILED_INIT;

  *done = FALSE;

  if(curlx_dyn_len(&mq->sendbuf)) {
    /* send the remainder of an outgoing packet */
    result = mqtt_send(data, curlx_dyn_ptr(&mq->sendbuf),
                       curlx_dyn_len(&mq->sendbuf));
    if(result)
      return result;
  }

  result = mqtt_ping(data);
  if(result)
    return result;

  infof(data, "mqtt_doing: state [%d]", (int) mqtt->state);
  switch(mqtt->state) {
  case MQTT_FIRST:
    /* Read the initial byte only */
    result = Curl_xfer_recv(data, (char *)&mq->firstbyte, 1, &nread);
    if(result)
      break;
    else if(!nread) {
      failf(data, "Connection disconnected");
      *done = TRUE;
      result = CURLE_RECV_ERROR;
      break;
    }
    Curl_debug(data, CURLINFO_HEADER_IN, (const char *)&mq->firstbyte, 1);

    /* we received something */
    mq->lastTime = curlx_now();

    /* remember the first byte */
    mq->npacket = 0;
    mqstate(data, MQTT_REMAINING_LENGTH, MQTT_NOSTATE);
    FALLTHROUGH();
  case MQTT_REMAINING_LENGTH:
    do {
      result = Curl_xfer_recv(data, (char *)&recvbyte, 1, &nread);
      if(result || !nread)
        break;
      Curl_debug(data, CURLINFO_HEADER_IN, (const char *)&recvbyte, 1);
      mq->pkt_hd[mq->npacket++] = recvbyte;
    } while((recvbyte & 0x80) && (mq->npacket < 4));
    if(!result && nread && (recvbyte & 0x80))
      /* MQTT supports up to 127 * 128^0 + 127 * 128^1 + 127 * 128^2 +
         127 * 128^3 bytes. server tried to send more */
      result = CURLE_WEIRD_SERVER_REPLY;
    if(result)
      break;
    mq->remaining_length = mqtt_decode_len(mq->pkt_hd, mq->npacket, NULL);
    mq->npacket = 0;
    if(mq->remaining_length) {
      mqstate(data, mqtt->nextstate, MQTT_NOSTATE);
      break;
    }
    mqstate(data, MQTT_FIRST, MQTT_FIRST);

    if(mq->firstbyte == MQTT_MSG_DISCONNECT) {
      infof(data, "Got DISCONNECT");
      *done = TRUE;
    }

    /* ping response */
    if(mq->firstbyte == MQTT_MSG_PINGRESP) {
      infof(data, "Received ping response.");
      mq->pingsent = FALSE;
      mqstate(data, MQTT_FIRST, MQTT_PUBWAIT);
    }
    break;
  case MQTT_CONNACK:
    result = mqtt_verify_connack(data);
    if(result)
      break;

    if(data->state.httpreq == HTTPREQ_POST) {
      result = mqtt_publish(data);
      if(!result) {
        result = mqtt_disconnect(data);
        *done = TRUE;
      }
      mqtt->nextstate = MQTT_FIRST;
    }
    else {
      result = mqtt_subscribe(data);
      if(!result) {
        mqstate(data, MQTT_FIRST, MQTT_SUBACK);
      }
    }
    break;

  case MQTT_SUBACK:
  case MQTT_PUBWAIT:
  case MQTT_PUB_REMAIN:
    result = mqtt_read_publish(data, done);
    break;

  default:
    failf(data, "State not handled yet");
    *done = TRUE;
    break;
  }

  if(result == CURLE_AGAIN)
    result = CURLE_OK;
  return result;
}

#endif /* CURL_DISABLE_MQTT */
