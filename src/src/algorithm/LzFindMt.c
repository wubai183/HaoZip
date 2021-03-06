/* LzFindMt.c -- multithreaded Match finder for LZ algorithms
2009-05-26 : Igor Pavlov : Public domain */

#include "algorithm/LzHash.h"

#include "algorithm/LzFindMt.h"

void MtSync_Construct(CMtSync *p)
{
  p->wasCreated = False;
  p->csWasInitialized = False;
  p->csWasEntered = False;
  Thread_Construct(&p->thread);
  Event_Construct(&p->canStart);
  Event_Construct(&p->wasStarted);
  Event_Construct(&p->wasStopped);
  Semaphore_Construct(&p->freeSemaphore);
  Semaphore_Construct(&p->filledSemaphore);
}

void MtSync_GetNextBlock(CMtSync *p)
{
  if (p->needStart)
  {
    p->numProcessedBlocks = 1;
    p->needStart = False;
    p->stopWriting = False;
    p->exit = False;
    Event_Reset(&p->wasStarted);
    Event_Reset(&p->wasStopped);

    Event_Set(&p->canStart);
    Event_Wait(&p->wasStarted);
  }
  else
  {
    CriticalSection_Leave(&p->cs);
    p->csWasEntered = False;
    p->numProcessedBlocks++;
    Semaphore_Release1(&p->freeSemaphore);
  }
  Semaphore_Wait(&p->filledSemaphore);
  CriticalSection_Enter(&p->cs);
  p->csWasEntered = True;
}

/* MtSync_StopWriting must be called if Writing was started */

void MtSync_StopWriting(CMtSync *p)
{
  uint32_t myNumBlocks = p->numProcessedBlocks;
  if (!Thread_WasCreated(&p->thread) || p->needStart)
    return;
  p->stopWriting = True;
  if (p->csWasEntered)
  {
    CriticalSection_Leave(&p->cs);
    p->csWasEntered = False;
  }
  Semaphore_Release1(&p->freeSemaphore);
 
  Event_Wait(&p->wasStopped);

  while (myNumBlocks++ != p->numProcessedBlocks)
  {
    Semaphore_Wait(&p->filledSemaphore);
    Semaphore_Release1(&p->freeSemaphore);
  }
  p->needStart = True;
}

void MtSync_Destruct(CMtSync *p)
{
  if (Thread_WasCreated(&p->thread))
  {
    MtSync_StopWriting(p);
    p->exit = True;
    if (p->needStart)
      Event_Set(&p->canStart);
    Thread_Wait(&p->thread);
    Thread_Close(&p->thread);
  }
  if (p->csWasInitialized)
  {
    CriticalSection_Delete(&p->cs);
    p->csWasInitialized = False;
  }

  Event_Close(&p->canStart);
  Event_Close(&p->wasStarted);
  Event_Close(&p->wasStopped);
  Semaphore_Close(&p->freeSemaphore);
  Semaphore_Close(&p->filledSemaphore);

  p->wasCreated = False;
}

#define RINOK_THREAD(x) { if ((x) != 0) return SZ_ERROR_THREAD; }

static result_t MtSync_Create2(CMtSync *p, unsigned (MY_STD_CALL *startAddress)(void *), void *obj, uint32_t numBlocks)
{
  if (p->wasCreated)
    return SZ_OK;

  RINOK_THREAD(CriticalSection_Init(&p->cs));
  p->csWasInitialized = True;

  RINOK_THREAD(AutoResetEvent_CreateNotSignaled(&p->canStart));
  RINOK_THREAD(AutoResetEvent_CreateNotSignaled(&p->wasStarted));
  RINOK_THREAD(AutoResetEvent_CreateNotSignaled(&p->wasStopped));
  
  RINOK_THREAD(Semaphore_Create(&p->freeSemaphore, numBlocks, numBlocks));
  RINOK_THREAD(Semaphore_Create(&p->filledSemaphore, 0, numBlocks));

  p->needStart = True;
  
  RINOK_THREAD(Thread_Create(&p->thread, startAddress, obj));
  p->wasCreated = True;
  return SZ_OK;
}

static result_t MtSync_Create(CMtSync *p, unsigned (MY_STD_CALL *startAddress)(void *), void *obj, uint32_t numBlocks)
{
  result_t res = MtSync_Create2(p, startAddress, obj, numBlocks);
  if (res != SZ_OK)
    MtSync_Destruct(p);
  return res;
}

void MtSync_Init(CMtSync *p) { p->needStart = True; }

#define kMtMaxValForNormalize 0xFFFFFFFF

#define DEF_GetHeads2(name, v, action) \
static void GetHeads ## name(const byte_t *p, uint32_t pos, \
uint32_t *hash, uint32_t hashMask, uint32_t *heads, uint32_t numHeads, const uint32_t *crc) \
{ action; for (; numHeads != 0; numHeads--) { \
const uint32_t value = (v); p++; *heads++ = pos - hash[value]; hash[value] = pos++;  } }

#define DEF_GetHeads(name, v) DEF_GetHeads2(name, v, ;)

DEF_GetHeads2(2,  (p[0] | ((uint32_t)p[1] << 8)), hashMask = hashMask; crc = crc; )
DEF_GetHeads(3,  (crc[p[0]] ^ p[1] ^ ((uint32_t)p[2] << 8)) & hashMask)
DEF_GetHeads(4,  (crc[p[0]] ^ p[1] ^ ((uint32_t)p[2] << 8) ^ (crc[p[3]] << 5)) & hashMask)
DEF_GetHeads(4b, (crc[p[0]] ^ p[1] ^ ((uint32_t)p[2] << 8) ^ ((uint32_t)p[3] << 16)) & hashMask)
/* DEF_GetHeads(5,  (crc[p[0]] ^ p[1] ^ ((uint32_t)p[2] << 8) ^ (crc[p[3]] << 5) ^ (crc[p[4]] << 3)) & hashMask) */

void HashThreadFunc(CMatchFinderMt *mt)
{
  CMtSync *p = &mt->hashSync;
  for (;;)
  {
    uint32_t numProcessedBlocks = 0;
    Event_Wait(&p->canStart);
    Event_Set(&p->wasStarted);
    for (;;)
    {
      if (p->exit)
        return;
      if (p->stopWriting)
      {
        p->numProcessedBlocks = numProcessedBlocks;
        Event_Set(&p->wasStopped);
        break;
      }

      {
        CMatchFinder *mf = mt->MatchFinder;
        if (MatchFinder_NeedMove(mf))
        {
          CriticalSection_Enter(&mt->btSync.cs);
          CriticalSection_Enter(&mt->hashSync.cs);
          {
            const byte_t *beforePtr = MatchFinder_GetPointerToCurrentPos(mf);
            const byte_t *afterPtr;
            MatchFinder_MoveBlock(mf);
            afterPtr = MatchFinder_GetPointerToCurrentPos(mf);
            mt->pointerToCurPos -= beforePtr - afterPtr;
            mt->buffer -= beforePtr - afterPtr;
          }
          CriticalSection_Leave(&mt->btSync.cs);
          CriticalSection_Leave(&mt->hashSync.cs);
          continue;
        }

        Semaphore_Wait(&p->freeSemaphore);

        MatchFinder_ReadIfRequired(mf);
        if (mf->pos > (kMtMaxValForNormalize - kMtHashBlockSize))
        {
          uint32_t subValue = (mf->pos - mf->historySize - 1);
          MatchFinder_ReduceOffsets(mf, subValue);
          MatchFinder_Normalize3(subValue, mf->hash + mf->fixedHashSize, mf->hashMask + 1);
        }
        {
          uint32_t *heads = mt->hashBuf + ((numProcessedBlocks++) & kMtHashNumBlocksMask) * kMtHashBlockSize;
          uint32_t num = mf->streamPos - mf->pos;
          heads[0] = 2;
          heads[1] = num;
          if (num >= mf->numHashBytes)
          {
            num = num - mf->numHashBytes + 1;
            if (num > kMtHashBlockSize - 2)
              num = kMtHashBlockSize - 2;
            mt->GetHeadsFunc(mf->buffer, mf->pos, mf->hash + mf->fixedHashSize, mf->hashMask, heads + 2, num, mf->crc);
            heads[0] += num;
          }
          mf->pos += num;
          mf->buffer += num;
        }
      }

      Semaphore_Release1(&p->filledSemaphore);
    }
  }
}

void MatchFinderMt_GetNextBlock_Hash(CMatchFinderMt *p)
{
  MtSync_GetNextBlock(&p->hashSync);
  p->hashBufPosLimit = p->hashBufPos = ((p->hashSync.numProcessedBlocks - 1) & kMtHashNumBlocksMask) * kMtHashBlockSize;
  p->hashBufPosLimit += p->hashBuf[p->hashBufPos++];
  p->hashNumAvail = p->hashBuf[p->hashBufPos++];
}

#define kEmptyHashValue 0

/* #define MFMT_GM_INLINE */

#ifdef MFMT_GM_INLINE

#define NO_INLINE MY_FAST_CALL

int32_t NO_INLINE GetMatchesSpecN(uint32_t lenLimit, uint32_t pos, const byte_t *cur, CLzRef *son,
    uint32_t _cyclicBufferPos, uint32_t _cyclicBufferSize, uint32_t _cutValue,
    uint32_t *_distances, uint32_t _maxLen, const uint32_t *hash, int32_t limit, uint32_t size, uint32_t *posRes)
{
  do
  {
  uint32_t *distances = _distances + 1;
  uint32_t curMatch = pos - *hash++;

  CLzRef *ptr0 = son + (_cyclicBufferPos << 1) + 1;
  CLzRef *ptr1 = son + (_cyclicBufferPos << 1);
  uint32_t len0 = 0, len1 = 0;
  uint32_t cutValue = _cutValue;
  uint32_t maxLen = _maxLen;
  for (;;)
  {
    uint32_t delta = pos - curMatch;
    if (cutValue-- == 0 || delta >= _cyclicBufferSize)
    {
      *ptr0 = *ptr1 = kEmptyHashValue;
      break;
    }
    {
      CLzRef *pair = son + ((_cyclicBufferPos - delta + ((delta > _cyclicBufferPos) ? _cyclicBufferSize : 0)) << 1);
      const byte_t *pb = cur - delta;
      uint32_t len = (len0 < len1 ? len0 : len1);
      if (pb[len] == cur[len])
      {
        if (++len != lenLimit && pb[len] == cur[len])
          while (++len != lenLimit)
            if (pb[len] != cur[len])
              break;
        if (maxLen < len)
        {
          *distances++ = maxLen = len;
          *distances++ = delta - 1;
          if (len == lenLimit)
          {
            *ptr1 = pair[0];
            *ptr0 = pair[1];
            break;
          }
        }
      }
      if (pb[len] < cur[len])
      {
        *ptr1 = curMatch;
        ptr1 = pair + 1;
        curMatch = *ptr1;
        len1 = len;
      }
      else
      {
        *ptr0 = curMatch;
        ptr0 = pair;
        curMatch = *ptr0;
        len0 = len;
      }
    }
  }
  pos++;
  _cyclicBufferPos++;
  cur++;
  {
    uint32_t num = (uint32_t)(distances - _distances);
    *_distances = num - 1;
    _distances += num;
    limit -= num;
  }
  }
  while (limit > 0 && --size != 0);
  *posRes = pos;
  return limit;
}

#endif

void BtGetMatches(CMatchFinderMt *p, uint32_t *distances)
{
  uint32_t numProcessed = 0;
  uint32_t curPos = 2;
  uint32_t limit = kMtBtBlockSize - (p->matchMaxLen * 2);
  distances[1] = p->hashNumAvail;
  while (curPos < limit)
  {
    if (p->hashBufPos == p->hashBufPosLimit)
    {
      MatchFinderMt_GetNextBlock_Hash(p);
      distances[1] = numProcessed + p->hashNumAvail;
      if (p->hashNumAvail >= p->numHashBytes)
        continue;
      for (; p->hashNumAvail != 0; p->hashNumAvail--)
        distances[curPos++] = 0;
      break;
    }
    {
      uint32_t size = p->hashBufPosLimit - p->hashBufPos;
      uint32_t lenLimit = p->matchMaxLen;
      uint32_t pos = p->pos;
      uint32_t cyclicBufferPos = p->cyclicBufferPos;
      if (lenLimit >= p->hashNumAvail)
        lenLimit = p->hashNumAvail;
      {
        uint32_t size2 = p->hashNumAvail - lenLimit + 1;
        if (size2 < size)
          size = size2;
        size2 = p->cyclicBufferSize - cyclicBufferPos;
        if (size2 < size)
          size = size2;
      }
      #ifndef MFMT_GM_INLINE
      while (curPos < limit && size-- != 0)
      {
        uint32_t *startDistances = distances + curPos;
        uint32_t num = (uint32_t)(GetMatchesSpec1(lenLimit, pos - p->hashBuf[p->hashBufPos++],
          pos, p->buffer, p->son, cyclicBufferPos, p->cyclicBufferSize, p->cutValue,
          startDistances + 1, p->numHashBytes - 1) - startDistances);
        *startDistances = num - 1;
        curPos += num;
        cyclicBufferPos++;
        pos++;
        p->buffer++;
      }
      #else
      {
        uint32_t posRes;
        curPos = limit - GetMatchesSpecN(lenLimit, pos, p->buffer, p->son, cyclicBufferPos, p->cyclicBufferSize, p->cutValue,
          distances + curPos, p->numHashBytes - 1, p->hashBuf + p->hashBufPos, (int32_t)(limit - curPos) , size, &posRes);
        p->hashBufPos += posRes - pos;
        cyclicBufferPos += posRes - pos;
        p->buffer += posRes - pos;
        pos = posRes;
      }
      #endif

      numProcessed += pos - p->pos;
      p->hashNumAvail -= pos - p->pos;
      p->pos = pos;
      if (cyclicBufferPos == p->cyclicBufferSize)
        cyclicBufferPos = 0;
      p->cyclicBufferPos = cyclicBufferPos;
    }
  }
  distances[0] = curPos;
}

void BtFillBlock(CMatchFinderMt *p, uint32_t globalBlockIndex)
{
  CMtSync *sync = &p->hashSync;
  if (!sync->needStart)
  {
    CriticalSection_Enter(&sync->cs);
    sync->csWasEntered = True;
  }
  
  BtGetMatches(p, p->btBuf + (globalBlockIndex & kMtBtNumBlocksMask) * kMtBtBlockSize);

  if (p->pos > kMtMaxValForNormalize - kMtBtBlockSize)
  {
    uint32_t subValue = p->pos - p->cyclicBufferSize;
    MatchFinder_Normalize3(subValue, p->son, p->cyclicBufferSize * 2);
    p->pos -= subValue;
  }

  if (!sync->needStart)
  {
    CriticalSection_Leave(&sync->cs);
    sync->csWasEntered = False;
  }
}

void BtThreadFunc(CMatchFinderMt *mt)
{
  CMtSync *p = &mt->btSync;
  for (;;)
  {
    uint32_t blockIndex = 0;
    Event_Wait(&p->canStart);
    Event_Set(&p->wasStarted);
    for (;;)
    {
      if (p->exit)
        return;
      if (p->stopWriting)
      {
        p->numProcessedBlocks = blockIndex;
        MtSync_StopWriting(&mt->hashSync);
        Event_Set(&p->wasStopped);
        break;
      }
      Semaphore_Wait(&p->freeSemaphore);
      BtFillBlock(mt, blockIndex++);
      Semaphore_Release1(&p->filledSemaphore);
    }
  }
}

void MatchFinderMt_Construct(CMatchFinderMt *p)
{
  p->hashBuf = 0;
  MtSync_Construct(&p->hashSync);
  MtSync_Construct(&p->btSync);
}

void MatchFinderMt_FreeMem(CMatchFinderMt *p, ISzAlloc *alloc)
{
  alloc->Free(alloc, p->hashBuf);
  p->hashBuf = 0;
}

void MatchFinderMt_Destruct(CMatchFinderMt *p, ISzAlloc *alloc)
{
  MtSync_Destruct(&p->hashSync);
  MtSync_Destruct(&p->btSync);
  MatchFinderMt_FreeMem(p, alloc);
}

#define kHashBufferSize (kMtHashBlockSize * kMtHashNumBlocks)
#define kBtBufferSize (kMtBtBlockSize * kMtBtNumBlocks)

static unsigned MY_STD_CALL HashThreadFunc2(void *p) { HashThreadFunc((CMatchFinderMt *)p);  return 0; }
static unsigned MY_STD_CALL BtThreadFunc2(void *p)
{
  byte_t allocaDummy[0x180];
  int i = 0;
  for (i = 0; i < 16; i++)
    allocaDummy[i] = (byte_t)i;
  BtThreadFunc((CMatchFinderMt *)p);
  return 0;
}

result_t MatchFinderMt_Create(CMatchFinderMt *p, uint32_t historySize, uint32_t keepAddBufferBefore,
    uint32_t matchMaxLen, uint32_t keepAddBufferAfter, ISzAlloc *alloc)
{
  CMatchFinder *mf = p->MatchFinder;
  p->historySize = historySize;
  if (kMtBtBlockSize <= matchMaxLen * 4)
    return SZ_ERROR_PARAM;
  if (p->hashBuf == 0)
  {
    p->hashBuf = (uint32_t *)alloc->Alloc(alloc, (kHashBufferSize + kBtBufferSize) * sizeof(uint32_t));
    if (p->hashBuf == 0)
      return SZ_ERROR_MEM;
    p->btBuf = p->hashBuf + kHashBufferSize;
  }
  keepAddBufferBefore += (kHashBufferSize + kBtBufferSize);
  keepAddBufferAfter += kMtHashBlockSize;
  if (!MatchFinder_Create(mf, historySize, keepAddBufferBefore, matchMaxLen, keepAddBufferAfter, alloc))
    return SZ_ERROR_MEM;

  RINOK(MtSync_Create(&p->hashSync, HashThreadFunc2, p, kMtHashNumBlocks));
  RINOK(MtSync_Create(&p->btSync, BtThreadFunc2, p, kMtBtNumBlocks));
  return SZ_OK;
}

/* Call it after ReleaseStream / SetStream */
void MatchFinderMt_Init(CMatchFinderMt *p)
{
  CMatchFinder *mf = p->MatchFinder;
  p->btBufPos = p->btBufPosLimit = 0;
  p->hashBufPos = p->hashBufPosLimit = 0;
  MatchFinder_Init(mf);
  p->pointerToCurPos = MatchFinder_GetPointerToCurrentPos(mf);
  p->btNumAvailBytes = 0;
  p->lzPos = p->historySize + 1;

  p->hash = mf->hash;
  p->fixedHashSize = mf->fixedHashSize;
  p->crc = mf->crc;

  p->son = mf->son;
  p->matchMaxLen = mf->matchMaxLen;
  p->numHashBytes = mf->numHashBytes;
  p->pos = mf->pos;
  p->buffer = mf->buffer;
  p->cyclicBufferPos = mf->cyclicBufferPos;
  p->cyclicBufferSize = mf->cyclicBufferSize;
  p->cutValue = mf->cutValue;
}

/* ReleaseStream is required to finish multithreading */
void MatchFinderMt_ReleaseStream(CMatchFinderMt *p)
{
  MtSync_StopWriting(&p->btSync);
  /* p->MatchFinder->ReleaseStream(); */
}

void MatchFinderMt_Normalize(CMatchFinderMt *p)
{
  MatchFinder_Normalize3(p->lzPos - p->historySize - 1, p->hash, p->fixedHashSize);
  p->lzPos = p->historySize + 1;
}

void MatchFinderMt_GetNextBlock_Bt(CMatchFinderMt *p)
{
  uint32_t blockIndex;
  MtSync_GetNextBlock(&p->btSync);
  blockIndex = ((p->btSync.numProcessedBlocks - 1) & kMtBtNumBlocksMask);
  p->btBufPosLimit = p->btBufPos = blockIndex * kMtBtBlockSize;
  p->btBufPosLimit += p->btBuf[p->btBufPos++];
  p->btNumAvailBytes = p->btBuf[p->btBufPos++];
  if (p->lzPos >= kMtMaxValForNormalize - kMtBtBlockSize)
    MatchFinderMt_Normalize(p);
}

const byte_t * MatchFinderMt_GetPointerToCurrentPos(CMatchFinderMt *p)
{
  return p->pointerToCurPos;
}

#define GET_NEXT_BLOCK_IF_REQUIRED if (p->btBufPos == p->btBufPosLimit) MatchFinderMt_GetNextBlock_Bt(p);

uint32_t MatchFinderMt_GetNumAvailableBytes(CMatchFinderMt *p)
{
  GET_NEXT_BLOCK_IF_REQUIRED;
  return p->btNumAvailBytes;
}

byte_t MatchFinderMt_GetIndexByte(CMatchFinderMt *p, int32_t index)
{
  return p->pointerToCurPos[index];
}

uint32_t * MixMatches2(CMatchFinderMt *p, uint32_t matchMinPos, uint32_t *distances)
{
  uint32_t hash2Value, curMatch2;
  uint32_t *hash = p->hash;
  const byte_t *cur = p->pointerToCurPos;
  uint32_t lzPos = p->lzPos;
  MT_HASH2_CALC
      
  curMatch2 = hash[hash2Value];
  hash[hash2Value] = lzPos;

  if (curMatch2 >= matchMinPos)
    if (cur[(ptrdiff_t)curMatch2 - lzPos] == cur[0])
    {
      *distances++ = 2;
      *distances++ = lzPos - curMatch2 - 1;
    }
  return distances;
}

uint32_t * MixMatches3(CMatchFinderMt *p, uint32_t matchMinPos, uint32_t *distances)
{
  uint32_t hash2Value, hash3Value, curMatch2, curMatch3;
  uint32_t *hash = p->hash;
  const byte_t *cur = p->pointerToCurPos;
  uint32_t lzPos = p->lzPos;
  MT_HASH3_CALC

  curMatch2 = hash[                hash2Value];
  curMatch3 = hash[kFix3HashSize + hash3Value];
  
  hash[                hash2Value] =
  hash[kFix3HashSize + hash3Value] =
    lzPos;

  if (curMatch2 >= matchMinPos && cur[(ptrdiff_t)curMatch2 - lzPos] == cur[0])
  {
    distances[1] = lzPos - curMatch2 - 1;
    if (cur[(ptrdiff_t)curMatch2 - lzPos + 2] == cur[2])
    {
      distances[0] = 3;
      return distances + 2;
    }
    distances[0] = 2;
    distances += 2;
  }
  if (curMatch3 >= matchMinPos && cur[(ptrdiff_t)curMatch3 - lzPos] == cur[0])
  {
    *distances++ = 3;
    *distances++ = lzPos - curMatch3 - 1;
  }
  return distances;
}

/*
uint32_t *MixMatches4(CMatchFinderMt *p, uint32_t matchMinPos, uint32_t *distances)
{
  uint32_t hash2Value, hash3Value, hash4Value, curMatch2, curMatch3, curMatch4;
  uint32_t *hash = p->hash;
  const byte_t *cur = p->pointerToCurPos;
  uint32_t lzPos = p->lzPos;
  MT_HASH4_CALC
      
  curMatch2 = hash[                hash2Value];
  curMatch3 = hash[kFix3HashSize + hash3Value];
  curMatch4 = hash[kFix4HashSize + hash4Value];
  
  hash[                hash2Value] =
  hash[kFix3HashSize + hash3Value] =
  hash[kFix4HashSize + hash4Value] =
    lzPos;

  if (curMatch2 >= matchMinPos && cur[(ptrdiff_t)curMatch2 - lzPos] == cur[0])
  {
    distances[1] = lzPos - curMatch2 - 1;
    if (cur[(ptrdiff_t)curMatch2 - lzPos + 2] == cur[2])
    {
      distances[0] =  (cur[(ptrdiff_t)curMatch2 - lzPos + 3] == cur[3]) ? 4 : 3;
      return distances + 2;
    }
    distances[0] = 2;
    distances += 2;
  }
  if (curMatch3 >= matchMinPos && cur[(ptrdiff_t)curMatch3 - lzPos] == cur[0])
  {
    distances[1] = lzPos - curMatch3 - 1;
    if (cur[(ptrdiff_t)curMatch3 - lzPos + 3] == cur[3])
    {
      distances[0] = 4;
      return distances + 2;
    }
    distances[0] = 3;
    distances += 2;
  }

  if (curMatch4 >= matchMinPos)
    if (
      cur[(ptrdiff_t)curMatch4 - lzPos] == cur[0] &&
      cur[(ptrdiff_t)curMatch4 - lzPos + 3] == cur[3]
      )
    {
      *distances++ = 4;
      *distances++ = lzPos - curMatch4 - 1;
    }
  return distances;
}
*/

#define INCREASE_LZ_POS p->lzPos++; p->pointerToCurPos++;

uint32_t MatchFinderMt2_GetMatches(CMatchFinderMt *p, uint32_t *distances)
{
  const uint32_t *btBuf = p->btBuf + p->btBufPos;
  uint32_t len = *btBuf++;
  p->btBufPos += 1 + len;
  p->btNumAvailBytes--;
  {
    uint32_t i;
    for (i = 0; i < len; i += 2)
    {
      *distances++ = *btBuf++;
      *distances++ = *btBuf++;
    }
  }
  INCREASE_LZ_POS
  return len;
}

uint32_t MatchFinderMt_GetMatches(CMatchFinderMt *p, uint32_t *distances)
{
  const uint32_t *btBuf = p->btBuf + p->btBufPos;
  uint32_t len = *btBuf++;
  p->btBufPos += 1 + len;

  if (len == 0)
  {
    if (p->btNumAvailBytes-- >= 4)
      len = (uint32_t)(p->MixMatchesFunc(p, p->lzPos - p->historySize, distances) - (distances));
  }
  else
  {
    /* Condition: there are matches in btBuf with length < p->numHashBytes */
    uint32_t *distances2;
    p->btNumAvailBytes--;
    distances2 = p->MixMatchesFunc(p, p->lzPos - btBuf[1], distances);
    do
    {
      *distances2++ = *btBuf++;
      *distances2++ = *btBuf++;
    }
    while ((len -= 2) != 0);
    len  = (uint32_t)(distances2 - (distances));
  }
  INCREASE_LZ_POS
  return len;
}

#define SKIP_HEADER2  do { GET_NEXT_BLOCK_IF_REQUIRED
#define SKIP_HEADER(n) SKIP_HEADER2 if (p->btNumAvailBytes-- >= (n)) { const byte_t *cur = p->pointerToCurPos; uint32_t *hash = p->hash;
#define SKIP_FOOTER } INCREASE_LZ_POS p->btBufPos += p->btBuf[p->btBufPos] + 1; } while (--num != 0);

void MatchFinderMt0_Skip(CMatchFinderMt *p, uint32_t num)
{
  SKIP_HEADER2 { p->btNumAvailBytes--;
  SKIP_FOOTER
}

void MatchFinderMt2_Skip(CMatchFinderMt *p, uint32_t num)
{
  SKIP_HEADER(2)
      uint32_t hash2Value;
      MT_HASH2_CALC
      hash[hash2Value] = p->lzPos;
  SKIP_FOOTER
}

void MatchFinderMt3_Skip(CMatchFinderMt *p, uint32_t num)
{
  SKIP_HEADER(3)
      uint32_t hash2Value, hash3Value;
      MT_HASH3_CALC
      hash[kFix3HashSize + hash3Value] =
      hash[                hash2Value] =
        p->lzPos;
  SKIP_FOOTER
}

/*
void MatchFinderMt4_Skip(CMatchFinderMt *p, uint32_t num)
{
  SKIP_HEADER(4)
      uint32_t hash2Value, hash3Value, hash4Value;
      MT_HASH4_CALC
      hash[kFix4HashSize + hash4Value] =
      hash[kFix3HashSize + hash3Value] =
      hash[                hash2Value] =
        p->lzPos;
  SKIP_FOOTER
}
*/

void MatchFinderMt_CreateVTable(CMatchFinderMt *p, IMatchFinder *vTable)
{
  vTable->Init = (Mf_Init_Func)MatchFinderMt_Init;
  vTable->GetIndexByte = (Mf_GetIndexByte_Func)MatchFinderMt_GetIndexByte;
  vTable->GetNumAvailableBytes = (Mf_GetNumAvailableBytes_Func)MatchFinderMt_GetNumAvailableBytes;
  vTable->GetPointerToCurrentPos = (Mf_GetPointerToCurrentPos_Func)MatchFinderMt_GetPointerToCurrentPos;
  vTable->GetMatches = (Mf_GetMatches_Func)MatchFinderMt_GetMatches;
  switch(p->MatchFinder->numHashBytes)
  {
    case 2:
      p->GetHeadsFunc = GetHeads2;
      p->MixMatchesFunc = (Mf_Mix_Matches)0;
      vTable->Skip = (Mf_Skip_Func)MatchFinderMt0_Skip;
      vTable->GetMatches = (Mf_GetMatches_Func)MatchFinderMt2_GetMatches;
      break;
    case 3:
      p->GetHeadsFunc = GetHeads3;
      p->MixMatchesFunc = (Mf_Mix_Matches)MixMatches2;
      vTable->Skip = (Mf_Skip_Func)MatchFinderMt2_Skip;
      break;
    default:
    /* case 4: */
      p->GetHeadsFunc = p->MatchFinder->bigHash ? GetHeads4b : GetHeads4;
      /* p->GetHeadsFunc = GetHeads4; */
      p->MixMatchesFunc = (Mf_Mix_Matches)MixMatches3;
      vTable->Skip = (Mf_Skip_Func)MatchFinderMt3_Skip;
      break;
    /*
    default:
      p->GetHeadsFunc = GetHeads5;
      p->MixMatchesFunc = (Mf_Mix_Matches)MixMatches4;
      vTable->Skip = (Mf_Skip_Func)MatchFinderMt4_Skip;
      break;
    */
  }
}
