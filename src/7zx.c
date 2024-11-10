/* 7zx.c - Library source file for 7zx
2015-03-22 : Lukas Duerrenberger : Public domain */

#include "Precomp.h"

#include <stdio.h>
#include <string.h>

#include "7z.h"
#include "7zAlloc.h"
#include "7zBuf.h"
#include "7zCrc.h"
#include "7zFile.h"
#include "7zVersion.h"

#ifndef USE_WINDOWS_FILE
/* for mkdir */
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <errno.h>
#endif
#endif

#define kInputBufSize ((size_t)1 << 18)

static ISzAlloc g_Alloc = { SzAlloc, SzFree };

static int Buf_EnsureSize(CBuf *dest, size_t size)
{
  if (dest->size >= size)
    return 1;
  Buf_Free(dest, &g_Alloc);
  return Buf_Create(dest, size, &g_Alloc);
}

#ifndef _WIN32

static Byte kUtf8Limits[5] = { 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };

static BoolInt Utf16_To_Utf8(Byte *dest, size_t *destLen, const UInt16 *src, size_t srcLen)
{
  size_t destPos = 0, srcPos = 0;
  for (;;)
  {
    unsigned numAdds;
    UInt32 value;
    if (srcPos == srcLen)
    {
      *destLen = destPos;
      return True;
    }
    value = src[srcPos++];
    if (value < 0x80)
    {
      if (dest)
        dest[destPos] = (char)value;
      destPos++;
      continue;
    }
    if (value >= 0xD800 && value < 0xE000)
    {
      UInt32 c2;
      if (value >= 0xDC00 || srcPos == srcLen)
        break;
      c2 = src[srcPos++];
      if (c2 < 0xDC00 || c2 >= 0xE000)
        break;
      value = (((value - 0xD800) << 10) | (c2 - 0xDC00)) + 0x10000;
    }
    for (numAdds = 1; numAdds < 5; numAdds++)
      if (value < (((UInt32)1) << (numAdds * 5 + 6)))
        break;
    if (dest)
      dest[destPos] = (char)(kUtf8Limits[numAdds - 1] + (value >> (6 * numAdds)));
    destPos++;
    do
    {
      numAdds--;
      if (dest)
        dest[destPos] = (char)(0x80 + ((value >> (6 * numAdds)) & 0x3F));
      destPos++;
    }
    while (numAdds != 0);
  }
  *destLen = destPos;
  return False;
}

static SRes Utf16_To_Utf8Buf(CBuf *dest, const UInt16 *src, size_t srcLen)
{
  size_t destLen = 0;
  BoolInt res;
  Utf16_To_Utf8(NULL, &destLen, src, srcLen);
  destLen += 1;
  if (!Buf_EnsureSize(dest, destLen))
    return SZ_ERROR_MEM;
  res = Utf16_To_Utf8(dest->data, &destLen, src, srcLen);
  dest->data[destLen] = 0;
  return res ? SZ_OK : SZ_ERROR_FAIL;
}

#endif

static SRes Utf16_To_Char(CBuf *buf, const UInt16 *s
    #ifdef _WIN32
    , UINT codePage
    #endif
    )
{
  unsigned len = 0;
  for (len = 0; s[len] != 0; len++);

  #ifdef _WIN32
  {
    unsigned size = len * 3 + 100;
    if (!Buf_EnsureSize(buf, size))
      return SZ_ERROR_MEM;
    {
      buf->data[0] = 0;
      if (len != 0)
      {
        char defaultChar = '_';
        BOOL defUsed;
        unsigned numChars = 0;
        numChars = WideCharToMultiByte(codePage, 0, s, len, (char *)buf->data, size, &defaultChar, &defUsed);
        if (numChars == 0 || numChars >= size)
          return SZ_ERROR_FAIL;
        buf->data[numChars] = 0;
      }
      return SZ_OK;
    }
  }
  #else
  return Utf16_To_Utf8Buf(buf, s, len);
  #endif
}

#ifdef _WIN32
  #ifndef USE_WINDOWS_FILE
    static UINT g_FileCodePage = CP_ACP;
  #endif
  #define MY_FILE_CODE_PAGE_PARAM ,g_FileCodePage
#else
  #define MY_FILE_CODE_PAGE_PARAM
#endif

static WRes MyCreateDir(const UInt16 *name)
{
  #ifdef USE_WINDOWS_FILE

  return CreateDirectoryW(name, NULL) ? 0 : GetLastError();

  #else

  CBuf buf;
  WRes res;
  Buf_Init(&buf);
  RINOK(Utf16_To_Char(&buf, name MY_FILE_CODE_PAGE_PARAM));

  res =
  #ifdef _WIN32
  _mkdir((const char *)buf.data)
  #else
  mkdir((const char *)buf.data, 0777)
  #endif
  == 0 ? 0 : errno;
  Buf_Free(&buf, &g_Alloc);
  return res;

  #endif
}

static WRes OutFile_OpenUtf16(CSzFile *p, const UInt16 *name)
{
  #ifdef USE_WINDOWS_FILE
  return OutFile_OpenW(p, name);
  #else
  CBuf buf;
  WRes res;
  Buf_Init(&buf);
  RINOK(Utf16_To_Char(&buf, name MY_FILE_CODE_PAGE_PARAM));
  res = OutFile_Open(p, (const char *)buf.data);
  Buf_Free(&buf, &g_Alloc);
  return res;
  #endif
}

static void UInt64ToStr(UInt64 value, char *s)
{
  char temp[32];
  int pos = 0;
  do
  {
    temp[pos++] = (char)('0' + (unsigned)(value % 10));
    value /= 10;
  }
  while (value != 0);
  do
    *s++ = temp[--pos];
  while (pos);
  *s = '\0';
}

static char *UIntToStr(char *s, unsigned value, int numDigits)
{
  char temp[16];
  int pos = 0;
  do
    temp[pos++] = (char)('0' + (value % 10));
  while (value /= 10);
  for (numDigits -= pos; numDigits > 0; numDigits--)
    *s++ = '0';
  do
    *s++ = temp[--pos];
  while (pos);
  *s = '\0';
  return s;
}

static void UIntToStr_2(char *s, unsigned value)
{
  s[0] = (char)('0' + (value / 10));
  s[1] = (char)('0' + (value % 10));
}

#define PERIOD_4 (4 * 365 + 1)
#define PERIOD_100 (PERIOD_4 * 25 - 1)
#define PERIOD_400 (PERIOD_100 * 4 + 1)

static void ConvertFileTimeToString(const CNtfsFileTime *nt, char *s)
{
  unsigned year, mon, hour, min, sec;
  Byte ms[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  unsigned t;
  UInt32 v;
  UInt64 v64 = nt->Low | ((UInt64)nt->High << 32);
  v64 /= 10000000;
  sec = (unsigned)(v64 % 60); v64 /= 60;
  min = (unsigned)(v64 % 60); v64 /= 60;
  hour = (unsigned)(v64 % 24); v64 /= 24;

  v = (UInt32)v64;

  year = (unsigned)(1601 + v / PERIOD_400 * 400);
  v %= PERIOD_400;

  t = v / PERIOD_100; if (t ==  4) t =  3; year += t * 100; v -= t * PERIOD_100;
  t = v / PERIOD_4;   if (t == 25) t = 24; year += t * 4;   v -= t * PERIOD_4;
  t = v / 365;        if (t ==  4) t =  3; year += t;       v -= t * 365;

  if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
    ms[1] = 29;
  for (mon = 0;; mon++)
  {
    unsigned s = ms[mon];
    if (v < s)
      break;
    v -= s;
  }
  s = UIntToStr(s, year, 4); *s++ = '-';
  UIntToStr_2(s, mon + 1); s[2] = '-'; s += 3;
  UIntToStr_2(s, (unsigned)v + 1); s[2] = ' '; s += 3;
  UIntToStr_2(s, hour); s[2] = ':'; s += 3;
  UIntToStr_2(s, min); s[2] = ':'; s += 3;
  UIntToStr_2(s, sec); s[2] = 0;
}

#ifdef USE_WINDOWS_FILE
static void GetAttribString(UInt32 wa, BoolInt isDir, char *s)
{
  s[0] = (char)(((wa & FILE_ATTRIBUTE_DIRECTORY) != 0 || isDir) ? 'D' : '.');
  s[1] = (char)(((wa & FILE_ATTRIBUTE_READONLY ) != 0) ? 'R': '.');
  s[2] = (char)(((wa & FILE_ATTRIBUTE_HIDDEN   ) != 0) ? 'H': '.');
  s[3] = (char)(((wa & FILE_ATTRIBUTE_SYSTEM   ) != 0) ? 'S': '.');
  s[4] = (char)(((wa & FILE_ATTRIBUTE_ARCHIVE  ) != 0) ? 'A': '.');
  s[5] = '\0';
}
#else
static void GetAttribString(UInt32, BoolInt, char *s)
{
  s[0] = '\0';
}
#endif

// #define NUM_PARENTS_MAX 128

SRes SzxExtract(const char* filename, BoolInt fullPaths)
{
  CFileInStream archiveStream;
  CLookToRead2 lookStream;
  CSzArEx db;
  SRes res;
  ISzAlloc allocImp;
  ISzAlloc allocTempImp;
  UInt16 *temp = NULL;
  size_t tempSize = 0;
  // UInt32 parents[NUM_PARENTS_MAX];

  #if defined(_WIN32) && !defined(USE_WINDOWS_FILE) && !defined(UNDER_CE)
  g_FileCodePage = AreFileApisANSI() ? CP_ACP : CP_OEMCP;
  #endif

  allocImp = g_Alloc;

  allocTempImp = g_Alloc;

  #ifdef UNDER_CE
  if (InFile_OpenW(&archiveStream.file, L"\test.7z"))
  #else
  if (InFile_Open(&archiveStream.file, filename))
  #endif
  {
    return SZ_ERROR_FAIL;
  }

  FileInStream_CreateVTable(&archiveStream);
  archiveStream.wres = 0;
  LookToRead2_CreateVTable(&lookStream, False);
  lookStream.buf = NULL;

  {
    lookStream.buf = (Byte *)ISzAlloc_Alloc(&allocImp, kInputBufSize);
    if (!lookStream.buf)
      res = SZ_ERROR_MEM;
    else
    {
      lookStream.bufSize = kInputBufSize;
      lookStream.realStream = &archiveStream.vt;
      LookToRead2_INIT(&lookStream)
    }
  }

  CrcGenerateTable();

  SzArEx_Init(&db);
  res = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp);
  if (res == SZ_OK)
  {
    UInt32 i;

    /*
    if you need cache, use these 3 variables.
    if you use external function, you can make these variable as static.
    */
    UInt32 blockIndex = 0xFFFFFFFF; /* it can have any value before first call (if outBuffer = 0) */
    Byte *outBuffer = 0; /* it must be 0 before first call for each new archive. */
    size_t outBufferSize = 0;  /* it can have any value before first call (if outBuffer = 0) */

    for (i = 0; i < db.NumFiles; i++)
    {
      size_t offset = 0;
      size_t outSizeProcessed = 0;
      // const CSzFileItem *f = db.Files + i;
      size_t len;
      int isDir = SzArEx_IsDir(&db, i);
      if (isDir && !fullPaths)
        continue;
      len = SzArEx_GetFileNameUtf16(&db, i, NULL);
      // len = SzArEx_GetFullNameLen(&db, i);

      if (len > tempSize)
      {
        SzFree(NULL, temp);
        tempSize = len;
        temp = (UInt16 *)SzAlloc(NULL, tempSize * sizeof(temp[0]));
        if (!temp)
        {
          res = SZ_ERROR_MEM;
          break;
        }
      }

      SzArEx_GetFileNameUtf16(&db, i, temp);
      /*
      if (SzArEx_GetFullNameUtf16_Back(&db, i, temp + len) != temp)
      {
        res = SZ_ERROR_FAIL;
        break;
      }
      */

      if (!isDir)
      {
        res = SzArEx_Extract(&db, &lookStream.vt, i,
            &blockIndex, &outBuffer, &outBufferSize,
            &offset, &outSizeProcessed,
            &allocImp, &allocTempImp);
        if (res != SZ_OK)
          break;
      }

      CSzFile outFile;
      size_t processedSize;
      size_t j;
      UInt16 *name = (UInt16 *)temp;
      const UInt16 *destPath = (const UInt16 *)name;
      for (j = 0; name[j] != 0; j++)
        if (name[j] == '/')
        {
          if (fullPaths)
          {
            name[j] = 0;
            MyCreateDir(name);
            name[j] = CHAR_PATH_SEPARATOR;
          }
          else
            destPath = name + j + 1;
        }

      if (isDir)
      {
        MyCreateDir(destPath);
        continue;
      }
      else if (OutFile_OpenUtf16(&outFile, destPath))
      {
        res = SZ_ERROR_FAIL;
        break;
      }
      processedSize = outSizeProcessed;
      if (File_Write(&outFile, outBuffer + offset, &processedSize) != 0 || processedSize != outSizeProcessed)
      {
        res = SZ_ERROR_FAIL;
        break;
      }
      if (File_Close(&outFile))
      {
        res = SZ_ERROR_FAIL;
        break;
      }
      #ifdef USE_WINDOWS_FILE
      if (SzBitWithVals_Check(&db.Attribs, i))
        SetFileAttributesW(destPath, db.Attribs.Vals[i]);
      #endif
    }
    IAlloc_Free(&allocImp, outBuffer);
  }
  SzArEx_Free(&db, &allocImp);
  SzFree(NULL, temp);

  File_Close(&archiveStream.file);

  return res;
}

SRes SzxList(const char* filename, char* list, size_t* size)
{
  CFileInStream archiveStream;
  CLookToRead2 lookStream;
  CSzArEx db;
  SRes res;
  ISzAlloc allocImp;
  ISzAlloc allocTempImp;
  UInt16 *temp = NULL;
  size_t tempSize = 0;
  // UInt32 parents[NUM_PARENTS_MAX];

  #if defined(_WIN32) && !defined(USE_WINDOWS_FILE) && !defined(UNDER_CE)
  g_FileCodePage = AreFileApisANSI() ? CP_ACP : CP_OEMCP;
  #endif

  allocImp = g_Alloc;

  allocTempImp = g_Alloc;

  #ifdef UNDER_CE
  if (InFile_OpenW(&archiveStream.file, L"\test.7z"))
  #else
  if (InFile_Open(&archiveStream.file, filename))
  #endif
  {
    return SZ_ERROR_FAIL;
  }

  FileInStream_CreateVTable(&archiveStream);
  archiveStream.wres = 0;
  LookToRead2_CreateVTable(&lookStream, False);
  lookStream.buf = NULL;

  {
    lookStream.buf = (Byte *)ISzAlloc_Alloc(&allocImp, kInputBufSize);
    if (!lookStream.buf)
      res = SZ_ERROR_MEM;
    else
    {
      lookStream.bufSize = kInputBufSize;
      lookStream.realStream = &archiveStream.vt;
      LookToRead2_INIT(&lookStream)
    }
  }

  CrcGenerateTable();

  SzArEx_Init(&db);
  res = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp);
  if (res == SZ_OK)
  {
    UInt32 i;
    size_t listIt = 0;

    /*
    if you need cache, use these 3 variables.
    if you use external function, you can make these variable as static.
    */
    Byte *outBuffer = 0; /* it must be 0 before first call for each new archive. */

    for (i = 0; i < db.NumFiles; i++)
    {
      // const CSzFileItem *f = db.Files + i;
      size_t len;
      const BoolInt isDir = SzArEx_IsDir(&db, i);
      len = SzArEx_GetFileNameUtf16(&db, i, NULL);
      // len = SzArEx_GetFullNameLen(&db, i);

      if (len > tempSize)
      {
        SzFree(NULL, temp);
        tempSize = len;
        temp = (UInt16 *)SzAlloc(NULL, tempSize * sizeof(temp[0]));
        if (!temp)
        {
          res = SZ_ERROR_MEM;
          break;
        }
      }

      SzArEx_GetFileNameUtf16(&db, i, temp);
      /*
      if (SzArEx_GetFullNameUtf16_Back(&db, i, temp + len) != temp)
      {
        res = SZ_ERROR_FAIL;
        break;
      }
      */

      char attr[8], s[32], t[32];
      UInt64 fileSize;

      GetAttribString(SzBitWithVals_Check(&db.Attribs, i) ? db.Attribs.Vals[i] : 0, isDir, attr);

      fileSize = SzArEx_GetFileSize(&db, i);
      UInt64ToStr(fileSize, s);
      if (SzBitWithVals_Check(&db.MTime, i))
        ConvertFileTimeToString(&db.MTime.Vals[i], t);
      else
      {
        size_t j;
        for (j = 0; j < 19; j++)
          t[j] = ' ';
        t[j] = '\0';
      }

      /*
      generate list
      */
      CBuf buf;
      Buf_Init(&buf);
      res = Utf16_To_Char(&buf, temp
      #ifdef _WIN32
      , CP_OEMCP
      #endif
      );

      if (res == SZ_OK)
      {
        size_t line = strlen(t) + strlen(attr) + strlen(s) + strlen(buf.data) + 3;

        if(listIt + line < *size)
        {
          strncat(list, t, 32);
          listIt += strlen(t);
          list[listIt] = '\t';
          list[listIt + 1] = '\0';

          strncat(list, attr, 8);
          listIt += strlen(attr) + 1;
          list[listIt] = '\t';
          list[listIt + 1] = '\0';

          strncat(list, s, 32);
          listIt += strlen(s) + 1;
          list[listIt] = '\t';
          list[listIt + 1] = '\0';

          strncat(list, buf.data, buf.size);
          listIt += strlen(buf.data) + 1;
          list[listIt] = '\n';

          ++listIt;
        }
      }

      Buf_Free(&buf, &g_Alloc);
    }

    *size = listIt;

    IAlloc_Free(&allocImp, outBuffer);
  }
  SzArEx_Free(&db, &allocImp);
  SzFree(NULL, temp);

  File_Close(&archiveStream.file);

  return res;
}

SRes SzxTest(const char* filename)
{
  CFileInStream archiveStream;
  CLookToRead2 lookStream;
  CSzArEx db;
  SRes res;
  ISzAlloc allocImp;
  ISzAlloc allocTempImp;
  UInt16 *temp = NULL;
  size_t tempSize = 0;
  // UInt32 parents[NUM_PARENTS_MAX];

  #if defined(_WIN32) && !defined(USE_WINDOWS_FILE) && !defined(UNDER_CE)
  g_FileCodePage = AreFileApisANSI() ? CP_ACP : CP_OEMCP;
  #endif

  allocImp = g_Alloc;

  allocTempImp = g_Alloc;

  #ifdef UNDER_CE
  if (InFile_OpenW(&archiveStream.file, L"\test.7z"))
  #else
  if (InFile_Open(&archiveStream.file, filename))
  #endif
  {
    return SZ_ERROR_FAIL;
  }

  FileInStream_CreateVTable(&archiveStream);
  archiveStream.wres = 0;
  LookToRead2_CreateVTable(&lookStream, False);
  lookStream.buf = NULL;

  {
    lookStream.buf = (Byte *)ISzAlloc_Alloc(&allocImp, kInputBufSize);
    if (!lookStream.buf)
      res = SZ_ERROR_MEM;
    else
    {
      lookStream.bufSize = kInputBufSize;
      lookStream.realStream = &archiveStream.vt;
      LookToRead2_INIT(&lookStream)
    }
  }

  CrcGenerateTable();

  SzArEx_Init(&db);
  res = SzArEx_Open(&db, &lookStream.vt, &allocImp, &allocTempImp);
  if (res == SZ_OK)
  {
    UInt32 i;

    /*
    if you need cache, use these 3 variables.
    if you use external function, you can make these variable as static.
    */
    UInt32 blockIndex = 0xFFFFFFFF; /* it can have any value before first call (if outBuffer = 0) */
    Byte *outBuffer = 0; /* it must be 0 before first call for each new archive. */
    size_t outBufferSize = 0;  /* it can have any value before first call (if outBuffer = 0) */

    for (i = 0; i < db.NumFiles; i++)
    {
      size_t offset = 0;
      size_t outSizeProcessed = 0;
      // const CSzFileItem *f = db.Files + i;
      size_t len;
      int isDir = SzArEx_IsDir(&db, i);
      if (isDir)
        continue;
      len = SzArEx_GetFileNameUtf16(&db, i, NULL);
      // len = SzArEx_GetFullNameLen(&db, i);

      if (len > tempSize)
      {
        SzFree(NULL, temp);
        tempSize = len;
        temp = (UInt16 *)SzAlloc(NULL, tempSize * sizeof(temp[0]));
        if (!temp)
        {
          res = SZ_ERROR_MEM;
          break;
        }
      }

      SzArEx_GetFileNameUtf16(&db, i, temp);
      /*
      if (SzArEx_GetFullNameUtf16_Back(&db, i, temp + len) != temp)
      {
        res = SZ_ERROR_FAIL;
        break;
      }
      */

      if (!isDir)
      {
        res = SzArEx_Extract(&db, &lookStream.vt, i,
            &blockIndex, &outBuffer, &outBufferSize,
            &offset, &outSizeProcessed,
            &allocImp, &allocTempImp);
        if (res != SZ_OK)
          break;
      }
    }
    IAlloc_Free(&allocImp, outBuffer);

  }
  SzArEx_Free(&db, &allocImp);
  SzFree(NULL, temp);

  File_Close(&archiveStream.file);

  return res;
}
