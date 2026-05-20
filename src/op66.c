



INC      a        →  finish( mpn_add_1(out, view(a), 1),  n_a + 1 )
DEC      a        →  if a==0: 0  else finish( mpn_sub_1(out, view(a), 1), n_a )
ADD      a b      →  longer first; finish( mpn_add(out, va, vb),  max(na,nb)+1 )
SUB      a b      →  if a<b: throw/0  else finish( mpn_sub(out, va, vb), na )
MUL      a b      →  finish( mpn_mul(out, va, vb),  na + nb )
DIV      a b      →  if b==0: throw;  tdiv_qr(Q,R, va,vb);  finish(Q, na-nb+1)
MOD      a b      →  if b==0: throw;  tdiv_qr(Q,R, va,vb);  finish(R, nb)
BEX      n        →  out = zeroed(n/64 + 1); set bit n; finish(out, n/64+1)
                     (BEX n  ==  2^n)




LSH    a k   →  finish( mpn_lshift(out, va, k%64) , na + k/64 + 1 )   // Template B
               (whole-limb part k/64 = memmove; bit part k%64 = mpn_lshift)
RSH    a k   →  finish( mpn_rshift(out, va shifted down k/64, k%64), na - k/64 )

TEST   a i   →  (i/64 >= na) ? 0 : (limb[i/64] >> (i&63)) & 1            // Template A, returns bit
SET    a i   →  copy a; ensure i/64 limbs;  limb[i/64] |=  (1 << (i&63)); finish
CLEAR  a i   →  copy a;  if i/64 < na: limb[i/64] &= ~(1 << (i&63));      finish

TRUNC  a k   →  copy low k bits: keep k/64 limbs, mask top limb to k&63 bits; finish
TRUNC8/16/32/64  a  →  TRUNC with k = 8/16/32/64

BITS   a     →  a==0 ? 0 : (na-1)*64 + (64 - clz(top_limb))              // Template A
BYTES  a     →  (BITS(a) + 7) / 8
NIB    a     →  (BITS(a) + 3) / 4



LOAD8   a i      →  i >= byte_len(a) ? 0 : ((uint8_t*)limbs)[i]     // little-endian byte view
STORE8  a i v    →  copy a; ensure byte i exists; ((uint8_t*)limbs)[i] = v & 0xFF; finish
LOADVAR a i      →  load a machine-word-sized chunk at limb i (or 0 past end)



nat_cmp(a,b):                          // the ONE function
    if na != nb: return na < nb ? -1 : +1
    return mpn_cmp(va.limbs, vb.limbs, na)

CMP   a b  →  nat_cmp(a,b)              // -1 / 0 / +1   (often encoded 0/1/2)
EQ    a b  →  nat_cmp(a,b) == 0
NE    a b  →  nat_cmp(a,b) != 0
LT    a b  →  nat_cmp(a,b) <  0
LE    a b  →  nat_cmp(a,b) <= 0
GT    a b  →  nat_cmp(a,b) >  0
GE    a b  →  nat_cmp(a,b) >= 0
EQUAL a b  →  structural: same tag? recurse on children; nat case → nat_cmp==0


TYPE     x   →  x.header.tag                       // 0..3 for pin/law/app/nat
IS_PIN   x   →  tag(x) == PIN
IS_LAW   x   →  tag(x) == LAW
IS_APP   x   →  tag(x) == APP
IS_NAT   x   →  tag(x) == NAT
NAT      x   →  tag(x)==NAT ? x : 0                // coerce-to-nat
ARITY    x   →  PIN→arity(item); LAW→x.law_arity; APP→arity(hd)-1; NAT→ depends on ABI
NAME     x   →  LAW→x.law_name        else 0
BODY     x   →  LAW→x.law_body        else 0
HD       x   →  APP→x.fun             else x        // head of application
LAST     x   →  APP→x.arg             else x        // last argument
INIT     x   →  APP→x.fun             else x        // app minus its last arg
UNPIN    x   →  PIN→x.item            else x
NAT (op) x   →  same as IS-coerce above
-------------------------

SZ(row) = row.n_args
IX(row, i) = row.args[i]
HD(row) = row.fn

ROW    n x0..x(n-1)  →  alloc row of n; copy elems; return                // build
REP    n v           →  alloc row of n; fill all slots with v
SZ     r             →  r.row_len                                         // Template A
IX     r i           →  i < SZ(r) ? r.elem[i] : throw
IX0..IX7  r          →  IX with i = 0..7  (goto do_ix)
SLICE  r s e         →  alloc row of (e-s); copy r.elem[s..e]
WELD   r1 r2         →  alloc row of SZ(r1)+SZ(r2); copy both
UP     r v           →  alloc row of SZ(r)+1; copy r; append v             // push
UP_UNIQ r v          →  UP only if v not already in r (linear scan)
COUP   r             →  alloc row of SZ(r)-1; copy all but last            // pop



CASE   x b0..b(n-1)  →  k = nat_value(x);  k < n ? force(b_k) : throw
CASE2..CASE16        →  CASE with n = 2..16   (numbered variant — goto, don't reimplement)


NIL          →  push 0
TRUTH        →  push 1
OR    a b    →  is_zero(a) ? b : a              // short-circuit: nonzero wins
NOR   a b    →  is_zero(OR(a,b)) ? 1 : 0
AND   a b    →  is_zero(a) ? a : b
IF    c t f  →  is_zero(c) ? f : t
IFZ   c t f  →  is_zero(c) ? t : f              // IF with branches swapped



SEQ    a b       →  force(a); return b                       // force a, discard, yield b
SEQ2   a b c     →  force(a); force(b); return c
SEQ3   a b c d   →  force(a); force(b); force(c); return d
SAP    f x       →  return force( apply(f, x) )              // strict application
SAP2   f x y     →  return force( apply(apply(f,x), y) )
FORCE  x         →  return force(x)                          // WHNF
DEEPSEQ x        →  recursively force x and all children; return x
TRY    f         →  install handler; r = force(f); on throw → catch value; return r
THROW  v         →  unwind to nearest TRY with payload v