#include <iostream>
#include "kernel/mod2.h"
#include "Singular/ipid.h"
#include "Singular/links/silink.h"
#include "Singular/lists.h"
#include "Singular/blackbox.h"
#include <cstring>
#include <errno.h>
#include <vector>
#include <map>
#include <iterator>

using namespace std;

namespace { // putting the implementation inside an anonymous namespace

vector<map<string, string>*> tables;
map<string, long> tableindices;

int table_id;

void *table_init(blackbox *b) {
  return omAlloc0(sizeof(long));
}

void *new_table(long index) {
  void *result = omAlloc0(sizeof(long));
  *(long *)result = index;
  return result;
}

void table_destroy(blackbox *b, void *d) {
}

void *table_copy(blackbox *b, void *d) {
  void *result = table_init(b);
  *(long *)result = *(long *)d;
  return result;
}

BOOLEAN table_assign(leftv l, leftv r) {
  if (r->Typ() == l->Typ()) {
    if (l->rtyp == IDHDL) {
      omFree(IDDATA((idhdl)l->data));
      IDDATA((idhdl)l->data) = (char*)table_copy(NULL,r->Data());
    } else {
      leftv ll=l->LData();
      if (ll==NULL)
      {
	return TRUE; // out of array bounds or similiar
      }
      omFree(ll->data);
      ll->data = table_copy(NULL,r->Data());
    }
  }
  return FALSE;
}

char *table_string(blackbox *b, void *d) {
  char buf[32];
  sprintf(buf, "<atomic table #%ld>", *(long *)d);
  return omStrDup(buf);
}

BOOLEAN makeTable(leftv result, leftv arg) {
  long index = 0;
  if (arg == NULL || arg->next != NULL) {
    WerrorS("makeTable: wrong number of arguments");
    return TRUE;
  }
  if (arg->Typ() != STRING_CMD) {
    WerrorS("makeTable: not a valid URI");
    return TRUE;
  }
  string s = (char *)(arg->Data());
  if (tableindices.count(s)) {
    index = tableindices[s];
  } else {
    tables.push_back(new map<string, string>());
    tableindices.insert(make_pair(s, tables.size()-1));
    index = tables.size() - 1;
  }
  result->rtyp = table_id;
  result->data = (char *)(new_table(index));
  return FALSE;
}

BOOLEAN getTable(leftv result, leftv arg) {
  if (arg == NULL || arg->next == NULL || arg->next->next != NULL) {
    WerrorS("getTable: wrong number of arguments");
    return TRUE;
  }
  if (arg->Typ() != table_id) {
    WerrorS("getTable: not a valid table");
    return TRUE;
  }
  if (arg->next->Typ() != STRING_CMD) {
    WerrorS("getTable: not a valid table key");
    return TRUE;
  }
  long index = * (long *) (arg->Data());
  string key = (char *)(arg->next->Data());
  if (index < 0 || index >= tables.size()) {
    WerrorS("getTable: not a valid table ID");
    return TRUE;
  }
  map<string, string> *m = tables[index];
  if (m->count(key)) {
    result->rtyp = STRING_CMD;
    result->data = omStrDup(m->at(key).c_str());
    return FALSE;
  } else {
    WerrorS("getTable: key not found");
    return TRUE;
  }
}

BOOLEAN inTable(leftv result, leftv arg) {
  if (arg == NULL || arg->next == NULL || arg->next->next != NULL) {
    WerrorS("inTable: wrong number of arguments");
    return TRUE;
  }
  if (arg->Typ() != table_id) {
    WerrorS("inTable: not a valid table");
    return TRUE;
  }
  if (arg->next->Typ() != STRING_CMD) {
    WerrorS("inTable: not a valid table key");
    return TRUE;
  }
  long index = * (long *) (arg->Data());
  string key = (char *)(arg->next->Data());
  if (index < 0 || index >= tables.size()) {
    WerrorS("inTable: not a valid table ID");
    return TRUE;
  }
  map<string, string> *m = tables[index];
  result->rtyp = INT_CMD;
  result->data = (char *)(long)(m->count(key));
  return FALSE;
}

BOOLEAN putTable(leftv result, leftv arg) {
  if (arg == NULL || arg->next == NULL || arg->next->next == NULL
      || arg->next->next->next != NULL) {
    WerrorS("putTable: wrong number of arguments");
    return TRUE;
  }
  if (arg->Typ() != table_id) {
    WerrorS("putTable: not a valid table ID");
    return TRUE;
  }
  if (arg->next->Typ() != STRING_CMD) {
    WerrorS("putTable: not a valid table key");
    return TRUE;
  }
  if (arg->next->next->Typ() != STRING_CMD) {
    WerrorS("putTable: not a valid table value");
    return TRUE;
  }
  long index = * (long *) (arg->Data());
  string key = (char *)(arg->next->Data());
  string value = (char *)(arg->next->next->Data());
  if (index < 0 || index >= tables.size()) {
    WerrorS("putTable: not a valid table ID");
    return TRUE;
  }
  map<string, string> *m = tables[index];
  m->erase(key);
  m->insert(make_pair(key, value));
  result->rtyp = NONE;
  return FALSE;
}

}


extern "C" int mod_init(SModulFunctions *fn)
{
  const char *libname = currPack->libname;
  if (!libname) libname = "";

  blackbox *b=(blackbox*)omAlloc0(sizeof(blackbox));
  b->blackbox_Init = table_init;
  b->blackbox_destroy = table_destroy;
  b->blackbox_Copy = table_copy;
  b->blackbox_String = table_string;
  b->blackbox_Assign = table_assign;
  table_id = setBlackboxStuff(b, "atomic_table");

  // fn->iiAddCproc(libname, "zmq_socket", FALSE, zmqSocket);
  // fn->iiAddCproc(libname, "zmq_bind", FALSE, zmqBind);
  // fn->iiAddCproc(libname, "zmq_connect", FALSE, zmqConnect);
  // fn->iiAddCproc(libname, "zmq_close", FALSE, zmqClose);
  // fn->iiAddCproc(libname, "zmq_send", FALSE, zmqSend);
  // fn->iiAddCproc(libname, "zmq_recv", FALSE, zmqRecv);
  // fn->iiAddCproc(libname, "zmq_recv_list", FALSE, zmqRecvList);
  fn->iiAddCproc(libname, "putTable", FALSE, putTable);
  fn->iiAddCproc(libname, "getTable", FALSE, getTable);
  fn->iiAddCproc(libname, "inTable", FALSE, inTable);
  fn->iiAddCproc(libname, "makeAtomicTable", FALSE, makeTable);
  return MAX_TOK;
}
