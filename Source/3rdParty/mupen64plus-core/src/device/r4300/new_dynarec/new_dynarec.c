/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - new_dynarec.c                                           *
 *   Copyright (C) 2009-2011 Ari64                                         *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> // needed for u_int, u_char, etc
#include <assert.h>

#if defined(__APPLE__)
#define MAP_ANONYMOUS MAP_ANON
#endif

#include "new_dynarec.h"
#include "api/m64p_types.h"
#include "api/callbacks.h"
#include "main/main.h"
#include "main/rom.h"
#include "device/memory/memory.h"
#include "device/r4300/cached_interp.h"
#include "device/r4300/cp0.h"
#include "device/r4300/cp1.h"
#include "device/r4300/interrupt.h"
#include "device/r4300/tlb.h"
#include "device/r4300/fpu.h"
#include "device/rcp/mi/mi_controller.h"
#include "device/rcp/rsp/rsp_core.h"

#if !defined(WIN32)
#include <sys/mman.h>
#endif

#if defined(RECOMPILER_DEBUG) && !defined(RECOMP_DBG)
void recomp_dbg_init(void);
void recomp_dbg_cleanup(void);
void recomp_dbg_block(int addr);
#endif

#if NEW_DYNAREC == NEW_DYNAREC_X86
#include "x86/assem_x86.h"
#elif  NEW_DYNAREC == NEW_DYNAREC_X64
#include "x64/assem_x64.h"
#elif NEW_DYNAREC == NEW_DYNAREC_ARM
#include "arm/arm_cpu_features.h"
#include "arm/assem_arm.h"
#elif  NEW_DYNAREC == NEW_DYNAREC_ARM64
#include "arm64/assem_arm64.h"
#else
#error Unsupported dynarec architecture
#endif

/* debug */
#define ASSEM_DEBUG 0
#define INV_DEBUG 0
#define COUNT_NOTCOMPILEDS 0

//#define INTERPRET_LOAD
//#define INTERPRET_STORE
//#define INTERPRET_C1LS
//#define INTERPRET_LOADLR
//#define INTERPRET_STORELR
//#define INTERPRET_MULT
//#define INTERPRET_DIV
//#define INTERPRET_MULT64
//#define INTERPRET_DIV64
//#define INTERPRET_FCONV
//#define INTERPRET_FLOAT
//#define INTERPRET_FCOMP

#if ASSEM_DEBUG
    #define assem_debug(...) DebugMessage(M64MSG_VERBOSE, __VA_ARGS__)
#else
    #define assem_debug(...)
#endif
#if INV_DEBUG
    #define inv_debug(...) DebugMessage(M64MSG_VERBOSE, __VA_ARGS__)
#else
    #define inv_debug(...)
#endif

#ifdef NDEBUG
    #define assem_strcpy(...)
#else
    #define assem_strcpy strcpy
#endif

/* registers that may be allocated */
/* 1-31 gpr */
#define HIREG 32 // hi
#define LOREG 33 // lo
#define FSREG 34 // FPU status (FCSR)
#define CSREG 35 // Coprocessor status
#define CCREG 36 // Cycle count
#define INVCP 37 // Pointer to invalid_code
#define MMREG 38 // Pointer to memory_map
#define ROREG 39 // ram offset (if rdram!=0x80000000)
#define TEMPREG 40
#define FTEMP 40 // FPU temporary register
#define PTEMP 41 // Prefetch temporary register
#define TLREG 42 // TLB mapping offset
#define RHASH 43 // Return address hash
#define RHTBL 44 // Return address hash table address
#define RTEMP 45 // JR/JALR address register
#define MAXREG 45
#define AGEN1 46 // Address generation temporary register
#define AGEN2 47 // Address generation temporary register
#define MGEN1 48 // Maptable address generation temporary register
#define MGEN2 49 // Maptable address generation temporary register
#define BTREG 50 // Branch target temporary register

/* instruction types */
#define NOP 0     // No operation
#define LOAD 1    // Load
#define STORE 2   // Store
#define LOADLR 3  // Unaligned load
#define STORELR 4 // Unaligned store
#define MOV 5     // Move
#define ALU 6     // Arithmetic/logic
#define MULTDIV 7 // Multiply/divide
#define SHIFT 8   // Shift by register
#define SHIFTIMM 9// Shift by immediate
#define IMM16 10  // 16-bit immediate
#define RJUMP 11  // Unconditional jump to register
#define UJUMP 12  // Unconditional jump
#define CJUMP 13  // Conditional branch (BEQ/BNE/BGTZ/BLEZ)
#define SJUMP 14  // Conditional branch (regimm format)
#define COP0 15   // Coprocessor 0
#define COP1 16   // Coprocessor 1
#define C1LS 17   // Coprocessor 1 load/store
#define FJUMP 18  // Conditional branch (floating point)
#define FLOAT 19  // Floating point unit
#define FCONV 20  // Convert integer to float
#define FCOMP 21  // Floating point compare (sets FSREG)
#define SYSCALL 22// SYSCALL
#define OTHER 23  // Other
#define SPAN 24   // Branch/delay slot spans 2 pages
#define NI 25     // Not implemented

/* stubs */
#define CC_STUB 1
#define FP_STUB 2
#define LOADB_STUB 3
#define LOADH_STUB 4
#define LOADW_STUB 5
#define LOADD_STUB 6
#define LOADBU_STUB 7
#define LOADHU_STUB 8
#define LOADWU_STUB 9
#define STOREB_STUB 10
#define STOREH_STUB 11
#define STOREW_STUB 12
#define STORED_STUB 13
#define LOADWR_STUB 14
#define LOADWL_STUB 15
#define LOADDR_STUB 16
#define LOADDL_STUB 17
#define STOREWL_STUB 18
#define STOREWR_STUB 19
#define STOREDL_STUB 20
#define STOREDR_STUB 21
#define INVCODE_STUB 22

/* branch codes */
#define TAKEN 1
#define NOTTAKEN 2
#define NULLDS 3

#define MAXBLOCK 4096
#define MAX_OUTPUT_BLOCK_SIZE 262144
#define CLOCK_DIVIDER g_dev.r4300.cp0.count_per_op

struct regstat
{
  signed char regmap_entry[HOST_REGS];
  signed char regmap[HOST_REGS];
  uint64_t was32;
  uint64_t is32;
  uint64_t wasdirty;
  uint64_t dirty;
  uint64_t u;
  uint64_t uu;
  u_int wasconst;
  u_int isconst;
  uint64_t constmap[HOST_REGS];
};

struct ll_entry
{
  void *addr;
  void *clean_addr;
  void *copy;
  struct ll_entry *next;
  u_int vaddr;
  u_int reg32;
  u_int start;
  u_int length;
};

/* linkage */
void verify_code(void);
void cc_interrupt(void);
void do_interrupt(void);
void fp_exception(void);
void jump_syscall(void);
void jump_eret(void);
void dyna_linker(void);
void dyna_linker_ds(void);
void breakpoint(void);

int new_recompile_block(int addr);
void invalidate_block(u_int block);
void *get_addr_ht(u_int vaddr);
void *get_addr_32(u_int vaddr,u_int flags);

static void load_regs_entry(int t);
static void inline_readstub(int type,int i,u_int addr_const,char addr,struct regstat *i_regs,int target,int adj,u_int reglist);

void *base_addr;
void *base_addr_rx;
u_char *out;
unsigned int using_tlb;
unsigned int stop_after_jal;

static u_int start;
static u_int *source;
static u_int pagelimit;
static char insn[MAXBLOCK][10];
static u_char itype[MAXBLOCK];
static u_char opcode[MAXBLOCK];
static u_char opcode2[MAXBLOCK];
static u_char bt[MAXBLOCK];
static u_char rs1[MAXBLOCK];
static u_char rs2[MAXBLOCK];
static u_char rt1[MAXBLOCK];
static u_char rt2[MAXBLOCK];
static u_char us1[MAXBLOCK];
static u_char us2[MAXBLOCK];
static u_char dep1[MAXBLOCK];
static u_char dep2[MAXBLOCK];
static u_char lt1[MAXBLOCK];
static int imm[MAXBLOCK];
static u_int ba[MAXBLOCK];
static char likely[MAXBLOCK];
static char is_ds[MAXBLOCK];
static char ooo[MAXBLOCK];
static uint64_t unneeded_reg[MAXBLOCK];
static uint64_t unneeded_reg_upper[MAXBLOCK];
static uint64_t branch_unneeded_reg[MAXBLOCK];
static uint64_t branch_unneeded_reg_upper[MAXBLOCK];
static uint64_t p32[MAXBLOCK];
static uint64_t pr32[MAXBLOCK];
static signed char regmap_pre[MAXBLOCK][HOST_REGS];
static uint64_t constmap[MAXBLOCK][HOST_REGS];
static struct regstat regs[MAXBLOCK];
static struct regstat branch_regs[MAXBLOCK];
static signed char minimum_free_regs[MAXBLOCK];
static u_int needed_reg[MAXBLOCK];
static uint64_t requires_32bit[MAXBLOCK];
static u_int wont_dirty[MAXBLOCK];
static u_int will_dirty[MAXBLOCK];
static int ccadj[MAXBLOCK];
static int slen;
static uintptr_t instr_addr[MAXBLOCK];
static uintptr_t link_addr[MAXBLOCK][3];
static int linkcount;
static uintptr_t stubs[MAXBLOCK*3][8];
static int stubcount;
static int literalcount;
static int is_delayslot;
static int cop1_usable;
static char *copy;
static int expirep;
static u_int dirty_entry_count;
static u_int copy_size;
static struct ll_entry* hash_table[65536][2];
static struct ll_entry *jump_in[4096];
static struct ll_entry *jump_dirty[4096];
static struct ll_entry *jump_out[4096];
static unsigned char restore_candidate[512];

#if COUNT_NOTCOMPILEDS
static int notcompiledCount = 0;
#endif

#if ASSEM_DEBUG
static signed char regmap[MAXBLOCK][HOST_REGS];
static signed char regmap_entry[MAXBLOCK][HOST_REGS];
#endif

static void clear_all_regs(signed char regmap[])
{
  int hr;
  for (hr=0;hr<HOST_REGS;hr++) regmap[hr]=-1;
}

static signed char get_reg(signed char regmap[],int r)
{
  int hr;
  for (hr=0;hr<HOST_REGS;hr++) if(hr!=EXCLUDE_REG&&regmap[hr]==r) return hr;
  return -1;
}

// Find a register that is available for two consecutive cycles
static signed char get_reg2(signed char regmap1[],signed char regmap2[],int r)
{
  int hr;
  for (hr=0;hr<HOST_REGS;hr++) if(hr!=EXCLUDE_REG&&regmap1[hr]==r&&regmap2[hr]==r) return hr;
  return -1;
}

static int count_free_regs(signed char regmap[])
{
  int count=0;
  int hr;
  for(hr=0;hr<HOST_REGS;hr++)
  {
    if(hr!=EXCLUDE_REG) {
      if(regmap[hr]<0) count++;
    }
  }
  return count;
}

static void dirty_reg(struct regstat *cur,signed char reg)
{
  int hr;
  if(!reg) return;
  for (hr=0;hr<HOST_REGS;hr++) {
    if((cur->regmap[hr]&63)==reg) {
      cur->dirty|=1LL<<hr;
    }
  }
}

// If we dirty the lower half of a 64 bit register which is now being
// sign-extended, we need to dump the upper half.
// Note: Do this only after completion of the instruction, because
// some instructions may need to read the full 64-bit value even if
// overwriting it (eg SLTI, DSRA32).
static void flush_dirty_uppers(struct regstat *cur)
{
  int hr,reg;
  for (hr=0;hr<HOST_REGS;hr++) {
    if((cur->dirty>>hr)&1) {
      reg=cur->regmap[hr];
      if(reg>=64)
        if((cur->is32>>(reg&63))&1) cur->regmap[hr]=-1;
    }
  }
}

static void set_const(struct regstat *cur,signed char reg,uint64_t value)
{
  int hr;
  if(!reg) return;
  for (hr=0;hr<HOST_REGS;hr++) {
    if(cur->regmap[hr]==reg) {
      cur->isconst|=1<<hr;
      cur->constmap[hr]=value;
    }
    else if((cur->regmap[hr]^64)==reg) {
      cur->isconst|=1<<hr;
      cur->constmap[hr]=value>>32;
    }
  }
}

static void clear_const(struct regstat *cur,signed char reg)
{
  int hr;
  if(!reg) return;
  for (hr=0;hr<HOST_REGS;hr++) {
    if((cur->regmap[hr]&63)==reg) {
      cur->isconst&=~(1<<hr);
    }
  }
}

static int is_const(struct regstat *cur,signed char reg)
{
  int hr;
  if(reg<0) return 0;
  if(!reg) return 1;
  for (hr=0;hr<HOST_REGS;hr++) {
    if((cur->regmap[hr]&63)==reg) {
      return (cur->isconst>>hr)&1;
    }
  }
  return 0;
}
static uint64_t get_const(struct regstat *cur,signed char reg)
{
  int hr;
  if(!reg) return 0;
  for (hr=0;hr<HOST_REGS;hr++) {
    if(cur->regmap[hr]==reg) {
      return cur->constmap[hr];
    }
  }
  DebugMessage(M64MSG_ERROR, "Unknown constant in r%d",reg);
  exit(1);
}

// Least soon needed registers
// Look at the next ten instructions and see which registers
// will be used.  Try not to reallocate these.
static void lsn(u_char hsn[], int i, int *preferred_reg)
{
  int j;
  int b=-1;
  for(j=0;j<9;j++)
  {
    if(i+j>=slen) {
      j=slen-i-1;
      break;
    }
    if(itype[i+j]==UJUMP||itype[i+j]==RJUMP||(source[i+j]>>16)==0x1000)
    {
      // Don't go past an unconditional jump
      j++;
      break;
    }
  }
  for(;j>=0;j--)
  {
    if(rs1[i+j]) hsn[rs1[i+j]]=j;
    if(rs2[i+j]) hsn[rs2[i+j]]=j;
    if(rt1[i+j]) hsn[rt1[i+j]]=j;
    if(rt2[i+j]) hsn[rt2[i+j]]=j;
    if(itype[i+j]==STORE || itype[i+j]==STORELR) {
      // Stores can allocate zero
      hsn[rs1[i+j]]=j;
      hsn[rs2[i+j]]=j;
    }
    // On some architectures stores need invc_ptr
    #if defined(HOST_IMM8) || defined(NEED_INVC_PTR)
    if(itype[i+j]==STORE || itype[i+j]==STORELR || (opcode[i+j]&0x3b)==0x39) {
      hsn[INVCP]=j;
    }
    #endif
    if(i+j>=0&&(itype[i+j]==UJUMP||itype[i+j]==CJUMP||itype[i+j]==SJUMP||itype[i+j]==FJUMP))
    {
      hsn[CCREG]=j;
      b=j;
    }
  }
  if(b>=0)
  {
    if(ba[i+b]>=start && ba[i+b]<(start+slen*4))
    {
      // Follow first branch
      int t=(ba[i+b]-start)>>2;
      j=7-b;if(t+j>=slen) j=slen-t-1;
      for(;j>=0;j--)
      {
        if(rs1[t+j]) if(hsn[rs1[t+j]]>j+b+2) hsn[rs1[t+j]]=j+b+2;
        if(rs2[t+j]) if(hsn[rs2[t+j]]>j+b+2) hsn[rs2[t+j]]=j+b+2;
        //if(rt1[t+j]) if(hsn[rt1[t+j]]>j+b+2) hsn[rt1[t+j]]=j+b+2;
        //if(rt2[t+j]) if(hsn[rt2[t+j]]>j+b+2) hsn[rt2[t+j]]=j+b+2;
      }
    }
    // TODO: preferred register based on backward branch
  }
  // Delay slot should preferably not overwrite branch conditions or cycle count
  if(i>0&&(itype[i-1]==RJUMP||itype[i-1]==UJUMP||itype[i-1]==CJUMP||itype[i-1]==SJUMP||itype[i-1]==FJUMP)) {
    if(rs1[i-1]) if(hsn[rs1[i-1]]>1) hsn[rs1[i-1]]=1;
    if(rs2[i-1]) if(hsn[rs2[i-1]]>1) hsn[rs2[i-1]]=1;
    hsn[CCREG]=1;
    // ...or hash tables
    hsn[RHASH]=1;
    hsn[RHTBL]=1;
  }
  // Coprocessor load/store needs FTEMP, even if not declared
  if(itype[i]==C1LS) {
    hsn[FTEMP]=0;
  }
  // Load L/R also uses FTEMP as a temporary register
  if(itype[i]==LOADLR) {
    hsn[FTEMP]=0;
  }
  // Also 64-bit SDL/SDR
  if(opcode[i]==0x2c||opcode[i]==0x2d) {
    hsn[FTEMP]=0;
  }
  // Don't remove the TLB registers either
  if(itype[i]==LOAD || itype[i]==LOADLR || itype[i]==STORE || itype[i]==STORELR || itype[i]==C1LS ) {
    hsn[TLREG]=0;
  }
  // Don't remove the miniht registers
  if(itype[i]==UJUMP||itype[i]==RJUMP)
  {
    hsn[RHASH]=0;
    hsn[RHTBL]=0;
  }
}

// We only want to allocate registers if we're going to use them again soon
static int needed_again(int r, int i)
{
  int j;
  /*int b=-1;*/
  int rn=10;

  if(i>0&&(itype[i-1]==UJUMP||itype[i-1]==RJUMP||(source[i-1]>>16)==0x1000))
  {
    if(ba[i-1]<start || ba[i-1]>start+slen*4-4)
      return 0; // Don't need any registers if exiting the block
  }
  for(j=0;j<9;j++)
  {
    if(i+j>=slen) {
      j=slen-i-1;
      break;
    }
    if(itype[i+j]==UJUMP||itype[i+j]==RJUMP||(source[i+j]>>16)==0x1000)
    {
      // Don't go past an unconditional jump
      j++;
      break;
    }
    if(itype[i+j]==SYSCALL||((source[i+j]&0xfc00003f)==0x0d))
    {
      break;
    }
  }
  for(;j>=1;j--)
  {
    if(rs1[i+j]==r) rn=j;
    if(rs2[i+j]==r) rn=j;
    if((unneeded_reg[i+j]>>r)&1) rn=10;
    if(i+j>=0&&(itype[i+j]==UJUMP||itype[i+j]==CJUMP||itype[i+j]==SJUMP||itype[i+j]==FJUMP))
    {
      /*b=j;*/
    }
  }
  /*
  if(b>=0)
  {
    if(ba[i+b]>=start && ba[i+b]<(start+slen*4))
    {
      // Follow first branch
      int o=rn;
      int t=(ba[i+b]-start)>>2;
      j=7-b;if(t+j>=slen) j=slen-t-1;
      for(;j>=0;j--)
      {
        if(!((unneeded_reg[t+j]>>r)&1)) {
          if(rs1[t+j]==r) if(rn>j+b+2) rn=j+b+2;
          if(rs2[t+j]==r) if(rn>j+b+2) rn=j+b+2;
        }
        else rn=o;
      }
    }
  }*/
  if(rn<10) return 1;
  return 0;
}

// Try to match register allocations at the end of a loop with those
// at the beginning
static int loop_reg(int i, int r, int hr)
{
  int j,k;
  for(j=0;j<9;j++)
  {
    if(i+j>=slen) {
      j=slen-i-1;
      break;
    }
    if(itype[i+j]==UJUMP||itype[i+j]==RJUMP||(source[i+j]>>16)==0x1000)
    {
      // Don't go past an unconditional jump
      j++;
      break;
    }
  }
  k=0;
  if(i>0){
    if(itype[i-1]==UJUMP||itype[i-1]==CJUMP||itype[i-1]==SJUMP||itype[i-1]==FJUMP)
      k--;
  }
  for(;k<j;k++)
  {
    if(r<64&&((unneeded_reg[i+k]>>r)&1)) return hr;
    if(r>64&&((unneeded_reg_upper[i+k]>>r)&1)) return hr;
    if(i+k>=0&&(itype[i+k]==UJUMP||itype[i+k]==CJUMP||itype[i+k]==SJUMP||itype[i+k]==FJUMP))
    {
      if(ba[i+k]>=start && ba[i+k]<(start+i*4))
      {
        int t=(ba[i+k]-start)>>2;
        int reg=get_reg(regs[t].regmap_entry,r);
        if(reg>=0) return reg;
        //reg=get_reg(regs[t+1].regmap_entry,r);
        //if(reg>=0) return reg;
      }
    }
  }
  return hr;
}

// Basic liveness analysis for MIPS registers
static void unneeded_registers(int istart,int iend,int r)
{
  int i;
  uint64_t u,uu,b,bu;
  uint64_t temp_u,temp_uu;
  uint64_t tdep;
  if(iend==slen-1) {
    u=1;uu=1;
  }else{
    u=unneeded_reg[iend+1];
    uu=unneeded_reg_upper[iend+1];
    u=1;uu=1;
  }
  for (i=iend;i>=istart;i--)
  {
    //DebugMessage(M64MSG_VERBOSE, "unneeded registers i=%d (%d,%d) r=%d",i,istart,iend,r);
    if(itype[i]==RJUMP||itype[i]==UJUMP||itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP)
    {
      // If subroutine call, flag return address as a possible branch target
      if(rt1[i]==31 && i<slen-2) bt[i+2]=1;

      if(ba[i]<start || ba[i]>=(start+slen*4))
      {
        // Branch out of this block, flush all regs
        u=1;
        uu=1;
        /* Hexagon hack
        if(itype[i]==UJUMP&&rt1[i]==31)
        {
          uu=u=0x300C00F; // Discard at, v0-v1, t6-t9
        }
        if(itype[i]==RJUMP&&rs1[i]==31)
        {
          uu=u=0x300C0F3; // Discard at, a0-a3, t6-t9
        }
        if(start>0x80000400&&start<0x80800000) {
          if(itype[i]==UJUMP&&rt1[i]==31)
          {
            //uu=u=0x30300FF0FLL; // Discard at, v0-v1, t0-t9, lo, hi
            uu=u=0x300FF0F; // Discard at, v0-v1, t0-t9
          }
          if(itype[i]==RJUMP&&rs1[i]==31)
          {
            //uu=u=0x30300FFF3LL; // Discard at, a0-a3, t0-t9, lo, hi
            uu=u=0x300FFF3; // Discard at, a0-a3, t0-t9
          }
        }*/
        branch_unneeded_reg[i]=u;
        branch_unneeded_reg_upper[i]=uu;
        // Merge in delay slot
        tdep=(~uu>>rt1[i+1])&1;
        u|=(1LL<<rt1[i+1])|(1LL<<rt2[i+1]);
        uu|=(1LL<<rt1[i+1])|(1LL<<rt2[i+1]);
        u&=~((1LL<<rs1[i+1])|(1LL<<rs2[i+1]));
        uu&=~((1LL<<us1[i+1])|(1LL<<us2[i+1]));
        uu&=~((tdep<<dep1[i+1])|(tdep<<dep2[i+1]));
        u|=1;uu|=1;
        // If branch is "likely" (and conditional)
        // then we skip the delay slot on the fall-thru path
        if(likely[i]) {
          if(i<slen-1) {
            u&=unneeded_reg[i+2];
            uu&=unneeded_reg_upper[i+2];
          }
          else
          {
            u=1;
            uu=1;
          }
        }
      }
      else
      {
        // Internal branch, flag target
        bt[(ba[i]-start)>>2]=1;
        if(ba[i]<=start+i*4) {
          // Backward branch
          if(itype[i]==RJUMP||itype[i]==UJUMP||(source[i]>>16)==0x1000)
          {
            // Unconditional branch
            temp_u=1;temp_uu=1;
          } else {
            // Conditional branch (not taken case)
            temp_u=unneeded_reg[i+2];
            temp_uu=unneeded_reg_upper[i+2];
          }
          // Merge in delay slot
          tdep=(~temp_uu>>rt1[i+1])&1;
          temp_u|=(1LL<<rt1[i+1])|(1LL<<rt2[i+1]);
          temp_uu|=(1LL<<rt1[i+1])|(1LL<<rt2[i+1]);
          temp_u&=~((1LL<<rs1[i+1])|(1LL<<rs2[i+1]));
          temp_uu&=~((1LL<<us1[i+1])|(1LL<<us2[i+1]));
          temp_uu&=~((tdep<<dep1[i+1])|(tdep<<dep2[i+1]));
          temp_u|=1;temp_uu|=1;
          // If branch is "likely" (and conditional)
          // then we skip the delay slot on the fall-thru path
          if(likely[i]) {
            if(i<slen-1) {
              temp_u&=unneeded_reg[i+2];
              temp_uu&=unneeded_reg_upper[i+2];
            }
            else
            {
              temp_u=1;
              temp_uu=1;
            }
          }
          tdep=(~temp_uu>>rt1[i])&1;
          temp_u|=(1LL<<rt1[i])|(1LL<<rt2[i]);
          temp_uu|=(1LL<<rt1[i])|(1LL<<rt2[i]);
          temp_u&=~((1LL<<rs1[i])|(1LL<<rs2[i]));
          temp_uu&=~((1LL<<us1[i])|(1LL<<us2[i]));
          temp_uu&=~((tdep<<dep1[i])|(tdep<<dep2[i]));
          temp_u|=1;temp_uu|=1;
          unneeded_reg[i]=temp_u;
          unneeded_reg_upper[i]=temp_uu;
          // Only go three levels deep.  This recursion can take an
          // excessive amount of time if there are a lot of nested loops.
          if(r<2) {
            unneeded_registers((ba[i]-start)>>2,i-1,r+1);
          }else{
            unneeded_reg[(ba[i]-start)>>2]=1;
            unneeded_reg_upper[(ba[i]-start)>>2]=1;
          }
        } /*else*/ if(1) {
          if(itype[i]==RJUMP||itype[i]==UJUMP||(source[i]>>16)==0x1000)
          {
            // Unconditional branch
            u=unneeded_reg[(ba[i]-start)>>2];
            uu=unneeded_reg_upper[(ba[i]-start)>>2];
            branch_unneeded_reg[i]=u;
            branch_unneeded_reg_upper[i]=uu;
        //u=1;
        //uu=1;
        //branch_unneeded_reg[i]=u;
        //branch_unneeded_reg_upper[i]=uu;
            // Merge in delay slot
            tdep=(~uu>>rt1[i+1])&1;
            u|=(1LL<<rt1[i+1])|(1LL<<rt2[i+1]);
            uu|=(1LL<<rt1[i+1])|(1LL<<rt2[i+1]);
            u&=~((1LL<<rs1[i+1])|(1LL<<rs2[i+1]));
            uu&=~((1LL<<us1[i+1])|(1LL<<us2[i+1]));
            uu&=~((tdep<<dep1[i+1])|(tdep<<dep2[i+1]));
            u|=1;uu|=1;
          } else {
            // Conditional branch
            b=unneeded_reg[(ba[i]-start)>>2];
            bu=unneeded_reg_upper[(ba[i]-start)>>2];
            branch_unneeded_reg[i]=b;
            branch_unneeded_reg_upper[i]=bu;
        //b=1;
        //bu=1;
        //branch_unneeded_reg[i]=b;
        //branch_unneeded_reg_upper[i]=bu;
            // Branch delay slot
            tdep=(~uu>>rt1[i+1])&1;
            b|=(1LL<<rt1[i+1])|(1LL<<rt2[i+1]);
            bu|=(1LL<<rt1[i+1])|(1LL<<rt2[i+1]);
            b&=~((1LL<<rs1[i+1])|(1LL<<rs2[i+1]));
            bu&=~((1LL<<us1[i+1])|(1LL<<us2[i+1]));
            bu&=~((tdep<<dep1[i+1])|(tdep<<dep2[i+1]));
            b|=1;bu|=1;
            // If branch is "likely" then we skip the
            // delay slot on the fall-thru path
            if(likely[i]) {
              u=b;
              uu=bu;
              if(i<slen-1) {
                u&=unneeded_reg[i+2];
                uu&=unneeded_reg_upper[i+2];
        //u=1;
        //uu=1;
              }
            } else {
              u&=b;
              uu&=bu;
        //u=1;
        //uu=1;
            }
            if(i<slen-1) {
              branch_unneeded_reg[i]&=unneeded_reg[i+2];
              branch_unneeded_reg_upper[i]&=unneeded_reg_upper[i+2];
        //branch_unneeded_reg[i]=1;
        //branch_unneeded_reg_upper[i]=1;
            } else {
              branch_unneeded_reg[i]=1;
              branch_unneeded_reg_upper[i]=1;
            }
          }
        }
      }
    }
    else if(itype[i]==SYSCALL)
    {
      // SYSCALL instruction (software interrupt)
      u=1;
      uu=1;
    }
    else if(itype[i]==COP0 && (source[i]&0x3f)==0x18)
    {
      // ERET instruction (return from interrupt)
      u=1;
      uu=1;
    }
    //u=uu=1; // DEBUG
    tdep=(~uu>>rt1[i])&1;
    // Written registers are unneeded
    u|=1LL<<rt1[i];
    u|=1LL<<rt2[i];
    uu|=1LL<<rt1[i];
    uu|=1LL<<rt2[i];
    // Accessed registers are needed
    u&=~(1LL<<rs1[i]);
    u&=~(1LL<<rs2[i]);
    uu&=~(1LL<<us1[i]);
    uu&=~(1LL<<us2[i]);
    // Source-target dependencies
    uu&=~(tdep<<dep1[i]);
    uu&=~(tdep<<dep2[i]);
    // R0 is always unneeded
    u|=1;uu|=1;
    // Save it
    unneeded_reg[i]=u;
    unneeded_reg_upper[i]=uu;
    /*
    DebugMessage(M64MSG_VERBOSE, "ur (%d,%d) %x: ",istart,iend,start+i*4);
    DebugMessage(M64MSG_VERBOSE, "U:");
    int r;
    for(r=1;r<=CCREG;r++) {
      if((unneeded_reg[i]>>r)&1) {
        if(r==HIREG) DebugMessage(M64MSG_VERBOSE, " HI");
        else if(r==LOREG) DebugMessage(M64MSG_VERBOSE, " LO");
        else DebugMessage(M64MSG_VERBOSE, " r%d",r);
      }
    }
    DebugMessage(M64MSG_VERBOSE, " UU:");
    for(r=1;r<=CCREG;r++) {
      if(((unneeded_reg_upper[i]&~unneeded_reg[i])>>r)&1) {
        if(r==HIREG) DebugMessage(M64MSG_VERBOSE, " HI");
        else if(r==LOREG) DebugMessage(M64MSG_VERBOSE, " LO");
        else DebugMessage(M64MSG_VERBOSE, " r%d",r);
      }
    }*/
  }
}

// Identify registers which are likely to contain 32-bit values
// This is used to predict whether any branches will jump to a
// location with 64-bit values in registers.
static void provisional_32bit(void)
{
  int i,j;
  uint64_t is32=1;
  uint64_t lastbranch=1;

  for(i=0;i<slen;i++)
  {
    if(i>0) {
      if(itype[i-1]==CJUMP||itype[i-1]==SJUMP||itype[i-1]==FJUMP) {
        if(i>1) is32=lastbranch;
        else is32=1;
      }
    }
    if(i>1)
    {
      if(itype[i-2]==CJUMP||itype[i-2]==SJUMP||itype[i-2]==FJUMP) {
        if(likely[i-2]) {
          if(i>2) is32=lastbranch;
          else is32=1;
        }
      }
      if((opcode[i-2]&0x2f)==0x05) // BNE/BNEL
      {
        if(rs1[i-2]==0||rs2[i-2]==0)
        {
          if(rs1[i-2]) {
            is32|=1LL<<rs1[i-2];
          }
          if(rs2[i-2]) {
            is32|=1LL<<rs2[i-2];
          }
        }
      }
    }
    // If something jumps here with 64-bit values
    // then promote those registers to 64 bits
    if(bt[i])
    {
      uint64_t temp_is32=is32;
      for(j=i-1;j>=0;j--)
      {
        if(ba[j]==start+i*4)
          //temp_is32&=branch_regs[j].is32;
          temp_is32&=p32[j];
      }
      for(j=i;j<slen;j++)
      {
        if(ba[j]==start+i*4)
          temp_is32=1;
      }
      is32=temp_is32;
    }
    int type=itype[i];
    int op=opcode[i];
    int op2=opcode2[i];
    int rt=rt1[i];
    int s1=rs1[i];
    int s2=rs2[i];
    if(type==UJUMP||type==RJUMP||type==CJUMP||type==SJUMP||type==FJUMP) {
      // Branches don't write registers, consider the delay slot instead.
      type=itype[i+1];
      op=opcode[i+1];
      op2=opcode2[i+1];
      rt=rt1[i+1];
      s1=rs1[i+1];
      s2=rs2[i+1];
      lastbranch=is32;
    }
    switch(type) {
      case LOAD:
        if(opcode[i]==0x27||opcode[i]==0x37|| // LWU/LD
           opcode[i]==0x1A||opcode[i]==0x1B) // LDL/LDR
          is32&=~(1LL<<rt);
        else
          is32|=1LL<<rt;
        break;
      case STORE:
      case STORELR:
        break;
      case LOADLR:
        if(op==0x1a||op==0x1b) is32&=~(1LL<<rt); // LDR/LDL
        if(op==0x22) is32|=1LL<<rt; // LWL
        break;
      case IMM16:
        if (op==0x08||op==0x09|| // ADDI/ADDIU
            op==0x0a||op==0x0b|| // SLTI/SLTIU
            op==0x0c|| // ANDI
            op==0x0f)  // LUI
        {
          is32|=1LL<<rt;
        }
        if(op==0x18||op==0x19) { // DADDI/DADDIU
          is32&=~(1LL<<rt);
          //if(imm[i]==0)
          //  is32|=((is32>>s1)&1LL)<<rt;
        }
        if(op==0x0d||op==0x0e) { // ORI/XORI
          uint64_t sr=((is32>>s1)&1LL);
          is32&=~(1LL<<rt);
          is32|=sr<<rt;
        }
        break;
      case UJUMP:
        break;
      case RJUMP:
        break;
      case CJUMP:
        break;
      case SJUMP:
        break;
      case FJUMP:
        break;
      case ALU:
        if(op2>=0x20&&op2<=0x23) { // ADD/ADDU/SUB/SUBU
          is32|=1LL<<rt;
        }
        if(op2==0x2a||op2==0x2b) { // SLT/SLTU
          is32|=1LL<<rt;
        }
        else if(op2>=0x24&&op2<=0x27) { // AND/OR/XOR/NOR
          uint64_t sr=((is32>>s1)&(is32>>s2)&1LL);
          is32&=~(1LL<<rt);
          is32|=sr<<rt;
        }
        else if(op2>=0x2c&&op2<=0x2d) { // DADD/DADDU
          if(s1==0&&s2==0) {
            is32|=1LL<<rt;
          }
          else if(s2==0) {
            uint64_t sr=((is32>>s1)&1LL);
            is32&=~(1LL<<rt);
            is32|=sr<<rt;
          }
          else if(s1==0) {
            uint64_t sr=((is32>>s2)&1LL);
            is32&=~(1LL<<rt);
            is32|=sr<<rt;
          }
          else {
            is32&=~(1LL<<rt);
          }
        }
        else if(op2>=0x2e&&op2<=0x2f) { // DSUB/DSUBU
          if(s1==0&&s2==0) {
            is32|=1LL<<rt;
          }
          else if(s2==0) {
            uint64_t sr=((is32>>s1)&1LL);
            is32&=~(1LL<<rt);
            is32|=sr<<rt;
          }
          else {
            is32&=~(1LL<<rt);
          }
        }
        break;
      case MULTDIV:
        if (op2>=0x1c&&op2<=0x1f) { // DMULT/DMULTU/DDIV/DDIVU
          is32&=~((1LL<<HIREG)|(1LL<<LOREG));
        }
        else {
          is32|=(1LL<<HIREG)|(1LL<<LOREG);
        }
        break;
      case MOV:
        {
          uint64_t sr=((is32>>s1)&1LL);
          is32&=~(1LL<<rt);
          is32|=sr<<rt;
        }
        break;
      case SHIFT:
        if(op2>=0x14&&op2<=0x17) is32&=~(1LL<<rt); // DSLLV/DSRLV/DSRAV
        else is32|=1LL<<rt; // SLLV/SRLV/SRAV
        break;
      case SHIFTIMM:
        is32|=1LL<<rt;
        // DSLL/DSRL/DSRA/DSLL32/DSRL32 but not DSRA32 have 64-bit result
        if(op2>=0x38&&op2<0x3f) is32&=~(1LL<<rt);
        break;
      case COP0:
        if(op2==0) is32|=1LL<<rt; // MFC0
        break;
      case COP1:
        if(op2==0) is32|=1LL<<rt; // MFC1
        if(op2==1) is32&=~(1LL<<rt); // DMFC1
        if(op2==2) is32|=1LL<<rt; // CFC1
        break;
      case C1LS:
        break;
      case FLOAT:
      case FCONV:
        break;
      case FCOMP:
        break;
      case SYSCALL:
        break;
      default:
        break;
    }
    is32|=1;
    p32[i]=is32;

    if(i>0)
    {
      if(itype[i-1]==UJUMP||itype[i-1]==RJUMP||(source[i-1]>>16)==0x1000)
      {
        if(rt1[i-1]==31) // JAL/JALR
        {
          // Subroutine call will return here, don't alloc any registers
          is32=1;
        }
        else if(i+1<slen)
        {
          // Internal branch will jump here, match registers to caller
          is32=0x3FFFFFFFFLL;
        }
      }
    }
  }
}

// Identify registers which may be assumed to contain 32-bit values
// and where optimizations will rely on this.
// This is used to determine whether backward branches can safely
// jump to a location with 64-bit values in registers.
static void provisional_r32(void)
{
  u_int r32=0;
  int i;

  for (i=slen-1;i>=0;i--)
  {
    int hr;
    if(itype[i]==RJUMP||itype[i]==UJUMP||itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP)
    {
      if(ba[i]<start || ba[i]>=(start+slen*4))
      {
        // Branch out of this block, don't need anything
        r32=0;
      }
      else
      {
        // Internal branch
        // Need whatever matches the target
        // (and doesn't get overwritten by the delay slot instruction)
        r32=0;
        int t=(ba[i]-start)>>2;
        if(ba[i]>start+i*4) {
          // Forward branch
          //if(!(requires_32bit[t]&~regs[i].was32))
          //  r32|=requires_32bit[t]&(~(1LL<<rt1[i+1]))&(~(1LL<<rt2[i+1]));
          if(!(pr32[t]&~regs[i].was32))
            r32|=pr32[t]&(~(1LL<<rt1[i+1]))&(~(1LL<<rt2[i+1]));
        }else{
          // Backward branch
          if(!(regs[t].was32&~unneeded_reg_upper[t]&~regs[i].was32))
            r32|=regs[t].was32&~unneeded_reg_upper[t]&(~(1LL<<rt1[i+1]))&(~(1LL<<rt2[i+1]));
        }
      }
      // Conditional branch may need registers for following instructions
      if(itype[i]!=RJUMP&&itype[i]!=UJUMP&&(source[i]>>16)!=0x1000)
      {
        if(i<slen-2) {
          //r32|=requires_32bit[i+2];
          r32|=pr32[i+2];
          r32&=regs[i].was32;
          // Mark this address as a branch target since it may be called
          // upon return from interrupt
          //bt[i+2]=1;
        }
      }
      // Merge in delay slot
      if(!likely[i]) {
        // These are overwritten unless the branch is "likely"
        // and the delay slot is nullified if not taken
        r32&=~(1LL<<rt1[i+1]);
        r32&=~(1LL<<rt2[i+1]);
      }
      // Assume these are needed (delay slot)
      if(us1[i+1]>0)
      {
        if((regs[i].was32>>us1[i+1])&1) r32|=1LL<<us1[i+1];
      }
      if(us2[i+1]>0)
      {
        if((regs[i].was32>>us2[i+1])&1) r32|=1LL<<us2[i+1];
      }
      if(dep1[i+1]&&!((unneeded_reg_upper[i]>>dep1[i+1])&1))
      {
        if((regs[i].was32>>dep1[i+1])&1) r32|=1LL<<dep1[i+1];
      }
      if(dep2[i+1]&&!((unneeded_reg_upper[i]>>dep2[i+1])&1))
      {
        if((regs[i].was32>>dep2[i+1])&1) r32|=1LL<<dep2[i+1];
      }
    }
    else if(itype[i]==SYSCALL)
    {
      // SYSCALL instruction (software interrupt)
      r32=0;
    }
    else if(itype[i]==COP0 && (source[i]&0x3f)==0x18)
    {
      // ERET instruction (return from interrupt)
      r32=0;
    }
    // Check 32 bits
    r32&=~(1LL<<rt1[i]);
    r32&=~(1LL<<rt2[i]);
    if(us1[i]>0)
    {
      if((regs[i].was32>>us1[i])&1) r32|=1LL<<us1[i];
    }
    if(us2[i]>0)
    {
      if((regs[i].was32>>us2[i])&1) r32|=1LL<<us2[i];
    }
    if(dep1[i]&&!((unneeded_reg_upper[i]>>dep1[i])&1))
    {
      if((regs[i].was32>>dep1[i])&1) r32|=1LL<<dep1[i];
    }
    if(dep2[i]&&!((unneeded_reg_upper[i]>>dep2[i])&1))
    {
      if((regs[i].was32>>dep2[i])&1) r32|=1LL<<dep2[i];
    }
    //requires_32bit[i]=r32;
    pr32[i]=r32;

    // Dirty registers which are 32-bit, require 32-bit input
    // as they will be written as 32-bit values
    for(hr=0;hr<HOST_REGS;hr++)
    {
      if(regs[i].regmap_entry[hr]>0&&regs[i].regmap_entry[hr]<64) {
        if((regs[i].was32>>regs[i].regmap_entry[hr])&(regs[i].wasdirty>>hr)&1) {
          if(!((unneeded_reg_upper[i]>>regs[i].regmap_entry[hr])&1))
          pr32[i]|=1LL<<regs[i].regmap_entry[hr];
          //requires_32bit[i]|=1LL<<regs[i].regmap_entry[hr];
        }
      }
    }
  }
}

// Write back dirty registers as soon as we will no longer modify them,
// so that we don't end up with lots of writes at the branches.
static void clean_registers(int istart,int iend,int wr)
{
  int i;
  int r;
  u_int will_dirty_i,will_dirty_next,temp_will_dirty;
  u_int wont_dirty_i,wont_dirty_next,temp_wont_dirty;
  if(iend==slen-1) {
    will_dirty_i=will_dirty_next=0;
    wont_dirty_i=wont_dirty_next=0;
  }else{
    will_dirty_i=will_dirty_next=will_dirty[iend+1];
    wont_dirty_i=wont_dirty_next=wont_dirty[iend+1];
  }
  for (i=iend;i>=istart;i--)
  {
    if(itype[i]==RJUMP||itype[i]==UJUMP||itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP)
    {
      if(ba[i]<start || ba[i]>=(start+slen*4))
      {
        // Branch out of this block, flush all regs
        if(itype[i]==RJUMP||itype[i]==UJUMP||(source[i]>>16)==0x1000)
        {
          // Unconditional branch
          will_dirty_i=0;
          wont_dirty_i=0;
          // Merge in delay slot (will dirty)
          for(r=0;r<HOST_REGS;r++) {
            if(r!=EXCLUDE_REG) {
              if((branch_regs[i].regmap[r]&63)==rt1[i]) will_dirty_i|=1<<r;
              if((branch_regs[i].regmap[r]&63)==rt2[i]) will_dirty_i|=1<<r;
              if((branch_regs[i].regmap[r]&63)==rt1[i+1]) will_dirty_i|=1<<r;
              if((branch_regs[i].regmap[r]&63)==rt2[i+1]) will_dirty_i|=1<<r;
              if((branch_regs[i].regmap[r]&63)>33) will_dirty_i&=~(1<<r);
              if(branch_regs[i].regmap[r]<=0) will_dirty_i&=~(1<<r);
              if(branch_regs[i].regmap[r]==CCREG) will_dirty_i|=1<<r;
              if((regs[i].regmap[r]&63)==rt1[i]) will_dirty_i|=1<<r;
              if((regs[i].regmap[r]&63)==rt2[i]) will_dirty_i|=1<<r;
              if((regs[i].regmap[r]&63)==rt1[i+1]) will_dirty_i|=1<<r;
              if((regs[i].regmap[r]&63)==rt2[i+1]) will_dirty_i|=1<<r;
              if((regs[i].regmap[r]&63)>33) will_dirty_i&=~(1<<r);
              if(regs[i].regmap[r]<=0) will_dirty_i&=~(1<<r);
              if(regs[i].regmap[r]==CCREG) will_dirty_i|=1<<r;
            }
          }
        }
        else
        {
          // Conditional branch
          will_dirty_i=0;
          wont_dirty_i=wont_dirty_next;
          // Merge in delay slot (will dirty)
          for(r=0;r<HOST_REGS;r++) {
            if(r!=EXCLUDE_REG) {
              if(!likely[i]) {
                // Might not dirty if likely branch is not taken
                if((branch_regs[i].regmap[r]&63)==rt1[i]) will_dirty_i|=1<<r;
                if((branch_regs[i].regmap[r]&63)==rt2[i]) will_dirty_i|=1<<r;
                if((branch_regs[i].regmap[r]&63)==rt1[i+1]) will_dirty_i|=1<<r;
                if((branch_regs[i].regmap[r]&63)==rt2[i+1]) will_dirty_i|=1<<r;
                if((branch_regs[i].regmap[r]&63)>33) will_dirty_i&=~(1<<r);
                if(branch_regs[i].regmap[r]==0) will_dirty_i&=~(1<<r);
                if(branch_regs[i].regmap[r]==CCREG) will_dirty_i|=1<<r;
                //if((regs[i].regmap[r]&63)==rt1[i]) will_dirty_i|=1<<r;
                //if((regs[i].regmap[r]&63)==rt2[i]) will_dirty_i|=1<<r;
                if((regs[i].regmap[r]&63)==rt1[i+1]) will_dirty_i|=1<<r;
                if((regs[i].regmap[r]&63)==rt2[i+1]) will_dirty_i|=1<<r;
                if((regs[i].regmap[r]&63)>33) will_dirty_i&=~(1<<r);
                if(regs[i].regmap[r]<=0) will_dirty_i&=~(1<<r);
                if(regs[i].regmap[r]==CCREG) will_dirty_i|=1<<r;
              }
            }
          }
        }
        // Merge in delay slot (wont dirty)
        for(r=0;r<HOST_REGS;r++) {
          if(r!=EXCLUDE_REG) {
            if((regs[i].regmap[r]&63)==rt1[i]) wont_dirty_i|=1<<r;
            if((regs[i].regmap[r]&63)==rt2[i]) wont_dirty_i|=1<<r;
            if((regs[i].regmap[r]&63)==rt1[i+1]) wont_dirty_i|=1<<r;
            if((regs[i].regmap[r]&63)==rt2[i+1]) wont_dirty_i|=1<<r;
            if(regs[i].regmap[r]==CCREG) wont_dirty_i|=1<<r;
            if((branch_regs[i].regmap[r]&63)==rt1[i]) wont_dirty_i|=1<<r;
            if((branch_regs[i].regmap[r]&63)==rt2[i]) wont_dirty_i|=1<<r;
            if((branch_regs[i].regmap[r]&63)==rt1[i+1]) wont_dirty_i|=1<<r;
            if((branch_regs[i].regmap[r]&63)==rt2[i+1]) wont_dirty_i|=1<<r;
            if(branch_regs[i].regmap[r]==CCREG) wont_dirty_i|=1<<r;
          }
        }
        if(wr) {
          #ifndef DESTRUCTIVE_WRITEBACK
          branch_regs[i].dirty&=wont_dirty_i;
          #endif
          branch_regs[i].dirty|=will_dirty_i;
        }
      }
      else
      {
        // Internal branch
        if(ba[i]<=start+i*4) {
          // Backward branch
          if(itype[i]==RJUMP||itype[i]==UJUMP||(source[i]>>16)==0x1000)
          {
            // Unconditional branch
            temp_will_dirty=0;
            temp_wont_dirty=0;
            // Merge in delay slot (will dirty)
            for(r=0;r<HOST_REGS;r++) {
              if(r!=EXCLUDE_REG) {
                if((branch_regs[i].regmap[r]&63)==rt1[i]) temp_will_dirty|=1<<r;
                if((branch_regs[i].regmap[r]&63)==rt2[i]) temp_will_dirty|=1<<r;
                if((branch_regs[i].regmap[r]&63)==rt1[i+1]) temp_will_dirty|=1<<r;
                if((branch_regs[i].regmap[r]&63)==rt2[i+1]) temp_will_dirty|=1<<r;
                if((branch_regs[i].regmap[r]&63)>33) temp_will_dirty&=~(1<<r);
                if(branch_regs[i].regmap[r]<=0) temp_will_dirty&=~(1<<r);
                if(branch_regs[i].regmap[r]==CCREG) temp_will_dirty|=1<<r;
                if((regs[i].regmap[r]&63)==rt1[i]) temp_will_dirty|=1<<r;
                if((regs[i].regmap[r]&63)==rt2[i]) temp_will_dirty|=1<<r;
                if((regs[i].regmap[r]&63)==rt1[i+1]) temp_will_dirty|=1<<r;
                if((regs[i].regmap[r]&63)==rt2[i+1]) temp_will_dirty|=1<<r;
                if((regs[i].regmap[r]&63)>33) temp_will_dirty&=~(1<<r);
                if(regs[i].regmap[r]<=0) temp_will_dirty&=~(1<<r);
                if(regs[i].regmap[r]==CCREG) temp_will_dirty|=1<<r;
              }
            }
          } else {
            // Conditional branch (not taken case)
            temp_will_dirty=will_dirty_next;
            temp_wont_dirty=wont_dirty_next;
            // Merge in delay slot (will dirty)
            for(r=0;r<HOST_REGS;r++) {
              if(r!=EXCLUDE_REG) {
                if(!likely[i]) {
                  // Will not dirty if likely branch is not taken
                  if((branch_regs[i].regmap[r]&63)==rt1[i]) temp_will_dirty|=1<<r;
                  if((branch_regs[i].regmap[r]&63)==rt2[i]) temp_will_dirty|=1<<r;
                  if((branch_regs[i].regmap[r]&63)==rt1[i+1]) temp_will_dirty|=1<<r;
                  if((branch_regs[i].regmap[r]&63)==rt2[i+1]) temp_will_dirty|=1<<r;
                  if((branch_regs[i].regmap[r]&63)>33) temp_will_dirty&=~(1<<r);
                  if(branch_regs[i].regmap[r]==0) temp_will_dirty&=~(1<<r);
                  if(branch_regs[i].regmap[r]==CCREG) temp_will_dirty|=1<<r;
                  //if((regs[i].regmap[r]&63)==rt1[i]) temp_will_dirty|=1<<r;
                  //if((regs[i].regmap[r]&63)==rt2[i]) temp_will_dirty|=1<<r;
                  if((regs[i].regmap[r]&63)==rt1[i+1]) temp_will_dirty|=1<<r;
                  if((regs[i].regmap[r]&63)==rt2[i+1]) temp_will_dirty|=1<<r;
                  if((regs[i].regmap[r]&63)>33) temp_will_dirty&=~(1<<r);
                  if(regs[i].regmap[r]<=0) temp_will_dirty&=~(1<<r);
                  if(regs[i].regmap[r]==CCREG) temp_will_dirty|=1<<r;
                }
              }
            }
          }
          // Merge in delay slot (wont dirty)
          for(r=0;r<HOST_REGS;r++) {
            if(r!=EXCLUDE_REG) {
              if((regs[i].regmap[r]&63)==rt1[i]) temp_wont_dirty|=1<<r;
              if((regs[i].regmap[r]&63)==rt2[i]) temp_wont_dirty|=1<<r;
              if((regs[i].regmap[r]&63)==rt1[i+1]) temp_wont_dirty|=1<<r;
              if((regs[i].regmap[r]&63)==rt2[i+1]) temp_wont_dirty|=1<<r;
              if(regs[i].regmap[r]==CCREG) temp_wont_dirty|=1<<r;
              if((branch_regs[i].regmap[r]&63)==rt1[i]) temp_wont_dirty|=1<<r;
              if((branch_regs[i].regmap[r]&63)==rt2[i]) temp_wont_dirty|=1<<r;
              if((branch_regs[i].regmap[r]&63)==rt1[i+1]) temp_wont_dirty|=1<<r;
              if((branch_regs[i].regmap[r]&63)==rt2[i+1]) temp_wont_dirty|=1<<r;
              if(branch_regs[i].regmap[r]==CCREG) temp_wont_dirty|=1<<r;
            }
          }
          // Deal with changed mappings
          if(i<iend) {
            for(r=0;r<HOST_REGS;r++) {
              if(r!=EXCLUDE_REG) {
                if(regs[i].regmap[r]!=regmap_pre[i][r]) {
                  temp_will_dirty&=~(1<<r);
                  temp_wont_dirty&=~(1<<r);
                  if((regmap_pre[i][r]&63)>0 && (regmap_pre[i][r]&63)<CSREG) {
                    temp_will_dirty|=((unneeded_reg[i]>>(regmap_pre[i][r]&63))&1)<<r;
                    temp_wont_dirty|=((unneeded_reg[i]>>(regmap_pre[i][r]&63))&1)<<r;
                  } else {
                    temp_will_dirty|=1<<r;
                    temp_wont_dirty|=1<<r;
                  }
                }
              }
            }
          }
          if(wr) {
            will_dirty[i]=temp_will_dirty;
            wont_dirty[i]=temp_wont_dirty;
            clean_registers((ba[i]-start)>>2,i-1,0);
          }else{
            // Limit recursion.  It can take an excessive amount
            // of time if there are a lot of nested loops.
            will_dirty[(ba[i]-start)>>2]=0;
            wont_dirty[(ba[i]-start)>>2]=-1;
          }
        }
        /*else*/ if(1)
        {
          if(itype[i]==RJUMP||itype[i]==UJUMP||(source[i]>>16)==0x1000)
          {
            // Unconditional branch
            will_dirty_i=0;
            wont_dirty_i=0;
          //if(ba[i]>start+i*4) { // Disable recursion (for debugging)
            for(r=0;r<HOST_REGS;r++) {
              if(r!=EXCLUDE_REG) {
                if(branch_regs[i].regmap[r]==regs[(ba[i]-start)>>2].regmap_entry[r]) {
                  will_dirty_i|=will_dirty[(ba[i]-start)>>2]&(1<<r);
                  wont_dirty_i|=wont_dirty[(ba[i]-start)>>2]&(1<<r);
                }
                if(branch_regs[i].regmap[r]>=0) {
                  will_dirty_i|=((unneeded_reg[(ba[i]-start)>>2]>>(branch_regs[i].regmap[r]&63))&1)<<r;
                  wont_dirty_i|=((unneeded_reg[(ba[i]-start)>>2]>>(branch_regs[i].regmap[r]&63))&1)<<r;
                }
              }
            }
          //}
            // Merge in delay slot
            for(r=0;r<HOST_REGS;r++) {
              if(r!=EXCLUDE_REG) {
                if((branch_regs[i].regmap[r]&63)==rt1[i]) will_dirty_i|=1<<r;
                if((branch_regs[i].regmap[r]&63)==rt2[i]) will_dirty_i|=1<<r;
                if((branch_regs[i].regmap[r]&63)==rt1[i+1]) will_dirty_i|=1<<r;
                if((branch_regs[i].regmap[r]&63)==rt2[i+1]) will_dirty_i|=1<<r;
                if((branch_regs[i].regmap[r]&63)>33) will_dirty_i&=~(1<<r);
                if(branch_regs[i].regmap[r]<=0) will_dirty_i&=~(1<<r);
                if(branch_regs[i].regmap[r]==CCREG) will_dirty_i|=1<<r;
                if((regs[i].regmap[r]&63)==rt1[i]) will_dirty_i|=1<<r;
                if((regs[i].regmap[r]&63)==rt2[i]) will_dirty_i|=1<<r;
                if((regs[i].regmap[r]&63)==rt1[i+1]) will_dirty_i|=1<<r;
                if((regs[i].regmap[r]&63)==rt2[i+1]) will_dirty_i|=1<<r;
                if((regs[i].regmap[r]&63)>33) will_dirty_i&=~(1<<r);
                if(regs[i].regmap[r]<=0) will_dirty_i&=~(1<<r);
                if(regs[i].regmap[r]==CCREG) will_dirty_i|=1<<r;
              }
            }
          } else {
            // Conditional branch
            will_dirty_i=will_dirty_next;
            wont_dirty_i=wont_dirty_next;
          //if(ba[i]>start+i*4) { // Disable recursion (for debugging)
            for(r=0;r<HOST_REGS;r++) {
              if(r!=EXCLUDE_REG) {
                signed char target_reg=branch_regs[i].regmap[r];
                if(target_reg==regs[(ba[i]-start)>>2].regmap_entry[r]) {
                  will_dirty_i&=will_dirty[(ba[i]-start)>>2]&(1<<r);
                  wont_dirty_i|=wont_dirty[(ba[i]-start)>>2]&(1<<r);
                }
                else if(target_reg>=0) {
                  will_dirty_i&=((unneeded_reg[(ba[i]-start)>>2]>>(target_reg&63))&1)<<r;
                  wont_dirty_i|=((unneeded_reg[(ba[i]-start)>>2]>>(target_reg&63))&1)<<r;
                }
                // Treat delay slot as part of branch too
                /*if(regs[i+1].regmap[r]==regs[(ba[i]-start)>>2].regmap_entry[r]) {
                  will_dirty[i+1]&=will_dirty[(ba[i]-start)>>2]&(1<<r);
                  wont_dirty[i+1]|=wont_dirty[(ba[i]-start)>>2]&(1<<r);
                }
                else
                {
                  will_dirty[i+1]&=~(1<<r);
                }*/
              }
            }
          //}
            // Merge in delay slot
            for(r=0;r<HOST_REGS;r++) {
              if(r!=EXCLUDE_REG) {
                if(!likely[i]) {
                  // Might not dirty if likely branch is not taken
                  if((branch_regs[i].regmap[r]&63)==rt1[i]) will_dirty_i|=1<<r;
                  if((branch_regs[i].regmap[r]&63)==rt2[i]) will_dirty_i|=1<<r;
                  if((branch_regs[i].regmap[r]&63)==rt1[i+1]) will_dirty_i|=1<<r;
                  if((branch_regs[i].regmap[r]&63)==rt2[i+1]) will_dirty_i|=1<<r;
                  if((branch_regs[i].regmap[r]&63)>33) will_dirty_i&=~(1<<r);
                  if(branch_regs[i].regmap[r]<=0) will_dirty_i&=~(1<<r);
                  if(branch_regs[i].regmap[r]==CCREG) will_dirty_i|=1<<r;
                  //if((regs[i].regmap[r]&63)==rt1[i]) will_dirty_i|=1<<r;
                  //if((regs[i].regmap[r]&63)==rt2[i]) will_dirty_i|=1<<r;
                  if((regs[i].regmap[r]&63)==rt1[i+1]) will_dirty_i|=1<<r;
                  if((regs[i].regmap[r]&63)==rt2[i+1]) will_dirty_i|=1<<r;
                  if((regs[i].regmap[r]&63)>33) will_dirty_i&=~(1<<r);
                  if(regs[i].regmap[r]<=0) will_dirty_i&=~(1<<r);
                  if(regs[i].regmap[r]==CCREG) will_dirty_i|=1<<r;
                }
              }
            }
          }
          // Merge in delay slot (won't dirty)
          for(r=0;r<HOST_REGS;r++) {
            if(r!=EXCLUDE_REG) {
              if((regs[i].regmap[r]&63)==rt1[i]) wont_dirty_i|=1<<r;
              if((regs[i].regmap[r]&63)==rt2[i]) wont_dirty_i|=1<<r;
              if((regs[i].regmap[r]&63)==rt1[i+1]) wont_dirty_i|=1<<r;
              if((regs[i].regmap[r]&63)==rt2[i+1]) wont_dirty_i|=1<<r;
              if(regs[i].regmap[r]==CCREG) wont_dirty_i|=1<<r;
              if((branch_regs[i].regmap[r]&63)==rt1[i]) wont_dirty_i|=1<<r;
              if((branch_regs[i].regmap[r]&63)==rt2[i]) wont_dirty_i|=1<<r;
              if((branch_regs[i].regmap[r]&63)==rt1[i+1]) wont_dirty_i|=1<<r;
              if((branch_regs[i].regmap[r]&63)==rt2[i+1]) wont_dirty_i|=1<<r;
              if(branch_regs[i].regmap[r]==CCREG) wont_dirty_i|=1<<r;
            }
          }
          if(wr) {
            #ifndef DESTRUCTIVE_WRITEBACK
            branch_regs[i].dirty&=wont_dirty_i;
            #endif
            branch_regs[i].dirty|=will_dirty_i;
          }
        }
      }
    }
    else if(itype[i]==SYSCALL)
    {
      // SYSCALL instruction (software interrupt)
      will_dirty_i=0;
      wont_dirty_i=0;
    }
    else if(itype[i]==COP0 && (source[i]&0x3f)==0x18)
    {
      // ERET instruction (return from interrupt)
      will_dirty_i=0;
      wont_dirty_i=0;
    }
    will_dirty_next=will_dirty_i;
    wont_dirty_next=wont_dirty_i;
    for(r=0;r<HOST_REGS;r++) {
      if(r!=EXCLUDE_REG) {
        if((regs[i].regmap[r]&63)==rt1[i]) will_dirty_i|=1<<r;
        if((regs[i].regmap[r]&63)==rt2[i]) will_dirty_i|=1<<r;
        if((regs[i].regmap[r]&63)>33) will_dirty_i&=~(1<<r);
        if(regs[i].regmap[r]<=0) will_dirty_i&=~(1<<r);
        if(regs[i].regmap[r]==CCREG) will_dirty_i|=1<<r;
        if((regs[i].regmap[r]&63)==rt1[i]) wont_dirty_i|=1<<r;
        if((regs[i].regmap[r]&63)==rt2[i]) wont_dirty_i|=1<<r;
        if(regs[i].regmap[r]==CCREG) wont_dirty_i|=1<<r;
        if(i>istart) {
          if(itype[i]!=RJUMP&&itype[i]!=UJUMP&&itype[i]!=CJUMP&&itype[i]!=SJUMP&&itype[i]!=FJUMP)
          {
            // Don't store a register immediately after writing it,
            // may prevent dual-issue.
            if((regs[i].regmap[r]&63)==rt1[i-1]) wont_dirty_i|=1<<r;
            if((regs[i].regmap[r]&63)==rt2[i-1]) wont_dirty_i|=1<<r;
          }
        }
      }
    }
    // Save it
    will_dirty[i]=will_dirty_i;
    wont_dirty[i]=wont_dirty_i;
    // Mark registers that won't be dirtied as not dirty
    if(wr) {
      /*DebugMessage(M64MSG_VERBOSE, "wr (%d,%d) %x will:",istart,iend,start+i*4);
      for(r=0;r<HOST_REGS;r++) {
        if((will_dirty_i>>r)&1) {
          DebugMessage(M64MSG_VERBOSE, " r%d",r);
        }
      }*/

      //if(i==istart||(itype[i-1]!=RJUMP&&itype[i-1]!=UJUMP&&itype[i-1]!=CJUMP&&itype[i-1]!=SJUMP&&itype[i-1]!=FJUMP)) {
        regs[i].dirty|=will_dirty_i;
        #ifndef DESTRUCTIVE_WRITEBACK
        regs[i].dirty&=wont_dirty_i;
        if(itype[i]==RJUMP||itype[i]==UJUMP||itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP)
        {
          if(i<iend-1&&itype[i]!=RJUMP&&itype[i]!=UJUMP&&(source[i]>>16)!=0x1000) {
            for(r=0;r<HOST_REGS;r++) {
              if(r!=EXCLUDE_REG) {
                if(regs[i].regmap[r]==regmap_pre[i+2][r]) {
                  regs[i+2].wasdirty&=wont_dirty_i|~(1<<r);
                }else {/*DebugMessage(M64MSG_VERBOSE, "i: %x (%d) mismatch(+2): %d",start+i*4,i,r); / *assert(!((wont_dirty_i>>r)&1));*/}
              }
            }
          }
        }
        else
        {
          if(i<iend) {
            for(r=0;r<HOST_REGS;r++) {
              if(r!=EXCLUDE_REG) {
                if(regs[i].regmap[r]==regmap_pre[i+1][r]) {
                  regs[i+1].wasdirty&=wont_dirty_i|~(1<<r);
                }else {/*DebugMessage(M64MSG_VERBOSE, "i: %x (%d) mismatch(+1): %d",start+i*4,i,r);/ *assert(!((wont_dirty_i>>r)&1));*/}
              }
            }
          }
        }
        #endif
      //}
    }
    // Deal with changed mappings
    temp_will_dirty=will_dirty_i;
    temp_wont_dirty=wont_dirty_i;
    for(r=0;r<HOST_REGS;r++) {
      if(r!=EXCLUDE_REG) {
        int nr;
        if(regs[i].regmap[r]==regmap_pre[i][r]) {
          if(wr) {
            #ifndef DESTRUCTIVE_WRITEBACK
            regs[i].wasdirty&=wont_dirty_i|~(1<<r);
            #endif
            regs[i].wasdirty|=will_dirty_i&(1<<r);
          }
        }
        else if(regmap_pre[i][r]>=0&&(nr=get_reg(regs[i].regmap,regmap_pre[i][r]))>=0) {
          // Register moved to a different register
          will_dirty_i&=~(1<<r);
          wont_dirty_i&=~(1<<r);
          will_dirty_i|=((temp_will_dirty>>nr)&1)<<r;
          wont_dirty_i|=((temp_wont_dirty>>nr)&1)<<r;
          if(wr) {
            #ifndef DESTRUCTIVE_WRITEBACK
            regs[i].wasdirty&=wont_dirty_i|~(1<<r);
            #endif
            regs[i].wasdirty|=will_dirty_i&(1<<r);
          }
        }
        else {
          will_dirty_i&=~(1<<r);
          wont_dirty_i&=~(1<<r);
          if((regmap_pre[i][r]&63)>0 && (regmap_pre[i][r]&63)<CSREG) {
            will_dirty_i|=((unneeded_reg[i]>>(regmap_pre[i][r]&63))&1)<<r;
            wont_dirty_i|=((unneeded_reg[i]>>(regmap_pre[i][r]&63))&1)<<r;
          } else {
            wont_dirty_i|=1<<r;
            /*DebugMessage(M64MSG_VERBOSE, "i: %x (%d) mismatch: %d",start+i*4,i,r);/ *assert(!((will_dirty>>r)&1));*/
          }
        }
      }
    }
  }
}

// Is the branch target a valid internal jump?
static int internal_branch(uint64_t i_is32,int addr)
{
  if(addr&1) return 0; // Indirect (register) jump
  if((u_int)addr>=start && (u_int)addr<start+slen*4-4)
  {
    int t=(addr-start)>>2;
    // Delay slots are not valid branch targets
    //if(t>0&&(itype[t-1]==RJUMP||itype[t-1]==UJUMP||itype[t-1]==CJUMP||itype[t-1]==SJUMP||itype[t-1]==FJUMP)) return 0;
    // 64 -> 32 bit transition requires a recompile
    /*if(is32[t]&~unneeded_reg_upper[t]&~i_is32)
    {
      if(requires_32bit[t]&~i_is32) DebugMessage(M64MSG_VERBOSE, "optimizable: no");
      else DebugMessage(M64MSG_VERBOSE, "optimizable: yes");
    }*/
    //if(is32[t]&~unneeded_reg_upper[t]&~i_is32) return 0;
    if(requires_32bit[t]&~i_is32) return 0;
    else return 1;
  }
  return 0;
}

static int get_final_value(int hr, int i, int *value)
{
  int reg=regs[i].regmap[hr];
  while(i<slen-1) {
    if(regs[i+1].regmap[hr]!=reg) break;
    if(!((regs[i+1].isconst>>hr)&1)) break;
    if(bt[i+1]) break;
    i++;
  }
  if(i<slen-1) {
    if(itype[i]==UJUMP||itype[i]==RJUMP||itype[i]==CJUMP||itype[i]==SJUMP) {
      *value=constmap[i][hr];
      return 1;
    }
  }
  *value=constmap[i][hr];
  //DebugMessage(M64MSG_VERBOSE, "c=%x",(int)constmap[i][hr]);
  if(i==slen-1) return 1;
  if(reg<64) {
    return !((unneeded_reg[i+1]>>reg)&1);
  }else{
    return !((unneeded_reg_upper[i+1]>>reg)&1);
  }
}

static int match_bt(signed char i_regmap[],uint64_t i_is32,uint64_t i_dirty,int addr)
{
  if((u_int)addr>=start && (u_int)addr<start+slen*4-4)
  {
    int t=(addr-start)>>2;
    int hr;
    if(regs[t].regmap_entry[HOST_CCREG]!=CCREG) return 0;
    for(hr=0;hr<HOST_REGS;hr++)
    {
      if(hr!=EXCLUDE_REG)
      {
        if(i_regmap[hr]!=regs[t].regmap_entry[hr])
        {
          if(regs[t].regmap_entry[hr]>=0&&(regs[t].regmap_entry[hr]|64)<TEMPREG+64)
          {
            return 0;
          }
          else
          if((i_dirty>>hr)&1)
          {
            if(i_regmap[hr]<TEMPREG)
            {
              if(!((unneeded_reg[t]>>i_regmap[hr])&1))
                return 0;
            }
            else if(i_regmap[hr]>=64&&i_regmap[hr]<TEMPREG+64)
            {
              if(!((unneeded_reg_upper[t]>>(i_regmap[hr]&63))&1))
                return 0;
            }
          }
        }
        else // Same register but is it 32-bit or dirty?
        if(i_regmap[hr]>=0)
        {
          if(!((regs[t].dirty>>hr)&1))
          {
            if((i_dirty>>hr)&1)
            {
              if(!((unneeded_reg[t]>>i_regmap[hr])&1))
              {
                //DebugMessage(M64MSG_VERBOSE, "%x: dirty no match",addr);
                return 0;
              }
            }
          }
          if((((regs[t].was32^i_is32)&~unneeded_reg_upper[t])>>(i_regmap[hr]&63))&1)
          {
            //DebugMessage(M64MSG_VERBOSE, "%x: is32 no match",addr);
            return 0;
          }
        }
      }
    }
    //if(is32[t]&~unneeded_reg_upper[t]&~i_is32) return 0;
    if(requires_32bit[t]&~i_is32) return 0;
    // Delay slots are not valid branch targets
    //if(t>0&&(itype[t-1]==RJUMP||itype[t-1]==UJUMP||itype[t-1]==CJUMP||itype[t-1]==SJUMP||itype[t-1]==FJUMP)) return 0;
    // Delay slots require additional processing, so do not match
    if(is_ds[t]) return 0;
  }
  else
  {
    int hr;
    for(hr=0;hr<HOST_REGS;hr++)
    {
      if(hr!=EXCLUDE_REG)
      {
        if(i_regmap[hr]>=0)
        {
          if(hr!=HOST_CCREG||i_regmap[hr]!=CCREG)
          {
            if((i_dirty>>hr)&1)
            {
              return 0;
            }
          }
        }
      }
    }
  }
  return 1;
}

// Allocate every register, preserving source/target regs
static void alloc_all(struct regstat *cur,int i)
{
  int hr;

  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG) {
      if(((cur->regmap[hr]&63)!=rs1[i])&&((cur->regmap[hr]&63)!=rs2[i])&&
         ((cur->regmap[hr]&63)!=rt1[i])&&((cur->regmap[hr]&63)!=rt2[i]))
      {
        cur->regmap[hr]=-1;
        cur->dirty&=~(1LL<<hr);
      }
      // Don't need zeros
      if((cur->regmap[hr]&63)==0)
      {
        cur->regmap[hr]=-1;
        cur->dirty&=~(1LL<<hr);
      }
    }
  }
}

static void add_to_linker(intptr_t addr,u_int target,int ext)
{
  assert(linkcount<MAXBLOCK);
  link_addr[linkcount][0]=addr;
  link_addr[linkcount][1]=target;
  link_addr[linkcount][2]=ext;
  linkcount++;
}

static void add_stub(int type,intptr_t addr,intptr_t retaddr,int a,intptr_t b,intptr_t c,int d,int e)
{
  assert(stubcount<MAXBLOCK*3);
  stubs[stubcount][0]=type;
  stubs[stubcount][1]=addr;
  stubs[stubcount][2]=retaddr;
  stubs[stubcount][3]=a;
  stubs[stubcount][4]=b;
  stubs[stubcount][5]=c;
  stubs[stubcount][6]=d;
  stubs[stubcount][7]=e;
  stubcount++;
}

static void remove_hash(u_int vaddr)
{
  //DebugMessage(M64MSG_VERBOSE, "remove hash: %x",vaddr);
  struct ll_entry **ht_bin=hash_table[(((vaddr)>>16)^vaddr)&0xFFFF];
  if(ht_bin[1]&&ht_bin[1]->vaddr==vaddr) {
    ht_bin[1]=NULL;
  }
  if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) {
    ht_bin[0]=ht_bin[1];
    ht_bin[1]=NULL;
  }
}

/**** Interpreted opcodes ****/
#define UPDATE_COUNT_IN \
  struct r4300_core* r4300 = &g_dev.r4300; \
  struct new_dynarec_hot_state* state = &r4300->new_dynarec_hot_state; \
  state->cycle_count += count; \
  state->pending_exception = 0;

#define UPDATE_COUNT_OUT \
  state->cycle_count -= !state->pending_exception * count;

void* SYSCALL_new(void)
{
    struct r4300_core* r4300 = &g_dev.r4300;
    struct new_dynarec_hot_state* state = &r4300->new_dynarec_hot_state;
    r4300->delay_slot = 0;
    cached_interp_SYSCALL();
    return get_addr_ht(state->pcaddr);
}

void* ERET_new(void)
{
    struct r4300_core* r4300 = &g_dev.r4300;
    struct new_dynarec_hot_state* state = &r4300->new_dynarec_hot_state;

    cp0_update_count(r4300);
    if (state->cp0_regs[CP0_STATUS_REG] & CP0_STATUS_ERL)
    {
        DebugMessage(M64MSG_ERROR, "error in ERET");
        state->stop = 1;
    }
    else
    {
        state->cp0_regs[CP0_STATUS_REG] &= ~CP0_STATUS_EXL;
        state->pcaddr = state->cp0_regs[CP0_EPC_REG];
    }
    r4300->llbit = 0;
    r4300->delay_slot = 0;
    r4300_check_interrupt(r4300, CP0_CAUSE_IP2, r4300->mi->regs[MI_INTR_REG] & r4300->mi->regs[MI_INTR_MASK_REG]); // ???
    r4300->cp0.last_addr = state->pcaddr;
    state->pending_exception = 0;
    if (state->cycle_count >= 0) { gen_interrupt(r4300); }

    if(state->stop)
        return NULL;

    if(state->pending_exception)
        return get_addr_ht(state->pcaddr);
    else
    {
        uint32_t is64 = 0;
        for(int i=0;i<32;i++)
            is64 = (((int)(state->regs[i]>>32)^((int)state->regs[i]>>31))!=0)<<i;

        is64 |= (((int)(state->hi>>32)^((int)state->hi>>31))!=0);
        is64 |= (((int)(state->lo>>32)^((int)state->lo>>31))!=0);
        return get_addr_32(state->pcaddr, is64);
    }
}

static void TLBWI_new(int pcaddr, int count)
{
  unsigned int i;
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr;

  /* Remove old entries */
  unsigned int old_start_even=r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].start_even;
  unsigned int old_end_even=r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].end_even;
  unsigned int old_start_odd=r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].start_odd;
  unsigned int old_end_odd=r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].end_odd;
  for (i=old_start_even>>12; i<=old_end_even>>12; i++)
  {
    if(i<0x80000||i>0xBFFFF)
    {
      invalidate_block(i);
      state->memory_map[i]=(uintptr_t)-1;
    }
  }
  for (i=old_start_odd>>12; i<=old_end_odd>>12; i++)
  {
    if(i<0x80000||i>0xBFFFF)
    {
      invalidate_block(i);
      state->memory_map[i]=(uintptr_t)-1;
    }
  }
  cached_interp_TLBWI();
  //DebugMessage(M64MSG_VERBOSE, "TLBWI: index=%d",state->cp0_regs[CP0_INDEX_REG]);
  //DebugMessage(M64MSG_VERBOSE, "TLBWI: start_even=%x end_even=%x phys_even=%x v=%d d=%d",r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].start_even,r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].end_even,r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].phys_even,r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].v_even,r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].d_even);
  //DebugMessage(M64MSG_VERBOSE, "TLBWI: start_odd=%x end_odd=%x phys_odd=%x v=%d d=%d",r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].start_odd,r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].end_odd,r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].phys_odd,r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].v_odd,r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].d_odd);
  /* Combine r4300->cp0.tlb.LUT_r, r4300->cp0.tlb.LUT_w, and invalid_code into a single table
     for fast look up. */
  for (i=r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].start_even>>12; i<=r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].end_even>>12; i++)
  {
    //DebugMessage(M64MSG_VERBOSE, "%x: r:%8x w:%8x",i,r4300->cp0.tlb.LUT_r[i],r4300->cp0.tlb.LUT_w[i]);
    if(i<0x80000||i>0xBFFFF)
    {
      if(r4300->cp0.tlb.LUT_r[i]) {
        state->memory_map[i]=((uintptr_t)g_dev.rdram.dram+(uintptr_t)((r4300->cp0.tlb.LUT_r[i]&0xFFFFF000)-0x80000000)-(i<<12))>>2;
        // FIXME: should make sure the physical page is invalid too
        if(!r4300->cp0.tlb.LUT_w[i]||!r4300->cached_interp.invalid_code[i]) {
          state->memory_map[i]|=WRITE_PROTECT; // Write protect
        }else{
          assert(r4300->cp0.tlb.LUT_r[i]==r4300->cp0.tlb.LUT_w[i]);
        }
        if(!using_tlb) DebugMessage(M64MSG_VERBOSE, "Enabled TLB");
        // Tell the dynamic recompiler to generate tlb lookup code
        using_tlb=1;
      }
      else state->memory_map[i]=(uintptr_t)-1;
    }
    //DebugMessage(M64MSG_VERBOSE, "memory_map[%x]: %8x (+%8x)",i,state->memory_map[i],state->memory_map[i]<<2);
  }
  for (i=r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].start_odd>>12; i<=r4300->cp0.tlb.entries[state->cp0_regs[CP0_INDEX_REG]&0x3F].end_odd>>12; i++)
  {
    //DebugMessage(M64MSG_VERBOSE, "%x: r:%8x w:%8x",i,r4300->cp0.tlb.LUT_r[i],r4300->cp0.tlb.LUT_w[i]);
    if(i<0x80000||i>0xBFFFF)
    {
      if(r4300->cp0.tlb.LUT_r[i]) {
        state->memory_map[i]=((uintptr_t)g_dev.rdram.dram+(uintptr_t)((r4300->cp0.tlb.LUT_r[i]&0xFFFFF000)-0x80000000)-(i<<12))>>2;
        // FIXME: should make sure the physical page is invalid too
        if(!r4300->cp0.tlb.LUT_w[i]||!r4300->cached_interp.invalid_code[i]) {
          state->memory_map[i]|=WRITE_PROTECT; // Write protect
        }else{
          assert(r4300->cp0.tlb.LUT_r[i]==r4300->cp0.tlb.LUT_w[i]);
        }
        if(!using_tlb) DebugMessage(M64MSG_VERBOSE, "Enabled TLB");
        // Tell the dynamic recompiler to generate tlb lookup code
        using_tlb=1;
      }
      else state->memory_map[i]=(uintptr_t)-1;
    }
    //DebugMessage(M64MSG_VERBOSE, "memory_map[%x]: %8x (+%8x)",i,state->memory_map[i],state->memory_map[i]<<2);
  }
  UPDATE_COUNT_OUT
}

static void TLBWR_new(int pcaddr, int count)
{
  unsigned int i;
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr;
  cp0_update_count(r4300);
  state->cp0_regs[CP0_RANDOM_REG] = (state->cp0_regs[CP0_COUNT_REG]/r4300->cp0.count_per_op % (32 - state->cp0_regs[CP0_WIRED_REG])) + state->cp0_regs[CP0_WIRED_REG];
  /* Remove old entries */
  unsigned int old_start_even=r4300->cp0.tlb.entries[state->cp0_regs[CP0_RANDOM_REG]&0x3F].start_even;
  unsigned int old_end_even=r4300->cp0.tlb.entries[state->cp0_regs[CP0_RANDOM_REG]&0x3F].end_even;
  unsigned int old_start_odd=r4300->cp0.tlb.entries[state->cp0_regs[CP0_RANDOM_REG]&0x3F].start_odd;
  unsigned int old_end_odd=r4300->cp0.tlb.entries[state->cp0_regs[CP0_RANDOM_REG]&0x3F].end_odd;
  for (i=old_start_even>>12; i<=old_end_even>>12; i++)
  {
    if(i<0x80000||i>0xBFFFF)
    {
      invalidate_block(i);
      state->memory_map[i]=(uintptr_t)-1;
    }
  }
  for (i=old_start_odd>>12; i<=old_end_odd>>12; i++)
  {
    if(i<0x80000||i>0xBFFFF)
    {
      invalidate_block(i);
      state->memory_map[i]=(uintptr_t)-1;
    }
  }
  cached_interp_TLBWR();
  /* Combine r4300->cp0.tlb.LUT_r, r4300->cp0.tlb.LUT_w, and invalid_code into a single table
     for fast look up. */
  for (i=r4300->cp0.tlb.entries[state->cp0_regs[CP0_RANDOM_REG]&0x3F].start_even>>12; i<=r4300->cp0.tlb.entries[state->cp0_regs[CP0_RANDOM_REG]&0x3F].end_even>>12; i++)
  {
    //DebugMessage(M64MSG_VERBOSE, "%x: r:%8x w:%8x",i,r4300->cp0.tlb.LUT_r[i],r4300->cp0.tlb.LUT_w[i]);
    if(i<0x80000||i>0xBFFFF)
    {
      if(r4300->cp0.tlb.LUT_r[i]) {
        state->memory_map[i]=((uintptr_t)g_dev.rdram.dram+(uintptr_t)((r4300->cp0.tlb.LUT_r[i]&0xFFFFF000)-0x80000000)-(i<<12))>>2;
        // FIXME: should make sure the physical page is invalid too
        if(!r4300->cp0.tlb.LUT_w[i]||!r4300->cached_interp.invalid_code[i]) {
          state->memory_map[i]|=WRITE_PROTECT; // Write protect
        }else{
          assert(r4300->cp0.tlb.LUT_r[i]==r4300->cp0.tlb.LUT_w[i]);
        }
        if(!using_tlb) DebugMessage(M64MSG_VERBOSE, "Enabled TLB");
        // Tell the dynamic recompiler to generate tlb lookup code
        using_tlb=1;
      }
      else state->memory_map[i]=(uintptr_t)-1;
    }
    //DebugMessage(M64MSG_VERBOSE, "memory_map[%x]: %8x (+%8x)",i,state->memory_map[i],state->memory_map[i]<<2);
  }
  for (i=r4300->cp0.tlb.entries[state->cp0_regs[CP0_RANDOM_REG]&0x3F].start_odd>>12; i<=r4300->cp0.tlb.entries[state->cp0_regs[CP0_RANDOM_REG]&0x3F].end_odd>>12; i++)
  {
    //DebugMessage(M64MSG_VERBOSE, "%x: r:%8x w:%8x",i,r4300->cp0.tlb.LUT_r[i],r4300->cp0.tlb.LUT_w[i]);
    if(i<0x80000||i>0xBFFFF)
    {
      if(r4300->cp0.tlb.LUT_r[i]) {
        state->memory_map[i]=((uintptr_t)g_dev.rdram.dram+(uintptr_t)((r4300->cp0.tlb.LUT_r[i]&0xFFFFF000)-0x80000000)-(i<<12))>>2;
        // FIXME: should make sure the physical page is invalid too
        if(!r4300->cp0.tlb.LUT_w[i]||!r4300->cached_interp.invalid_code[i]) {
          state->memory_map[i]|=WRITE_PROTECT; // Write protect
        }else{
          assert(r4300->cp0.tlb.LUT_r[i]==r4300->cp0.tlb.LUT_w[i]);
        }
        if(!using_tlb) DebugMessage(M64MSG_VERBOSE, "Enabled TLB");
        // Tell the dynamic recompiler to generate tlb lookup code
        using_tlb=1;
      }
      else state->memory_map[i]=(uintptr_t)-1;
    }
    //DebugMessage(M64MSG_VERBOSE, "memory_map[%x]: %8x (+%8x)",i,state->memory_map[i],state->memory_map[i]<<2);
  }
  UPDATE_COUNT_OUT
}

static void MFC0_new(int copr, int count)
{
  UPDATE_COUNT_IN
  state->fake_pc.f.r.nrd = copr;
  cached_interp_MFC0();
  UPDATE_COUNT_OUT
}

static void MTC0_new(int copr, int count, int pcaddr)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr;
  r4300->delay_slot = 0;
  state->fake_pc.f.r.nrd = copr;
  cached_interp_MTC0();
  UPDATE_COUNT_OUT
}

#define BITS_BELOW_MASK32(x) ((UINT32_C(1) << (x)) - 1)
#define BITS_ABOVE_MASK32(x) (~(BITS_BELOW_MASK32((x))))

#define BITS_BELOW_MASK64(x) ((UINT64_C(1) << (x)) - 1)
#define BITS_ABOVE_MASK64(x) (~(BITS_BELOW_MASK64((x))))

static unsigned int bshift(uint32_t address)
{
    return ((address & 3) ^ 3) << 3;
}

static unsigned int hshift(uint32_t address)
{
    return ((address & 2) ^ 2) << 3;
}

static void read_byte_new(int pcaddr, int count)
{
  uint32_t value;
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  unsigned int shift = bshift(state->address);
  if (r4300_read_aligned_word(r4300, state->address, &value)) {
    state->rdword = (uint64_t)((value >> shift) & 0xff);
  }
  UPDATE_COUNT_OUT
}

static void read_hword_new(int pcaddr, int count)
{
  uint32_t value;
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  unsigned int shift = hshift(state->address);
  if (r4300_read_aligned_word(r4300, state->address, &value)) {
    state->rdword = (uint64_t)((value >> shift) & 0xffff);
  }
  UPDATE_COUNT_OUT
}

static void read_word_new(int pcaddr, int count)
{
  uint32_t value;
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  if (r4300_read_aligned_word(r4300, state->address, &value)) {
    state->rdword = (uint64_t)(value);
  }
  UPDATE_COUNT_OUT
}

static void read_dword_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  r4300_read_aligned_dword(r4300, state->address, (uint64_t*)&state->rdword);
  UPDATE_COUNT_OUT
}

static void write_byte_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  unsigned int shift = bshift(state->address);
  state->wword <<= shift;
  r4300_write_aligned_word(r4300, state->address, state->wword, UINT32_C(0xff) << shift);
  UPDATE_COUNT_OUT
}

static void write_hword_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  unsigned int shift = hshift(state->address);
  state->wword <<= shift;
  r4300_write_aligned_word(r4300, state->address, state->wword, UINT32_C(0xffff) << shift);
  UPDATE_COUNT_OUT
}

static void write_word_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  r4300_write_aligned_word(r4300, state->address, state->wword, UINT32_C(0xffffffff));
  UPDATE_COUNT_OUT
}

static void write_dword_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  /* NOTE: in dynarec, we only need an all-one mask */
  r4300_write_aligned_dword(r4300, state->address, state->wdword, ~UINT64_C(0));
  UPDATE_COUNT_OUT
}

static void LWL_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  uint32_t value;
  unsigned int n = (state->address & 3);
  unsigned int shift = 8 * n;
  uint32_t mask = BITS_BELOW_MASK32(8 * n);
  if (r4300_read_aligned_word(r4300, state->address & ~UINT32_C(0x3), &value)) {
    state->rdword = (uint64_t)((state->wword & mask) | (value << shift));
  }
  UPDATE_COUNT_OUT
}

static void LWR_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  uint32_t value;
  unsigned int n = (state->address & 3);
  unsigned int shift = 8 * (3 - n);
  uint32_t mask = (n == 3) ? UINT32_C(0) : BITS_ABOVE_MASK32(8 * (n + 1));
  if (r4300_read_aligned_word(r4300, state->address & ~UINT32_C(0x3), &value)) {
    state->rdword = (uint64_t)((state->wword & mask) | (value >> shift));
  }
  UPDATE_COUNT_OUT
}

static void LDL_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  uint64_t value;
  unsigned int n = (state->address & 7);
  unsigned int shift = 8 * n;
  uint64_t mask = BITS_BELOW_MASK64(8 * n);
  if(r4300_read_aligned_dword(r4300, state->address & ~UINT32_C(7), &value)) {
      state->rdword = (state->wdword & mask) | (value << shift);
  }
  UPDATE_COUNT_OUT
}

static void LDR_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  uint64_t value;
  unsigned int n = (state->address & 7);
  unsigned int shift = 8 * (7 - n);
  uint64_t mask = (n == 7) ? UINT64_C(0) : BITS_ABOVE_MASK64(8 * (n + 1));
  if(r4300_read_aligned_dword(r4300, state->address & ~UINT32_C(7), &value)){
      state->rdword = (state->wdword & mask) | (value >> shift);
  }
  UPDATE_COUNT_OUT
}

static void SWL_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  unsigned int n = (state->address & 3);
  unsigned int shift = 8 * n;
  uint32_t mask = (n == 0) ? ~UINT32_C(0) : BITS_BELOW_MASK32(8 * (4 - n));
  r4300_write_aligned_word(r4300, state->address & ~UINT32_C(0x3), state->wword >> shift, mask);
  UPDATE_COUNT_OUT
}

static void SWR_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  unsigned int n = (state->address & 3);
  unsigned int shift = 8 * (3 - n);
  uint32_t mask = BITS_ABOVE_MASK32(8 * (3 - n));
  r4300_write_aligned_word(r4300, state->address & ~UINT32_C(0x3), state->wword << shift, mask);
  UPDATE_COUNT_OUT
}

static void SDL_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  unsigned int n = (state->address & 7);
  unsigned int shift = 8 * n;
  uint64_t mask = (n == 0) ? ~UINT64_C(0) : BITS_BELOW_MASK64(8 * (8 - n));
  r4300_write_aligned_dword(r4300, state->address & ~UINT32_C(0x7), state->wdword >> shift, mask);
  UPDATE_COUNT_OUT
}

static void SDR_new(int pcaddr, int count)
{
  UPDATE_COUNT_IN
  state->pcaddr = pcaddr&~1;
  r4300->delay_slot = pcaddr & 1;
  unsigned int n = (state->address & 7);
  unsigned int shift = 8 * (7 - n);
  uint64_t mask = BITS_ABOVE_MASK64(8 * (7 - n));
  r4300_write_aligned_dword(r4300, state->address & ~UINT32_C(0x7), state->wdword << shift, mask);
  UPDATE_COUNT_OUT
}

#if NEW_DYNAREC == NEW_DYNAREC_X86
#include "x86/assem_x86.c"
#elif NEW_DYNAREC == NEW_DYNAREC_X64
#include "x64/assem_x64.c"
#elif NEW_DYNAREC == NEW_DYNAREC_ARM
#include "arm/assem_arm.c"
#elif  NEW_DYNAREC == NEW_DYNAREC_ARM64
#include "arm64/assem_arm64.c"
#else
#error Unsupported dynarec architecture
#endif

static void tlb_speed_hacks()
{
  // Goldeneye hack
  if (strncmp((char *) ROM_HEADER.Name, "GOLDENEYE",9) == 0)
  {
    u_int addr;
    int n;
    switch (ROM_HEADER.Country_code)
    {
      case 0x45: // U
        addr=0x34b30;
        break;
      case 0x4A: // J
        addr=0x34b70;
        break;
      case 0x50: // E
        addr=0x329f0;
        break;
      default:
        // Unknown country code
        addr=0;
        break;
    }
    uintptr_t rom_addr=(uintptr_t)g_dev.cart.cart_rom.rom;
    #ifdef ROM_COPY
    // Since memory_map is 32-bit, on 64-bit systems the rom needs to be
    // in the lower 4G of memory to use this hack.  Copy it if necessary.
    if((void *)g_dev.cart.cart_rom.rom>(void *)0xffffffff) {
      munmap(ROM_COPY, 67108864);
      if(mmap(ROM_COPY, 12582912,
              PROT_READ | PROT_WRITE,
              MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
              -1, 0) <= 0) {DebugMessage(M64MSG_ERROR, "mmap() failed");}
      memcpy(ROM_COPY,g_dev.cart.cart_rom.rom,12582912);
      rom_addr=(uintptr_t)ROM_COPY;
    }
    #endif
    if(addr) {
      for(n=0x7F000;n<0x80000;n++) {
        g_dev.r4300.new_dynarec_hot_state.memory_map[n]=(((uintptr_t)(rom_addr+addr-0x7F000000))>>2)|WRITE_PROTECT;
      }
    }
  }
}

/**** Linker ****/
u_int verify_dirty(struct ll_entry * head)
{
  void *source;
  if((int)head->start>=0xa0000000&&(int)head->start<0xa07fffff) {
    source=(void *)((uintptr_t)g_dev.rdram.dram+head->start-0xa0000000);
  }else if((int)head->start>=0xa4000000&&(int)head->start<0xa4001000) {
    source=(void *)((uintptr_t)g_dev.sp.mem+head->start-0xa4000000);
  }else if((int)head->start>=0x80000000&&(int)head->start<0x80800000) {
    source=(void *)((uintptr_t)g_dev.rdram.dram+head->start-(uintptr_t)0x80000000);
  }
  else if((signed int)head->start>=(signed int)0xC0000000) {
    unsigned int page=head->start>>12;
    uintptr_t map_value=g_dev.r4300.new_dynarec_hot_state.memory_map[page];

    if((intptr_t)map_value<(intptr_t)0)
      return head->vaddr;

    while(page<((head->start+head->length-1)>>12)) {
      if((g_dev.r4300.new_dynarec_hot_state.memory_map[++page]<<2)!=(map_value<<2))
        return head->vaddr;
    }
    source=(void*)(head->start+(map_value<<2));
  }
  else
    assert(0);

  if(memcmp(source,head->copy,head->length))
    return head->vaddr;
  else
    return 0;
}

// Add virtual address mapping for 32-bit compiled block
static struct ll_entry *ll_add_32(struct ll_entry **head,int vaddr,u_int reg32,void *addr,void *clean_addr,u_int start,void *copy,u_int length)
{
  struct ll_entry *new_entry;
  new_entry=(struct ll_entry *)malloc(sizeof(struct ll_entry));
  assert(new_entry!=NULL);
  new_entry->vaddr=vaddr;
  new_entry->reg32=reg32;
  new_entry->addr=addr;
  new_entry->clean_addr=clean_addr;
  new_entry->start=start;
  new_entry->copy=copy;
  new_entry->length=length;
  new_entry->next=*head;
  *head=new_entry;
  return new_entry;
}

// Add virtual address mapping to linked list
static struct ll_entry *ll_add(struct ll_entry **head,int vaddr,void *addr,void *clean_addr,u_int start,void *copy,u_int length)
{
  return ll_add_32(head,vaddr,0,addr,clean_addr,start,copy,length);
}

static void ll_remove_matching_addrs(struct ll_entry **head,intptr_t addr,int shift)
{
  struct ll_entry **cur=head;
  struct ll_entry *next;
  while(*cur) {
    if((((uintptr_t)((*cur)->addr)-(uintptr_t)base_addr)>>shift)==((addr-(uintptr_t)base_addr)>>shift) ||
       (((uintptr_t)((*cur)->addr)-(uintptr_t)base_addr-MAX_OUTPUT_BLOCK_SIZE)>>shift)==((addr-(uintptr_t)base_addr)>>shift))
    {
      if((*cur)->addr!=(*cur)->clean_addr){ //jump_dirty
        assert(head>=jump_dirty&&head<(jump_dirty+4096));
        u_int length=(*cur)->length;
        u_int* ptr=(u_int*)(*cur)->copy;
        ptr[length>>2]--;
        if(ptr[length>>2]==0){
          free(ptr);
          copy_size-=length+4;
        }
      }
      inv_debug("EXP: Remove pointer to %x (%x)\n",(intptr_t)(*cur)->addr,(*cur)->vaddr);
      remove_hash((*cur)->vaddr);
      next=(*cur)->next;
      free(*cur);
      *cur=next;
    }
    else
    {
      cur=&((*cur)->next);
    }
  }
}

// Remove all entries from linked list
static void ll_clear(struct ll_entry **head)
{
  struct ll_entry *cur;
  struct ll_entry *next;
  if((cur=*head)) {
    *head=0;
    while(cur) {
      if(cur->addr!=cur->clean_addr){ //jump_dirty
        assert(head>=jump_dirty&&head<(jump_dirty+4096));
        u_int length=cur->length;
        u_int* ptr=(u_int*)cur->copy;
        ptr[length>>2]--;
        if(ptr[length>>2]==0){
          free(ptr);
          copy_size-=length+4;
        }
      }
      next=cur->next;
      free(cur);
      cur=next;
    }
  }
}

// Dereference the pointers and remove if it matches
static void ll_kill_pointers(struct ll_entry *head,intptr_t addr,int shift)
{
  while(head) {
    uintptr_t ptr=get_pointer(head->addr);
    inv_debug("EXP: Lookup pointer to %x at %x (%x)\n",(intptr_t)ptr,(intptr_t)head->addr,head->vaddr);
    if((((ptr-(uintptr_t)base_addr)>>shift)==((addr-(uintptr_t)base_addr)>>shift)) ||
       (((ptr-(uintptr_t)base_addr-MAX_OUTPUT_BLOCK_SIZE)>>shift)==((addr-(uintptr_t)base_addr)>>shift)))
    {
      inv_debug("EXP: Kill pointer at %x (%x)\n",(intptr_t)head->addr,head->vaddr);
      uintptr_t host_addr=(intptr_t)kill_pointer(head->addr);
      #if NEW_DYNAREC >= NEW_DYNAREC_ARM
        needs_clear_cache[(host_addr-(uintptr_t)base_addr)>>17]|=1<<(((host_addr-(uintptr_t)base_addr)>>12)&31);
      #else
        /* avoid unused variable warning */
        (void)host_addr;
      #endif
    }
    head=head->next;
  }
}

// Add an entry to jump_out after making a link
static void add_link(u_int vaddr,void *src)
{
  u_int page=(vaddr^0x80000000)>>12;
  if(page>262143&&g_dev.r4300.cp0.tlb.LUT_r[vaddr>>12]) page=(g_dev.r4300.cp0.tlb.LUT_r[vaddr>>12]^0x80000000)>>12;
  if(page>4095) page=2048+(page&2047);
  inv_debug("add_link: %x -> %x (%d)\n",(intptr_t)src,vaddr,page);
  (void)ll_add(jump_out+page,vaddr,src,src,0,NULL,0);
  //int ptr=get_pointer(src);
  //inv_debug("add_link: Pointer is to %x\n",(intptr_t)ptr);
}

static struct ll_entry *get_clean(struct r4300_core* r4300,u_int vaddr,u_int flags)
{
  u_int page=(vaddr^0x80000000)>>12;
  if(page>262143&&r4300->cp0.tlb.LUT_r[vaddr>>12]) page=(r4300->cp0.tlb.LUT_r[vaddr>>12]^0x80000000)>>12;
  if(page>2048) page=2048+(page&2047);
  struct ll_entry *head;
  head=jump_in[page];
  while(head!=NULL) {
    if(head->vaddr==vaddr&&(head->reg32&flags)==0) {
      return head;
    }
    head=head->next;
  }
  return NULL;
}

static struct ll_entry *get_dirty(struct r4300_core* r4300,u_int vaddr,u_int flags)
{
  u_int page=(vaddr^0x80000000)>>12;
  u_int vpage=page;
  if(page>262143&&r4300->cp0.tlb.LUT_r[vaddr>>12]) page=(r4300->cp0.tlb.LUT_r[vaddr>>12]^0x80000000)>>12;
  if(page>2048) page=2048+(page&2047);
  if(vpage>262143&&r4300->cp0.tlb.LUT_r[vaddr>>12]) vpage&=2047; // jump_dirty uses a hash of the virtual address instead
  if(vpage>2048) vpage=2048+(vpage&2047);
  struct ll_entry *head;
  head=jump_dirty[vpage];
  while(head!=NULL) {
    if(head->vaddr==vaddr&&(head->reg32&flags)==0) {
      // Don't restore blocks which are about to expire from the cache
      if((((uintptr_t)head->addr-(uintptr_t)out)<<(32-TARGET_SIZE_2))>0x60000000+(MAX_OUTPUT_BLOCK_SIZE<<(32-TARGET_SIZE_2))) {
        if(verify_dirty(head)==0) {
          r4300->cached_interp.invalid_code[vaddr>>12]=0;
          r4300->new_dynarec_hot_state.memory_map[vaddr>>12]|=WRITE_PROTECT;
          if(vpage<2048) {
            if(r4300->cp0.tlb.LUT_r[vaddr>>12]) {
              r4300->cached_interp.invalid_code[r4300->cp0.tlb.LUT_r[vaddr>>12]>>12]=0;
              r4300->new_dynarec_hot_state.memory_map[r4300->cp0.tlb.LUT_r[vaddr>>12]>>12]|=WRITE_PROTECT;
            }
            restore_candidate[vpage>>3]|=1<<(vpage&7);
          }
          else restore_candidate[page>>3]|=1<<(page&7);
          return head;
        }
      }
    }
    head=head->next;
  }
  return NULL;
}

void *dynamic_linker(void * src, u_int vaddr)
{
  assert((vaddr&1)==0);
  struct r4300_core* r4300 = &g_dev.r4300;
  struct ll_entry *head;

#ifndef DISABLE_BLOCK_LINKING
  head=get_clean(r4300,vaddr,~0);
  if(head!=NULL){
    void* src_rw=(void*)(((intptr_t)src-(intptr_t)base_addr_rx)+(intptr_t)base_addr);
#if NEW_DYNAREC == NEW_DYNAREC_ARM64
    //TODO: Avoid disabling link between blocks for conditional branches
    int *ptr=(int*)src_rw;
    if((*ptr&0xfc000000)==0x14000000) { //b
      add_link(vaddr, add_pointer(src_rw,head->addr));
    }
#else
    add_link(vaddr, add_pointer(src_rw,head->addr));
#endif
    return (void*)(((intptr_t)head->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  }
#endif

  struct ll_entry **ht_bin=hash_table[((vaddr>>16)^vaddr)&0xFFFF];
  if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) return (void *)(((intptr_t)ht_bin[0]->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  if(ht_bin[1]&&ht_bin[1]->vaddr==vaddr) return (void *)(((intptr_t)ht_bin[1]->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);

#ifdef DISABLE_BLOCK_LINKING
  head=get_clean(r4300,vaddr,~0);
  if(head!=NULL){
    ht_bin[1]=ht_bin[0];
    ht_bin[0]=head;
    return (void*)(((intptr_t)head->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  }
#endif

  head=get_dirty(r4300,vaddr,~0);
  if(head!=NULL){
    if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) {
      ht_bin[0]=head; // Replace existing entry
    }
    else
    {
      ht_bin[1]=ht_bin[0];
      ht_bin[0]=head;
    }
    return (void*)(((intptr_t)head->clean_addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  }

  int r=new_recompile_block(vaddr);
  if(r==0) return dynamic_linker(src,vaddr);
  // Execute in unmapped page, generate pagefault execption
  assert(r4300->cp0.tlb.LUT_r[(vaddr&~1) >> 12] == 0);
  assert((intptr_t)r4300->new_dynarec_hot_state.memory_map[(vaddr&~1) >> 12] < 0);
  r4300->delay_slot = vaddr&1;
  TLB_refill_exception(r4300, vaddr&~1, 2);
  return get_addr_ht(r4300->new_dynarec_hot_state.pcaddr);
}

void *dynamic_linker_ds(void * src, u_int vaddr)
{
  struct r4300_core* r4300 = &g_dev.r4300;
  struct ll_entry *head;

#ifndef DISABLE_BLOCK_LINKING
  head=get_clean(r4300,vaddr,~0);
  if(head!=NULL){
    void* src_rw=(void*)(((intptr_t)src-(intptr_t)base_addr_rx)+(intptr_t)base_addr);
#if NEW_DYNAREC == NEW_DYNAREC_ARM64
    //TODO: Avoid disabling link between blocks for conditional branches
    int *ptr=(int*)src_rw;
    if((*ptr&0xfc000000)==0x14000000) { //b
      add_link(vaddr, add_pointer(src_rw,head->addr));
    }
#else
    add_link(vaddr, add_pointer(src_rw,head->addr));
#endif
    return (void*)(((intptr_t)head->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  }
#endif

  struct ll_entry **ht_bin=hash_table[((vaddr>>16)^vaddr)&0xFFFF];
  if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) return (void *)(((intptr_t)ht_bin[0]->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  if(ht_bin[1]&&ht_bin[1]->vaddr==vaddr) return (void *)(((intptr_t)ht_bin[1]->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);

#ifdef DISABLE_BLOCK_LINKING
  head=get_clean(r4300,vaddr,~0);
  if(head!=NULL){
    ht_bin[1]=ht_bin[0];
    ht_bin[0]=head;
    return (void*)(((intptr_t)head->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  }
#endif

  head=get_dirty(r4300,vaddr,~0);
  if(head!=NULL){
    if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) {
      ht_bin[0]=head; // Replace existing entry
    }
    else
    {
      ht_bin[1]=ht_bin[0];
      ht_bin[0]=head;
    }
    return (void*)(((intptr_t)head->clean_addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  }

  int r=new_recompile_block((vaddr&0xFFFFFFF8)+1);
  if(r==0) return dynamic_linker_ds(src,vaddr);
  // Execute in unmapped page, generate pagefault execption
  assert(r4300->cp0.tlb.LUT_r[(vaddr&~1) >> 12] == 0);
  assert((intptr_t)r4300->new_dynarec_hot_state.memory_map[(vaddr&~1) >> 12] < 0);
  r4300->delay_slot = vaddr&1;
  TLB_refill_exception(r4300, vaddr&~1, 2);
  return get_addr_ht(r4300->new_dynarec_hot_state.pcaddr);
}

// Get address from virtual address
// This is called from the recompiled JR/JALR instructions
void *get_addr(u_int vaddr)
{
  struct r4300_core* r4300 = &g_dev.r4300;
  struct ll_entry *head;
  struct ll_entry **ht_bin=hash_table[((vaddr>>16)^vaddr)&0xFFFF];

  head=get_clean(r4300,vaddr,~0);
  if(head!=NULL){
    ht_bin[1]=ht_bin[0];
    ht_bin[0]=head;
    return (void*)(((intptr_t)head->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  }

  head=get_dirty(r4300,vaddr,~0);
  if(head!=NULL){
    if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) {
      ht_bin[0]=head; // Replace existing entry
    }
    else
    {
      ht_bin[1]=ht_bin[0];
      ht_bin[0]=head;
    }
    return (void*)(((intptr_t)head->clean_addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  }

  int r=new_recompile_block(vaddr);
  if(r==0) return get_addr(vaddr);
  // Execute in unmapped page, generate pagefault execption
  assert(r4300->cp0.tlb.LUT_r[(vaddr&~1) >> 12] == 0);
  assert((intptr_t)r4300->new_dynarec_hot_state.memory_map[(vaddr&~1) >> 12] < 0);
  r4300->delay_slot = vaddr&1;
  TLB_refill_exception(r4300, vaddr&~1, 2);
  return get_addr_ht(r4300->new_dynarec_hot_state.pcaddr);
}

// Look up address in hash table first
void *get_addr_ht(u_int vaddr)
{
  struct ll_entry **ht_bin=hash_table[((vaddr>>16)^vaddr)&0xFFFF];
  if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) return (void *)(((intptr_t)ht_bin[0]->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  if(ht_bin[1]&&ht_bin[1]->vaddr==vaddr) return (void *)(((intptr_t)ht_bin[1]->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  return get_addr(vaddr);
}

void *get_addr_32(u_int vaddr,u_int flags)
{
  struct ll_entry **ht_bin=hash_table[((vaddr>>16)^vaddr)&0xFFFF];
  if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) return (void *)(((intptr_t)ht_bin[0]->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  if(ht_bin[1]&&ht_bin[1]->vaddr==vaddr) return (void *)(((intptr_t)ht_bin[1]->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);

  struct r4300_core* r4300 = &g_dev.r4300;
  struct ll_entry *head;
  head=get_clean(r4300,vaddr,flags);
  if(head!=NULL){
    if(head->reg32==0) {
      if(ht_bin[0]==NULL) {
        ht_bin[0]=head;
      }else if(ht_bin[1]==NULL) {
        ht_bin[1]=head;
      }
    }
    return (void*)(((intptr_t)head->addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  }

  head=get_dirty(r4300,vaddr,flags);
  if(head!=NULL){
     if(head->reg32==0) {
      if(ht_bin[0]==NULL) {
        ht_bin[0]=head;
      }else if(ht_bin[1]==NULL) {
        ht_bin[1]=head;
      }
     }
    return (void*)(((intptr_t)head->clean_addr-(intptr_t)base_addr)+(intptr_t)base_addr_rx);
  }

  int r=new_recompile_block(vaddr);
  if(r==0) return get_addr(vaddr);
  // Execute in unmapped page, generate pagefault execption
  assert(r4300->cp0.tlb.LUT_r[(vaddr&~1) >> 12] == 0);
  assert((intptr_t)r4300->new_dynarec_hot_state.memory_map[(vaddr&~1) >> 12] < 0);
  r4300->delay_slot = vaddr&1;
  TLB_refill_exception(r4300, vaddr&~1, 2);
  return get_addr_ht(r4300->new_dynarec_hot_state.pcaddr);
}

// Check if an address is already compiled
// but don't return addresses which are about to expire from the cache
static void *check_addr(u_int vaddr)
{
  struct ll_entry **ht_bin=hash_table[((vaddr>>16)^vaddr)&0xFFFF];

  if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) {
    if((((uintptr_t)ht_bin[0]->addr-MAX_OUTPUT_BLOCK_SIZE-(uintptr_t)out)<<(32-TARGET_SIZE_2))>0x60000000+(MAX_OUTPUT_BLOCK_SIZE<<(32-TARGET_SIZE_2)))
      if(ht_bin[0]->addr==ht_bin[0]->clean_addr) return ht_bin[0]->addr; //jump_in
  }
  if(ht_bin[1]&&ht_bin[1]->vaddr==vaddr) {
    if((((uintptr_t)ht_bin[1]->addr-MAX_OUTPUT_BLOCK_SIZE-(uintptr_t)out)<<(32-TARGET_SIZE_2))>0x60000000+(MAX_OUTPUT_BLOCK_SIZE<<(32-TARGET_SIZE_2)))
      if(ht_bin[1]->addr==ht_bin[1]->clean_addr) return ht_bin[1]->addr; //jump_in
  }

  struct r4300_core* r4300 = &g_dev.r4300;
  struct ll_entry *head;
  head=get_clean(r4300,vaddr,~0);
  if(head!=NULL){
    if((((uintptr_t)head->addr-(uintptr_t)out)<<(32-TARGET_SIZE_2))>0x60000000+(MAX_OUTPUT_BLOCK_SIZE<<(32-TARGET_SIZE_2))) {
      // Update existing entry with current address
      if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) {
        ht_bin[0]=head;
        return head->addr;
      }
      if(ht_bin[1]&&ht_bin[1]->vaddr==vaddr) {
        ht_bin[1]=head;
        return head->addr;
      }
      // Insert into hash table with low priority.
      // Don't evict existing entries, as they are probably
      // addresses that are being accessed frequently.
      if(ht_bin[0]==NULL) {
        ht_bin[0]=head;
      }else if(ht_bin[1]==NULL) {
        ht_bin[1]=head;
      }
      return head->addr;
    }
  }
  return NULL;
}

// This is called when we write to a compiled block (see do_invstub)
static void invalidate_page(u_int page)
{
  struct ll_entry *head;
  struct ll_entry *next;
  head=jump_in[page];
  jump_in[page]=0;
  while(head!=NULL) {
    inv_debug("INVALIDATE: %x\n",head->vaddr);
    remove_hash(head->vaddr);
    next=head->next;
    free(head);
    head=next;
  }
  head=jump_out[page];
  jump_out[page]=0;
  while(head!=NULL) {
    inv_debug("INVALIDATE: kill pointer to %x (%x)\n",head->vaddr,(intptr_t)head->addr);
      uintptr_t host_addr=(intptr_t)kill_pointer(head->addr);
    #if NEW_DYNAREC >= NEW_DYNAREC_ARM
      needs_clear_cache[(host_addr-(uintptr_t)base_addr)>>17]|=1<<(((host_addr-(uintptr_t)base_addr)>>12)&31);
    #else
      /* avoid unused variable warning */
      (void)host_addr;
    #endif
    next=head->next;
    free(head);
    head=next;
  }
}

void invalidate_block(u_int block)
{
  u_int page;
  page=block^0x80000;
  if(block<0x100000&&page>262143&&g_dev.r4300.cp0.tlb.LUT_r[block]) page=(g_dev.r4300.cp0.tlb.LUT_r[block]^0x80000000)>>12;
  if(page>2048) page=2048+(page&2047);
  inv_debug("INVALIDATE: %x (%d)\n",block<<12,page);
  u_int first,last;
  first=last=page;
  struct ll_entry *head;
  head=jump_in[page];
  u_int start,end;

  while(head!=NULL) {
    if((signed int)head->vaddr>=0x80000000&&(signed int)head->vaddr<0x80800000) {
      assert(page<2048);
      start=(head->start^0x80000000)>>12;
      end=((head->start+head->length-1)^0x80000000)>>12;
      assert(start<2048&&end<2048);
    }
    if((signed int)head->vaddr>=(signed int)0xC0000000) {
      assert(page<2048);
      assert(g_dev.r4300.new_dynarec_hot_state.memory_map[head->vaddr>>12]!=(uintptr_t)-1);
      u_int paddr=head->vaddr+(g_dev.r4300.new_dynarec_hot_state.memory_map[head->vaddr>>12]<<2)-(uintptr_t)g_dev.rdram.dram;
      start=(paddr-(head->vaddr-head->start))>>12;
      end=(paddr+((head->start+head->length)-head->vaddr)-1)>>12;
      assert(start<2048&&end<2048);
    }
    else if((signed int)head->vaddr>=(signed int)0x80800000) {
      assert(page>=2048);
      start=(head->start^0x80000000)>>12;
      end=((head->start+head->length-1)^0x80000000)>>12;
      assert(start>=2048&&end>=2048);
      start=2048+(start&2047);
      end=2048+(end&2047);
    }

    if((start<=page)&&(end>=page)) {
      if(start<first) first=start;
      if(end>last) last=end;
    }
    head=head->next;
  }

  invalidate_page(page);
  assert(first+5>page); // NB: this assumes MAXBLOCK<=4096 (4 pages)
  assert(last<page+5);
  // Invalidate the adjacent pages if a block crosses a 4K boundary
  while(first<page) {
    invalidate_page(first);
    first++;
  }
  for(first=page+1;first<last;first++) {
    invalidate_page(first);
  }
  #if NEW_DYNAREC >= NEW_DYNAREC_ARM
    do_clear_cache();
  #endif

  // Don't trap writes
  if(block<0x100000) {
    g_dev.r4300.cached_interp.invalid_code[block]=1;
  }
  // If there is a valid TLB entry for this page, remove write protect
  if(block<0x100000&&g_dev.r4300.cp0.tlb.LUT_w[block]) {
    assert(g_dev.r4300.cp0.tlb.LUT_r[block]==g_dev.r4300.cp0.tlb.LUT_w[block]);
    g_dev.r4300.new_dynarec_hot_state.memory_map[block]=((uintptr_t)g_dev.rdram.dram+(uintptr_t)((g_dev.r4300.cp0.tlb.LUT_w[block]&0xFFFFF000)-0x80000000)-(block<<12))>>2;
    u_int real_block=g_dev.r4300.cp0.tlb.LUT_w[block]>>12;
    g_dev.r4300.cached_interp.invalid_code[real_block]=1;
    if(real_block>=0x80000&&real_block<0x80800) g_dev.r4300.new_dynarec_hot_state.memory_map[real_block]=((uintptr_t)g_dev.rdram.dram-(uintptr_t)0x80000000)>>2;
  }
  else if(block>=0x80000&&block<0x80800) g_dev.r4300.new_dynarec_hot_state.memory_map[block]=((uintptr_t)g_dev.rdram.dram-(uintptr_t)0x80000000)>>2;
  #ifdef USE_MINI_HT
  memset(g_dev.r4300.new_dynarec_hot_state.mini_ht,-1,sizeof(g_dev.r4300.new_dynarec_hot_state.mini_ht));
  #endif
}

// This is called when loading a save state.
// Anything could have changed, so invalidate everything.
static void invalidate_all_pages(void)
{
  u_int page;
  for(page=0;page<4096;page++)
    invalidate_page(page);
  for(page=0;page<1048576;page++)
  {
    if(!g_dev.r4300.cached_interp.invalid_code[page]) {
      restore_candidate[(page&2047)>>3]|=1<<(page&7);
      restore_candidate[((page&2047)>>3)+256]|=1<<(page&7);
    }
  }
  #if NEW_DYNAREC >= NEW_DYNAREC_ARM
  cache_flush((char *)base_addr_rx,(char *)base_addr_rx+(1<<TARGET_SIZE_2));
  #endif
  #ifdef USE_MINI_HT
  memset(g_dev.r4300.new_dynarec_hot_state.mini_ht,-1,sizeof(g_dev.r4300.new_dynarec_hot_state.mini_ht));
  #endif
  // TLB
  for(page=0;page<0x100000;page++) {
    if(g_dev.r4300.cp0.tlb.LUT_r[page]) {
      g_dev.r4300.new_dynarec_hot_state.memory_map[page]=((uintptr_t)g_dev.rdram.dram+(uintptr_t)((g_dev.r4300.cp0.tlb.LUT_r[page]&0xFFFFF000)-0x80000000)-(page<<12))>>2;
      if(!g_dev.r4300.cp0.tlb.LUT_w[page]||!g_dev.r4300.cached_interp.invalid_code[page])
        g_dev.r4300.new_dynarec_hot_state.memory_map[page]|=WRITE_PROTECT; // Write protect
    }
    else g_dev.r4300.new_dynarec_hot_state.memory_map[page]=(uintptr_t)-1;
    if(page==0x80000) page=0xC0000;
  }
  tlb_speed_hacks();
}

void invalidate_cached_code_new_dynarec(struct r4300_core* r4300, uint32_t address, size_t size)
{
    size_t i;
    size_t begin;
    size_t end;

    if (size == 0)
    {
        invalidate_all_pages();
    }
    else
    {
        begin = address >> 12;
        end = (address+size-1) >> 12;

        for(i = begin; i <= end; ++i) {
            if(r4300->cached_interp.invalid_code[i] == 0) {
                invalidate_block(i);
            }
        }
    }
}

// If a code block was found to be unmodified (bit was set in
// restore_candidate) and it remains unmodified (bit is clear
// in invalid_code) then move the entries for that 4K page from
// the dirty list to the clean list.
void clean_blocks(u_int page)
{
  struct ll_entry *head;
  inv_debug("INV: clean_blocks page=%d\n",page);
  head=jump_dirty[page];
  while(head!=NULL) {
    if(!g_dev.r4300.cached_interp.invalid_code[head->vaddr>>12]) {
      // Don't restore blocks which are about to expire from the cache
      if((((uintptr_t)head->addr-(uintptr_t)out)<<(32-TARGET_SIZE_2))>0x60000000+(MAX_OUTPUT_BLOCK_SIZE<<(32-TARGET_SIZE_2))) {
        if(verify_dirty(head)==0) {
          //DebugMessage(M64MSG_VERBOSE, "Possibly Restore %x (%x)",head->vaddr, (intptr_t)head->addr);
          u_int i,j;
          u_int inv=0;
          u_int start,end;
          if((signed int)head->vaddr>=0x80000000&&(signed int)head->vaddr<0x80800000) {
            start=head->start>>12;
            end=(head->start+head->length-1)>>12;
            for(i=start;i<=end;i++) {
              inv|=g_dev.r4300.cached_interp.invalid_code[i];
            }
          }
          else if((signed int)head->vaddr>=(signed int)0xC0000000) {
            uintptr_t map_value=g_dev.r4300.new_dynarec_hot_state.memory_map[head->vaddr>>12];
            start=head->start>>12;
            end=(head->start+head->length-1)>>12;
            for(i=start;i<=end;i++) {
              inv|=g_dev.r4300.cached_interp.invalid_code[i];
              assert((g_dev.r4300.new_dynarec_hot_state.memory_map[i]<<2)==(map_value<<2)); //mapping was already checked in verify_dirty
              j=(((uintptr_t)i<<12)+(uintptr_t)(g_dev.r4300.new_dynarec_hot_state.memory_map[i]<<2)-(uintptr_t)g_dev.rdram.dram+(uintptr_t)0x80000000)>>12;
              inv|=g_dev.r4300.cached_interp.invalid_code[j];
            }
          }
          else if((signed int)head->vaddr>=(signed int)0x80800000) {
            //TODO: allow restoring?
            inv=1;
          }
          if(!inv) {
            if((((uintptr_t)head->clean_addr-(uintptr_t)out)<<(32-TARGET_SIZE_2))>0x60000000+(MAX_OUTPUT_BLOCK_SIZE<<(32-TARGET_SIZE_2))) {
              u_int ppage=page;
              if(page<2048&&g_dev.r4300.cp0.tlb.LUT_r[head->vaddr>>12]) ppage=(g_dev.r4300.cp0.tlb.LUT_r[head->vaddr>>12]^0x80000000)>>12;
              inv_debug("INV: Restored %x (%x/%x)\n",head->vaddr, (intptr_t)head->addr, (intptr_t)head->clean_addr);
              //DebugMessage(M64MSG_VERBOSE, "page=%x, addr=%x",page,head->vaddr);
              //assert(head->vaddr>>12==(page|0x80000));
              struct ll_entry *clean_head=ll_add_32(jump_in+ppage,head->vaddr,head->reg32,head->clean_addr,head->clean_addr,head->start,head->copy,head->length);
              struct ll_entry **ht_bin=hash_table[((head->vaddr>>16)^head->vaddr)&0xFFFF];
              if(!head->reg32) {
                if(ht_bin[0]&&ht_bin[0]->vaddr==head->vaddr) {
                  ht_bin[0]=clean_head; // Replace existing entry
                }
                if(ht_bin[1]&&ht_bin[1]->vaddr==head->vaddr) {
                  ht_bin[1]=clean_head; // Replace existing entry
                }
              }
            }
          }
        }
      }
    }
    head=head->next;
  }
}

void* cop1_unusable(void)
{
    struct r4300_core* r4300 = &g_dev.r4300;
    struct new_dynarec_hot_state* state = &r4300->new_dynarec_hot_state;
    r4300->delay_slot = state->pcaddr & 1;
    state->pcaddr &= ~1;
    state->cp0_regs[CP0_CAUSE_REG] = CP0_CAUSE_EXCCODE_CPU | CP0_CAUSE_CE1;
    exception_general(r4300);
    return get_addr_ht(state->pcaddr);
}

void dynarec_gen_interrupt(void)
{
    struct r4300_core* r4300 = &g_dev.r4300;
    struct new_dynarec_hot_state* state = &r4300->new_dynarec_hot_state;
    cp0_update_count(r4300);
    uint32_t page = ((state->cp0_regs[CP0_COUNT_REG]>>19)&0x1fc);
    unsigned int *candidate = (unsigned int *)&restore_candidate[page];
    page <<= 3;
    r4300->delay_slot = 0;

    if(*candidate)
    {
        for(int i=0;i<32;i++)
        {
            if((*candidate>>i)&1)
                clean_blocks(page+i);
        }
        *candidate = 0;
    }

    gen_interrupt(r4300);
}

/**** Register allocation ****/
static void mov_alloc(struct regstat *current,int i)
{
  // Note: Don't need to actually alloc the source registers
  if((~current->is32>>rs1[i])&1) {
    //alloc_reg64(current,i,rs1[i]);
    alloc_reg64(current,i,rt1[i]);
    current->is32&=~(1LL<<rt1[i]);
  } else {
    //alloc_reg(current,i,rs1[i]);
    alloc_reg(current,i,rt1[i]);
    current->is32|=(1LL<<rt1[i]);
  }
  clear_const(current,rs1[i]);
  clear_const(current,rt1[i]);
  dirty_reg(current,rt1[i]);
}

static void shiftimm_alloc(struct regstat *current,int i)
{
  clear_const(current,rs1[i]);
  clear_const(current,rt1[i]);
  if(opcode2[i]<=0x3) // SLL/SRL/SRA
  {
    if(rt1[i]) {
      if(rs1[i]&&needed_again(rs1[i],i)) alloc_reg(current,i,rs1[i]);
      else lt1[i]=rs1[i];
      alloc_reg(current,i,rt1[i]);
      current->is32|=1LL<<rt1[i];
      dirty_reg(current,rt1[i]);
    }
  }
  if(opcode2[i]>=0x38&&opcode2[i]<=0x3b) // DSLL/DSRL/DSRA
  {
    if(rt1[i]) {
      if(rs1[i]) alloc_reg64(current,i,rs1[i]);
      alloc_reg64(current,i,rt1[i]);
      current->is32&=~(1LL<<rt1[i]);
      dirty_reg(current,rt1[i]);
    }
  }
  if(opcode2[i]==0x3c) // DSLL32
  {
    if(rt1[i]) {
      if(rs1[i]) alloc_reg(current,i,rs1[i]);
      alloc_reg64(current,i,rt1[i]);
      current->is32&=~(1LL<<rt1[i]);
      dirty_reg(current,rt1[i]);
    }
  }
  if(opcode2[i]==0x3e) // DSRL32
  {
    if(rt1[i]) {
      alloc_reg64(current,i,rs1[i]);
      if(imm[i]==32) {
        alloc_reg64(current,i,rt1[i]);
        current->is32&=~(1LL<<rt1[i]);
      } else {
        alloc_reg(current,i,rt1[i]);
        current->is32|=1LL<<rt1[i];
      }
      dirty_reg(current,rt1[i]);
    }
  }
  if(opcode2[i]==0x3f) // DSRA32
  {
    if(rt1[i]) {
      alloc_reg64(current,i,rs1[i]);
      alloc_reg(current,i,rt1[i]);
      current->is32|=1LL<<rt1[i];
      dirty_reg(current,rt1[i]);
    }
  }
}

static void shift_alloc(struct regstat *current,int i)
{
  if(rt1[i]) {
    if(opcode2[i]<=0x07) // SLLV/SRLV/SRAV
    {
      if(rs1[i]) alloc_reg(current,i,rs1[i]);
      if(rs2[i]) alloc_reg(current,i,rs2[i]);
      alloc_reg(current,i,rt1[i]);
      if(rt1[i]==rs2[i]) {
        alloc_reg_temp(current,i,-1);
        minimum_free_regs[i]=1;
      }
      current->is32|=1LL<<rt1[i];
    } else { // DSLLV/DSRLV/DSRAV
      if(rs1[i]) alloc_reg64(current,i,rs1[i]);
      if(rs2[i]) alloc_reg(current,i,rs2[i]);
      alloc_reg64(current,i,rt1[i]);
      current->is32&=~(1LL<<rt1[i]);
#if NEW_DYNAREC!=NEW_DYNAREC_ARM64
      if(opcode2[i]==0x16||opcode2[i]==0x17) // DSRLV and DSRAV need a temporary register
      {
        alloc_reg_temp(current,i,-1);
        minimum_free_regs[i]=1;
      }
#endif
    }
    clear_const(current,rs1[i]);
    clear_const(current,rs2[i]);
    clear_const(current,rt1[i]);
    dirty_reg(current,rt1[i]);
  }
}

static void alu_alloc(struct regstat *current,int i)
{
  if(opcode2[i]>=0x20&&opcode2[i]<=0x23) { // ADD/ADDU/SUB/SUBU
    if(rt1[i]) {
      if(rs1[i]&&rs2[i]) {
        alloc_reg(current,i,rs1[i]);
        alloc_reg(current,i,rs2[i]);
      }
      else {
        if(rs1[i]&&needed_again(rs1[i],i)) alloc_reg(current,i,rs1[i]);
        if(rs2[i]&&needed_again(rs2[i],i)) alloc_reg(current,i,rs2[i]);
      }
      alloc_reg(current,i,rt1[i]);
    }
    current->is32|=1LL<<rt1[i];
  }
  if(opcode2[i]==0x2a||opcode2[i]==0x2b) { // SLT/SLTU
    if(rt1[i]) {
      if(!((current->is32>>rs1[i])&(current->is32>>rs2[i])&1))
      {
        alloc_reg64(current,i,rs1[i]);
        alloc_reg64(current,i,rs2[i]);
        alloc_reg(current,i,rt1[i]);
      } else {
        alloc_reg(current,i,rs1[i]);
        alloc_reg(current,i,rs2[i]);
        alloc_reg(current,i,rt1[i]);
      }
    }
    current->is32|=1LL<<rt1[i];
  }
  if(opcode2[i]>=0x24&&opcode2[i]<=0x27) { // AND/OR/XOR/NOR
    if(rt1[i]) {
      if(rs1[i]&&rs2[i]) {
        alloc_reg(current,i,rs1[i]);
        alloc_reg(current,i,rs2[i]);
      }
      else
      {
        if(rs1[i]&&needed_again(rs1[i],i)) alloc_reg(current,i,rs1[i]);
        if(rs2[i]&&needed_again(rs2[i],i)) alloc_reg(current,i,rs2[i]);
      }
      alloc_reg(current,i,rt1[i]);
      if(!((current->is32>>rs1[i])&(current->is32>>rs2[i])&1))
      {
        if(!((current->uu>>rt1[i])&1)) {
          alloc_reg64(current,i,rt1[i]);
        }
        if(get_reg(current->regmap,rt1[i]|64)>=0) {
          if(rs1[i]&&rs2[i]) {
            alloc_reg64(current,i,rs1[i]);
            alloc_reg64(current,i,rs2[i]);
          }
          else
          {
            // Is is really worth it to keep 64-bit values in registers?
            #ifdef NATIVE_64BIT
            if(rs1[i]&&needed_again(rs1[i],i)) alloc_reg64(current,i,rs1[i]);
            if(rs2[i]&&needed_again(rs2[i],i)) alloc_reg64(current,i,rs2[i]);
            #endif
          }
        }
        current->is32&=~(1LL<<rt1[i]);
      } else {
        current->is32|=1LL<<rt1[i];
      }
    }
  }
  if(opcode2[i]>=0x2c&&opcode2[i]<=0x2f) { // DADD/DADDU/DSUB/DSUBU
    if(rt1[i]) {
      if(rs1[i]&&rs2[i]) {
        if(!((current->uu>>rt1[i])&1)||get_reg(current->regmap,rt1[i]|64)>=0) {
          alloc_reg64(current,i,rs1[i]);
          alloc_reg64(current,i,rs2[i]);
          alloc_reg64(current,i,rt1[i]);
        } else {
          alloc_reg(current,i,rs1[i]);
          alloc_reg(current,i,rs2[i]);
          alloc_reg(current,i,rt1[i]);
        }
      }
      else {
        alloc_reg(current,i,rt1[i]);
        if(!((current->uu>>rt1[i])&1)||get_reg(current->regmap,rt1[i]|64)>=0) {
          // DADD used as move, or zeroing
          // If we have a 64-bit source, then make the target 64 bits too
          if(rs1[i]&&!((current->is32>>rs1[i])&1)) {
            if(get_reg(current->regmap,rs1[i])>=0) alloc_reg64(current,i,rs1[i]);
            alloc_reg64(current,i,rt1[i]);
          } else if(rs2[i]&&!((current->is32>>rs2[i])&1)) {
            if(get_reg(current->regmap,rs2[i])>=0) alloc_reg64(current,i,rs2[i]);
            alloc_reg64(current,i,rt1[i]);
          }
          if(opcode2[i]>=0x2e&&rs2[i]) {
            // DSUB used as negation - 64-bit result
            // If we have a 32-bit register, extend it to 64 bits
            if(get_reg(current->regmap,rs2[i])>=0) alloc_reg64(current,i,rs2[i]);
            alloc_reg64(current,i,rt1[i]);
          }
        }
      }
      if(rs1[i]&&rs2[i]) {
        current->is32&=~(1LL<<rt1[i]);
      } else if(rs1[i]) {
        current->is32&=~(1LL<<rt1[i]);
        if((current->is32>>rs1[i])&1)
          current->is32|=1LL<<rt1[i];
      } else if(rs2[i]) {
        current->is32&=~(1LL<<rt1[i]);
        if((current->is32>>rs2[i])&1)
          current->is32|=1LL<<rt1[i];
      } else {
        current->is32|=1LL<<rt1[i];
      }
    }
  }
  clear_const(current,rs1[i]);
  clear_const(current,rs2[i]);
  clear_const(current,rt1[i]);
  dirty_reg(current,rt1[i]);
}

static void imm16_alloc(struct regstat *current,int i)
{
  if(rs1[i]&&needed_again(rs1[i],i)) alloc_reg(current,i,rs1[i]);
  else lt1[i]=rs1[i];
  if(rt1[i]) alloc_reg(current,i,rt1[i]);
  if(opcode[i]==0x18||opcode[i]==0x19) { // DADDI/DADDIU
    current->is32&=~(1LL<<rt1[i]);
    if(!((current->uu>>rt1[i])&1)||get_reg(current->regmap,rt1[i]|64)>=0) {
      // TODO: Could preserve the 32-bit flag if the immediate is zero
      alloc_reg64(current,i,rt1[i]);
      alloc_reg64(current,i,rs1[i]);
    }
    clear_const(current,rs1[i]);
    clear_const(current,rt1[i]);
  }
  else if(opcode[i]==0x0a||opcode[i]==0x0b) { // SLTI/SLTIU
    if((~current->is32>>rs1[i])&1) alloc_reg64(current,i,rs1[i]);
    current->is32|=1LL<<rt1[i];
    clear_const(current,rs1[i]);
    clear_const(current,rt1[i]);
  }
  else if(opcode[i]>=0x0c&&opcode[i]<=0x0e) { // ANDI/ORI/XORI
    if(((~current->is32>>rs1[i])&1)&&opcode[i]>0x0c) {
      if(rs1[i]!=rt1[i]) {
        if(needed_again(rs1[i],i)) alloc_reg64(current,i,rs1[i]);
        alloc_reg64(current,i,rt1[i]);
        current->is32&=~(1LL<<rt1[i]);
      }
    }
    else current->is32|=1LL<<rt1[i]; // ANDI clears upper bits
    if(is_const(current,rs1[i])) {
      int v=get_const(current,rs1[i]);
      if(opcode[i]==0x0c) set_const(current,rt1[i],v&imm[i]);
      if(opcode[i]==0x0d) set_const(current,rt1[i],v|imm[i]);
      if(opcode[i]==0x0e) set_const(current,rt1[i],v^imm[i]);
    }
    else clear_const(current,rt1[i]);
  }
  else if(opcode[i]==0x08||opcode[i]==0x09) { // ADDI/ADDIU
    if(is_const(current,rs1[i])) {
      int v=get_const(current,rs1[i]);
      set_const(current,rt1[i],v+imm[i]);
    }
    else clear_const(current,rt1[i]);
    current->is32|=1LL<<rt1[i];
  }
  else {
    set_const(current,rt1[i],((long long)((short)imm[i]))<<16); // LUI
    current->is32|=1LL<<rt1[i];
  }
  dirty_reg(current,rt1[i]);
}

static void load_alloc(struct regstat *current,int i)
{
  clear_const(current,rt1[i]);
  //if(rs1[i]!=rt1[i]&&needed_again(rs1[i],i)) clear_const(current,rs1[i]); // Does this help or hurt?
  if(!rs1[i]) current->u&=~1LL; // Allow allocating r0 if it's the source register
  if(needed_again(rs1[i],i)) alloc_reg(current,i,rs1[i]);
  if(rt1[i]&&!((current->u>>rt1[i])&1)) {
    alloc_reg(current,i,rt1[i]);
    assert(get_reg(current->regmap,rt1[i])>=0);
    if(opcode[i]==0x27||opcode[i]==0x37||opcode[i]==0x1A||opcode[i]==0x1B) // LWU/LD/LDL/LDR
    {
      current->is32&=~(1LL<<rt1[i]);
      if(opcode[i]==0x27) current->uu&=~(1LL<<rt1[i]); // LWU: always alloc upper part in order to zero extend the destination register
      alloc_reg64(current,i,rt1[i]);
    }
    else current->is32|=1LL<<rt1[i];
    dirty_reg(current,rt1[i]);

    if(opcode[i]==0x22||opcode[i]==0x26) // LWL/LWR
#if NEW_DYNAREC == NEW_DYNAREC_X86
      alloc_reg64(current,i,FTEMP); // x86 needs another temp register
#else
      alloc_reg(current,i,FTEMP);
#endif
    else if(opcode[i]==0x1A||opcode[i]==0x1B) // LDL/LDR
      alloc_reg64(current,i,FTEMP);

    // If using TLB, need a register for pointer to the mapping table
    if(using_tlb) alloc_reg(current,i,TLREG);

    alloc_reg_temp(current,i,-1);
    minimum_free_regs[i]=1;
  }
  else
  {
    // Load to r0 or unneeded register (dummy load)
    // but we still need a register to calculate the address
    if(opcode[i]==0x22||opcode[i]==0x26)
      alloc_reg(current,i,FTEMP); // LWL/LWR need another temporary
    if(opcode[i]==0x1A||opcode[i]==0x1B) // LDL/LDR
      alloc_reg64(current,i,FTEMP);

    // If using TLB, need a register for pointer to the mapping table
    if(using_tlb) alloc_reg(current,i,TLREG);

    alloc_reg_temp(current,i,-1);
    minimum_free_regs[i]=1;
  }
}

static void store_alloc(struct regstat *current,int i)
{
  clear_const(current,rs2[i]);
  if(!(rs2[i])) current->u&=~1LL; // Allow allocating r0 if necessary
  if(needed_again(rs1[i],i)) alloc_reg(current,i,rs1[i]);
  alloc_reg(current,i,rs2[i]);
  if(opcode[i]==0x2c||opcode[i]==0x2d||opcode[i]==0x3f) { // 64-bit SDL/SDR/SD
    alloc_reg64(current,i,rs2[i]);
    if(rs2[i]) alloc_reg(current,i,FTEMP);
  }
  // If using TLB, need a register for pointer to the mapping table
  if(using_tlb) alloc_reg(current,i,TLREG);
  #if defined(HOST_IMM8) || defined(NEED_INVC_PTR)
  // On CPUs without 32-bit immediates we need a pointer to invalid_code
  else alloc_reg(current,i,INVCP);
  #endif
  if(opcode[i]==0x2c||opcode[i]==0x2d) { // 64-bit SDL/SDR
    alloc_reg(current,i,FTEMP);
  }
  // We need a temporary register for address generation
  alloc_reg_temp(current,i,-1);
  minimum_free_regs[i]=1;
}

static void c1ls_alloc(struct regstat *current,int i)
{
  //clear_const(current,rs1[i]); // FIXME
  clear_const(current,rt1[i]);
  if(needed_again(rs1[i],i)) alloc_reg(current,i,rs1[i]);
  alloc_reg(current,i,CSREG); // Status
  alloc_reg(current,i,FTEMP);
  if(opcode[i]==0x35||opcode[i]==0x3d) { // 64-bit LDC1/SDC1
    alloc_reg64(current,i,FTEMP);
  }
  // If using TLB, need a register for pointer to the mapping table
  if(using_tlb) alloc_reg(current,i,TLREG);
  #if defined(HOST_IMM8) || defined(NEED_INVC_PTR)
  // On CPUs without 32-bit immediates we need a pointer to invalid_code
  else if((opcode[i]&0x3b)==0x39) // SWC1/SDC1
    alloc_reg(current,i,INVCP);
  #endif
  // We need a temporary register for address generation
  alloc_reg_temp(current,i,-1);
  minimum_free_regs[i]=1;
}

#ifndef multdiv_alloc
void multdiv_alloc(struct regstat *current,int i)
{
  //  case 0x18: MULT
  //  case 0x19: MULTU
  //  case 0x1A: DIV
  //  case 0x1B: DIVU
  //  case 0x1C: DMULT
  //  case 0x1D: DMULTU
  //  case 0x1E: DDIV
  //  case 0x1F: DDIVU
  clear_const(current,rs1[i]);
  clear_const(current,rs2[i]);
  if(rs1[i]&&rs2[i])
  {
    if((opcode2[i]&4)==0) // 32-bit
    {
#ifndef INTERPRET_MULT
      if((opcode2[i]==0x18) || (opcode2[i]==0x19)) { //MULT/MULTU
        current->u&=~(1LL<<HIREG);
        current->u&=~(1LL<<LOREG);
      }
#endif
#ifndef INTERPRET_DIV
      if((opcode2[i]==0x1A) || (opcode2[i]==0x1B)) { //DIV/DIVU
        current->u&=~(1LL<<HIREG);
        current->u&=~(1LL<<LOREG);
      }
#endif
      alloc_reg(current,i,HIREG);
      alloc_reg(current,i,LOREG);
      alloc_reg(current,i,rs1[i]);
      alloc_reg(current,i,rs2[i]);
      current->is32|=1LL<<HIREG;
      current->is32|=1LL<<LOREG;
      dirty_reg(current,HIREG);
      dirty_reg(current,LOREG);
    }
    else // 64-bit
    {
#ifndef INTERPRET_MULT64
      if((opcode2[i]==0x1C)||(opcode2[i]==0x1D)) // DMULT/DMULTU
      {
        current->u&=~(1LL<<HIREG);
        current->uu&=~(1LL<<HIREG);
        current->u&=~(1LL<<LOREG);
        current->uu&=~(1LL<<LOREG);
      }
#endif
#ifndef INTERPRET_DIV64
      if((opcode2[i]==0x1E)||(opcode2[i]==0x1F)) // DDIV/DIVU
      {
        current->u&=~(1LL<<HIREG);
        current->uu&=~(1LL<<HIREG);
        current->u&=~(1LL<<LOREG);
        current->uu&=~(1LL<<LOREG);
      }
#endif
      alloc_reg64(current,i,HIREG);
      alloc_reg64(current,i,LOREG);
      alloc_reg64(current,i,rs1[i]);
      alloc_reg64(current,i,rs2[i]);
      current->is32&=~(1LL<<HIREG);
      current->is32&=~(1LL<<LOREG);
      dirty_reg(current,HIREG);
      dirty_reg(current,LOREG);
    }
  }
  else
  {
    // Multiply by zero is zero.
    // MIPS does not have a divide by zero exception.
    // The result is undefined, we return zero.
    alloc_reg(current,i,HIREG);
    alloc_reg(current,i,LOREG);
    current->is32|=1LL<<HIREG;
    current->is32|=1LL<<LOREG;
    dirty_reg(current,HIREG);
    dirty_reg(current,LOREG);
  }
}
#endif

static void cop0_alloc(struct regstat *current,int i)
{
  if(opcode2[i]==0) // MFC0
  {
    if(rt1[i]) {
      clear_const(current,rt1[i]);
      alloc_reg(current,i,rt1[i]);
      current->is32|=1LL<<rt1[i];
      dirty_reg(current,rt1[i]);
    }
  }
  else if(opcode2[i]==4) // MTC0
  {
    if(rs1[i]){
      clear_const(current,rs1[i]);
      alloc_reg(current,i,rs1[i]);
    }
    else {
      current->u&=~1LL;
      alloc_reg(current,i,0);
    }
  }
  else
  {
    // TLBR/TLBWI/TLBWR/TLBP/ERET
    assert(opcode2[i]==0x10);

    if((source[i]&0x3f)==0x18) // ERET
    {
      alloc_all(current,i);
      minimum_free_regs[i]=HOST_REGS;
    }
  }
}

static void cop1_alloc(struct regstat *current,int i)
{
  alloc_reg(current,i,CSREG); // Load status
  if(opcode2[i]<3) // MFC1/DMFC1/CFC1
  {
    assert(rt1[i]);
    clear_const(current,rt1[i]);
    if(opcode2[i]==1) {
      alloc_reg64(current,i,rt1[i]); // DMFC1
      current->is32&=~(1LL<<rt1[i]);
    }else{
      alloc_reg(current,i,rt1[i]); // MFC1/CFC1
      current->is32|=1LL<<rt1[i];
    }
    dirty_reg(current,rt1[i]);
    alloc_reg_temp(current,i,-1);
  }
  else if(opcode2[i]>3) // MTC1/DMTC1/CTC1
  {
    if(rs1[i]){
      clear_const(current,rs1[i]);
      if(opcode2[i]==5)
        alloc_reg64(current,i,rs1[i]); // DMTC1
      else
        alloc_reg(current,i,rs1[i]); // MTC1/CTC1
      alloc_reg_temp(current,i,-1);
    }
    else {
      current->u&=~1LL;
      alloc_reg(current,i,0);
      alloc_reg_temp(current,i,-1);
    }
  }
  minimum_free_regs[i]=1;
}

static void fconv_alloc(struct regstat *current,int i)
{
  alloc_reg(current,i,CSREG); // Load status
  alloc_reg_temp(current,i,-1);
  minimum_free_regs[i]=1;
}

static void float_alloc(struct regstat *current,int i)
{
  alloc_reg(current,i,CSREG); // Load status
  alloc_reg_temp(current,i,-1);
  minimum_free_regs[i]=1;
}

static void fcomp_alloc(struct regstat *current,int i)
{
  alloc_reg(current,i,CSREG); // Load status
  alloc_reg(current,i,FSREG); // Load flags
  dirty_reg(current,FSREG); // Flag will be modified
  alloc_reg_temp(current,i,-1);
  minimum_free_regs[i]=1;
}

static void syscall_alloc(struct regstat *current,int i)
{
  alloc_cc(current,i);
  dirty_reg(current,CCREG);
  alloc_all(current,i);
  minimum_free_regs[i]=HOST_REGS;
  current->isconst=0;
}

static void delayslot_alloc(struct regstat *current,int i)
{
  switch(itype[i]) {
    case UJUMP:
    case CJUMP:
    case SJUMP:
    case RJUMP:
    case FJUMP:
    case SYSCALL:
    case SPAN:
      assem_debug("jump in the delay slot.  this shouldn't happen.");//exit(1);
      DebugMessage(M64MSG_VERBOSE, "Disabled speculative precompilation");
      stop_after_jal=1;
      ccadj[i-1]+=1;
      break;
    case IMM16:
      imm16_alloc(current,i);
      break;
    case LOAD:
    case LOADLR:
      load_alloc(current,i);
      break;
    case STORE:
    case STORELR:
      store_alloc(current,i);
      break;
    case ALU:
      alu_alloc(current,i);
      break;
    case SHIFT:
      shift_alloc(current,i);
      break;
    case MULTDIV:
      multdiv_alloc(current,i);
      break;
    case SHIFTIMM:
      shiftimm_alloc(current,i);
      break;
    case MOV:
      mov_alloc(current,i);
      break;
    case COP0:
      cop0_alloc(current,i);
      break;
    case COP1:
      cop1_alloc(current,i);
      break;
    case C1LS:
      c1ls_alloc(current,i);
      break;
    case FCONV:
      fconv_alloc(current,i);
      break;
    case FLOAT:
      float_alloc(current,i);
      break;
    case FCOMP:
      fcomp_alloc(current,i);
      break;
  }
}

// Special case where a branch and delay slot span two pages in virtual memory
static void pagespan_alloc(struct regstat *current,int i)
{
  current->isconst=0;
  current->wasconst=0;
  regs[i].wasconst=0;
  minimum_free_regs[i]=HOST_REGS;
  alloc_all(current,i);
  alloc_cc(current,i);
  dirty_reg(current,CCREG);
  if(opcode[i]==3) // JAL
  {
    alloc_reg(current,i,31);
    dirty_reg(current,31);
  }
  if(opcode[i]==0&&(opcode2[i]&0x3E)==8) // JR/JALR
  {
    alloc_reg(current,i,rs1[i]);
    if (rt1[i]!=0) {
      alloc_reg(current,i,rt1[i]);
      dirty_reg(current,rt1[i]);
    }
  }
  if((opcode[i]&0x2E)==4) // BEQ/BNE/BEQL/BNEL
  {
    if(rs1[i]) alloc_reg(current,i,rs1[i]);
    if(rs2[i]) alloc_reg(current,i,rs2[i]);
    if(!((current->is32>>rs1[i])&(current->is32>>rs2[i])&1))
    {
      if(rs1[i]) alloc_reg64(current,i,rs1[i]);
      if(rs2[i]) alloc_reg64(current,i,rs2[i]);
    }
  }
  else
  if((opcode[i]&0x2E)==6) // BLEZ/BGTZ/BLEZL/BGTZL
  {
    if(rs1[i]) alloc_reg(current,i,rs1[i]);
    if(!((current->is32>>rs1[i])&1))
    {
      if(rs1[i]) alloc_reg64(current,i,rs1[i]);
    }
  }
  else
  if(opcode[i]==0x11) // BC1
  {
    alloc_reg(current,i,FSREG);
    alloc_reg(current,i,CSREG);
  }
  //else ...
}

/**** Assembler utils ****/
static void wb_invalidate(signed char pre[],signed char entry[],uint64_t dirty,uint64_t is32,uint64_t u,uint64_t uu)
{
  int hr;
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG) {
      if(pre[hr]!=entry[hr]) {
        if(pre[hr]>=0) {
          if((dirty>>hr)&1) {
            if(get_reg(entry,pre[hr])<0) {
              if(pre[hr]<64) {
                if(!((u>>pre[hr])&1)) {
                  emit_storereg(pre[hr],hr);
                  if( ((is32>>pre[hr])&1) && !((uu>>pre[hr])&1) ) {
                    emit_sarimm(hr,31,hr);
                    emit_storereg(pre[hr]|64,hr);
                  }
                }
              }else{
                if(!((uu>>(pre[hr]&63))&1) && !((is32>>(pre[hr]&63))&1)) {
                  emit_storereg(pre[hr],hr);
                }
              }
            }
          }
        }
      }
    }
  }
  // Move from one register to another (no writeback)
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG) {
      if(pre[hr]!=entry[hr]) {
        if(pre[hr]>=0&&(pre[hr]&63)<TEMPREG) {
          int nr;
          if((nr=get_reg(entry,pre[hr]))>=0) {
            #ifdef NATIVE_64
            if(pre[hr]>=INVCP) emit_mov64(hr,nr);
            else
            #endif
            emit_mov(hr,nr);
          }
        }
      }
    }
  }
}

// Write out all dirty registers (except cycle count)
static void wb_dirtys(signed char i_regmap[],uint64_t i_is32,uint64_t i_dirty)
{
  int hr;
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG) {
      if(((i_regmap[hr]&63)>0)&&((i_regmap[hr]&63)<CSREG)) {
        if((i_dirty>>hr)&1) {
          if(i_regmap[hr]<64) {
            emit_storereg(i_regmap[hr],hr);
            if(((i_is32>>i_regmap[hr])&1)) {
              #ifdef DESTRUCTIVE_WRITEBACK
              emit_sarimm(hr,31,hr);
              emit_storereg(i_regmap[hr]|64,hr);
              #else
              emit_sarimm(hr,31,HOST_TEMPREG);
              emit_storereg(i_regmap[hr]|64,HOST_TEMPREG);
              #endif
            }
          }else{
            if(!((i_is32>>(i_regmap[hr]&63))&1)) {
              emit_storereg(i_regmap[hr],hr);
            }
          }
        }
      }
    }
  }
}

// Write out dirty registers that we need to reload (pair with load_needed_regs)
// This writes the registers not written by store_regs_bt
static void wb_needed_dirtys(signed char i_regmap[],uint64_t i_is32,uint64_t i_dirty,int addr)
{
  int hr;
  int t=(addr-start)>>2;
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG) {
      if(((i_regmap[hr]&63)>0)&&((i_regmap[hr]&63)<CSREG)) {
        if(i_regmap[hr]==regs[t].regmap_entry[hr] && ((regs[t].dirty>>hr)&1) && !(((i_is32&~regs[t].was32&~unneeded_reg_upper[t])>>(i_regmap[hr]&63))&1)) {
          if((i_dirty>>hr)&1) {
            if(i_regmap[hr]<64) {
              emit_storereg(i_regmap[hr],hr);
              if(((i_is32>>i_regmap[hr])&1)) {
                #ifdef DESTRUCTIVE_WRITEBACK
                emit_sarimm(hr,31,hr);
                emit_storereg(i_regmap[hr]|64,hr);
                #else
                emit_sarimm(hr,31,HOST_TEMPREG);
                emit_storereg(i_regmap[hr]|64,HOST_TEMPREG);
                #endif
              }
            }else{
              if(!((i_is32>>(i_regmap[hr]&63))&1)) {
                emit_storereg(i_regmap[hr],hr);
              }
            }
          }
        }
      }
    }
  }
}

#ifndef DESTRUCTIVE_WRITEBACK
// Sign-extend to 64 bits and write out upper half of a register
// This is useful where we have a 32-bit value in a register, and want to
// keep it in a 32-bit register, but can't guarantee that it won't be read
// as a 64-bit value later.
static void wb_sx(signed char pre[],signed char entry[],uint64_t dirty,uint64_t is32_pre,uint64_t is32,uint64_t u,uint64_t uu)
{
  if(is32_pre==is32) return;
  int hr,reg;
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG) {
      //if(pre[hr]==entry[hr]) {
        if((reg=pre[hr])>=0) {
          if((dirty>>hr)&1) {
            if( ((is32_pre&~is32&~uu)>>reg)&1 ) {
              emit_sarimm(hr,31,HOST_TEMPREG);
              emit_storereg(reg|64,HOST_TEMPREG);
            }
          }
        }
      //}
    }
  }
}

static void wb_valid(signed char pre[],signed char entry[],u_int dirty_pre,u_int dirty,uint64_t is32_pre,uint64_t u,uint64_t uu)
{
  //if(dirty_pre==dirty) return;
  int hr,reg;
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG) {
      reg=pre[hr];
      if(((~u)>>(reg&63))&1) {
        if(((reg&63)>0)&&((reg&63)<CSREG)) {
          if(((dirty_pre&~dirty)>>hr)&1) {
            if(reg<64) {
              emit_storereg(reg,hr);
              if( ((is32_pre&~uu)>>reg)&1 ) {
                emit_sarimm(hr,31,HOST_TEMPREG);
                emit_storereg(reg|64,HOST_TEMPREG);
              }
            } else {
              emit_storereg(reg,hr);
            }
          }
        }
      }
    }
  }
}
#endif

// Write out a single register
static void wb_register(signed char r,signed char regmap[],uint64_t dirty,uint64_t is32)
{
  int hr;
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG) {
      if((regmap[hr]&63)==r) {
        if((dirty>>hr)&1) {
          if(regmap[hr]<64) {
            emit_storereg(r,hr);
            if((is32>>regmap[hr])&1) {
              emit_sarimm(hr,31,hr);
              emit_storereg(r|64,hr);
            }
          }else{
            emit_storereg(r|64,hr);
          }
        }
      }
    }
  }
}

// Store dirty registers prior to branch
static void store_regs_bt(signed char i_regmap[],uint64_t i_is32,uint64_t i_dirty,int addr)
{
  if(internal_branch(i_is32,addr))
  {
    int t=(addr-start)>>2;
    int hr;
    for(hr=0;hr<HOST_REGS;hr++) {
      if(hr!=EXCLUDE_REG) {
        if(((i_regmap[hr]&63)>0)&&((i_regmap[hr]&63)<CSREG)) {
          if(i_regmap[hr]!=regs[t].regmap_entry[hr] || !((regs[t].dirty>>hr)&1) || (((i_is32&~regs[t].was32&~unneeded_reg_upper[t])>>(i_regmap[hr]&63))&1)) {
            if((i_dirty>>hr)&1) {
              if(i_regmap[hr]<64) {
                if(!((unneeded_reg[t]>>i_regmap[hr])&1)) {
                  emit_storereg(i_regmap[hr],hr);
                  if(((i_is32>>i_regmap[hr])&1) && !((unneeded_reg_upper[t]>>i_regmap[hr])&1)) {
                    #ifdef DESTRUCTIVE_WRITEBACK
                    emit_sarimm(hr,31,hr);
                    emit_storereg(i_regmap[hr]|64,hr);
                    #else
                    emit_sarimm(hr,31,HOST_TEMPREG);
                    emit_storereg(i_regmap[hr]|64,HOST_TEMPREG);
                    #endif
                  }
                }
              }else{
                if(!((i_is32>>(i_regmap[hr]&63))&1) && !((unneeded_reg_upper[t]>>(i_regmap[hr]&63))&1)) {
                  emit_storereg(i_regmap[hr],hr);
                }
              }
            }
          }
        }
      }
    }
  }
  else
  {
    // Branch out of this block, write out all dirty regs
    wb_dirtys(i_regmap,i_is32,i_dirty);
  }
}

// Load all needed registers for branch target
static void load_regs_bt(signed char i_regmap[],uint64_t i_is32,uint64_t i_dirty,int addr)
{
  //if(addr>=start && addr<(start+slen*4))
  if(internal_branch(i_is32,addr))
  {
    int t=(addr-start)>>2;
    int hr;
    // Store the cycle count before loading something else
    if(i_regmap[HOST_CCREG]!=CCREG) {
      assert(i_regmap[HOST_CCREG]==-1);
    }
    if(regs[t].regmap_entry[HOST_CCREG]!=CCREG) {
      emit_storereg(CCREG,HOST_CCREG);
    }
    // Load 32-bit regs
    for(hr=0;hr<HOST_REGS;hr++) {
      if(hr!=EXCLUDE_REG&&regs[t].regmap_entry[hr]>=0&&regs[t].regmap_entry[hr]<TEMPREG) {
        #ifdef DESTRUCTIVE_WRITEBACK
        if(i_regmap[hr]!=regs[t].regmap_entry[hr] || ( !((regs[t].dirty>>hr)&1) && ((i_dirty>>hr)&1) && (((i_is32&~unneeded_reg_upper[t])>>i_regmap[hr])&1) ) || (((i_is32&~regs[t].was32&~unneeded_reg_upper[t])>>(i_regmap[hr]&63))&1)) {
        #else
        if(i_regmap[hr]!=regs[t].regmap_entry[hr] ) {
        #endif
          if(regs[t].regmap_entry[hr]==0) {
            emit_zeroreg(hr);
          }
          else if(regs[t].regmap_entry[hr]!=CCREG)
          {
            emit_loadreg(regs[t].regmap_entry[hr],hr);
          }
        }
      }
    }
    //Load 64-bit regs
    for(hr=0;hr<HOST_REGS;hr++) {
      if(hr!=EXCLUDE_REG&&regs[t].regmap_entry[hr]>=64&&regs[t].regmap_entry[hr]<TEMPREG+64) {
        if(i_regmap[hr]!=regs[t].regmap_entry[hr]) {
          assert(regs[t].regmap_entry[hr]!=64);
          if((i_is32>>(regs[t].regmap_entry[hr]&63))&1) {
            int lr=get_reg(regs[t].regmap_entry,regs[t].regmap_entry[hr]-64);
            if(lr<0) {
              emit_loadreg(regs[t].regmap_entry[hr],hr);
            }
            else
            {
              emit_sarimm(lr,31,hr);
            }
          }
          else
          {
            emit_loadreg(regs[t].regmap_entry[hr],hr);
          }
        }
        else if((i_is32>>(regs[t].regmap_entry[hr]&63))&1) {
          int lr=get_reg(regs[t].regmap_entry,regs[t].regmap_entry[hr]-64);
          if(lr<0) {
            emit_loadreg(regs[t].regmap_entry[hr],hr);
          }
          else
          {
            emit_sarimm(lr,31,hr);
          }
        }
      }
    }
  }
}

// Load the specified registers
// This only loads the registers given as arguments because
// we don't want to load things that will be overwritten
static void load_regs(signed char entry[],signed char regmap[],int is32,int rs1,int rs2)
{
  int hr;
  // Load 32-bit regs
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG&&regmap[hr]>=0) {
      if(entry[hr]!=regmap[hr]) {
        if(regmap[hr]==rs1||regmap[hr]==rs2)
        {
          if(regmap[hr]==0) {
            emit_zeroreg(hr);
          }
          else
          {
            emit_loadreg(regmap[hr],hr);
          }
        }
      }
    }
  }
  //Load 64-bit regs
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG&&regmap[hr]>=0) {
      if(entry[hr]!=regmap[hr]) {
        if(regmap[hr]-64==rs1||regmap[hr]-64==rs2)
        {
          assert(regmap[hr]!=64);
          if((is32>>(regmap[hr]&63))&1) {
            int lr=get_reg(regmap,regmap[hr]-64);
            if(lr>=0)
              emit_sarimm(lr,31,hr);
            else
              emit_loadreg(regmap[hr],hr);
          }
          else
          {
            emit_loadreg(regmap[hr],hr);
          }
        }
      }
    }
  }
}

// Load registers with known constants
static void load_consts(signed char pre[],signed char regmap[],int is32,int i)
{
  int hr;
  // Load 32-bit regs
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG&&regmap[hr]>=0) {
      //if(entry[hr]!=regmap[hr]) {
      if(i==0||!((regs[i-1].isconst>>hr)&1)||pre[hr]!=regmap[hr]||bt[i]) {
        if(((regs[i].isconst>>hr)&1)&&regmap[hr]<64&&regmap[hr]>0) {
          int value;
          if(get_final_value(hr,i,&value)) {
            if(value==0) {
              emit_zeroreg(hr);
            }
            else {
              emit_movimm(value,hr);
            }
          }
        }
      }
    }
  }
  // Load 64-bit regs
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG&&regmap[hr]>=0) {
      //if(entry[hr]!=regmap[hr]) {
      if(i==0||!((regs[i-1].isconst>>hr)&1)||pre[hr]!=regmap[hr]||bt[i]) {
        if(((regs[i].isconst>>hr)&1)&&regmap[hr]>64) {
          if((is32>>(regmap[hr]&63))&1) {
            int lr=get_reg(regmap,regmap[hr]-64);
            assert(lr>=0);
            emit_sarimm(lr,31,hr);
          }
          else
          {
            int value;
            if(get_final_value(hr,i,&value)) {
              if(value==0) {
                emit_zeroreg(hr);
              }
              else {
                emit_movimm(value,hr);
              }
            }
          }
        }
      }
    }
  }
}

static void load_all_consts(signed char regmap[],int is32,u_int dirty,u_int isconst,int i)
{
  int hr;
  // Load 32-bit regs
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG&&regmap[hr]>=0&&((dirty>>hr)&1)) {
      if(((isconst>>hr)&1)&&regmap[hr]<64&&regmap[hr]>0) {
        int value=constmap[i][hr];
        if(value==0) {
          emit_zeroreg(hr);
        }
        else {
          emit_movimm(value,hr);
        }
      }
    }
  }
  // Load 64-bit regs
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG&&regmap[hr]>=0&&((dirty>>hr)&1)) {
      if(((isconst>>hr)&1)&&regmap[hr]>64) {
        if((is32>>(regmap[hr]&63))&1) {
          int lr=get_reg(regmap,regmap[hr]-64);
          assert(lr>=0);
          emit_sarimm(lr,31,hr);
        }
        else
        {
          int value=constmap[i][hr];
          if(value==0) {
            emit_zeroreg(hr);
          }
          else {
            emit_movimm(value,hr);
          }
        }
      }
    }
  }
}

// Load all registers (except cycle count)
static void load_all_regs(signed char i_regmap[])
{
  int hr;
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG) {
      if(i_regmap[hr]==0) {
        emit_zeroreg(hr);
      }
      else
      if(i_regmap[hr]>0 && (i_regmap[hr]&63)<TEMPREG && i_regmap[hr]!=CCREG)
      {
        emit_loadreg(i_regmap[hr],hr);
      }
    }
  }
}

// Load all current registers also needed by next instruction
static void load_needed_regs(signed char i_regmap[],signed char next_regmap[])
{
  int hr;
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG) {
      if(get_reg(next_regmap,i_regmap[hr])>=0) {
        if(i_regmap[hr]==0) {
          emit_zeroreg(hr);
        }
        else
        if(i_regmap[hr]>0 && (i_regmap[hr]&63)<TEMPREG && i_regmap[hr]!=CCREG)
        {
          emit_loadreg(i_regmap[hr],hr);
        }
      }
    }
  }
}

// Load all regs, storing cycle count if necessary
static void load_regs_entry(int t)
{
  int hr;
  if(is_ds[t]) emit_addimm(HOST_CCREG,CLOCK_DIVIDER,HOST_CCREG);
  else if(ccadj[t]) emit_addimm(HOST_CCREG,-ccadj[t]*CLOCK_DIVIDER,HOST_CCREG);
  if(regs[t].regmap_entry[HOST_CCREG]!=CCREG) {
    emit_storereg(CCREG,HOST_CCREG);
  }
  // Load 32-bit regs
  for(hr=0;hr<HOST_REGS;hr++) {
    if(regs[t].regmap_entry[hr]>=0&&regs[t].regmap_entry[hr]<TEMPREG) {
      if(regs[t].regmap_entry[hr]==0) {
        emit_zeroreg(hr);
      }
      else if(regs[t].regmap_entry[hr]!=CCREG)
      {
        emit_loadreg(regs[t].regmap_entry[hr],hr);
      }
    }
  }
  // Load 64-bit regs
  for(hr=0;hr<HOST_REGS;hr++) {
    if(regs[t].regmap_entry[hr]>=64&&regs[t].regmap_entry[hr]<TEMPREG+64) {
      assert(regs[t].regmap_entry[hr]!=64);
      if((regs[t].was32>>(regs[t].regmap_entry[hr]&63))&1) {
        int lr=get_reg(regs[t].regmap_entry,regs[t].regmap_entry[hr]-64);
        if(lr<0) {
          emit_loadreg(regs[t].regmap_entry[hr],hr);
        }
        else
        {
          emit_sarimm(lr,31,hr);
        }
      }
      else
      {
        emit_loadreg(regs[t].regmap_entry[hr],hr);
      }
    }
  }
}

// Load registers prior to the start of a loop
// so that they are not loaded within the loop
static void loop_preload(signed char pre[],signed char entry[])
{
  int hr;
  for(hr=0;hr<HOST_REGS;hr++) {
    if(hr!=EXCLUDE_REG) {
      if(pre[hr]!=entry[hr]) {
        if(entry[hr]>=0) {
          if(get_reg(pre,entry[hr])<0) {
            assem_debug("loop preload:");
            //DebugMessage(M64MSG_VERBOSE, "loop preload: %d",hr);
            if(entry[hr]==0) {
              emit_zeroreg(hr);
            }
            else if(entry[hr]<TEMPREG)
            {
              emit_loadreg(entry[hr],hr);
            }
            else if(entry[hr]-64<TEMPREG)
            {
              emit_loadreg(entry[hr],hr);
            }
          }
        }
      }
    }
  }
}

// Generate address for load/store instruction
static void address_generation(int i,struct regstat *i_regs,signed char entry[])
{
  if(i>=MAXBLOCK) return;
  if(itype[i]==LOAD||itype[i]==LOADLR||itype[i]==STORE||itype[i]==STORELR||itype[i]==C1LS) {
    int ra=0;
    int agr=AGEN1+(i&1);
    int mgr=MGEN1+(i&1);
    if(itype[i]==LOAD) {
      ra=get_reg(i_regs->regmap,agr);
      if(ra<0) ra=get_reg(i_regs->regmap,-1);
      assert(ra>=0);
    }
    if(itype[i]==LOADLR) {
      ra=get_reg(i_regs->regmap,FTEMP);
    }
    if(itype[i]==STORE||itype[i]==STORELR) {
      ra=get_reg(i_regs->regmap,agr);
      if(ra<0) ra=get_reg(i_regs->regmap,-1);
    }
    if(itype[i]==C1LS) {
      if (opcode[i]==0x31||opcode[i]==0x35) // LWC1/LDC1
        ra=get_reg(i_regs->regmap,FTEMP);
      else { // SWC1/SDC1
        ra=get_reg(i_regs->regmap,agr);
        if(ra<0) ra=get_reg(i_regs->regmap,-1);
      }
    }
    int rs=get_reg(i_regs->regmap,rs1[i]);
    int rm=get_reg(i_regs->regmap,TLREG);
    if(ra>=0) {
      int offset=imm[i];
      int c=(i_regs->wasconst>>rs)&1;
      if(rs1[i]==0) {
        // Using r0 as a base address
        /*if(rm>=0) {
          if(!entry||entry[rm]!=mgr) {
            generate_map_const(offset,rm);
          } // else did it in the previous cycle
        }*/
        if(!entry||entry[ra]!=agr) {
          emit_movimm(offset,ra);
        } // else did it in the previous cycle
      }
      else if(rs<0) {
        if(!entry||entry[ra]!=rs1[i])
          emit_loadreg(rs1[i],ra);
        //if(!entry||entry[ra]!=rs1[i])
        //  DebugMessage(M64MSG_VERBOSE, "poor load scheduling!");
      }
      else if(c) {
        if(rm>=0) {
          if(!entry||entry[rm]!=mgr) {
            if(itype[i]==STORE||itype[i]==STORELR||opcode[i]==0x39||opcode[i]==0x3D) {
              // Stores to memory go thru the mapper to detect self-modifying
              // code, loads don't.
              if((unsigned int)(constmap[i][rs]+offset)>=0xC0000000 ||
                 (unsigned int)(constmap[i][rs]+offset)<0x80800000 )
                generate_map_const(constmap[i][rs]+offset,rm);
            }else{
              if((signed int)(constmap[i][rs]+offset)>=(signed int)0xC0000000)
                generate_map_const(constmap[i][rs]+offset,rm);
            }
          }
        }

        if(!entry||entry[ra]!=agr) {
          int load=0;
          #ifndef INTERPRET_LOAD
          load|=(itype[i]==LOAD);
          #endif
          #ifndef INTERPRET_C1LS
          load|=(opcode[i]==0x31||opcode[i]==0x35);
          #endif
          #ifndef INTERPRET_LOADLR
          load|=(opcode[i]==0x22||opcode[i]==0x26||opcode[i]==0x1a||opcode[i]==0x1b);
          #endif

          #ifdef HOST_IMM_ADDR32
          // on x86 when source reg is constant
          // LB/LBU/LH/LHU/LW/LWU/LD/LWC1/LDC1 doesn't need address generation when not using tlb
          // SDL/SDR/SB/SH/SW/SD/SWC1/SDC1 always need address generation
          // ROREG not required
          if(!load||(using_tlb&&((signed int)constmap[i][rs]+offset)>=(signed int)0xC0000000))
          #endif
          #if defined(RAM_OFFSET) && !defined(NATIVE_64)
          // on arm address generation is always required
          // When LB/LBU/LH/LHU/LW/LWU/LD/LWC1/LDC1 loading from rdram, add rdram address to load address to avoid loading ROREG
          // ROREG only required for SDL/SDR/SB/SH/SW/SD/SWC1/SDC1 and when not loading from rdram
          if(load&&(signed int)constmap[i][rs]+offset<(signed int)0x80800000)
            emit_movimm(constmap[i][rs]+offset+(intptr_t)g_dev.rdram.dram-(intptr_t)0x80000000,ra);
          else
          #endif
          // on arm64 and x64 when source reg is constant
          // address generation is always required
          // ROREG is always required
          emit_movimm(constmap[i][rs]+offset,ra);

        } // else did it in the previous cycle
      }
      if(offset&&!c&&rs1[i]) {
        if(rs>=0) {
          emit_addimm(rs,offset,ra);
        }else{
          emit_addimm(ra,offset,ra);
        }
      }
    }
  }
  // Preload constants for next instruction
  if(i+1>=MAXBLOCK) return;
  if(itype[i+1]==LOAD||itype[i+1]==LOADLR||itype[i+1]==STORE||itype[i+1]==STORELR||itype[i+1]==C1LS) {
    int agr,ra;
    #if (NEW_DYNAREC!=NEW_DYNAREC_X86) && (NEW_DYNAREC!=NEW_DYNAREC_X64)
    // Mapper entry
    agr=MGEN1+((i+1)&1);
    ra=get_reg(i_regs->regmap,agr);
    if(ra>=0) {
      int rs=get_reg(regs[i+1].regmap,rs1[i+1]);
      int offset=imm[i+1];
      int c=(regs[i+1].wasconst>>rs)&1;
      if(c) {
        if(itype[i+1]==STORE||itype[i+1]==STORELR||opcode[i+1]==0x39||opcode[i+1]==0x3D) {
          // Stores to memory go thru the mapper to detect self-modifying
          // code, loads don't.
          if((unsigned int)(constmap[i+1][rs]+offset)>=0xC0000000 ||
             (unsigned int)(constmap[i+1][rs]+offset)<0x80800000 )
            generate_map_const(constmap[i+1][rs]+offset,ra);
        }else{
          if((signed int)(constmap[i+1][rs]+offset)>=(signed int)0xC0000000)
            generate_map_const(constmap[i+1][rs]+offset,ra);
        }
      }
      /*else if(rs1[i]==0) {
        generate_map_const(offset,ra);
      }*/
    }
    #endif
    // Actual address
    agr=AGEN1+((i+1)&1);
    ra=get_reg(i_regs->regmap,agr);
    if(ra>=0) {
      int rs=get_reg(regs[i+1].regmap,rs1[i+1]);
      int offset=imm[i+1];
      int c=(regs[i+1].wasconst>>rs)&1;
      if(c) {
        int load=0;
        #ifndef INTERPRET_LOAD
        load|=(itype[i+1]==LOAD);
        #endif
        #ifndef INTERPRET_C1LS
        load|=(opcode[i+1]==0x31||opcode[i+1]==0x35);
        #endif
        #ifndef INTERPRET_LOADLR
        load|=(opcode[i+1]==0x22||opcode[i+1]==0x26||opcode[i+1]==0x1a||opcode[i+1]==0x1b);
        #endif

        #ifdef HOST_IMM_ADDR32
        if(!load||(using_tlb&&((signed int)constmap[i+1][rs]+offset)>=(signed int)0xC0000000))
        #endif
        #if defined(RAM_OFFSET) && !defined(NATIVE_64)
        if(load&&(signed int)constmap[i+1][rs]+offset<(signed int)0x80800000)
          emit_movimm(constmap[i+1][rs]+offset+(intptr_t)g_dev.rdram.dram-(intptr_t)0x80000000,ra);
        else
        #endif
        emit_movimm(constmap[i+1][rs]+offset,ra);
      }
      else if(rs1[i+1]==0) {
        // Using r0 as a base address
        emit_movimm(offset,ra);
      }
    }
  }
}

static void emit_extjump(intptr_t addr, int target)
{
  emit_extjump2(addr, target, (intptr_t)dyna_linker);
}

static void emit_extjump_ds(intptr_t addr, int target)
{
  emit_extjump2(addr, target, (intptr_t)dyna_linker_ds);
}

static void do_cc(int i,signed char i_regmap[],int *adj,int addr,int taken,int invert)
{
  int count;
  intptr_t jaddr;
  intptr_t idle=0;
  if(itype[i]==RJUMP)
  {
    *adj=0;
  }
  //if(ba[i]>=start && ba[i]<(start+slen*4))
  if(internal_branch(branch_regs[i].is32,ba[i]))
  {
    int t=(ba[i]-start)>>2;
    if(is_ds[t]) *adj=-1; // Branch into delay slot adds an extra cycle
    else *adj=ccadj[t];
  }
  else
  {
    *adj=0;
  }
  count=ccadj[i];
  if(taken==TAKEN && i==(ba[i]-start)>>2 && source[i+1]==0) {
    // Idle loop
    idle=(intptr_t)out;
    emit_test(HOST_CCREG,HOST_CCREG);
#if NEW_DYNAREC >= NEW_DYNAREC_ARM
    emit_cmovs_imm(0,HOST_CCREG);
#else
    emit_cmovs(&const_zero,HOST_CCREG);
#endif
    emit_addimm(HOST_CCREG,CLOCK_DIVIDER*2,HOST_CCREG);
    jaddr=(intptr_t)out;
    emit_jmp(0);
  }
  else if(*adj==0||invert) {
    if(g_dev.r4300.cp0.count_per_op_denom_pot) {
      count += (1 << g_dev.r4300.cp0.count_per_op_denom_pot) - 1;
      count >>= g_dev.r4300.cp0.count_per_op_denom_pot;
    }
    emit_addimm_and_set_flags(CLOCK_DIVIDER*(count+2),HOST_CCREG);
    jaddr=(intptr_t)out;
    emit_jns(0);
  }
  else
  {
    emit_cmpimm(HOST_CCREG,-(int)CLOCK_DIVIDER*(count+2));
    jaddr=(intptr_t)out;
    emit_jns(0);
  }
  add_stub(CC_STUB,jaddr,idle?idle:(intptr_t)out,(*adj==0||invert||idle)?0:(count+2),i,addr,taken,0);
}

/**** Stubs ****/
static void do_ccstub(int n)
{
  literal_pool(256);
  assem_debug("do_ccstub %x",start+stubs[n][4]*4);
  set_jump_target(stubs[n][1],(intptr_t)out);
  int i=stubs[n][4];
  if(stubs[n][6]==NULLDS) {
    // Delay slot instruction is nullified ("likely" branch)
    wb_dirtys(regs[i].regmap,regs[i].is32,regs[i].dirty);
  }
  else if(stubs[n][6]!=TAKEN) {
    wb_dirtys(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty);
  }
  else {
    if(internal_branch(branch_regs[i].is32,ba[i]))
      wb_needed_dirtys(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
  }
  if(stubs[n][5]!=-1)
  {
    // Save PC as return address
    emit_movimm(stubs[n][5],0);
    emit_writeword(0,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.pcaddr);
  }
  else
  {
    // Return address depends on which way the branch goes
    if(itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP)
    {
      int s1l=get_reg(branch_regs[i].regmap,rs1[i]);
      int s1h=get_reg(branch_regs[i].regmap,rs1[i]|64);
      int s2l=get_reg(branch_regs[i].regmap,rs2[i]);
      int s2h=get_reg(branch_regs[i].regmap,rs2[i]|64);
      if(rs1[i]==0)
      {
        s1l=s2l;s1h=s2h;
        s2l=s2h=-1;
      }
      else if(rs2[i]==0)
      {
        s2l=s2h=-1;
      }
      if((branch_regs[i].is32>>rs1[i])&(branch_regs[i].is32>>rs2[i])&1) {
        s1h=s2h=-1;
      }
      assert(s1l>=0);
      #ifdef DESTRUCTIVE_WRITEBACK
      if(rs1[i]) {
        if((branch_regs[i].dirty>>s1l)&(branch_regs[i].is32>>rs1[i])&1)
          emit_loadreg(rs1[i],s1l);
      }
      else {
        if((branch_regs[i].dirty>>s1l)&(branch_regs[i].is32>>rs2[i])&1)
          emit_loadreg(rs2[i],s1l);
      }
      if(s2l>=0)
        if((branch_regs[i].dirty>>s2l)&(branch_regs[i].is32>>rs2[i])&1)
          emit_loadreg(rs2[i],s2l);
      #endif
      int hr=0;
      int addr,alt,ntaddr;
      while(hr<HOST_REGS)
      {
        if(hr!=EXCLUDE_REG && hr!=HOST_CCREG &&
           (branch_regs[i].regmap[hr]&63)!=rs1[i] &&
           (branch_regs[i].regmap[hr]&63)!=rs2[i] )
        {
          addr=hr++;break;
        }
        hr++;
      }
      while(hr<HOST_REGS)
      {
        if(hr!=EXCLUDE_REG && hr!=HOST_CCREG &&
           (branch_regs[i].regmap[hr]&63)!=rs1[i] &&
           (branch_regs[i].regmap[hr]&63)!=rs2[i] )
        {
          alt=hr++;break;
        }
        hr++;
      }
      if((opcode[i]&0x2E)==6) // BLEZ/BGTZ needs another register
      {
        while(hr<HOST_REGS)
        {
          if(hr!=EXCLUDE_REG && hr!=HOST_CCREG &&
             (branch_regs[i].regmap[hr]&63)!=rs1[i] &&
             (branch_regs[i].regmap[hr]&63)!=rs2[i] )
          {
            ntaddr=hr;break;
          }
          hr++;
        }
        assert(hr<HOST_REGS);
      }
      if((opcode[i]&0x2f)==4) // BEQ
      {
        #ifdef HAVE_CMOV_IMM
        if(s1h<0) {
          if(s2l>=0) emit_cmp(s1l,s2l);
          else emit_test(s1l,s1l);
          emit_cmov2imm_e_ne_compact(ba[i],start+i*4+8,addr);
        }
        else
        #endif
        {
          emit_mov2imm_compact(ba[i],addr,start+i*4+8,alt);
          if(s1h>=0) {
            if(s2h>=0) emit_cmp(s1h,s2h);
            else emit_test(s1h,s1h);
            emit_cmovne_reg(alt,addr);
          }
          if(s2l>=0) emit_cmp(s1l,s2l);
          else emit_test(s1l,s1l);
          emit_cmovne_reg(alt,addr);
        }
      }
      if((opcode[i]&0x2f)==5) // BNE
      {
        #ifdef HAVE_CMOV_IMM
        if(s1h<0) {
          if(s2l>=0) emit_cmp(s1l,s2l);
          else emit_test(s1l,s1l);
          emit_cmov2imm_e_ne_compact(start+i*4+8,ba[i],addr);
        }
        else
        #endif
        {
          emit_mov2imm_compact(start+i*4+8,addr,ba[i],alt);
          if(s1h>=0) {
            if(s2h>=0) emit_cmp(s1h,s2h);
            else emit_test(s1h,s1h);
            emit_cmovne_reg(alt,addr);
          }
          if(s2l>=0) emit_cmp(s1l,s2l);
          else emit_test(s1l,s1l);
          emit_cmovne_reg(alt,addr);
        }
      }
      if((opcode[i]&0x2f)==6) // BLEZ
      {
        //emit_movimm(ba[i],alt);
        //emit_movimm(start+i*4+8,addr);
        emit_mov2imm_compact(ba[i],alt,start+i*4+8,addr);
        emit_cmpimm(s1l,1);
        if(s1h>=0) emit_mov(addr,ntaddr);
        emit_cmovl_reg(alt,addr);
        if(s1h>=0) {
          emit_test(s1h,s1h);
          emit_cmovne_reg(ntaddr,addr);
          emit_cmovs_reg(alt,addr);
        }
      }
      if((opcode[i]&0x2f)==7) // BGTZ
      {
        //emit_movimm(ba[i],addr);
        //emit_movimm(start+i*4+8,ntaddr);
        emit_mov2imm_compact(ba[i],addr,start+i*4+8,ntaddr);
        emit_cmpimm(s1l,1);
        if(s1h>=0) emit_mov(addr,alt);
        emit_cmovl_reg(ntaddr,addr);
        if(s1h>=0) {
          emit_test(s1h,s1h);
          emit_cmovne_reg(alt,addr);
          emit_cmovs_reg(ntaddr,addr);
        }
      }
      if((opcode[i]==1)&&(opcode2[i]&0x2D)==0) // BLTZ
      {
        //emit_movimm(ba[i],alt);
        //emit_movimm(start+i*4+8,addr);
        emit_mov2imm_compact(ba[i],alt,start+i*4+8,addr);
        if(s1h>=0) emit_test(s1h,s1h);
        else emit_test(s1l,s1l);
        emit_cmovs_reg(alt,addr);
      }
      if((opcode[i]==1)&&(opcode2[i]&0x2D)==1) // BGEZ
      {
        //emit_movimm(ba[i],addr);
        //emit_movimm(start+i*4+8,alt);
        emit_mov2imm_compact(ba[i],addr,start+i*4+8,alt);
        if(s1h>=0) emit_test(s1h,s1h);
        else emit_test(s1l,s1l);
        emit_cmovs_reg(alt,addr);
      }
      if(opcode[i]==0x11 && opcode2[i]==0x08 ) {
        if(source[i]&0x10000) // BC1T
        {
          //emit_movimm(ba[i],alt);
          //emit_movimm(start+i*4+8,addr);
          emit_mov2imm_compact(ba[i],alt,start+i*4+8,addr);
          emit_testimm(s1l,0x800000);
          emit_cmovne_reg(alt,addr);
        }
        else // BC1F
        {
          //emit_movimm(ba[i],addr);
          //emit_movimm(start+i*4+8,alt);
          emit_mov2imm_compact(ba[i],addr,start+i*4+8,alt);
          emit_testimm(s1l,0x800000);
          emit_cmovne_reg(alt,addr);
        }
      }
      emit_writeword(addr,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.pcaddr);
    }
    else
    if(itype[i]==RJUMP)
    {
      int r=get_reg(branch_regs[i].regmap,rs1[i]);
      if((rs1[i]==rt1[i+1]||rs1[i]==rt2[i+1])&&(rs1[i]!=0)) {
        r=get_reg(branch_regs[i].regmap,RTEMP);
      }
      emit_writeword(r,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.pcaddr);
    }
    else {DebugMessage(M64MSG_ERROR, "Unknown branch type in do_ccstub");exit(1);}
  }
  // Update cycle count
  assert(branch_regs[i].regmap[HOST_CCREG]==CCREG||branch_regs[i].regmap[HOST_CCREG]==-1);
  if(stubs[n][3]) emit_addimm(HOST_CCREG,CLOCK_DIVIDER*stubs[n][3],HOST_CCREG);
  emit_call((intptr_t)cc_interrupt);
  if(stubs[n][3]) emit_addimm(HOST_CCREG,-(int)CLOCK_DIVIDER*stubs[n][3],HOST_CCREG);
  if(stubs[n][6]==TAKEN) {
    if(internal_branch(branch_regs[i].is32,ba[i]))
      load_needed_regs(branch_regs[i].regmap,regs[(ba[i]-start)>>2].regmap_entry);
    else if(itype[i]==RJUMP) {
      int r=get_reg(branch_regs[i].regmap,rs1[i]);
      if((rs1[i]==rt1[i+1]||rs1[i]==rt2[i+1])&&(rs1[i]!=0)) {
        r=get_reg(branch_regs[i].regmap,RTEMP);
      }
#if NEW_DYNAREC==NEW_DYNAREC_ARM64
      if(r==18) {
        // x18 is used for trampoline jumps, move it to another register (x0)
        emit_mov(r,0);
        r=0;
        stubs[n][2]=jump_vaddr_reg[0];
      }
#endif
      emit_readword((intptr_t)&g_dev.r4300.new_dynarec_hot_state.pcaddr,r);
    }
  }else if(stubs[n][6]==NOTTAKEN) {
    if(i<slen-2) load_needed_regs(branch_regs[i].regmap,regmap_pre[i+2]);
    else load_all_regs(branch_regs[i].regmap);
  }else if(stubs[n][6]==NULLDS) {
    // Delay slot instruction is nullified ("likely" branch)
    if(i<slen-2) load_needed_regs(regs[i].regmap,regmap_pre[i+2]);
    else load_all_regs(regs[i].regmap);
  }else{
    load_all_regs(branch_regs[i].regmap);
  }
  emit_jmp(stubs[n][2]); // return address
}

static void do_cop1stub(int n)
{
  literal_pool(256);
  assem_debug("do_cop1stub %x",start+stubs[n][3]*4);
  set_jump_target(stubs[n][1],(intptr_t)out);
  int i=stubs[n][3];
  struct regstat *i_regs=(struct regstat *)stubs[n][5];
  int ds=stubs[n][6];
  if(!ds) {
    load_all_consts(regs[i].regmap_entry,regs[i].was32,regs[i].wasdirty,regs[i].wasconst,i);
    //if(i_regs!=&regs[i]) DebugMessage(M64MSG_VERBOSE, "oops: regs[i]=%x i_regs=%x",(int)&regs[i],(int)i_regs);
  }
  wb_dirtys(i_regs->regmap_entry,i_regs->was32,i_regs->wasdirty);

  if(get_reg(i_regs->regmap,CCREG)<0) {
    emit_loadreg(CCREG,HOST_CCREG);
  }

  emit_movimm(start+(i*4)+ds,0); // Get PC
  emit_addimm(HOST_CCREG,CLOCK_DIVIDER*ccadj[i],HOST_CCREG); // CHECK: is this right?  There should probably be an extra cycle...
  emit_jmp((intptr_t)fp_exception);
}

static void do_readstub(int n)
{
  assem_debug("do_readstub %x",start+stubs[n][3]*4);
  literal_pool(256);
  set_jump_target(stubs[n][1],(intptr_t)out);
  int type=stubs[n][0];
  int i=stubs[n][3];
  int addr=stubs[n][4];
  struct regstat *i_regs=(struct regstat *)stubs[n][5];
  u_int reglist=stubs[n][7];
  signed char *i_regmap=i_regs->regmap;
  int rth,rt;

  if(itype[i]==C1LS) {
    rth=get_reg(i_regmap,FTEMP|64);
    rt=get_reg(i_regmap,FTEMP);
  }else{
    rth=get_reg(i_regmap,rt1[i]|64);
    rt=get_reg(i_regmap,rt1[i]);
  }

  assert(addr>=0);
  emit_writeword(addr,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.address);

  intptr_t ftable=0;
  if(type==LOADB_STUB||type==LOADBU_STUB)
    ftable=(intptr_t)read_byte_new;
  else if(type==LOADH_STUB||type==LOADHU_STUB)
    ftable=(intptr_t)read_hword_new;
  else if(type==LOADW_STUB||type==LOADWU_STUB)
    ftable=(intptr_t)read_word_new;
  else if(type==LOADD_STUB)
    ftable=(intptr_t)read_dword_new;
  else if(type==LOADWL_STUB)
  {
    assert(rt>=0);
    ftable=(intptr_t)LWL_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==LOADWR_STUB)
  {
    assert(rt>=0);
    ftable=(intptr_t)LWR_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==LOADDL_STUB)
  {
    assert(rt>=0);
    assert(rth>=0);
    ftable=(intptr_t)LDL_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword);
    emit_writeword(rth,((intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword)+4);
  }
  else if(type==LOADDR_STUB)
  {
    assert(rt>=0);
    assert(rth>=0);
    ftable=(intptr_t)LDR_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword);
    emit_writeword(rth,((intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword)+4);
  }

  int cc=get_reg(i_regmap,CCREG);
  if(cc>=0) {
    emit_storereg(CCREG,cc);
  }

  save_regs(reglist);
  int ds=i_regs!=&regs[i];

#if NEW_DYNAREC == NEW_DYNAREC_X86
  emit_pushimm(CLOCK_DIVIDER*(stubs[n][6]+1));
  emit_pushimm((start+(i+1)*4)+ds);
  emit_call((intptr_t)ftable);
  emit_addimm(ESP,8,ESP);
#else
  emit_movimm((start+(i+1)*4)+ds,ARG1_REG);
  emit_movimm(CLOCK_DIVIDER*(stubs[n][6]+1),ARG2_REG);
  emit_call((intptr_t)ftable);
#endif

  restore_regs(reglist);

  emit_cmpmem_imm((intptr_t)&g_dev.r4300.new_dynarec_hot_state.pending_exception,0);
  intptr_t jaddr=(intptr_t)out;
  emit_jeq(0);

  if(!ds) load_all_consts(regs[i].regmap_entry,regs[i].was32,regs[i].wasdirty,regs[i].wasconst,i);
  wb_dirtys(i_regs->regmap_entry,i_regs->was32,i_regs->wasdirty);

  emit_jmp((intptr_t)&do_interrupt);
  set_jump_target(jaddr,(intptr_t)out);

  if(rt>=0) {
    if(type==LOADB_STUB)
      emit_movsbl((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword,rt);
    else if(type==LOADBU_STUB)
      emit_movzbl((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword,rt);
    else if(type==LOADH_STUB)
      emit_movswl((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword,rt);
    else if(type==LOADHU_STUB)
      emit_movzwl((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword,rt);
    else if(type==LOADW_STUB||type==LOADWU_STUB||type==LOADWL_STUB||type==LOADWR_STUB) {
      emit_readword((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword,rt);
      if(type==LOADWU_STUB) emit_zeroreg(rth);
    }
    else if(type==LOADD_STUB||type==LOADDL_STUB||type==LOADDR_STUB) {
      emit_readword((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword,rt);
      if(rth>=0) emit_readword((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword+4,rth);
    }
  }
  emit_jmp(stubs[n][2]); // return address
}

static void inline_readstub(int type, int i, u_int addr_const, char addr, struct regstat *i_regs, int target, int adj, u_int reglist)
{
  assem_debug("inline_readstub");
  int rth=get_reg(i_regs->regmap,target|64);
  int rt=get_reg(i_regs->regmap,target);

#if NEW_DYNAREC <= NEW_DYNAREC_X64
  if(addr_const)
    emit_writeword_imm(addr_const,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.address);
  else
#endif
  {
    assert(addr>=0);
    emit_writeword(addr,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.address);
  }

  intptr_t ftable=0;
  if(type==LOADB_STUB||type==LOADBU_STUB)
    ftable=(intptr_t)read_byte_new;
  else if(type==LOADH_STUB||type==LOADHU_STUB)
    ftable=(intptr_t)read_hword_new;
  else if(type==LOADW_STUB||type==LOADWU_STUB)
    ftable=(intptr_t)read_word_new;
  else if(type==LOADD_STUB)
    ftable=(intptr_t)read_dword_new;
  else if(type==LOADWL_STUB)
  {
    assert(rt>=0);
    ftable=(intptr_t)LWL_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==LOADWR_STUB)
  {
    assert(rt>=0);
    ftable=(intptr_t)LWR_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==LOADDL_STUB)
  {
    assert(rt>=0);
    assert(rth>=0);
    ftable=(intptr_t)LDL_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword);
    emit_writeword(rth,((intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword)+4);
  }
  else if(type==LOADDR_STUB)
  {
    assert(rt>=0);
    assert(rth>=0);
    ftable=(intptr_t)LDR_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword);
    emit_writeword(rth,((intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword)+4);
  }

  int cc=get_reg(i_regs->regmap,CCREG);
  if(cc>=0) {
    emit_storereg(CCREG,cc);
  }

  save_regs(reglist);
  int ds=i_regs!=&regs[i];

#if NEW_DYNAREC == NEW_DYNAREC_X86
  emit_pushimm(CLOCK_DIVIDER*(adj+1));
  emit_pushimm((start+(i+1)*4)+ds);
  emit_call((intptr_t)ftable);
  emit_addimm(ESP,8,ESP);
#else
  emit_movimm((start+(i+1)*4)+ds,ARG1_REG);
  emit_movimm(CLOCK_DIVIDER*(adj+1),ARG2_REG);
  emit_call((intptr_t)ftable);
#endif

  restore_regs(reglist);

  if((signed int)addr_const>=(signed int)0xC0000000) {
    // Theoretically we can have a pagefault here, if the TLB has never
    // been enabled and the address is outside the range 80000000..BFFFFFFF
    // Write out the registers so the pagefault can be handled.  This is
    // a very rare case and likely represents a bug.
    emit_cmpmem_imm((intptr_t)&g_dev.r4300.new_dynarec_hot_state.pending_exception,0);
    intptr_t jaddr=(intptr_t)out;
    emit_jeq(0);

    if(!ds) load_all_consts(regs[i].regmap_entry,regs[i].was32,regs[i].wasdirty,regs[i].wasconst,i);
    wb_dirtys(i_regs->regmap_entry,i_regs->was32,i_regs->wasdirty);

    emit_jmp((intptr_t)&do_interrupt);
    set_jump_target(jaddr,(intptr_t)out);
  }

  if(rt>=0) {
    if(type==LOADB_STUB)
      emit_movsbl((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword,rt);
    else if(type==LOADBU_STUB)
      emit_movzbl((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword,rt);
    else if(type==LOADH_STUB)
      emit_movswl((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword,rt);
    else if(type==LOADHU_STUB)
      emit_movzwl((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword,rt);
    else if(type==LOADW_STUB||type==LOADWU_STUB||type==LOADWL_STUB||type==LOADWR_STUB) {
      emit_readword((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword,rt);
      if(type==LOADWU_STUB) emit_zeroreg(rth);
    }
    else if(type==LOADD_STUB||type==LOADDL_STUB||type==LOADDR_STUB) {
      emit_readword((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword,rt);
      if(rth>=0) emit_readword((intptr_t)&g_dev.r4300.new_dynarec_hot_state.rdword+4,rth);
    }
  }
}

static void do_writestub(int n)
{
  assem_debug("do_writestub %x",start+stubs[n][3]*4);
  literal_pool(256);
  set_jump_target(stubs[n][1],(intptr_t)out);
  int type=stubs[n][0];
  int i=stubs[n][3];
  int addr=stubs[n][4];
  struct regstat *i_regs=(struct regstat *)stubs[n][5];
  u_int reglist=stubs[n][7];
  signed char *i_regmap=i_regs->regmap;
  int rth,rt,r;

  if(itype[i]==C1LS) {
    rth=get_reg(i_regmap,FTEMP|64);
    rt=get_reg(i_regmap,r=FTEMP);
  }else{
    rth=get_reg(i_regmap,rs2[i]|64);
    rt=get_reg(i_regmap,r=rs2[i]);
  }
  assert(addr>=0);
  assert(rt>=0);
  emit_writeword(addr,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.address);

  intptr_t ftable=0;
  if(type==STOREB_STUB){
    ftable=(intptr_t)write_byte_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==STOREH_STUB){
    ftable=(intptr_t)write_hword_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==STOREW_STUB){
    ftable=(intptr_t)write_word_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==STORED_STUB){
    ftable=(intptr_t)write_dword_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword);
    emit_writeword(r?rth:rt,((intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword)+4);
  }
  else if(type==STOREWL_STUB){
    ftable=(intptr_t)SWL_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==STOREWR_STUB){
    ftable=(intptr_t)SWR_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==STOREDL_STUB){
    ftable=(intptr_t)SDL_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword);
    emit_writeword(r?rth:rt,((intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword)+4);
  }
  else if(type==STOREDR_STUB){
    ftable=(intptr_t)SDR_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword);
    emit_writeword(r?rth:rt,((intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword)+4);
  }

  int cc=get_reg(i_regmap,CCREG);
  if(cc>=0) {
    emit_storereg(CCREG,cc);
  }

  save_regs(reglist);
  int ds=i_regs!=&regs[i];

#if NEW_DYNAREC == NEW_DYNAREC_X86
  emit_pushimm(CLOCK_DIVIDER*(stubs[n][6]+1));
  emit_pushimm((start+(i+1)*4)+ds);
  emit_call((intptr_t)ftable);
  emit_addimm(ESP,8,ESP);
#else
  emit_movimm((start+(i+1)*4)+ds,ARG1_REG);
  emit_movimm(CLOCK_DIVIDER*(stubs[n][6]+1),ARG2_REG);
  emit_call((intptr_t)ftable);
#endif

  restore_regs(reglist);

  emit_cmpmem_imm((intptr_t)&g_dev.r4300.new_dynarec_hot_state.pending_exception,0);
  intptr_t jaddr=(intptr_t)out;
  emit_jeq(0);

  if(!ds) load_all_consts(regs[i].regmap_entry,regs[i].was32,regs[i].wasdirty,regs[i].wasconst,i);
  wb_dirtys(i_regs->regmap_entry,i_regs->was32,i_regs->wasdirty);

  emit_jmp((intptr_t)&do_interrupt);
  set_jump_target(jaddr,(intptr_t)out);

  if(cc>=0) {
    emit_loadreg(CCREG,cc);
  }
  emit_jmp(stubs[n][2]); // return address
}

static void inline_writestub(int type, int i, u_int addr_const, char addr, struct regstat *i_regs, int target, int adj, u_int reglist)
{
  assem_debug("inline_writestub");
  int rth=get_reg(i_regs->regmap,target|64);
  int rt=get_reg(i_regs->regmap,target);
  assert(rt>=0);

#if NEW_DYNAREC <= NEW_DYNAREC_X64
  if(addr_const)
    emit_writeword_imm(addr_const,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.address);
  else
#endif
  {
    assert(addr>=0);
    emit_writeword(addr,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.address);
  }

  intptr_t ftable=0;
  if(type==STOREB_STUB){
    ftable=(intptr_t)write_byte_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==STOREH_STUB){
    ftable=(intptr_t)write_hword_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==STOREW_STUB){
    ftable=(intptr_t)write_word_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==STORED_STUB){
    ftable=(intptr_t)write_dword_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword);
    emit_writeword(target?rth:rt,((intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword)+4);
  }
  else if(type==STOREWL_STUB){
    ftable=(intptr_t)SWL_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==STOREWR_STUB){
    ftable=(intptr_t)SWR_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wword);
  }
  else if(type==STOREDL_STUB){
    ftable=(intptr_t)SDL_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword);
    emit_writeword(target?rth:rt,((intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword)+4);
  }
  else if(type==STOREDR_STUB){
    ftable=(intptr_t)SDR_new;
    emit_writeword(rt,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword);
    emit_writeword(target?rth:rt,((intptr_t)&g_dev.r4300.new_dynarec_hot_state.wdword)+4);
  }

  int cc=get_reg(i_regs->regmap,CCREG);
  if(cc>=0) {
    emit_storereg(CCREG,cc);
  }

  save_regs(reglist);
  int ds=i_regs!=&regs[i];

#if NEW_DYNAREC == NEW_DYNAREC_X86
  emit_pushimm(CLOCK_DIVIDER*(adj+1));
  emit_pushimm((start+(i+1)*4)+ds);
  emit_call((intptr_t)ftable);
  emit_addimm(ESP,8,ESP);
#else
  emit_movimm((start+(i+1)*4)+ds,ARG1_REG);
  emit_movimm(CLOCK_DIVIDER*(adj+1),ARG2_REG);
  emit_call((intptr_t)ftable);
#endif

  restore_regs(reglist);

  if(((signed int)addr_const>=(signed int)0xC0000000)||((addr_const>>16)==0xa430)||((addr_const>>16)==0x8430)) {
    // Theoretically we can have a pagefault here, if the TLB has never
    // been enabled and the address is outside the range 80000000..BFFFFFFF
    // Write out the registers so the pagefault can be handled.  This is
    // a very rare case and likely represents a bug.
    emit_cmpmem_imm((intptr_t)&g_dev.r4300.new_dynarec_hot_state.pending_exception,0);
    intptr_t jaddr=(intptr_t)out;
    emit_jeq(0);

    if(!ds) load_all_consts(regs[i].regmap_entry,regs[i].was32,regs[i].wasdirty,regs[i].wasconst,i);
    wb_dirtys(i_regs->regmap_entry,i_regs->was32,i_regs->wasdirty);

    emit_jmp((intptr_t)&do_interrupt);
    set_jump_target(jaddr,(intptr_t)out);
  }

  if(cc>=0) {
    emit_loadreg(CCREG,cc);
  }
}

/**** Assemble ****/
static void cop0_assemble(int i,struct regstat *i_regs)
{
  u_int hr,reglist=0;
  for(hr=0;hr<HOST_REGS;hr++) {
    if(i_regs->regmap[hr]>=0) reglist|=1<<hr;
  }

  if(opcode2[i]==0) // MFC0
  {
    if(rt1[i]) {
      signed char t=get_reg(i_regs->regmap,rt1[i]);
      char copr=(source[i]>>11)&0x1f;
      if(t>=0) {
        reglist&=~(1<<t);

        int cc=get_reg(i_regs->regmap,CCREG);
        if(cc>=0) {
          emit_storereg(CCREG,cc);
        }

        save_regs(reglist);

        //Always update the count even if it's only necessary when (copr==CP0_COUNT_REG||copr==CP0_RANDOM_REG)
#if NEW_DYNAREC == NEW_DYNAREC_X86
        emit_pushimm(CLOCK_DIVIDER*ccadj[i]);
        emit_pushimm(copr);
        emit_call((intptr_t)MFC0_new);
        emit_addimm(ESP,8,ESP);
#else
        emit_movimm(copr,ARG1_REG);
        emit_movimm(CLOCK_DIVIDER*ccadj[i],ARG2_REG);
        emit_call((intptr_t)MFC0_new);
#endif

        restore_regs(reglist);
        emit_readword((uintptr_t)&g_dev.r4300.new_dynarec_hot_state.rt,t);
      }
    }
  }
  else if(opcode2[i]==4) // MTC0
  {
    signed char s=get_reg(i_regs->regmap,rs1[i]);
    char copr=(source[i]>>11)&0x1f;
    assert(s>=0);

    emit_writeword(s,(uintptr_t)&g_dev.r4300.new_dynarec_hot_state.rt);

    int cc=get_reg(i_regs->regmap,CCREG);
    if(cc>=0) {
      emit_storereg(CCREG,cc);
    }

    save_regs(reglist);

    // Always update the count even if it's only necessary when (copr==CP0_COUNT_REG||copr==CP0_COMPARE_REG||copr==CP0_STATUS_REG)
    // Always update the pcaddr even if it's only necessary when (copr==CP0_COUNT_REG||copr==CP0_STATUS_REG)
#if NEW_DYNAREC == NEW_DYNAREC_X86
    emit_pushimm(start+i*4);
    emit_pushimm(CLOCK_DIVIDER*(ccadj[i]+(copr==CP0_STATUS_REG)));
    emit_pushimm(copr);
    emit_call((intptr_t)MTC0_new);
    emit_addimm(ESP,12,ESP);
#else
    emit_movimm(copr,ARG1_REG);
    emit_movimm(CLOCK_DIVIDER*(ccadj[i]+(copr==CP0_STATUS_REG)),ARG2_REG);
    emit_movimm(start+i*4,ARG3_REG);
    emit_call((intptr_t)MTC0_new);
#endif

    restore_regs(reglist);

    if(copr==CP0_COUNT_REG||copr==CP0_STATUS_REG) {
      assert(!is_delayslot);
      emit_cmpmem_imm((intptr_t)&g_dev.r4300.new_dynarec_hot_state.pending_exception,0);
      intptr_t jaddr=(intptr_t)out;
      emit_jeq(0);
      load_all_consts(regs[i].regmap_entry,regs[i].was32,regs[i].wasdirty,regs[i].wasconst,i);
      wb_dirtys(i_regs->regmap_entry,i_regs->was32,i_regs->wasdirty);
      emit_jmp((intptr_t)&do_interrupt);
      set_jump_target(jaddr,(intptr_t)out);
    }

    if(copr==CP0_COUNT_REG||copr==CP0_COMPARE_REG||copr==CP0_STATUS_REG){
      if(cc>=0) {
        emit_loadreg(CCREG,cc);
      }
    }

    cop1_usable=0;
  }
  else
  {
    assert(opcode2[i]==0x10);
    if((source[i]&0x3f)==0x08) // TLBP
    {
      save_regs(reglist);
      emit_call((intptr_t)cached_interp_TLBP);
      restore_regs(reglist);
    }
    else if((source[i]&0x3f)==0x01) // TLBR
    {
      save_regs(reglist);
      emit_call((intptr_t)cached_interp_TLBR);
      restore_regs(reglist);
    }
    else if((source[i]&0x3f)==0x02) {  // TLBWI
      assert(!is_delayslot);

      int cc=get_reg(i_regs->regmap,CCREG);
      if(cc>=0) {
        emit_storereg(CCREG,cc);
      }

      save_regs(reglist);

#if NEW_DYNAREC == NEW_DYNAREC_X86
      emit_pushimm(CLOCK_DIVIDER*ccadj[i]);
      emit_pushimm(start+i*4);
      emit_call((intptr_t)TLBWI_new);
      emit_addimm(ESP,8,ESP);
#else
      emit_movimm(start+i*4,ARG1_REG);
      emit_movimm(CLOCK_DIVIDER*ccadj[i],ARG2_REG);
      emit_call((intptr_t)TLBWI_new);
#endif

      restore_regs(reglist);
    }
    else if((source[i]&0x3f)==0x06) { // TLBWR
      assert(!is_delayslot);

      int cc=get_reg(i_regs->regmap,CCREG);
      if(cc>=0) {
        emit_storereg(CCREG,cc);
      }

      save_regs(reglist);

#if NEW_DYNAREC == NEW_DYNAREC_X86
      emit_pushimm(CLOCK_DIVIDER*ccadj[i]);
      emit_pushimm(start+i*4);
      emit_call((intptr_t)TLBWR_new);
      emit_addimm(ESP,8,ESP);
#else
      emit_movimm(start+i*4,ARG1_REG);
      emit_movimm(CLOCK_DIVIDER*ccadj[i],ARG2_REG);
      emit_call((intptr_t)TLBWR_new);
#endif

      restore_regs(reglist);
    }
    else if((source[i]&0x3f)==0x18) // ERET
    {
      assert(!is_delayslot);
      int count=ccadj[i];
      if(i_regs->regmap[HOST_CCREG]!=CCREG) emit_loadreg(CCREG,HOST_CCREG);
      emit_addimm(HOST_CCREG,CLOCK_DIVIDER*count,HOST_CCREG);
      emit_jmp((intptr_t)jump_eret);
    }
  }
}

static void cop1_assemble(int i,struct regstat *i_regs)
{
  // Check cop1 unusable
  if(!cop1_usable) {
    signed char rs=get_reg(i_regs->regmap,CSREG);
    assert(rs>=0);
    emit_testimm(rs,CP0_STATUS_CU1);
    intptr_t jaddr=(intptr_t)out;
    emit_jeq(0);
    add_stub(FP_STUB,jaddr,(intptr_t)out,i,rs,(intptr_t)i_regs,is_delayslot,0);
    cop1_usable=1;
  }
  if (opcode2[i]==0) { // MFC1
    signed char tl=get_reg(i_regs->regmap,rt1[i]);
    if(tl>=0) {
      emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[(source[i]>>11)&0x1f],tl);
      emit_readword_indexed(0,tl,tl);
    }
  }
  else if (opcode2[i]==1) { // DMFC1
    signed char tl=get_reg(i_regs->regmap,rt1[i]);
    signed char th=get_reg(i_regs->regmap,rt1[i]|64);
    if(tl>=0) {
      emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[(source[i]>>11)&0x1f],tl);
      if(th>=0) emit_readword_indexed(4,tl,th);
      emit_readword_indexed(0,tl,tl);
    }
  }
  else if (opcode2[i]==4) { // MTC1
    signed char sl=get_reg(i_regs->regmap,rs1[i]);
    signed char temp=get_reg(i_regs->regmap,-1);
    emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[(source[i]>>11)&0x1f],temp);
    emit_writeword_indexed(sl,0,temp);
  }
  else if (opcode2[i]==5) { // DMTC1
    signed char sl=get_reg(i_regs->regmap,rs1[i]);
    signed char sh=rs1[i]>0?get_reg(i_regs->regmap,rs1[i]|64):sl;
    signed char temp=get_reg(i_regs->regmap,-1);
    emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[(source[i]>>11)&0x1f],temp);
    emit_writeword_indexed(sh,4,temp);
    emit_writeword_indexed(sl,0,temp);
  }
  else if (opcode2[i]==2) // CFC1
  {
    signed char tl=get_reg(i_regs->regmap,rt1[i]);
    signed char fs=get_reg(i_regs->regmap,FSREG);
    if(tl>=0) {
      u_int copr=(source[i]>>11)&0x1f;
      if(copr==0) emit_readword((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_fcr0,tl);
      if(copr==31)
      {
          if(fs>=0) emit_mov(fs,tl);
          else emit_loadreg(FSREG,tl);
      }
    }
  }
  else if (opcode2[i]==6) // CTC1
  {
    signed char sl=get_reg(i_regs->regmap,rs1[i]);
    signed char fs=get_reg(i_regs->regmap,FSREG);
    signed char temp=get_reg(i_regs->regmap,-1);
    u_int copr=(source[i]>>11)&0x1f;
    assert(sl>=0);
    if(copr==31)
    {
      if(fs>=0) emit_mov(sl,fs);
      else emit_storereg(FSREG,sl);
      set_rounding_mode(sl,temp);
    }
  }
}

static void alu_assemble(int i,struct regstat *i_regs)
{
  if(opcode2[i]>=0x20&&opcode2[i]<=0x23) { // ADD/ADDU/SUB/SUBU
    if(rt1[i]) {
      signed char s1,s2,t;
      t=get_reg(i_regs->regmap,rt1[i]);
      if(t>=0) {
        s1=get_reg(i_regs->regmap,rs1[i]);
        s2=get_reg(i_regs->regmap,rs2[i]);
        if(rs1[i]&&rs2[i]) {
          assert(s1>=0);
          assert(s2>=0);
          if(opcode2[i]&2) emit_sub(s1,s2,t);
          else emit_add(s1,s2,t);
        }
        else if(rs1[i]) {
          if(s1>=0) emit_mov(s1,t);
          else emit_loadreg(rs1[i],t);
        }
        else if(rs2[i]) {
          if(s2>=0) {
            if(opcode2[i]&2) emit_neg(s2,t);
            else emit_mov(s2,t);
          }
          else {
            emit_loadreg(rs2[i],t);
            if(opcode2[i]&2) emit_neg(t,t);
          }
        }
        else emit_zeroreg(t);
      }
    }
  }
  if(opcode2[i]>=0x2c&&opcode2[i]<=0x2f) { // DADD/DADDU/DSUB/DSUBU
    if(rt1[i]) {
      signed char s1l,s2l,s1h,s2h,tl,th;
      tl=get_reg(i_regs->regmap,rt1[i]);
      th=get_reg(i_regs->regmap,rt1[i]|64);
      if(tl>=0) {
        s1l=get_reg(i_regs->regmap,rs1[i]);
        s2l=get_reg(i_regs->regmap,rs2[i]);
        s1h=get_reg(i_regs->regmap,rs1[i]|64);
        s2h=get_reg(i_regs->regmap,rs2[i]|64);
        if(rs1[i]&&rs2[i]) {
          assert(s1l>=0);
          assert(s2l>=0);
          if(th>=0) {
            #ifdef INVERTED_CARRY
            if(opcode2[i]&2) emit_sub64_32(s1l,s1h,s2l,s2h,tl,th);
            #else
            if(opcode2[i]&2) {
              emit_subs(s1l,s2l,tl);
              emit_sbc(s1h,s2h,th);
            }
            #endif
            else {
              emit_adds(s1l,s2l,tl);
              emit_adc(s1h,s2h,th);
            }
          }
          else {
            if(opcode2[i]&2) emit_subs(s1l,s2l,tl);
            else emit_adds(s1l,s2l,tl);
          }
        }
        else if(rs1[i]) {
          if(s1l>=0) emit_mov(s1l,tl);
          else emit_loadreg(rs1[i],tl);
          if(th>=0) {
            if(s1h>=0) emit_mov(s1h,th);
            else emit_loadreg(rs1[i]|64,th);
          }
        }
        else if(rs2[i]) {
          if(s2l>=0) {
            if(opcode2[i]&2) emit_negs(s2l,tl);
            else emit_mov(s2l,tl);
          }
          else {
            emit_loadreg(rs2[i],tl);
            if(opcode2[i]&2) emit_negs(tl,tl);
          }
          if(th>=0) {
            #ifdef INVERTED_CARRY
            if(s2h>=0) emit_mov(s2h,th);
            else emit_loadreg(rs2[i]|64,th);
            if(opcode2[i]&2) {
              emit_adcimm(-1,th); // x86 has inverted carry flag
              emit_not(th,th);
            }
            #else
            if(opcode2[i]&2) {
              if(s2h>=0) emit_rscimm(s2h,0,th);
              else {
                emit_loadreg(rs2[i]|64,th);
                emit_rscimm(th,0,th);
              }
            }else{
              if(s2h>=0) emit_mov(s2h,th);
              else emit_loadreg(rs2[i]|64,th);
            }
            #endif
          }
        }
        else {
          emit_zeroreg(tl);
          if(th>=0) emit_zeroreg(th);
        }
      }
    }
  }
  if(opcode2[i]==0x2a||opcode2[i]==0x2b) { // SLT/SLTU
    if(rt1[i]) {
      signed char s1l,s1h,s2l,s2h,t;
      if(!((i_regs->was32>>rs1[i])&(i_regs->was32>>rs2[i])&1))
      {
        t=get_reg(i_regs->regmap,rt1[i]);
        //assert(t>=0);
        if(t>=0) {
          s1l=get_reg(i_regs->regmap,rs1[i]);
          s1h=get_reg(i_regs->regmap,rs1[i]|64);
          s2l=get_reg(i_regs->regmap,rs2[i]);
          s2h=get_reg(i_regs->regmap,rs2[i]|64);
          if(rs2[i]==0) // rx<r0
          {
            assert(s1h>=0);
            if(opcode2[i]==0x2a) // SLT
              emit_shrimm(s1h,31,t);
            else // SLTU (unsigned can not be less than zero)
              emit_zeroreg(t);
          }
          else if(rs1[i]==0) // r0<rx
          {
            assert(s2h>=0);
            if(opcode2[i]==0x2a) // SLT
              emit_set_gz64_32(s2h,s2l,t);
            else // SLTU (set if not zero)
              emit_set_nz64_32(s2h,s2l,t);
          }
          else {
            assert(s1l>=0);assert(s1h>=0);
            assert(s2l>=0);assert(s2h>=0);
            if(opcode2[i]==0x2a) // SLT
              emit_set_if_less64_32(s1h,s1l,s2h,s2l,t);
            else // SLTU
              emit_set_if_carry64_32(s1h,s1l,s2h,s2l,t);
          }
        }
      } else {
        t=get_reg(i_regs->regmap,rt1[i]);
        //assert(t>=0);
        if(t>=0) {
          s1l=get_reg(i_regs->regmap,rs1[i]);
          s2l=get_reg(i_regs->regmap,rs2[i]);
          if(rs2[i]==0) // rx<r0
          {
            assert(s1l>=0);
            if(opcode2[i]==0x2a) // SLT
              emit_shrimm(s1l,31,t);
            else // SLTU (unsigned can not be less than zero)
              emit_zeroreg(t);
          }
          else if(rs1[i]==0) // r0<rx
          {
            assert(s2l>=0);
            if(opcode2[i]==0x2a) // SLT
              emit_set_gz32(s2l,t);
            else // SLTU (set if not zero)
              emit_set_nz32(s2l,t);
          }
          else{
            assert(s1l>=0);assert(s2l>=0);
            if(opcode2[i]==0x2a) // SLT
              emit_set_if_less32(s1l,s2l,t);
            else // SLTU
              emit_set_if_carry32(s1l,s2l,t);
          }
        }
      }
    }
  }
  if(opcode2[i]>=0x24&&opcode2[i]<=0x27) { // AND/OR/XOR/NOR
    if(rt1[i]) {
      signed char s1l,s1h,s2l,s2h,th,tl;
      tl=get_reg(i_regs->regmap,rt1[i]);
      th=get_reg(i_regs->regmap,rt1[i]|64);
      if(!((i_regs->was32>>rs1[i])&(i_regs->was32>>rs2[i])&1)&&th>=0)
      {
        assert(tl>=0);
        if(tl>=0) {
          s1l=get_reg(i_regs->regmap,rs1[i]);
          s1h=get_reg(i_regs->regmap,rs1[i]|64);
          s2l=get_reg(i_regs->regmap,rs2[i]);
          s2h=get_reg(i_regs->regmap,rs2[i]|64);
          if(rs1[i]&&rs2[i]) {
            assert(s1l>=0);assert(s1h>=0);
            assert(s2l>=0);assert(s2h>=0);
            if(opcode2[i]==0x24) { // AND
              emit_and(s1l,s2l,tl);
              emit_and(s1h,s2h,th);
            } else
            if(opcode2[i]==0x25) { // OR
              emit_or(s1l,s2l,tl);
              emit_or(s1h,s2h,th);
            } else
            if(opcode2[i]==0x26) { // XOR
              emit_xor(s1l,s2l,tl);
              emit_xor(s1h,s2h,th);
            } else
            if(opcode2[i]==0x27) { // NOR
              emit_or(s1l,s2l,tl);
              emit_or(s1h,s2h,th);
              emit_not(tl,tl);
              emit_not(th,th);
            }
          }
          else
          {
            if(opcode2[i]==0x24) { // AND
              emit_zeroreg(tl);
              emit_zeroreg(th);
            } else
            if(opcode2[i]==0x25||opcode2[i]==0x26) { // OR/XOR
              if(rs1[i]){
                if(s1l>=0) emit_mov(s1l,tl);
                else emit_loadreg(rs1[i],tl);
                if(s1h>=0) emit_mov(s1h,th);
                else emit_loadreg(rs1[i]|64,th);
              }
              else
              if(rs2[i]){
                if(s2l>=0) emit_mov(s2l,tl);
                else emit_loadreg(rs2[i],tl);
                if(s2h>=0) emit_mov(s2h,th);
                else emit_loadreg(rs2[i]|64,th);
              }
              else{
                emit_zeroreg(tl);
                emit_zeroreg(th);
              }
            } else
            if(opcode2[i]==0x27) { // NOR
              if(rs1[i]){
                if(s1l>=0) emit_not(s1l,tl);
                else{
                  emit_loadreg(rs1[i],tl);
                  emit_not(tl,tl);
                }
                if(s1h>=0) emit_not(s1h,th);
                else{
                  emit_loadreg(rs1[i]|64,th);
                  emit_not(th,th);
                }
              }
              else
              if(rs2[i]){
                if(s2l>=0) emit_not(s2l,tl);
                else{
                  emit_loadreg(rs2[i],tl);
                  emit_not(tl,tl);
                }
                if(s2h>=0) emit_not(s2h,th);
                else{
                  emit_loadreg(rs2[i]|64,th);
                  emit_not(th,th);
                }
              }
              else {
                emit_movimm(-1,tl);
                emit_movimm(-1,th);
              }
            }
          }
        }
      }
      else
      {
        // 32 bit
        if(tl>=0) {
          s1l=get_reg(i_regs->regmap,rs1[i]);
          s2l=get_reg(i_regs->regmap,rs2[i]);
          if(rs1[i]&&rs2[i]) {
            assert(s1l>=0);
            assert(s2l>=0);
            if(opcode2[i]==0x24) { // AND
              emit_and(s1l,s2l,tl);
            } else
            if(opcode2[i]==0x25) { // OR
              emit_or(s1l,s2l,tl);
            } else
            if(opcode2[i]==0x26) { // XOR
              emit_xor(s1l,s2l,tl);
            } else
            if(opcode2[i]==0x27) { // NOR
              emit_or(s1l,s2l,tl);
              emit_not(tl,tl);
            }
          }
          else
          {
            if(opcode2[i]==0x24) { // AND
              emit_zeroreg(tl);
            } else
            if(opcode2[i]==0x25||opcode2[i]==0x26) { // OR/XOR
              if(rs1[i]){
                if(s1l>=0) emit_mov(s1l,tl);
                else emit_loadreg(rs1[i],tl); // CHECK: regmap_entry?
              }
              else
              if(rs2[i]){
                if(s2l>=0) emit_mov(s2l,tl);
                else emit_loadreg(rs2[i],tl); // CHECK: regmap_entry?
              }
              else emit_zeroreg(tl);
            } else
            if(opcode2[i]==0x27) { // NOR
              if(rs1[i]){
                if(s1l>=0) emit_not(s1l,tl);
                else {
                  emit_loadreg(rs1[i],tl);
                  emit_not(tl,tl);
                }
              }
              else
              if(rs2[i]){
                if(s2l>=0) emit_not(s2l,tl);
                else {
                  emit_loadreg(rs2[i],tl);
                  emit_not(tl,tl);
                }
              }
              else emit_movimm(-1,tl);
            }
          }
        }
      }
    }
  }
}

static void imm16_assemble(int i,struct regstat *i_regs)
{
  if (opcode[i]==0x0f) { // LUI
    if(rt1[i]) {
      signed char t;
      t=get_reg(i_regs->regmap,rt1[i]);
      //assert(t>=0);
      if(t>=0) {
        if(!((i_regs->isconst>>t)&1))
          emit_movimm(imm[i]<<16,t);
      }
    }
  }
  if(opcode[i]==0x08||opcode[i]==0x09) { // ADDI/ADDIU
    if(rt1[i]) {
      signed char s,t;
      t=get_reg(i_regs->regmap,rt1[i]);
      s=get_reg(i_regs->regmap,rs1[i]);
      if(rs1[i]) {
        //assert(t>=0);
        //assert(s>=0);
        if(t>=0) {
          if(!((i_regs->isconst>>t)&1)) {
            if(s<0) {
              if(i_regs->regmap_entry[t]!=rs1[i]) emit_loadreg(rs1[i],t);
              emit_addimm(t,imm[i],t);
            }else{
              if(!((i_regs->wasconst>>s)&1))
                emit_addimm(s,imm[i],t);
              else
                emit_movimm(constmap[i][s]+imm[i],t);
            }
          }
        }
      } else {
        if(t>=0) {
          if(!((i_regs->isconst>>t)&1))
            emit_movimm(imm[i],t);
        }
      }
    }
  }
  if(opcode[i]==0x18||opcode[i]==0x19) { // DADDI/DADDIU
    if(rt1[i]) {
      signed char sh,sl,th,tl;
      th=get_reg(i_regs->regmap,rt1[i]|64);
      tl=get_reg(i_regs->regmap,rt1[i]);
      sh=get_reg(i_regs->regmap,rs1[i]|64);
      sl=get_reg(i_regs->regmap,rs1[i]);
      if(tl>=0) {
        if(rs1[i]) {
          assert(sh>=0);
          assert(sl>=0);
          if(th>=0) {
            emit_addimm64_32(sh,sl,imm[i],th,tl);
          }
          else {
            emit_addimm(sl,imm[i],tl);
          }
        } else {
          emit_movimm(imm[i],tl);
          if(th>=0) emit_movimm(((signed int)imm[i])>>31,th);
        }
      }
    }
  }
  else if(opcode[i]==0x0a||opcode[i]==0x0b) { // SLTI/SLTIU
    if(rt1[i]) {
      //assert(rs1[i]!=0); // r0 might be valid, but it's probably a bug
      signed char sh,sl,t;
      t=get_reg(i_regs->regmap,rt1[i]);
      sh=get_reg(i_regs->regmap,rs1[i]|64);
      sl=get_reg(i_regs->regmap,rs1[i]);
      //assert(t>=0);
      if(t>=0) {
        if(rs1[i]>0) {
          if(sh<0) assert((i_regs->was32>>rs1[i])&1);
          if(sh<0||((i_regs->was32>>rs1[i])&1)) {
            if(opcode[i]==0x0a) { // SLTI
              if(sl<0) {
                if(i_regs->regmap_entry[t]!=rs1[i]) emit_loadreg(rs1[i],t);
                emit_slti32(t,imm[i],t);
              }else{
                emit_slti32(sl,imm[i],t);
              }
            }
            else { // SLTIU
              if(sl<0) {
                if(i_regs->regmap_entry[t]!=rs1[i]) emit_loadreg(rs1[i],t);
                emit_sltiu32(t,imm[i],t);
              }else{
                emit_sltiu32(sl,imm[i],t);
              }
            }
          }else{ // 64-bit
            assert(sl>=0);
            if(opcode[i]==0x0a) // SLTI
              emit_slti64_32(sh,sl,imm[i],t);
            else // SLTIU
              emit_sltiu64_32(sh,sl,imm[i],t);
          }
        }else{
          // SLTI(U) with r0 is just stupid,
          // nonetheless examples can be found
          if(opcode[i]==0x0a) // SLTI
            if(0<imm[i]) emit_movimm(1,t);
            else emit_zeroreg(t);
          else // SLTIU
          {
            if(imm[i]) emit_movimm(1,t);
            else emit_zeroreg(t);
          }
        }
      }
    }
  }
  else if(opcode[i]>=0x0c&&opcode[i]<=0x0e) { // ANDI/ORI/XORI
    if(rt1[i]) {
      signed char sh,sl,th,tl;
      th=get_reg(i_regs->regmap,rt1[i]|64);
      tl=get_reg(i_regs->regmap,rt1[i]);
      sh=get_reg(i_regs->regmap,rs1[i]|64);
      sl=get_reg(i_regs->regmap,rs1[i]);
      if(tl>=0 && !((i_regs->isconst>>tl)&1)) {
        if(opcode[i]==0x0c) //ANDI
        {
          if(rs1[i]) {
            if(sl<0) {
              if(i_regs->regmap_entry[tl]!=rs1[i]) emit_loadreg(rs1[i],tl);
              emit_andimm(tl,imm[i],tl);
            }else{
              if(!((i_regs->wasconst>>sl)&1))
                emit_andimm(sl,imm[i],tl);
              else
                emit_movimm(constmap[i][sl]&imm[i],tl);
            }
          }
          else
            emit_zeroreg(tl);
          if(th>=0) emit_zeroreg(th);
        }
        else
        {
          if(rs1[i]) {
            if(sl<0) {
              if(i_regs->regmap_entry[tl]!=rs1[i]) emit_loadreg(rs1[i],tl);
            }
            if(th>=0) {
              if(sh<0) {
                emit_loadreg(rs1[i]|64,th);
              }else{
                emit_mov(sh,th);
              }
            }
            if(opcode[i]==0x0d) { //ORI
              if(sl<0) {
                emit_orimm(tl,imm[i],tl);
              }else{
                if(!((i_regs->wasconst>>sl)&1))
                  emit_orimm(sl,imm[i],tl);
                else
                  emit_movimm((int)(constmap[i][sl]|imm[i]),tl);
              }
            }
            if(opcode[i]==0x0e) { //XORI
              if(sl<0) {
                emit_xorimm(tl,imm[i],tl);
              }else{
                if(!((i_regs->wasconst>>sl)&1))
                  emit_xorimm(sl,imm[i],tl);
                else
                  emit_movimm((int)(constmap[i][sl]^imm[i]),tl);
              }
            }
          }
          else {
            emit_movimm(imm[i],tl);
            if(th>=0) emit_zeroreg(th);
          }
        }
      }
    }
  }
}

static void shiftimm_assemble(int i,struct regstat *i_regs)
{
  if(opcode2[i]<=0x3) // SLL/SRL/SRA
  {
    if(rt1[i]) {
      signed char s,t;
      t=get_reg(i_regs->regmap,rt1[i]);
      s=get_reg(i_regs->regmap,rs1[i]);
      //assert(t>=0);
      if(t>=0){
        if(rs1[i]==0)
        {
          emit_zeroreg(t);
        }
        else
        {
          if(s<0&&i_regs->regmap_entry[t]!=rs1[i]) emit_loadreg(rs1[i],t);
          if(imm[i]) {
            if(opcode2[i]==0) // SLL
            {
              emit_shlimm(s<0?t:s,imm[i],t);
            }
            if(opcode2[i]==2) // SRL
            {
              emit_shrimm(s<0?t:s,imm[i],t);
            }
            if(opcode2[i]==3) // SRA
            {
              emit_sarimm(s<0?t:s,imm[i],t);
            }
          }else{
            // Shift by zero
            if(s>=0 && s!=t) emit_mov(s,t);
          }
        }
      }
      //emit_storereg(rt1[i],t); //DEBUG
    }
  }
  if(opcode2[i]>=0x38&&opcode2[i]<=0x3b) // DSLL/DSRL/DSRA
  {
    if(rt1[i]) {
      signed char sh,sl,th,tl;
      th=get_reg(i_regs->regmap,rt1[i]|64);
      tl=get_reg(i_regs->regmap,rt1[i]);
      sh=get_reg(i_regs->regmap,rs1[i]|64);
      sl=get_reg(i_regs->regmap,rs1[i]);
      if(tl>=0) {
        if(rs1[i]==0)
        {
          emit_zeroreg(tl);
          if(th>=0) emit_zeroreg(th);
        }
        else
        {
          assert(sl>=0);
          assert(sh>=0);
          if(imm[i]) {
            if(opcode2[i]==0x38) // DSLL
            {
              if(th>=0) emit_shldimm(sh,sl,imm[i],th);
              emit_shlimm(sl,imm[i],tl);
            }
            if(opcode2[i]==0x3a) // DSRL
            {
              emit_shrdimm(sl,sh,imm[i],tl);
              if(th>=0) emit_shrimm(sh,imm[i],th);
            }
            if(opcode2[i]==0x3b) // DSRA
            {
              emit_shrdimm(sl,sh,imm[i],tl);
              if(th>=0) emit_sarimm(sh,imm[i],th);
            }
          }else{
            // Shift by zero
            if(sl!=tl) emit_mov(sl,tl);
            if(th>=0&&sh!=th) emit_mov(sh,th);
          }
        }
      }
    }
  }
  if(opcode2[i]==0x3c) // DSLL32
  {
    if(rt1[i]) {
      signed char sl,tl,th;
      tl=get_reg(i_regs->regmap,rt1[i]);
      th=get_reg(i_regs->regmap,rt1[i]|64);
      sl=get_reg(i_regs->regmap,rs1[i]);
      if(th>=0||tl>=0){
        assert(tl>=0);
        assert(th>=0);
        assert(sl>=0);
        emit_mov(sl,th);
        emit_zeroreg(tl);
        if(imm[i]>32)
        {
          emit_shlimm(th,imm[i]&31,th);
        }
      }
    }
  }
  if(opcode2[i]==0x3e) // DSRL32
  {
    if(rt1[i]) {
      signed char sh,tl,th;
      tl=get_reg(i_regs->regmap,rt1[i]);
      th=get_reg(i_regs->regmap,rt1[i]|64);
      sh=get_reg(i_regs->regmap,rs1[i]|64);
      if(tl>=0){
        assert(sh>=0);
        emit_mov(sh,tl);
        if(th>=0) emit_zeroreg(th);
        if(imm[i]>32)
        {
          emit_shrimm(tl,imm[i]&31,tl);
        }
      }
    }
  }
  if(opcode2[i]==0x3f) // DSRA32
  {
    if(rt1[i]) {
      signed char sh,tl;
      tl=get_reg(i_regs->regmap,rt1[i]);
      sh=get_reg(i_regs->regmap,rs1[i]|64);
      if(tl>=0){
        assert(sh>=0);
        emit_mov(sh,tl);
        if(imm[i]>32)
        {
          emit_sarimm(tl,imm[i]&31,tl);
        }
      }
    }
  }
}

#ifndef shift_assemble
void shift_assemble(int i,struct regstat *i_regs)
{
  DebugMessage(M64MSG_ERROR, "Need shift_assemble for this architecture.");
  exit(1);
}
#endif

static void load_assemble(int i,struct regstat *i_regs)
{
  signed char s,th,tl,addr,map=-1,cache=-1;
  int offset,type=0,memtarget=0,c=0;
  intptr_t jaddr=0;
  u_int hr,reglist=0;
  int agr=AGEN1+(i&1);
  th=get_reg(i_regs->regmap,rt1[i]|64);
  tl=get_reg(i_regs->regmap,rt1[i]);
  s=get_reg(i_regs->regmap,rs1[i]);
  offset=imm[i];
  for(hr=0;hr<HOST_REGS;hr++) {
    if(i_regs->regmap[hr]>=0) reglist|=1<<hr;
  }
  if(i_regs->regmap[HOST_CCREG]==CCREG) reglist&=~(1<<HOST_CCREG);
  if(s>=0) {
    c=(i_regs->wasconst>>s)&1;
    memtarget=c&&((signed int)(constmap[i][s]+offset))<(signed int)0x80800000;
    if(c&&using_tlb&&((signed int)(constmap[i][s]+offset))>=(signed int)0xC0000000) memtarget=1;
  }

  int temp=get_reg(i_regs->regmap,agr);
  if(temp<0) temp=get_reg(i_regs->regmap,-1);
  assert(temp>=0);
  if(tl<0) tl=temp;
  if(offset||s<0||c) addr=temp;
  else addr=s;
  assert(tl>=0); // Even if the load is a NOP, we must check for pagefaults and I/O
  int dummy=(rt1[i]==0)||(tl!=get_reg(i_regs->regmap,rt1[i])); // ignore loads to r0 and unneeded reg

  switch(opcode[i]) {
    case 0x20: type=LOADB_STUB; break;
    case 0x21: type=LOADH_STUB; break;
    case 0x23: type=LOADW_STUB; break;
    case 0x24: type=LOADBU_STUB; break;
    case 0x25: type=LOADHU_STUB; break;
    case 0x27: type=LOADWU_STUB; break;
    case 0x37: type=LOADD_STUB; break;
  }

#ifndef INTERPRET_LOAD
  if(!using_tlb) {
    if(!c) {
//#define R29_HACK 1
      #ifdef R29_HACK
      // Strmnnrmn's speed hack
      if(rs1[i]!=29||start<0x80001000||start>=0x80800000)
      #endif
      {
        emit_cmpimm(addr,0x800000);
        jaddr=(intptr_t)out;
        #ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
        // Hint to branch predictor that the branch is unlikely to be taken
        if(rs1[i]>=28)
          emit_jno_unlikely(0);
        else
        #endif
        emit_jno(0);
      }
    }
    #ifdef RAM_OFFSET
    #ifndef NATIVE_64
    if(!c&&!dummy)
    #else
    if((!c||memtarget)&&!dummy)
    #endif
    {
      map=get_reg(i_regs->regmap,ROREG);
      if(map<0) emit_loadreg(ROREG,map=HOST_TEMPREG);
    }
    #endif
  }else{ // using tlb
    int x=0;
    if (opcode[i]==0x20||opcode[i]==0x24) x=3; // LB/LBU
    if (opcode[i]==0x21||opcode[i]==0x25) x=2; // LH/LHU
    map=get_reg(i_regs->regmap,TLREG);
    cache=get_reg(i_regs->regmap,MMREG);
    assert(map>=0);
    reglist&=~(1<<map);
    map=do_tlb_r(addr,temp,map,cache,x,c,constmap[i][s]+offset);
    do_tlb_r_branch(map,c,constmap[i][s]+offset,&jaddr);
  }

  if((!c||memtarget)&&!dummy) {
    if (opcode[i]==0x20) { // LB
      #ifdef HOST_IMM_ADDR32
      if(c)
        emit_movsbl_tlb((constmap[i][s]+offset)^3,map,tl);
      else
      #endif
      {
        int x=0;
        if(!c) emit_xorimm(addr,3,temp);
        else x=((constmap[i][s]+offset)^3)-(constmap[i][s]+offset);
        emit_movsbl_indexed_tlb(x,temp,map,tl);
      }
    }
    else if (opcode[i]==0x21) { // LH
      #ifdef HOST_IMM_ADDR32
      if(c)
        emit_movswl_tlb((constmap[i][s]+offset)^2,map,tl);
      else
      #endif
      {
        int x=0;
        if(!c) emit_xorimm(addr,2,temp);
        else x=((constmap[i][s]+offset)^2)-(constmap[i][s]+offset);
        emit_movswl_indexed_tlb(x,temp,map,tl);
      }
    }
    else if (opcode[i]==0x23) { // LW
      #ifdef HOST_IMM_ADDR32
      if(c)
        emit_readword_tlb(constmap[i][s]+offset,map,tl);
      else
      #endif
      emit_readword_indexed_tlb(0,addr,map,tl);
    }
    else if (opcode[i]==0x24) { // LBU
      #ifdef HOST_IMM_ADDR32
      if(c)
        emit_movzbl_tlb((constmap[i][s]+offset)^3,map,tl);
      else
      #endif
      {
        int x=0;
        if(!c) emit_xorimm(addr,3,temp);
        else x=((constmap[i][s]+offset)^3)-(constmap[i][s]+offset);
        emit_movzbl_indexed_tlb(x,temp,map,tl);
      }
    }
    else if (opcode[i]==0x25) { // LHU
      #ifdef HOST_IMM_ADDR32
      if(c)
        emit_movzwl_tlb((constmap[i][s]+offset)^2,map,tl);
      else
      #endif
      {
        int x=0;
        if(!c) emit_xorimm(addr,2,temp);
        else x=((constmap[i][s]+offset)^2)-(constmap[i][s]+offset);
        emit_movzwl_indexed_tlb(x,temp,map,tl);
      }
    }
    else if (opcode[i]==0x27) { // LWU
      assert(th>=0);
      #ifdef HOST_IMM_ADDR32
      if(c)
        emit_readword_tlb(constmap[i][s]+offset,map,tl);
      else
      #endif
      emit_readword_indexed_tlb(0,addr,map,tl);
      emit_zeroreg(th);
    }
    else if (opcode[i]==0x37) { // LD
      #ifdef HOST_IMM_ADDR32
      if(c)
        emit_readdword_tlb(constmap[i][s]+offset,map,th,tl);
      else
      #endif
      emit_readdword_indexed_tlb(0,addr,map,th,tl);
    }
  }
  if(jaddr) {
    add_stub(type,jaddr,(intptr_t)out,i,addr,(intptr_t)i_regs,ccadj[i],reglist);
  } else if(c&&!memtarget) {
    inline_readstub(type,i,constmap[i][s]+offset,addr,i_regs,rt1[i],ccadj[i],reglist);
  }
#else
  inline_readstub(type,i,c?(constmap[i][s]+offset):0,addr,i_regs,rt1[i],ccadj[i],reglist);
#endif
}

#ifndef loadlr_assemble
static void loadlr_assemble(int i,struct regstat *i_regs)
{
  DebugMessage(M64MSG_ERROR, "Need loadlr_assemble for this architecture.");
  exit(1);
}
#endif

static void store_assemble(int i,struct regstat *i_regs)
{
  signed char s,th,tl,real_addr,addr,temp,map=-1,cache=-1;
  int offset,type=0,memtarget=0,c=0;
  intptr_t jaddr=0;
  u_int hr,reglist=0;
  int agr=AGEN1+(i&1);
  th=get_reg(i_regs->regmap,rs2[i]|64);
  tl=get_reg(i_regs->regmap,rs2[i]);
  s=get_reg(i_regs->regmap,rs1[i]);
  temp=get_reg(i_regs->regmap,agr);
  if(temp<0) temp=get_reg(i_regs->regmap,-1);
  offset=imm[i];
  if(s>=0) {
    c=(i_regs->wasconst>>s)&1;
    memtarget=c&&((signed int)(constmap[i][s]+offset))<(signed int)0x80800000;
    if(c&&using_tlb&&((signed int)(constmap[i][s]+offset))>=(signed int)0xC0000000) memtarget=1;
  }
  assert(tl>=0);
  assert(temp>=0);
  for(hr=0;hr<HOST_REGS;hr++) {
    if(i_regs->regmap[hr]>=0) reglist|=1<<hr;
  }
  if(i_regs->regmap[HOST_CCREG]==CCREG) reglist&=~(1<<HOST_CCREG);
  if(offset||s<0||c) addr=temp;
  else addr=s;
  real_addr=addr;

  switch(opcode[i]) {
    case 0x28: type=STOREB_STUB; break;
    case 0x29: type=STOREH_STUB; break;
    case 0x2B: type=STOREW_STUB; break;
    case 0x3F: type=STORED_STUB; break;
  }

#ifndef INTERPRET_STORE
  if(!using_tlb) {
    if(!c) {
      #ifdef R29_HACK
      // Strmnnrmn's speed hack
      memtarget=1;
      if(rs1[i]!=29||start<0x80001000||start>=0x80800000)
      #endif
      emit_cmpimm(addr,0x800000);
      #ifdef R29_HACK
      if(rs1[i]!=29||start<0x80001000||start>=0x80800000)
      #endif
      {
        jaddr=(intptr_t)out;
        #ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
        // Hint to branch predictor that the branch is unlikely to be taken
        if(rs1[i]>=28)
          emit_jno_unlikely(0);
        else
        #endif
        emit_jno(0);
      }
      #ifdef DESTRUCTIVE_SHIFT
      if(s==addr) emit_mov(s,temp);
      #endif
    }
    #ifdef RAM_OFFSET
    if(!c||memtarget) {
      map=get_reg(i_regs->regmap,ROREG);
      if(map<0) emit_loadreg(ROREG,map=HOST_TEMPREG);
    }
    #endif
  }else{ // using tlb
    int x=0;
    if (opcode[i]==0x28) x=3; // SB
    if (opcode[i]==0x29) x=2; // SH
    map=get_reg(i_regs->regmap,TLREG);
    cache=get_reg(i_regs->regmap,MMREG);
    assert(map>=0);
    reglist&=~(1<<map);
    map=do_tlb_w(addr,temp,map,cache,x,c,constmap[i][s]+offset);
    do_tlb_w_branch(map,c,constmap[i][s]+offset,&jaddr);
  }

  if(!c||memtarget) {
    if (opcode[i]==0x28) { // SB
      int x=0;
      if(!c) emit_xorimm(addr,3,temp);
      else x=((constmap[i][s]+offset)^3)-(constmap[i][s]+offset);
      emit_writebyte_indexed_tlb(tl,x,temp,map);
    }
    else if (opcode[i]==0x29) { // SH
      int x=0;
      if(!c) emit_xorimm(addr,2,temp);
      else x=((constmap[i][s]+offset)^2)-(constmap[i][s]+offset);
      emit_writehword_indexed_tlb(tl,x,temp,map);
    }
    else if (opcode[i]==0x2B) { // SW
      emit_writeword_indexed_tlb(tl,0,addr,map);
    }
    else if (opcode[i]==0x3F) { // SD
      if(rs2[i]) {
        assert(th>=0);
        emit_writedword_indexed_tlb(th,tl,0,addr,map);
      }else{
        // Store zero
        emit_writedword_indexed_tlb(tl,tl,0,addr,map);
      }
    }

    if(!using_tlb) {
      #ifdef DESTRUCTIVE_SHIFT
      // The x86 shift operation is 'destructive'; it overwrites the
      // source register, so we need to make a copy first and use that.
      addr=temp;
      #endif
      #if defined(HOST_IMM8) || defined(NEED_INVC_PTR)
      int ir=get_reg(i_regs->regmap,INVCP);
      assert(ir>=0);
      emit_cmpmem_indexedsr12_reg(ir,addr,1);
      #else
      emit_cmpmem_indexedsr12_imm((intptr_t)g_dev.r4300.cached_interp.invalid_code,addr,1);
      #endif
      #if defined(HAVE_CONDITIONAL_CALL) && !defined(DESTRUCTIVE_SHIFT)
      emit_callne(invalidate_addr_reg[addr]);
      #else
      intptr_t jaddr2=(intptr_t)out;
      emit_jne(0);
      add_stub(INVCODE_STUB,jaddr2,(intptr_t)out,reglist|(1<<HOST_CCREG),addr,0,0,0);
      #endif
    }
  }
  if(jaddr) {
    add_stub(type,jaddr,(intptr_t)out,i,real_addr,(intptr_t)i_regs,ccadj[i],reglist);
  } else if(c&&!memtarget) {
    inline_writestub(type,i,constmap[i][s]+offset,real_addr,i_regs,rs2[i],ccadj[i],reglist);
  }
#else
  inline_writestub(type,i,c?(constmap[i][s]+offset):0,real_addr,i_regs,rs2[i],ccadj[i],reglist);
#endif
}

static void storelr_assemble(int i,struct regstat *i_regs)
{
  signed char s,th,tl,real_addr,addr,temp,temp2,map=-1;
  int offset,type=0,memtarget=0,c=0;
  intptr_t jaddr=0;
  u_int hr,reglist=0;
  int agr=AGEN1+(i&1);
  th=get_reg(i_regs->regmap,rs2[i]|64);
  tl=get_reg(i_regs->regmap,rs2[i]);
  s=get_reg(i_regs->regmap,rs1[i]);
  temp=get_reg(i_regs->regmap,agr);
  if(temp<0) temp=get_reg(i_regs->regmap,-1);
  offset=imm[i];
  if(s>=0) {
    c=(i_regs->isconst>>s)&1;
    memtarget=c&&((signed int)(constmap[i][s]+offset))<(signed int)0x80800000;
    if(c&&using_tlb&&((signed int)(constmap[i][s]+offset))>=(signed int)0xC0000000) memtarget=1;
  }
  assert(tl>=0);
  assert(temp>=0);
  for(hr=0;hr<HOST_REGS;hr++) {
    if(i_regs->regmap[hr]>=0) reglist|=1<<hr;
  }
  if(offset||s<0||c) addr=temp;
  else addr=s;
  real_addr=addr;

  if (opcode[i]==0x2C||opcode[i]==0x2D) { // SDL/SDR
    temp2=get_reg(i_regs->regmap,FTEMP);
    if(!rs2[i]) temp2=th=tl;
  }

  switch(opcode[i]) {
    case 0x2A: type=STOREWL_STUB; break;
    case 0x2E: type=STOREWR_STUB; break;
    case 0x2C: type=STOREDL_STUB; break;
    case 0x2D: type=STOREDR_STUB; break;
  }

#ifndef INTERPRET_STORELR
  if(!using_tlb) {
    if(!c) {
      emit_cmpimm(addr,0x800000);
      jaddr=(intptr_t)out;
      emit_jno(0);
      #ifdef DESTRUCTIVE_SHIFT
      if(s==addr) emit_mov(s,temp);
      #endif
    }
    #ifdef RAM_OFFSET
    if(!c||memtarget) {
      map=get_reg(i_regs->regmap,ROREG);
      if(map<0) emit_loadreg(ROREG,map=HOST_TEMPREG);
    }
    #endif
  }else{ // using tlb
    map=get_reg(i_regs->regmap,TLREG);
    int cache=get_reg(i_regs->regmap,MMREG);
    assert(map>=0);
    reglist&=~(1<<map);
    map=do_tlb_w(addr,temp,map,cache,0,c,constmap[i][s]+offset);
    do_tlb_w_branch(map,c,constmap[i][s]+offset,&jaddr);
  }

  if(!c||memtarget)
  {
    intptr_t case1,case2,case3;
    intptr_t done0,done1,done2;

#if NEW_DYNAREC >= NEW_DYNAREC_ARM
    assert(map>=0);
    emit_addsl2(addr,map,map);
    addr=map; map=-1;
#endif
    if(!c) {
      emit_testimm(real_addr,2);
      case2=(intptr_t)out;
      emit_jne(0);
      emit_testimm(real_addr,1);
      case1=(intptr_t)out;
      emit_jne(0);
    }
    if(!c||((constmap[i][s]+offset)&3)==0) {
      if (opcode[i]==0x2A) { // SWL
        emit_writeword_indexed_tlb(tl,0,addr,map);
      }
      else if (opcode[i]==0x2E) { // SWR
        emit_writebyte_indexed_tlb(tl,3,addr,map);
      }
      else if (opcode[i]==0x2C) { // SDL
        emit_writeword_indexed_tlb(th,0,addr,map);
        if(rs2[i]) emit_mov(tl,temp2);
      }
      else if (opcode[i]==0x2D) { // SDR
        emit_writebyte_indexed_tlb(tl,3,addr,map);
        if(rs2[i]) emit_shldimm(th,tl,24,temp2);
      }
    }
    if(!c) {
      done0=(intptr_t)out;
      emit_jmp(0);
      set_jump_target(case1,(intptr_t)out);
    }
    if(!c||((constmap[i][s]+offset)&3)==1) {
      if (opcode[i]==0x2A) { // SWL
        // Write 3 msb into three least significant bytes
        if(rs2[i]) emit_rorimm(tl,8,tl);
        emit_writehword_indexed_tlb(tl,-1,addr,map);
        if(rs2[i]) emit_rorimm(tl,16,tl);
        emit_writebyte_indexed_tlb(tl,1,addr,map);
        if(rs2[i]) emit_rorimm(tl,8,tl);
      }
      else if (opcode[i]==0x2E) { // SWR
        // Write two lsb into two most significant bytes
        emit_writehword_indexed_tlb(tl,1,addr,map);
      }
      else if (opcode[i]==0x2C) { // SDL
        if(rs2[i]) emit_shrdimm(tl,th,8,temp2);
        // Write 3 msb into three least significant bytes
        if(rs2[i]) emit_rorimm(th,8,th);
        emit_writehword_indexed_tlb(th,-1,addr,map);
        if(rs2[i]) emit_rorimm(th,16,th);
        emit_writebyte_indexed_tlb(th,1,addr,map);
        if(rs2[i]) emit_rorimm(th,8,th);
      }
      else if (opcode[i]==0x2D) { // SDR
        if(rs2[i]) emit_shldimm(th,tl,16,temp2);
        // Write two lsb into two most significant bytes
        emit_writehword_indexed_tlb(tl,1,addr,map);
      }
    }

    if(!c) {
      done1=(intptr_t)out;
      emit_jmp(0);
      set_jump_target(case2,(intptr_t)out);
      emit_testimm(real_addr,1);
      case3=(intptr_t)out;
      emit_jne(0);
    }

    if(!c||((constmap[i][s]+offset)&3)==2) {
      if (opcode[i]==0x2A) { // SWL
        // Write two msb into two least significant bytes
        if(rs2[i]) emit_rorimm(tl,16,tl);
        emit_writehword_indexed_tlb(tl,-2,addr,map);
        if(rs2[i]) emit_rorimm(tl,16,tl);
      }
      else if (opcode[i]==0x2E) { // SWR
        // Write 3 lsb into three most significant bytes
        emit_writebyte_indexed_tlb(tl,-1,addr,map);
        if(rs2[i]) emit_rorimm(tl,8,tl);
        emit_writehword_indexed_tlb(tl,0,addr,map);
        if(rs2[i]) emit_rorimm(tl,24,tl);
      }
      else if (opcode[i]==0x2C) { // SDL
        if(rs2[i]) emit_shrdimm(tl,th,16,temp2);
        // Write two msb into two least significant bytes
        if(rs2[i]) emit_rorimm(th,16,th);
        emit_writehword_indexed_tlb(th,-2,addr,map);
        if(rs2[i]) emit_rorimm(th,16,th);
      }
      else if (opcode[i]==0x2D) { // SDR
        if(rs2[i]) emit_shldimm(th,tl,8,temp2);
        // Write 3 lsb into three most significant bytes
        emit_writebyte_indexed_tlb(tl,-1,addr,map);
        if(rs2[i]) emit_rorimm(tl,8,tl);
        emit_writehword_indexed_tlb(tl,0,addr,map);
        if(rs2[i]) emit_rorimm(tl,24,tl);
      }
    }

    if(!c) {
      done2=(intptr_t)out;
      emit_jmp(0);
      set_jump_target(case3,(intptr_t)out);
    }

    if(!c||((constmap[i][s]+offset)&3)==3) {
      if (opcode[i]==0x2A) { // SWL
        // Write msb into least significant byte
        if(rs2[i]) emit_rorimm(tl,24,tl);
        emit_writebyte_indexed_tlb(tl,-3,addr,map);
        if(rs2[i]) emit_rorimm(tl,8,tl);
      }
      else if (opcode[i]==0x2E) { // SWR
        // Write entire word
        emit_writeword_indexed_tlb(tl,-3,addr,map);
      }
      else if (opcode[i]==0x2C) { // SDL
        if(rs2[i]) emit_shrdimm(tl,th,24,temp2);
        // Write msb into least significant byte
        if(rs2[i]) emit_rorimm(th,24,th);
        emit_writebyte_indexed_tlb(th,-3,addr,map);
        if(rs2[i]) emit_rorimm(th,8,th);
      }
      else if (opcode[i]==0x2D) { // SDR
        if(rs2[i]) emit_mov(th,temp2);
        // Write entire word
        emit_writeword_indexed_tlb(tl,-3,addr,map);
      }
    }

    if(!c) {
      set_jump_target(done0,(intptr_t)out);
      set_jump_target(done1,(intptr_t)out);
      set_jump_target(done2,(intptr_t)out);
    }

    signed char temp3=(addr==s)?temp:addr;
    if (opcode[i]==0x2C) { // SDL
      if(!c) {
        emit_testimm(real_addr,4);
        done0=(intptr_t)out;
        emit_jne(0);
      }
      if(!c||((constmap[i][s]+offset)&4)==0) {
        #if NEW_DYNAREC == NEW_DYNAREC_ARM64
        emit_andimm64(addr,~3,temp3);
        #else
        emit_andimm(addr,~3,temp3);
        #endif
        emit_writeword_indexed_tlb(temp2,4,temp3,map);
      }
    }
    else if (opcode[i]==0x2D) { // SDR
      if(!c) {
        emit_testimm(real_addr,4);
        done0=(intptr_t)out;
        emit_jeq(0);
      }
      if(!c||((constmap[i][s]+offset)&4)!=0) {
        #if NEW_DYNAREC == NEW_DYNAREC_ARM64
        emit_andimm64(addr,~3,temp3);
        #else
        emit_andimm(addr,~3,temp3);
        #endif
        emit_writeword_indexed_tlb(temp2,-4,temp3,map);
      }
    }

    if(!c)
      set_jump_target(done0,(intptr_t)out);

    if(!using_tlb) {
      #if NEW_DYNAREC >= NEW_DYNAREC_ARM
      // Restore ram offset if allocated
      map=get_reg(i_regs->regmap,ROREG);
      if(map>=0) emit_loadreg(ROREG,map);
      #endif
      #ifdef DESTRUCTIVE_SHIFT
      // The x86 shift operation is 'destructive'; it overwrites the
      // source register, so we need to make a copy first and use that.
      addr=temp;
      #endif
      #if defined(HOST_IMM8) || defined(NEED_INVC_PTR)
      int ir=get_reg(i_regs->regmap,INVCP);
      assert(ir>=0);
      emit_cmpmem_indexedsr12_reg(ir,addr,1);
      #else
      emit_cmpmem_indexedsr12_imm((intptr_t)g_dev.r4300.cached_interp.invalid_code,addr,1);
      #endif
      #if defined(HAVE_CONDITIONAL_CALL) && !defined(DESTRUCTIVE_SHIFT)
      emit_callne(invalidate_addr_reg[addr]);
      #else
      intptr_t jaddr2=(intptr_t)out;
      emit_jne(0);
      add_stub(INVCODE_STUB,jaddr2,(intptr_t)out,reglist|(1<<HOST_CCREG),addr,0,0,0);
      #endif
    }
  }
  if(jaddr) {
    add_stub(type,jaddr,(intptr_t)out,i,real_addr,(intptr_t)i_regs,ccadj[i],reglist);
  } else if(c&&!memtarget) {
    inline_writestub(type,i,constmap[i][s]+offset,real_addr,i_regs,rs2[i],ccadj[i],reglist);
  }
#else
  inline_writestub(type,i,c?(constmap[i][s]+offset):0,real_addr,i_regs,rs2[i],ccadj[i],reglist);
#endif
}

static void c1ls_assemble(int i,struct regstat *i_regs)
{
  signed char s,th,tl,real_addr,addr,temp,ar,map=-1;
  int offset,type=0,memtarget=0,c=0;
  intptr_t jaddr,jaddr2=0;
  u_int hr,reglist=0;
  int agr=AGEN1+(i&1);
  th=get_reg(i_regs->regmap,FTEMP|64);
  tl=get_reg(i_regs->regmap,FTEMP);
  s=get_reg(i_regs->regmap,rs1[i]);
  temp=get_reg(i_regs->regmap,agr);
  if(temp<0) temp=get_reg(i_regs->regmap,-1);
  offset=imm[i];
  assert(tl>=0);
  assert(rs1[i]>0);
  assert(temp>=0);
  for(hr=0;hr<HOST_REGS;hr++) {
    if(i_regs->regmap[hr]>=0) reglist|=1<<hr;
  }
  if(i_regs->regmap[HOST_CCREG]==CCREG) reglist&=~(1<<HOST_CCREG);
  if (opcode[i]==0x31||opcode[i]==0x35) // LWC1/LDC1
  {
    // Loads use a temporary register which we need to save
    reglist|=1<<temp;
  }
  if (opcode[i]==0x39||opcode[i]==0x3D) // SWC1/SDC1
    ar=temp;
  else // LWC1/LDC1
    ar=tl;

  if(s>=0) {
    c=(i_regs->wasconst>>s)&1;
    memtarget=c&&((signed int)(constmap[i][s]+offset))<(signed int)0x80800000;
    if(c&&using_tlb&&((signed int)(constmap[i][s]+offset))>=(signed int)0xC0000000) memtarget=1;
  }
  if(offset||s<0||c) addr=ar;
  else addr=s;
  real_addr=addr;

  switch(opcode[i]) {
    case 0x31: type=LOADW_STUB; break;
    case 0x35: type=LOADD_STUB; break;
    case 0x39: type=STOREW_STUB; break;
    case 0x3D: type=STORED_STUB; break;
  }

  // Check cop1 unusable
  if(!cop1_usable) {
    signed char rs=get_reg(i_regs->regmap,CSREG);
    assert(rs>=0);
    emit_testimm(rs,CP0_STATUS_CU1);
    jaddr=(intptr_t)out;
    emit_jeq(0);
    add_stub(FP_STUB,jaddr,(intptr_t)out,i,rs,(intptr_t)i_regs,is_delayslot,0);
    cop1_usable=1;
  }
  if (opcode[i]==0x39) { // SWC1 (get float address)
    emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[(source[i]>>16)&0x1f],tl);
    emit_readword_indexed(0,tl,tl);
  }
  else if (opcode[i]==0x3D) { // SDC1 (get double address)
    emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[(source[i]>>16)&0x1f],tl);
    emit_readword_indexed(4,tl,th);
    emit_readword_indexed(0,tl,tl);
  }

#ifndef INTERPRET_C1LS
  // Generate address + offset
  if(!using_tlb) {
    if(!c) {
      emit_cmpimm(addr,0x800000);
      jaddr2=(intptr_t)out;
      emit_jno(0);
      #ifdef DESTRUCTIVE_SHIFT
      if (opcode[i]==0x39||opcode[i]==0x3D) { // SWC1/SDC1
        if(s==addr) emit_mov(s,temp);
      }
      #endif
    }
    #ifdef RAM_OFFSET
    #ifndef NATIVE_64
    if ((!c&&(opcode[i]==0x31||opcode[i]==0x35))||((!c||memtarget)&&(opcode[i]==0x39||opcode[i]==0x3D)))
    #else
    if(!c||memtarget)
    #endif
    {
      map=get_reg(i_regs->regmap,ROREG);
      if(map<0) emit_loadreg(ROREG,map=HOST_TEMPREG);
    }
    #endif
  }
  else
  {
    map=get_reg(i_regs->regmap,TLREG);
    int cache=get_reg(i_regs->regmap,MMREG);
    assert(map>=0);
    reglist&=~(1<<map);
    if (opcode[i]==0x31||opcode[i]==0x35) { // LWC1/LDC1
      map=do_tlb_r(addr,ar,map,cache,0,c,constmap[i][s]+offset);
      do_tlb_r_branch(map,c,constmap[i][s]+offset,&jaddr2);
    }
    else if (opcode[i]==0x39||opcode[i]==0x3D) { // SWC1/SDC1
      map=do_tlb_w(addr,ar,map,cache,0,c,constmap[i][s]+offset);
      do_tlb_w_branch(map,c,constmap[i][s]+offset,&jaddr2);
    }
  }

  if(!c||memtarget) {
    if (opcode[i]==0x31) { // LWC1
      #ifdef HOST_IMM_ADDR32
      if(c) emit_readword_tlb(constmap[i][s]+offset,map,tl);
      else
      #endif
      emit_readword_indexed_tlb(0,addr,map,tl);
    }
    else if (opcode[i]==0x35) { // LDC1
      assert(th>=0);
      #ifdef HOST_IMM_ADDR32
      if(c) emit_readdword_tlb(constmap[i][s]+offset,map,th,tl);
      else
      #endif
      emit_readdword_indexed_tlb(0,addr,map,th,tl);
    }
    else if (opcode[i]==0x39) { // SWC1
      emit_writeword_indexed_tlb(tl,0,addr,map);
    }
    else if (opcode[i]==0x3D) { // SDC1
      assert(th>=0);
      emit_writedword_indexed_tlb(th,tl,0,addr,map);
    }

    if(!using_tlb) {
      if (opcode[i]==0x39||opcode[i]==0x3D) { // SWC1/SDC1
        #ifdef DESTRUCTIVE_SHIFT
        addr=temp;
        #endif
        #if defined(HOST_IMM8) || defined(NEED_INVC_PTR)
        int ir=get_reg(i_regs->regmap,INVCP);
        assert(ir>=0);
        emit_cmpmem_indexedsr12_reg(ir,addr,1);
        #else
        emit_cmpmem_indexedsr12_imm((intptr_t)g_dev.r4300.cached_interp.invalid_code,addr,1);
        #endif
        #if defined(HAVE_CONDITIONAL_CALL) && !defined(DESTRUCTIVE_SHIFT)
        emit_callne(invalidate_addr_reg[addr]);
        #else
        intptr_t jaddr3=(intptr_t)out;
        emit_jne(0);
        add_stub(INVCODE_STUB,jaddr3,(intptr_t)out,reglist|(1<<HOST_CCREG),addr,0,0,0);
        #endif
      }
    }
  }
  if(jaddr2) {
    add_stub(type,jaddr2,(intptr_t)out,i,real_addr,(intptr_t)i_regs,ccadj[i],reglist);
  } else if(c&&!memtarget) {
    if (opcode[i]==0x39||opcode[i]==0x3D) // SWC1/SDC1
      inline_writestub(type,i,constmap[i][s]+offset,real_addr,i_regs,FTEMP,ccadj[i],reglist);
    else // LWC1/LDC1
      inline_readstub(type,i,constmap[i][s]+offset,real_addr,i_regs,FTEMP,ccadj[i],reglist);
  }
#else
  if (opcode[i]==0x39||opcode[i]==0x3D) // SWC1/SDC1
    inline_writestub(type,i,c?(constmap[i][s]+offset):0,real_addr,i_regs,FTEMP,ccadj[i],reglist);
  else // LWC1/LDC1
    inline_readstub(type,i,c?(constmap[i][s]+offset):0,real_addr,i_regs,FTEMP,ccadj[i],reglist);
#endif

  if (opcode[i]==0x31) { // LWC1 (write float)
    emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_simple[(source[i]>>16)&0x1f],temp);
    emit_writeword_indexed(tl,0,temp);
  }
  else if (opcode[i]==0x35) { // LDC1 (write double)
    emit_readptr((intptr_t)&g_dev.r4300.new_dynarec_hot_state.cp1_regs_double[(source[i]>>16)&0x1f],temp);
    emit_writeword_indexed(th,4,temp);
    emit_writeword_indexed(tl,0,temp);
  }
}

#ifndef multdiv_assemble
void multdiv_assemble(int i,struct regstat *i_regs)
{
  DebugMessage(M64MSG_ERROR, "Need multdiv_assemble for this architecture.");
  exit(1);
}
#endif

static void mov_assemble(int i,struct regstat *i_regs)
{
  //if(opcode2[i]==0x10||opcode2[i]==0x12) { // MFHI/MFLO
  //if(opcode2[i]==0x11||opcode2[i]==0x13) { // MTHI/MTLO
  if(rt1[i]) {
    signed char sh,sl,th,tl;
    th=get_reg(i_regs->regmap,rt1[i]|64);
    tl=get_reg(i_regs->regmap,rt1[i]);
    //assert(tl>=0);
    if(tl>=0) {
      sh=get_reg(i_regs->regmap,rs1[i]|64);
      sl=get_reg(i_regs->regmap,rs1[i]);
      if(sl>=0) emit_mov(sl,tl);
      else emit_loadreg(rs1[i],tl);
      if(th>=0) {
        if(sh>=0) emit_mov(sh,th);
        else emit_loadreg(rs1[i]|64,th);
      }
    }
  }
}

#ifndef fconv_assemble
void fconv_assemble(int i,struct regstat *i_regs)
{
  DebugMessage(M64MSG_ERROR, "Need fconv_assemble for this architecture.");
  exit(1);
}
#endif

#if 0
static void float_assemble(int i,struct regstat *i_regs)
{
  DebugMessage(M64MSG_ERROR, "Need float_assemble for this architecture.");
  exit(1);
}
#endif

static void syscall_assemble(int i,struct regstat *i_regs)
{
  signed char ccreg=get_reg(i_regs->regmap,CCREG);
  assert(ccreg==HOST_CCREG);
  assert(!is_delayslot);
  emit_movimm(start+i*4,0); // Get PC
  emit_addimm(HOST_CCREG,CLOCK_DIVIDER*ccadj[i],HOST_CCREG); // CHECK: is this right?  There should probably be an extra cycle...
  emit_jmp((intptr_t)jump_syscall);
}

static void ds_assemble(int i,struct regstat *i_regs)
{
  is_delayslot=1;
  switch(itype[i]) {
    case ALU:
      alu_assemble(i,i_regs);break;
    case IMM16:
      imm16_assemble(i,i_regs);break;
    case SHIFT:
      shift_assemble(i,i_regs);break;
    case SHIFTIMM:
      shiftimm_assemble(i,i_regs);break;
    case LOAD:
      load_assemble(i,i_regs);break;
    case LOADLR:
      loadlr_assemble(i,i_regs);break;
    case STORE:
      store_assemble(i,i_regs);break;
    case STORELR:
      storelr_assemble(i,i_regs);break;
    case COP0:
      cop0_assemble(i,i_regs);break;
    case COP1:
      cop1_assemble(i,i_regs);break;
    case C1LS:
      c1ls_assemble(i,i_regs);break;
    case FCONV:
      fconv_assemble(i,i_regs);break;
    case FLOAT:
      float_assemble(i,i_regs);break;
    case FCOMP:
      fcomp_assemble(i,i_regs);break;
    case MULTDIV:
      multdiv_assemble(i,i_regs);break;
    case MOV:
      mov_assemble(i,i_regs);break;
    case SYSCALL:
    case SPAN:
    case UJUMP:
    case RJUMP:
    case CJUMP:
    case SJUMP:
    case FJUMP:
      DebugMessage(M64MSG_VERBOSE, "Jump in the delay slot.  This is probably a bug.");
  }
  is_delayslot=0;
}

// Used when a branch jumps into the delay slot of another branch
static void ds_assemble_entry(int i)
{
  int t=(ba[i]-start)>>2;
  if(!instr_addr[t]) instr_addr[t]=(uintptr_t)out;
  assem_debug("Assemble delay slot at %x",ba[i]);
  assem_debug("<->");
  if(regs[t].regmap_entry[HOST_CCREG]==CCREG&&regs[t].regmap[HOST_CCREG]!=CCREG)
    wb_register(CCREG,regs[t].regmap_entry,regs[t].wasdirty,regs[t].was32);
  load_regs(regs[t].regmap_entry,regs[t].regmap,regs[t].was32,rs1[t],rs2[t]);
  address_generation(t,&regs[t],regs[t].regmap_entry);
  if(itype[t]==LOAD||itype[t]==LOADLR||itype[t]==STORE||itype[t]==STORELR||itype[t]==C1LS)
    load_regs(regs[t].regmap_entry,regs[t].regmap,regs[t].was32,MMREG,ROREG);
  if(itype[t]==STORE||itype[t]==STORELR||(opcode[t]&0x3b)==0x39)
    load_regs(regs[t].regmap_entry,regs[t].regmap,regs[t].was32,INVCP,INVCP);
  cop1_usable=0;
  is_delayslot=0;
  int cc=ccadj[t];
  ccadj[t]=-1;
  switch(itype[t]) {
    case ALU:
      alu_assemble(t,&regs[t]);break;
    case IMM16:
      imm16_assemble(t,&regs[t]);break;
    case SHIFT:
      shift_assemble(t,&regs[t]);break;
    case SHIFTIMM:
      shiftimm_assemble(t,&regs[t]);break;
    case LOAD:
      load_assemble(t,&regs[t]);break;
    case LOADLR:
      loadlr_assemble(t,&regs[t]);break;
    case STORE:
      store_assemble(t,&regs[t]);break;
    case STORELR:
      storelr_assemble(t,&regs[t]);break;
    case COP0:
      cop0_assemble(t,&regs[t]);break;
    case COP1:
      cop1_assemble(t,&regs[t]);break;
    case C1LS:
      c1ls_assemble(t,&regs[t]);break;
    case FCONV:
      fconv_assemble(t,&regs[t]);break;
    case FLOAT:
      float_assemble(t,&regs[t]);break;
    case FCOMP:
      fcomp_assemble(t,&regs[t]);break;
    case MULTDIV:
      multdiv_assemble(t,&regs[t]);break;
    case MOV:
      mov_assemble(t,&regs[t]);break;
    case SYSCALL:
    case SPAN:
    case UJUMP:
    case RJUMP:
    case CJUMP:
    case SJUMP:
    case FJUMP:
      DebugMessage(M64MSG_VERBOSE, "Jump in the delay slot.  This is probably a bug.");
  }
  store_regs_bt(regs[t].regmap,regs[t].is32,regs[t].dirty,ba[i]+4);
  load_regs_bt(regs[t].regmap,regs[t].is32,regs[t].dirty,ba[i]+4);
  if(internal_branch(regs[t].is32,ba[i]+4))
    assem_debug("branch: internal");
  else
    assem_debug("branch: external");
  assert(internal_branch(regs[t].is32,ba[i]+4));
  add_to_linker((intptr_t)out,ba[i]+4,internal_branch(regs[t].is32,ba[i]+4));
  emit_jmp(0);
  ccadj[t]=cc;
}

static void ujump_assemble(int i,struct regstat *i_regs)
{
  #ifdef REG_PREFETCH
  signed char *i_regmap=i_regs->regmap;
  #endif
  if(i==(ba[i]-start)>>2) assem_debug("idle loop");
  address_generation(i+1,i_regs,regs[i].regmap_entry);

  if((rt1[i]==31)&&(rt1[i+1]==31||rs1[i+1]==31||rs2[i+1]==31))
  {
    signed char rt=get_reg(branch_regs[i].regmap,31);
    assert(get_reg(i_regs->regmap,31)==rt);
    emit_movimm(start+i*4+8,rt); // PC into link register
  }

  #ifdef REG_PREFETCH
  int temp=get_reg(branch_regs[i].regmap,PTEMP);
  if(rt1[i]==31&&temp>=0)
  {
    int return_address=start+i*4+8;
    if(get_reg(branch_regs[i].regmap,31)>0)
    if(i_regmap[temp]==PTEMP) emit_movimm((intptr_t)hash_table[((return_address>>16)^return_address)&0xFFFF],temp);
  }
  #endif
  ds_assemble(i+1,i_regs);
  uint64_t bc_unneeded=branch_regs[i].u;
  uint64_t bc_unneeded_upper=branch_regs[i].uu;
  bc_unneeded|=1|(1LL<<rt1[i]);
  bc_unneeded_upper|=1|(1LL<<rt1[i]);
  wb_invalidate(regs[i].regmap,branch_regs[i].regmap,regs[i].dirty,regs[i].is32,
                bc_unneeded,bc_unneeded_upper);
  load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,CCREG,CCREG);
  if(rt1[i]==31&&rt1[i+1]!=31) {
    int rt;
    unsigned int return_address;
    assert(rt2[i+1]!=31);
    rt=get_reg(branch_regs[i].regmap,31);
    //assert(rt>=0);
    return_address=start+i*4+8;
    if(rt>=0) {
      #ifdef USE_MINI_HT
      if(internal_branch(branch_regs[i].is32,return_address)) {
        int temp=-1; //x86 doesn't need a temp reg
        #ifdef HOST_TEMPREG
        if(temp<0) temp=HOST_TEMPREG;
        #endif
        do_miniht_insert(return_address,rt,temp);
      }
      else
      #endif
      {
        #ifdef REG_PREFETCH
        if(temp>=0)
        {
          if(i_regmap[temp]!=PTEMP) emit_movimm((intptr_t)hash_table[((return_address>>16)^return_address)&0xFFFF],temp);
        }
        #endif
        emit_movimm(return_address,rt); // PC into link register
        #ifdef IMM_PREFETCH
        emit_prefetch(hash_table[((return_address>>16)^return_address)&0xFFFF]);
        #endif
      }
    }
  }
  int cc,adj;
  cc=get_reg(branch_regs[i].regmap,CCREG);
  assert(cc==HOST_CCREG);
  store_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
  #ifdef REG_PREFETCH
  if(rt1[i]==31&&temp>=0) emit_prefetchreg(temp);
  #endif
  do_cc(i,branch_regs[i].regmap,&adj,ba[i],TAKEN,0);
  if(i!=(ba[i]-start)>>2 || source[i+1]!=0) {
    if(adj) emit_addimm(cc,CLOCK_DIVIDER*(ccadj[i]+2-adj),cc);
    load_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
    if(internal_branch(branch_regs[i].is32,ba[i]))
      assem_debug("branch: internal");
    else
      assem_debug("branch: external");
    if(internal_branch(branch_regs[i].is32,ba[i])&&is_ds[(ba[i]-start)>>2]) {
      ds_assemble_entry(i);
    }
    else {
      add_to_linker((intptr_t)out,ba[i],internal_branch(branch_regs[i].is32,ba[i]));
      emit_jmp(0);
    }
  }
}

static void rjump_assemble(int i,struct regstat *i_regs)
{
  #ifdef REG_PREFETCH
  signed char *i_regmap=i_regs->regmap;
  #endif
  int temp;
  int rs,cc;
  rs=get_reg(branch_regs[i].regmap,rs1[i]);
  assert(rs>=0);
  if((rs1[i]==rt1[i+1]||rs1[i]==rt2[i+1])&&(rs1[i]!=0)) {
    // Delay slot abuse, make a copy of the branch address register
    temp=get_reg(branch_regs[i].regmap,RTEMP);
    assert(temp>=0);
    assert(regs[i].regmap[temp]==RTEMP);
    emit_mov(rs,temp);
    rs=temp;
  }
  address_generation(i+1,i_regs,regs[i].regmap_entry);
  #ifdef REG_PREFETCH
  if(rt1[i]==31)
  {
    if((temp=get_reg(branch_regs[i].regmap,PTEMP))>=0) {
      int return_address=start+i*4+8;
      if(i_regmap[temp]==PTEMP) emit_movimm((intptr_t)hash_table[((return_address>>16)^return_address)&0xFFFF],temp);
    }
  }
  #endif
  #ifdef USE_MINI_HT
  if(rs1[i]==31) {
    int rh=get_reg(regs[i].regmap,RHASH);
    if(rh>=0) do_preload_rhash(rh);
  }
  #endif
  ds_assemble(i+1,i_regs);
  uint64_t bc_unneeded=branch_regs[i].u;
  uint64_t bc_unneeded_upper=branch_regs[i].uu;
  bc_unneeded|=1|(1LL<<rt1[i]);
  bc_unneeded_upper|=1|(1LL<<rt1[i]);
  bc_unneeded&=~(1LL<<rs1[i]);
  wb_invalidate(regs[i].regmap,branch_regs[i].regmap,regs[i].dirty,regs[i].is32,
                bc_unneeded,bc_unneeded_upper);
  load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,rs1[i],CCREG);
  if(rt1[i]!=0) {
    int rt,return_address;
    assert(rt1[i+1]!=rt1[i]);
    assert(rt2[i+1]!=rt1[i]);
    rt=get_reg(branch_regs[i].regmap,rt1[i]);
    assert(rt>=0);
    return_address=start+i*4+8;
    #ifdef REG_PREFETCH
    if(temp>=0)
    {
      if(i_regmap[temp]!=PTEMP) emit_movimm((intptr_t)hash_table[((return_address>>16)^return_address)&0xFFFF],temp);
    }
    #endif
    emit_movimm(return_address,rt); // PC into link register
    #ifdef IMM_PREFETCH
    emit_prefetch(hash_table[((return_address>>16)^return_address)&0xFFFF]);
    #endif
  }
  cc=get_reg(branch_regs[i].regmap,CCREG);
  assert(cc==HOST_CCREG);
  #ifdef USE_MINI_HT
  int rh=get_reg(branch_regs[i].regmap,RHASH);
  int ht=get_reg(branch_regs[i].regmap,RHTBL);
  if(rs1[i]==31) {
    if(regs[i].regmap[rh]!=RHASH) do_preload_rhash(rh);
    do_preload_rhtbl(ht);
    do_rhash(rs,rh);
  }
  #endif
  store_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,-1);
  #ifdef DESTRUCTIVE_WRITEBACK
  if((branch_regs[i].dirty>>rs)&(branch_regs[i].is32>>rs1[i])&1) {
    if(rs1[i]!=rt1[i+1]&&rs1[i]!=rt2[i+1]) {
      emit_loadreg(rs1[i],rs);
    }
  }
  #endif
  #ifdef REG_PREFETCH
  if(rt1[i]==31&&temp>=0) emit_prefetchreg(temp);
  #endif
  #ifdef USE_MINI_HT
  if(rs1[i]==31) {
    do_miniht_load(ht,rh);
  }
  #endif
  //do_cc(i,branch_regs[i].regmap,&adj,-1,TAKEN);
  //if(adj) emit_addimm(cc,2*(ccadj[i]+2-adj),cc); // ??? - Shouldn't happen
  //assert(adj==0);
  emit_addimm_and_set_flags(CLOCK_DIVIDER*(ccadj[i]+2),HOST_CCREG);
  add_stub(CC_STUB,(intptr_t)out,jump_vaddr_reg[rs],0,i,-1,TAKEN,0);
  emit_jns(0);
  //load_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,-1);
  #ifdef USE_MINI_HT
  if(rs1[i]==31) {
    do_miniht_jump(rs,rh,ht);
  }
  else
  #endif
  {
#if NEW_DYNAREC==NEW_DYNAREC_ARM64
  if(rs==18) {
    // x18 is used for trampoline jumps, move it to another register (x0)
    emit_mov(rs,0);
    rs=0;
  }
#endif
    emit_jmp(jump_vaddr_reg[rs]);
  }
  #ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
  if(rt1[i]!=31&&i<slen-2&&(((uintptr_t)out)&7)) emit_mov(13,13);
  #endif
}

static void cjump_assemble(int i,struct regstat *i_regs)
{
  signed char *i_regmap=i_regs->regmap;
  int cc;
  int match;
  match=match_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
  assem_debug("match=%d",match);
  int s1h,s1l,s2h,s2l;
  int prev_cop1_usable=cop1_usable;
  int unconditional=0,nop=0;
  int only32=0;
  int invert=0;
  int branch_internal=internal_branch(branch_regs[i].is32,ba[i]);
  if(i==(ba[i]-start)>>2) assem_debug("idle loop");
  if(!match) invert=1;
  #ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
  if(i>(ba[i]-start)>>2) invert=1;
  #endif

  if(ooo[i]) {
    s1l=get_reg(branch_regs[i].regmap,rs1[i]);
    s1h=get_reg(branch_regs[i].regmap,rs1[i]|64);
    s2l=get_reg(branch_regs[i].regmap,rs2[i]);
    s2h=get_reg(branch_regs[i].regmap,rs2[i]|64);
  }
  else {
    s1l=get_reg(i_regmap,rs1[i]);
    s1h=get_reg(i_regmap,rs1[i]|64);
    s2l=get_reg(i_regmap,rs2[i]);
    s2h=get_reg(i_regmap,rs2[i]|64);
  }
  if(rs1[i]==0&&rs2[i]==0)
  {
    if(opcode[i]&1) nop=1;
    else unconditional=1;
    //assert(opcode[i]!=5);
    //assert(opcode[i]!=7);
    //assert(opcode[i]!=0x15);
    //assert(opcode[i]!=0x17);
  }
  else if(rs1[i]==0)
  {
    s1l=s2l;s1h=s2h;
    s2l=s2h=-1;
    only32=(regs[i].was32>>rs2[i])&1;
  }
  else if(rs2[i]==0)
  {
    s2l=s2h=-1;
    only32=(regs[i].was32>>rs1[i])&1;
  }
  else {
    only32=(regs[i].was32>>rs1[i])&(regs[i].was32>>rs2[i])&1;
  }

  if(ooo[i]) {
    // Out of order execution (delay slot first)
    //DebugMessage(M64MSG_VERBOSE, "OOOE");
    address_generation(i+1,i_regs,regs[i].regmap_entry);
    ds_assemble(i+1,i_regs);
    int adj;
    uint64_t bc_unneeded=branch_regs[i].u;
    uint64_t bc_unneeded_upper=branch_regs[i].uu;
    bc_unneeded&=~((1LL<<rs1[i])|(1LL<<rs2[i]));
    bc_unneeded_upper&=~((1LL<<us1[i])|(1LL<<us2[i]));
    bc_unneeded|=1;
    bc_unneeded_upper|=1;
    wb_invalidate(regs[i].regmap,branch_regs[i].regmap,regs[i].dirty,regs[i].is32,
                  bc_unneeded,bc_unneeded_upper);
    load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,rs1[i],rs2[i]);
    load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,CCREG,CCREG);
    cc=get_reg(branch_regs[i].regmap,CCREG);
    assert(cc==HOST_CCREG);
    if(unconditional)
      store_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
    //do_cc(i,branch_regs[i].regmap,&adj,unconditional?ba[i]:-1,unconditional);
    //assem_debug("cycle count (adj)");
    if(unconditional) {
      do_cc(i,branch_regs[i].regmap,&adj,ba[i],TAKEN,0);
      if(i!=(ba[i]-start)>>2 || source[i+1]!=0) {
        if(adj) emit_addimm(cc,CLOCK_DIVIDER*(ccadj[i]+2-adj),cc);
        load_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
        if(branch_internal)
          assem_debug("branch: internal");
        else
          assem_debug("branch: external");
        if(branch_internal&&is_ds[(ba[i]-start)>>2]) {
          ds_assemble_entry(i);
        }
        else {
          add_to_linker((intptr_t)out,ba[i],branch_internal);
          emit_jmp(0);
        }
        #ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
        if(((uintptr_t)out)&7) emit_addnop(0);
        #endif
      }
    }
    else if(nop) {
      emit_addimm_and_set_flags(CLOCK_DIVIDER*(ccadj[i]+2),cc);
      intptr_t jaddr=(intptr_t)out;
      emit_jns(0);
      add_stub(CC_STUB,jaddr,(intptr_t)out,0,i,start+i*4+8,NOTTAKEN,0);
    }
    else {
      intptr_t taken=0,nottaken=0,nottaken1=0;
      do_cc(i,branch_regs[i].regmap,&adj,-1,0,invert);
      if(adj&&!invert) emit_addimm(cc,CLOCK_DIVIDER*(ccadj[i]+2-adj),cc);
      if(!only32)
      {
        assert(s1h>=0);
        if(opcode[i]==4) // BEQ
        {
          if(s2h>=0) emit_cmp(s1h,s2h);
          else emit_test(s1h,s1h);
          nottaken1=(intptr_t)out;
          emit_jne(1);
        }
        if(opcode[i]==5) // BNE
        {
          if(s2h>=0) emit_cmp(s1h,s2h);
          else emit_test(s1h,s1h);
          if(invert) taken=(intptr_t)out;
          else add_to_linker((intptr_t)out,ba[i],branch_internal);
          emit_jne(0);
        }
        if(opcode[i]==6) // BLEZ
        {
          emit_test(s1h,s1h);
          if(invert) taken=(intptr_t)out;
          else add_to_linker((intptr_t)out,ba[i],branch_internal);
          emit_js(0);
          nottaken1=(intptr_t)out;
          emit_jne(1);
        }
        if(opcode[i]==7) // BGTZ
        {
          emit_test(s1h,s1h);
          nottaken1=(intptr_t)out;
          emit_js(1);
          if(invert) taken=(intptr_t)out;
          else add_to_linker((intptr_t)out,ba[i],branch_internal);
          emit_jne(0);
        }
      } // if(!only32)

      assert(s1l>=0);
      if(opcode[i]==4) // BEQ
      {
        if(s2l>=0) emit_cmp(s1l,s2l);
        else emit_test(s1l,s1l);
        if(invert){
          nottaken=(intptr_t)out;
          emit_jne(1);
        }else{
          add_to_linker((intptr_t)out,ba[i],branch_internal);
          emit_jeq(0);
        }
      }
      if(opcode[i]==5) // BNE
      {
        if(s2l>=0) emit_cmp(s1l,s2l);
        else emit_test(s1l,s1l);
        if(invert){
          nottaken=(intptr_t)out;
          emit_jeq(1);
        }else{
          add_to_linker((intptr_t)out,ba[i],branch_internal);
          emit_jne(0);
        }
      }
      if(opcode[i]==6) // BLEZ
      {
        emit_cmpimm(s1l,1);
        if(invert){
          nottaken=(intptr_t)out;
          if(only32) emit_jge(1);
          else emit_jae(1);
        }else{
          add_to_linker((intptr_t)out,ba[i],branch_internal);
          if(only32) emit_jl(0);
          else emit_jb(0);
        }
      }
      if(opcode[i]==7) // BGTZ
      {
        emit_cmpimm(s1l,1);
        if(invert){
          nottaken=(intptr_t)out;
          if(only32) emit_jl(1);
          else emit_jb(1);
        }else{
          add_to_linker((intptr_t)out,ba[i],branch_internal);
          if(only32) emit_jge(0);
          else emit_jae(0);
        }
      }
      if(invert) {
        if(taken) set_jump_target(taken,(intptr_t)out);
        #ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
        if(match&&(!branch_internal||!is_ds[(ba[i]-start)>>2])) {
          if(adj) {
            emit_addimm(cc,-CLOCK_DIVIDER*adj,cc);
            add_to_linker((intptr_t)out,ba[i],branch_internal);
          }else{
            emit_addnop(13);
            add_to_linker((intptr_t)out,ba[i],branch_internal*2);
          }
          emit_jmp(0);
        }else
        #endif
        {
          if(adj) emit_addimm(cc,-(int)CLOCK_DIVIDER*adj,cc);
          store_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
          load_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
          if(branch_internal)
            assem_debug("branch: internal");
          else
            assem_debug("branch: external");
          if(branch_internal&&is_ds[(ba[i]-start)>>2]) {
            ds_assemble_entry(i);
          }
          else {
            add_to_linker((intptr_t)out,ba[i],branch_internal);
            emit_jmp(0);
          }
        }
        set_jump_target(nottaken,(intptr_t)out);
      }

      if(nottaken1) set_jump_target(nottaken1,(intptr_t)out);
      if(adj) {
        if(!invert) emit_addimm(cc,CLOCK_DIVIDER*adj,cc);
      }
    } // (!unconditional)
  } // if(ooo)
  else
  {
    // In-order execution (branch first)
    //if(likely[i]) DebugMessage(M64MSG_VERBOSE, "IOL");
    //else
    //DebugMessage(M64MSG_VERBOSE, "IOE");
    intptr_t taken=0,nottaken=0,nottaken1=0;
    if(!unconditional&&!nop) {
      if(!only32)
      {
        assert(s1h>=0);
        if((opcode[i]&0x2f)==4) // BEQ
        {
          if(s2h>=0) emit_cmp(s1h,s2h);
          else emit_test(s1h,s1h);
          nottaken1=(intptr_t)out;
          emit_jne(2);
        }
        if((opcode[i]&0x2f)==5) // BNE
        {
          if(s2h>=0) emit_cmp(s1h,s2h);
          else emit_test(s1h,s1h);
          taken=(intptr_t)out;
          emit_jne(1);
        }
        if((opcode[i]&0x2f)==6) // BLEZ
        {
          emit_test(s1h,s1h);
          taken=(intptr_t)out;
          emit_js(1);
          nottaken1=(intptr_t)out;
          emit_jne(2);
        }
        if((opcode[i]&0x2f)==7) // BGTZ
        {
          emit_test(s1h,s1h);
          nottaken1=(intptr_t)out;
          emit_js(2);
          taken=(intptr_t)out;
          emit_jne(1);
        }
      } // if(!only32)

      assert(s1l>=0);
      if((opcode[i]&0x2f)==4) // BEQ
      {
        if(s2l>=0) emit_cmp(s1l,s2l);
        else emit_test(s1l,s1l);
        nottaken=(intptr_t)out;
        emit_jne(2);
      }
      if((opcode[i]&0x2f)==5) // BNE
      {
        if(s2l>=0) emit_cmp(s1l,s2l);
        else emit_test(s1l,s1l);
        nottaken=(intptr_t)out;
        emit_jeq(2);
      }
      if((opcode[i]&0x2f)==6) // BLEZ
      {
        emit_cmpimm(s1l,1);
        nottaken=(intptr_t)out;
        if(only32) emit_jge(2);
        else emit_jae(2);
      }
      if((opcode[i]&0x2f)==7) // BGTZ
      {
        emit_cmpimm(s1l,1);
        nottaken=(intptr_t)out;
        if(only32) emit_jl(2);
        else emit_jb(2);
      }
    } // if(!unconditional)
    int adj;
    uint64_t ds_unneeded=branch_regs[i].u;
    uint64_t ds_unneeded_upper=branch_regs[i].uu;
    ds_unneeded&=~((1LL<<rs1[i+1])|(1LL<<rs2[i+1]));
    ds_unneeded_upper&=~((1LL<<us1[i+1])|(1LL<<us2[i+1]));
    if((~ds_unneeded_upper>>rt1[i+1])&1) ds_unneeded_upper&=~((1LL<<dep1[i+1])|(1LL<<dep2[i+1]));
    ds_unneeded|=1;
    ds_unneeded_upper|=1;
    // branch taken
    if(!nop) {
      if(taken) set_jump_target(taken,(intptr_t)out);
      assem_debug("1:");
      wb_invalidate(regs[i].regmap,branch_regs[i].regmap,regs[i].dirty,regs[i].is32,
                    ds_unneeded,ds_unneeded_upper);
      // load regs
      load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,rs1[i+1],rs2[i+1]);
      address_generation(i+1,&branch_regs[i],0);
      load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,CCREG,INVCP);
      ds_assemble(i+1,&branch_regs[i]);
      cc=get_reg(branch_regs[i].regmap,CCREG);
      if(cc==-1) {
        emit_loadreg(CCREG,cc=HOST_CCREG);
        // CHECK: Is the following instruction (fall thru) allocated ok?
      }
      assert(cc==HOST_CCREG);
      store_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
      do_cc(i,i_regmap,&adj,ba[i],TAKEN,0);
      if(i!=(ba[i]-start)>>2 || source[i+1]!=0) {
        assem_debug("cycle count (adj)");
        if(adj) emit_addimm(cc,CLOCK_DIVIDER*(ccadj[i]+2-adj),cc);
        load_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
        if(branch_internal)
          assem_debug("branch: internal");
        else
          assem_debug("branch: external");
        if(branch_internal&&is_ds[(ba[i]-start)>>2]) {
          ds_assemble_entry(i);
        }
        else {
          add_to_linker((intptr_t)out,ba[i],branch_internal);
          emit_jmp(0);
        }
      }
    }
    // branch not taken
    cop1_usable=prev_cop1_usable;
    if(!unconditional) {
      if(nottaken1) set_jump_target(nottaken1,(intptr_t)out);
      set_jump_target(nottaken,(intptr_t)out);
      assem_debug("2:");
      if(!likely[i]) {
        wb_invalidate(regs[i].regmap,branch_regs[i].regmap,regs[i].dirty,regs[i].is32,
                      ds_unneeded,ds_unneeded_upper);
        load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,rs1[i+1],rs2[i+1]);
        address_generation(i+1,&branch_regs[i],0);
        load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,CCREG,CCREG);
        ds_assemble(i+1,&branch_regs[i]);
      }
      cc=get_reg(branch_regs[i].regmap,CCREG);
      if(cc==-1&&!likely[i]) {
        // Cycle count isn't in a register, temporarily load it then write it out
        emit_loadreg(CCREG,HOST_CCREG);
        emit_addimm_and_set_flags(CLOCK_DIVIDER*(ccadj[i]+2),HOST_CCREG);
        intptr_t jaddr=(intptr_t)out;
        emit_jns(0);
        add_stub(CC_STUB,jaddr,(intptr_t)out,0,i,start+i*4+8,NOTTAKEN,0);
        emit_storereg(CCREG,HOST_CCREG);
      }
      else{
        cc=get_reg(i_regmap,CCREG);
        assert(cc==HOST_CCREG);
        emit_addimm_and_set_flags(CLOCK_DIVIDER*(ccadj[i]+2),cc);
        intptr_t jaddr=(intptr_t)out;
        emit_jns(0);
        add_stub(CC_STUB,jaddr,(intptr_t)out,0,i,start+i*4+8,likely[i]?NULLDS:NOTTAKEN,0);
      }
    }
  }
}

static void sjump_assemble(int i,struct regstat *i_regs)
{
  signed char *i_regmap=i_regs->regmap;
  int cc;
  int match;
  match=match_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
  assem_debug("smatch=%d",match);
  int s1h,s1l;
  int prev_cop1_usable=cop1_usable;
  int unconditional=0,nevertaken=0;
  int only32=0;
  int invert=0;
  int branch_internal=internal_branch(branch_regs[i].is32,ba[i]);
  if(i==(ba[i]-start)>>2) assem_debug("idle loop");
  if(!match) invert=1;
  #ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
  if(i>(ba[i]-start)>>2) invert=1;
  #endif

  //if(opcode2[i]>=0x10) return; // FIXME (BxxZAL)
  assert(opcode2[i]<0x10||rs1[i]==0); // FIXME (BxxZAL)

  if(ooo[i]) {
    s1l=get_reg(branch_regs[i].regmap,rs1[i]);
    s1h=get_reg(branch_regs[i].regmap,rs1[i]|64);
  }
  else {
    s1l=get_reg(i_regmap,rs1[i]);
    s1h=get_reg(i_regmap,rs1[i]|64);
  }
  if(rs1[i]==0)
  {
    if(opcode2[i]&1) unconditional=1;
    else nevertaken=1;
    // These are never taken (r0 is never less than zero)
    //assert(opcode2[i]!=0);
    //assert(opcode2[i]!=2);
    //assert(opcode2[i]!=0x10);
    //assert(opcode2[i]!=0x12);
  }
  else {
    only32=(regs[i].was32>>rs1[i])&1;
  }

  if(ooo[i]) {
    // Out of order execution (delay slot first)
    //DebugMessage(M64MSG_VERBOSE, "OOOE");
    address_generation(i+1,i_regs,regs[i].regmap_entry);
    ds_assemble(i+1,i_regs);
    int adj;
    uint64_t bc_unneeded=branch_regs[i].u;
    uint64_t bc_unneeded_upper=branch_regs[i].uu;
    bc_unneeded&=~((1LL<<rs1[i])|(1LL<<rs2[i]));
    bc_unneeded_upper&=~((1LL<<us1[i])|(1LL<<us2[i]));
    bc_unneeded|=1;
    bc_unneeded_upper|=1;
    wb_invalidate(regs[i].regmap,branch_regs[i].regmap,regs[i].dirty,regs[i].is32,
                  bc_unneeded,bc_unneeded_upper);
    load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,rs1[i],rs1[i]);
    load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,CCREG,CCREG);
    if(rt1[i]==31) {
      int rt,return_address;
      assert(rt1[i+1]!=31);
      assert(rt2[i+1]!=31);
      rt=get_reg(branch_regs[i].regmap,31);
      if(rt>=0) {
        // Save the PC even if the branch is not taken
        return_address=start+i*4+8;
        emit_movimm(return_address,rt); // PC into link register
        #ifdef IMM_PREFETCH
        if(!nevertaken) emit_prefetch(hash_table[((return_address>>16)^return_address)&0xFFFF]);
        #endif
      }
    }
    cc=get_reg(branch_regs[i].regmap,CCREG);
    assert(cc==HOST_CCREG);
    if(unconditional)
      store_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
    //do_cc(i,branch_regs[i].regmap,&adj,unconditional?ba[i]:-1,unconditional);
    assem_debug("cycle count (adj)");
    if(unconditional) {
      do_cc(i,branch_regs[i].regmap,&adj,ba[i],TAKEN,0);
      if(i!=(ba[i]-start)>>2 || source[i+1]!=0) {
        if(adj) emit_addimm(cc,CLOCK_DIVIDER*(ccadj[i]+2-adj),cc);
        load_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
        if(branch_internal)
          assem_debug("branch: internal");
        else
          assem_debug("branch: external");
        if(branch_internal&&is_ds[(ba[i]-start)>>2]) {
          ds_assemble_entry(i);
        }
        else {
          add_to_linker((intptr_t)out,ba[i],branch_internal);
          emit_jmp(0);
        }
        #ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
        if(((uintptr_t)out)&7) emit_addnop(0);
        #endif
      }
    }
    else if(nevertaken) {
      emit_addimm_and_set_flags(CLOCK_DIVIDER*(ccadj[i]+2),cc);
      intptr_t jaddr=(intptr_t)out;
      emit_jns(0);
      add_stub(CC_STUB,jaddr,(intptr_t)out,0,i,start+i*4+8,NOTTAKEN,0);
    }
    else {
      intptr_t nottaken=0;
      do_cc(i,branch_regs[i].regmap,&adj,-1,0,invert);
      if(adj&&!invert) emit_addimm(cc,CLOCK_DIVIDER*(ccadj[i]+2-adj),cc);
      if(!only32)
      {
        assert(s1h>=0);
        if(opcode2[i]==0) // BLTZ
        {
          emit_test(s1h,s1h);
          if(invert){
            nottaken=(intptr_t)out;
            emit_jns(1);
          }else{
            add_to_linker((intptr_t)out,ba[i],branch_internal);
            emit_js(0);
          }
        }
        if(opcode2[i]==1) // BGEZ
        {
          emit_test(s1h,s1h);
          if(invert){
            nottaken=(intptr_t)out;
            emit_js(1);
          }else{
            add_to_linker((intptr_t)out,ba[i],branch_internal);
            emit_jns(0);
          }
        }
      } // if(!only32)
      else
      {
        assert(s1l>=0);
        if(opcode2[i]==0) // BLTZ
        {
          emit_test(s1l,s1l);
          if(invert){
            nottaken=(intptr_t)out;
            emit_jns(1);
          }else{
            add_to_linker((intptr_t)out,ba[i],branch_internal);
            emit_js(0);
          }
        }
        if(opcode2[i]==1) // BGEZ
        {
          emit_test(s1l,s1l);
          if(invert){
            nottaken=(intptr_t)out;
            emit_js(1);
          }else{
            add_to_linker((intptr_t)out,ba[i],branch_internal);
            emit_jns(0);
          }
        }
      } // if(!only32)

      if(invert) {
        #ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
        if(match&&(!branch_internal||!is_ds[(ba[i]-start)>>2])) {
          if(adj) {
            emit_addimm(cc,-CLOCK_DIVIDER*adj,cc);
            add_to_linker((intptr_t)out,ba[i],branch_internal);
          }else{
            emit_addnop(13);
            add_to_linker((intptr_t)out,ba[i],branch_internal*2);
          }
          emit_jmp(0);
        }else
        #endif
        {
          if(adj) emit_addimm(cc,-(int)CLOCK_DIVIDER*adj,cc);
          store_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
          load_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
          if(branch_internal)
            assem_debug("branch: internal");
          else
            assem_debug("branch: external");
          if(branch_internal&&is_ds[(ba[i]-start)>>2]) {
            ds_assemble_entry(i);
          }
          else {
            add_to_linker((intptr_t)out,ba[i],branch_internal);
            emit_jmp(0);
          }
        }
        set_jump_target(nottaken,(intptr_t)out);
      }

      if(adj) {
        if(!invert) emit_addimm(cc,CLOCK_DIVIDER*adj,cc);
      }
    } // (!unconditional)
  } // if(ooo)
  else
  {
    // In-order execution (branch first)
    //DebugMessage(M64MSG_VERBOSE, "IOE");
    intptr_t nottaken=0;
    if(!unconditional) {
      if(!only32)
      {
        assert(s1h>=0);
        if((opcode2[i]&0x1d)==0) // BLTZ/BLTZL
        {
          emit_test(s1h,s1h);
          nottaken=(intptr_t)out;
          emit_jns(1);
        }
        if((opcode2[i]&0x1d)==1) // BGEZ/BGEZL
        {
          emit_test(s1h,s1h);
          nottaken=(intptr_t)out;
          emit_js(1);
        }
      } // if(!only32)
      else
      {
        assert(s1l>=0);
        if((opcode2[i]&0x1d)==0) // BLTZ/BLTZL
        {
          emit_test(s1l,s1l);
          nottaken=(intptr_t)out;
          emit_jns(1);
        }
        if((opcode2[i]&0x1d)==1) // BGEZ/BGEZL
        {
          emit_test(s1l,s1l);
          nottaken=(intptr_t)out;
          emit_js(1);
        }
      }
    } // if(!unconditional)
    int adj;
    uint64_t ds_unneeded=branch_regs[i].u;
    uint64_t ds_unneeded_upper=branch_regs[i].uu;
    ds_unneeded&=~((1LL<<rs1[i+1])|(1LL<<rs2[i+1]));
    ds_unneeded_upper&=~((1LL<<us1[i+1])|(1LL<<us2[i+1]));
    if((~ds_unneeded_upper>>rt1[i+1])&1) ds_unneeded_upper&=~((1LL<<dep1[i+1])|(1LL<<dep2[i+1]));
    ds_unneeded|=1;
    ds_unneeded_upper|=1;
    // branch taken
    if(!nevertaken) {
      //assem_debug("1:");
      wb_invalidate(regs[i].regmap,branch_regs[i].regmap,regs[i].dirty,regs[i].is32,
                    ds_unneeded,ds_unneeded_upper);
      // load regs
      load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,rs1[i+1],rs2[i+1]);
      address_generation(i+1,&branch_regs[i],0);
      load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,CCREG,INVCP);
      ds_assemble(i+1,&branch_regs[i]);
      cc=get_reg(branch_regs[i].regmap,CCREG);
      if(cc==-1) {
        emit_loadreg(CCREG,cc=HOST_CCREG);
        // CHECK: Is the following instruction (fall thru) allocated ok?
      }
      assert(cc==HOST_CCREG);
      store_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
      do_cc(i,i_regmap,&adj,ba[i],TAKEN,0);
      if(i!=(ba[i]-start)>>2 || source[i+1]!=0) {
        assem_debug("cycle count (adj)");
        if(adj) emit_addimm(cc,CLOCK_DIVIDER*(ccadj[i]+2-adj),cc);
        load_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
        if(branch_internal)
          assem_debug("branch: internal");
        else
          assem_debug("branch: external");
        if(branch_internal&&is_ds[(ba[i]-start)>>2]) {
          ds_assemble_entry(i);
        }
        else {
          add_to_linker((intptr_t)out,ba[i],branch_internal);
          emit_jmp(0);
        }
      }
    }
    // branch not taken
    cop1_usable=prev_cop1_usable;
    if(!unconditional) {
      set_jump_target(nottaken,(intptr_t)out);
      assem_debug("1:");
      if(!likely[i]) {
        wb_invalidate(regs[i].regmap,branch_regs[i].regmap,regs[i].dirty,regs[i].is32,
                      ds_unneeded,ds_unneeded_upper);
        load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,rs1[i+1],rs2[i+1]);
        address_generation(i+1,&branch_regs[i],0);
        load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,CCREG,CCREG);
        ds_assemble(i+1,&branch_regs[i]);
      }
      cc=get_reg(branch_regs[i].regmap,CCREG);
      if(cc==-1&&!likely[i]) {
        // Cycle count isn't in a register, temporarily load it then write it out
        emit_loadreg(CCREG,HOST_CCREG);
        emit_addimm_and_set_flags(CLOCK_DIVIDER*(ccadj[i]+2),HOST_CCREG);
        intptr_t jaddr=(intptr_t)out;
        emit_jns(0);
        add_stub(CC_STUB,jaddr,(intptr_t)out,0,i,start+i*4+8,NOTTAKEN,0);
        emit_storereg(CCREG,HOST_CCREG);
      }
      else{
        cc=get_reg(i_regmap,CCREG);
        assert(cc==HOST_CCREG);
        emit_addimm_and_set_flags(CLOCK_DIVIDER*(ccadj[i]+2),cc);
        intptr_t jaddr=(intptr_t)out;
        emit_jns(0);
        add_stub(CC_STUB,jaddr,(intptr_t)out,0,i,start+i*4+8,likely[i]?NULLDS:NOTTAKEN,0);
      }
    }
  }
}

static void fjump_assemble(int i,struct regstat *i_regs)
{
  signed char *i_regmap=i_regs->regmap;
  int cc;
  int match;
  match=match_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
  assem_debug("fmatch=%d",match);
  int fs,cs;
  intptr_t eaddr;
  int invert=0;
  int branch_internal=internal_branch(branch_regs[i].is32,ba[i]);
  if(i==(ba[i]-start)>>2) assem_debug("idle loop");
  if(!match) invert=1;
  #ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
  if(i>(ba[i]-start)>>2) invert=1;
  #endif

  if(ooo[i]) {
    fs=get_reg(branch_regs[i].regmap,FSREG);
    address_generation(i+1,i_regs,regs[i].regmap_entry); // Is this okay?
  }
  else {
    fs=get_reg(i_regmap,FSREG);
  }

  // Check cop1 unusable
  if(!cop1_usable) {
    cs=get_reg(i_regmap,CSREG);
    assert(cs>=0);
    emit_testimm(cs,CP0_STATUS_CU1);
    eaddr=(intptr_t)out;
    emit_jeq(0);
    add_stub(FP_STUB,eaddr,(intptr_t)out,i,cs,(intptr_t)i_regs,0,0);
    cop1_usable=1;
  }

  if(ooo[i]) {
    // Out of order execution (delay slot first)
    //DebugMessage(M64MSG_VERBOSE, "OOOE");
    ds_assemble(i+1,i_regs);
    int adj;
    uint64_t bc_unneeded=branch_regs[i].u;
    uint64_t bc_unneeded_upper=branch_regs[i].uu;
    bc_unneeded&=~((1LL<<rs1[i])|(1LL<<rs2[i]));
    bc_unneeded_upper&=~((1LL<<us1[i])|(1LL<<us2[i]));
    bc_unneeded|=1;
    bc_unneeded_upper|=1;
    wb_invalidate(regs[i].regmap,branch_regs[i].regmap,regs[i].dirty,regs[i].is32,
                  bc_unneeded,bc_unneeded_upper);
    load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,rs1[i],rs1[i]);
    load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,CCREG,CCREG);
    cc=get_reg(branch_regs[i].regmap,CCREG);
    assert(cc==HOST_CCREG);
    do_cc(i,branch_regs[i].regmap,&adj,-1,0,invert);
    assem_debug("cycle count (adj)");
    if(1) {
      intptr_t nottaken=0;
      if(adj&&!invert) emit_addimm(cc,CLOCK_DIVIDER*(ccadj[i]+2-adj),cc);
      if(1) {
        assert(fs>=0);
        emit_testimm(fs,0x800000);
        if(source[i]&0x10000) // BC1T
        {
          if(invert){
            nottaken=(intptr_t)out;
            emit_jeq(1);
          }else{
            add_to_linker((intptr_t)out,ba[i],branch_internal);
            emit_jne(0);
          }
        }
        else // BC1F
          if(invert){
            nottaken=(intptr_t)out;
            emit_jne(1);
          }else{
            add_to_linker((intptr_t)out,ba[i],branch_internal);
            emit_jeq(0);
          }
        {
        }
      } // if(!only32)

      if(invert) {
        if(adj) emit_addimm(cc,-(int)CLOCK_DIVIDER*adj,cc);
        #ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
        else if(match) emit_addnop(13);
        #endif
        store_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
        load_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
        if(branch_internal)
          assem_debug("branch: internal");
        else
          assem_debug("branch: external");
        if(branch_internal&&is_ds[(ba[i]-start)>>2]) {
          ds_assemble_entry(i);
        }
        else {
          add_to_linker((intptr_t)out,ba[i],branch_internal);
          emit_jmp(0);
        }
        set_jump_target(nottaken,(intptr_t)out);
      }

      if(adj) {
        if(!invert) emit_addimm(cc,CLOCK_DIVIDER*adj,cc);
      }
    } // (!unconditional)
  } // if(ooo)
  else
  {
    // In-order execution (branch first)
    //DebugMessage(M64MSG_VERBOSE, "IOE");
    intptr_t nottaken=0;
    if(1) {
       if(1) {
        assert(fs>=0);
        emit_testimm(fs,0x800000);
        if(source[i]&0x10000) // BC1T
        {
          nottaken=(intptr_t)out;
          emit_jeq(1);
        }
        else // BC1F
        {
          nottaken=(intptr_t)out;
          emit_jne(1);
        }
      }
    } // if(!unconditional)
    int adj;
    uint64_t ds_unneeded=branch_regs[i].u;
    uint64_t ds_unneeded_upper=branch_regs[i].uu;
    ds_unneeded&=~((1LL<<rs1[i+1])|(1LL<<rs2[i+1]));
    ds_unneeded_upper&=~((1LL<<us1[i+1])|(1LL<<us2[i+1]));
    if((~ds_unneeded_upper>>rt1[i+1])&1) ds_unneeded_upper&=~((1LL<<dep1[i+1])|(1LL<<dep2[i+1]));
    ds_unneeded|=1;
    ds_unneeded_upper|=1;
    // branch taken
    //assem_debug("1:");
    wb_invalidate(regs[i].regmap,branch_regs[i].regmap,regs[i].dirty,regs[i].is32,
                  ds_unneeded,ds_unneeded_upper);
    // load regs
    load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,rs1[i+1],rs2[i+1]);
    address_generation(i+1,&branch_regs[i],0);
    load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,CCREG,INVCP);
    ds_assemble(i+1,&branch_regs[i]);
    cc=get_reg(branch_regs[i].regmap,CCREG);
    if(cc==-1) {
      emit_loadreg(CCREG,cc=HOST_CCREG);
      // CHECK: Is the following instruction (fall thru) allocated ok?
    }
    assert(cc==HOST_CCREG);
    store_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
    do_cc(i,i_regmap,&adj,ba[i],TAKEN,0);
    if(i!=(ba[i]-start)>>2 || source[i+1]!=0) {
      assem_debug("cycle count (adj)");
      if(adj) emit_addimm(cc,CLOCK_DIVIDER*(ccadj[i]+2-adj),cc);
      load_regs_bt(branch_regs[i].regmap,branch_regs[i].is32,branch_regs[i].dirty,ba[i]);
      if(branch_internal)
        assem_debug("branch: internal");
      else
        assem_debug("branch: external");
      if(branch_internal&&is_ds[(ba[i]-start)>>2]) {
        ds_assemble_entry(i);
      }
      else {
        add_to_linker((intptr_t)out,ba[i],branch_internal);
        emit_jmp(0);
      }
    }

    // branch not taken
    if(1) { // <- FIXME (don't need this)
      set_jump_target(nottaken,(intptr_t)out);
      assem_debug("1:");
      if(!likely[i]) {
        wb_invalidate(regs[i].regmap,branch_regs[i].regmap,regs[i].dirty,regs[i].is32,
                      ds_unneeded,ds_unneeded_upper);
        load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,rs1[i+1],rs2[i+1]);
        address_generation(i+1,&branch_regs[i],0);
        load_regs(regs[i].regmap,branch_regs[i].regmap,regs[i].was32,CCREG,CCREG);
        ds_assemble(i+1,&branch_regs[i]);
      }
      cc=get_reg(branch_regs[i].regmap,CCREG);
      if(cc==-1&&!likely[i]) {
        // Cycle count isn't in a register, temporarily load it then write it out
        emit_loadreg(CCREG,HOST_CCREG);
        emit_addimm_and_set_flags(CLOCK_DIVIDER*(ccadj[i]+2),HOST_CCREG);
        intptr_t jaddr=(intptr_t)out;
        emit_jns(0);
        add_stub(CC_STUB,jaddr,(intptr_t)out,0,i,start+i*4+8,NOTTAKEN,0);
        emit_storereg(CCREG,HOST_CCREG);
      }
      else{
        cc=get_reg(i_regmap,CCREG);
        assert(cc==HOST_CCREG);
        emit_addimm_and_set_flags(CLOCK_DIVIDER*(ccadj[i]+2),cc);
        intptr_t jaddr=(intptr_t)out;
        emit_jns(0);
        add_stub(CC_STUB,jaddr,(intptr_t)out,0,i,start+i*4+8,likely[i]?NULLDS:NOTTAKEN,0);
      }
    }
  }
}

static void pagespan_assemble(int i,struct regstat *i_regs)
{
  int s1l=get_reg(i_regs->regmap,rs1[i]);
  int s1h=get_reg(i_regs->regmap,rs1[i]|64);
  int s2l=get_reg(i_regs->regmap,rs2[i]);
  int s2h=get_reg(i_regs->regmap,rs2[i]|64);
  intptr_t taken=0;
  intptr_t nottaken=0;
  intptr_t nottaken1=0;
  int unconditional=0;
  assert(!(i==(ba[i]-start)>>2 && source[i+1]==0)); //FIXME: Idle loop
  if(rs1[i]==0)
  {
    s1l=s2l;s1h=s2h;
    s2l=s2h=-1;
  }
  else if(rs2[i]==0)
  {
    s2l=s2h=-1;
  }
  if((i_regs->is32>>rs1[i])&(i_regs->is32>>rs2[i])&1) {
    s1h=s2h=-1;
  }
  int hr=0;
  int addr,alt,ntaddr;
  if(i_regs->regmap[HOST_BTREG]<0) {addr=HOST_BTREG;}
  else {
    while(hr<HOST_REGS)
    {
      if(hr!=EXCLUDE_REG && hr!=HOST_CCREG &&
         (i_regs->regmap[hr]&63)!=rs1[i] &&
         (i_regs->regmap[hr]&63)!=rs2[i] )
      {
        addr=hr++;break;
      }
      hr++;
    }
  }
  while(hr<HOST_REGS)
  {
    if(hr!=EXCLUDE_REG && hr!=HOST_CCREG && hr!=HOST_BTREG &&
       (i_regs->regmap[hr]&63)!=rs1[i] &&
       (i_regs->regmap[hr]&63)!=rs2[i] )
    {
      alt=hr++;break;
    }
    hr++;
  }
  if((opcode[i]&0x2E)==6) // BLEZ/BGTZ needs another register
  {
    while(hr<HOST_REGS)
    {
      if(hr!=EXCLUDE_REG && hr!=HOST_CCREG && hr!=HOST_BTREG &&
         (i_regs->regmap[hr]&63)!=rs1[i] &&
         (i_regs->regmap[hr]&63)!=rs2[i] )
      {
        ntaddr=hr;break;
      }
      hr++;
    }
  }
  assert(hr<HOST_REGS);
  if((opcode[i]&0x2e)==4||opcode[i]==0x11) { // BEQ/BNE/BEQL/BNEL/BC1
    load_regs(regs[i].regmap_entry,regs[i].regmap,regs[i].was32,CCREG,CCREG);
  }
  if(opcode[i]==2) // J
  {
    unconditional=1;
  }
  if(opcode[i]==3) // JAL
  {
    // TODO: mini_ht
    int rt=get_reg(i_regs->regmap,31);
    emit_movimm(start+i*4+8,rt);
    unconditional=1;
  }
  if(opcode[i]==0&&(opcode2[i]&0x3E)==8) // JR/JALR
  {
    emit_mov(s1l,addr);
    if(opcode2[i]==9) // JALR
    {
      int rt=get_reg(i_regs->regmap,rt1[i]);
      emit_movimm(start+i*4+8,rt);
    }
  }
  if((opcode[i]&0x3f)==4) // BEQ
  {
    if(rs1[i]==rs2[i])
    {
      unconditional=1;
    }
    else
    #ifdef HAVE_CMOV_IMM
    if(s1h<0) {
      if(s2l>=0) emit_cmp(s1l,s2l);
      else emit_test(s1l,s1l);
      emit_cmov2imm_e_ne_compact(ba[i],start+i*4+8,addr);
    }
    else
    #endif
    {
      assert(s1l>=0);
      emit_mov2imm_compact(ba[i],addr,start+i*4+8,alt);
      if(s1h>=0) {
        if(s2h>=0) emit_cmp(s1h,s2h);
        else emit_test(s1h,s1h);
        emit_cmovne_reg(alt,addr);
      }
      if(s2l>=0) emit_cmp(s1l,s2l);
      else emit_test(s1l,s1l);
      emit_cmovne_reg(alt,addr);
    }
  }
  if((opcode[i]&0x3f)==5) // BNE
  {
    #ifdef HAVE_CMOV_IMM
    if(s1h<0) {
      if(s2l>=0) emit_cmp(s1l,s2l);
      else emit_test(s1l,s1l);
      emit_cmov2imm_e_ne_compact(start+i*4+8,ba[i],addr);
    }
    else
    #endif
    {
      assert(s1l>=0);
      emit_mov2imm_compact(start+i*4+8,addr,ba[i],alt);
      if(s1h>=0) {
        if(s2h>=0) emit_cmp(s1h,s2h);
        else emit_test(s1h,s1h);
        emit_cmovne_reg(alt,addr);
      }
      if(s2l>=0) emit_cmp(s1l,s2l);
      else emit_test(s1l,s1l);
      emit_cmovne_reg(alt,addr);
    }
  }
  if((opcode[i]&0x3f)==0x14) // BEQL
  {
    if(s1h>=0) {
      if(s2h>=0) emit_cmp(s1h,s2h);
      else emit_test(s1h,s1h);
      nottaken1=(intptr_t)out;
      emit_jne(0);
    }
    if(s2l>=0) emit_cmp(s1l,s2l);
    else emit_test(s1l,s1l);
    nottaken=(intptr_t)out;
    emit_jne(0);
  }
  if((opcode[i]&0x3f)==0x15) // BNEL
  {
    if(s1h>=0) {
      if(s2h>=0) emit_cmp(s1h,s2h);
      else emit_test(s1h,s1h);
      taken=(intptr_t)out;
      emit_jne(0);
    }
    if(s2l>=0) emit_cmp(s1l,s2l);
    else emit_test(s1l,s1l);
    nottaken=(intptr_t)out;
    emit_jeq(0);
    if(taken) set_jump_target(taken,(intptr_t)out);
  }
  if((opcode[i]&0x3f)==6) // BLEZ
  {
    emit_mov2imm_compact(ba[i],alt,start+i*4+8,addr);
    emit_cmpimm(s1l,1);
    if(s1h>=0) emit_mov(addr,ntaddr);
    emit_cmovl_reg(alt,addr);
    if(s1h>=0) {
      emit_test(s1h,s1h);
      emit_cmovne_reg(ntaddr,addr);
      emit_cmovs_reg(alt,addr);
    }
  }
  if((opcode[i]&0x3f)==7) // BGTZ
  {
    emit_mov2imm_compact(ba[i],addr,start+i*4+8,ntaddr);
    emit_cmpimm(s1l,1);
    if(s1h>=0) emit_mov(addr,alt);
    emit_cmovl_reg(ntaddr,addr);
    if(s1h>=0) {
      emit_test(s1h,s1h);
      emit_cmovne_reg(alt,addr);
      emit_cmovs_reg(ntaddr,addr);
    }
  }
  if((opcode[i]&0x3f)==0x16) // BLEZL
  {
    if(s1h>=0) {
      emit_test(s1h,s1h);
      taken=(intptr_t)out;
      emit_js(0);
      nottaken1=(intptr_t)out;
      emit_jne(0);
    }
    emit_cmpimm(s1l,1);
    nottaken=(intptr_t)out;
    if(s1h>=0) emit_jae(0);
    else emit_jge(0);
    if(taken) set_jump_target(taken,(intptr_t)out);
  }
  if((opcode[i]&0x3f)==0x17) // BGTZL
  {
    if(s1h>=0) {
      emit_test(s1h,s1h);
      nottaken1=(intptr_t)out;
      emit_js(0);
      taken=(intptr_t)out;
      emit_jne(0);
    }
    emit_cmpimm(s1l,1);
    nottaken=(intptr_t)out;
    if(s1h>=0) emit_jb(0);
    else emit_jl(0);
    if(taken) set_jump_target(taken,(intptr_t)out);
  }
  if((opcode[i]==1)&&(opcode2[i]==0)) // BLTZ
  {
    emit_mov2imm_compact(ba[i],alt,start+i*4+8,addr);
    if(s1h>=0) emit_test(s1h,s1h);
    else emit_test(s1l,s1l);
    emit_cmovs_reg(alt,addr);
  }
  if((opcode[i]==1)&&(opcode2[i]==1)) // BGEZ
  {
    emit_mov2imm_compact(ba[i],addr,start+i*4+8,alt);
    if(s1h>=0) emit_test(s1h,s1h);
    else emit_test(s1l,s1l);
    emit_cmovs_reg(alt,addr);
  }
  if((opcode[i]==1)&&(opcode2[i]==2)) // BLTZL
  {
    if(s1h>=0) emit_test(s1h,s1h);
    else emit_test(s1l,s1l);
    nottaken=(intptr_t)out;
    emit_jns(0);
  }
  if((opcode[i]==1)&&(opcode2[i]==3)) // BGEZL
  {
    if(s1h>=0) emit_test(s1h,s1h);
    else emit_test(s1l,s1l);
    nottaken=(intptr_t)out;
    emit_js(0);
  }

  if(opcode[i]==0x11 && opcode2[i]==0x08 ) {
    // Check cop1 unusable
    if(!cop1_usable) {
      signed char cs=get_reg(i_regs->regmap,CSREG);
      assert(cs>=0);
      emit_testimm(cs,CP0_STATUS_CU1);
      intptr_t jaddr=(intptr_t)out;
      emit_jeq(0);
      add_stub(FP_STUB,jaddr,(intptr_t)out,i,cs,(intptr_t)i_regs,0,0);
      cop1_usable=1;
    }
    if((source[i]&0x30000)==0) // BC1F
    {
      emit_mov2imm_compact(ba[i],addr,start+i*4+8,alt);
      emit_testimm(s1l,0x800000);
      emit_cmovne_reg(alt,addr);
    }
    if((source[i]&0x30000)==0x10000) // BC1T
    {
      emit_mov2imm_compact(ba[i],alt,start+i*4+8,addr);
      emit_testimm(s1l,0x800000);
      emit_cmovne_reg(alt,addr);
    }
    if((source[i]&0x30000)==0x20000) // BC1FL
    {
      emit_testimm(s1l,0x800000);
      nottaken=(intptr_t)out;
      emit_jne(0);
    }
    if((source[i]&0x30000)==0x30000) // BC1TL
    {
      emit_testimm(s1l,0x800000);
      nottaken=(intptr_t)out;
      emit_jeq(0);
    }
  }

  assert(i_regs->regmap[HOST_CCREG]==CCREG);
  emit_addimm(HOST_CCREG,CLOCK_DIVIDER*(ccadj[i]+1),HOST_CCREG);
  wb_dirtys(regs[i].regmap,regs[i].is32,regs[i].dirty);
  if(likely[i]||unconditional)
  {
    emit_movimm(ba[i],HOST_BTREG);
  }
  else if(addr!=HOST_BTREG)
  {
    emit_mov(addr,HOST_BTREG);
  }
  void *branch_addr=out;
  emit_jmp(0);
  int target_addr=start+i*4+5;
  void *stub=out;
  void *compiled_target_addr=check_addr(target_addr);
  emit_extjump_ds((intptr_t)branch_addr,target_addr);
#ifndef DISABLE_BLOCK_LINKING
  if(compiled_target_addr) {
    set_jump_target((intptr_t)branch_addr,(intptr_t)compiled_target_addr);
    add_link(target_addr,stub);
  }
  else
#endif
    set_jump_target((intptr_t)branch_addr,(intptr_t)stub);
  if(likely[i]) {
    // Not-taken path
    if(nottaken1) set_jump_target((intptr_t)nottaken1,(intptr_t)out);
    set_jump_target((intptr_t)nottaken,(intptr_t)out);
    emit_addimm(HOST_CCREG,CLOCK_DIVIDER*(ccadj[i]+2),HOST_CCREG);
    wb_dirtys(regs[i].regmap,regs[i].is32,regs[i].dirty);
    emit_test(HOST_CCREG,HOST_CCREG);
    intptr_t jaddr=(intptr_t)out;
    emit_js(0);
    emit_movimm(start+i*4+8,0);
    emit_writeword(0,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.pcaddr);
    emit_call((intptr_t)cc_interrupt);
    set_jump_target((intptr_t)jaddr,(intptr_t)out);
    void *branch_addr=out;
    emit_jmp(0);
    int target_addr=start+i*4+8;
    void *stub=out;
    void *compiled_target_addr=check_addr(target_addr);
    emit_extjump_ds((intptr_t)branch_addr,target_addr);
#ifndef DISABLE_BLOCK_LINKING
    if(compiled_target_addr) {
      set_jump_target((intptr_t)branch_addr,(intptr_t)compiled_target_addr);
      add_link(target_addr,stub);
    }
    else
#endif
      set_jump_target((intptr_t)branch_addr,(intptr_t)stub);
  }
}

// Assemble the delay slot for the above
static void pagespan_ds(void)
{
  assem_debug("initial delay slot:");
  u_int vaddr=start+1;
  u_int page=(0x80000000^vaddr)>>12;
  u_int vpage=page;
  if(page>262143&&g_dev.r4300.cp0.tlb.LUT_r[vaddr>>12]) page=(g_dev.r4300.cp0.tlb.LUT_r[page^0x80000]^0x80000000)>>12;
  if(page>2048) page=2048+(page&2047);
  if(vpage>262143&&g_dev.r4300.cp0.tlb.LUT_r[vaddr>>12]) vpage&=2047; // jump_dirty uses a hash of the virtual address instead
  if(vpage>2048) vpage=2048+(vpage&2047);
  struct ll_entry *head=ll_add(jump_dirty+vpage,vaddr,(void *)out,NULL,start,copy,slen*4);
  dirty_entry_count++;
  do_dirty_stub_ds(head);
  head->clean_addr=(void *)out;
  (void)ll_add(jump_in+page,vaddr,(void *)out,(void *)out,start,copy,slen*4);
  assert(regs[0].regmap_entry[HOST_CCREG]==CCREG);
  emit_addimm(HOST_CCREG,CLOCK_DIVIDER,HOST_CCREG);
  if(regs[0].regmap[HOST_CCREG]!=CCREG)
    wb_register(CCREG,regs[0].regmap_entry,regs[0].wasdirty,regs[0].was32);
  if(regs[0].regmap[HOST_BTREG]!=BTREG)
    emit_writeword(HOST_BTREG,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.branch_target);
  load_regs(regs[0].regmap_entry,regs[0].regmap,regs[0].was32,rs1[0],rs2[0]);
  address_generation(0,&regs[0],regs[0].regmap_entry);
  if(itype[0]==LOAD||itype[0]==LOADLR||itype[0]==STORE||itype[0]==STORELR||itype[0]==C1LS)
    load_regs(regs[0].regmap_entry,regs[0].regmap,regs[0].was32,MMREG,ROREG);
  if(itype[0]==STORE||itype[0]==STORELR||(opcode[0]&0x3b)==0x39)
    load_regs(regs[0].regmap_entry,regs[0].regmap,regs[0].was32,INVCP,INVCP);
  cop1_usable=0;
  is_delayslot=0;
  switch(itype[0]) {
    case ALU:
      alu_assemble(0,&regs[0]);break;
    case IMM16:
      imm16_assemble(0,&regs[0]);break;
    case SHIFT:
      shift_assemble(0,&regs[0]);break;
    case SHIFTIMM:
      shiftimm_assemble(0,&regs[0]);break;
    case LOAD:
      load_assemble(0,&regs[0]);break;
    case LOADLR:
      loadlr_assemble(0,&regs[0]);break;
    case STORE:
      store_assemble(0,&regs[0]);break;
    case STORELR:
      storelr_assemble(0,&regs[0]);break;
    case COP0:
      cop0_assemble(0,&regs[0]);break;
    case COP1:
      cop1_assemble(0,&regs[0]);break;
    case C1LS:
      c1ls_assemble(0,&regs[0]);break;
    case FCONV:
      fconv_assemble(0,&regs[0]);break;
    case FLOAT:
      float_assemble(0,&regs[0]);break;
    case FCOMP:
      fcomp_assemble(0,&regs[0]);break;
    case MULTDIV:
      multdiv_assemble(0,&regs[0]);break;
    case MOV:
      mov_assemble(0,&regs[0]);break;
    case SYSCALL:
    case SPAN:
    case UJUMP:
    case RJUMP:
    case CJUMP:
    case SJUMP:
    case FJUMP:
      DebugMessage(M64MSG_VERBOSE, "Jump in the delay slot.  This is probably a bug.");
  }
  int btaddr=get_reg(regs[0].regmap,BTREG);
  if(btaddr<0) {
    btaddr=get_reg(regs[0].regmap,-1);
    emit_readword((intptr_t)&g_dev.r4300.new_dynarec_hot_state.branch_target,btaddr);
  }
  assert(btaddr!=HOST_CCREG);
  if(regs[0].regmap[HOST_CCREG]!=CCREG) emit_loadreg(CCREG,HOST_CCREG);
  emit_test(HOST_CCREG,HOST_CCREG);
  intptr_t jaddr=(intptr_t)out;
  emit_js(0);
  wb_dirtys(regs[0].regmap,regs[0].is32,regs[0].dirty);
  emit_writeword(btaddr,(intptr_t)&g_dev.r4300.new_dynarec_hot_state.pcaddr);
  emit_call((intptr_t)cc_interrupt);
  load_all_regs(regs[0].regmap);
  set_jump_target((intptr_t)jaddr,(intptr_t)out);
#ifdef HOST_IMM8
  emit_movimm(start+4,HOST_TEMPREG);
  emit_cmp(btaddr,HOST_TEMPREG);
#else
  emit_cmpimm(btaddr,start+4);
#endif
  intptr_t branch=(intptr_t)out;
  emit_jeq(0);
  store_regs_bt(regs[0].regmap,regs[0].is32,regs[0].dirty,-1);
#if NEW_DYNAREC==NEW_DYNAREC_ARM64
  if(btaddr==18) {
    // x18 is used for trampoline jumps, move it to another register (x0)
    emit_mov(btaddr,0);
    btaddr=0;
  }
#endif
  emit_jmp(jump_vaddr_reg[btaddr]);
  set_jump_target(branch,(intptr_t)out);
  store_regs_bt(regs[0].regmap,regs[0].is32,regs[0].dirty,start+4);
  load_regs_bt(regs[0].regmap,regs[0].is32,regs[0].dirty,start+4);
}

/**** Recompiler ****/
void new_dynarec_init(void)
{
  DebugMessage(M64MSG_INFO, "Init new dynarec");

#if defined(RECOMPILER_DEBUG) && !defined(RECOMP_DBG)
  recomp_dbg_init();
#endif

#if !defined(RECOMP_DBG)
#if NEW_DYNAREC == NEW_DYNAREC_ARM64

#define FIXED_CACHE_ADDR 1    // Put the dynarec cache at extra_memory address
#define DYNAMIC_CACHE_ADDR 2  // Put the dynarec cache at random address
#define DOUBLE_CACHE_ADDR 3   // Put the dynarec cache at random address with RW address != RX address

// Default to fixed cache address
#define CACHE_ADDR FIXED_CACHE_ADDR

#if CACHE_ADDR==DOUBLE_CACHE_ADDR
  #include <unistd.h>
  #include <sys/types.h>
  #include <fcntl.h>

  int fd = shm_open("/new_dynarec", O_RDWR | O_CREAT | O_EXCL, 0600);
  assert(fd!=-1);
  shm_unlink("/new_dynarec");
  ftruncate(fd, 1<<TARGET_SIZE_2);
  base_addr = mmap((u_char *)g_dev.r4300.extra_memory, 1<<TARGET_SIZE_2,
                 PROT_READ | PROT_WRITE,
                 MAP_FIXED | MAP_SHARED, fd, 0);

  assert(base_addr!=(void*)-1);

  base_addr_rx = mmap(NULL, 1<<TARGET_SIZE_2,
                 PROT_READ | PROT_EXEC,
                 MAP_SHARED, fd, 0);

  assert(base_addr_rx!=(void*)-1);
  close(fd);
#elif CACHE_ADDR==FIXED_CACHE_ADDR
  mprotect ((u_char *)g_dev.r4300.extra_memory, 1<<TARGET_SIZE_2,
            PROT_READ | PROT_WRITE | PROT_EXEC);
  base_addr = g_dev.r4300.extra_memory;
  base_addr_rx = base_addr;
#else /*DYNAMIC_CACHE_ADDR*/
  base_addr = mmap (NULL, 1<<TARGET_SIZE_2,
                    PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS,
                    -1, 0);
  base_addr_rx = base_addr;
#endif
#elif NEW_DYNAREC == NEW_DYNAREC_ARM
  mprotect ((u_char *)g_dev.r4300.extra_memory, 1<<TARGET_SIZE_2,
            PROT_READ | PROT_WRITE | PROT_EXEC);
  base_addr = g_dev.r4300.extra_memory;
  base_addr_rx = base_addr;
#else
#if defined(WIN32)
  DWORD dummy;
  BOOL res=VirtualProtect((void*)g_dev.r4300.extra_memory, 33554432, PAGE_EXECUTE_READWRITE, &dummy);
  assert(res!=0);
  base_addr = base_addr_rx = (void*)g_dev.r4300.extra_memory;
#else
  mprotect ((u_char *)g_dev.r4300.extra_memory, 1<<TARGET_SIZE_2,
            PROT_READ | PROT_WRITE | PROT_EXEC);
  base_addr = g_dev.r4300.extra_memory;
  base_addr_rx = base_addr;
#endif
#endif
#endif

  if(base_addr==(void*)-1) DebugMessage(M64MSG_ERROR, "mmap() failed");

  assert(((uintptr_t)g_dev.rdram.dram&7)==0); //8 bytes aligned
  out=(u_char *)base_addr;

  g_dev.r4300.new_dynarec_hot_state.pc = &g_dev.r4300.new_dynarec_hot_state.fake_pc;
  g_dev.r4300.new_dynarec_hot_state.fake_pc.f.r.rs = &g_dev.r4300.new_dynarec_hot_state.rs;
  g_dev.r4300.new_dynarec_hot_state.fake_pc.f.r.rt = &g_dev.r4300.new_dynarec_hot_state.rt;
  g_dev.r4300.new_dynarec_hot_state.fake_pc.f.r.rd = &g_dev.r4300.new_dynarec_hot_state.rd;
  int n;
  for(n=0x80000;n<0x80800;n++)
    g_dev.r4300.cached_interp.invalid_code[n]=1;
  for(n=0;n<65536;n++)
    hash_table[n][0]=hash_table[n][1]=NULL;
  memset(g_dev.r4300.new_dynarec_hot_state.mini_ht,-1,sizeof(g_dev.r4300.new_dynarec_hot_state.mini_ht));
  memset(restore_candidate,0,sizeof(restore_candidate));
  copy_size=0;
  expirep=16384; // Expiry pointer, +2 blocks
  g_dev.r4300.new_dynarec_hot_state.pending_exception=0;
  literalcount=0;
#if defined(HOST_IMM8) || defined(NEED_INVC_PTR)
  // Copy this into local area so we don't have to put it in every literal pool
  g_dev.r4300.new_dynarec_hot_state.invc_ptr=g_dev.r4300.cached_interp.invalid_code;
#endif
  stop_after_jal=0;
  // TLB
  using_tlb=0;
  for(n=0;n<524288;n++) // 0 .. 0x7FFFFFFF
    g_dev.r4300.new_dynarec_hot_state.memory_map[n]=(uintptr_t)-1;
  for(n=524288;n<526336;n++) // 0x80000000 .. 0x807FFFFF
    g_dev.r4300.new_dynarec_hot_state.memory_map[n]=((uintptr_t)g_dev.rdram.dram-(uintptr_t)0x80000000)>>2;
  for(n=526336;n<1048576;n++) // 0x80800000 .. 0xFFFFFFFF
    g_dev.r4300.new_dynarec_hot_state.memory_map[n]=(uintptr_t)-1;

  tlb_speed_hacks();
  arch_init();
}

void new_dynarec_cleanup(void)
{
#if defined(RECOMPILER_DEBUG) && !defined(RECOMP_DBG)
  recomp_dbg_cleanup();
#endif

  int n;
  for(n=0;n<4096;n++) ll_clear(jump_in+n);
  for(n=0;n<4096;n++) ll_clear(jump_out+n);
  for(n=0;n<4096;n++) ll_clear(jump_dirty+n);
  assert(copy_size==0);
#if !defined(RECOMP_DBG)
  #if defined(WIN32)
    VirtualFree(base_addr, 0, MEM_RELEASE);
  #elif NEW_DYNAREC == NEW_DYNAREC_ARM64 && CACHE_ADDR!=FIXED_CACHE_ADDR
    if (munmap (base_addr_rx, 1<<TARGET_SIZE_2) < 0) {DebugMessage(M64MSG_ERROR, "munmap() failed");}
  #else
    mprotect(base_addr, 1<<TARGET_SIZE_2, PROT_READ | PROT_WRITE);
  #endif
#endif
#ifdef ROM_COPY
  if (munmap (ROM_COPY, 67108864) < 0) {DebugMessage(M64MSG_ERROR, "munmap() failed");}
#endif
}

int new_recompile_block(int addr)
{
#if defined(RECOMPILER_DEBUG) && !defined(RECOMP_DBG)
  recomp_dbg_block(addr);
#endif

  assem_debug("NOTCOMPILED: addr = %x -> %x", (int)addr, (intptr_t)out);
#if COUNT_NOTCOMPILEDS
  notcompiledCount++;
  DebugMessage(M64MSG_VERBOSE, "notcompiledCount=%i", notcompiledCount );
#endif
  start = (u_int)addr&~3;
  //assert(((u_int)addr&1)==0);
  if ((int)addr >= 0xa0000000 && (int)addr < 0xa07fffff) {
    source = (u_int *)((uintptr_t)g_dev.rdram.dram+start-0xa0000000);
    pagelimit = 0xa07fffff;
  }
  else if ((int)addr >= 0xa4000000 && (int)addr < 0xa4001000) {
    source = (u_int *)((uintptr_t)g_dev.sp.mem+start-0xa4000000);
    pagelimit = 0xa4001000;
  }
  else if ((int)addr >= 0x80000000 && (int)addr < 0x80800000) {
    source = (u_int *)((uintptr_t)g_dev.rdram.dram+start-(uintptr_t)0x80000000);
    pagelimit = 0x80800000;
  }
  else if ((signed int)addr >= (signed int)0xC0000000) {
    //DebugMessage(M64MSG_VERBOSE, "addr=%x mm=%x",(u_int)addr,(g_dev.r4300.new_dynarec_hot_state.memory_map[start>>12]<<2));
    //if(g_dev.r4300.cp0.tlb.LUT_r[start>>12])
    //source = (u_int *)(((intptr_t)g_dev.rdram.dram)+(g_dev.r4300.cp0.tlb.LUT_r[start>>12]&0xFFFFF000)+(((int)addr)&0xFFF)-(intptr_t)0x80000000);
    if((intptr_t)g_dev.r4300.new_dynarec_hot_state.memory_map[start>>12]>=0) {
      source = (u_int *)((uintptr_t)(start+(uintptr_t)(g_dev.r4300.new_dynarec_hot_state.memory_map[start>>12]<<2)));
      pagelimit=(start+4096)&0xFFFFF000;
      intptr_t map=g_dev.r4300.new_dynarec_hot_state.memory_map[start>>12];
      int i;
      for(i=0;i<5;i++) {
        //DebugMessage(M64MSG_VERBOSE, "start: %x next: %x",map,g_dev.r4300.new_dynarec_hot_state.memory_map[pagelimit>>12]);
        if((map&~WRITE_PROTECT)==(g_dev.r4300.new_dynarec_hot_state.memory_map[pagelimit>>12]&~WRITE_PROTECT)) pagelimit+=4096;
      }
      assem_debug("pagelimit=%x",pagelimit);
      assem_debug("mapping=%x (%x)",g_dev.r4300.new_dynarec_hot_state.memory_map[start>>12],(uintptr_t)(g_dev.r4300.new_dynarec_hot_state.memory_map[start>>12]<<2)+start);
    }
    else {
      assem_debug("Compile at unmapped memory address: %x ", (int)addr);
      //assem_debug("start: %x next: %x",g_dev.r4300.new_dynarec_hot_state.memory_map[start>>12],g_dev.r4300.new_dynarec_hot_state.memory_map[(start+4096)>>12]);
      return 1; // Caller will invoke exception handler
    }
    //DebugMessage(M64MSG_VERBOSE, "source= %x",(intptr_t)source);
  }
  else {
    //DebugMessage(M64MSG_VERBOSE, "Compile at bogus memory address: %x ", (int)addr);
    DebugMessage(M64MSG_ERROR, "Compile at bogus memory address: %x", (int)addr);
    exit(1);
  }

  /* Pass 1: disassemble */
  /* Pass 2: register dependencies, branch targets */
  /* Pass 3: register allocation */
  /* Pass 4: branch dependencies */
  /* Pass 5: pre-alloc */
  /* Pass 6: optimize clean/dirty state */
  /* Pass 7: flag 32-bit registers */
  /* Pass 8: assembly */
  /* Pass 9: linker */
  /* Pass 10: garbage collection / free memory */

  int i,j;
  int done=0;
  unsigned int type,op,op2;

  //DebugMessage(M64MSG_VERBOSE, "addr = %x source = %x %x", addr,source,source[0]);

  /* Pass 1 disassembly */

  for(i=0;!done;i++) {
    bt[i]=0;likely[i]=0;ooo[i]=0;op2=0;
    minimum_free_regs[i]=0;
    opcode[i]=op=source[i]>>26;
    switch(op)
    {
      case 0x00: assem_strcpy(insn[i],"special"); type=NI;
        op2=source[i]&0x3f;
        switch(op2)
        {
          case 0x00: assem_strcpy(insn[i],"SLL"); type=SHIFTIMM; break;
          case 0x02: assem_strcpy(insn[i],"SRL"); type=SHIFTIMM; break;
          case 0x03: assem_strcpy(insn[i],"SRA"); type=SHIFTIMM; break;
          case 0x04: assem_strcpy(insn[i],"SLLV"); type=SHIFT; break;
          case 0x06: assem_strcpy(insn[i],"SRLV"); type=SHIFT; break;
          case 0x07: assem_strcpy(insn[i],"SRAV"); type=SHIFT; break;
          case 0x08: assem_strcpy(insn[i],"JR"); type=RJUMP; break;
          case 0x09: assem_strcpy(insn[i],"JALR"); type=RJUMP; break;
          case 0x0C: assem_strcpy(insn[i],"SYSCALL"); type=SYSCALL; break;
          case 0x0D: assem_strcpy(insn[i],"BREAK"); type=OTHER; break;
          case 0x0F: assem_strcpy(insn[i],"SYNC"); type=OTHER; break;
          case 0x10: assem_strcpy(insn[i],"MFHI"); type=MOV; break;
          case 0x11: assem_strcpy(insn[i],"MTHI"); type=MOV; break;
          case 0x12: assem_strcpy(insn[i],"MFLO"); type=MOV; break;
          case 0x13: assem_strcpy(insn[i],"MTLO"); type=MOV; break;
          case 0x14: assem_strcpy(insn[i],"DSLLV"); type=SHIFT; break;
          case 0x16: assem_strcpy(insn[i],"DSRLV"); type=SHIFT; break;
          case 0x17: assem_strcpy(insn[i],"DSRAV"); type=SHIFT; break;
          case 0x18: assem_strcpy(insn[i],"MULT"); type=MULTDIV; break;
          case 0x19: assem_strcpy(insn[i],"MULTU"); type=MULTDIV; break;
          case 0x1A: assem_strcpy(insn[i],"DIV"); type=MULTDIV; break;
          case 0x1B: assem_strcpy(insn[i],"DIVU"); type=MULTDIV; break;
          case 0x1C: assem_strcpy(insn[i],"DMULT"); type=MULTDIV; break;
          case 0x1D: assem_strcpy(insn[i],"DMULTU"); type=MULTDIV; break;
          case 0x1E: assem_strcpy(insn[i],"DDIV"); type=MULTDIV; break;
          case 0x1F: assem_strcpy(insn[i],"DDIVU"); type=MULTDIV; break;
          case 0x20: assem_strcpy(insn[i],"ADD"); type=ALU; break;
          case 0x21: assem_strcpy(insn[i],"ADDU"); type=ALU; break;
          case 0x22: assem_strcpy(insn[i],"SUB"); type=ALU; break;
          case 0x23: assem_strcpy(insn[i],"SUBU"); type=ALU; break;
          case 0x24: assem_strcpy(insn[i],"AND"); type=ALU; break;
          case 0x25: assem_strcpy(insn[i],"OR"); type=ALU; break;
          case 0x26: assem_strcpy(insn[i],"XOR"); type=ALU; break;
          case 0x27: assem_strcpy(insn[i],"NOR"); type=ALU; break;
          case 0x2A: assem_strcpy(insn[i],"SLT"); type=ALU; break;
          case 0x2B: assem_strcpy(insn[i],"SLTU"); type=ALU; break;
          case 0x2C: assem_strcpy(insn[i],"DADD"); type=ALU; break;
          case 0x2D: assem_strcpy(insn[i],"DADDU"); type=ALU; break;
          case 0x2E: assem_strcpy(insn[i],"DSUB"); type=ALU; break;
          case 0x2F: assem_strcpy(insn[i],"DSUBU"); type=ALU; break;
          case 0x30: assem_strcpy(insn[i],"TGE"); type=NI; break;
          case 0x31: assem_strcpy(insn[i],"TGEU"); type=NI; break;
          case 0x32: assem_strcpy(insn[i],"TLT"); type=NI; break;
          case 0x33: assem_strcpy(insn[i],"TLTU"); type=NI; break;
          case 0x34: assem_strcpy(insn[i],"TEQ"); type=NI; break;
          case 0x36: assem_strcpy(insn[i],"TNE"); type=NI; break;
          case 0x38: assem_strcpy(insn[i],"DSLL"); type=SHIFTIMM; break;
          case 0x3A: assem_strcpy(insn[i],"DSRL"); type=SHIFTIMM; break;
          case 0x3B: assem_strcpy(insn[i],"DSRA"); type=SHIFTIMM; break;
          case 0x3C: assem_strcpy(insn[i],"DSLL32"); type=SHIFTIMM; break;
          case 0x3E: assem_strcpy(insn[i],"DSRL32"); type=SHIFTIMM; break;
          case 0x3F: assem_strcpy(insn[i],"DSRA32"); type=SHIFTIMM; break;
        }
        break;
      case 0x01: assem_strcpy(insn[i],"regimm"); type=NI;
        op2=(source[i]>>16)&0x1f;
        switch(op2)
        {
          case 0x00: assem_strcpy(insn[i],"BLTZ"); type=SJUMP; break;
          case 0x01: assem_strcpy(insn[i],"BGEZ"); type=SJUMP; break;
          case 0x02: assem_strcpy(insn[i],"BLTZL"); type=SJUMP; break;
          case 0x03: assem_strcpy(insn[i],"BGEZL"); type=SJUMP; break;
          case 0x08: assem_strcpy(insn[i],"TGEI"); type=NI; break;
          case 0x09: assem_strcpy(insn[i],"TGEIU"); type=NI; break;
          case 0x0A: assem_strcpy(insn[i],"TLTI"); type=NI; break;
          case 0x0B: assem_strcpy(insn[i],"TLTIU"); type=NI; break;
          case 0x0C: assem_strcpy(insn[i],"TEQI"); type=NI; break;
          case 0x0E: assem_strcpy(insn[i],"TNEI"); type=NI; break;
          case 0x10: assem_strcpy(insn[i],"BLTZAL"); type=SJUMP; break;
          case 0x11: assem_strcpy(insn[i],"BGEZAL"); type=SJUMP; break;
          case 0x12: assem_strcpy(insn[i],"BLTZALL"); type=SJUMP; break;
          case 0x13: assem_strcpy(insn[i],"BGEZALL"); type=SJUMP; break;
        }
        break;
      case 0x02: assem_strcpy(insn[i],"J"); type=UJUMP; break;
      case 0x03: assem_strcpy(insn[i],"JAL"); type=UJUMP; break;
      case 0x04: assem_strcpy(insn[i],"BEQ"); type=CJUMP; break;
      case 0x05: assem_strcpy(insn[i],"BNE"); type=CJUMP; break;
      case 0x06: assem_strcpy(insn[i],"BLEZ"); type=CJUMP; break;
      case 0x07: assem_strcpy(insn[i],"BGTZ"); type=CJUMP; break;
      case 0x08: assem_strcpy(insn[i],"ADDI"); type=IMM16; break;
      case 0x09: assem_strcpy(insn[i],"ADDIU"); type=IMM16; break;
      case 0x0A: assem_strcpy(insn[i],"SLTI"); type=IMM16; break;
      case 0x0B: assem_strcpy(insn[i],"SLTIU"); type=IMM16; break;
      case 0x0C: assem_strcpy(insn[i],"ANDI"); type=IMM16; break;
      case 0x0D: assem_strcpy(insn[i],"ORI"); type=IMM16; break;
      case 0x0E: assem_strcpy(insn[i],"XORI"); type=IMM16; break;
      case 0x0F: assem_strcpy(insn[i],"LUI"); type=IMM16; break;
      case 0x10: assem_strcpy(insn[i],"cop0"); type=NI;
        op2=(source[i]>>21)&0x1f;
        switch(op2)
        {
          case 0x00: assem_strcpy(insn[i],"MFC0"); type=COP0; break;
          case 0x04: assem_strcpy(insn[i],"MTC0"); type=COP0; break;
          case 0x10: assem_strcpy(insn[i],"tlb"); type=NI;
          switch(source[i]&0x3f)
          {
            case 0x01: assem_strcpy(insn[i],"TLBR"); type=COP0; break;
            case 0x02: assem_strcpy(insn[i],"TLBWI"); type=COP0; break;
            case 0x06: assem_strcpy(insn[i],"TLBWR"); type=COP0; break;
            case 0x08: assem_strcpy(insn[i],"TLBP"); type=COP0; break;
            case 0x18: assem_strcpy(insn[i],"ERET"); type=COP0; break;
          }
        }
        break;
      case 0x11: assem_strcpy(insn[i],"cop1"); type=NI;
        op2=(source[i]>>21)&0x1f;
        switch(op2)
        {
          case 0x00: assem_strcpy(insn[i],"MFC1"); type=COP1; break;
          case 0x01: assem_strcpy(insn[i],"DMFC1"); type=COP1; break;
          case 0x02: assem_strcpy(insn[i],"CFC1"); type=COP1; break;
          case 0x04: assem_strcpy(insn[i],"MTC1"); type=COP1; break;
          case 0x05: assem_strcpy(insn[i],"DMTC1"); type=COP1; break;
          case 0x06: assem_strcpy(insn[i],"CTC1"); type=COP1; break;
          case 0x08: assem_strcpy(insn[i],"BC1"); type=FJUMP;
          switch((source[i]>>16)&0x3)
          {
            case 0x00: assem_strcpy(insn[i],"BC1F"); break;
            case 0x01: assem_strcpy(insn[i],"BC1T"); break;
            case 0x02: assem_strcpy(insn[i],"BC1FL"); break;
            case 0x03: assem_strcpy(insn[i],"BC1TL"); break;
          }
          break;
          case 0x10: assem_strcpy(insn[i],"C1.S"); type=NI;
          switch(source[i]&0x3f)
          {
            case 0x00: assem_strcpy(insn[i],"ADD.S"); type=FLOAT; break;
            case 0x01: assem_strcpy(insn[i],"SUB.S"); type=FLOAT; break;
            case 0x02: assem_strcpy(insn[i],"MUL.S"); type=FLOAT; break;
            case 0x03: assem_strcpy(insn[i],"DIV.S"); type=FLOAT; break;
            case 0x04: assem_strcpy(insn[i],"SQRT.S"); type=FLOAT; break;
            case 0x05: assem_strcpy(insn[i],"ABS.S"); type=FLOAT; break;
            case 0x06: assem_strcpy(insn[i],"MOV.S"); type=FLOAT; break;
            case 0x07: assem_strcpy(insn[i],"NEG.S"); type=FLOAT; break;
            case 0x08: assem_strcpy(insn[i],"ROUND.L.S"); type=FCONV; break;
            case 0x09: assem_strcpy(insn[i],"TRUNC.L.S"); type=FCONV; break;
            case 0x0A: assem_strcpy(insn[i],"CEIL.L.S"); type=FCONV; break;
            case 0x0B: assem_strcpy(insn[i],"FLOOR.L.S"); type=FCONV; break;
            case 0x0C: assem_strcpy(insn[i],"ROUND.W.S"); type=FCONV; break;
            case 0x0D: assem_strcpy(insn[i],"TRUNC.W.S"); type=FCONV; break;
            case 0x0E: assem_strcpy(insn[i],"CEIL.W.S"); type=FCONV; break;
            case 0x0F: assem_strcpy(insn[i],"FLOOR.W.S"); type=FCONV; break;
            case 0x21: assem_strcpy(insn[i],"CVT.D.S"); type=FCONV; break;
            case 0x24: assem_strcpy(insn[i],"CVT.W.S"); type=FCONV; break;
            case 0x25: assem_strcpy(insn[i],"CVT.L.S"); type=FCONV; break;
            case 0x30: assem_strcpy(insn[i],"C.F.S"); type=FCOMP; break;
            case 0x31: assem_strcpy(insn[i],"C.UN.S"); type=FCOMP; break;
            case 0x32: assem_strcpy(insn[i],"C.EQ.S"); type=FCOMP; break;
            case 0x33: assem_strcpy(insn[i],"C.UEQ.S"); type=FCOMP; break;
            case 0x34: assem_strcpy(insn[i],"C.OLT.S"); type=FCOMP; break;
            case 0x35: assem_strcpy(insn[i],"C.ULT.S"); type=FCOMP; break;
            case 0x36: assem_strcpy(insn[i],"C.OLE.S"); type=FCOMP; break;
            case 0x37: assem_strcpy(insn[i],"C.ULE.S"); type=FCOMP; break;
            case 0x38: assem_strcpy(insn[i],"C.SF.S"); type=FCOMP; break;
            case 0x39: assem_strcpy(insn[i],"C.NGLE.S"); type=FCOMP; break;
            case 0x3A: assem_strcpy(insn[i],"C.SEQ.S"); type=FCOMP; break;
            case 0x3B: assem_strcpy(insn[i],"C.NGL.S"); type=FCOMP; break;
            case 0x3C: assem_strcpy(insn[i],"C.LT.S"); type=FCOMP; break;
            case 0x3D: assem_strcpy(insn[i],"C.NGE.S"); type=FCOMP; break;
            case 0x3E: assem_strcpy(insn[i],"C.LE.S"); type=FCOMP; break;
            case 0x3F: assem_strcpy(insn[i],"C.NGT.S"); type=FCOMP; break;
          }
          break;
          case 0x11: assem_strcpy(insn[i],"C1.D"); type=NI;
          switch(source[i]&0x3f)
          {
            case 0x00: assem_strcpy(insn[i],"ADD.D"); type=FLOAT; break;
            case 0x01: assem_strcpy(insn[i],"SUB.D"); type=FLOAT; break;
            case 0x02: assem_strcpy(insn[i],"MUL.D"); type=FLOAT; break;
            case 0x03: assem_strcpy(insn[i],"DIV.D"); type=FLOAT; break;
            case 0x04: assem_strcpy(insn[i],"SQRT.D"); type=FLOAT; break;
            case 0x05: assem_strcpy(insn[i],"ABS.D"); type=FLOAT; break;
            case 0x06: assem_strcpy(insn[i],"MOV.D"); type=FLOAT; break;
            case 0x07: assem_strcpy(insn[i],"NEG.D"); type=FLOAT; break;
            case 0x08: assem_strcpy(insn[i],"ROUND.L.D"); type=FCONV; break;
            case 0x09: assem_strcpy(insn[i],"TRUNC.L.D"); type=FCONV; break;
            case 0x0A: assem_strcpy(insn[i],"CEIL.L.D"); type=FCONV; break;
            case 0x0B: assem_strcpy(insn[i],"FLOOR.L.D"); type=FCONV; break;
            case 0x0C: assem_strcpy(insn[i],"ROUND.W.D"); type=FCONV; break;
            case 0x0D: assem_strcpy(insn[i],"TRUNC.W.D"); type=FCONV; break;
            case 0x0E: assem_strcpy(insn[i],"CEIL.W.D"); type=FCONV; break;
            case 0x0F: assem_strcpy(insn[i],"FLOOR.W.D"); type=FCONV; break;
            case 0x20: assem_strcpy(insn[i],"CVT.S.D"); type=FCONV; break;
            case 0x24: assem_strcpy(insn[i],"CVT.W.D"); type=FCONV; break;
            case 0x25: assem_strcpy(insn[i],"CVT.L.D"); type=FCONV; break;
            case 0x30: assem_strcpy(insn[i],"C.F.D"); type=FCOMP; break;
            case 0x31: assem_strcpy(insn[i],"C.UN.D"); type=FCOMP; break;
            case 0x32: assem_strcpy(insn[i],"C.EQ.D"); type=FCOMP; break;
            case 0x33: assem_strcpy(insn[i],"C.UEQ.D"); type=FCOMP; break;
            case 0x34: assem_strcpy(insn[i],"C.OLT.D"); type=FCOMP; break;
            case 0x35: assem_strcpy(insn[i],"C.ULT.D"); type=FCOMP; break;
            case 0x36: assem_strcpy(insn[i],"C.OLE.D"); type=FCOMP; break;
            case 0x37: assem_strcpy(insn[i],"C.ULE.D"); type=FCOMP; break;
            case 0x38: assem_strcpy(insn[i],"C.SF.D"); type=FCOMP; break;
            case 0x39: assem_strcpy(insn[i],"C.NGLE.D"); type=FCOMP; break;
            case 0x3A: assem_strcpy(insn[i],"C.SEQ.D"); type=FCOMP; break;
            case 0x3B: assem_strcpy(insn[i],"C.NGL.D"); type=FCOMP; break;
            case 0x3C: assem_strcpy(insn[i],"C.LT.D"); type=FCOMP; break;
            case 0x3D: assem_strcpy(insn[i],"C.NGE.D"); type=FCOMP; break;
            case 0x3E: assem_strcpy(insn[i],"C.LE.D"); type=FCOMP; break;
            case 0x3F: assem_strcpy(insn[i],"C.NGT.D"); type=FCOMP; break;
          }
          break;
          case 0x14: assem_strcpy(insn[i],"C1.W"); type=NI;
          switch(source[i]&0x3f)
          {
            case 0x20: assem_strcpy(insn[i],"CVT.S.W"); type=FCONV; break;
            case 0x21: assem_strcpy(insn[i],"CVT.D.W"); type=FCONV; break;
          }
          break;
          case 0x15: assem_strcpy(insn[i],"C1.L"); type=NI;
          switch(source[i]&0x3f)
          {
            case 0x20: assem_strcpy(insn[i],"CVT.S.L"); type=FCONV; break;
            case 0x21: assem_strcpy(insn[i],"CVT.D.L"); type=FCONV; break;
          }
          break;
        }
        break;
      case 0x14: assem_strcpy(insn[i],"BEQL"); type=CJUMP; break;
      case 0x15: assem_strcpy(insn[i],"BNEL"); type=CJUMP; break;
      case 0x16: assem_strcpy(insn[i],"BLEZL"); type=CJUMP; break;
      case 0x17: assem_strcpy(insn[i],"BGTZL"); type=CJUMP; break;
      case 0x18: assem_strcpy(insn[i],"DADDI"); type=IMM16; break;
      case 0x19: assem_strcpy(insn[i],"DADDIU"); type=IMM16; break;
      case 0x1A: assem_strcpy(insn[i],"LDL"); type=LOADLR; break;
      case 0x1B: assem_strcpy(insn[i],"LDR"); type=LOADLR; break;
      case 0x20: assem_strcpy(insn[i],"LB"); type=LOAD; break;
      case 0x21: assem_strcpy(insn[i],"LH"); type=LOAD; break;
      case 0x22: assem_strcpy(insn[i],"LWL"); type=LOADLR; break;
      case 0x23: assem_strcpy(insn[i],"LW"); type=LOAD; break;
      case 0x24: assem_strcpy(insn[i],"LBU"); type=LOAD; break;
      case 0x25: assem_strcpy(insn[i],"LHU"); type=LOAD; break;
      case 0x26: assem_strcpy(insn[i],"LWR"); type=LOADLR; break;
      case 0x27: assem_strcpy(insn[i],"LWU"); type=LOAD; break;
      case 0x28: assem_strcpy(insn[i],"SB"); type=STORE; break;
      case 0x29: assem_strcpy(insn[i],"SH"); type=STORE; break;
      case 0x2A: assem_strcpy(insn[i],"SWL"); type=STORELR; break;
      case 0x2B: assem_strcpy(insn[i],"SW"); type=STORE; break;
      case 0x2C: assem_strcpy(insn[i],"SDL"); type=STORELR; break;
      case 0x2D: assem_strcpy(insn[i],"SDR"); type=STORELR; break;
      case 0x2E: assem_strcpy(insn[i],"SWR"); type=STORELR; break;
      case 0x2F: assem_strcpy(insn[i],"CACHE"); type=NOP; break;
      case 0x30: assem_strcpy(insn[i],"LL"); type=NI; break;
      case 0x31: assem_strcpy(insn[i],"LWC1"); type=C1LS; break;
      case 0x34: assem_strcpy(insn[i],"LLD"); type=NI; break;
      case 0x35: assem_strcpy(insn[i],"LDC1"); type=C1LS; break;
      case 0x37: assem_strcpy(insn[i],"LD"); type=LOAD; break;
      case 0x38: assem_strcpy(insn[i],"SC"); type=NI; break;
      case 0x39: assem_strcpy(insn[i],"SWC1"); type=C1LS; break;
      case 0x3C: assem_strcpy(insn[i],"SCD"); type=NI; break;
      case 0x3D: assem_strcpy(insn[i],"SDC1"); type=C1LS; break;
      case 0x3F: assem_strcpy(insn[i],"SD"); type=STORE; break;
      default: assem_strcpy(insn[i],"???"); type=NI; break;
    }
    itype[i]=type;
    opcode2[i]=op2;
    /* Get registers/immediates */
    lt1[i]=0;
    us1[i]=0;
    us2[i]=0;
    dep1[i]=0;
    dep2[i]=0;
    switch(type) {
      case LOAD:
        rs1[i]=(source[i]>>21)&0x1f;
        rs2[i]=0;
        rt1[i]=(source[i]>>16)&0x1f;
        rt2[i]=0;
        imm[i]=(short)source[i];
        break;
      case STORE:
      case STORELR:
        rs1[i]=(source[i]>>21)&0x1f;
        rs2[i]=(source[i]>>16)&0x1f;
        rt1[i]=0;
        rt2[i]=0;
        imm[i]=(short)source[i];
        if(op==0x2c||op==0x2d||op==0x3f) us1[i]=rs2[i]; // 64-bit SDL/SDR/SD
        break;
      case LOADLR:
        // LWL/LWR only load part of the register,
        // therefore the target register must be treated as a source too
        rs1[i]=(source[i]>>21)&0x1f;
        rs2[i]=(source[i]>>16)&0x1f;
        rt1[i]=(source[i]>>16)&0x1f;
        rt2[i]=0;
        imm[i]=(short)source[i];
        if(op==0x1a||op==0x1b) us1[i]=rs2[i]; // LDR/LDL
        if(op==0x26) dep1[i]=rt1[i]; // LWR
        break;
      case IMM16:
        if (op==0x0f) rs1[i]=0; // LUI instruction has no source register
        else rs1[i]=(source[i]>>21)&0x1f;
        rs2[i]=0;
        rt1[i]=(source[i]>>16)&0x1f;
        rt2[i]=0;
        if(op>=0x0c&&op<=0x0e) { // ANDI/ORI/XORI
          imm[i]=(unsigned short)source[i];
        }else{
          imm[i]=(short)source[i];
        }
        if(op==0x18||op==0x19) us1[i]=rs1[i]; // DADDI/DADDIU
        if(op==0x0a||op==0x0b) us1[i]=rs1[i]; // SLTI/SLTIU
        if(op==0x0d||op==0x0e) dep1[i]=rs1[i]; // ORI/XORI
        break;
      case UJUMP:
        rs1[i]=0;
        rs2[i]=0;
        rt1[i]=0;
        rt2[i]=0;
        // The JAL instruction writes to r31.
        if (op&1) {
          rt1[i]=31;
        }
        rs2[i]=CCREG;
        break;
      case RJUMP:
        rs1[i]=(source[i]>>21)&0x1f;
        rs2[i]=0;
        rt1[i]=0;
        rt2[i]=0;
        // The JALR instruction writes to rd.
        if (op2&1) {
          rt1[i]=(source[i]>>11)&0x1f;
        }
        rs2[i]=CCREG;
        break;
      case CJUMP:
        rs1[i]=(source[i]>>21)&0x1f;
        rs2[i]=(source[i]>>16)&0x1f;
        rt1[i]=0;
        rt2[i]=0;
        if(op&2) { // BGTZ/BLEZ
          rs2[i]=0;
        }
        us1[i]=rs1[i];
        us2[i]=rs2[i];
        likely[i]=op>>4;
        break;
      case SJUMP:
        rs1[i]=(source[i]>>21)&0x1f;
        rs2[i]=CCREG;
        rt1[i]=0;
        rt2[i]=0;
        us1[i]=rs1[i];
        if(op2&0x10) { // BxxAL
          rt1[i]=31;
          // NOTE: If the branch is not taken, r31 is still overwritten
        }
        likely[i]=(op2&2)>>1;
        break;
      case FJUMP:
        rs1[i]=FSREG;
        rs2[i]=CSREG;
        rt1[i]=0;
        rt2[i]=0;
        likely[i]=((source[i])>>17)&1;
        break;
      case ALU:
        rs1[i]=(source[i]>>21)&0x1f; // source
        rs2[i]=(source[i]>>16)&0x1f; // subtract amount
        rt1[i]=(source[i]>>11)&0x1f; // destination
        rt2[i]=0;
        if(op2==0x2a||op2==0x2b) { // SLT/SLTU
          us1[i]=rs1[i];us2[i]=rs2[i];
        }
        else if(op2>=0x24&&op2<=0x27) { // AND/OR/XOR/NOR
          dep1[i]=rs1[i];dep2[i]=rs2[i];
        }
        else if(op2>=0x2c&&op2<=0x2f) { // DADD/DSUB
          dep1[i]=rs1[i];dep2[i]=rs2[i];
        }
        break;
      case MULTDIV:
        rs1[i]=(source[i]>>21)&0x1f; // source
        rs2[i]=(source[i]>>16)&0x1f; // divisor
        rt1[i]=HIREG;
        rt2[i]=LOREG;
        if (op2>=0x1c&&op2<=0x1f) { // DMULT/DMULTU/DDIV/DDIVU
          us1[i]=rs1[i];us2[i]=rs2[i];
        }
        break;
      case MOV:
        rs1[i]=0;
        rs2[i]=0;
        rt1[i]=0;
        rt2[i]=0;
        if(op2==0x10) rs1[i]=HIREG; // MFHI
        if(op2==0x11) rt1[i]=HIREG; // MTHI
        if(op2==0x12) rs1[i]=LOREG; // MFLO
        if(op2==0x13) rt1[i]=LOREG; // MTLO
        if((op2&0x1d)==0x10) rt1[i]=(source[i]>>11)&0x1f; // MFxx
        if((op2&0x1d)==0x11) rs1[i]=(source[i]>>21)&0x1f; // MTxx
        dep1[i]=rs1[i];
        break;
      case SHIFT:
        rs1[i]=(source[i]>>16)&0x1f; // target of shift
        rs2[i]=(source[i]>>21)&0x1f; // shift amount
        rt1[i]=(source[i]>>11)&0x1f; // destination
        rt2[i]=0;
        // DSLLV/DSRLV/DSRAV are 64-bit
        if(op2>=0x14&&op2<=0x17) us1[i]=rs1[i];
        break;
      case SHIFTIMM:
        rs1[i]=(source[i]>>16)&0x1f;
        rs2[i]=0;
        rt1[i]=(source[i]>>11)&0x1f;
        rt2[i]=0;
        imm[i]=(source[i]>>6)&0x1f;
        // DSxx32 instructions
        if(op2>=0x3c) imm[i]|=0x20;
        // DSLL/DSRL/DSRA/DSRA32/DSRL32 but not DSLL32 require 64-bit source
        if(op2>=0x38&&op2!=0x3c) us1[i]=rs1[i];
        break;
      case COP0:
        rs1[i]=0;
        rs2[i]=0;
        rt1[i]=0;
        rt2[i]=0;
        if(op2==0) rt1[i]=(source[i]>>16)&0x1F; // MFC0
        if(op2==4) rs1[i]=(source[i]>>16)&0x1F; // MTC0
        if(op2==4&&((source[i]>>11)&0x1f)==12) rt2[i]=CSREG; // Status
        if(op2==16) if((source[i]&0x3f)==0x18) rs2[i]=CCREG; // ERET
        break;
      case COP1:
        rs1[i]=0;
        rs2[i]=0;
        rt1[i]=0;
        rt2[i]=0;
        if(op2<3) rt1[i]=(source[i]>>16)&0x1F; // MFC1/DMFC1/CFC1
        if(op2>3) rs1[i]=(source[i]>>16)&0x1F; // MTC1/DMTC1/CTC1
        if(op2==5) us1[i]=rs1[i]; // DMTC1
        rs2[i]=CSREG;
        break;
      case C1LS:
        rs1[i]=(source[i]>>21)&0x1F;
        rs2[i]=CSREG;
        rt1[i]=0;
        rt2[i]=0;
        imm[i]=(short)source[i];
        break;
      case FLOAT:
      case FCONV:
        rs1[i]=0;
        rs2[i]=CSREG;
        rt1[i]=0;
        rt2[i]=0;
        break;
      case FCOMP:
        rs1[i]=FSREG;
        rs2[i]=CSREG;
        rt1[i]=FSREG;
        rt2[i]=0;
        break;
      case SYSCALL:
        rs1[i]=CCREG;
        rs2[i]=0;
        rt1[i]=0;
        rt2[i]=0;
        break;
      default:
        rs1[i]=0;
        rs2[i]=0;
        rt1[i]=0;
        rt2[i]=0;
    }
    /* Calculate branch target addresses */
    if(type==UJUMP)
      ba[i]=((start+i*4+4)&0xF0000000)|(((unsigned int)source[i]<<6)>>4);
    else if(type==CJUMP&&rs1[i]==rs2[i]&&(op&1))
      ba[i]=start+i*4+8; // Ignore never taken branch
    else if(type==SJUMP&&rs1[i]==0&&!(op2&1))
      ba[i]=start+i*4+8; // Ignore never taken branch
    else if(type==CJUMP||type==SJUMP||type==FJUMP)
      ba[i]=start+i*4+4+((signed int)((unsigned int)source[i]<<16)>>14);
    else ba[i]=-1;
    /* Is this the end of the block? */
    if(i>0&&(itype[i-1]==UJUMP||itype[i-1]==RJUMP||(source[i-1]>>16)==0x1000)) {
      if(rt1[i-1]==0) { // Continue past subroutine call (JAL)
        done=1;
        // Does the block continue due to a branch?
        for(j=i-1;j>=0;j--)
        {
          if(ba[j]==start+i*4) done=j=0; // Branch into delay slot
          if(ba[j]==start+i*4+4) done=j=0;
          if(ba[j]==start+i*4+8) done=j=0;
        }

        // Stop if we're compiling junk
        if(type==UJUMP||type==CJUMP||type==SJUMP||type==RJUMP||type==FJUMP)
        {
          done=stop_after_jal=1;
          itype[i]=NOP;
          DebugMessage(M64MSG_VERBOSE, "Disabled speculative precompilation");
        }
      }
      else {
        if(stop_after_jal) done=1;
        // Stop on BREAK
        if((source[i+1]&0xfc00003f)==0x0d) done=1;
      }
      // Don't recompile stuff that's already compiled
      if(check_addr(start+i*4+4)) done=1;
      // Don't get too close to the limit
      if(i>MAXBLOCK/2) done=1;
    }
    if(i>0&&itype[i]==SYSCALL&&stop_after_jal) done=1;
    assert(i<MAXBLOCK-1);
    if(start+i*4==pagelimit-4) done=1;
    assert(start+i*4<pagelimit);
    if (i==MAXBLOCK-1) done=1;
    // Stop if we're compiling junk
    if(itype[i]==NI&&opcode[i]==0x11) {
      done=stop_after_jal=1;
      DebugMessage(M64MSG_VERBOSE, "Disabled speculative precompilation");
    }
  }
  slen=i;
  if(itype[i-1]==UJUMP||itype[i-1]==CJUMP||itype[i-1]==SJUMP||itype[i-1]==RJUMP||itype[i-1]==FJUMP) {
    if(start+i*4==pagelimit) {
      itype[i-1]=SPAN;
    }
  }
  assert(slen>0);

  /* Pass 2 - Register dependencies and branch targets */

  unneeded_registers(0,slen-1,0);

  /* Pass 3 - Register allocation */

  struct regstat current; // Current register allocations/status
  current.is32=1;
  current.dirty=0;
  current.u=unneeded_reg[0];
  current.uu=unneeded_reg_upper[0];
  clear_all_regs(current.regmap);
  alloc_reg(&current,0,CCREG);
  dirty_reg(&current,CCREG);
  current.isconst=0;
  current.wasconst=0;
  int ds=0;
  int cc=0;
  int hr;

  provisional_32bit();

  if((u_int)addr&1) {
    // First instruction is delay slot
    cc=-1;
    bt[1]=1;
    ds=1;
    unneeded_reg[0]=1;
    unneeded_reg_upper[0]=1;
    current.regmap[HOST_BTREG]=BTREG;
  }

  for(i=0;i<slen;i++)
  {
    if(bt[i])
    {
      int hr;
      for(hr=0;hr<HOST_REGS;hr++)
      {
        // Is this really necessary?
        if(current.regmap[hr]==0) current.regmap[hr]=-1;
      }
      current.isconst=0;
    }
    memcpy(regmap_pre[i],current.regmap,sizeof(current.regmap));
    if(i>1)
    {
      if((opcode[i-2]&0x2f)==0x05) // BNE/BNEL
      {
        if(rs1[i-2]==0||rs2[i-2]==0)
        {
          if(rs1[i-2]) {
            current.is32|=1LL<<rs1[i-2];
            int hr=get_reg(current.regmap,rs1[i-2]|64);
            if(hr>=0) current.regmap[hr]=-1;
          }
          if(rs2[i-2]) {
            current.is32|=1LL<<rs2[i-2];
            int hr=get_reg(current.regmap,rs2[i-2]|64);
            if(hr>=0) current.regmap[hr]=-1;
          }
        }
      }
    }
    // If something jumps here with 64-bit values
    // then promote those registers to 64 bits
    if(bt[i])
    {
      uint64_t temp_is32=current.is32;
      for(j=i-1;j>=0;j--)
      {
        if(ba[j]==start+i*4)
          temp_is32&=branch_regs[j].is32;
      }
      for(j=i;j<slen;j++)
      {
        if(ba[j]==start+i*4)
          //temp_is32=1;
          temp_is32&=p32[j];
      }
      if(temp_is32!=current.is32) {
        //DebugMessage(M64MSG_VERBOSE, "dumping 32-bit regs (%x)",start+i*4);
        #ifndef DESTRUCTIVE_WRITEBACK
        if(ds)
        #endif
        for(hr=0;hr<HOST_REGS;hr++)
        {
          int r=current.regmap[hr];
          if(r>0&&r<64)
          {
            if((current.dirty>>hr)&((current.is32&~temp_is32)>>r)&1) {
              temp_is32|=1LL<<r;
              //DebugMessage(M64MSG_VERBOSE, "restore %d",r);
            }
          }
        }
        current.is32=temp_is32;
      }
    }
    regs[i].wasconst=current.isconst;
    regs[i].was32=current.is32;
    regs[i].wasdirty=current.dirty;
    #ifdef DESTRUCTIVE_WRITEBACK
    // To change a dirty register from 32 to 64 bits, we must write
    // it out during the previous cycle (for branches, 2 cycles)
    if(i<slen-1&&bt[i+1]&&itype[i-1]!=UJUMP&&itype[i-1]!=CJUMP&&itype[i-1]!=SJUMP&&itype[i-1]!=RJUMP&&itype[i-1]!=FJUMP)
    {
      uint64_t temp_is32=current.is32;
      for(j=i-1;j>=0;j--)
      {
        if(ba[j]==start+i*4+4)
          temp_is32&=branch_regs[j].is32;
      }
      for(j=i;j<slen;j++)
      {
        if(ba[j]==start+i*4+4)
          //temp_is32=1;
          temp_is32&=p32[j];
      }
      if(temp_is32!=current.is32) {
        //DebugMessage(M64MSG_VERBOSE, "pre-dumping 32-bit regs (%x)",start+i*4);
        for(hr=0;hr<HOST_REGS;hr++)
        {
          int r=current.regmap[hr];
          if(r>0)
          {
            if((current.dirty>>hr)&((current.is32&~temp_is32)>>(r&63))&1) {
              if(itype[i]!=UJUMP&&itype[i]!=CJUMP&&itype[i]!=SJUMP&&itype[i]!=RJUMP&&itype[i]!=FJUMP)
              {
                if(rs1[i]!=(r&63)&&rs2[i]!=(r&63))
                {
                  //DebugMessage(M64MSG_VERBOSE, "dump %d/r%d",hr,r);
                  current.regmap[hr]=-1;
                  if(get_reg(current.regmap,r|64)>=0)
                    current.regmap[get_reg(current.regmap,r|64)]=-1;
                }
              }
            }
          }
        }
      }
    }
    else if(i<slen-2&&bt[i+2]&&(source[i-1]>>16)!=0x1000&&(itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP))
    {
      uint64_t temp_is32=current.is32;
      for(j=i-1;j>=0;j--)
      {
        if(ba[j]==start+i*4+8)
          temp_is32&=branch_regs[j].is32;
      }
      for(j=i;j<slen;j++)
      {
        if(ba[j]==start+i*4+8)
          //temp_is32=1;
          temp_is32&=p32[j];
      }
      if(temp_is32!=current.is32) {
        //DebugMessage(M64MSG_VERBOSE, "pre-dumping 32-bit regs (%x)",start+i*4);
        for(hr=0;hr<HOST_REGS;hr++)
        {
          int r=current.regmap[hr];
          if(r>0)
          {
            if((current.dirty>>hr)&((current.is32&~temp_is32)>>(r&63))&1) {
              if(rs1[i]!=(r&63)&&rs2[i]!=(r&63)&&rs1[i+1]!=(r&63)&&rs2[i+1]!=(r&63))
              {
                //DebugMessage(M64MSG_VERBOSE, "dump %d/r%d",hr,r);
                current.regmap[hr]=-1;
                if(get_reg(current.regmap,r|64)>=0)
                  current.regmap[get_reg(current.regmap,r|64)]=-1;
              }
            }
          }
        }
      }
    }
    #endif
    if(itype[i]!=UJUMP&&itype[i]!=CJUMP&&itype[i]!=SJUMP&&itype[i]!=RJUMP&&itype[i]!=FJUMP) {
      if(i+1<slen) {
        current.u=unneeded_reg[i+1]&~((1LL<<rs1[i])|(1LL<<rs2[i]));
        current.uu=unneeded_reg_upper[i+1]&~((1LL<<us1[i])|(1LL<<us2[i]));
        if((~current.uu>>rt1[i])&1) current.uu&=~((1LL<<dep1[i])|(1LL<<dep2[i]));
        current.u|=1;
        current.uu|=1;
      } else {
        current.u=1;
        current.uu=1;
      }
    } else {
      if(i+1<slen) {
        current.u=branch_unneeded_reg[i]&~((1LL<<rs1[i+1])|(1LL<<rs2[i+1]));
        current.uu=branch_unneeded_reg_upper[i]&~((1LL<<us1[i+1])|(1LL<<us2[i+1]));
        if((~current.uu>>rt1[i+1])&1) current.uu&=~((1LL<<dep1[i+1])|(1LL<<dep2[i+1]));
        current.u&=~((1LL<<rs1[i])|(1LL<<rs2[i]));
        current.uu&=~((1LL<<us1[i])|(1LL<<us2[i]));
        current.u|=1;
        current.uu|=1;
      } else { DebugMessage(M64MSG_ERROR, "oops, branch at end of block with no delay slot");exit(1); }
    }
    is_ds[i]=ds;
    if(ds) {
      ds=0; // Skip delay slot, already allocated as part of branch
      // ...but we need to alloc it in case something jumps here
      if(i+1<slen) {
        current.u=branch_unneeded_reg[i-1]&unneeded_reg[i+1];
        current.uu=branch_unneeded_reg_upper[i-1]&unneeded_reg_upper[i+1];
      }else{
        current.u=branch_unneeded_reg[i-1];
        current.uu=branch_unneeded_reg_upper[i-1];
      }
      current.u&=~((1LL<<rs1[i])|(1LL<<rs2[i]));
      current.uu&=~((1LL<<us1[i])|(1LL<<us2[i]));
      if((~current.uu>>rt1[i])&1) current.uu&=~((1LL<<dep1[i])|(1LL<<dep2[i]));
      current.u|=1;
      current.uu|=1;
      struct regstat temp;
      memcpy(&temp,&current,sizeof(current));
      temp.wasdirty=temp.dirty;
      temp.was32=temp.is32;
      // TODO: Take into account unconditional branches, as below
      delayslot_alloc(&temp,i);
      memcpy(regs[i].regmap,temp.regmap,sizeof(temp.regmap));
      regs[i].wasdirty=temp.wasdirty;
      regs[i].was32=temp.was32;
      regs[i].dirty=temp.dirty;
      regs[i].is32=temp.is32;
      regs[i].isconst=0;
      regs[i].wasconst=0;
      current.isconst=0;
      // Create entry (branch target) regmap
      for(hr=0;hr<HOST_REGS;hr++)
      {
        int r=temp.regmap[hr];
        if(r>=0) {
          if(r!=regmap_pre[i][hr]) {
            regs[i].regmap_entry[hr]=-1;
          }
          else
          {
            if(r<64){
              if((current.u>>r)&1) {
                regs[i].regmap_entry[hr]=-1;
                regs[i].regmap[hr]=-1;
                //Don't clear regs in the delay slot as the branch might need them
                if(!ooo[i-1])
                    current.regmap[hr]=-1;
              }else
                regs[i].regmap_entry[hr]=r;
            }
            else {
              if((current.uu>>(r&63))&1) {
                regs[i].regmap_entry[hr]=-1;
                regs[i].regmap[hr]=-1;
                //Don't clear regs in the delay slot as the branch might need them
                if(!ooo[i-1])
                    current.regmap[hr]=-1;
              }else
                regs[i].regmap_entry[hr]=r;
            }
          }
        } else {
          // First instruction expects CCREG to be allocated
          if(i==0&&hr==HOST_CCREG)
            regs[i].regmap_entry[hr]=CCREG;
          else
            regs[i].regmap_entry[hr]=-1;
        }
      }
    }
    else { // Not delay slot
      switch(itype[i]) {
        case UJUMP:
          //current.isconst=0; // DEBUG
          //current.wasconst=0; // DEBUG
          //regs[i].wasconst=0; // DEBUG
          clear_const(&current,rt1[i]);
          alloc_cc(&current,i);
          dirty_reg(&current,CCREG);
          if (rt1[i]==31) {
            alloc_reg(&current,i,31);
            dirty_reg(&current,31);
            #ifdef REG_PREFETCH
            alloc_reg(&current,i,PTEMP);
            #endif
            //current.is32|=1LL<<rt1[i];
          }
          ooo[i]=1;
          delayslot_alloc(&current,i+1);
          //current.isconst=0; // DEBUG
          ds=1;
          //DebugMessage(M64MSG_VERBOSE, "i=%d, isconst=%x",i,current.isconst);
          break;
        case RJUMP:
          //current.isconst=0;
          //current.wasconst=0;
          //regs[i].wasconst=0;
          clear_const(&current,rs1[i]);
          clear_const(&current,rt1[i]);
          alloc_cc(&current,i);
          dirty_reg(&current,CCREG);
          if((rs1[i]!=rt1[i+1]&&rs1[i]!=rt2[i+1])||(rs1[i]==0)) {
            alloc_reg(&current,i,rs1[i]);
            if (rt1[i]!=0) {
              alloc_reg(&current,i,rt1[i]);
              dirty_reg(&current,rt1[i]);
              assert(rs1[i+1]!=31&&rs2[i+1]!=31);
              #ifdef REG_PREFETCH
              alloc_reg(&current,i,PTEMP);
              #endif
            }
            #ifdef USE_MINI_HT
            if(rs1[i]==31) { // JALR
              alloc_reg(&current,i,RHASH);
              #if !defined(HOST_IMM_ADDR32)&&(NEW_DYNAREC!=NEW_DYNAREC_X64)
              alloc_reg(&current,i,RHTBL);
              #endif
            }
            #endif
            delayslot_alloc(&current,i+1);
          } else {
            // The delay slot overwrites our source register,
            // allocate a temporary register to hold the old value.
            current.isconst=0;
            current.wasconst=0;
            regs[i].wasconst=0;
            delayslot_alloc(&current,i+1);
            current.isconst=0;
            alloc_reg(&current,i,RTEMP);
          }
          //current.isconst=0; // DEBUG
          ooo[i]=1;
          ds=1;
          break;
        case CJUMP:
          //current.isconst=0;
          //current.wasconst=0;
          //regs[i].wasconst=0;
          clear_const(&current,rs1[i]);
          clear_const(&current,rs2[i]);
          if((opcode[i]&0x3E)==4) // BEQ/BNE
          {
            alloc_cc(&current,i);
            dirty_reg(&current,CCREG);
            if(rs1[i]) alloc_reg(&current,i,rs1[i]);
            if(rs2[i]) alloc_reg(&current,i,rs2[i]);
            if(!((current.is32>>rs1[i])&(current.is32>>rs2[i])&1))
            {
              if(rs1[i]) alloc_reg64(&current,i,rs1[i]);
              if(rs2[i]) alloc_reg64(&current,i,rs2[i]);
            }
            if((rs1[i]&&(rs1[i]==rt1[i+1]||rs1[i]==rt2[i+1]))||
               (rs2[i]&&(rs2[i]==rt1[i+1]||rs2[i]==rt2[i+1]))) {
              // The delay slot overwrites one of our conditions.
              // Allocate the branch condition registers instead.
              current.isconst=0;
              current.wasconst=0;
              regs[i].wasconst=0;
              if(rs1[i]) alloc_reg(&current,i,rs1[i]);
              if(rs2[i]) alloc_reg(&current,i,rs2[i]);
              if(!((current.is32>>rs1[i])&(current.is32>>rs2[i])&1))
              {
                if(rs1[i]) alloc_reg64(&current,i,rs1[i]);
                if(rs2[i]) alloc_reg64(&current,i,rs2[i]);
              }
            }
            else if((i!=(ba[i]-start)>>2 || source[i+1]!=0))
            {
              ooo[i]=1;
              delayslot_alloc(&current,i+1);
            }
          }
          else
          if((opcode[i]&0x3E)==6) // BLEZ/BGTZ
          {
            alloc_cc(&current,i);
            dirty_reg(&current,CCREG);
            alloc_reg(&current,i,rs1[i]);
            if(!(current.is32>>rs1[i]&1))
            {
              alloc_reg64(&current,i,rs1[i]);
            }
            if(rs1[i]&&(rs1[i]==rt1[i+1]||rs1[i]==rt2[i+1])) {
              // The delay slot overwrites one of our conditions.
              // Allocate the branch condition registers instead.
              current.isconst=0;
              current.wasconst=0;
              regs[i].wasconst=0;
              if(rs1[i]) alloc_reg(&current,i,rs1[i]);
              if(!((current.is32>>rs1[i])&1))
              {
                if(rs1[i]) alloc_reg64(&current,i,rs1[i]);
              }
            }
            else if((i!=(ba[i]-start)>>2 || source[i+1]!=0))
            {
              ooo[i]=1;
              delayslot_alloc(&current,i+1);
            }
          }
          else
          // Don't alloc the delay slot yet because we might not execute it
          if((opcode[i]&0x3E)==0x14) // BEQL/BNEL
          {
            current.isconst=0;
            current.wasconst=0;
            regs[i].wasconst=0;
            alloc_cc(&current,i);
            dirty_reg(&current,CCREG);
            alloc_reg(&current,i,rs1[i]);
            alloc_reg(&current,i,rs2[i]);
            if(!((current.is32>>rs1[i])&(current.is32>>rs2[i])&1))
            {
              alloc_reg64(&current,i,rs1[i]);
              alloc_reg64(&current,i,rs2[i]);
            }
          }
          else
          if((opcode[i]&0x3E)==0x16) // BLEZL/BGTZL
          {
            current.isconst=0;
            current.wasconst=0;
            regs[i].wasconst=0;
            alloc_cc(&current,i);
            dirty_reg(&current,CCREG);
            alloc_reg(&current,i,rs1[i]);
            if(!(current.is32>>rs1[i]&1))
            {
              alloc_reg64(&current,i,rs1[i]);
            }
          }
          ds=1;
          //current.isconst=0;
          break;
        case SJUMP:
          //current.isconst=0;
          //current.wasconst=0;
          //regs[i].wasconst=0;
          clear_const(&current,rs1[i]);
          clear_const(&current,rt1[i]);
          //if((opcode2[i]&0x1E)==0x0) // BLTZ/BGEZ
          if((opcode2[i]&0x0E)==0x0) // BLTZ/BGEZ
          {
            alloc_cc(&current,i);
            dirty_reg(&current,CCREG);
            alloc_reg(&current,i,rs1[i]);
            if(!(current.is32>>rs1[i]&1))
            {
              alloc_reg64(&current,i,rs1[i]);
            }
            if (rt1[i]==31) { // BLTZAL/BGEZAL
              alloc_reg(&current,i,31);
              dirty_reg(&current,31);
              assert(rs1[i+1]!=31&&rs2[i+1]!=31);
              //#ifdef REG_PREFETCH
              //alloc_reg(&current,i,PTEMP);
              //#endif
              //current.is32|=1LL<<rt1[i];
            }
            if(rs1[i]&&(rs1[i]==rt1[i+1]||rs1[i]==rt2[i+1])) {
              // The delay slot overwrites the branch condition.
              // Allocate the branch condition registers instead.
              current.isconst=0;
              current.wasconst=0;
              regs[i].wasconst=0;
              if(rs1[i]) alloc_reg(&current,i,rs1[i]);
              if(!((current.is32>>rs1[i])&1))
              {
                if(rs1[i]) alloc_reg64(&current,i,rs1[i]);
              }
            }
            else if((i!=(ba[i]-start)>>2 || source[i+1]!=0))
            {
              ooo[i]=1;
              delayslot_alloc(&current,i+1);
            }
          }
          else
          // Don't alloc the delay slot yet because we might not execute it
          if((opcode2[i]&0x1E)==0x2) // BLTZL/BGEZL
          {
            current.isconst=0;
            current.wasconst=0;
            regs[i].wasconst=0;
            alloc_cc(&current,i);
            dirty_reg(&current,CCREG);
            alloc_reg(&current,i,rs1[i]);
            if(!(current.is32>>rs1[i]&1))
            {
              alloc_reg64(&current,i,rs1[i]);
            }
          }
          ds=1;
          //current.isconst=0;
          break;
        case FJUMP:
          current.isconst=0;
          current.wasconst=0;
          regs[i].wasconst=0;
          if(likely[i]==0) // BC1F/BC1T
          {
            // TODO: Theoretically we can run out of registers here on x86.
            // The delay slot can allocate up to six, and we need to check
            // CSREG before executing the delay slot.  Possibly we can drop
            // the cycle count and then reload it after checking that the
            // FPU is in a usable state, or don't do out-of-order execution.
            alloc_cc(&current,i);
            dirty_reg(&current,CCREG);
            alloc_reg(&current,i,FSREG);
            alloc_reg(&current,i,CSREG);
            if(itype[i+1]==FCOMP) {
              // The delay slot overwrites the branch condition.
              // Allocate the branch condition registers instead.
              alloc_cc(&current,i);
              dirty_reg(&current,CCREG);
              alloc_reg(&current,i,CSREG);
              alloc_reg(&current,i,FSREG);
            }
            else if((i!=(ba[i]-start)>>2 || source[i+1]!=0)) {
              ooo[i]=1;
              delayslot_alloc(&current,i+1);
              alloc_reg(&current,i+1,CSREG);
            }
          }
          else
          // Don't alloc the delay slot yet because we might not execute it
          if(likely[i]) // BC1FL/BC1TL
          {
            alloc_cc(&current,i);
            dirty_reg(&current,CCREG);
            alloc_reg(&current,i,CSREG);
            alloc_reg(&current,i,FSREG);
          }
          ds=1;
          current.isconst=0;
          break;
        case IMM16:
          imm16_alloc(&current,i);
          break;
        case LOAD:
        case LOADLR:
          load_alloc(&current,i);
          break;
        case STORE:
        case STORELR:
          store_alloc(&current,i);
          break;
        case ALU:
          alu_alloc(&current,i);
          break;
        case SHIFT:
          shift_alloc(&current,i);
          break;
        case MULTDIV:
          multdiv_alloc(&current,i);
          break;
        case SHIFTIMM:
          shiftimm_alloc(&current,i);
          break;
        case MOV:
          mov_alloc(&current,i);
          break;
        case COP0:
          cop0_alloc(&current,i);
          break;
        case COP1:
          cop1_alloc(&current,i);
          break;
        case C1LS:
          c1ls_alloc(&current,i);
          break;
        case FCONV:
          fconv_alloc(&current,i);
          break;
        case FLOAT:
          float_alloc(&current,i);
          break;
        case FCOMP:
          fcomp_alloc(&current,i);
          break;
        case SYSCALL:
          syscall_alloc(&current,i);
          break;
        case SPAN:
          pagespan_alloc(&current,i);
          break;
      }

      // Make sure the allocator didn't do anything bad
      for(hr=0;hr<HOST_REGS;hr++)
      {
        int r;
        r=current.regmap[hr];
        if(r>=0) {
          if(r==regmap_pre[i][hr]) {
            // If it was dirty before, make sure it's still dirty
            if((regs[i].wasdirty>>hr)&1) {
              if (!((current.dirty>>hr)&1))
              {
                //FIXME: delayslot_alloc realloc registers removed from cache by jump instruction, thus the missing dirty flag
                assert(itype[i]==CJUMP);
                current.dirty|=1LL<<hr;
              }
              //assert (((current.dirty>>hr)&1)!=0);
            }
          }
        }
      }

      // Drop the upper half of registers that have become 32-bit
      current.uu|=current.is32&((1LL<<rt1[i])|(1LL<<rt2[i]));
      if(itype[i]!=UJUMP&&itype[i]!=CJUMP&&itype[i]!=SJUMP&&itype[i]!=RJUMP&&itype[i]!=FJUMP) {
        current.uu&=~((1LL<<us1[i])|(1LL<<us2[i]));
        if((~current.uu>>rt1[i])&1) current.uu&=~((1LL<<dep1[i])|(1LL<<dep2[i]));
        current.uu|=1;
      } else {
        current.uu|=current.is32&((1LL<<rt1[i+1])|(1LL<<rt2[i+1]));
        current.uu&=~((1LL<<us1[i+1])|(1LL<<us2[i+1]));
        if((~current.uu>>rt1[i+1])&1) current.uu&=~((1LL<<dep1[i+1])|(1LL<<dep2[i+1]));
        current.uu&=~((1LL<<us1[i])|(1LL<<us2[i]));
        current.uu|=1;
      }

      // Create entry (branch target) regmap
      for(hr=0;hr<HOST_REGS;hr++)
      {
        int r,or;
        r=current.regmap[hr];
        if(r>=0) {
          if(r!=regmap_pre[i][hr]) {
            // TODO: delay slot (?)
            or=get_reg(regmap_pre[i],r); // Get old mapping for this register
            if(or<0||(r&63)>=TEMPREG){
              regs[i].regmap_entry[hr]=-1;
            }
            else
            {
              // Just move it to a different register
              regs[i].regmap_entry[hr]=r;
              // If it was dirty before, it's still dirty
              if((regs[i].wasdirty>>or)&1) dirty_reg(&current,r&63);
            }
          }
          else
          {
            // Unneeded
            if(r==0){
              regs[i].regmap_entry[hr]=0;
            }
            else
            if(r<64){
              if((current.u>>r)&1) {
                regs[i].regmap_entry[hr]=-1;
                //regs[i].regmap[hr]=-1;
                current.regmap[hr]=-1;
              }else
                regs[i].regmap_entry[hr]=r;
            }
            else {
              if((current.uu>>(r&63))&1) {
                regs[i].regmap_entry[hr]=-1;
                //regs[i].regmap[hr]=-1;
                current.regmap[hr]=-1;
              }else
                regs[i].regmap_entry[hr]=r;
            }
          }
        } else {
          // Branches expect CCREG to be allocated at the target
          if(regmap_pre[i][hr]==CCREG)
            regs[i].regmap_entry[hr]=CCREG;
          else
            regs[i].regmap_entry[hr]=-1;
        }
      }
      memcpy(regs[i].regmap,current.regmap,sizeof(current.regmap));
    }
    /* Branch post-alloc */
    if(i>0)
    {
      current.was32=current.is32;
      current.wasdirty=current.dirty;
      switch(itype[i-1]) {
        case UJUMP:
          memcpy(&branch_regs[i-1],&current,sizeof(current));
          branch_regs[i-1].isconst=0;
          branch_regs[i-1].wasconst=0;
          branch_regs[i-1].u=branch_unneeded_reg[i-1]&~((1LL<<rs1[i-1])|(1LL<<rs2[i-1]));
          branch_regs[i-1].uu=branch_unneeded_reg_upper[i-1]&~((1LL<<us1[i-1])|(1LL<<us2[i-1]));
          alloc_cc(&branch_regs[i-1],i-1);
          dirty_reg(&branch_regs[i-1],CCREG);
          if(rt1[i-1]==31) { // JAL
            alloc_reg(&branch_regs[i-1],i-1,31);
            dirty_reg(&branch_regs[i-1],31);
            branch_regs[i-1].is32|=1LL<<31;
          }
          memcpy(&branch_regs[i-1].regmap_entry,&branch_regs[i-1].regmap,sizeof(current.regmap));
          memcpy(constmap[i],constmap[i-1],sizeof(current.constmap));
          break;
        case RJUMP:
          memcpy(&branch_regs[i-1],&current,sizeof(current));
          branch_regs[i-1].isconst=0;
          branch_regs[i-1].wasconst=0;
          branch_regs[i-1].u=branch_unneeded_reg[i-1]&~((1LL<<rs1[i-1])|(1LL<<rs2[i-1]));
          branch_regs[i-1].uu=branch_unneeded_reg_upper[i-1]&~((1LL<<us1[i-1])|(1LL<<us2[i-1]));
          alloc_cc(&branch_regs[i-1],i-1);
          dirty_reg(&branch_regs[i-1],CCREG);
          alloc_reg(&branch_regs[i-1],i-1,rs1[i-1]);
          if(rt1[i-1]!=0) { // JALR
            alloc_reg(&branch_regs[i-1],i-1,rt1[i-1]);
            dirty_reg(&branch_regs[i-1],rt1[i-1]);
            branch_regs[i-1].is32|=1LL<<rt1[i-1];
          }
          #ifdef USE_MINI_HT
          if(rs1[i-1]==31) { // JALR
            alloc_reg(&branch_regs[i-1],i-1,RHASH);
            #if !defined(HOST_IMM_ADDR32)&&(NEW_DYNAREC!=NEW_DYNAREC_X64)
            alloc_reg(&branch_regs[i-1],i-1,RHTBL);
            #endif
          }
          #endif
          memcpy(&branch_regs[i-1].regmap_entry,&branch_regs[i-1].regmap,sizeof(current.regmap));
          memcpy(constmap[i],constmap[i-1],sizeof(current.constmap));
          break;
        case CJUMP:
          if((opcode[i-1]&0x3E)==4) // BEQ/BNE
          {
            alloc_cc(&current,i-1);
            dirty_reg(&current,CCREG);
            if((rs1[i-1]&&(rs1[i-1]==rt1[i]||rs1[i-1]==rt2[i]))||
               (rs2[i-1]&&(rs2[i-1]==rt1[i]||rs2[i-1]==rt2[i]))) {
              // The delay slot overwrote one of our conditions
              // Delay slot goes after the test (in order)
              current.u=branch_unneeded_reg[i-1]&~((1LL<<rs1[i])|(1LL<<rs2[i]));
              current.uu=branch_unneeded_reg_upper[i-1]&~((1LL<<us1[i])|(1LL<<us2[i]));
              if((~current.uu>>rt1[i])&1) current.uu&=~((1LL<<dep1[i])|(1LL<<dep2[i]));
              current.u|=1;
              current.uu|=1;
              delayslot_alloc(&current,i);
              current.isconst=0;
            }
            else
            {
              current.u=branch_unneeded_reg[i-1]&~((1LL<<rs1[i-1])|(1LL<<rs2[i-1]));
              current.uu=branch_unneeded_reg_upper[i-1]&~((1LL<<us1[i-1])|(1LL<<us2[i-1]));
              // Alloc the branch condition registers
              if(rs1[i-1]) alloc_reg(&current,i-1,rs1[i-1]);
              if(rs2[i-1]) alloc_reg(&current,i-1,rs2[i-1]);
              if(!((current.is32>>rs1[i-1])&(current.is32>>rs2[i-1])&1))
              {
                if(rs1[i-1]) alloc_reg64(&current,i-1,rs1[i-1]);
                if(rs2[i-1]) alloc_reg64(&current,i-1,rs2[i-1]);
              }
            }
            memcpy(&branch_regs[i-1],&current,sizeof(current));
            branch_regs[i-1].isconst=0;
            branch_regs[i-1].wasconst=0;
            memcpy(&branch_regs[i-1].regmap_entry,&current.regmap,sizeof(current.regmap));
            memcpy(constmap[i],constmap[i-1],sizeof(current.constmap));
          }
          else
          if((opcode[i-1]&0x3E)==6) // BLEZ/BGTZ
          {
            alloc_cc(&current,i-1);
            dirty_reg(&current,CCREG);
            if(rs1[i-1]==rt1[i]||rs1[i-1]==rt2[i]) {
              // The delay slot overwrote the branch condition
              // Delay slot goes after the test (in order)
              current.u=branch_unneeded_reg[i-1]&~((1LL<<rs1[i])|(1LL<<rs2[i]));
              current.uu=branch_unneeded_reg_upper[i-1]&~((1LL<<us1[i])|(1LL<<us2[i]));
              if((~current.uu>>rt1[i])&1) current.uu&=~((1LL<<dep1[i])|(1LL<<dep2[i]));
              current.u|=1;
              current.uu|=1;
              delayslot_alloc(&current,i);
              current.isconst=0;
            }
            else
            {
              current.u=branch_unneeded_reg[i-1]&~(1LL<<rs1[i-1]);
              current.uu=branch_unneeded_reg_upper[i-1]&~(1LL<<us1[i-1]);
              // Alloc the branch condition register
              alloc_reg(&current,i-1,rs1[i-1]);
              if(!(current.is32>>rs1[i-1]&1))
              {
                alloc_reg64(&current,i-1,rs1[i-1]);
              }
            }
            memcpy(&branch_regs[i-1],&current,sizeof(current));
            branch_regs[i-1].isconst=0;
            branch_regs[i-1].wasconst=0;
            memcpy(&branch_regs[i-1].regmap_entry,&current.regmap,sizeof(current.regmap));
            memcpy(constmap[i],constmap[i-1],sizeof(current.constmap));
          }
          else
          // Alloc the delay slot in case the branch is taken
          if((opcode[i-1]&0x3E)==0x14) // BEQL/BNEL
          {
            memcpy(&branch_regs[i-1],&current,sizeof(current));
            branch_regs[i-1].u=(branch_unneeded_reg[i-1]&~((1LL<<rs1[i])|(1LL<<rs2[i])|(1LL<<rt1[i])|(1LL<<rt2[i])))|1;
            branch_regs[i-1].uu=(branch_unneeded_reg_upper[i-1]&~((1LL<<us1[i])|(1LL<<us2[i])|(1LL<<rt1[i])|(1LL<<rt2[i])))|1;
            if((~branch_regs[i-1].uu>>rt1[i])&1) branch_regs[i-1].uu&=~((1LL<<dep1[i])|(1LL<<dep2[i]))|1;
            alloc_cc(&branch_regs[i-1],i);
            dirty_reg(&branch_regs[i-1],CCREG);
            delayslot_alloc(&branch_regs[i-1],i);
            branch_regs[i-1].isconst=0;
            alloc_reg(&current,i,CCREG); // Not taken path
            dirty_reg(&current,CCREG);
            memcpy(&branch_regs[i-1].regmap_entry,&branch_regs[i-1].regmap,sizeof(current.regmap));
          }
          else
          if((opcode[i-1]&0x3E)==0x16) // BLEZL/BGTZL
          {
            memcpy(&branch_regs[i-1],&current,sizeof(current));
            branch_regs[i-1].u=(branch_unneeded_reg[i-1]&~((1LL<<rs1[i])|(1LL<<rs2[i])|(1LL<<rt1[i])|(1LL<<rt2[i])))|1;
            branch_regs[i-1].uu=(branch_unneeded_reg_upper[i-1]&~((1LL<<us1[i])|(1LL<<us2[i])|(1LL<<rt1[i])|(1LL<<rt2[i])))|1;
            if((~branch_regs[i-1].uu>>rt1[i])&1) branch_regs[i-1].uu&=~((1LL<<dep1[i])|(1LL<<dep2[i]))|1;
            alloc_cc(&branch_regs[i-1],i);
            dirty_reg(&branch_regs[i-1],CCREG);
            delayslot_alloc(&branch_regs[i-1],i);
            branch_regs[i-1].isconst=0;
            alloc_reg(&current,i,CCREG); // Not taken path
            dirty_reg(&current,CCREG);
            memcpy(&branch_regs[i-1].regmap_entry,&branch_regs[i-1].regmap,sizeof(current.regmap));
          }
          break;
        case SJUMP:
          //if((opcode2[i-1]&0x1E)==0) // BLTZ/BGEZ
          if((opcode2[i-1]&0x0E)==0) // BLTZ/BGEZ
          {
            alloc_cc(&current,i-1);
            dirty_reg(&current,CCREG);
            if(rs1[i-1]==rt1[i]||rs1[i-1]==rt2[i]) {
              // The delay slot overwrote the branch condition
              // Delay slot goes after the test (in order)
              current.u=branch_unneeded_reg[i-1]&~((1LL<<rs1[i])|(1LL<<rs2[i]));
              current.uu=branch_unneeded_reg_upper[i-1]&~((1LL<<us1[i])|(1LL<<us2[i]));
              if((~current.uu>>rt1[i])&1) current.uu&=~((1LL<<dep1[i])|(1LL<<dep2[i]));
              current.u|=1;
              current.uu|=1;
              delayslot_alloc(&current,i);
              current.isconst=0;
            }
            else
            {
              current.u=branch_unneeded_reg[i-1]&~(1LL<<rs1[i-1]);
              current.uu=branch_unneeded_reg_upper[i-1]&~(1LL<<us1[i-1]);
              // Alloc the branch condition register
              alloc_reg(&current,i-1,rs1[i-1]);
              if(!(current.is32>>rs1[i-1]&1))
              {
                alloc_reg64(&current,i-1,rs1[i-1]);
              }
            }
            memcpy(&branch_regs[i-1],&current,sizeof(current));
            branch_regs[i-1].isconst=0;
            branch_regs[i-1].wasconst=0;
            memcpy(&branch_regs[i-1].regmap_entry,&current.regmap,sizeof(current.regmap));
            memcpy(constmap[i],constmap[i-1],sizeof(current.constmap));
          }
          else
          // Alloc the delay slot in case the branch is taken
          if((opcode2[i-1]&0x1E)==2) // BLTZL/BGEZL
          {
            memcpy(&branch_regs[i-1],&current,sizeof(current));
            branch_regs[i-1].u=(branch_unneeded_reg[i-1]&~((1LL<<rs1[i])|(1LL<<rs2[i])|(1LL<<rt1[i])|(1LL<<rt2[i])))|1;
            branch_regs[i-1].uu=(branch_unneeded_reg_upper[i-1]&~((1LL<<us1[i])|(1LL<<us2[i])|(1LL<<rt1[i])|(1LL<<rt2[i])))|1;
            if((~branch_regs[i-1].uu>>rt1[i])&1) branch_regs[i-1].uu&=~((1LL<<dep1[i])|(1LL<<dep2[i]))|1;
            alloc_cc(&branch_regs[i-1],i);
            dirty_reg(&branch_regs[i-1],CCREG);
            delayslot_alloc(&branch_regs[i-1],i);
            branch_regs[i-1].isconst=0;
            alloc_reg(&current,i,CCREG); // Not taken path
            dirty_reg(&current,CCREG);
            memcpy(&branch_regs[i-1].regmap_entry,&branch_regs[i-1].regmap,sizeof(current.regmap));
          }
          // FIXME: BLTZAL/BGEZAL
          if(opcode2[i-1]&0x10) { // BxxZAL
            alloc_reg(&branch_regs[i-1],i-1,31);
            dirty_reg(&branch_regs[i-1],31);
            branch_regs[i-1].is32|=1LL<<31;
          }
          break;
        case FJUMP:
          if(likely[i-1]==0) // BC1F/BC1T
          {
            alloc_cc(&current,i-1);
            dirty_reg(&current,CCREG);
            if(itype[i]==FCOMP) {
              // The delay slot overwrote the branch condition
              // Delay slot goes after the test (in order)
              delayslot_alloc(&current,i);
              current.isconst=0;
            }
            else
            {
              current.u=branch_unneeded_reg[i-1]&~(1LL<<rs1[i-1]);
              current.uu=branch_unneeded_reg_upper[i-1]&~(1LL<<us1[i-1]);
              // Alloc the branch condition register
              alloc_reg(&current,i-1,FSREG);
            }
            memcpy(&branch_regs[i-1],&current,sizeof(current));
            memcpy(&branch_regs[i-1].regmap_entry,&current.regmap,sizeof(current.regmap));
          }
          else // BC1FL/BC1TL
          {
            // Alloc the delay slot in case the branch is taken
            memcpy(&branch_regs[i-1],&current,sizeof(current));
            branch_regs[i-1].u=(branch_unneeded_reg[i-1]&~((1LL<<rs1[i])|(1LL<<rs2[i])|(1LL<<rt1[i])|(1LL<<rt2[i])))|1;
            branch_regs[i-1].uu=(branch_unneeded_reg_upper[i-1]&~((1LL<<us1[i])|(1LL<<us2[i])|(1LL<<rt1[i])|(1LL<<rt2[i])))|1;
            if((~branch_regs[i-1].uu>>rt1[i])&1) branch_regs[i-1].uu&=~((1LL<<dep1[i])|(1LL<<dep2[i]))|1;
            alloc_cc(&branch_regs[i-1],i);
            dirty_reg(&branch_regs[i-1],CCREG);
            delayslot_alloc(&branch_regs[i-1],i);
            branch_regs[i-1].isconst=0;
            alloc_reg(&current,i,CCREG); // Not taken path
            dirty_reg(&current,CCREG);
            memcpy(&branch_regs[i-1].regmap_entry,&branch_regs[i-1].regmap,sizeof(current.regmap));
          }
          break;
      }

      if(itype[i-1]==UJUMP||itype[i-1]==RJUMP||(source[i-1]>>16)==0x1000)
      {
        if(rt1[i-1]==31) // JAL/JALR
        {
          // Subroutine call will return here, don't alloc any registers
          current.is32=1;
          current.dirty=0;
          clear_all_regs(current.regmap);
          alloc_reg(&current,i,CCREG);
          dirty_reg(&current,CCREG);
        }
        else if(i+1<slen)
        {
          // Internal branch will jump here, match registers to caller
          current.is32=0x3FFFFFFFFLL;
          current.dirty=0;
          clear_all_regs(current.regmap);
          alloc_reg(&current,i,CCREG);
          dirty_reg(&current,CCREG);
          for(j=i-1;j>=0;j--)
          {
            if(ba[j]==start+i*4+4) {
              memcpy(current.regmap,branch_regs[j].regmap,sizeof(current.regmap));
              current.is32=branch_regs[j].is32;
              current.dirty=branch_regs[j].dirty;
              break;
            }
          }
          while(j>=0) {
            if(ba[j]==start+i*4+4) {
              for(hr=0;hr<HOST_REGS;hr++) {
                if(current.regmap[hr]!=branch_regs[j].regmap[hr]) {
                  current.regmap[hr]=-1;
                }
                current.is32&=branch_regs[j].is32;
                current.dirty&=branch_regs[j].dirty;
              }
            }
            j--;
          }
        }
      }
    }

    // Count cycles in between branches
    ccadj[i]=cc;
    if(i>0&&(itype[i-1]==RJUMP||itype[i-1]==UJUMP||itype[i-1]==CJUMP||itype[i-1]==SJUMP||itype[i-1]==FJUMP||itype[i]==SYSCALL))
    {
      cc=0;
    }
    else
    {
      cc++;
    }

    flush_dirty_uppers(&current);
    if(!is_ds[i]) {
      regs[i].is32=current.is32;
      regs[i].dirty=current.dirty;
      regs[i].isconst=current.isconst;
      memcpy(constmap[i],current.constmap,sizeof(current.constmap));
    }
    for(hr=0;hr<HOST_REGS;hr++) {
      if(hr!=EXCLUDE_REG&&regs[i].regmap[hr]>=0) {
        if(regmap_pre[i][hr]!=regs[i].regmap[hr]) {
          regs[i].wasconst&=~(1<<hr);
        }
      }
    }
    if(current.regmap[HOST_BTREG]==BTREG) current.regmap[HOST_BTREG]=-1;
  }

  /* Pass 4 - Cull unused host registers */

  uint64_t nr=0LL;

  for (i=slen-1;i>=0;i--)
  {
    int hr=0;
    if(itype[i]==RJUMP||itype[i]==UJUMP||itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP)
    {
      if(ba[i]<start || ba[i]>=(start+slen*4))
      {
        // Branch out of this block, don't need anything
        nr=0LL;
      }
      else
      {
        // Internal branch
        // Need whatever matches the target
        nr=0LL;
        int t=(ba[i]-start)>>2;
        for(hr=0;hr<HOST_REGS;hr++)
        {
          if(regs[i].regmap_entry[hr]>=0) {
            if(regs[i].regmap_entry[hr]==regs[t].regmap_entry[hr]) nr|=1LL<<hr;
          }
        }
      }
      // Conditional branch may need registers for following instructions
      if(itype[i]!=RJUMP&&itype[i]!=UJUMP&&(source[i]>>16)!=0x1000)
      {
        if(i<slen-2) {
          nr|=needed_reg[i+2];
          for(hr=0;hr<HOST_REGS;hr++)
          {
            if(regmap_pre[i+2][hr]>=0&&get_reg(regs[i+2].regmap_entry,regmap_pre[i+2][hr])<0) nr&=~(1LL<<hr);
            //if((regmap_entry[i+2][hr])>=0) if(!((nr>>hr)&1)) DebugMessage(M64MSG_VERBOSE, "%x-bogus(%d=%d)",start+i*4,hr,regmap_entry[i+2][hr]);
          }
        }
      }
      // Don't need stuff which is overwritten
      //FIXME
      if(regs[i].regmap[hr]!=regmap_pre[i][hr]) nr&=~(1LL<<hr);
      if(regs[i].regmap[hr]<0) nr&=~(1LL<<hr);
      // Merge in delay slot
      for(hr=0;hr<HOST_REGS;hr++)
      {
        if(!likely[i]) {
          // These are overwritten unless the branch is "likely"
          // and the delay slot is nullified if not taken
          if(rt1[i+1]&&rt1[i+1]==(regs[i].regmap[hr]&63)) nr&=~(1LL<<hr);
          if(rt2[i+1]&&rt2[i+1]==(regs[i].regmap[hr]&63)) nr&=~(1LL<<hr);
        }
        if(us1[i+1]==(regmap_pre[i][hr]&63)) nr|=1LL<<hr;
        if(us2[i+1]==(regmap_pre[i][hr]&63)) nr|=1LL<<hr;
        if(rs1[i+1]==regmap_pre[i][hr]) nr|=1LL<<hr;
        if(rs2[i+1]==regmap_pre[i][hr]) nr|=1LL<<hr;
        if(us1[i+1]==(regs[i].regmap_entry[hr]&63)) nr|=1LL<<hr;
        if(us2[i+1]==(regs[i].regmap_entry[hr]&63)) nr|=1LL<<hr;
        if(rs1[i+1]==regs[i].regmap_entry[hr]) nr|=1LL<<hr;
        if(rs2[i+1]==regs[i].regmap_entry[hr]) nr|=1LL<<hr;
        if(dep1[i+1]&&!((unneeded_reg_upper[i]>>dep1[i+1])&1)) {
          if(dep1[i+1]==(regmap_pre[i][hr]&63)) nr|=1LL<<hr;
          if(dep2[i+1]==(regmap_pre[i][hr]&63)) nr|=1LL<<hr;
        }
        if(dep2[i+1]&&!((unneeded_reg_upper[i]>>dep2[i+1])&1)) {
          if(dep1[i+1]==(regs[i].regmap_entry[hr]&63)) nr|=1LL<<hr;
          if(dep2[i+1]==(regs[i].regmap_entry[hr]&63)) nr|=1LL<<hr;
        }
        if(itype[i+1]==STORE || itype[i+1]==STORELR || (opcode[i+1]&0x3b)==0x39) {
          if(regmap_pre[i][hr]==INVCP) nr|=1LL<<hr;
          if(regs[i].regmap_entry[hr]==INVCP) nr|=1LL<<hr;
        }
      }
    }
    else if(itype[i]==SYSCALL)
    {
      // SYSCALL instruction (software interrupt)
      nr=0LL;
    }
    else if(itype[i]==COP0 && (source[i]&0x3f)==0x18)
    {
      // ERET instruction (return from interrupt)
      nr=0LL;
    }
    else // Non-branch
    {
      if(i<slen-1) {
        for(hr=0;hr<HOST_REGS;hr++) {
          if(regmap_pre[i+1][hr]>=0&&get_reg(regs[i+1].regmap_entry,regmap_pre[i+1][hr])<0) nr&=~(1LL<<hr);
          if(regs[i].regmap[hr]!=regmap_pre[i+1][hr]) nr&=~(1LL<<hr);
          if(regs[i].regmap[hr]!=regmap_pre[i][hr]) nr&=~(1LL<<hr);
          if(regs[i].regmap[hr]<0) nr&=~(1LL<<hr);
        }
      }
    }
    for(hr=0;hr<HOST_REGS;hr++)
    {
      // Overwritten registers are not needed
      if(rt1[i]&&rt1[i]==(regs[i].regmap[hr]&63)) nr&=~(1LL<<hr);
      if(rt2[i]&&rt2[i]==(regs[i].regmap[hr]&63)) nr&=~(1LL<<hr);
      if(FTEMP==(regs[i].regmap[hr]&63)) nr&=~(1LL<<hr);
      // Source registers are needed
      if(us1[i]==(regmap_pre[i][hr]&63)) nr|=1LL<<hr;
      if(us2[i]==(regmap_pre[i][hr]&63)) nr|=1LL<<hr;
      if(rs1[i]==regmap_pre[i][hr]) nr|=1LL<<hr;
      if(rs2[i]==regmap_pre[i][hr]) nr|=1LL<<hr;
      if(us1[i]==(regs[i].regmap_entry[hr]&63)) nr|=1LL<<hr;
      if(us2[i]==(regs[i].regmap_entry[hr]&63)) nr|=1LL<<hr;
      if(rs1[i]==regs[i].regmap_entry[hr]) nr|=1LL<<hr;
      if(rs2[i]==regs[i].regmap_entry[hr]) nr|=1LL<<hr;
      if(dep1[i]&&!((unneeded_reg_upper[i]>>dep1[i])&1)) {
        if(dep1[i]==(regmap_pre[i][hr]&63)) nr|=1LL<<hr;
        if(dep1[i]==(regs[i].regmap_entry[hr]&63)) nr|=1LL<<hr;
      }
      if(dep2[i]&&!((unneeded_reg_upper[i]>>dep2[i])&1)) {
        if(dep2[i]==(regmap_pre[i][hr]&63)) nr|=1LL<<hr;
        if(dep2[i]==(regs[i].regmap_entry[hr]&63)) nr|=1LL<<hr;
      }
      if(itype[i]==STORE || itype[i]==STORELR || (opcode[i]&0x3b)==0x39) {
        if(regmap_pre[i][hr]==INVCP) nr|=1LL<<hr;
        if(regs[i].regmap_entry[hr]==INVCP) nr|=1LL<<hr;
      }
      // Don't store a register immediately after writing it,
      // may prevent dual-issue.
      // But do so if this is a branch target, otherwise we
      // might have to load the register before the branch.
      if(i>0&&!bt[i]&&((regs[i].wasdirty>>hr)&1)) {
        if((regmap_pre[i][hr]>0&&regmap_pre[i][hr]<64&&!((unneeded_reg[i]>>regmap_pre[i][hr])&1)) ||
           (regmap_pre[i][hr]>64&&!((unneeded_reg_upper[i]>>(regmap_pre[i][hr]&63))&1)) ) {
          if(rt1[i-1]==(regmap_pre[i][hr]&63)) nr|=1LL<<hr;
          if(rt2[i-1]==(regmap_pre[i][hr]&63)) nr|=1LL<<hr;
        }
        if((regs[i].regmap_entry[hr]>0&&regs[i].regmap_entry[hr]<64&&!((unneeded_reg[i]>>regs[i].regmap_entry[hr])&1)) ||
           (regs[i].regmap_entry[hr]>64&&!((unneeded_reg_upper[i]>>(regs[i].regmap_entry[hr]&63))&1)) ) {
          if(rt1[i-1]==(regs[i].regmap_entry[hr]&63)) nr|=1LL<<hr;
          if(rt2[i-1]==(regs[i].regmap_entry[hr]&63)) nr|=1LL<<hr;
        }
      }
    }
    // Cycle count is needed at branches.  Assume it is needed at the target too.
    if(i==0||bt[i]||itype[i]==CJUMP||itype[i]==FJUMP||itype[i]==SPAN) {
      if(regmap_pre[i][HOST_CCREG]==CCREG) nr|=1LL<<HOST_CCREG;
      if(regs[i].regmap_entry[HOST_CCREG]==CCREG) nr|=1LL<<HOST_CCREG;
    }
    // Save it
    needed_reg[i]=nr;

    // Deallocate unneeded registers
    for(hr=0;hr<HOST_REGS;hr++)
    {
      if(!((nr>>hr)&1)) {
        if(regs[i].regmap_entry[hr]!=CCREG) regs[i].regmap_entry[hr]=-1;
        if((regs[i].regmap[hr]&63)!=rs1[i] && (regs[i].regmap[hr]&63)!=rs2[i] &&
           (regs[i].regmap[hr]&63)!=rt1[i] && (regs[i].regmap[hr]&63)!=rt2[i] &&
           (regs[i].regmap[hr]&63)!=PTEMP && (regs[i].regmap[hr]&63)!=CCREG)
        {
          if(itype[i]!=RJUMP&&itype[i]!=UJUMP&&(source[i]>>16)!=0x1000)
          {
            if(likely[i]) {
              regs[i].regmap[hr]=-1;
              regs[i].isconst&=~(1<<hr);
              if(i<slen-2) {
                regmap_pre[i+2][hr]=-1;
                regs[i+2].wasconst&=~(1<<hr);
              }
            }
          }
        }
        if(itype[i]==RJUMP||itype[i]==UJUMP||itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP)
        {
          int d1=0,d2=0,map=0,temp=0;
          if(get_reg(regs[i].regmap,rt1[i+1]|64)>=0||get_reg(branch_regs[i].regmap,rt1[i+1]|64)>=0)
          {
            d1=dep1[i+1];
            d2=dep2[i+1];
          }
          if(using_tlb) {
            if(itype[i+1]==LOAD || itype[i+1]==LOADLR ||
               itype[i+1]==STORE || itype[i+1]==STORELR ||
               itype[i+1]==C1LS )
            map=TLREG;
          } else
          if(itype[i+1]==STORE || itype[i+1]==STORELR || (opcode[i+1]&0x3b)==0x39) {
            map=INVCP;
          }
          if(itype[i+1]==LOADLR || itype[i+1]==STORELR ||
             itype[i+1]==C1LS )
            temp=FTEMP;
          if((regs[i].regmap[hr]&63)!=rs1[i] && (regs[i].regmap[hr]&63)!=rs2[i] &&
             (regs[i].regmap[hr]&63)!=rt1[i] && (regs[i].regmap[hr]&63)!=rt2[i] &&
             (regs[i].regmap[hr]&63)!=rt1[i+1] && (regs[i].regmap[hr]&63)!=rt2[i+1] &&
             (regs[i].regmap[hr]^64)!=us1[i+1] && (regs[i].regmap[hr]^64)!=us2[i+1] &&
             (regs[i].regmap[hr]^64)!=d1 && (regs[i].regmap[hr]^64)!=d2 &&
             regs[i].regmap[hr]!=rs1[i+1] && regs[i].regmap[hr]!=rs2[i+1] &&
             (regs[i].regmap[hr]&63)!=temp && regs[i].regmap[hr]!=PTEMP &&
             regs[i].regmap[hr]!=RHASH && regs[i].regmap[hr]!=RHTBL &&
             regs[i].regmap[hr]!=RTEMP && regs[i].regmap[hr]!=CCREG &&
             regs[i].regmap[hr]!=map )
          {
            regs[i].regmap[hr]=-1;
            regs[i].isconst&=~(1<<hr);
            if((branch_regs[i].regmap[hr]&63)!=rs1[i] && (branch_regs[i].regmap[hr]&63)!=rs2[i] &&
               (branch_regs[i].regmap[hr]&63)!=rt1[i] && (branch_regs[i].regmap[hr]&63)!=rt2[i] &&
               (branch_regs[i].regmap[hr]&63)!=rt1[i+1] && (branch_regs[i].regmap[hr]&63)!=rt2[i+1] &&
               (branch_regs[i].regmap[hr]^64)!=us1[i+1] && (branch_regs[i].regmap[hr]^64)!=us2[i+1] &&
               (branch_regs[i].regmap[hr]^64)!=d1 && (branch_regs[i].regmap[hr]^64)!=d2 &&
               branch_regs[i].regmap[hr]!=rs1[i+1] && branch_regs[i].regmap[hr]!=rs2[i+1] &&
               (branch_regs[i].regmap[hr]&63)!=temp && branch_regs[i].regmap[hr]!=PTEMP &&
               branch_regs[i].regmap[hr]!=RHASH && branch_regs[i].regmap[hr]!=RHTBL &&
               branch_regs[i].regmap[hr]!=RTEMP && branch_regs[i].regmap[hr]!=CCREG &&
               branch_regs[i].regmap[hr]!=map)
            {
              branch_regs[i].regmap[hr]=-1;
              branch_regs[i].regmap_entry[hr]=-1;
              if(itype[i]!=RJUMP&&itype[i]!=UJUMP&&(source[i]>>16)!=0x1000)
              {
                if(!likely[i]&&i<slen-2) {
                  regmap_pre[i+2][hr]=-1;
                  regs[i+2].wasconst&=~(1<<hr);
                }
              }
            }
          }
        }
        else
        {
          // Non-branch
          if(i>0)
          {
            int d1=0,d2=0,map=-1,temp=-1;
            if(get_reg(regs[i].regmap,rt1[i]|64)>=0)
            {
              d1=dep1[i];
              d2=dep2[i];
            }
            if(using_tlb) {
              if(itype[i]==LOAD || itype[i]==LOADLR ||
                 itype[i]==STORE || itype[i]==STORELR ||
                 itype[i]==C1LS )
              map=TLREG;
            } else if(itype[i]==STORE || itype[i]==STORELR || (opcode[i]&0x3b)==0x39) {
              map=INVCP;
            }
            if(itype[i]==LOADLR || itype[i]==STORELR ||
               itype[i]==C1LS )
              temp=FTEMP;
            if((regs[i].regmap[hr]&63)!=rt1[i] && (regs[i].regmap[hr]&63)!=rt2[i] &&
               (regs[i].regmap[hr]^64)!=us1[i] && (regs[i].regmap[hr]^64)!=us2[i] &&
               (regs[i].regmap[hr]^64)!=d1 && (regs[i].regmap[hr]^64)!=d2 &&
               regs[i].regmap[hr]!=rs1[i] && regs[i].regmap[hr]!=rs2[i] &&
               (regs[i].regmap[hr]&63)!=temp && regs[i].regmap[hr]!=map &&
               (itype[i]!=SPAN||regs[i].regmap[hr]!=CCREG))
            {
              if(i<slen-1&&!is_ds[i]) {
                if(regmap_pre[i+1][hr]!=-1 || regs[i].regmap[hr]!=-1)
                if(regmap_pre[i+1][hr]!=regs[i].regmap[hr])
                if(regs[i].regmap[hr]<64||!((regs[i].was32>>(regs[i].regmap[hr]&63))&1))
                {
                  DebugMessage(M64MSG_VERBOSE, "fail: %x (%d %d!=%d)",start+i*4,hr,regmap_pre[i+1][hr],regs[i].regmap[hr]);
                  assert(regmap_pre[i+1][hr]==regs[i].regmap[hr]);
                }
                regmap_pre[i+1][hr]=-1;
                if(regs[i+1].regmap_entry[hr]==CCREG) regs[i+1].regmap_entry[hr]=-1;
                regs[i+1].wasconst&=~(1<<hr);
              }
              regs[i].regmap[hr]=-1;
              regs[i].isconst&=~(1<<hr);
            }
          }
        }
      }
    }
  }

  /* Pass 5 - Pre-allocate registers */

  // If a register is allocated during a loop, try to allocate it for the
  // entire loop, if possible.  This avoids loading/storing registers
  // inside of the loop.

  signed char f_regmap[HOST_REGS];
  clear_all_regs(f_regmap);
  for(i=0;i<slen-1;i++)
  {
    if(itype[i]==UJUMP||itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP)
    {
      if(ba[i]<start||ba[i]>=(start+i*4)) continue; // jump out of this block or forward branch
      if(itype[i+1]==NOP||itype[i+1]==MOV||itype[i+1]==ALU
      ||itype[i+1]==SHIFTIMM||itype[i+1]==IMM16||itype[i+1]==LOAD
      ||itype[i+1]==STORE||itype[i+1]==STORELR||itype[i+1]==C1LS
      ||itype[i+1]==SHIFT||itype[i+1]==COP1||itype[i+1]==FLOAT
      ||itype[i+1]==FCOMP||itype[i+1]==FCONV)
      {
        int t=(ba[i]-start)>>2;
        if(t<=0||(itype[t-1]==UJUMP||itype[t-1]==RJUMP||itype[t-1]==CJUMP||itype[t-1]==SJUMP||itype[t-1]==FJUMP)) continue; // loop_preload can't handle jumps into delay slots
        if(t>=2&&(itype[t-2]==UJUMP||itype[t-2]==RJUMP)&&rt1[t-2]==31) continue; // call/ret assumes no registers allocated
        for(hr=0;hr<HOST_REGS;hr++)
        {
          if(regs[i].regmap[hr]>64) {
            if(!((regs[i].dirty>>hr)&1))
              f_regmap[hr]=regs[i].regmap[hr];
            else f_regmap[hr]=-1;
          }
          else if(regs[i].regmap[hr]>=0) {
            if(f_regmap[hr]!=regs[i].regmap[hr]) {
              // dealloc old register
              int n;
              for(n=0;n<HOST_REGS;n++)
              {
                if(f_regmap[n]==regs[i].regmap[hr]) {f_regmap[n]=-1;}
              }
              // and alloc new one
              f_regmap[hr]=regs[i].regmap[hr];
            }
          }
          if(branch_regs[i].regmap[hr]>64) {
            if(!((branch_regs[i].dirty>>hr)&1))
              f_regmap[hr]=branch_regs[i].regmap[hr];
            else f_regmap[hr]=-1;
          }
          else if(branch_regs[i].regmap[hr]>=0) {
            if(f_regmap[hr]!=branch_regs[i].regmap[hr]) {
              // dealloc old register
              int n;
              for(n=0;n<HOST_REGS;n++)
              {
                if(f_regmap[n]==branch_regs[i].regmap[hr]) {f_regmap[n]=-1;}
              }
              // and alloc new one
              f_regmap[hr]=branch_regs[i].regmap[hr];
            }
          }
          if(ooo[i]) {
            if(count_free_regs(regs[i].regmap)<=minimum_free_regs[i+1])
              f_regmap[hr]=branch_regs[i].regmap[hr];
          }else{
            if(count_free_regs(branch_regs[i].regmap)<=minimum_free_regs[i+1])
              f_regmap[hr]=branch_regs[i].regmap[hr];
          }
          // Avoid dirty->clean transition
          #ifdef DESTRUCTIVE_WRITEBACK
          if(t>0) if(get_reg(regmap_pre[t],f_regmap[hr])>=0) if((regs[t].wasdirty>>get_reg(regmap_pre[t],f_regmap[hr]))&1) f_regmap[hr]=-1;
          #endif
          // This check is only strictly required in the DESTRUCTIVE_WRITEBACK
          // case above, however it's always a good idea.  We can't hoist the
          // load if the register was already allocated, so there's no point
          // wasting time analyzing most of these cases.  It only "succeeds"
          // when the mapping was different and the load can be replaced with
          // a mov, which is of negligible benefit.  So such cases are
          // skipped below.
          if(f_regmap[hr]>0) {
            if(regs[t].regmap[hr]==f_regmap[hr]||(regs[t].regmap_entry[hr]<0&&get_reg(regmap_pre[t],f_regmap[hr])<0)) {
              int r=f_regmap[hr];
              for(j=t;j<=i;j++)
              {
                //DebugMessage(M64MSG_VERBOSE, "Test %x -> %x, %x %d/%d",start+i*4,ba[i],start+j*4,hr,r);
                if(r<34&&((unneeded_reg[j]>>r)&1)) break;
                if(r>63&&((unneeded_reg_upper[j]>>(r&63))&1)) break;
                if(r>63) {
                  // NB This can exclude the case where the upper-half
                  // register is lower numbered than the lower-half
                  // register.  Not sure if it's worth fixing...
                  if(get_reg(regs[j].regmap,r&63)<0) break;
                  if(get_reg(regs[j].regmap_entry,r&63)<0) break;
                  if(regs[j].is32&(1LL<<(r&63))) break;
                }
                if(regs[j].regmap[hr]==f_regmap[hr]&&(f_regmap[hr]&63)<TEMPREG) {
                  //DebugMessage(M64MSG_VERBOSE, "Hit %x -> %x, %x %d/%d",start+i*4,ba[i],start+j*4,hr,r);
                  int k;
                  if(regs[i].regmap[hr]==-1&&branch_regs[i].regmap[hr]==-1) {
                    if(get_reg(regs[i].regmap,f_regmap[hr])>=0) break;
                    if(get_reg(branch_regs[i].regmap,f_regmap[hr])>=0) break;
                    if(get_reg(regs[i+2].regmap,f_regmap[hr])>=0) break;
                    if(r>63) {
                      if(get_reg(regs[i].regmap,r&63)<0) break;
                      if(get_reg(branch_regs[i].regmap,r&63)<0) break;
                    }
                    k=i;
                    while(k>1&&regs[k-1].regmap[hr]==-1) {
                      if(count_free_regs(regs[k-1].regmap)<=minimum_free_regs[k-1]) {
                        //DebugMessage(M64MSG_VERBOSE, "no free regs for store %x",start+(k-1)*4);
                        break;
                      }
                      if(get_reg(regs[k-1].regmap,f_regmap[hr])>=0) {
                        //DebugMessage(M64MSG_VERBOSE, "no-match due to different register");
                        break;
                      }
                      if(itype[k-2]==UJUMP||itype[k-2]==RJUMP||itype[k-2]==CJUMP||itype[k-2]==SJUMP||itype[k-2]==FJUMP) {
                        //DebugMessage(M64MSG_VERBOSE, "no-match due to branch");
                        break;
                      }
                      // call/ret fast path assumes no registers allocated
                      if(k>2&&(itype[k-3]==UJUMP||itype[k-3]==RJUMP)&&rt1[k-3]==31) {
                        break;
                      }
                      if(r>63) {
                        // NB This can exclude the case where the upper-half
                        // register is lower numbered than the lower-half
                        // register.  Not sure if it's worth fixing...
                        if(get_reg(regs[k-1].regmap,r&63)<0) break;
                        if(regs[k-1].is32&(1LL<<(r&63))) break;
                      }
                      k--;
                    }
                    if(i<slen-1) {
                      if((regs[k].is32&(1LL<<(f_regmap[hr]&63)))!=
                        (regs[i+2].was32&(1LL<<(f_regmap[hr]&63)))) {
                        //DebugMessage(M64MSG_VERBOSE, "bad match after branch");
                        break;
                      }
                    }
                    if(regs[k-1].regmap[hr]==f_regmap[hr]&&regmap_pre[k][hr]==f_regmap[hr]) {
                      //DebugMessage(M64MSG_VERBOSE, "Extend r%d, %x ->",hr,start+k*4);
                      while(k<i) {
                        regs[k].regmap_entry[hr]=f_regmap[hr];
                        regs[k].regmap[hr]=f_regmap[hr];
                        regmap_pre[k+1][hr]=f_regmap[hr];
                        regs[k].wasdirty&=~(1LL<<hr);
                        regs[k].dirty&=~(1LL<<hr);
                        regs[k].wasdirty|=(1LL<<hr)&regs[k-1].dirty;
                        regs[k].dirty|=(1LL<<hr)&regs[k].wasdirty;
                        regs[k].wasconst&=~(1LL<<hr);
                        regs[k].isconst&=~(1LL<<hr);
                        k++;
                      }
                    }
                    else {
                      //DebugMessage(M64MSG_VERBOSE, "Fail Extend r%d, %x ->",hr,start+k*4);
                      break;
                    }
                    assert(regs[i-1].regmap[hr]==f_regmap[hr]);
                    if(regs[i-1].regmap[hr]==f_regmap[hr]&&regmap_pre[i][hr]==f_regmap[hr]) {
                      //DebugMessage(M64MSG_VERBOSE, "OK fill %x (r%d)",start+i*4,hr);
                      regs[i].regmap_entry[hr]=f_regmap[hr];
                      regs[i].regmap[hr]=f_regmap[hr];
                      regs[i].wasdirty&=~(1LL<<hr);
                      regs[i].dirty&=~(1LL<<hr);
                      regs[i].wasdirty|=(1LL<<hr)&regs[i-1].dirty;
                      regs[i].dirty|=(1LL<<hr)&regs[i-1].dirty;
                      regs[i].wasconst&=~(1<<hr);
                      regs[i].isconst&=~(1<<hr);
                      branch_regs[i].regmap_entry[hr]=f_regmap[hr];
                      branch_regs[i].wasdirty&=~(1LL<<hr);
                      branch_regs[i].wasdirty|=(1LL<<hr)&regs[i].dirty;
                      branch_regs[i].regmap[hr]=f_regmap[hr];
                      branch_regs[i].dirty&=~(1LL<<hr);
                      branch_regs[i].dirty|=(1LL<<hr)&regs[i].dirty;
                      branch_regs[i].wasconst&=~(1<<hr);
                      branch_regs[i].isconst&=~(1<<hr);
                      if(itype[i]!=RJUMP&&itype[i]!=UJUMP&&(source[i]>>16)!=0x1000) {
                        regmap_pre[i+2][hr]=f_regmap[hr];
                        regs[i+2].wasdirty&=~(1LL<<hr);
                        regs[i+2].wasdirty|=(1LL<<hr)&regs[i].dirty;
                        assert((branch_regs[i].is32&(1LL<<(f_regmap[hr]&63)))==
                          (regs[i+2].was32&(1LL<<(f_regmap[hr]&63))));
                      }
                    }
                  }
                  for(k=t;k<j;k++) {
                    // Alloc register clean at beginning of loop,
                    // but may dirty it in pass 6
                    regs[k].regmap_entry[hr]=f_regmap[hr];
                    regs[k].regmap[hr]=f_regmap[hr];
                    regs[k].dirty&=~(1LL<<hr);
                    regs[k].wasconst&=~(1<<hr);
                    regs[k].isconst&=~(1<<hr);
                    if(itype[k]==UJUMP||itype[k]==RJUMP||itype[k]==CJUMP||itype[k]==SJUMP||itype[k]==FJUMP) {
                      branch_regs[k].regmap_entry[hr]=f_regmap[hr];
                      branch_regs[k].regmap[hr]=f_regmap[hr];
                      branch_regs[k].dirty&=~(1LL<<hr);
                      branch_regs[k].wasconst&=~(1<<hr);
                      branch_regs[k].isconst&=~(1<<hr);
                      if(itype[k]!=RJUMP&&itype[k]!=UJUMP&&(source[k]>>16)!=0x1000) {
                        regmap_pre[k+2][hr]=f_regmap[hr];
                        regs[k+2].wasdirty&=~(1LL<<hr);
                        assert((branch_regs[k].is32&(1LL<<(f_regmap[hr]&63)))==
                          (regs[k+2].was32&(1LL<<(f_regmap[hr]&63))));//DebugMessage(M64MSG_VERBOSE, "Extend CC, %x -> %x",start+k*4,start+j*4);
                      }
                    }
                    else
                    {
                      regmap_pre[k+1][hr]=f_regmap[hr];
                      regs[k+1].wasdirty&=~(1LL<<hr);
                    }
                  }
                  if(regs[j].regmap[hr]==f_regmap[hr])
                    regs[j].regmap_entry[hr]=f_regmap[hr];
                  break;
                }
                if(j==i) break;
                if(regs[j].regmap[hr]>=0)
                  break;
                if(get_reg(regs[j].regmap,f_regmap[hr])>=0) {
                  //DebugMessage(M64MSG_VERBOSE, "no-match due to different register");
                  break;
                }
                if((regs[j+1].is32&(1LL<<(f_regmap[hr]&63)))!=(regs[j].is32&(1LL<<(f_regmap[hr]&63)))) {
                  //DebugMessage(M64MSG_VERBOSE, "32/64 mismatch %x %d",start+j*4,hr);
                  break;
                }
                if(itype[j]==UJUMP||itype[j]==RJUMP||(source[j]>>16)==0x1000)
                {
                  // Stop on unconditional branch
                  break;
                }
                if(itype[j]==CJUMP||itype[j]==SJUMP||itype[j]==FJUMP)
                {
                  if(branch_regs[j].regmap[hr]>=0)
                    break;
                  if(ooo[j]) {
                    if(count_free_regs(regs[j].regmap)<=minimum_free_regs[j+1])
                      break;
                  }else{
                    if(count_free_regs(branch_regs[j].regmap)<=minimum_free_regs[j+1])
                      break;
                  }
                  if(get_reg(branch_regs[j].regmap,f_regmap[hr])>=0) {
                    //DebugMessage(M64MSG_VERBOSE, "no-match due to different register (branch)");
                    break;
                  }
                }
                if(count_free_regs(regs[j].regmap)<=minimum_free_regs[j]) {
                  //DebugMessage(M64MSG_VERBOSE, "No free regs for store %x",start+j*4);
                  break;
                }
                if(f_regmap[hr]>=64) {
                  if(regs[j].is32&(1LL<<(f_regmap[hr]&63))) {
                    break;
                  }
                  else
                  {
                    if(get_reg(regs[j].regmap,f_regmap[hr]&63)<0) {
                      break;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }else{
      // Non branch or undetermined branch target
      for(hr=0;hr<HOST_REGS;hr++)
      {
        if(hr!=EXCLUDE_REG) {
          if(regs[i].regmap[hr]>64) {
            if(!((regs[i].dirty>>hr)&1))
              f_regmap[hr]=regs[i].regmap[hr];
          }
          else if(regs[i].regmap[hr]>=0) {
            if(f_regmap[hr]!=regs[i].regmap[hr]) {
              // dealloc old register
              int n;
              for(n=0;n<HOST_REGS;n++)
              {
                if(f_regmap[n]==regs[i].regmap[hr]) {f_regmap[n]=-1;}
              }
              // and alloc new one
              f_regmap[hr]=regs[i].regmap[hr];
            }
          }
        }
      }
      // Try to restore cycle count at branch targets
      if(bt[i]) {
        for(j=i;j<slen-1;j++) {
          if(regs[j].regmap[HOST_CCREG]!=-1) break;
          if(count_free_regs(regs[j].regmap)<=minimum_free_regs[j]) {
            //DebugMessage(M64MSG_VERBOSE, "no free regs for store %x",start+j*4);
            break;
          }
        }
        if(regs[j].regmap[HOST_CCREG]==CCREG) {
          int k=i;
          //DebugMessage(M64MSG_VERBOSE, "Extend CC, %x -> %x",start+k*4,start+j*4);
          while(k<j) {
            regs[k].regmap_entry[HOST_CCREG]=CCREG;
            regs[k].regmap[HOST_CCREG]=CCREG;
            regmap_pre[k+1][HOST_CCREG]=CCREG;
            regs[k+1].wasdirty|=1LL<<HOST_CCREG;
            regs[k].dirty|=1LL<<HOST_CCREG;
            regs[k].wasconst&=~(1<<HOST_CCREG);
            regs[k].isconst&=~(1<<HOST_CCREG);
            k++;
          }
          regs[j].regmap_entry[HOST_CCREG]=CCREG;
        }
        // Work backwards from the branch target
        if(j>i&&f_regmap[HOST_CCREG]==CCREG)
        {
          //DebugMessage(M64MSG_VERBOSE, "Extend backwards");
          int k;
          k=i;
          while(regs[k-1].regmap[HOST_CCREG]==-1) {
            if(count_free_regs(regs[k-1].regmap)<=minimum_free_regs[k-1]) {
              //DebugMessage(M64MSG_VERBOSE, "no free regs for store %x",start+(k-1)*4);
              break;
            }
            k--;
          }
          if(regs[k-1].regmap[HOST_CCREG]==CCREG) {
            //DebugMessage(M64MSG_VERBOSE, "Extend CC, %x ->",start+k*4);
            while(k<=i) {
              regs[k].regmap_entry[HOST_CCREG]=CCREG;
              regs[k].regmap[HOST_CCREG]=CCREG;
              regmap_pre[k+1][HOST_CCREG]=CCREG;
              regs[k+1].wasdirty|=1LL<<HOST_CCREG;
              regs[k].dirty|=1LL<<HOST_CCREG;
              regs[k].wasconst&=~(1<<HOST_CCREG);
              regs[k].isconst&=~(1<<HOST_CCREG);
              k++;
            }
          }
          else {
            //DebugMessage(M64MSG_VERBOSE, "Fail Extend CC, %x ->",start+k*4);
          }
        }
      }
      if(itype[i]!=STORE&&itype[i]!=STORELR&&itype[i]!=C1LS&&itype[i]!=SHIFT&&
         itype[i]!=NOP&&itype[i]!=MOV&&itype[i]!=ALU&&itype[i]!=SHIFTIMM&&
         itype[i]!=IMM16&&itype[i]!=LOAD&&itype[i]!=COP1&&itype[i]!=FLOAT&&
         itype[i]!=FCONV&&itype[i]!=FCOMP)
      {
        memcpy(f_regmap,regs[i].regmap,sizeof(f_regmap));
      }
    }
  }

  // Cache memory offset or tlb map pointer if a register is available
  #ifndef HOST_IMM_ADDR32
  #ifndef RAM_OFFSET
  if(using_tlb)
  #endif
  {
    int earliest_available[HOST_REGS];
    int loop_start[HOST_REGS];
    int score[HOST_REGS];
    int end[HOST_REGS];
    int reg=using_tlb?MMREG:ROREG;

    // Init
    for(hr=0;hr<HOST_REGS;hr++) {
      score[hr]=0;earliest_available[hr]=0;
      loop_start[hr]=MAXBLOCK;
    }
    for(i=0;i<slen-1;i++)
    {
      // Can't do anything if no registers are available
      if(count_free_regs(regs[i].regmap)<=minimum_free_regs[i]) {
        for(hr=0;hr<HOST_REGS;hr++) {
          score[hr]=0;earliest_available[hr]=i+1;
          loop_start[hr]=MAXBLOCK;
        }
      }
      if(itype[i]==UJUMP||itype[i]==RJUMP||itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP) {
        if(!ooo[i]) {
          if(count_free_regs(branch_regs[i].regmap)<=minimum_free_regs[i+1]) {
            for(hr=0;hr<HOST_REGS;hr++) {
              score[hr]=0;earliest_available[hr]=i+1;
              loop_start[hr]=MAXBLOCK;
            }
          }
        }else{
          if(count_free_regs(regs[i].regmap)<=minimum_free_regs[i+1]) {
            for(hr=0;hr<HOST_REGS;hr++) {
              score[hr]=0;earliest_available[hr]=i+1;
              loop_start[hr]=MAXBLOCK;
            }
          }
        }
      }
      // Mark unavailable registers
      for(hr=0;hr<HOST_REGS;hr++) {
        if(regs[i].regmap[hr]>=0) {
          score[hr]=0;earliest_available[hr]=i+1;
          loop_start[hr]=MAXBLOCK;
        }
        if(itype[i]==UJUMP||itype[i]==RJUMP||itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP) {
          if(branch_regs[i].regmap[hr]>=0) {
            score[hr]=0;earliest_available[hr]=i+2;
            loop_start[hr]=MAXBLOCK;
          }
        }
      }
      // No register allocations after unconditional jumps
      if(itype[i]==UJUMP||itype[i]==RJUMP||(source[i]>>16)==0x1000)
      {
        for(hr=0;hr<HOST_REGS;hr++) {
          score[hr]=0;earliest_available[hr]=i+2;
          loop_start[hr]=MAXBLOCK;
        }
        i++; // Skip delay slot too
        //DebugMessage(M64MSG_VERBOSE, "skip delay slot: %x",start+i*4);
      }
      else
      // Possible match
      if(itype[i]==LOAD||itype[i]==LOADLR||
         itype[i]==STORE||itype[i]==STORELR||itype[i]==C1LS) {
        for(hr=0;hr<HOST_REGS;hr++) {
          if(hr!=EXCLUDE_REG) {
            end[hr]=i-1;
            for(j=i;j<slen-1;j++) {
              if(regs[j].regmap[hr]>=0) break;
              if(itype[j]==UJUMP||itype[j]==RJUMP||itype[j]==CJUMP||itype[j]==SJUMP||itype[j]==FJUMP) {
                if(branch_regs[j].regmap[hr]>=0) break;
                if(ooo[j]) {
                  if(count_free_regs(regs[j].regmap)<=minimum_free_regs[j+1]) break;
                }else{
                  if(count_free_regs(branch_regs[j].regmap)<=minimum_free_regs[j+1]) break;
                }
              }
              else if(count_free_regs(regs[j].regmap)<=minimum_free_regs[j]) break;
              if(itype[j]==UJUMP||itype[j]==RJUMP||itype[j]==CJUMP||itype[j]==SJUMP||itype[j]==FJUMP) {
                int t=(ba[j]-start)>>2;
                if(t<j&&t>=earliest_available[hr]) {
                  if(t==1||(t>1&&itype[t-2]!=UJUMP&&itype[t-2]!=RJUMP)||(t>1&&rt1[t-2]!=31)) { // call/ret assumes no registers allocated
                    // Score a point for hoisting loop invariant
                    if(t<loop_start[hr]) loop_start[hr]=t;
                    //DebugMessage(M64MSG_VERBOSE, "set loop_start: i=%x j=%x (%x)",start+i*4,start+j*4,start+t*4);
                    score[hr]++;
                    end[hr]=j;
                  }
                }
                else if(t<j) {
                  if(regs[t].regmap[hr]==reg) {
                    // Score a point if the branch target matches this register
                    score[hr]++;
                    end[hr]=j;
                  }
                }
                if(itype[j+1]==LOAD||itype[j+1]==LOADLR||
                   itype[j+1]==STORE||itype[j+1]==STORELR||itype[j+1]==C1LS) {
                  score[hr]++;
                  end[hr]=j;
                }
              }
              if(itype[j]==UJUMP||itype[j]==RJUMP||(source[j]>>16)==0x1000)
              {
                // Stop on unconditional branch
                break;
              }
              else
              if(itype[j]==LOAD||itype[j]==LOADLR||
                 itype[j]==STORE||itype[j]==STORELR||itype[j]==C1LS) {
                score[hr]++;
                end[hr]=j;
              }
            }
          }
        }
        // Find highest score and allocate that register
        int maxscore=0;
        for(hr=0;hr<HOST_REGS;hr++) {
          if(hr!=EXCLUDE_REG) {
            if(score[hr]>score[maxscore]) {
              maxscore=hr;
              //DebugMessage(M64MSG_VERBOSE, "highest score: %d %d (%x->%x)",score[hr],hr,start+i*4,start+end[hr]*4);
            }
          }
        }
        if(score[maxscore]>1)
        {
          if(i<loop_start[maxscore]) loop_start[maxscore]=i;
          for(j=loop_start[maxscore];j<slen&&j<=end[maxscore];j++) {
            //if(regs[j].regmap[maxscore]>=0) {DebugMessage(M64MSG_ERROR, "oops: %x %x was %d=%d",loop_start[maxscore]*4+start,j*4+start,maxscore,regs[j].regmap[maxscore]);}
            assert(regs[j].regmap[maxscore]<0);
            if(j>loop_start[maxscore]) regs[j].regmap_entry[maxscore]=reg;
            regs[j].regmap[maxscore]=reg;
            regs[j].dirty&=~(1LL<<maxscore);
            regs[j].wasconst&=~(1<<maxscore);
            regs[j].isconst&=~(1<<maxscore);
            if(itype[j]==UJUMP||itype[j]==RJUMP||itype[j]==CJUMP||itype[j]==SJUMP||itype[j]==FJUMP) {
              branch_regs[j].regmap[maxscore]=reg;
              branch_regs[j].wasdirty&=~(1LL<<maxscore);
              branch_regs[j].dirty&=~(1LL<<maxscore);
              branch_regs[j].wasconst&=~(1<<maxscore);
              branch_regs[j].isconst&=~(1<<maxscore);
              if(itype[j]!=RJUMP&&itype[j]!=UJUMP&&(source[j]>>16)!=0x1000) {
                regmap_pre[j+2][maxscore]=reg;
                regs[j+2].wasdirty&=~(1LL<<maxscore);
              }
              // loop optimization (loop_preload)
              int t=(ba[j]-start)>>2;
              if(t==loop_start[maxscore]) {
                if(t==1||(t>1&&itype[t-2]!=UJUMP&&itype[t-2]!=RJUMP)||(t>1&&rt1[t-2]!=31)) // call/ret assumes no registers allocated
                  regs[t].regmap_entry[maxscore]=reg;
              }
            }
            else
            {
              if(j<1||(itype[j-1]!=RJUMP&&itype[j-1]!=UJUMP&&itype[j-1]!=CJUMP&&itype[j-1]!=SJUMP&&itype[j-1]!=FJUMP)) {
                regmap_pre[j+1][maxscore]=reg;
                regs[j+1].wasdirty&=~(1LL<<maxscore);
              }
            }
          }
          i=j-1;
          if(itype[j-1]==RJUMP||itype[j-1]==UJUMP||itype[j-1]==CJUMP||itype[j-1]==SJUMP||itype[j-1]==FJUMP) i++; // skip delay slot
          for(hr=0;hr<HOST_REGS;hr++) {
            score[hr]=0;earliest_available[hr]=i+i;
            loop_start[hr]=MAXBLOCK;
          }
        }
      }
    }
  }
  #endif

  // This allocates registers (if possible) one instruction prior
  // to use, which can avoid a load-use penalty on certain CPUs.
  for(i=0;i<slen-1;i++)
  {
    // Avoid replacing an allocated temporary register
    int free_regs=count_free_regs(regs[i].regmap)-minimum_free_regs[i];

    if(!i||(itype[i-1]!=UJUMP&&itype[i-1]!=CJUMP&&itype[i-1]!=SJUMP&&itype[i-1]!=RJUMP&&itype[i-1]!=FJUMP))
    {
      if(!bt[i+1])
      {
        if(itype[i]==ALU||itype[i]==MOV||itype[i]==LOAD||itype[i]==SHIFTIMM||itype[i]==IMM16||(itype[i]==COP1&&opcode2[i]<3))
        {
          if(rs1[i+1]) {
            if((hr=get_reg(regs[i+1].regmap,rs1[i+1]))>=0)
            {
              if(regs[i].regmap[hr]<0&&regs[i+1].regmap_entry[hr]<0&&free_regs>0)
              {
                regs[i].regmap[hr]=regs[i+1].regmap[hr];
                regmap_pre[i+1][hr]=regs[i+1].regmap[hr];
                regs[i+1].regmap_entry[hr]=regs[i+1].regmap[hr];
                regs[i].isconst&=~(1<<hr);
                regs[i].isconst|=regs[i+1].isconst&(1<<hr);
                constmap[i][hr]=constmap[i+1][hr];
                regs[i+1].wasdirty&=~(1LL<<hr);
                regs[i].dirty&=~(1LL<<hr);
                free_regs--;
              }
            }
          }
          if(rs2[i+1]) {
            if((hr=get_reg(regs[i+1].regmap,rs2[i+1]))>=0)
            {
              if(regs[i].regmap[hr]<0&&regs[i+1].regmap_entry[hr]<0&&free_regs>0)
              {
                regs[i].regmap[hr]=regs[i+1].regmap[hr];
                regmap_pre[i+1][hr]=regs[i+1].regmap[hr];
                regs[i+1].regmap_entry[hr]=regs[i+1].regmap[hr];
                regs[i].isconst&=~(1<<hr);
                regs[i].isconst|=regs[i+1].isconst&(1<<hr);
                constmap[i][hr]=constmap[i+1][hr];
                regs[i+1].wasdirty&=~(1LL<<hr);
                regs[i].dirty&=~(1LL<<hr);
                free_regs--;
              }
            }
          }
          // Load source into target register
          if(lt1[i+1]&&get_reg(regs[i+1].regmap,rs1[i+1])<0) {
            if((hr=get_reg(regs[i+1].regmap,rt1[i+1]))>=0)
            {
              if(regs[i].regmap[hr]<0&&regs[i+1].regmap_entry[hr]<0&&free_regs>0)
              {
                regs[i].regmap[hr]=rs1[i+1];
                regmap_pre[i+1][hr]=rs1[i+1];
                regs[i+1].regmap_entry[hr]=rs1[i+1];
                regs[i].isconst&=~(1<<hr);
                regs[i].isconst|=regs[i+1].isconst&(1<<hr);
                constmap[i][hr]=constmap[i+1][hr];
                regs[i+1].wasdirty&=~(1LL<<hr);
                regs[i].dirty&=~(1LL<<hr);
                free_regs--;
              }
            }
          }
          // Preload map address
          #if (NEW_DYNAREC!=NEW_DYNAREC_X86) && (NEW_DYNAREC!=NEW_DYNAREC_X64)
          if(itype[i+1]==LOAD||itype[i+1]==LOADLR||itype[i+1]==STORE||itype[i+1]==STORELR||itype[i+1]==C1LS) {
            hr=get_reg(regs[i+1].regmap,TLREG);
            if(hr>=0) {
              int sr=get_reg(regs[i+1].regmap,rs1[i+1]);
              if(sr>=0&&((regs[i+1].wasconst>>sr)&1)) {
                int nr;
                if(regs[i].regmap[hr]<0&&regs[i+1].regmap_entry[hr]<0&&free_regs>0)
                {
                  regs[i].regmap[hr]=MGEN1+((i+1)&1);
                  regmap_pre[i+1][hr]=MGEN1+((i+1)&1);
                  regs[i+1].regmap_entry[hr]=MGEN1+((i+1)&1);
                  regs[i].isconst&=~(1<<hr);
                  regs[i].isconst|=regs[i+1].isconst&(1<<hr);
                  constmap[i][hr]=constmap[i+1][hr];
                  regs[i+1].wasdirty&=~(1LL<<hr);
                  regs[i].dirty&=~(1LL<<hr);
                  free_regs--;
                }
                else if((nr=get_reg2(regs[i].regmap,regs[i+1].regmap,-1))>=0&&free_regs>0)
                {
                  // move it to another register
                  regs[i+1].regmap[hr]=-1;
                  regmap_pre[i+2][hr]=-1;
                  regs[i+1].regmap[nr]=TLREG;
                  regmap_pre[i+2][nr]=TLREG;
                  regs[i].regmap[nr]=MGEN1+((i+1)&1);
                  regmap_pre[i+1][nr]=MGEN1+((i+1)&1);
                  regs[i+1].regmap_entry[nr]=MGEN1+((i+1)&1);
                  regs[i].isconst&=~(1<<nr);
                  regs[i+1].isconst&=~(1<<nr);
                  regs[i].dirty&=~(1LL<<nr);
                  regs[i+1].wasdirty&=~(1LL<<nr);
                  regs[i+1].dirty&=~(1LL<<nr);
                  regs[i+2].wasdirty&=~(1LL<<nr);
                  free_regs--;
                }
              }
            }
          }
          #endif
          // Address for load/store instruction (non-constant)
          if(itype[i+1]==LOAD||itype[i+1]==STORE||itype[i+1]==STORELR||opcode[i+1]==0x39||opcode[i+1]==0x3D) { // SWC1/SDC1
            if(get_reg(regs[i+1].regmap,rs1[i+1])<0) {
              hr=get_reg2(regs[i].regmap,regs[i+1].regmap,-1);
              if(hr<0) hr=get_reg(regs[i+1].regmap,-1);
              else {regs[i+1].regmap[hr]=AGEN1+((i+1)&1);regs[i+1].isconst&=~(1<<hr);}
              assert(hr>=0);
              if(regs[i].regmap[hr]<0&&regs[i+1].regmap_entry[hr]<0&&free_regs>0)
              {
                regs[i].regmap[hr]=rs1[i+1];
                regmap_pre[i+1][hr]=rs1[i+1];
                regs[i+1].regmap_entry[hr]=rs1[i+1];
                regs[i].isconst&=~(1<<hr);
                regs[i].isconst|=regs[i+1].isconst&(1<<hr);
                constmap[i][hr]=constmap[i+1][hr];
                regs[i+1].wasdirty&=~(1LL<<hr);
                regs[i].dirty&=~(1LL<<hr);
                free_regs--;
              }
            }
          }
          if(itype[i+1]==LOADLR||opcode[i+1]==0x31||opcode[i+1]==0x35) { // LWC1/LDC1
            if(get_reg(regs[i+1].regmap,rs1[i+1])<0) {
              int nr;
              hr=get_reg(regs[i+1].regmap,FTEMP);
              assert(hr>=0);
              if(regs[i].regmap[hr]<0&&regs[i+1].regmap_entry[hr]<0&&free_regs>0)
              {
                regs[i].regmap[hr]=rs1[i+1];
                regmap_pre[i+1][hr]=rs1[i+1];
                regs[i+1].regmap_entry[hr]=rs1[i+1];
                regs[i].isconst&=~(1<<hr);
                regs[i].isconst|=regs[i+1].isconst&(1<<hr);
                constmap[i][hr]=constmap[i+1][hr];
                regs[i+1].wasdirty&=~(1LL<<hr);
                regs[i].dirty&=~(1LL<<hr);
                free_regs--;
              }
              else if((nr=get_reg2(regs[i].regmap,regs[i+1].regmap,-1))>=0&&free_regs>0)
              {
                // move it to another register
                regs[i+1].regmap[hr]=-1;
                regmap_pre[i+2][hr]=-1;
                regs[i+1].regmap[nr]=FTEMP;
                regmap_pre[i+2][nr]=FTEMP;
                regs[i].regmap[nr]=rs1[i+1];
                regmap_pre[i+1][nr]=rs1[i+1];
                regs[i+1].regmap_entry[nr]=rs1[i+1];
                regs[i].isconst&=~(1<<nr);
                regs[i+1].isconst&=~(1<<nr);
                regs[i].dirty&=~(1LL<<nr);
                regs[i+1].wasdirty&=~(1LL<<nr);
                regs[i+1].dirty&=~(1LL<<nr);
                regs[i+2].wasdirty&=~(1LL<<nr);
                free_regs--;
              }
            }
          }
          if(itype[i+1]==LOAD||itype[i+1]==LOADLR||itype[i+1]==STORE||itype[i+1]==STORELR/*||itype[i+1]==C1LS*/) {
            if(itype[i+1]==LOADLR||opcode[i+1]==0x31||opcode[i+1]==0x35) // LWC1/LDC1
              hr=get_reg(regs[i+1].regmap,FTEMP);
            if(itype[i+1]==LOAD||itype[i+1]==STORE||itype[i+1]==STORELR||opcode[i+1]==0x39||opcode[i+1]==0x3D) { // SWC1/SDC1
              hr=get_reg(regs[i+1].regmap,AGEN1+((i+1)&1));
              if(hr<0) hr=get_reg(regs[i+1].regmap,-1);
            }
            if(hr>=0&&regs[i].regmap[hr]<0&&free_regs>0) {
              int rs=get_reg(regs[i+1].regmap,rs1[i+1]);
              if(rs>=0&&((regs[i+1].wasconst>>rs)&1)) {
                regs[i].regmap[hr]=AGEN1+((i+1)&1);
                regmap_pre[i+1][hr]=AGEN1+((i+1)&1);
                regs[i+1].regmap_entry[hr]=AGEN1+((i+1)&1);
                regs[i].isconst&=~(1<<hr);
                regs[i+1].wasdirty&=~(1LL<<hr);
                regs[i].dirty&=~(1LL<<hr);
              }
            }
          }
        }
      }
    }
  }

  /* Pass 6 - Optimize clean/dirty state */
  clean_registers(0,slen-1,1);

  /* Pass 7 - Identify 32-bit registers */

  provisional_r32();

  u_int r32=0;

  for (i=slen-1;i>=0;i--)
  {
    int hr;
    if(itype[i]==RJUMP||itype[i]==UJUMP||itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP)
    {
      if(ba[i]<start || ba[i]>=(start+slen*4))
      {
        // Branch out of this block, don't need anything
        r32=0;
      }
      else
      {
        // Internal branch
        // Need whatever matches the target
        // (and doesn't get overwritten by the delay slot instruction)
        r32=0;
        int t=(ba[i]-start)>>2;
        if(ba[i]>start+i*4) {
          // Forward branch
          if(!(requires_32bit[t]&~regs[i].was32))
            r32|=requires_32bit[t]&(~(1LL<<rt1[i+1]))&(~(1LL<<rt2[i+1]));
        }else{
          // Backward branch
          //if(!(regs[t].was32&~unneeded_reg_upper[t]&~regs[i].was32))
          //  r32|=regs[t].was32&~unneeded_reg_upper[t]&(~(1LL<<rt1[i+1]))&(~(1LL<<rt2[i+1]));
          if(!(pr32[t]&~regs[i].was32))
            r32|=pr32[t]&(~(1LL<<rt1[i+1]))&(~(1LL<<rt2[i+1]));
        }
      }
      // Conditional branch may need registers for following instructions
      if(itype[i]!=RJUMP&&itype[i]!=UJUMP&&(source[i]>>16)!=0x1000)
      {
        if(i<slen-2) {
          r32|=requires_32bit[i+2];
          r32&=regs[i].was32;
          // Mark this address as a branch target since it may be called
          // upon return from interrupt
          bt[i+2]=1;
        }
      }
      // Merge in delay slot
      if(!likely[i]) {
        // These are overwritten unless the branch is "likely"
        // and the delay slot is nullified if not taken
        r32&=~(1LL<<rt1[i+1]);
        r32&=~(1LL<<rt2[i+1]);
      }
      // Assume these are needed (delay slot)
      if(us1[i+1]>0)
      {
        if((regs[i].was32>>us1[i+1])&1) r32|=1LL<<us1[i+1];
      }
      if(us2[i+1]>0)
      {
        if((regs[i].was32>>us2[i+1])&1) r32|=1LL<<us2[i+1];
      }
      if(dep1[i+1]&&!((unneeded_reg_upper[i]>>dep1[i+1])&1))
      {
        if((regs[i].was32>>dep1[i+1])&1) r32|=1LL<<dep1[i+1];
      }
      if(dep2[i+1]&&!((unneeded_reg_upper[i]>>dep2[i+1])&1))
      {
        if((regs[i].was32>>dep2[i+1])&1) r32|=1LL<<dep2[i+1];
      }
    }
    else if(itype[i]==SYSCALL)
    {
      // SYSCALL instruction (software interrupt)
      r32=0;
    }
    else if(itype[i]==COP0 && (source[i]&0x3f)==0x18)
    {
      // ERET instruction (return from interrupt)
      r32=0;
    }
    // Check 32 bits
    r32&=~(1LL<<rt1[i]);
    r32&=~(1LL<<rt2[i]);
    if(us1[i]>0)
    {
      if((regs[i].was32>>us1[i])&1) r32|=1LL<<us1[i];
    }
    if(us2[i]>0)
    {
      if((regs[i].was32>>us2[i])&1) r32|=1LL<<us2[i];
    }
    if(dep1[i]&&!((unneeded_reg_upper[i]>>dep1[i])&1))
    {
      if((regs[i].was32>>dep1[i])&1) r32|=1LL<<dep1[i];
    }
    if(dep2[i]&&!((unneeded_reg_upper[i]>>dep2[i])&1))
    {
      if((regs[i].was32>>dep2[i])&1) r32|=1LL<<dep2[i];
    }
    requires_32bit[i]=r32;

    // Dirty registers which are 32-bit, require 32-bit input
    // as they will be written as 32-bit values
    for(hr=0;hr<HOST_REGS;hr++)
    {
      if(regs[i].regmap_entry[hr]>0&&regs[i].regmap_entry[hr]<64) {
        if((regs[i].was32>>regs[i].regmap_entry[hr])&(regs[i].wasdirty>>hr)&1) {
          if(!((unneeded_reg_upper[i]>>regs[i].regmap_entry[hr])&1))
          requires_32bit[i]|=1LL<<regs[i].regmap_entry[hr];
        }
      }
    }
    //requires_32bit[i]=is32[i]&~unneeded_reg_upper[i]; // DEBUG
  }

  if(itype[slen-1]==SPAN) {
    bt[slen-1]=1; // Mark as a branch target so instruction can restart after exception
  }

  /* Pass 8 - Assembly */
  linkcount=0;stubcount=0;
  ds=0;is_delayslot=0;
  cop1_usable=0;
  dirty_entry_count=0;
  #ifndef DESTRUCTIVE_WRITEBACK
  uint64_t is32_pre=0;
  u_int dirty_pre=0;
  #endif

  copy=NULL;
  copy=(char*)malloc((slen*4)+4);
  assert(copy);
  copy_size+=((slen*4)+4);
  //DebugMessage(M64MSG_VERBOSE, "Currently used memory for copy: %d",copy_size);

  uintptr_t beginning=(uintptr_t)out;
  if((u_int)addr&1) {
    ds=1;
    pagespan_ds();
  }
  for(i=0;i<slen;i++)
  {
#if ASSEM_DEBUG
    disassemble_inst(i);
#endif
    if(ds) {
      ds=0; // Skip delay slot
      if(bt[i]) assem_debug("OOPS - branch into delay slot");
      instr_addr[i]=0;
    } else {
      #ifndef DESTRUCTIVE_WRITEBACK
      if(i<2||(itype[i-2]!=UJUMP&&itype[i-2]!=RJUMP&&(source[i-2]>>16)!=0x1000))
      {
        wb_sx(regmap_pre[i],regs[i].regmap_entry,regs[i].wasdirty,is32_pre,regs[i].was32,
              unneeded_reg[i],unneeded_reg_upper[i]);
        wb_valid(regmap_pre[i],regs[i].regmap_entry,dirty_pre,regs[i].wasdirty,is32_pre,
              unneeded_reg[i],unneeded_reg_upper[i]);
      }
      if((itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP)&&!likely[i]) {
        is32_pre=branch_regs[i].is32;
        dirty_pre=branch_regs[i].dirty;
      }else{
        is32_pre=regs[i].is32;
        dirty_pre=regs[i].dirty;
      }
      #endif
      // write back
      if(i<2||(itype[i-2]!=UJUMP&&itype[i-2]!=RJUMP&&(source[i-2]>>16)!=0x1000))
      {
        wb_invalidate(regmap_pre[i],regs[i].regmap_entry,regs[i].wasdirty,regs[i].was32,
                      unneeded_reg[i],unneeded_reg_upper[i]);
        loop_preload(regmap_pre[i],regs[i].regmap_entry);
      }
      // branch target entry point
      instr_addr[i]=(uintptr_t)out;
      assem_debug("<->");
      // load regs
      if(regs[i].regmap_entry[HOST_CCREG]==CCREG&&regs[i].regmap[HOST_CCREG]!=CCREG)
        wb_register(CCREG,regs[i].regmap_entry,regs[i].wasdirty,regs[i].was32);
      load_regs(regs[i].regmap_entry,regs[i].regmap,regs[i].was32,rs1[i],rs2[i]);
      address_generation(i,&regs[i],regs[i].regmap_entry);
      load_consts(regmap_pre[i],regs[i].regmap,regs[i].was32,i);
      if(itype[i]==RJUMP||itype[i]==UJUMP||itype[i]==CJUMP||itype[i]==SJUMP||itype[i]==FJUMP)
      {
        // Load the delay slot registers if necessary
        if(rs1[i+1]!=rs1[i]&&rs1[i+1]!=rs2[i])
          load_regs(regs[i].regmap_entry,regs[i].regmap,regs[i].was32,rs1[i+1],rs1[i+1]);
        if(rs2[i+1]!=rs1[i+1]&&rs2[i+1]!=rs1[i]&&rs2[i+1]!=rs2[i])
          load_regs(regs[i].regmap_entry,regs[i].regmap,regs[i].was32,rs2[i+1],rs2[i+1]);
        if(itype[i+1]==STORE||itype[i+1]==STORELR||(opcode[i+1]&0x3b)==0x39)
          load_regs(regs[i].regmap_entry,regs[i].regmap,regs[i].was32,INVCP,INVCP);
      }
      else if(i+1<slen)
      {
        // Preload registers for following instruction
        if(rs1[i+1]!=rs1[i]&&rs1[i+1]!=rs2[i])
          if(rs1[i+1]!=rt1[i]&&rs1[i+1]!=rt2[i])
            load_regs(regs[i].regmap_entry,regs[i].regmap,regs[i].was32,rs1[i+1],rs1[i+1]);
        if(rs2[i+1]!=rs1[i+1]&&rs2[i+1]!=rs1[i]&&rs2[i+1]!=rs2[i])
          if(rs2[i+1]!=rt1[i]&&rs2[i+1]!=rt2[i])
            load_regs(regs[i].regmap_entry,regs[i].regmap,regs[i].was32,rs2[i+1],rs2[i+1]);
      }
      // TODO: if(is_ooo(i)) address_generation(i+1);
      if(itype[i]==CJUMP||itype[i]==FJUMP)
        load_regs(regs[i].regmap_entry,regs[i].regmap,regs[i].was32,CCREG,CCREG);
      if(itype[i]==LOAD||itype[i]==LOADLR||itype[i]==STORE||itype[i]==STORELR||itype[i]==C1LS)
        load_regs(regs[i].regmap_entry,regs[i].regmap,regs[i].was32,MMREG,ROREG);
      if(itype[i]==STORE||itype[i]==STORELR||(opcode[i]&0x3b)==0x39)
        load_regs(regs[i].regmap_entry,regs[i].regmap,regs[i].was32,INVCP,INVCP);
      if(bt[i]) cop1_usable=0;
      // assemble
      switch(itype[i]) {
        case ALU:
          alu_assemble(i,&regs[i]);break;
        case IMM16:
          imm16_assemble(i,&regs[i]);break;
        case SHIFT:
          shift_assemble(i,&regs[i]);break;
        case SHIFTIMM:
          shiftimm_assemble(i,&regs[i]);break;
        case LOAD:
          load_assemble(i,&regs[i]);break;
        case LOADLR:
          loadlr_assemble(i,&regs[i]);break;
        case STORE:
          store_assemble(i,&regs[i]);break;
        case STORELR:
          storelr_assemble(i,&regs[i]);break;
        case COP0:
          cop0_assemble(i,&regs[i]);break;
        case COP1:
          cop1_assemble(i,&regs[i]);break;
        case C1LS:
          c1ls_assemble(i,&regs[i]);break;
        case FCONV:
          fconv_assemble(i,&regs[i]);break;
        case FLOAT:
          float_assemble(i,&regs[i]);break;
        case FCOMP:
          fcomp_assemble(i,&regs[i]);break;
        case MULTDIV:
          multdiv_assemble(i,&regs[i]);break;
        case MOV:
          mov_assemble(i,&regs[i]);break;
        case SYSCALL:
          syscall_assemble(i,&regs[i]);break;
        case UJUMP:
          ujump_assemble(i,&regs[i]);ds=1;break;
        case RJUMP:
          rjump_assemble(i,&regs[i]);ds=1;break;
        case CJUMP:
          cjump_assemble(i,&regs[i]);ds=1;break;
        case SJUMP:
          sjump_assemble(i,&regs[i]);ds=1;break;
        case FJUMP:
          fjump_assemble(i,&regs[i]);ds=1;break;
        case SPAN:
          pagespan_assemble(i,&regs[i]);break;
      }
      if(itype[i]==UJUMP||itype[i]==RJUMP||(source[i]>>16)==0x1000)
        literal_pool(1024);
      else
        literal_pool_jumpover(256);
    }
  }
  //assert(itype[i-2]==UJUMP||itype[i-2]==RJUMP||(source[i-2]>>16)==0x1000);
  // If the block did not end with an unconditional branch,
  // add a jump to the next instruction.
  if(i>1) {
    if(itype[i-2]!=UJUMP&&itype[i-2]!=RJUMP&&(source[i-2]>>16)!=0x1000&&itype[i-1]!=SPAN) {
      assert(itype[i-1]!=UJUMP&&itype[i-1]!=CJUMP&&itype[i-1]!=SJUMP&&itype[i-1]!=RJUMP&&itype[i-1]!=FJUMP);
      assert(i==slen);
      if(itype[i-2]!=CJUMP&&itype[i-2]!=SJUMP&&itype[i-2]!=FJUMP) {
        store_regs_bt(regs[i-1].regmap,regs[i-1].is32,regs[i-1].dirty,start+i*4);
        if(regs[i-1].regmap[HOST_CCREG]!=CCREG)
          emit_loadreg(CCREG,HOST_CCREG);
        emit_addimm(HOST_CCREG,CLOCK_DIVIDER*(ccadj[i-1]+1),HOST_CCREG);
      }
      else if(!likely[i-2])
      {
        store_regs_bt(branch_regs[i-2].regmap,branch_regs[i-2].is32,branch_regs[i-2].dirty,start+i*4);
        assert(branch_regs[i-2].regmap[HOST_CCREG]==CCREG);
      }
      else
      {
        store_regs_bt(regs[i-2].regmap,regs[i-2].is32,regs[i-2].dirty,start+i*4);
        assert(regs[i-2].regmap[HOST_CCREG]==CCREG);
      }
      add_to_linker((intptr_t)out,start+i*4,0);
      emit_jmp(0);
    }
  }
  else
  {
    assert(i>0);
    assert(itype[i-1]!=UJUMP&&itype[i-1]!=CJUMP&&itype[i-1]!=SJUMP&&itype[i-1]!=RJUMP&&itype[i-1]!=FJUMP);
    store_regs_bt(regs[i-1].regmap,regs[i-1].is32,regs[i-1].dirty,start+i*4);
    if(regs[i-1].regmap[HOST_CCREG]!=CCREG)
      emit_loadreg(CCREG,HOST_CCREG);
    emit_addimm(HOST_CCREG,CLOCK_DIVIDER*(ccadj[i-1]+1),HOST_CCREG);
    add_to_linker((intptr_t)out,start+i*4,0);
    emit_jmp(0);
  }

  // TODO: delay slot stubs?
  // Stubs
  for(i=0;i<stubcount;i++)
  {
    switch(stubs[i][0])
    {
      case LOADB_STUB:
      case LOADH_STUB:
      case LOADW_STUB:
      case LOADD_STUB:
      case LOADBU_STUB:
      case LOADHU_STUB:
      case LOADWU_STUB:
      case LOADWL_STUB:
      case LOADWR_STUB:
      case LOADDL_STUB:
      case LOADDR_STUB:
        do_readstub(i);break;
      case STOREB_STUB:
      case STOREH_STUB:
      case STOREW_STUB:
      case STORED_STUB:
      case STOREWL_STUB:
      case STOREWR_STUB:
      case STOREDL_STUB:
      case STOREDR_STUB:
        do_writestub(i);break;
      case CC_STUB:
        do_ccstub(i);break;
      case INVCODE_STUB:
        do_invstub(i);break;
      case FP_STUB:
        do_cop1stub(i);break;
    }
  }

  /* Pass 9 - Linker */
  for(i=0;i<linkcount;i++)
  {
    assem_debug("%8x -> %8x",link_addr[i][0],link_addr[i][1]);
    literal_pool(64);
    if(!link_addr[i][2])
    {
      void *stub=out;
      void *addr=check_addr(link_addr[i][1]);
      emit_extjump(link_addr[i][0],link_addr[i][1]);
#ifndef DISABLE_BLOCK_LINKING
#if NEW_DYNAREC==NEW_DYNAREC_ARM64
      //TODO: Avoid disabling link between blocks for conditional branches
      u_char *ptr=(u_char *)link_addr[i][0];
      if(addr&&((ptr[3]&0xfc)==0x14)) {
#else
      if(addr) {
#endif
        set_jump_target(link_addr[i][0],(intptr_t)addr);
        add_link(link_addr[i][1],stub);
      }
      else
#endif
        set_jump_target(link_addr[i][0],(intptr_t)stub);
    }
    else
    {
      // Internal branch
      int target=(link_addr[i][1]-start)>>2;
      assert(target>=0&&target<slen);
      assert(instr_addr[target]);
      //#ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
      //set_jump_target_fillslot(link_addr[i][0],instr_addr[target],link_addr[i][2]>>1);
      //#else
      set_jump_target(link_addr[i][0],instr_addr[target]);
      //#endif
    }
  }
  // External Branch Targets (jump_in)
  for(i=0;i<slen;i++)
  {
    if(bt[i]||i==0)
    {
      if(instr_addr[i]) // TODO - delay slots (=null)
      {
        u_int vaddr=start+i*4;
        u_int page=(0x80000000^vaddr)>>12;
        u_int vpage=page;
        if(page>262143&&g_dev.r4300.cp0.tlb.LUT_r[vaddr>>12]) page=(g_dev.r4300.cp0.tlb.LUT_r[page^0x80000]^0x80000000)>>12;
        if(page>2048) page=2048+(page&2047);
        if(vpage>262143&&g_dev.r4300.cp0.tlb.LUT_r[vaddr>>12]) vpage&=2047; // jump_dirty uses a hash of the virtual address instead
        if(vpage>2048) vpage=2048+(vpage&2047);
        literal_pool(256);
        //if(!(is32[i]&(~unneeded_reg_upper[i])&~(1LL<<CCREG)))
        if(!requires_32bit[i])
        {
          assem_debug("%8x (%d) <- %8x",instr_addr[i],i,start+i*4);
          assem_debug("jump_in: %x",start+i*4);
          struct ll_entry *head=ll_add(jump_dirty+vpage,vaddr,(void *)out,NULL,start,copy,slen*4);
          dirty_entry_count++;
          intptr_t entry_point=do_dirty_stub(i,head);
          head->clean_addr=(void*)entry_point;
          head=ll_add(jump_in+page,vaddr,(void *)entry_point,(void *)entry_point,start,copy,slen*4);
          // If there was an existing entry in the hash table,
          // replace it with the new address.
          // Don't add new entries.  We'll insert the
          // ones that actually get used in check_addr().
          struct ll_entry **ht_bin=hash_table[((vaddr>>16)^vaddr)&0xFFFF];
          if(ht_bin[0]&&ht_bin[0]->vaddr==vaddr) {
            ht_bin[0]=head;
          }
          if(ht_bin[1]&&ht_bin[1]->vaddr==vaddr) {
            ht_bin[1]=head;
          }
        }
        else
        {
          u_int r=requires_32bit[i]|!!(requires_32bit[i]>>32);
          assem_debug("%8x (%d) <- %8x",instr_addr[i],i,start+i*4);
          assem_debug("jump_in: %x (restricted - %x)",start+i*4,r);
          //intptr_t entry_point=(intptr_t)out;
          ////assem_debug("entry_point: %x",entry_point);
          //load_regs_entry(i);
          //if(entry_point==(intptr_t)out)
          //  entry_point=instr_addr[i];
          //else
          //  emit_jmp(instr_addr[i]);
          //struct ll_entry *head=ll_add_32(jump_dirty+vpage,vaddr,r,(void *)entry_point,NULL,start,copy,slen*4);
          struct ll_entry *head=ll_add_32(jump_dirty+vpage,vaddr,r,(void *)out,NULL,start,copy,slen*4);
          dirty_entry_count++;
          intptr_t entry_point=do_dirty_stub(i,head);
          head->clean_addr=(void*)entry_point;
          (void)ll_add_32(jump_in+page,vaddr,r,(void *)entry_point,(void *)entry_point,start,copy,slen*4);
        }
      }
    }
  }
  // Write out the literal pool if necessary
  literal_pool(0);
  #ifdef CORTEX_A8_BRANCH_PREDICTION_HACK
  // Align code
  if(((uintptr_t)out)&7) emit_addnop(13);
  #endif
  assert((uintptr_t)out-beginning<MAX_OUTPUT_BLOCK_SIZE);
  memcpy(copy,(char*)source,slen*4);
  u_int *ptr=(u_int*)copy;
  ptr[slen]=dirty_entry_count;

  #if NEW_DYNAREC >= NEW_DYNAREC_ARM
  intptr_t beginning_rx=((intptr_t)beginning-(intptr_t)base_addr)+(intptr_t)base_addr_rx;
  intptr_t out_rx=((intptr_t)out-(intptr_t)base_addr)+(intptr_t)base_addr_rx;
  cache_flush((char *)beginning_rx,(char *)out_rx);
  #endif

  // If we're within 256K of the end of the buffer,
  // start over from the beginning. (Is 256K enough?)
  if(out > (u_char *)((u_char *)base_addr+(1<<TARGET_SIZE_2)-MAX_OUTPUT_BLOCK_SIZE-JUMP_TABLE_SIZE))
    out=(u_char *)base_addr;

  // Trap writes to any of the pages we compiled
  for(i=start>>12;i<=(int)((start+slen*4-4)>>12);i++) {
    g_dev.r4300.cached_interp.invalid_code[i]=0;
    g_dev.r4300.new_dynarec_hot_state.memory_map[i]|=WRITE_PROTECT;
    if((signed int)start>=(signed int)0xC0000000) {
      assert(using_tlb);
      assert(g_dev.r4300.new_dynarec_hot_state.memory_map[i]!=-1);
      j=(((uintptr_t)i<<12)+(uintptr_t)(g_dev.r4300.new_dynarec_hot_state.memory_map[i]<<2)-(uintptr_t)g_dev.rdram.dram+(uintptr_t)0x80000000)>>12;
      g_dev.r4300.cached_interp.invalid_code[j]=0;
      g_dev.r4300.new_dynarec_hot_state.memory_map[j]|=WRITE_PROTECT;
      //DebugMessage(M64MSG_VERBOSE, "write protect physical page: %x (virtual %x)",j<<12,start);
    }
  }

  /* Pass 10 - Free memory by expiring oldest blocks */

  int end=((((intptr_t)out-(intptr_t)base_addr)>>(TARGET_SIZE_2-16))+16384)&65535;
  while(expirep!=end)
  {
    int shift=TARGET_SIZE_2-3; // Divide into 8 blocks
    intptr_t base=(intptr_t)base_addr+((expirep>>13)<<shift); // Base address of this block
    inv_debug("EXP: Phase %d\n",expirep);
    switch((expirep>>11)&3)
    {
      case 0:
        // Clear jump_in and jump_dirty
        ll_remove_matching_addrs(jump_in+(expirep&2047),base,shift);
        ll_remove_matching_addrs(jump_dirty+(expirep&2047),base,shift);
        ll_remove_matching_addrs(jump_in+2048+(expirep&2047),base,shift);
        ll_remove_matching_addrs(jump_dirty+2048+(expirep&2047),base,shift);
        break;
      case 1:
        // Clear pointers
        ll_kill_pointers(jump_out[expirep&2047],base,shift);
        ll_kill_pointers(jump_out[(expirep&2047)+2048],base,shift);
        break;
      case 2:
        // Clear hash table
        for(i=0;i<32;i++) {
          struct ll_entry **ht_bin=hash_table[((expirep&2047)<<5)+i];
          if(ht_bin[1]&&((((uintptr_t)ht_bin[1]->addr-(uintptr_t)base_addr)>>shift)==((base-(uintptr_t)base_addr)>>shift) ||
             (((uintptr_t)ht_bin[1]->addr-(uintptr_t)base_addr-MAX_OUTPUT_BLOCK_SIZE)>>shift)==((base-(uintptr_t)base_addr)>>shift))) {
            inv_debug("EXP: Remove hash %x -> %x\n",ht_bin[1]->vaddr,ht_bin[1]->addr);
            ht_bin[1]=NULL;
          }
          if(ht_bin[0]&&((((uintptr_t)ht_bin[0]->addr-(uintptr_t)base_addr)>>shift)==((base-(uintptr_t)base_addr)>>shift) ||
             (((uintptr_t)ht_bin[0]->addr-(uintptr_t)base_addr-MAX_OUTPUT_BLOCK_SIZE)>>shift)==((base-(uintptr_t)base_addr)>>shift))) {
            inv_debug("EXP: Remove hash %x -> %x\n",ht_bin[0]->vaddr,ht_bin[0]->addr);
            ht_bin[0]=ht_bin[1];
            ht_bin[1]=NULL;
          }
        }
        break;
      case 3:
        // Clear jump_out
        #if NEW_DYNAREC >= NEW_DYNAREC_ARM
        if((expirep&2047)==0)
          do_clear_cache();
        #endif
        ll_remove_matching_addrs(jump_out+(expirep&2047),base,shift);
        ll_remove_matching_addrs(jump_out+2048+(expirep&2047),base,shift);
        break;
    }
    expirep=(expirep+1)&65535;
  }
  return 0;
}
