#include "lintree.h"

#define TRANSEXT_PRIVATES 1 /* allow access to transext internals */

#include <kernel/mod2.h>

#include <omalloc/omalloc.h>

#include <misc/intvec.h>
#include <misc/options.h>

#include <reporter/si_signals.h>
#include <reporter/s_buff.h>

#include <coeffs/bigintmat.h>
#include <coeffs/longrat.h>

#include <polys/monomials/ring.h>
#include <polys/monomials/p_polys.h>
#include <polys/ext_fields/transext.h>
#include <polys/simpleideals.h>
#include <polys/matpol.h>

#include <kernel/oswrapper/timer.h>
#include <kernel/oswrapper/feread.h>
#include <kernel/oswrapper/rlimit.h>

#include <Singular/tok.h>
#include <Singular/ipid.h>
#include <Singular/ipshell.h>
#include <Singular/subexpr.h>
#include <Singular/links/silink.h>
#include <Singular/cntrlc.h>
#include <Singular/lists.h>
#include <Singular/blackbox.h>

namespace LinTree {

using namespace std;

vector<LinTreeEncodeFunc> encoders;
vector<LinTreeDecodeFunc> decoders;
vector<LinTreeRefFunc> refupdaters;
vector<char> needs_ring;

void install(int typ,
  LinTreeEncodeFunc enc,
  LinTreeDecodeFunc dec,
  LinTreeRefFunc ref)
{
  size_t n;
  for (;;) {
    n = encoders.size();
    if (n > typ) break;
    if (n == 0)
      n = 256;
    else
      n = n * 2;
    encoders.resize(n);
    decoders.resize(n);
    refupdaters.resize(n);
    needs_ring.resize(n);
  }
  encoders[typ] = enc;
  decoders[typ] = dec;
  refupdaters[typ] = ref;
}

void set_needs_ring(int typ) {
  needs_ring[typ] = 1;
}

void encode(LinTree &lintree, leftv val) {
  void encode_ring(LinTree &lintree, const ring r);
  int typ = val->Typ();
  char enc_ring = 0;
  LinTreeEncodeFunc fn;
  if (typ < encoders.size()) {
    fn = encoders[typ];
    enc_ring = needs_ring[typ];
  }
  else
    fn = NULL;
  if (fn) {
    if (enc_ring && !lintree.has_last_ring()) {
      lintree.put_int(-1);
      encode_ring(lintree, currRing);
      lintree.set_last_ring(currRing);
    }
    lintree.put_int(typ);
    fn(lintree, val);
  } else
    lintree.mark_error("trying to share unsupported data type");
}

leftv decode(LinTree &lintree) {
  ring decode_ring_raw(LinTree &lintree);
  int typ = lintree.get_int();
  if (typ < 0) {
    lintree.set_last_ring(decode_ring_raw(lintree));
    typ = lintree.get_int();
  }
  LinTreeDecodeFunc fn = decoders[typ];
  return fn(lintree);
}

void updateref(LinTree &lintree, int by) {
  int typ = lintree.get_int();
  LinTreeRefFunc fn = refupdaters[typ];
  fn(lintree, by);
}

leftv new_leftv(int code, void *data) {
  leftv result = (leftv) omAlloc0(sizeof(sleftv));
  result->rtyp = code;
  result->data = (char *)data;
  return result;
}

leftv new_leftv(int code, long data) {
  leftv result = (leftv) omAlloc0(sizeof(sleftv));
  result->rtyp = code;
  result->data = (char *)data;
  return result;
}

// INT_CMD

void encode_int(LinTree &lintree, leftv val) {
  long data = (long)(val->data);
  lintree.put(data);
}

leftv decode_int(LinTree &lintree) {
  long data = lintree.get<long>();
  return new_leftv(INT_CMD, data);
}

void ref_int(LinTree &lintree, int by) {
  lintree.skip<long>();
}

// STRING_CMD

void encode_string(LinTree &lintree, leftv val) {
  char *p = (char *)val->data;
  size_t len = strlen(p);
  lintree.put(len);
  lintree.put_bytes(p, len);
}

leftv decode_string(LinTree &lintree) {
  size_t len = lintree.get<size_t>();
  const char *p = lintree.get_bytes(len);
  leftv result = new_leftv(STRING_CMD, NULL);
  result->data = omAlloc0(len+1);
  memcpy(result->data, p, len);
  return result;
}

void ref_string(LinTree &lintree, int by) {
  size_t len = lintree.get<size_t>();
  lintree.skip_bytes(len);
}

// NUMBER_CMD

void encode_number_cf(LinTree &lintree, const number n, const coeffs cf) {
  void encode_poly(LinTree &lintree, int typ, poly p, const ring r);
  n_coeffType ct = getCoeffType(cf);
  // lintree.put_int((int)ct);
  switch (ct) {
    case n_transExt:
      {
      fraction f= (fraction) n;
      encode_poly(lintree, POLY_CMD, NUM(f), cf->extRing);
      encode_poly(lintree, POLY_CMD, DEN(f), cf->extRing);
      }
      break;
    case n_algExt:
      encode_poly(lintree, POLY_CMD, (poly) n, cf->extRing);
      break;
    case n_Zp:
      lintree.put<long>((long) n);
      break;
    default:
      lintree.mark_error("coefficient type not supported");
      break;
  }
}

number decode_number_cf(LinTree &lintree, const coeffs cf) {
  poly decode_poly(LinTree &lintree, const ring r);
  n_coeffType ct = getCoeffType(cf);
  switch (ct) {
    case n_transExt:
      {
      fraction f= (fraction) n_Init(1, cf);
      NUM(f) = decode_poly(lintree, cf->extRing);
      DEN(f) = decode_poly(lintree, cf->extRing);
      return (number) f;
      }
    case n_algExt:
      return (number) decode_poly(lintree, cf->extRing);
    case n_Zp:
      return (number) (lintree.get<long>());
    default:
      lintree.mark_error("coefficient type not supported");
      return NULL;
  }
}

leftv decode_number(LinTree &lintree) {
  return new_leftv(NUMBER_CMD,
    decode_number_cf(lintree, ((ring)lintree.get_last_ring())->cf));
}

void encode_number(LinTree &lintree, leftv val) {
  encode_number_cf(lintree, (number)val->Data(),
    ((ring) lintree.get_last_ring())->cf);
}

void ref_number(LinTree &lintree, int by) {
  void ref_poly(LinTree &lintree, int by);
  coeffs cf = ((ring) lintree.get_last_ring())->cf;
  switch (getCoeffType(cf)) {
    case n_transExt:
      ref_poly(lintree, by);
      ref_poly(lintree, by);
      break;
    case n_algExt:
      ref_poly(lintree, by);
      break;
    case n_Zp:
      lintree.skip<long>();
      break;
    default:
      abort(); // should never happen
      break;
  }
}

// POLY_CMD

void encode_poly(LinTree &lintree, int typ, poly p, const ring r) {
  lintree.put_int(pLength(p));
  while (p != NULL) {
    encode_number_cf(lintree, pGetCoeff(p), r->cf);
    lintree.put_int(p_GetComp(p, r));
    for (int j=1; j<=rVar(r); j++) {
      lintree.put_int(p_GetExp(p, j, r));
    }
    pIter(p);
  }
}

void encode_poly(LinTree &lintree, int typ, poly p) {
  encode_poly(lintree, typ, p, (ring) lintree.get_last_ring());
}

void encode_poly(LinTree &lintree, leftv val) {
  encode_poly(lintree, val->Typ(), (poly) val->Data());
}

poly decode_poly(LinTree &lintree, const ring r) {
  int len = lintree.get_int();
  poly p;
  poly ret = NULL;
  poly prev = NULL;
  for (int l = 0; l < len; l++) {
    p = p_Init(r);
    pSetCoeff0(p, decode_number_cf(lintree, r->cf));
    int d;
    d = lintree.get_int();
    p_SetComp(p, d, r);
    for(int i=1;i<=rVar(r);i++)
    {
      d=lintree.get_int();
      p_SetExp(p,i,d,r);
    }
    p_Setm(p,r);
    p_Test(p,r);
    if (ret==NULL) ret=p;
    else           pNext(prev)=p;
    prev=p;
  }
  return ret;
}

leftv decode_poly(LinTree &lintree) {
  ring r = (ring) lintree.get_last_ring();
  return new_leftv(POLY_CMD, decode_poly(lintree, r));
}

void ref_poly(LinTree &lintree, int by) {
  ring r = (ring) lintree.get_last_ring();
  int len = lintree.get_int();
  for (int l = 0; l <len; l++) {
    ref_number(lintree, by);
    lintree.skip_int();
    for (int i=1; i<=rVar(r); i++)
      lintree.skip_int();
  }
}

// IDEAL_CMD
ideal decode_ideal(LinTree &lintree, int typ, const ring r) {
  int n = lintree.get_int();
  ideal I = idInit(n, 1);
  for (int i=0; i<IDELEMS(I); i++)
    I->m[i] = decode_poly(lintree, r);
  return I;
}

void encode_ideal(LinTree &lintree, int typ, const ideal I, const ring R) {
  matrix M = (matrix) I;
  int mn;
  if (typ == MATRIX_CMD)
  {
    mn = MATROWS(M) * MATCOLS(M);
    lintree.put_int(MATROWS(M));
    lintree.put_int(MATCOLS(M));
  } else {
    mn = IDELEMS(I);
    lintree.put_int(IDELEMS(I));
  }
  int tt;
  if (typ == MODUL_CMD)
    tt = VECTOR_CMD;
  else
    tt = POLY_CMD;
  for (int i=0; i<mn; i++)
    encode_poly(lintree, tt, I->m[i], R);
}

void encode_ideal(LinTree &lintree, int typ, const ideal I) {
  encode_ideal(lintree, typ, I, (ring) lintree.get_last_ring());
}

void encode_ideal(LinTree &lintree, leftv val) {
  void encode_ring(LinTree &lintree, const ring r);
  int typ = val->Typ();
  void *data = val->Data();
  encode_ring(lintree, currRing);
  switch (typ) {
    case IDEAL_CMD:
    case MATRIX_CMD:
      break;
    case MODUL_CMD:
      {
	ideal M = (ideal) data;
	lintree.put_int((int) M->rank);
      }
      break;
  }
  encode_ideal(lintree, typ, (ideal) data);
}


// RING_CMD

void encode_ring(LinTree &lintree, const ring r) {
  if (r == NULL) {
    lintree.put_int(-4);
    return;
  }
  if (r == lintree.get_last_ring()) {
    lintree.put_int(-5);
    return;
  }
  if (rField_is_Q(r) || rField_is_Zp(r)) {
    lintree.put_int(n_GetChar(r->cf));
    lintree.put_int(r->N);
  } else if (rFieldType(r) == n_transExt) {
    lintree.put_int(-1);
    lintree.put_int(r->N);
  } else if (rFieldType(r) == n_algExt) {
    lintree.put_int(-2);
    lintree.put_int(r->N);
  } else {
    lintree.put_int(-3);
    lintree.put_int(r->N);
    lintree.put_cstring(nCoeffName(r->cf));
  }
  for (int i=0; i<r->N; i++) {
    lintree.put_cstring(r->names[i]);
  }
  int i = 0;
  if (r->order) while (r->order[i] != 0) i++;
  lintree.put_int(i);
  i = 0;
  if (r->order) while (r->order[i] != 0) {
    lintree.put_int(r->order[i]);
    lintree.put_int(r->block0[i]);
    lintree.put_int(r->block1[i]);
    switch (r->order[i]) {
      case ringorder_a:
      case ringorder_wp:
      case ringorder_Wp:
      case ringorder_ws:
      case ringorder_Ws:
      case ringorder_aa:
	for (int j = r->block0[i]; j <= r->block1[i]; j++)
	  lintree.put_int(r->wvhdl[i][j-r->block0[i]]);
	break;
      case ringorder_a64:
      case ringorder_M:
      case ringorder_L:
      case ringorder_IS:
        lintree.mark_error("ring order not implemented");
	break;
      default:
        break;
    }
    i++;
  }
  if (rFieldType(r) == n_transExt || rFieldType(r) == n_algExt) {
    encode_ring(lintree, r->cf->extRing);
  }
  if (r->qideal) {
    lintree.put_int(IDEAL_CMD);
    encode_ideal(lintree, IDEAL_CMD, r->qideal, r);
  } else {
    lintree.put_int(0);
  }
}

void encode_ring(LinTree &lintree, leftv val) {
  encode_ring(lintree, (ring) val->Data());
}

ring decode_ring_raw(LinTree &lintree) {
  int ch = lintree.get_int();
  int N = lintree.get_int();
  coeffs cf = NULL;
  char **names = NULL;
  switch (ch) {
    case -1:
    case -2:
    case -3:
      {
	const char *cf_name = lintree.get_cstring();
	cf = nFindCoeffByName(cf_name);
	if (cf == NULL) {
	  lintree.mark_error("cannot find coeffient name");
	  return NULL;
	}
      }
      break;
    case -4:
      return NULL;
      break;
    case -5:
      return (ring) lintree.get_last_ring();
      break;
  }
  if (N != 0) {
    names = (char **)omAlloc(N*sizeof(char *));
    for (int i=0; i<N; i++)
      names[i] = omStrDup(lintree.get_cstring());
  }
  int num_ord;
  num_ord = lintree.get_int();
  int *ord = (int *)omAlloc0((num_ord + 1)*sizeof(int));
  int *block0 = (int *)omAlloc0((num_ord + 1)*sizeof(int));
  int *block1 = (int *)omAlloc0((num_ord + 1)*sizeof(int));
  int **wvhdl = (int **)omAlloc0((num_ord + 1)*sizeof(int*));
  for (int i=0; i<num_ord; i++)
  {
    ord[i] = lintree.get_int();
    block0[i] = lintree.get_int();
    block1[i] = lintree.get_int();
    switch (ord[i]) {
      case ringorder_a:
      case ringorder_wp:
      case ringorder_Wp:
      case ringorder_ws:
      case ringorder_Ws:
      case ringorder_aa:
        wvhdl[i] = (int *)omAlloc((block1[i]-block0[i]+1)*sizeof(int));
	for (int j=block0[i]; j<=block1[i]; j++)
	  wvhdl[i][j-block0[i]] = lintree.get_int();
	break;
      case ringorder_a64:
      case ringorder_M:
      case ringorder_L:
      case ringorder_IS:
        lintree.mark_error("ring oder not implemented for lintrees");
        break;
      default:
        break;
    }
  }
  if (N == 0) {
    omFree(ord);
    omFree(block0);
    omFree(block1);
    omFree(wvhdl);
    return NULL;
  } else {
    ring r = NULL;
    if (ch >= 0)
      r = rDefault(ch, N, names, num_ord, ord, block0, block1, wvhdl);
    else if (ch == -1) {
      TransExtInfo T;
      T.r = decode_ring_raw(lintree);
      if (T.r == NULL) return NULL;
      cf = nInitChar(n_transExt, &T);
      r = rDefault(cf, N, names, num_ord, ord, block0, block1, wvhdl);
    } else if (ch == -2) {
      TransExtInfo T;
      T.r = decode_ring_raw(lintree);
      if (T.r == NULL) return NULL;
      cf = nInitChar(n_algExt, &T);
      r = rDefault(cf, N, names, num_ord, ord, block0, block1, wvhdl);
    } else if (ch == -3) {
      r = rDefault(cf, N, names, num_ord, ord, block0, block1, wvhdl);
    }
    lintree.set_last_ring(r);
    if (lintree.get_int()) {
      r->qideal = decode_ideal(lintree, IDEAL_CMD, r);
    }
    return r;
  }
}

leftv decode_ring(LinTree &lintree) {
  return new_leftv(RING_CMD, decode_ring_raw(lintree));
}

void ref_ring(LinTree &lintree, int by) {
  int ch = lintree.get_int();
  int N = lintree.get_int();
  switch (ch) {
    case -3:
      lintree.skip_cstring();
    case -4:
    case -5:
      return;
  }
  for (int i=0; i<N; i++)
    lintree.skip_cstring();
  int num_ord = lintree.get_int();
  for (int i=0; i<N; i++) {
    int ord = lintree.get_int();
    int block0 = lintree.get_int();
    int block1 = lintree.get_int();
    switch (ord) {
      case ringorder_a:
      case ringorder_wp:
      case ringorder_Wp:
      case ringorder_ws:
      case ringorder_Ws:
      case ringorder_aa:
	for (int j=block0; j<=block1; j++)
	  lintree.skip_int();
	break;
    }
  }
  if (N != 0) {
    if (ch == -1 || ch == -2) {
      ref_ring(lintree, by);
    }
  }
}

// LIST_CMD

void encode_list(LinTree &lintree, leftv val) {
  lists l = (lists) val->Data();
  int n = lSize(l);
  lintree.put_int(n);
  for (int i=0; i<=n; i++) {
    encode(lintree, &l->m[i]);
  }
}

leftv decode_list(LinTree &lintree) {
  int n = lintree.get_int();
  lists l = (lists)omAlloc(sizeof(*l));
  l->Init(n+1);
  for (int i=0; i<=n; i++) {
    leftv val = decode(lintree);
    memcpy(&l->m[i], val, sizeof(*val));
    omFreeBin(val, sleftv_bin);
  }
  return new_leftv(LIST_CMD, l);
}

void ref_list(LinTree &lintree, int by) {
  int n = lintree.get_int();
  for (int i = 0; i < n; i++) {
    updateref(lintree, by);
  }
}

void dump_string(string str) {
  printf("%d: ", (int)str.size());
  for (int i=0; i<str.size(); i++) {
    char ch = str[i];
    if (ch < ' ' || ch >= 0x7f)
      printf("#%02x", (int) ch & 0xff);
    else
      printf("%c", ch);
  }
  printf("\n");
  fflush(stdout);
}

std::string to_string(leftv val) {
  LinTree lintree;
  encode(lintree, val);
  return lintree.to_string();
}

leftv from_string(std::string &str) {
  LinTree lintree(str);
  return decode(lintree);
}

void init() {
  install(INT_CMD, encode_int, decode_int, ref_int);
  install(LIST_CMD, encode_list, decode_list, ref_list);
  install(STRING_CMD, encode_string, decode_string, ref_string);
  install(NUMBER_CMD, encode_number, decode_number, ref_number);
  set_needs_ring(NUMBER_CMD);
  install(RING_CMD, encode_ring, decode_ring, ref_ring);
  install(POLY_CMD, encode_poly, decode_poly, ref_poly);
  set_needs_ring(POLY_CMD);
}

LinTree::LinTree() : cursor(0), memory(), error(NULL), last_ring(NULL) {
}

LinTree::LinTree(std::string &source) :
  cursor(0), memory(source), error(NULL), last_ring(NULL) {
}

void LinTree::set_last_ring(void *r) {
  if (last_ring)
    rKill((ring) last_ring);
  last_ring = r;
  if (r) ((ring) r)->ref++;
}

LinTree::~LinTree() {
  if (last_ring)
    rKill((ring) last_ring);
}

}
