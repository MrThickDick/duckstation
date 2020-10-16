/***************************************************************************
 *   Original copyright notice from PGXP code from Beetle PSX.             *
 *   Copyright (C) 2016 by iCatButler                                      *
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
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#include "pgxp.h"
#include "settings.h"
#include <climits>
#include <cmath>

namespace PGXP {
// pgxp_types.h
typedef struct PGXP_value_Tag
{
  float x;
  float y;
  float z;
  union
  {
    unsigned int flags;
    unsigned char compFlags[4];
    unsigned short halfFlags[2];
  };
  unsigned int count;
  unsigned int value;

  unsigned short gFlags;
  unsigned char lFlags;
  unsigned char hFlags;
} PGXP_value;

// pgxp_value.h
typedef union
{
  struct
  {
    u8 l, h, h2, h3;
  } b;
  struct
  {
    u16 l, h;
  } w;
  struct
  {
    s8 l, h, h2, h3;
  } sb;
  struct
  {
    s16 l, h;
  } sw;
  u32 d;
  s32 sd;
} psx_value;

typedef enum
{
  UNINITIALISED = 0,
  INVALID_PSX_VALUE = 1,
  INVALID_ADDRESS = 2,
  INVALID_BITWISE_OP = 3,
  DIVIDE_BY_ZERO = 4,
  INVALID_8BIT_LOAD = 5,
  INVALID_8BIT_STORE = 6
} PGXP_error_states;

typedef enum
{
  VALID_HALF = (1 << 0)
} PGXP_half_flags;

#define NONE 0
#define ALL 0xFFFFFFFF
#define VALID 1
#define VALID_0 (VALID << 0)
#define VALID_1 (VALID << 8)
#define VALID_2 (VALID << 16)
#define VALID_3 (VALID << 24)
#define VALID_01 (VALID_0 | VALID_1)
#define VALID_012 (VALID_0 | VALID_1 | VALID_2)
#define VALID_ALL (VALID_0 | VALID_1 | VALID_2 | VALID_3)
#define INV_VALID_ALL (ALL ^ VALID_ALL)

static const PGXP_value PGXP_value_invalid_address = {0.f, 0.f, 0.f, {0}, 0, 0, INVALID_ADDRESS, 0, 0};
static const PGXP_value PGXP_value_zero = {0.f, 0.f, 0.f, {0}, 0, VALID_ALL, 0, 0, 0};

static void MakeValid(PGXP_value* pV, u32 psxV);
static void Validate(PGXP_value* pV, u32 psxV);
static void MaskValidate(PGXP_value* pV, u32 psxV, u32 mask, u32 validMask);

static double f16Sign(double in);
static double f16Unsign(double in);
static double f16Overflow(double in);

typedef union
{
  struct
  {
    s16 x;
    s16 y;
  };
  struct
  {
    u16 ux;
    u16 uy;
  };
  u32 word;
} low_value;

// pgxp_mem.h
static u32 PGXP_ConvertAddress(u32 addr);
static PGXP_value* GetPtr(u32 addr);
static PGXP_value* ReadMem(u32 addr);

static void ValidateAndCopyMem(PGXP_value* dest, u32 addr, u32 value);
static void ValidateAndCopyMem16(PGXP_value* dest, u32 addr, u32 value, int sign);

static void WriteMem(PGXP_value* value, u32 addr);
static void WriteMem16(PGXP_value* src, u32 addr);

// pgxp_gpu.h
enum : u32
{
  VERTEX_CACHE_WIDTH = 0x800 * 2,
  VERTEX_CACHE_HEIGHT = 0x800 * 2,
  VERTEX_CACHE_SIZE = VERTEX_CACHE_WIDTH * VERTEX_CACHE_HEIGHT,
  PGXP_MEM_SIZE = 3 * 2048 * 1024 / 4 // mirror 2MB in 32-bit words * 3
};

static PGXP_value* Mem = nullptr;

const unsigned int mode_init = 0;
const unsigned int mode_write = 1;
const unsigned int mode_read = 2;
const unsigned int mode_fail = 3;

unsigned int baseID = 0;
unsigned int lastID = 0;
unsigned int cacheMode = 0;
static PGXP_value* vertexCache = nullptr;

void PGXP_CacheVertex(short sx, short sy, const PGXP_value* _pVertex);

// pgxp_gte.h
static void PGXP_InitGTE();

// pgxp_cpu.h
static void PGXP_InitCPU();
static PGXP_value CPU_reg_mem[34];
#define CPU_Hi CPU_reg[32]
#define CPU_Lo CPU_reg[33]
static PGXP_value CP0_reg_mem[32];

static PGXP_value* CPU_reg = CPU_reg_mem;
static PGXP_value* CP0_reg = CP0_reg_mem;

// pgxp_value.c
void MakeValid(PGXP_value* pV, u32 psxV)
{
  psx_value psx;
  psx.d = psxV;
  if (VALID_01 != (pV->flags & VALID_01))
  {
    pV->x = psx.sw.l;
    pV->y = psx.sw.h;
    pV->z = 0.f;
    pV->flags |= VALID_01;
    pV->value = psx.d;
  }
}

void Validate(PGXP_value* pV, u32 psxV)
{
  // assume pV is not NULL
  pV->flags &= (pV->value == psxV) ? ALL : INV_VALID_ALL;
}

void MaskValidate(PGXP_value* pV, u32 psxV, u32 mask, u32 validMask)
{
  // assume pV is not NULL
  pV->flags &= ((pV->value & mask) == (psxV & mask)) ? ALL : (ALL ^ (validMask));
}

double f16Sign(double in)
{
  u32 s = (u32)(in * (double)((u32)1 << 16));
  return ((double)*((s32*)&s)) / (double)((s32)1 << 16);
}
double f16Unsign(double in)
{
  return (in >= 0) ? in : ((double)in + (double)USHRT_MAX + 1);
}
double f16Overflow(double in)
{
  double out = 0;
  s64 v = ((s64)in) >> 16;
  out = (double)v;
  return out;
}

// pgxp_mem.c
static void PGXP_InitMem();
static const u32 UserMemOffset = 0;
static const u32 ScratchOffset = 2048 * 1024 / 4;
static const u32 RegisterOffset = 2 * 2048 * 1024 / 4;
static const u32 InvalidAddress = 3 * 2048 * 1024 / 4;

void PGXP_InitMem()
{
  if (!Mem)
  {
    Mem = static_cast<PGXP_value*>(std::calloc(PGXP_MEM_SIZE, sizeof(PGXP_value)));
    if (!Mem)
    {
      std::fprintf(stderr, "Failed to allocate PGXP memory\n");
      std::abort();
    }
  }
  else
  {
    std::memset(Mem, 0, sizeof(PGXP_value) * PGXP_MEM_SIZE);
  }
}

u32 PGXP_ConvertAddress(u32 addr)
{
  u32 paddr = addr;
  switch (paddr >> 24)
  {
    case 0x80:
    case 0xa0:
    case 0x00:
      // RAM further mirrored over 8MB
      paddr = ((paddr & 0x7FFFFF) % 0x200000) >> 2;
      paddr = UserMemOffset + paddr;
      break;
    default:
      if ((paddr >> 20) == 0x1f8)
      {
        if (paddr >= 0x1f801000)
        {
          //	paddr = ((paddr & 0xFFFF) - 0x1000);
          //	paddr = (paddr % 0x2000) >> 2;
          paddr = ((paddr & 0xFFFF) - 0x1000) >> 2;
          paddr = RegisterOffset + paddr;
          break;
        }
        else
        {
          // paddr = ((paddr & 0xFFF) % 0x400) >> 2;
          paddr = (paddr & 0x3FF) >> 2;
          paddr = ScratchOffset + paddr;
          break;
        }
      }

      paddr = InvalidAddress;
      break;
  }

#ifdef GTE_LOG
    // GTE_LOG("PGXP_Read %x [%x] |", addr, paddr);
#endif

  return paddr;
}

PGXP_value* GetPtr(u32 addr)
{
  addr = PGXP_ConvertAddress(addr);

  if (addr != InvalidAddress)
    return &Mem[addr];
  return NULL;
}

PGXP_value* ReadMem(u32 addr)
{
  return GetPtr(addr);
}

void ValidateAndCopyMem(PGXP_value* dest, u32 addr, u32 value)
{
  PGXP_value* pMem = GetPtr(addr);
  if (pMem != NULL)
  {
    Validate(pMem, value);
    *dest = *pMem;
    return;
  }

  *dest = PGXP_value_invalid_address;
}

void ValidateAndCopyMem16(PGXP_value* dest, u32 addr, u32 value, int sign)
{
  u32 validMask = 0;
  psx_value val, mask;
  PGXP_value* pMem = GetPtr(addr);
  if (pMem != NULL)
  {
    mask.d = val.d = 0;
    // determine if high or low word
    if ((addr % 4) == 2)
    {
      val.w.h = static_cast<u16>(value);
      mask.w.h = 0xFFFF;
      validMask = VALID_1;
    }
    else
    {
      val.w.l = static_cast<u16>(value);
      mask.w.l = 0xFFFF;
      validMask = VALID_0;
    }

    // validate and copy whole value
    MaskValidate(pMem, val.d, mask.d, validMask);
    *dest = *pMem;

    // if high word then shift
    if ((addr % 4) == 2)
    {
      dest->x = dest->y;
      dest->lFlags = dest->hFlags;
      dest->compFlags[0] = dest->compFlags[1];
    }

    // truncate value
    dest->y = (dest->x < 0) ? -1.f * sign : 0.f; // 0.f;
    dest->hFlags = 0;
    dest->value = value;
    dest->compFlags[1] = VALID; // iCB: High word is valid, just 0
    return;
  }

  *dest = PGXP_value_invalid_address;
}

void WriteMem(PGXP_value* value, u32 addr)
{
  PGXP_value* pMem = GetPtr(addr);

  if (pMem)
    *pMem = *value;
}

void WriteMem16(PGXP_value* src, u32 addr)
{
  PGXP_value* dest = GetPtr(addr);
  psx_value* pVal = NULL;

  if (dest)
  {
    pVal = (psx_value*)&dest->value;
    // determine if high or low word
    if ((addr % 4) == 2)
    {
      dest->y = src->x;
      dest->hFlags = src->lFlags;
      dest->compFlags[1] = src->compFlags[0];
      pVal->w.h = (u16)src->value;
    }
    else
    {
      dest->x = src->x;
      dest->lFlags = src->lFlags;
      dest->compFlags[0] = src->compFlags[0];
      pVal->w.l = (u16)src->value;
    }

    // overwrite z/w if valid
    if (src->compFlags[2] == VALID)
    {
      dest->z = src->z;
      dest->compFlags[2] = src->compFlags[2];
    }

    // dest->valid = dest->valid && src->valid;
    dest->gFlags |= src->gFlags; // inherit flags from both values (?)
  }
}

// pgxp_main.c
void Initialize()
{
  PGXP_InitMem();
  PGXP_InitCPU();
  PGXP_InitGTE();
}

void Shutdown()
{
  cacheMode = mode_init;
  if (vertexCache)
  {
    std::free(vertexCache);
    vertexCache = nullptr;
  }
  if (Mem)
  {
    std::free(Mem);
    Mem = nullptr;
  }
}

// pgxp_gte.c

// GTE registers
static PGXP_value GTE_data_reg_mem[32];
static PGXP_value GTE_ctrl_reg_mem[32];

static PGXP_value* GTE_data_reg = GTE_data_reg_mem;
static PGXP_value* GTE_ctrl_reg = GTE_ctrl_reg_mem;

void PGXP_InitGTE()
{
  memset(GTE_data_reg_mem, 0, sizeof(GTE_data_reg_mem));
  memset(GTE_ctrl_reg_mem, 0, sizeof(GTE_ctrl_reg_mem));
}

// Instruction register decoding
#define op(_instr) (_instr >> 26)          // The op part of the instruction register
#define func(_instr) ((_instr)&0x3F)       // The funct part of the instruction register
#define sa(_instr) ((_instr >> 6) & 0x1F)  // The sa part of the instruction register
#define rd(_instr) ((_instr >> 11) & 0x1F) // The rd part of the instruction register
#define rt(_instr) ((_instr >> 16) & 0x1F) // The rt part of the instruction register
#define rs(_instr) ((_instr >> 21) & 0x1F) // The rs part of the instruction register
#define imm(_instr) (_instr & 0xFFFF)      // The immediate part of the instruction register

#define SX0 (GTE_data_reg[12].x)
#define SY0 (GTE_data_reg[12].y)
#define SX1 (GTE_data_reg[13].x)
#define SY1 (GTE_data_reg[13].y)
#define SX2 (GTE_data_reg[14].x)
#define SY2 (GTE_data_reg[14].y)

#define SXY0 (GTE_data_reg[12])
#define SXY1 (GTE_data_reg[13])
#define SXY2 (GTE_data_reg[14])
#define SXYP (GTE_data_reg[15])

void GTE_PushSXYZ2f(float _x, float _y, float _z, unsigned int _v)
{
  static unsigned int uCount = 0;
  low_value temp;
  // push values down FIFO
  SXY0 = SXY1;
  SXY1 = SXY2;

  SXY2.x = _x;
  SXY2.y = _y;
  SXY2.z = _z;
  SXY2.value = _v;
  SXY2.flags = VALID_ALL;
  SXY2.count = uCount++;

  // cache value in GPU plugin
  temp.word = _v;
  if (g_settings.gpu_pgxp_vertex_cache)
    PGXP_CacheVertex(temp.x, temp.y, &SXY2);
  else
    PGXP_CacheVertex(0, 0, NULL);

#ifdef GTE_LOG
  GTE_LOG("PGXP_PUSH (%f, %f) %u %u|", SXY2.x, SXY2.y, SXY2.flags, SXY2.count);
#endif
}

void GTE_PushSXYZ2s(s64 _x, s64 _y, s64 _z, u32 v)
{
  float fx = (float)(_x) / (float)(1 << 16);
  float fy = (float)(_y) / (float)(1 << 16);
  float fz = (float)(_z);

  // if(Config.PGXP_GTE)
  GTE_PushSXYZ2f(fx, fy, fz, v);
}

void GTE_AVSZ3(u16 z1, u16 z2, u16 z3, s16 scale)
{
  //SXY0.z *= float(scale) / 32767.0f;
  //SXY1.z *= float(scale) / 32767.0f;
  //SXY2.z *= float(scale) / 32767.0f;
}

void GTE_AVSZ4(u16 z1, u16 z2, u16 z3, s16 scale)
{
  //SXY0.z *= float(scale) / 32767.0f;
  //SXY1.z *= float(scale) / 32767.0f;
  //SXY2.z *= float(scale) / 32767.0f;
  //SXY3.z *= float(scale) / 32767.0f;
}

#define VX(n) (psxRegs.CP2D.p[n << 1].sw.l)
#define VY(n) (psxRegs.CP2D.p[n << 1].sw.h)
#define VZ(n) (psxRegs.CP2D.p[(n << 1) + 1].sw.l)

int GTE_NCLIP_valid(u32 sxy0, u32 sxy1, u32 sxy2)
{
  Validate(&SXY0, sxy0);
  Validate(&SXY1, sxy1);
  Validate(&SXY2, sxy2);
  if (((SXY0.flags & SXY1.flags & SXY2.flags & VALID_01) == VALID_01)) // && Config.PGXP_GTE && (Config.PGXP_Mode > 0))
    return 1;
  return 0;
}

float GTE_NCLIP()
{
  float nclip = ((SX0 * SY1) + (SX1 * SY2) + (SX2 * SY0) - (SX0 * SY2) - (SX1 * SY0) - (SX2 * SY1));

  // ensure fractional values are not incorrectly rounded to 0
  float nclipAbs = std::abs(nclip);
  if ((0.1f < nclipAbs) && (nclipAbs < 1.f))
    nclip += (nclip < 0.f ? -1 : 1);

  // float AX = SX1 - SX0;
  // float AY = SY1 - SY0;

  // float BX = SX2 - SX0;
  // float BY = SY2 - SY0;

  //// normalise A and B
  // float mA = sqrt((AX*AX) + (AY*AY));
  // float mB = sqrt((BX*BX) + (BY*BY));

  //// calculate AxB to get Z component of C
  // float CZ = ((AX * BY) - (AY * BX)) * (1 << 12);

  return nclip;
}

static void PGXP_MTC2_int(PGXP_value value, u32 reg)
{
  switch (reg)
  {
    case 15:
      // push FIFO
      SXY0 = SXY1;
      SXY1 = SXY2;
      SXY2 = value;
      SXYP = SXY2;
      break;

    case 31:
      return;
  }

  GTE_data_reg[reg] = value;
}

////////////////////////////////////
// Data transfer tracking
////////////////////////////////////

void CPU_MFC2(u32 instr, u32 rtVal, u32 rdVal)
{
  // CPU[Rt] = GTE_D[Rd]
  Validate(&GTE_data_reg[rd(instr)], rdVal);
  CPU_reg[rt(instr)] = GTE_data_reg[rd(instr)];
  CPU_reg[rt(instr)].value = rtVal;
}

void CPU_MTC2(u32 instr, u32 rdVal, u32 rtVal)
{
  // GTE_D[Rd] = CPU[Rt]
  Validate(&CPU_reg[rt(instr)], rtVal);
  PGXP_MTC2_int(CPU_reg[rt(instr)], rd(instr));
  GTE_data_reg[rd(instr)].value = rdVal;
}

void CPU_CFC2(u32 instr, u32 rtVal, u32 rdVal)
{
  // CPU[Rt] = GTE_C[Rd]
  Validate(&GTE_ctrl_reg[rd(instr)], rdVal);
  CPU_reg[rt(instr)] = GTE_ctrl_reg[rd(instr)];
  CPU_reg[rt(instr)].value = rtVal;
}

void CPU_CTC2(u32 instr, u32 rdVal, u32 rtVal)
{
  // GTE_C[Rd] = CPU[Rt]
  Validate(&CPU_reg[rt(instr)], rtVal);
  GTE_ctrl_reg[rd(instr)] = CPU_reg[rt(instr)];
  GTE_ctrl_reg[rd(instr)].value = rdVal;
}

////////////////////////////////////
// Memory Access
////////////////////////////////////
void CPU_LWC2(u32 instr, u32 rtVal, u32 addr)
{
  // GTE_D[Rt] = Mem[addr]
  PGXP_value val;
  ValidateAndCopyMem(&val, addr, rtVal);
  PGXP_MTC2_int(val, rt(instr));
}

void CPU_SWC2(u32 instr, u32 rtVal, u32 addr)
{
  //  Mem[addr] = GTE_D[Rt]
  Validate(&GTE_data_reg[rt(instr)], rtVal);
  WriteMem(&GTE_data_reg[rt(instr)], addr);
}

// pgxp_gpu.c
/////////////////////////////////
//// Blade_Arma's Vertex Cache (CatBlade?)
/////////////////////////////////
unsigned int IsSessionID(unsigned int vertID)
{
  // No wrapping
  if (lastID >= baseID)
    return (vertID >= baseID);

  // If vertID is >= baseID it is pre-wrap and in session
  if (vertID >= baseID)
    return 1;

  // vertID is < baseID, If it is <= lastID it is post-wrap and in session
  if (vertID <= lastID)
    return 1;

  return 0;
}

static void InitPGXPVertexCache()
{
  if (!vertexCache)
  {
    vertexCache = static_cast<PGXP_value*>(std::calloc(VERTEX_CACHE_SIZE, sizeof(PGXP_value)));
    if (!vertexCache)
    {
      std::fprintf(stderr, "Failed to allocate PGXP vertex cache memory\n");
      std::abort();
    }
  }
  else
  {
    memset(vertexCache, 0x00, VERTEX_CACHE_SIZE * sizeof(PGXP_value));
  }
}

void PGXP_CacheVertex(short sx, short sy, const PGXP_value* _pVertex)
{
  const PGXP_value* pNewVertex = (const PGXP_value*)_pVertex;
  PGXP_value* pOldVertex = NULL;

  if (!pNewVertex)
  {
    cacheMode = mode_fail;
    return;
  }

  // Initialise cache on first use
  if (!vertexCache)
    InitPGXPVertexCache();

  // if (bGteAccuracy)
  {
    if (cacheMode != mode_write)
    {
      // First vertex of write session (frame?)
      cacheMode = mode_write;
      baseID = pNewVertex->count;
    }

    lastID = pNewVertex->count;

    if (sx >= -0x800 && sx <= 0x7ff && sy >= -0x800 && sy <= 0x7ff)
    {
      pOldVertex = &vertexCache[(sy + 0x800) * VERTEX_CACHE_WIDTH + (sx + 0x800)];

      // To avoid ambiguity there can only be one valid entry per-session
      if (0) //(IsSessionID(pOldVertex->count) && (pOldVertex->value == pNewVertex->value))
      {
        // check to ensure this isn't identical
        if ((fabsf(pOldVertex->x - pNewVertex->x) > 0.1f) || (fabsf(pOldVertex->y - pNewVertex->y) > 0.1f) ||
            (fabsf(pOldVertex->z - pNewVertex->z) > 0.1f))
        {
          *pOldVertex = *pNewVertex;
          pOldVertex->gFlags = 5;
          return;
        }
      }

      // Write vertex into cache
      *pOldVertex = *pNewVertex;
      pOldVertex->gFlags = 1;
    }
  }
}

PGXP_value* PGXP_GetCachedVertex(short sx, short sy)
{
  if (g_settings.gpu_pgxp_vertex_cache)
  {
    if (cacheMode != mode_read)
    {
      if (cacheMode == mode_fail)
        return NULL;

      // First vertex of read session (frame?)
      cacheMode = mode_read;
    }

    // Initialise cache on first use
    if (!vertexCache)
      InitPGXPVertexCache();

    if (sx >= -0x800 && sx <= 0x7ff && sy >= -0x800 && sy <= 0x7ff)
    {
      // Return pointer to cache entry
      return &vertexCache[(sy + 0x800) * VERTEX_CACHE_WIDTH + (sx + 0x800)];
    }
  }

  return NULL;
}

static ALWAYS_INLINE_RELEASE float TruncateVertexPosition(float p)
{
  const s32 int_part = static_cast<s32>(p);
  const float int_part_f = static_cast<float>(int_part);
  return static_cast<float>(static_cast<s16>(int_part << 5) >> 5) + (p - int_part_f);
}

static ALWAYS_INLINE_RELEASE bool IsWithinTolerance(float precise_x, float precise_y, int int_x, int int_y)
{
  const float tolerance = g_settings.gpu_pgxp_tolerance;
  if (tolerance < 0.0f)
    return true;

  return (std::abs(precise_x - static_cast<float>(int_x)) <= tolerance &&
          std::abs(precise_y - static_cast<float>(int_y)) <= tolerance);
}

bool GetPreciseVertex(u32 addr, u32 value, int x, int y, int xOffs, int yOffs, float* out_x, float* out_y, float* out_w)
{
  const PGXP_value* vert = ReadMem(addr);
  if (vert && ((vert->flags & VALID_01) == VALID_01) && (vert->value == value))
  {
    // There is a value here with valid X and Y coordinates
    *out_x = TruncateVertexPosition(vert->x) + static_cast<float>(xOffs);
    *out_y = TruncateVertexPosition(vert->y) + static_cast<float>(yOffs);
    *out_w = vert->z / 32768.0f;
    if (IsWithinTolerance(*out_x, *out_y, x, y))
    {
      // check validity of z component
      return ((vert->flags & VALID_2) == VALID_2);
    }
  }
  else
  {
    const short psx_x = (short)(value & 0xFFFFu);
    const short psx_y = (short)(value >> 16);

    // Look in cache for valid vertex
    vert = PGXP_GetCachedVertex(psx_x, psx_y);
    if ((vert) && /*(IsSessionID(vert->count)) &&*/ (vert->gFlags == 1))
    {
      // a value is found, it is from the current session and is unambiguous (there was only one value recorded at that
      // position)
      *out_x = TruncateVertexPosition(vert->x) + static_cast<float>(xOffs);
      *out_y = TruncateVertexPosition(vert->y) + static_cast<float>(yOffs);
      *out_w = vert->z / 32768.0f;

      if (IsWithinTolerance(*out_x, *out_y, x, y))
      {
        return false; // iCB: Getting the wrong w component causes too great an error when using perspective correction
                      // so disable it
      }
    }
  }

  // no valid value can be found anywhere, use the native PSX data
  *out_x = static_cast<float>(x);
  *out_y = static_cast<float>(y);
  *out_w = 1.0f;
  return false;
}

// pgxp_cpu.c

// Instruction register decoding
#define op(_instr) (_instr >> 26)          // The op part of the instruction register
#define func(_instr) ((_instr)&0x3F)       // The funct part of the instruction register
#define sa(_instr) ((_instr >> 6) & 0x1F)  // The sa part of the instruction register
#define rd(_instr) ((_instr >> 11) & 0x1F) // The rd part of the instruction register
#define rt(_instr) ((_instr >> 16) & 0x1F) // The rt part of the instruction register
#define rs(_instr) ((_instr >> 21) & 0x1F) // The rs part of the instruction register
#define imm(_instr) (_instr & 0xFFFF)      // The immediate part of the instruction register

void PGXP_InitCPU()
{
  memset(CPU_reg_mem, 0, sizeof(CPU_reg_mem));
  memset(CP0_reg_mem, 0, sizeof(CP0_reg_mem));
}

// invalidate register (invalid 8 bit read)
static void InvalidLoad(u32 addr, u32 code, u32 value)
{
  u32 reg = ((code >> 16) & 0x1F); // The rt part of the instruction register
  PGXP_value* pD = NULL;
  PGXP_value p;

  p.x = p.y = -1337; // default values

  // p.valid = 0;
  // p.count = value;
  pD = ReadMem(addr);

  if (pD)
  {
    p.count = addr;
    p = *pD;
  }
  else
  {
    p.count = value;
  }

  p.flags = 0;

  // invalidate register
  CPU_reg[reg] = p;
}

// invalidate memory address (invalid 8 bit write)
static void InvalidStore(u32 addr, u32 code, u32 value)
{
  u32 reg = ((code >> 16) & 0x1F); // The rt part of the instruction register
  PGXP_value* pD = NULL;
  PGXP_value p;

  pD = ReadMem(addr);

  p.x = p.y = -2337;

  if (pD)
    p = *pD;

  p.flags = 0;
  p.count = (reg * 1000) + value;

  // invalidate memory
  WriteMem(&p, addr);
}

void CPU_LW(u32 instr, u32 rtVal, u32 addr)
{
  // Rt = Mem[Rs + Im]
  ValidateAndCopyMem(&CPU_reg[rt(instr)], addr, rtVal);
}

void CPU_LBx(u32 instr, u32 rtVal, u32 addr)
{
  InvalidLoad(addr, instr, 116);
}

void CPU_LHx(u32 instr, u32 rtVal, u32 addr)
{
  // Rt = Mem[Rs + Im] (sign/zero extended)
  ValidateAndCopyMem16(&CPU_reg[rt(instr)], addr, rtVal, 1);
}

void CPU_SB(u32 instr, u8 rtVal, u32 addr)
{
  InvalidStore(addr, instr, 208);
}

void CPU_SH(u32 instr, u16 rtVal, u32 addr)
{
  // validate and copy half value
  MaskValidate(&CPU_reg[rt(instr)], rtVal, 0xFFFF, VALID_0);
  WriteMem16(&CPU_reg[rt(instr)], addr);
}

void CPU_SW(u32 instr, u32 rtVal, u32 addr)
{
  // Mem[Rs + Im] = Rt
  Validate(&CPU_reg[rt(instr)], rtVal);
  WriteMem(&CPU_reg[rt(instr)], addr);
}

void CPU_MOVE(u32 rd_and_rs, u32 rsVal)
{
  const u32 Rs = (rd_and_rs & 0xFFu);
  Validate(&CPU_reg[Rs], rsVal);
  CPU_reg[(rd_and_rs >> 8)] = CPU_reg[Rs];
}

void CPU_ADDI(u32 instr, u32 rtVal, u32 rsVal)
{
  // Rt = Rs + Imm (signed)
  psx_value tempImm;
  PGXP_value ret;

  Validate(&CPU_reg[rs(instr)], rsVal);
  ret = CPU_reg[rs(instr)];
  tempImm.d = imm(instr);
  tempImm.sd = (tempImm.sd << 16) >> 16; // sign extend

  if (tempImm.d != 0)
  {
    ret.x = (float)f16Unsign(ret.x);
    ret.x += (float)tempImm.w.l;

    // carry on over/underflow
    float of = (ret.x > USHRT_MAX) ? 1.f : (ret.x < 0) ? -1.f : 0.f;
    ret.x = (float)f16Sign(ret.x);
    // ret.x -= of * (USHRT_MAX + 1);
    ret.y += tempImm.sw.h + of;

    // truncate on overflow/underflow
    ret.y += (ret.y > SHRT_MAX) ? -(USHRT_MAX + 1) : (ret.y < SHRT_MIN) ? USHRT_MAX + 1 : 0.f;
  }

  CPU_reg[rt(instr)] = ret;
  CPU_reg[rt(instr)].value = rtVal;
}

void CPU_ADDIU(u32 instr, u32 rtVal, u32 rsVal)
{
  // Rt = Rs + Imm (signed) (unsafe?)
  CPU_ADDI(instr, rtVal, rsVal);
}

void CPU_ANDI(u32 instr, u32 rtVal, u32 rsVal)
{
  // Rt = Rs & Imm
  psx_value vRt;
  PGXP_value ret;

  Validate(&CPU_reg[rs(instr)], rsVal);
  ret = CPU_reg[rs(instr)];

  vRt.d = rtVal;

  ret.y = 0.f; // remove upper 16-bits

  switch (imm(instr))
  {
    case 0:
      // if 0 then x == 0
      ret.x = 0.f;
      break;
    case 0xFFFF:
      // if saturated then x == x
      break;
    default:
      // otherwise x is low precision value
      ret.x = vRt.sw.l;
      ret.flags |= VALID_0;
  }

  ret.flags |= VALID_1;

  CPU_reg[rt(instr)] = ret;
  CPU_reg[rt(instr)].value = rtVal;
}

void CPU_ORI(u32 instr, u32 rtVal, u32 rsVal)
{
  // Rt = Rs | Imm
  psx_value vRt;
  PGXP_value ret;

  Validate(&CPU_reg[rs(instr)], rsVal);
  ret = CPU_reg[rs(instr)];

  vRt.d = rtVal;

  switch (imm(instr))
  {
    case 0:
      // if 0 then x == x
      break;
    default:
      // otherwise x is low precision value
      ret.x = vRt.sw.l;
      ret.flags |= VALID_0;
  }

  ret.value = rtVal;
  CPU_reg[rt(instr)] = ret;
}

void CPU_XORI(u32 instr, u32 rtVal, u32 rsVal)
{
  // Rt = Rs ^ Imm
  psx_value vRt;
  PGXP_value ret;

  Validate(&CPU_reg[rs(instr)], rsVal);
  ret = CPU_reg[rs(instr)];

  vRt.d = rtVal;

  switch (imm(instr))
  {
    case 0:
      // if 0 then x == x
      break;
    default:
      // otherwise x is low precision value
      ret.x = vRt.sw.l;
      ret.flags |= VALID_0;
  }

  ret.value = rtVal;
  CPU_reg[rt(instr)] = ret;
}

void CPU_SLTI(u32 instr, u32 rtVal, u32 rsVal)
{
  // Rt = Rs < Imm (signed)
  psx_value tempImm;
  PGXP_value ret;

  Validate(&CPU_reg[rs(instr)], rsVal);
  ret = CPU_reg[rs(instr)];

  tempImm.w.h = imm(instr);
  ret.y = 0.f;
  ret.x = (CPU_reg[rs(instr)].x < tempImm.sw.h) ? 1.f : 0.f;
  ret.flags |= VALID_1;
  ret.value = rtVal;

  CPU_reg[rt(instr)] = ret;
}

void CPU_SLTIU(u32 instr, u32 rtVal, u32 rsVal)
{
  // Rt = Rs < Imm (Unsigned)
  psx_value tempImm;
  PGXP_value ret;

  Validate(&CPU_reg[rs(instr)], rsVal);
  ret = CPU_reg[rs(instr)];

  tempImm.w.h = imm(instr);
  ret.y = 0.f;
  ret.x = (f16Unsign(CPU_reg[rs(instr)].x) < tempImm.w.h) ? 1.f : 0.f;
  ret.flags |= VALID_1;
  ret.value = rtVal;

  CPU_reg[rt(instr)] = ret;
}

////////////////////////////////////
// Load Upper
////////////////////////////////////
void CPU_LUI(u32 instr, u32 rtVal)
{
  // Rt = Imm << 16
  CPU_reg[rt(instr)] = PGXP_value_zero;
  CPU_reg[rt(instr)].y = (float)(s16)imm(instr);
  CPU_reg[rt(instr)].hFlags = VALID_HALF;
  CPU_reg[rt(instr)].value = rtVal;
  CPU_reg[rt(instr)].flags = VALID_01;
}

////////////////////////////////////
// Register Arithmetic
////////////////////////////////////

void CPU_ADD(u32 instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  // Rd = Rs + Rt (signed)
  PGXP_value ret;
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  if (rtVal != 0)
  {
    // iCB: Only require one valid input
    if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
    {
      MakeValid(&CPU_reg[rs(instr)], rsVal);
      MakeValid(&CPU_reg[rt(instr)], rtVal);
    }

    ret = CPU_reg[rs(instr)];

    ret.x = (float)f16Unsign(ret.x);
    ret.x += (float)f16Unsign(CPU_reg[rt(instr)].x);

    // carry on over/underflow
    float of = (ret.x > USHRT_MAX) ? 1.f : (ret.x < 0) ? -1.f : 0.f;
    ret.x = (float)f16Sign(ret.x);
    // ret.x -= of * (USHRT_MAX + 1);
    ret.y += CPU_reg[rt(instr)].y + of;

    // truncate on overflow/underflow
    ret.y += (ret.y > SHRT_MAX) ? -(USHRT_MAX + 1) : (ret.y < SHRT_MIN) ? USHRT_MAX + 1 : 0.f;

    // TODO: decide which "z/w" component to use

    ret.halfFlags[0] &= CPU_reg[rt(instr)].halfFlags[0];
    ret.gFlags |= CPU_reg[rt(instr)].gFlags;
    ret.lFlags |= CPU_reg[rt(instr)].lFlags;
    ret.hFlags |= CPU_reg[rt(instr)].hFlags;
  }
  else
  {
    ret = CPU_reg[rs(instr)];
  }

  ret.value = rdVal;

  CPU_reg[rd(instr)] = ret;
}

void CPU_ADDU(u32 instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  // Rd = Rs + Rt (signed) (unsafe?)
  CPU_ADD(instr, rdVal, rsVal, rtVal);
}

void CPU_SUB(u32 instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  // Rd = Rs - Rt (signed)
  PGXP_value ret;
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
  {
    MakeValid(&CPU_reg[rs(instr)], rsVal);
    MakeValid(&CPU_reg[rt(instr)], rtVal);
  }

  ret = CPU_reg[rs(instr)];

  ret.x = (float)f16Unsign(ret.x);
  ret.x -= (float)f16Unsign(CPU_reg[rt(instr)].x);

  // carry on over/underflow
  float of = (ret.x > USHRT_MAX) ? 1.f : (ret.x < 0) ? -1.f : 0.f;
  ret.x = (float)f16Sign(ret.x);
  // ret.x -= of * (USHRT_MAX + 1);
  ret.y -= CPU_reg[rt(instr)].y - of;

  // truncate on overflow/underflow
  ret.y += (ret.y > SHRT_MAX) ? -(USHRT_MAX + 1) : (ret.y < SHRT_MIN) ? USHRT_MAX + 1 : 0.f;

  ret.halfFlags[0] &= CPU_reg[rt(instr)].halfFlags[0];
  ret.gFlags |= CPU_reg[rt(instr)].gFlags;
  ret.lFlags |= CPU_reg[rt(instr)].lFlags;
  ret.hFlags |= CPU_reg[rt(instr)].hFlags;

  ret.value = rdVal;

  CPU_reg[rd(instr)] = ret;
}

void CPU_SUBU(u32 instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  // Rd = Rs - Rt (signed) (unsafe?)
  CPU_SUB(instr, rdVal, rsVal, rtVal);
}

void CPU_AND_(u32 instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  // Rd = Rs & Rt
  psx_value vald, vals, valt;
  PGXP_value ret;

  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
  {
    MakeValid(&CPU_reg[rs(instr)], rsVal);
    MakeValid(&CPU_reg[rt(instr)], rtVal);
  }

  vald.d = rdVal;
  vals.d = rsVal;
  valt.d = rtVal;

  //	CPU_reg[rd(instr)].valid = CPU_reg[rs(instr)].valid && CPU_reg[rt(instr)].valid;
  ret.flags = VALID_01;

  if (vald.w.l == 0)
  {
    ret.x = 0.f;
    ret.lFlags = VALID_HALF;
  }
  else if (vald.w.l == vals.w.l)
  {
    ret.x = CPU_reg[rs(instr)].x;
    ret.lFlags = CPU_reg[rs(instr)].lFlags;
    ret.compFlags[0] = CPU_reg[rs(instr)].compFlags[0];
  }
  else if (vald.w.l == valt.w.l)
  {
    ret.x = CPU_reg[rt(instr)].x;
    ret.lFlags = CPU_reg[rt(instr)].lFlags;
    ret.compFlags[0] = CPU_reg[rt(instr)].compFlags[0];
  }
  else
  {
    ret.x = (float)vald.sw.l;
    ret.compFlags[0] = VALID;
    ret.lFlags = 0;
  }

  if (vald.w.h == 0)
  {
    ret.y = 0.f;
    ret.hFlags = VALID_HALF;
  }
  else if (vald.w.h == vals.w.h)
  {
    ret.y = CPU_reg[rs(instr)].y;
    ret.hFlags = CPU_reg[rs(instr)].hFlags;
    ret.compFlags[1] &= CPU_reg[rs(instr)].compFlags[1];
  }
  else if (vald.w.h == valt.w.h)
  {
    ret.y = CPU_reg[rt(instr)].y;
    ret.hFlags = CPU_reg[rt(instr)].hFlags;
    ret.compFlags[1] &= CPU_reg[rt(instr)].compFlags[1];
  }
  else
  {
    ret.y = (float)vald.sw.h;
    ret.compFlags[1] = VALID;
    ret.hFlags = 0;
  }

  // iCB Hack: Force validity if even one half is valid
  // if ((ret.hFlags & VALID_HALF) || (ret.lFlags & VALID_HALF))
  //	ret.valid = 1;
  // /iCB Hack

  // Get a valid W
  if ((CPU_reg[rs(instr)].flags & VALID_2) == VALID_2)
  {
    ret.z = CPU_reg[rs(instr)].z;
    ret.compFlags[2] = CPU_reg[rs(instr)].compFlags[2];
  }
  else if ((CPU_reg[rt(instr)].flags & VALID_2) == VALID_2)
  {
    ret.z = CPU_reg[rt(instr)].z;
    ret.compFlags[2] = CPU_reg[rt(instr)].compFlags[2];
  }

  ret.value = rdVal;
  CPU_reg[rd(instr)] = ret;
}

void CPU_OR_(u32 instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  // Rd = Rs | Rt
  CPU_AND_(instr, rdVal, rsVal, rtVal);
}

void CPU_XOR_(u32 instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  // Rd = Rs ^ Rt
  CPU_AND_(instr, rdVal, rsVal, rtVal);
}

void CPU_NOR(u32 instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  // Rd = Rs NOR Rt
  CPU_AND_(instr, rdVal, rsVal, rtVal);
}

void CPU_SLT(u32 instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  // Rd = Rs < Rt (signed)
  PGXP_value ret;
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
  {
    MakeValid(&CPU_reg[rs(instr)], rsVal);
    MakeValid(&CPU_reg[rt(instr)], rtVal);
  }

  ret = CPU_reg[rs(instr)];
  ret.y = 0.f;
  ret.compFlags[1] = VALID;

  ret.x = (CPU_reg[rs(instr)].y < CPU_reg[rt(instr)].y) ?
            1.f :
            (f16Unsign(CPU_reg[rs(instr)].x) < f16Unsign(CPU_reg[rt(instr)].x)) ? 1.f : 0.f;

  ret.value = rdVal;
  CPU_reg[rd(instr)] = ret;
}

void CPU_SLTU(u32 instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  // Rd = Rs < Rt (unsigned)
  PGXP_value ret;
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
  {
    MakeValid(&CPU_reg[rs(instr)], rsVal);
    MakeValid(&CPU_reg[rt(instr)], rtVal);
  }

  ret = CPU_reg[rs(instr)];
  ret.y = 0.f;
  ret.compFlags[1] = VALID;

  ret.x = (f16Unsign(CPU_reg[rs(instr)].y) < f16Unsign(CPU_reg[rt(instr)].y)) ?
            1.f :
            (f16Unsign(CPU_reg[rs(instr)].x) < f16Unsign(CPU_reg[rt(instr)].x)) ? 1.f : 0.f;

  ret.value = rdVal;
  CPU_reg[rd(instr)] = ret;
}

////////////////////////////////////
// Register mult/div
////////////////////////////////////

void CPU_MULT(u32 instr, u32 hiVal, u32 loVal, u32 rsVal, u32 rtVal)
{
  // Hi/Lo = Rs * Rt (signed)
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
  {
    MakeValid(&CPU_reg[rs(instr)], rsVal);
    MakeValid(&CPU_reg[rt(instr)], rtVal);
  }

  CPU_Lo = CPU_Hi = CPU_reg[rs(instr)];

  CPU_Lo.halfFlags[0] = CPU_Hi.halfFlags[0] = (CPU_reg[rs(instr)].halfFlags[0] & CPU_reg[rt(instr)].halfFlags[0]);

  double xx, xy, yx, yy;
  double lx = 0, ly = 0, hx = 0, hy = 0;

  // Multiply out components
  xx = f16Unsign(CPU_reg[rs(instr)].x) * f16Unsign(CPU_reg[rt(instr)].x);
  xy = f16Unsign(CPU_reg[rs(instr)].x) * (CPU_reg[rt(instr)].y);
  yx = (CPU_reg[rs(instr)].y) * f16Unsign(CPU_reg[rt(instr)].x);
  yy = (CPU_reg[rs(instr)].y) * (CPU_reg[rt(instr)].y);

  // Split values into outputs
  lx = xx;

  ly = f16Overflow(xx);
  ly += xy + yx;

  hx = f16Overflow(ly);
  hx += yy;

  hy = f16Overflow(hx);

  CPU_Lo.x = (float)f16Sign(lx);
  CPU_Lo.y = (float)f16Sign(ly);
  CPU_Hi.x = (float)f16Sign(hx);
  CPU_Hi.y = (float)f16Sign(hy);

  CPU_Lo.value = loVal;
  CPU_Hi.value = hiVal;
}

void CPU_MULTU(u32 instr, u32 hiVal, u32 loVal, u32 rsVal, u32 rtVal)
{
  // Hi/Lo = Rs * Rt (unsigned)
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  // iCB: Only require one valid input
  if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
  {
    MakeValid(&CPU_reg[rs(instr)], rsVal);
    MakeValid(&CPU_reg[rt(instr)], rtVal);
  }

  CPU_Lo = CPU_Hi = CPU_reg[rs(instr)];

  CPU_Lo.halfFlags[0] = CPU_Hi.halfFlags[0] = (CPU_reg[rs(instr)].halfFlags[0] & CPU_reg[rt(instr)].halfFlags[0]);

  double xx, xy, yx, yy;
  double lx = 0, ly = 0, hx = 0, hy = 0;

  // Multiply out components
  xx = f16Unsign(CPU_reg[rs(instr)].x) * f16Unsign(CPU_reg[rt(instr)].x);
  xy = f16Unsign(CPU_reg[rs(instr)].x) * f16Unsign(CPU_reg[rt(instr)].y);
  yx = f16Unsign(CPU_reg[rs(instr)].y) * f16Unsign(CPU_reg[rt(instr)].x);
  yy = f16Unsign(CPU_reg[rs(instr)].y) * f16Unsign(CPU_reg[rt(instr)].y);

  // Split values into outputs
  lx = xx;

  ly = f16Overflow(xx);
  ly += xy + yx;

  hx = f16Overflow(ly);
  hx += yy;

  hy = f16Overflow(hx);

  CPU_Lo.x = (float)f16Sign(lx);
  CPU_Lo.y = (float)f16Sign(ly);
  CPU_Hi.x = (float)f16Sign(hx);
  CPU_Hi.y = (float)f16Sign(hy);

  CPU_Lo.value = loVal;
  CPU_Hi.value = hiVal;
}

void CPU_DIV(u32 instr, u32 hiVal, u32 loVal, u32 rsVal, u32 rtVal)
{
  // Lo = Rs / Rt (signed)
  // Hi = Rs % Rt (signed)
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  //// iCB: Only require one valid input
  if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
  {
    MakeValid(&CPU_reg[rs(instr)], rsVal);
    MakeValid(&CPU_reg[rt(instr)], rtVal);
  }

  CPU_Lo = CPU_Hi = CPU_reg[rs(instr)];

  CPU_Lo.halfFlags[0] = CPU_Hi.halfFlags[0] = (CPU_reg[rs(instr)].halfFlags[0] & CPU_reg[rt(instr)].halfFlags[0]);

  double vs = f16Unsign(CPU_reg[rs(instr)].x) + (CPU_reg[rs(instr)].y) * (double)(1 << 16);
  double vt = f16Unsign(CPU_reg[rt(instr)].x) + (CPU_reg[rt(instr)].y) * (double)(1 << 16);

  double lo = vs / vt;
  CPU_Lo.y = (float)f16Sign(f16Overflow(lo));
  CPU_Lo.x = (float)f16Sign(lo);

  double hi = fmod(vs, vt);
  CPU_Hi.y = (float)f16Sign(f16Overflow(hi));
  CPU_Hi.x = (float)f16Sign(hi);

  CPU_Lo.value = loVal;
  CPU_Hi.value = hiVal;
}

void CPU_DIVU(u32 instr, u32 hiVal, u32 loVal, u32 rsVal, u32 rtVal)
{
  // Lo = Rs / Rt (unsigned)
  // Hi = Rs % Rt (unsigned)
  Validate(&CPU_reg[rs(instr)], rsVal);
  Validate(&CPU_reg[rt(instr)], rtVal);

  //// iCB: Only require one valid input
  if (((CPU_reg[rt(instr)].flags & VALID_01) != VALID_01) != ((CPU_reg[rs(instr)].flags & VALID_01) != VALID_01))
  {
    MakeValid(&CPU_reg[rs(instr)], rsVal);
    MakeValid(&CPU_reg[rt(instr)], rtVal);
  }

  CPU_Lo = CPU_Hi = CPU_reg[rs(instr)];

  CPU_Lo.halfFlags[0] = CPU_Hi.halfFlags[0] = (CPU_reg[rs(instr)].halfFlags[0] & CPU_reg[rt(instr)].halfFlags[0]);

  double vs = f16Unsign(CPU_reg[rs(instr)].x) + f16Unsign(CPU_reg[rs(instr)].y) * (double)(1 << 16);
  double vt = f16Unsign(CPU_reg[rt(instr)].x) + f16Unsign(CPU_reg[rt(instr)].y) * (double)(1 << 16);

  double lo = vs / vt;
  CPU_Lo.y = (float)f16Sign(f16Overflow(lo));
  CPU_Lo.x = (float)f16Sign(lo);

  double hi = fmod(vs, vt);
  CPU_Hi.y = (float)f16Sign(f16Overflow(hi));
  CPU_Hi.x = (float)f16Sign(hi);

  CPU_Lo.value = loVal;
  CPU_Hi.value = hiVal;
}

////////////////////////////////////
// Shift operations (sa)
////////////////////////////////////
void CPU_SLL(u32 instr, u32 rdVal, u32 rtVal)
{
  // Rd = Rt << Sa
  PGXP_value ret;
  u32 sh = sa(instr);
  Validate(&CPU_reg[rt(instr)], rtVal);

  ret = CPU_reg[rt(instr)];

  // TODO: Shift flags
  double x = f16Unsign(CPU_reg[rt(instr)].x);
  double y = f16Unsign(CPU_reg[rt(instr)].y);
  if (sh >= 32)
  {
    x = 0.f;
    y = 0.f;
  }
  else if (sh == 16)
  {
    y = f16Sign(x);
    x = 0.f;
  }
  else if (sh >= 16)
  {
    y = x * (1 << (sh - 16));
    y = f16Sign(y);
    x = 0.f;
  }
  else
  {
    x = x * (1 << sh);
    y = y * (1 << sh);
    y += f16Overflow(x);
    x = f16Sign(x);
    y = f16Sign(y);
  }

  ret.x = (float)x;
  ret.y = (float)y;

  ret.value = rdVal;
  CPU_reg[rd(instr)] = ret;
}

void CPU_SRL(u32 instr, u32 rdVal, u32 rtVal)
{
  // Rd = Rt >> Sa
  PGXP_value ret;
  u32 sh = sa(instr);
  Validate(&CPU_reg[rt(instr)], rtVal);

  ret = CPU_reg[rt(instr)];

  double x = CPU_reg[rt(instr)].x, y = f16Unsign(CPU_reg[rt(instr)].y);

  psx_value iX;
  iX.d = rtVal;
  psx_value iY;
  iY.d = rtVal;

  iX.sd = (iX.sd << 16) >> 16; // remove Y
  iY.sw.l = iX.sw.h;           // overwrite x with sign(x)

  // Shift test values
  psx_value dX;
  dX.sd = iX.sd >> sh;
  psx_value dY;
  dY.d = iY.d >> sh;

  if (dX.sw.l != iX.sw.h)
    x = x / (1 << sh);
  else
    x = dX.sw.l; // only sign bits left

  if (dY.sw.l != iX.sw.h)
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * (1 << (16 - sh));
      if (CPU_reg[rt(instr)].x < 0)
        x += 1 << (16 - sh);
    }
    else
    {
      x += y / (1 << (sh - 16));
    }
  }

  if ((dY.sw.h == 0) || (dY.sw.h == -1))
    y = dY.sw.h;
  else
    y = y / (1 << sh);

  x = f16Sign(x);
  y = f16Sign(y);

  ret.x = (float)x;
  ret.y = (float)y;

  ret.value = rdVal;
  CPU_reg[rd(instr)] = ret;
}

void CPU_SRA(u32 instr, u32 rdVal, u32 rtVal)
{
  // Rd = Rt >> Sa
  PGXP_value ret;
  u32 sh = sa(instr);
  Validate(&CPU_reg[rt(instr)], rtVal);
  ret = CPU_reg[rt(instr)];

  double x = CPU_reg[rt(instr)].x, y = CPU_reg[rt(instr)].y;

  psx_value iX;
  iX.d = rtVal;
  psx_value iY;
  iY.d = rtVal;

  iX.sd = (iX.sd << 16) >> 16; // remove Y
  iY.sw.l = iX.sw.h;           // overwrite x with sign(x)

  // Shift test values
  psx_value dX;
  dX.sd = iX.sd >> sh;
  psx_value dY;
  dY.sd = iY.sd >> sh;

  if (dX.sw.l != iX.sw.h)
    x = x / (1 << sh);
  else
    x = dX.sw.l; // only sign bits left

  if (dY.sw.l != iX.sw.h)
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * (1 << (16 - sh));
      if (CPU_reg[rt(instr)].x < 0)
        x += 1 << (16 - sh);
    }
    else
    {
      x += y / (1 << (sh - 16));
    }
  }

  if ((dY.sw.h == 0) || (dY.sw.h == -1))
    y = dY.sw.h;
  else
    y = y / (1 << sh);

  x = f16Sign(x);
  y = f16Sign(y);

  ret.x = (float)x;
  ret.y = (float)y;

  ret.value = rdVal;
  CPU_reg[rd(instr)] = ret;
}

////////////////////////////////////
// Shift operations variable
////////////////////////////////////
void CPU_SLLV(u32 instr, u32 rdVal, u32 rtVal, u32 rsVal)
{
  // Rd = Rt << Rs
  PGXP_value ret;
  u32 sh = rsVal & 0x1F;
  Validate(&CPU_reg[rt(instr)], rtVal);
  Validate(&CPU_reg[rs(instr)], rsVal);

  ret = CPU_reg[rt(instr)];

  double x = f16Unsign(CPU_reg[rt(instr)].x);
  double y = f16Unsign(CPU_reg[rt(instr)].y);
  if (sh >= 32)
  {
    x = 0.f;
    y = 0.f;
  }
  else if (sh == 16)
  {
    y = f16Sign(x);
    x = 0.f;
  }
  else if (sh >= 16)
  {
    y = x * (1 << (sh - 16));
    y = f16Sign(y);
    x = 0.f;
  }
  else
  {
    x = x * (1 << sh);
    y = y * (1 << sh);
    y += f16Overflow(x);
    x = f16Sign(x);
    y = f16Sign(y);
  }

  ret.x = (float)x;
  ret.y = (float)y;

  ret.value = rdVal;
  CPU_reg[rd(instr)] = ret;
}

void CPU_SRLV(u32 instr, u32 rdVal, u32 rtVal, u32 rsVal)
{
  // Rd = Rt >> Sa
  PGXP_value ret;
  u32 sh = rsVal & 0x1F;
  Validate(&CPU_reg[rt(instr)], rtVal);
  Validate(&CPU_reg[rs(instr)], rsVal);

  ret = CPU_reg[rt(instr)];

  double x = CPU_reg[rt(instr)].x, y = f16Unsign(CPU_reg[rt(instr)].y);

  psx_value iX;
  iX.d = rtVal;
  psx_value iY;
  iY.d = rtVal;

  iX.sd = (iX.sd << 16) >> 16; // remove Y
  iY.sw.l = iX.sw.h;           // overwrite x with sign(x)

  // Shift test values
  psx_value dX;
  dX.sd = iX.sd >> sh;
  psx_value dY;
  dY.d = iY.d >> sh;

  if (dX.sw.l != iX.sw.h)
    x = x / (1 << sh);
  else
    x = dX.sw.l; // only sign bits left

  if (dY.sw.l != iX.sw.h)
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * (1 << (16 - sh));
      if (CPU_reg[rt(instr)].x < 0)
        x += 1 << (16 - sh);
    }
    else
    {
      x += y / (1 << (sh - 16));
    }
  }

  if ((dY.sw.h == 0) || (dY.sw.h == -1))
    y = dY.sw.h;
  else
    y = y / (1 << sh);

  x = f16Sign(x);
  y = f16Sign(y);

  ret.x = (float)x;
  ret.y = (float)y;

  ret.value = rdVal;
  CPU_reg[rd(instr)] = ret;
}

void CPU_SRAV(u32 instr, u32 rdVal, u32 rtVal, u32 rsVal)
{
  // Rd = Rt >> Sa
  PGXP_value ret;
  u32 sh = rsVal & 0x1F;
  Validate(&CPU_reg[rt(instr)], rtVal);
  Validate(&CPU_reg[rs(instr)], rsVal);

  ret = CPU_reg[rt(instr)];

  double x = CPU_reg[rt(instr)].x, y = CPU_reg[rt(instr)].y;

  psx_value iX;
  iX.d = rtVal;
  psx_value iY;
  iY.d = rtVal;

  iX.sd = (iX.sd << 16) >> 16; // remove Y
  iY.sw.l = iX.sw.h;           // overwrite x with sign(x)

  // Shift test values
  psx_value dX;
  dX.sd = iX.sd >> sh;
  psx_value dY;
  dY.sd = iY.sd >> sh;

  if (dX.sw.l != iX.sw.h)
    x = x / (1 << sh);
  else
    x = dX.sw.l; // only sign bits left

  if (dY.sw.l != iX.sw.h)
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * (1 << (16 - sh));
      if (CPU_reg[rt(instr)].x < 0)
        x += 1 << (16 - sh);
    }
    else
    {
      x += y / (1 << (sh - 16));
    }
  }

  if ((dY.sw.h == 0) || (dY.sw.h == -1))
    y = dY.sw.h;
  else
    y = y / (1 << sh);

  x = f16Sign(x);
  y = f16Sign(y);

  ret.x = (float)x;
  ret.y = (float)y;

  ret.value = rdVal;
  CPU_reg[rd(instr)] = ret;
}

void CPU_MFHI(u32 instr, u32 rdVal, u32 hiVal)
{
  // Rd = Hi
  Validate(&CPU_Hi, hiVal);

  CPU_reg[rd(instr)] = CPU_Hi;
}

void CPU_MTHI(u32 instr, u32 hiVal, u32 rdVal)
{
  // Hi = Rd
  Validate(&CPU_reg[rd(instr)], rdVal);

  CPU_Hi = CPU_reg[rd(instr)];
}

void CPU_MFLO(u32 instr, u32 rdVal, u32 loVal)
{
  // Rd = Lo
  Validate(&CPU_Lo, loVal);

  CPU_reg[rd(instr)] = CPU_Lo;
}

void CPU_MTLO(u32 instr, u32 loVal, u32 rdVal)
{
  // Lo = Rd
  Validate(&CPU_reg[rd(instr)], rdVal);

  CPU_Lo = CPU_reg[rd(instr)];
}

void CPU_MFC0(u32 instr, u32 rtVal, u32 rdVal)
{
  // CPU[Rt] = CP0[Rd]
  Validate(&CP0_reg[rd(instr)], rdVal);
  CPU_reg[rt(instr)] = CP0_reg[rd(instr)];
  CPU_reg[rt(instr)].value = rtVal;
}

void CPU_MTC0(u32 instr, u32 rdVal, u32 rtVal)
{
  // CP0[Rd] = CPU[Rt]
  Validate(&CPU_reg[rt(instr)], rtVal);
  CP0_reg[rd(instr)] = CPU_reg[rt(instr)];
  CP0_reg[rd(instr)].value = rdVal;
}

void CPU_CFC0(u32 instr, u32 rtVal, u32 rdVal)
{
  // CPU[Rt] = CP0[Rd]
  Validate(&CP0_reg[rd(instr)], rdVal);
  CPU_reg[rt(instr)] = CP0_reg[rd(instr)];
  CPU_reg[rt(instr)].value = rtVal;
}

void CPU_CTC0(u32 instr, u32 rdVal, u32 rtVal)
{
  // CP0[Rd] = CPU[Rt]
  Validate(&CPU_reg[rt(instr)], rtVal);
  CP0_reg[rd(instr)] = CPU_reg[rt(instr)];
  CP0_reg[rd(instr)].value = rdVal;
}

} // namespace PGXP
