/*
 * Project 25 IMBE Encoder/Decoder Fixed-Point implementation
 * Developed by Pavel Yazev E-mail: pyazev@gmail.com
 * Version 1.0 (c) Copyright 2009
 * 
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * The software is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Boston, MA
 * 02110-1301, USA.
 */

#ifndef typedefs_h
#define typedefs_h "$Id $"

/*
********************************************************************************
*                         INCLUDE FILES
********************************************************************************
*/
#include <cfloat>
#include <climits>

#define __MSDOS__

/*
********************************************************************************
*                         DEFINITION OF CONSTANTS 
********************************************************************************
*/
/*
 ********* define char type
 */
typedef char Char;

/*
 ********* define 8 bit signed/unsigned types & constants
 */
#if SCHAR_MAX == 127
typedef signed char Word8;
#define minWord8  SCHAR_MIN
#define maxWord8  SCHAR_MAX

typedef unsigned char UWord8;
#define minUWord8 0
#define maxUWord8 UCHAR_MAX
#else
#error cannot find 8-bit type
#endif

/*
 ********* define 16 bit signed/unsigned types & constants
 */
#if INT_MAX == 32767
typedef int Word16;
#define minWord16     INT_MIN
#define maxWord16     INT_MAX
typedef unsigned int UWord16;
#define minUWord16    0
#define maxUWord16    UINT_MAX
#elif SHRT_MAX == 32767
typedef short Word16;
#define minWord16     SHRT_MIN
#define maxWord16     SHRT_MAX
typedef unsigned short UWord16;
#define minUWord16    0
#define maxUWord16    USHRT_MAX
#else
#error cannot find 16-bit type
#endif

/*
 ********* define 32 bit signed/unsigned types & constants
 */
#if INT_MAX == 2147483647
typedef int Word32;
#define minWord32     INT_MIN
#define maxWord32     INT_MAX
typedef unsigned int UWord32;
#define minUWord32    0
#define maxUWord32    UINT_MAX
#elif LONG_MAX == 2147483647
typedef long Word32;
#define minWord32     LONG_MIN
#define maxWord32     LONG_MAX
typedef unsigned long UWord32;
#define minUWord32    0
#define maxUWord32    ULONG_MAX
#else
#error cannot find 32-bit type
#endif

/*
 ********* define floating point type & constants
 */
/* use "#if 0" below if Float should be double;
   use "#if 1" below if Float should be float
 */
#if 0
typedef float Float;
#define maxFloat      FLT_MAX
#define minFloat      FLT_MIN
#else
typedef double Float;
#define maxFloat      DBL_MAX
#define minFloat      DBL_MIN
#endif

/*
 ********* define complex type
 */
typedef struct {
  Float r;  /* real      part */
  Float i;  /* imaginary part */
} CPX;

/*
 ********* define boolean type
 */
typedef int Bool;
#define false 0
#define true 1

/*
 ********* Check current platform
 */
#if defined(__MSDOS__)
#define PC
#define PLATFORM "PC"
#define LSBFIRST
#elif defined(__osf__)
#define OSF
#define PLATFORM "OSF"
#define LSBFIRST
#elif defined(__sun__) || defined(__sun)
#define SUN
#define PLATFORM "SUN"
#undef LSBFIRST
#elif defined(linux) && defined(i386)
#define PC
#define PLATFORM "PC"
#define LSBFIRST
#else
#error "can't determine architecture; adapt typedefs.h to your platform"
#endif

#endif

