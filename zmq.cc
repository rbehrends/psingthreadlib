#include <iostream>
#include "kernel/mod2.h"
#include "Singular/ipid.h"
#include "Singular/links/silink.h"
#include <zmq.h>
#include <cstring>
#include <errno.h>

extern si_link_extension si_link_root;

enum ZmqSocketStatus { zsDisconnected, zsBound, zsConnected };

static int zmqSocketID;
static void *zmqContext;

struct ZmqSocket {
  void *socket;
  ZmqSocketStatus status;
};

void zmqInitRuntime() {
  zmqContext = zmq_init(2);
}

#define MAX_POLL 1024

BOOLEAN zmqPoll(leftv result, leftv arg) {
  zmq_pollitem_t ev[MAX_POLL];
  si_link links[MAX_POLL];
  short modes[MAX_POLL];
  short mode = ZMQ_POLLIN;
  int n = 0;
  while (arg != NULL) {
    si_link l;
    if (n == MAX_POLL) {
      WerrorS("ZeroMQ: Too many links to poll");
      return TRUE;
    }
    if (arg->Typ() != LINK_CMD) {
      if (arg->Typ() != STRING_CMD) {
	WerrorS("ZeroMQ: trying to poll a non-link");
	return TRUE;
      }
      char *s = (char *)(arg->Data());
      if (strcmp(s, "r") == 0)
        mode = ZMQ_POLLIN;
      else if (strcmp(s, "w") == 0)
        mode = ZMQ_POLLOUT;
      else if (strcmp(s, "rw") == 0)
        mode = ZMQ_POLLIN|ZMQ_POLLOUT;
      else {
	WerrorS("ZeroMQ: invalid poll mode");
	return TRUE;
      }
      arg = arg->next;
      continue;
    }
    l = (si_link)(arg->Data());
    if (strcmp(l->m->type, "zmq") != 0) {
      WerrorS("ZeroMQ: trying to poll a non-zmq link");
      return TRUE;
    }
    if (!SI_LINK_OPEN_P(l)) {
      WerrorS("ZeroMQ: trying to poll a closed link");
      return TRUE;
    }
    arg = arg->next;
    modes[n] = mode;
    links[n++] = l;
  }
  for (int i=0; i<n; i++) {
    ev[i].socket = ((ZmqSocket *)(links[i]->data))->socket;
    ev[i].fd = -1;
    ev[i].events = modes[i];
  }
  if (zmq_poll(ev, n, -1) < 0) {
    WerrorS("ZeroMQ: zmq_poll() experienced an internal error");
    return TRUE;
  }
  for (int i=0; i<n; i++) {
    if (ev[i].events & ev[i].revents) {
      result->rtyp = INT_CMD;
      result->data = (char *)(long)(i);
      return FALSE;
    }
  }
  WerrorS("ZeroMQ: zmq_poll() experienced an internal error");
  return TRUE;
}

static char *prefix(char *arg, const char *str) {
  while (*str) {
    if (*arg != *str)
      return NULL;
    arg++; str++;
  }
  if (*arg++ != ':')
    return NULL;
  return arg;
}

BOOLEAN zmqLinkOpen(si_link l, short flag, leftv u) {
  char *arg = l->name;
  char *tail;
  bool connect;
  void *socket;
  ZmqSocket *zsocket;
  int type;
  const char *mode;
  if (tail = prefix(arg, "push"))
    type = ZMQ_PUSH;
  else if (tail = prefix(arg, "pull"))
    type = ZMQ_PULL;
  else if (tail = prefix(arg, "dealer"))
    type = ZMQ_DEALER;
  else if (tail = prefix(arg, "router"))
    type = ZMQ_ROUTER;
  else if (tail = prefix(arg, "req"))
    type = ZMQ_REQ;
  else if (tail = prefix(arg, "rep"))
    type = ZMQ_REP;
  else if (tail = prefix(arg, "pub"))
    type = ZMQ_PUB;
  else if (tail = prefix(arg, "sub"))
    type = ZMQ_SUB;
  else {
    Werror("ZeroMQ: Invalid socket type: '%s'", arg);
    return TRUE;
  }
  connect = (*tail == '+');
  if (connect)
    tail++;
  socket = zmq_socket(zmqContext, type);
  if (!socket) {
    Werror("ZeroMQ: Cannot create socket: '%s'", arg);
    return TRUE;
  }
  if (connect) {
    if (zmq_connect(socket, tail) < 0) {
      zmq_close(socket);
      Werror("ZeroMQ: Cannot connect to address: '%s'", tail);
      return TRUE;
    }
  } else {
    if (zmq_bind(socket, tail) < 0) {
      zmq_close(socket);
      Werror("ZeroMQ: Cannot bind to address: '%s'", tail);
      return TRUE;
    }
  }
  zsocket = (ZmqSocket *)omAlloc(sizeof(ZmqSocket));
  zsocket->socket = socket;
  zsocket->status = connect ? zsConnected : zsBound;
  l->data = (void *)zsocket;
  switch (type) {
    case ZMQ_PULL:
    case ZMQ_SUB:
      SI_LINK_SET_R_OPEN_P(l);
      break;
    case ZMQ_PUSH:
    case ZMQ_PUB:
      SI_LINK_SET_W_OPEN_P(l);
      break;
    default:
      SI_LINK_SET_RW_OPEN_P(l);
      break;
  }
  return FALSE;
}

BOOLEAN zmqLinkClose(si_link l) {
  ZmqSocket *zsocket = (ZmqSocket *)l->data;
  if (zsocket->status != zsDisconnected)
    zmq_close(zsocket->socket);
  omFreeSize((ADDRESS) zsocket, sizeof(ZmqSocket));
  SI_LINK_SET_CLOSE_P(l);
  return FALSE;
}

static leftv zmqRecvStr(void *socket) {
  zmq_msg_t msg;
  zmq_msg_init(&msg);
  restart:
  if (zmq_recvmsg(socket, &msg, 0) < 0) {
    if (zmq_errno() == EINTR)
      goto restart;
    WerrorS("ZeroMQ: recv failed");
    return NULL;
  }
  void *contents = zmq_msg_data(&msg);
  size_t size = zmq_msg_size(&msg);
  char *buf = (char *)omAlloc(size+1);
  memcpy(buf, (char *)contents, size);
  buf[size] = '\0';
  zmq_msg_close(&msg);
  leftv result = (leftv)omAlloc0Bin(sleftv_bin);
  result->rtyp = STRING_CMD;
  result->data = buf;
  return result;
}

static bool zmqHasMore(void *socket) {
  int64_t more = 0;
  size_t more_size = sizeof(more);
  zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);
  return more != 0;
}

struct LinkedList {
  LinkedList *next;
  leftv value;
};

leftv zmqLinkRead(si_link l) {
  ZmqSocket *zsocket = (ZmqSocket *)l->data;
  LinkedList *head, *tail;
  int n;
  void *socket = zsocket->socket;
  leftv v = zmqRecvStr(socket);
  if (!v)
    return NULL;
  if (!zmqHasMore(socket)) {
    return v;
  }
  head = (LinkedList *)omAlloc(sizeof(LinkedList));
  head->next = NULL;
  head->value = v;
  tail = head;
  n = 1;
  do {
    v = zmqRecvStr(socket);
    if (!v) break;
    n++;
    tail->next = (LinkedList *) omAlloc(sizeof(LinkedList));
    tail = tail->next;
    tail->next = NULL;
    tail->value = v;
  } while (zmqHasMore(socket));
  lists L = (lists)omAllocBin(slists_bin);
  L->Init(n);
  tail = head;
  for (int i=0; i<n; i++) {
    L->m[i].data = (tail->value->Data());
    L->m[i].rtyp = (tail->value->Typ());
    omFreeBin(tail->value, sleftv_bin);
    tail = tail->next;
    omFree(head);
    head = tail;
  }
  leftv result = (leftv) omAlloc0Bin(sleftv_bin);
  result->rtyp = LIST_CMD;
  result->data = (void *)L;
}

static BOOLEAN zmqSendStr(void *socket, char *str, int flags) {
  size_t len = strlen(str);
  zmq_msg_t msg;
  zmq_msg_init_size(&msg, len);
  memcpy(zmq_msg_data(&msg), str, len);
  if (zmq_sendmsg(socket, &msg, flags) < 0) {
    WerrorS("ZeroMQ: send failed");
    return TRUE;
  }
  zmq_msg_close(&msg);
  return FALSE;
}

BOOLEAN zmqLinkWrite(si_link l, leftv arg) {
  ZmqSocket *zsocket = (ZmqSocket *)l->data;
  void *socket = zsocket->socket;
  while (arg) {
    if (arg->Typ() == LIST_CMD) {
      lists l = (lists)(arg->Data());
      int n = l->nr+1;
      for (int i=0; i<n; i++) {
	if (l->m[i].Typ() != STRING_CMD) {
	  WerrorS("ZeroMQ: writing a non-string to a socket");
	  return TRUE;
	}
      }
      for (int i=0; i<n; i++) {
        char *str = (char *)(l->m[i].Data());
	int more = ZMQ_SNDMORE;
	if (i == n-1) more = 0;
	if (zmqSendStr(socket, str, (i==n-1) ? 0 : ZMQ_SNDMORE))
	  return TRUE;
      }
    } else if (arg->Typ() == STRING_CMD) {
      char *str = (char *)(arg->Data());
      if (zmqSendStr(socket, str, 0))
        return TRUE;
    } else {
      WerrorS("ZeroMQ: writing a non-string to a socket");
      return TRUE;
    }
    arg = arg->next;
  }
  return FALSE;
}

leftv zmqLinkRead2(si_link l, leftv count) {
  ZmqSocket *zsocket = (ZmqSocket *)l->data;
  if (count->Typ() != INT_CMD) {
    WerrorS("ZeroMQ: read(sock, n) requires second argument to be an integer");
    return NULL;
  }
  return NULL;
}

static const char *zmqStatus(si_link l, short mode) {
  zmq_pollitem_t ev;
  ev.socket = ((ZmqSocket *)(l->data))->socket;
  ev.fd = -1;
  ev.events = mode;
  if (zmq_poll(&ev, 1, 0)< 0)
    return "not ready";
  if (ev.events & ev.revents)
    return "ready";
  return "not ready";
}

const char *zmqLinkStatus(si_link l, const char *req) {
  if (strcmp(req, "read") == 0) {
    if (!SI_LINK_R_OPEN_P(l))
      return "not ready";
    return zmqStatus(l, ZMQ_POLLIN);
  } else if (strcmp(req, "write") == 0) {
    if (!SI_LINK_W_OPEN_P(l))
      return "not ready";
    return zmqStatus(l, ZMQ_POLLOUT);
  }
  return "unknown or unimplemented status request";
}

void zmqLinkInit() {
  si_link_extension s;
  s = (si_link_extension)omAlloc0Bin(s_si_link_extension_bin);
  s->Open = zmqLinkOpen;
  s->Close = zmqLinkClose;
  s->Kill = NULL;
  s->Read = zmqLinkRead;
  s->Read2 = zmqLinkRead2;
  s->Write = zmqLinkWrite;
  s->Dump = NULL;
  s->GetDump = NULL;
  s->Status = zmqLinkStatus;
  s->type = "zmq";
  s->next = si_link_root;
  si_link_root->next = s;
}

extern "C" int mod_init(SModulFunctions *fn)
{
  const char *libname = currPack->libname;
  if (!libname) libname = "";

  zmqInitRuntime();
  zmqLinkInit();

  // fn->iiAddCproc(libname, "zmq_socket", FALSE, zmqSocket);
  // fn->iiAddCproc(libname, "zmq_bind", FALSE, zmqBind);
  // fn->iiAddCproc(libname, "zmq_connect", FALSE, zmqConnect);
  // fn->iiAddCproc(libname, "zmq_close", FALSE, zmqClose);
  // fn->iiAddCproc(libname, "zmq_send", FALSE, zmqSend);
  // fn->iiAddCproc(libname, "zmq_recv", FALSE, zmqRecv);
  // fn->iiAddCproc(libname, "zmq_recv_list", FALSE, zmqRecvList);
  fn->iiAddCproc(libname, "zmq_poll", FALSE, zmqPoll);
  return 0;
}
