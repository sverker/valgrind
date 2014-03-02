
/*--------------------------------------------------------------------*/
/*--- A Valgrind tool for memory debugging.              mh_main.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is copied from Lackey, an example Valgrind tool that does
   some simple program measurement and tracing.

   Copyright (C) 2002-2010 Nicholas Nethercote
      njn@valgrind.org

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

// This code was initially forked from lackey/lk_main.c

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_debuginfo.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_options.h"
#include "pub_tool_machine.h"     // VG_(fnptr_to_fnentry)
#include "pub_tool_execontext.h"
#include "pub_tool_threadstate.h"
#include "pub_tool_mallocfree.h"

#include "memhist.h"  // client requests
#include "rb_tree.h"

#define MH_DEBUG

#ifdef MH_DEBUG
#  define MH_ASSERT tl_assert
#  define MH_ASSERT2 tl_assert2
#else
#  define MH_ASSERT(C)
#  define MH_ASSERT2(C, FMT, ARGS...)
#endif


/*------------------------------------------------------------*/
/*--- Command line options                                 ---*/
/*------------------------------------------------------------*/

/* Command line options controlling instrumentation kinds, as described at
 * the top of this file. */
static Bool clo_track_mem       = True;
static Bool clo_trace_mem       = False;

static Bool mh_process_cmd_line_option(const HChar* arg)
{
   if VG_BOOL_CLO(arg, "--trace-mem", clo_trace_mem) {}
   else
      return False;

   return True;
}

static void mh_print_usage(void)
{
   VG_(printf)(
"    --trace-mem=no|yes        trace all stores [no]\n"
   );
}

static void mh_print_debug_usage(void)
{
   VG_(printf)("    (none)\n");
}


#ifdef MH_DEBUG
static Bool fit_in_ubytes(ULong v, Int nbytes)
{
    return nbytes >= sizeof(ULong) || (v >> (nbytes*8)) == 0;
}
#endif


/*
 * Memory access tracking
 */

#define MAX_DSIZE    512


struct mh_mem_access_t
{
    ExeContext* call_stack;
    unsigned time_stamp;
    HWord data;
};

enum mh_track_type {
    MH_TRACK    = 1,
    MH_READONLY = 2
};

struct mh_track_mem_block_t
{
    rb_tree_node node;
    Addr start;
    Addr end;
    const char* name;
    unsigned birth_time_stamp;
    unsigned readonly_time_stamp;
    Bool     enabled;
    enum mh_track_type type;
    unsigned word_sz;  /* in bytes */
    unsigned nwords;   /* #columns */
    unsigned history;  /* #rows */
    struct mh_mem_access_t* access_matrix;
    unsigned hist_ix_vec[0];
};

static int region_cmp_key(rb_tree_node* a_node, void* b_key)
{
    struct mh_track_mem_block_t* a = (struct mh_track_mem_block_t*)a_node;
    Addr b_start = (Addr)b_key;
    return a->start < b_start ? -1 : (a->start == b_start ? 0 : 1);
}

static int region_cmp(rb_tree_node* a_node, rb_tree_node* b_node)
{
    struct mh_track_mem_block_t* b = (struct mh_track_mem_block_t*)b_node;
    return region_cmp_key(a_node, (void*)b->start);
}

static void region_print(rb_tree_node* a_node, int depth)
{
    static char spaces[] = "                                                  ";
    struct mh_track_mem_block_t* a = (struct mh_track_mem_block_t*)a_node;
    VG_(umsg)("%.*s%p -> %p", depth, spaces, (void*)a->start, (void*)a->end);
}


static struct rb_tree region_tree;

static
struct mh_track_mem_block_t* region_insert(struct mh_track_mem_block_t* tmb)
{
    return (struct mh_track_mem_block_t*) rb_tree_insert(&region_tree,
							 &tmb->node);
}

static void region_remove(struct mh_track_mem_block_t* tmb)
{
    rb_tree_remove(&region_tree, &tmb->node);
}

static
struct mh_track_mem_block_t* region_min(void)
{
    return (struct mh_track_mem_block_t*) rb_tree_min(&region_tree);
}

static
struct mh_track_mem_block_t* region_succ(struct mh_track_mem_block_t* tmb)
{
    return (struct mh_track_mem_block_t*) rb_tree_succ(&region_tree,
						       &tmb->node);
}

static
struct mh_track_mem_block_t* region_pred(struct mh_track_mem_block_t* tmb)
{
    return (struct mh_track_mem_block_t*) rb_tree_pred(&region_tree,
						       &tmb->node);
}

static
struct mh_track_mem_block_t* region_lookup_maxle(Addr addr)
{
    return (struct mh_track_mem_block_t*) rb_tree_lookup_maxle(&region_tree,
							       (void*)addr);
}

static void insert_nonoverlapping(struct mh_track_mem_block_t* tmb)
{
    struct mh_track_mem_block_t* clash = region_insert(tmb);
    tl_assert(!clash);
    tl_assert(!(clash=region_pred(tmb)) || clash->end <= tmb->start);
    tl_assert(!(clash=region_succ(tmb)) || clash->start >= tmb->end);
}

static unsigned mh_logical_time = 0;

/*
VG_REGPARM(2)
static void track_instr(Addr addr, SizeT size)
{
}

VG_REGPARM(2)
static void track_load(Addr addr, SizeT size)
{
}
*/

static void report_store_in_block(struct mh_track_mem_block_t *tmb,
				  Addr addr, SizeT size, Addr64 data)
{
    ThreadId tid = VG_(get_running_tid)();  // Should tid be passed as arg instead?
    ExeContext *ec = VG_(record_ExeContext)(tid, 0);
    unsigned wix; /* word index */
    unsigned start_wix, end_wix;
    Addr start = addr;
    Addr end = addr + size;

    if (start < tmb->start) {
	union {
	    Addr64 words[2];
	    char bytes[2*8];
	}u;
	int offs = tmb->start - start;
	u.words[0] = data;
	u.words[1] = 0;
	tl_assert(offs < 8);
	data = *(Addr64*) &u.bytes[offs];    /* BUG: Unaligned word access */
	start = tmb->start;
    }
    if (end > tmb->end)
	end = tmb->end;

    start_wix = (start - tmb->start) / tmb->word_sz;
    end_wix = (end - tmb->start - 1) / tmb->word_sz + 1;
    tl_assert(start_wix < end_wix);
    tl_assert(end_wix <= tmb->nwords);

    if (clo_trace_mem) {
	VG_(umsg)("TRACE: %u bytes written at addr %p at time %u:\n",
		  (unsigned)size, (void *)addr, mh_logical_time);
	VG_(pp_ExeContext)(ec);
    }

    for (wix = start_wix; wix < end_wix; wix++) {
	int i;
	unsigned hix = tmb->hist_ix_vec[wix]++;

	if (tmb->hist_ix_vec[wix] >= tmb->history)
	    tmb->hist_ix_vec[wix] = 0;

	i = (tmb->history * wix) + hix;
	//VG_(umsg)("TRACE: Saving at wix=%u hix=%u -> i=%u\n", wix, hix, i);
	tmb->access_matrix[i].call_stack = ec;
	tmb->access_matrix[i].time_stamp = mh_logical_time;
	tmb->access_matrix[i].data = data;

	start += tmb->word_sz;
    }
}

static void report_store_in_readonly(struct mh_track_mem_block_t *tmb,
				     Addr addr, SizeT size, Addr64 data)
{
    VG_(umsg)("Provoking SEGV: %u bytes written to READONLY mem at addr %p at time %u:\n",
	      (unsigned)size, (void *)addr, mh_logical_time);
}

#define track_store_REGPARM 2

VG_REGPARM(track_store_REGPARM)
static Int track_store(Addr addr, SizeT size, Long data)
{
    Addr start = addr;
    Addr end = addr + size;
    struct mh_track_mem_block_t *tmb = region_lookup_maxle(addr);
    Bool got_a_hit = 0;
    Int crash_it = 0;

    if (!tmb || start >= tmb->end)
	return 0;

    do {
	tl_assert(end > tmb->start && start < tmb->end);

	if (tmb->enabled) {
	    if (tmb->type & MH_READONLY) {
		report_store_in_readonly(tmb, addr, size, data);
		crash_it = 1;
		break;
	    }
	    if (tmb->type & MH_TRACK) {
		report_store_in_block(tmb, addr, size, data);
	    }
	    got_a_hit = 1;
	}
	else {
	    //VG_(umsg)("TRACE: Disabled???\n");
	}
	if (end <= tmb->end)
	    break;

	tmb = region_succ(tmb);
    }while (tmb && end > tmb->start);

    if (got_a_hit)
	++mh_logical_time;
    return crash_it;
}

VG_REGPARM(track_store_REGPARM)
static Int track_cas(Addr addr, SizeT size, ULong expected, ULong data)
{
    ULong actual;
    MH_ASSERT2(fit_in_ubytes(expected, size), " expected=%llx size=%u", expected, (int)size);
    MH_ASSERT2(fit_in_ubytes(data, size), " data=%llx size=%u", data, (int)size);
    switch (size) {
    case 1: actual = *(UChar*)addr;  break;
    case 2: actual = *(UShort*)addr; break;
    case 4: actual = *(UInt*)addr;   break;
    case 8: actual = *(ULong*)addr;  break;
    default:
	tl_assert2(0,"CAS on %u-words not implemented",size);
    }
    return (actual == expected) ? track_store(addr, size, data) : 0;
}


#if VEX_HOST_WORDSIZE == 4
#  define IRConst_HWord IRConst_U32
#elif VEX_HOST_WORDSIZE == 8
#  define IRConst_HWord IRConst_U64
#else
#  error "VEX_HOST_WORDSIZE not set to 4 or 8"
#endif


static void addEvent_Ir(IRSB* sb, IRExpr* iaddr, UInt isize)
{
}

static void addEvent_Dr(IRSB* sb, IRExpr* daddr, Int dsize)
{
}

static IRType size2itype(int size)
{
    IRType type;
    switch (size) {
    case 1:  type = Ity_I8;   break;
    case 2:  type = Ity_I16;  break;
    case 4:  type = Ity_I32;  break;
    case 8:  type = Ity_I64;  break;
    case 16: type = Ity_I128; break;
    default:
	tl_assert2(0,"Invalid integer size %u", size);
    }
    return type;
}


static IRExpr* expr2atom(IRSB* sb, IRExpr* e)
{
    if (!isIRAtom(e)) {
	Int size = sizeofIRType(typeOfIRExpr(sb->tyenv, e));
	IRTemp tmp = newIRTemp(sb->tyenv, size2itype(size));
	addStmtToIRSB(sb, IRStmt_WrTmp(tmp, e));
	e = IRExpr_RdTmp(tmp);
    }
    return e;
}

static IRExpr*
widen_to_U64(IRSB* sb, IRExpr* iexpr)
{
    Int size = sizeofIRType(typeOfIRExpr(sb->tyenv, iexpr));
    switch (size) {
    case 1: return IRExpr_Unop(Iop_8Uto64, iexpr);
    case 2: return IRExpr_Unop(Iop_16Uto64, iexpr);
    case 4: return IRExpr_Unop(Iop_32Uto64, iexpr);
    case 8: return iexpr;
    }
    return NULL;
}


static
void addEvent_Dw (IRSB* sb, IRExpr* daddr, Int dsize,
		  IRExpr* expected, /* if CAS */
		  IRExpr* data, HWord ip)
{
    IRTemp retval_tmp, cond_tmp;
    IRExpr* cond_ex;
    IRExpr**   argv;
    IRDirty*   di;
    IRExpr*    data64 = NULL;
    IRExpr*    expd64;
    void* fn;
    const char* fn_name;

    tl_assert(clo_track_mem);
    tl_assert(isIRAtom(daddr));
    tl_assert(dsize >= 1 && dsize <= MAX_DSIZE);

    if (data) {
	data64 = widen_to_U64(sb, data);
    }
    if (!data64) {
	data64 = IRExpr_Const(IRConst_U64((ULong)0xdead));
    }

    if (expected) {
	/*  Emit:
         *
         *  if (track_cas(daddr, dsize, expd, data))
         *      exit(SEGV);
         */
	fn = track_cas;
	fn_name = "track_cas";
	expd64 = widen_to_U64(sb, expected);
	tl_assert(expd64 != NULL);
	argv = mkIRExprVec_4(daddr, mkIRExpr_HWord(dsize),
			     expr2atom(sb, expd64), expr2atom(sb, data64));
    }
    else {
	/*  Emit:
         *
         *  if (track_store(daddr, dsize, data))
         *      exit(SEGV);
         */
	fn = track_store;
	fn_name = "track_store";
	argv = mkIRExprVec_3(daddr, mkIRExpr_HWord(dsize), expr2atom(sb, data64));
    }

    retval_tmp = newIRTemp(sb->tyenv, Ity_I32);
    di = unsafeIRDirty_1_N(retval_tmp,
			   track_store_REGPARM,
			   fn_name,
			   VG_(fnptr_to_fnentry)(fn),
			   argv);
    addStmtToIRSB(sb, IRStmt_Dirty(di));
    cond_ex = IRExpr_Unop(Iop_32to1, IRExpr_RdTmp(retval_tmp));
    cond_tmp = newIRTemp(sb->tyenv, Ity_I1);
    addStmtToIRSB(sb, IRStmt_WrTmp(cond_tmp, cond_ex));
    addStmtToIRSB(sb, IRStmt_Exit(IRExpr_RdTmp(cond_tmp), Ijk_SigSEGV,
				  IRConst_HWord(ip), sb->offsIP));
}


/*------------------------------------------------------------*/
/*--- Basic tool functions                                 ---*/
/*------------------------------------------------------------*/

static void mh_post_clo_init(void)
{
}

static
IRSB* mh_instrument ( VgCallbackClosure* closure,
                      IRSB* sbIn,
                      VexGuestLayout* layout,
                      VexGuestExtents* vge,
		      VexArchInfo* arch,
                      IRType gWordTy, IRType hWordTy )
{
   Int        i;
   IRSB*      sbOut;
   IRTypeEnv* tyenv = sbIn->tyenv;
   HWord      currIP = 0;

   if (gWordTy != hWordTy) {
      /* We don't currently support this case. */
      VG_(tool_panic)("host/guest word size mismatch");
   }

   /* Set up SB */
   sbOut = deepCopyIRSBExceptStmts(sbIn);

   // Copy verbatim any IR preamble preceding the first IMark
   i = 0;
   while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
      addStmtToIRSB( sbOut, sbIn->stmts[i] );
      i++;
   }

   for (/*use current i*/; i < sbIn->stmts_used; i++) {
      IRStmt* st = sbIn->stmts[i];
      if (!st || st->tag == Ist_NoOp) continue;

      switch (st->tag) {
         case Ist_NoOp:
         case Ist_AbiHint:
         case Ist_Put:
         case Ist_PutI:
         case Ist_MBE:
            break;

         case Ist_IMark:
            if (clo_track_mem) {
		addEvent_Ir(sbOut, mkIRExpr_HWord((HWord)st->Ist.IMark.addr),
			    st->Ist.IMark.len);
            }
	    /* Remember pointer to current hw instruction */
	    currIP = (HWord)st->Ist.IMark.addr;
            break;

         case Ist_WrTmp:
            if (clo_track_mem) {
               IRExpr* data = st->Ist.WrTmp.data;
               if (data->tag == Iex_Load) {
                  addEvent_Dr(sbOut, data->Iex.Load.addr,
			      sizeofIRType(data->Iex.Load.ty) );
               }
            }
            break;

         case Ist_Store:
            if (clo_track_mem) {
               IRExpr* data  = st->Ist.Store.data;
               addEvent_Dw( sbOut, st->Ist.Store.addr,
                            sizeofIRType(typeOfIRExpr(tyenv, data)),
			    NULL,
			    data,
			    currIP);
            }
            break;

        case Ist_StoreG:
	    tl_assert(!"Tell Sverker he forgot to implement Ist_StoreGTo.");
	    break;

        case Ist_LoadG:
	    tl_assert(!"Tell Sverker he forgot to implement Ist_LoadG.");
	    break;

         case Ist_Dirty: {
            if (clo_track_mem) {
               Int      dsize;
               IRDirty* d = st->Ist.Dirty.details;
               if (d->mFx != Ifx_None) {
                  // This dirty helper accesses memory.  Collect the details.
                  tl_assert(d->mAddr != NULL);
                  tl_assert(d->mSize != 0);
                  dsize = d->mSize;
                  if (d->mFx == Ifx_Read || d->mFx == Ifx_Modify)
                     addEvent_Dr( sbOut, d->mAddr, dsize );
                  if (d->mFx == Ifx_Write || d->mFx == Ifx_Modify)
                     addEvent_Dw( sbOut, d->mAddr, dsize, NULL, NULL, currIP);
               } else {
                  tl_assert(d->mAddr == NULL);
                  tl_assert(d->mSize == 0);
               }
            }
            break;
         }

         case Ist_CAS: {
            /* We treat it as a read and a write of the location.  I
               think that is the same behaviour as it was before IRCAS
               was introduced, since prior to that point, the Vex
               front ends would translate a lock-prefixed instruction
               into a (normal) read followed by a (normal) write. */
	    if (clo_track_mem) {
		Int    dataSize;
		IRCAS*  cas = st->Ist.CAS.details;
		IRExpr* data = cas->dataLo;
		IRExpr* expd = cas->expdLo;
		tl_assert(cas->addr != NULL);
		tl_assert(cas->dataLo != NULL);
		dataSize = sizeofIRType(typeOfIRExpr(tyenv, cas->dataLo));
		tl_assert(dataSize == sizeofIRType(typeOfIRExpr(tyenv, cas->expdLo)));
		if (cas->dataHi != NULL) {  /* a doubleword-CAS */
		    IROp mergeOp;
		    dataSize *= 2;
		    switch (dataSize) {
		    case  2: mergeOp = Iop_8HLto16;   break;
		    case  4: mergeOp = Iop_16HLto32;  break;
		    case  8: mergeOp = Iop_32HLto64;  break;
		    default:
			tl_assert2(0,"doubleword CAS instruction with size %u not implemented", dataSize);
		    }
		    data = IRExpr_Binop(mergeOp, cas->dataHi, cas->dataLo);
		    expd = IRExpr_Binop(mergeOp, cas->expdHi, cas->expdLo);
		}
		addEvent_Dr(sbOut, cas->addr, dataSize);
		addEvent_Dw(sbOut, cas->addr, dataSize, expd, data, currIP);
	    }
	    break;
         }

         case Ist_LLSC: {
            IRType dataTy;
            if (st->Ist.LLSC.storedata == NULL) {
               /* LL */
               dataTy = typeOfIRTemp(tyenv, st->Ist.LLSC.result);
               if (clo_track_mem)
                  addEvent_Dr( sbOut, st->Ist.LLSC.addr,
                                      sizeofIRType(dataTy) );
            } else {
               /* SC */
               dataTy = typeOfIRExpr(tyenv, st->Ist.LLSC.storedata);
               if (clo_track_mem) {
		  /* Todo:
		   * Do real *conditional* store as we do for CAS above
		   * This is trickier to do as we don't know until *after* the
		   * original LLSC instruction if the store really happened.
		   */
                  addEvent_Dw(sbOut, st->Ist.LLSC.addr, sizeofIRType(dataTy),
			      NULL, st->Ist.LLSC.storedata, currIP);
	       }
            }
            break;
         }

         case Ist_Exit:
            break;

         default:
            tl_assert(0);
      }
      addStmtToIRSB(sbOut, st);      // Original statement
   }

   return sbOut;
}

static unsigned align_up(unsigned unit, unsigned value)
{
    return ((value + unit - 1) / unit) * unit;
}

static void track_mem_write(Addr addr, SizeT size, unsigned word_sz, unsigned history,
			    const char* name)
{
    struct mh_track_mem_block_t *tmb;
    const unsigned nwords = (size + word_sz - 1) / word_sz;
    const unsigned sizeof_hist_ix_vec = nwords * sizeof(*tmb->hist_ix_vec);
    unsigned i;
    const unsigned matrix_offset = align_up(sizeof(void*),
					    (sizeof(struct mh_track_mem_block_t)
					     + sizeof_hist_ix_vec));

    if (clo_trace_mem) {
	VG_(umsg)("TRACE: Tracking %u-words from %p to %p with history %u\n",
		  word_sz, (void*)addr, (void*)(addr+size), history);
    }

    tmb = VG_(malloc)("track_mem_write",
		      matrix_offset + history * nwords * sizeof(struct mh_mem_access_t));
    tmb->start = addr;
    tmb->end = addr + size;
    tmb->name = name;
    tmb->birth_time_stamp = mh_logical_time++;
    tmb->enabled = True;
    tmb->type = MH_TRACK;
    tmb->word_sz = word_sz;
    tmb->nwords = nwords;
    tmb->history = history;
    tmb->access_matrix = (struct mh_mem_access_t*) ((char*)tmb + matrix_offset);
    tl_assert((char*)&tmb->hist_ix_vec[nwords] <= (char*)tmb->access_matrix);
    for (i = 0; i < nwords; i++) {
	tmb->hist_ix_vec[i] = 0;
    }
    for (i = 0; i < history*nwords; i++) {
	tmb->access_matrix[i].call_stack = NULL;
	tmb->access_matrix[i].time_stamp = 0;
    }

    insert_nonoverlapping(tmb);
}

static void remove_track_mem_block(Addr addr, SizeT size, enum mh_track_type type)
{
    Addr end = addr + size;
    struct mh_track_mem_block_t* tmb = region_lookup_maxle(addr);

    tl_assert2(tmb && addr == tmb->start && end == tmb->end,
	       "Could not find region to remove [%p -> %p]", addr, end);
    tl_assert((type & tmb->type) == type);

    tmb->type &= ~type;
    if (clo_trace_mem) {
	if (tmb->type & MH_TRACK) {
	    VG_(umsg)("TRACE: Untracking '%s' from %p to %p\n",
		      tmb->name, (void*)addr, (void*)(addr + size));
	}
	if (tmb->type & MH_READONLY) {
	    VG_(umsg)("TRACE: Make '%s' writable from %p to %p\n",
		      tmb->name, (void*)addr, (void*)(addr + size));
	}
    }
    if (!tmb->type) {
	region_remove(tmb);
	VG_(free) (tmb);
    }
}

static void untrack_mem_write (Addr addr, SizeT size)
{
    remove_track_mem_block(addr, size, MH_TRACK);
}

static void track_able(Addr addr, SizeT size, Bool enabled)
{
    Addr end = addr + size;
    struct mh_track_mem_block_t* tmb = region_lookup_maxle(addr);

    tl_assert2(tmb && addr == tmb->start && end == tmb->end,
	       "Could not find region to %sable", enabled ? "en":"dis");

    if (clo_trace_mem) {
	VG_(umsg)("TRACE: %sable '%s' from %p to %p\n",
		  (enabled ? "En" : "Dis"),
		  tmb->name, (void*)addr, (void*)(addr + size));
    }
    tmb->enabled = enabled;
}


static void set_mem_readonly(Addr addr, SizeT size, const char* name)
{
    struct mh_track_mem_block_t *tmb;

    if (clo_trace_mem) {
	VG_(umsg)("TRACE: Set '%s' readonly from %p to %p\n",
		  name, (void*)addr, (void*)(addr+size));
    }

    tmb = region_lookup_maxle(addr);
    if (tmb->start == addr) {
	tl_assert(tmb->end == addr+size);
	tmb->type |= MH_READONLY;
	tmb->readonly_time_stamp = mh_logical_time++;
    }
    else {
	tmb = VG_(malloc)("set_mem_readonly", sizeof(struct mh_track_mem_block_t));
	tmb->start = addr;
	tmb->end = addr + size;
	tmb->name = name;
	tmb->birth_time_stamp = mh_logical_time++;
	tmb->enabled = True;
	tmb->type = MH_READONLY;
	insert_nonoverlapping(tmb);
    }
}

static void set_mem_writable (Addr addr, SizeT size)
{
    remove_track_mem_block(addr, size, MH_READONLY);
}


/*------------------------------------------------------------*/
/*--- Client requests                                      ---*/
/*------------------------------------------------------------*/

static Bool mh_handle_client_request ( ThreadId tid, UWord* arg, UWord* ret )
{
   if (!VG_IS_TOOL_USERREQ('M','H',arg[0])) {
      return False;
   }

   switch (arg[0]) {
      case VG_USERREQ__TRACK_MEM_WRITE:
	  track_mem_write (arg[1], arg[2], arg[3], arg[4], (char*)arg[5]);
         *ret = -1;
         break;
      case VG_USERREQ__UNTRACK_MEM_WRITE:
         untrack_mem_write (arg[1], arg[2]);
         *ret = -1;
         break;

      case VG_USERREQ__TRACK_ENABLE:
	  track_able (arg[1], arg[2], 1);
	  *ret = -1;
	  break;

      case VG_USERREQ__TRACK_DISABLE:
	  track_able (arg[1], arg[2], 0);
	  *ret = -1;
	  break;

      case VG_USERREQ__SET_READONLY:
	 set_mem_readonly(arg[1], arg[2], (char*)arg[3]);
	 *ret = -1;
	 break;

      case VG_USERREQ__SET_WRITABLE:
	  set_mem_writable(arg[1], arg[2]);
	  *ret = -1;
	  break;

      default:
         VG_(message)(
            Vg_UserMsg,
            "Warning: unknown memcheck client request code %llx\n",
            (ULong)arg[0]
         );
         return False;
   }
   return True;
}


static void print_word(unsigned word_sz, struct mh_mem_access_t* ap)
{
    switch (word_sz) {
#if VEX_HOST_WORDSIZE == 8
    case sizeof(HWord):
	VG_(umsg)("%p", (void*)ap->data); break;
#endif
    case sizeof(int):
	VG_(umsg)("%#x", (int)ap->data); break;
    case sizeof(short):
	VG_(umsg)("%#x", (int)(short)ap->data); break;
    case sizeof(char):
	VG_(umsg)("%#x", (int)(char)ap->data); break;
    default:
	VG_(umsg)("(?)"); break;
    }
}


static void mh_fini(Int exitcode)
{
    struct mh_track_mem_block_t* tmb = region_min();

    for (tmb = region_min(); tmb; tmb = region_succ(tmb)) {
      if (tmb->type & MH_TRACK) {
	unsigned wix = 0; /* word index */
	Addr addr = tmb->start;
	VG_(umsg) ("Memhist tracking '%s' from %p to %p with word size %u "
		   "and history %u created at time %u.\n", tmb->name,
		   (void*)tmb->start, (void*)tmb->end, tmb->word_sz,
		   tmb->history, tmb->birth_time_stamp);
	for (addr=tmb->start; addr < tmb->end; wix++, addr += tmb->word_sz) {
	    unsigned h;
	    int hist_ix = tmb->hist_ix_vec[wix] - 1;

	    for (h=0; h < tmb->history; h++, hist_ix--) {
		struct mh_mem_access_t* ap;

		if (hist_ix < 0)
		    hist_ix = tmb->history - 1;

		//VG_(umsg)("TRACE: Reading at ix=%u\n", wix*tmb->history + hist_ix);
		ap = &tmb->access_matrix[wix*tmb->history + hist_ix];
		if (ap->call_stack) {
		    if (!h) {
			VG_(umsg)("%u-bytes ", tmb->word_sz);
			print_word(tmb->word_sz, ap);
			VG_(umsg)(" written to address %p at time %u:\n",
				  (void*)addr, ap->time_stamp);
		    }
		    else {
			VG_(umsg) ("       AND ");
			print_word(tmb->word_sz, ap);
			VG_(umsg) (" written at time %u:\n", ap->time_stamp);
		    }
		    VG_(pp_ExeContext)(ap->call_stack);
		}
		else {
		    if (!h)
			VG_(umsg) ("%u-bytes at %p not written.\n", tmb->word_sz, (void*)addr);
		    break;
		}
	    }
	}
      }
      if (tmb->type & MH_READONLY) {
	  VG_(umsg) ("Region '%s' made READONLY from %p to %p at time %u.\n",
		     tmb->name, (void*)tmb->start, (void*)tmb->end,
		     tmb->readonly_time_stamp);
      }
    }
}

static void mh_pre_clo_init(void)
{
    rb_tree_init(&region_tree, region_cmp, region_cmp_key, region_print);

   VG_(details_name)            ("Memhist");
   VG_(details_version)         (NULL);
   VG_(details_description)     ("Sverker's Valgrind tool for tracking memory access history");
   VG_(details_copyright_author)(
      "Copyright (C) 2014, and GNU GPL'd, by Sverker Eriksson.");
   VG_(details_bug_reports_to)  (VG_BUGS_TO);
   VG_(details_avg_translation_sizeB) ( 200 );

   VG_(basic_tool_funcs)          (mh_post_clo_init,
                                   mh_instrument,
                                   mh_fini);
   VG_(needs_command_line_options)(mh_process_cmd_line_option,
                                   mh_print_usage,
                                   mh_print_debug_usage);
   VG_(needs_client_requests)     (mh_handle_client_request);
}

VG_DETERMINE_INTERFACE_VERSION(mh_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                mh_main.c ---*/
/*--------------------------------------------------------------------*/