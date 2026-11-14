/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_KAFEL_YY_PARSER_H_INCLUDED
# define YY_KAFEL_YY_PARSER_H_INCLUDED
/* Debug traces.  */
#ifndef KAFEL_YYDEBUG
# if defined YYDEBUG
#if YYDEBUG
#   define KAFEL_YYDEBUG 1
#  else
#   define KAFEL_YYDEBUG 0
#  endif
# else /* ! defined YYDEBUG */
#  define KAFEL_YYDEBUG 0
# endif /* ! defined YYDEBUG */
#endif  /* ! defined KAFEL_YYDEBUG */
#if KAFEL_YYDEBUG
extern int kafel_yydebug;
#endif
/* "%code requires" blocks.  */
#line 96 "../../src/parser.y"

#include <stdint.h>

#include "context.h"
#include "expression.h"
#include "policy.h"
#include "syscall.h"

#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void* yyscan_t;
#endif

#define KAFEL_YYLTYPE struct kafel_source_location

#line 73 "parser.h"

/* Token kinds.  */
#ifndef KAFEL_YYTOKENTYPE
# define KAFEL_YYTOKENTYPE
  enum kafel_yytokentype
  {
    KAFEL_YYEMPTY = -2,
    KAFEL_YYEOF = 0,               /* "end of file"  */
    KAFEL_YYerror = 256,           /* error  */
    KAFEL_YYUNDEF = 257,           /* "invalid token"  */
    BIT_AND = 258,                 /* BIT_AND  */
    BIT_OR = 259,                  /* BIT_OR  */
    LOGIC_OR = 260,                /* LOGIC_OR  */
    LOGIC_AND = 261,               /* LOGIC_AND  */
    IDENTIFIER = 262,              /* IDENTIFIER  */
    NUMBER = 263,                  /* NUMBER  */
    POLICY = 264,                  /* POLICY  */
    USE = 265,                     /* USE  */
    DEFAULT = 266,                 /* DEFAULT  */
    SYSCALL = 267,                 /* SYSCALL  */
    DEFINE = 268,                  /* DEFINE  */
    ON = 269,                      /* ON  */
    ALLOW = 270,                   /* ALLOW  */
    LOG = 271,                     /* LOG  */
    KILL = 272,                    /* KILL  */
    KILL_PROCESS = 273,            /* KILL_PROCESS  */
    DENY = 274,                    /* DENY  */
    ERRNO = 275,                   /* ERRNO  */
    TRAP = 276,                    /* TRAP  */
    TRACE = 277,                   /* TRACE  */
    USER_NOTIF = 278,              /* USER_NOTIF  */
    GT = 279,                      /* GT  */
    LT = 280,                      /* LT  */
    GE = 281,                      /* GE  */
    LE = 282,                      /* LE  */
    EQ = 283,                      /* EQ  */
    NEQ = 284                      /* NEQ  */
  };
  typedef enum kafel_yytokentype kafel_yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined KAFEL_YYSTYPE && ! defined KAFEL_YYSTYPE_IS_DECLARED
union KAFEL_YYSTYPE
{
#line 121 "../../src/parser.y"

    char *id;
    uint32_t action;
    uint64_t number;
    uint32_t archs;
    struct custom_syscall_args *syscall_args;
    struct expr_tree *expr;
    struct syscall_filter *filter;
    struct syscall_spec *syscall_spec;
    struct filterslist filters;
    struct policy_entry *entry;
    struct entrieslist entries;
    struct policy *policy;

#line 134 "parser.h"

};
typedef union KAFEL_YYSTYPE KAFEL_YYSTYPE;
# define KAFEL_YYSTYPE_IS_TRIVIAL 1
# define KAFEL_YYSTYPE_IS_DECLARED 1
#endif

/* Location type.  */
#if ! defined KAFEL_YYLTYPE && ! defined KAFEL_YYLTYPE_IS_DECLARED
typedef struct KAFEL_YYLTYPE KAFEL_YYLTYPE;
struct KAFEL_YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
};
# define KAFEL_YYLTYPE_IS_DECLARED 1
# define KAFEL_YYLTYPE_IS_TRIVIAL 1
#endif




int kafel_yyparse (struct kafel_ctxt* ctxt, yyscan_t scanner);


#endif /* !YY_KAFEL_YY_PARSER_H_INCLUDED  */
