/***********************************************************************
 * 
 *  LUSH Lisp Universal Shell
 *    Copyright (C) 2002 Leon Bottou, Yann Le Cun, AT&T Corp, NECI.
 *  Includes parts of TL3:
 *    Copyright (C) 1987-1999 Leon Bottou and Neuristique.
 *  Includes selected parts of SN3.2:
 *    Copyright (C) 1991-2001 AT&T Corp.
 * 
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 * 
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 * 
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA
 * 
 ***********************************************************************/

/***********************************************************************
 * $Id: define.h,v 1.17 2011-10-09 22:48:05 leonb Exp $
 **********************************************************************/

#ifndef DEFINE_H
#define DEFINE_H

#ifdef HAVE_CONFIG_H
# include "lushconf.h"
#endif

/* --------- GENERAL PURPOSE DEFINITIONS ---------- */

#if HAVE_SYS_TYPES_H 
# include <sys/types.h>
#endif

#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <inttypes.h>

/* untyped pointer */
typedef void* gptr;
#define NIL 0L

/* boolean constants */
#ifndef TRUE
# define TRUE 1
# define FALSE 0
#endif

/* --------- MACHINE DEPENDANT STUFF -------- */

#ifdef UNIX
# define INIT_MACHINE      init_unix()
# define FINI_MACHINE      fini_unix()
# define TOPLEVEL_MACHINE  toplevel_unix()
# define CHECK_MACHINE(s)  check_unix(s);
# ifdef HAVE_WAITPID
#  define NEED_POPEN
# endif
# define popen             unix_popen
# define pclose            unix_pclose
#endif

#ifndef TLAPI
# define TLAPI            /**/
#endif
#ifndef LUSHAPI
# define LUSHAPI TLAPI
#endif
#ifndef INIT_MACHINE
# define INIT_MACHINE     /**/
#endif
#ifndef FINI_MACHINE
# define FINI_MACHINE     /**/
#endif
#ifndef TOPLEVEL_MACHINE
# define TOPLEVEL_MACHINE /**/
#endif
#ifndef CHECK_MACHINE
# define CHECK_MACHINE    /**/
#endif
#ifndef FMODE_TEXT
# define FMODE_TEXT(f)    /**/
#endif
#ifndef FMODE_BINARY
# define FMODE_BINARY(f)  /**/
#endif

/* --------- AUTOCONF --------- */

#include <string.h>

#ifdef HAVE_WCHAR_H
# include <wchar.h>
# include <limits.h>
# ifdef HAVE_WCTYPE_H
#  include <wctype.h>
# endif
# ifndef HAVE_WINT_T
#  define wint_t int
# endif
# ifndef HAVE_MBSTATE_T
#  define mbstate_t int
# endif
# ifndef HAVE_MBRTOWC
#  define mbrtowc(p,s,n,ps) mbtowc(p,s,n)
# endif
# ifndef HAVE_WCRTOMB
#  define wcrtomb(s,w,ps) wctomb(s,w)
# endif
# ifndef HAVE_MBRLEN
#  define mbrlen(s,n,ps) mblen(s,n)
# endif
#endif

#ifndef HAVE_SIGSETJMP
# ifndef sigsetjmp
#  ifndef siglongjmp
#   ifndef sigjmp_buf
#    define sigjmp_buf jmp_buf
#    define sigsetjmp(env, arg) setjmp(env)
#    define siglongjmp(env, arg) longjmp(env,arg)
#   endif
#  endif
# endif
#endif

#ifdef HAVE_ALLOCA_H
# include <alloca.h>
#endif

/* --------- GENERIC NAMES --------------- */

/* These macros may be redefined in
 * the machine dependent part, just below 
 */

# define name2(a,b)      _name2(a,b)
# define _name2(a,b)     a##b
# define name3(a,b,c)    _name3(a,b,c)
# define _name3(a,b,c)   a##b##c

/* return the variable in a string */
# define enclos2_in_string(a) #a
# define enclose_in_string(a) enclos2_in_string(a)

/* --------- FORMAT MACROS ---------- */

#ifdef INTG_IS_LONG
# define FMT_INTG "ld"
#else
# define FMT_INTG "d"
#endif

/* --------- LISP CONSTANTS --------- */

#define DXSTACKSIZE   (int)3000	/* max size for the DX stack */
#define MAXARGMAPC    (int)8	/* max number of listes in MAPCAR */
#define CONSCHUNKSIZE (int)2048	/* minimal cons allocation unit */
#define HASHTABLESIZE (int)1024	/* symbol hashtable size */
#define STRING_BUFFER (int)4096	/* string operations buffer size */
#define LINE_BUFFER   (int)1024	/* line buffer length */
#define FILELEN       (int)1024	/* file names length */
#define DZ_STACK_SIZE (int)1000 /* stack size for DZs */

/* --------- UNFORTUNATE NAMES -------- */

#define abort     TLabort
#define error     TLerror
#define class     TLclass
#define true      TLtrue
#define basename  TLbasename
#define dirname   TLdirname

/* ---------------------------------- */
#endif
