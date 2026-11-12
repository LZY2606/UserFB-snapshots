/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

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

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 2

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1

/* Substitute the type names.  */
#define YYSTYPE         KAFEL_YYSTYPE
#define YYLTYPE         KAFEL_YYLTYPE
/* Substitute the variable and function names.  */
#define yyparse         kafel_yyparse
#define yylex           kafel_yylex
#define yyerror         kafel_yyerror
#define yydebug         kafel_yydebug
#define yynerrs         kafel_yynerrs

/* First part of user prologue.  */
#line 26 "../../src/parser.y"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"
#include "parser.h"
#include "lexer.h"

void yyerror(YYLTYPE * loc, struct kafel_ctxt* ctxt, yyscan_t scanner,
             const char *msg);

struct custom_syscall_args {
  int cur;
  struct custom_syscall_arg args[SYSCALL_MAX_ARGS];
};

struct custom_syscall_args* custom_syscall_args_create(void);
bool custom_syscall_args_has_already(struct custom_syscall_args* args, const char* name);
void custom_syscall_args_destroy(struct custom_syscall_args** args);

#define emit_error(loc, fmt, ...)                          \
    do {                                                   \
        append_error(ctxt, "%d:%d: "fmt, (loc).first_line, \
                    (loc).first_column, ##__VA_ARGS__);    \
    } while(0)

#define checked_expr_binary(result, loc, type, left, right)     \
    do {                                                        \
      if ((left)->depth >= MAX_EXPRESSION_DEPTH ||              \
        (right)->depth >= MAX_EXPRESSION_DEPTH) {               \
        emit_error((loc), "Max expression depth (%d) exceeded", \
                   MAX_EXPRESSION_DEPTH);                       \
        expr_destroy(&(left));                                  \
        expr_destroy(&(right));                                 \
        YYERROR;                                                \
      }                                                         \
      result = expr_create_binary((type), (left), (right));     \
    } while(0)

#define checked_expr_unary(result, loc, type, child)            \
    do {                                                        \
      if ((child)->depth >= MAX_EXPRESSION_DEPTH) {             \
        emit_error((loc), "Max expression depth (%d) exceeded", \
                   MAX_EXPRESSION_DEPTH);                       \
        expr_destroy(&(child));                                 \
        YYERROR;                                                \
      }                                                         \
      result = expr_create_unary((type), (child));              \
    } while(0)

#define YYLLOC_DEFAULT(Cur, Rhs, N )                    \
do {                                                    \
  if (N) {                                              \
    (Cur).filename = YYRHSLOC(Rhs, 1).filename;         \
    (Cur).first_line = YYRHSLOC(Rhs, 1).first_line;     \
    (Cur).first_column = YYRHSLOC(Rhs, 1).first_column; \
    (Cur).last_line = YYRHSLOC(Rhs, N).last_line;       \
    (Cur).last_column = YYRHSLOC(Rhs, N).last_column;   \
  }                                                     \
  else {                                                \
    (Cur).filename = YYRHSLOC(Rhs, 0).filename;         \
    (Cur).first_line = (Cur).last_line =                \
      YYRHSLOC(Rhs, 0).last_line;                       \
    (Cur).first_column = (Cur).last_column =            \
      YYRHSLOC(Rhs, 0).last_column;                     \
  }                                                     \
} while (0)

#line 148 "parser.c"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

#include "parser.h"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_BIT_AND = 3,                    /* BIT_AND  */
  YYSYMBOL_BIT_OR = 4,                     /* BIT_OR  */
  YYSYMBOL_LOGIC_OR = 5,                   /* LOGIC_OR  */
  YYSYMBOL_LOGIC_AND = 6,                  /* LOGIC_AND  */
  YYSYMBOL_IDENTIFIER = 7,                 /* IDENTIFIER  */
  YYSYMBOL_NUMBER = 8,                     /* NUMBER  */
  YYSYMBOL_POLICY = 9,                     /* POLICY  */
  YYSYMBOL_USE = 10,                       /* USE  */
  YYSYMBOL_DEFAULT = 11,                   /* DEFAULT  */
  YYSYMBOL_SYSCALL = 12,                   /* SYSCALL  */
  YYSYMBOL_DEFINE = 13,                    /* DEFINE  */
  YYSYMBOL_ON = 14,                        /* ON  */
  YYSYMBOL_ALLOW = 15,                     /* ALLOW  */
  YYSYMBOL_LOG = 16,                       /* LOG  */
  YYSYMBOL_KILL = 17,                      /* KILL  */
  YYSYMBOL_KILL_PROCESS = 18,              /* KILL_PROCESS  */
  YYSYMBOL_DENY = 19,                      /* DENY  */
  YYSYMBOL_ERRNO = 20,                     /* ERRNO  */
  YYSYMBOL_TRAP = 21,                      /* TRAP  */
  YYSYMBOL_TRACE = 22,                     /* TRACE  */
  YYSYMBOL_USER_NOTIF = 23,                /* USER_NOTIF  */
  YYSYMBOL_GT = 24,                        /* GT  */
  YYSYMBOL_LT = 25,                        /* LT  */
  YYSYMBOL_GE = 26,                        /* GE  */
  YYSYMBOL_LE = 27,                        /* LE  */
  YYSYMBOL_EQ = 28,                        /* EQ  */
  YYSYMBOL_NEQ = 29,                       /* NEQ  */
  YYSYMBOL_30_ = 30,                       /* '{'  */
  YYSYMBOL_31_ = 31,                       /* '}'  */
  YYSYMBOL_32_ = 32,                       /* ','  */
  YYSYMBOL_33_ = 33,                       /* '('  */
  YYSYMBOL_34_ = 34,                       /* ')'  */
  YYSYMBOL_35_ = 35,                       /* '['  */
  YYSYMBOL_36_ = 36,                       /* ']'  */
  YYSYMBOL_37_ = 37,                       /* '!'  */
  YYSYMBOL_38_ = 38,                       /* '#'  */
  YYSYMBOL_YYACCEPT = 39,                  /* $accept  */
  YYSYMBOL_program = 40,                   /* program  */
  YYSYMBOL_program_stmts = 41,             /* program_stmts  */
  YYSYMBOL_program_stmt = 42,              /* program_stmt  */
  YYSYMBOL_policy = 43,                    /* policy  */
  YYSYMBOL_policy_statements = 44,         /* policy_statements  */
  YYSYMBOL_policy_statement = 45,          /* policy_statement  */
  YYSYMBOL_use_statement = 46,             /* use_statement  */
  YYSYMBOL_action_block = 47,              /* action_block  */
  YYSYMBOL_action = 48,                    /* action  */
  YYSYMBOL_syscall_filters = 49,           /* syscall_filters  */
  YYSYMBOL_syscall_filter = 50,            /* syscall_filter  */
  YYSYMBOL_arch_guard_opt = 51,            /* arch_guard_opt  */
  YYSYMBOL_arch_list = 52,                 /* arch_list  */
  YYSYMBOL_syscall_id = 53,                /* syscall_id  */
  YYSYMBOL_syscall = 54,                   /* syscall  */
  YYSYMBOL_syscall_args = 55,              /* syscall_args  */
  YYSYMBOL_bool_expr = 56,                 /* bool_expr  */
  YYSYMBOL_or_bool_expr = 57,              /* or_bool_expr  */
  YYSYMBOL_and_bool_expr = 58,             /* and_bool_expr  */
  YYSYMBOL_primary_bool_expr = 59,         /* primary_bool_expr  */
  YYSYMBOL_bit_or_expr = 60,               /* bit_or_expr  */
  YYSYMBOL_bit_and_expr = 61,              /* bit_and_expr  */
  YYSYMBOL_operand = 62,                   /* operand  */
  YYSYMBOL_const_def = 63                  /* const_def  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_int8 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if 1

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* 1 */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined KAFEL_YYLTYPE_IS_TRIVIAL && KAFEL_YYLTYPE_IS_TRIVIAL \
             && defined KAFEL_YYSTYPE_IS_TRIVIAL && KAFEL_YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
  YYLTYPE yyls_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE) \
             + YYSIZEOF (YYLTYPE)) \
      + 2 * YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  31
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   146

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  39
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  25
/* YYNRULES -- Number of rules.  */
#define YYNRULES  65
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  120

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   284


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,    37,     2,    38,     2,     2,     2,     2,
      33,    34,     2,     2,    32,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,    35,     2,    36,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,    30,     2,    31,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29
};

#if KAFEL_YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,   179,   179,   183,   184,   188,   189,   198,   207,   218,
     219,   227,   228,   237,   238,   242,   256,   257,   261,   262,
     263,   264,   265,   266,   267,   275,   283,   294,   295,   304,
     306,   308,   314,   315,   326,   331,   342,   361,   371,   378,
     382,   389,   399,   406,   427,   428,   432,   434,   438,   440,
     444,   445,   446,   447,   448,   449,   450,   451,   455,   456,
     460,   461,   465,   466,   467,   480
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if 1
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "BIT_AND", "BIT_OR",
  "LOGIC_OR", "LOGIC_AND", "IDENTIFIER", "NUMBER", "POLICY", "USE",
  "DEFAULT", "SYSCALL", "DEFINE", "ON", "ALLOW", "LOG", "KILL",
  "KILL_PROCESS", "DENY", "ERRNO", "TRAP", "TRACE", "USER_NOTIF", "GT",
  "LT", "GE", "LE", "EQ", "NEQ", "'{'", "'}'", "','", "'('", "')'", "'['",
  "']'", "'!'", "'#'", "$accept", "program", "program_stmts",
  "program_stmt", "policy", "policy_statements", "policy_statement",
  "use_statement", "action_block", "action", "syscall_filters",
  "syscall_filter", "arch_guard_opt", "arch_list", "syscall_id", "syscall",
  "syscall_args", "bool_expr", "or_bool_expr", "and_bool_expr",
  "primary_bool_expr", "bit_or_expr", "bit_and_expr", "operand",
  "const_def", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-78)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-1)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
      33,    -3,    10,    85,   -78,   -78,   -78,   -78,   -78,    24,
      34,    77,   -78,    53,   109,    33,   -78,   -78,    79,   -78,
     -78,   -78,    55,   -78,    82,   -78,   -78,   105,   106,   107,
     110,   -78,   -78,    76,    58,    59,    84,    86,    87,   108,
     -78,   -78,    88,   -78,   -18,   -78,    89,   111,   -78,   -16,
     -78,   -78,   -78,   -78,   116,   -78,    61,    11,    28,    96,
     -78,    83,   -78,   -78,   -78,   -31,   -78,   120,     1,   -78,
     121,   -78,   -78,    52,   -78,   -78,   -78,     4,     4,    56,
     124,   125,   -78,    35,   127,   -78,   -78,   -78,   126,   -27,
       2,   -78,   -78,     4,     4,     4,    39,    39,    39,    39,
      39,    39,    39,    39,   -78,   -78,   -78,   124,   125,   -78,
      39,   127,   128,   128,   128,   128,   128,   128,   -78,     6
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       0,     0,     0,     0,    18,    19,    21,    22,    20,     0,
       0,     0,    23,     0,     0,     2,     3,     6,     7,    11,
      13,    14,     0,     5,     0,    15,     8,     0,     0,     0,
       0,     1,     4,     0,     0,     0,     0,     0,     0,     0,
      12,    37,     0,    16,     0,    27,    39,    32,     9,     0,
      24,    26,    25,    65,     0,    17,     0,     0,     0,    29,
      10,     0,    28,    42,    40,     0,    33,     0,     0,    38,
       0,    41,    35,     0,    64,    62,    30,     0,     0,     0,
      45,    47,    49,     0,    59,    61,    43,    34,     0,     0,
       0,    51,    31,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,    36,    50,    63,    44,    46,    48,
       0,    58,    52,    53,    54,    55,    56,    57,    60,     0
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
     -78,   -78,   -78,   119,   -78,   100,   103,   -78,   -78,   134,
     -78,    90,   -78,   -78,   -78,   -78,   -78,    62,    45,    46,
     -76,   -77,    47,    38,   -78
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int8 yydefgoto[] =
{
       0,    14,    15,    16,    17,    18,    19,    20,    21,    22,
      44,    45,    59,    73,    46,    47,    65,    79,    80,    81,
      82,    83,    84,    85,    23
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int8 yytable[] =
{
      90,    70,    91,    71,    24,    93,    96,   105,    74,    75,
      96,    74,    75,    55,    56,    60,    33,    25,    63,   109,
     112,   113,   114,   115,   116,   117,    97,    98,    99,   100,
     101,   102,    76,   119,    77,    66,   106,    77,    78,    96,
     106,    78,     1,     2,     3,    64,    74,    75,     4,     5,
       6,     7,     8,     9,    10,    11,    12,    27,    67,    97,
      98,    99,   100,   101,   102,    41,    30,    28,    41,     2,
      42,    13,   110,    42,     4,     5,     6,     7,     8,     9,
      10,    11,    12,    87,    88,    34,     2,    92,    93,    43,
      48,     4,     5,     6,     7,     8,     9,    10,    11,    12,
       4,     5,     6,     7,     8,     9,    10,    11,    12,    31,
      29,    33,    35,    36,    37,    38,    53,    39,    50,    69,
      51,    52,    57,    54,    61,    58,    68,    72,    86,    94,
     103,    95,    96,   104,    32,    49,    40,    26,   107,    89,
     108,   118,     0,   111,     0,     0,    62
};

static const yytype_int8 yycheck[] =
{
      77,    32,    78,    34,     7,    32,     4,    34,     7,     8,
       4,     7,     8,    31,    32,    31,    32,     7,     7,    95,
      97,    98,    99,   100,   101,   102,    24,    25,    26,    27,
      28,    29,    31,   110,    33,     7,    34,    33,    37,     4,
      34,    37,     9,    10,    11,    34,     7,     8,    15,    16,
      17,    18,    19,    20,    21,    22,    23,    33,    30,    24,
      25,    26,    27,    28,    29,     7,    13,    33,     7,    10,
      12,    38,    33,    12,    15,    16,    17,    18,    19,    20,
      21,    22,    23,    31,    32,    30,    10,    31,    32,    31,
      31,    15,    16,    17,    18,    19,    20,    21,    22,    23,
      15,    16,    17,    18,    19,    20,    21,    22,    23,     0,
      33,    32,    30,     8,     8,     8,     8,     7,    34,    36,
      34,    34,    33,    35,     8,    14,    30,     7,     7,     5,
       3,     6,     4,     7,    15,    35,    33,     3,    93,    77,
      94,   103,    -1,    96,    -1,    -1,    56
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,     9,    10,    11,    15,    16,    17,    18,    19,    20,
      21,    22,    23,    38,    40,    41,    42,    43,    44,    45,
      46,    47,    48,    63,     7,     7,    48,    33,    33,    33,
      13,     0,    42,    32,    30,    30,     8,     8,     8,     7,
      45,     7,    12,    31,    49,    50,    53,    54,    31,    44,
      34,    34,    34,     8,    35,    31,    32,    33,    14,    51,
      31,     8,    50,     7,    34,    55,     7,    30,    30,    36,
      32,    34,     7,    52,     7,     8,    31,    33,    37,    56,
      57,    58,    59,    60,    61,    62,     7,    31,    32,    56,
      60,    59,    31,    32,     5,     6,     4,    24,    25,    26,
      27,    28,    29,     3,     7,    34,    34,    57,    58,    59,
      33,    61,    60,    60,    60,    60,    60,    60,    62,    60
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] =
{
       0,    39,    40,    41,    41,    42,    42,    42,    42,    43,
      43,    44,    44,    45,    45,    46,    47,    47,    48,    48,
      48,    48,    48,    48,    48,    48,    48,    49,    49,    50,
      50,    50,    51,    51,    51,    52,    52,    53,    53,    54,
      54,    54,    55,    55,    56,    56,    57,    57,    58,    58,
      59,    59,    59,    59,    59,    59,    59,    59,    60,    60,
      61,    61,    62,    62,    62,    63
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     1,     1,     2,     1,     1,     1,     2,     4,
       5,     1,     3,     1,     1,     2,     3,     4,     1,     1,
       1,     1,     1,     1,     4,     4,     4,     1,     3,     2,
       4,     5,     0,     2,     4,     1,     3,     1,     4,     1,
       3,     4,     1,     3,     3,     1,     3,     1,     3,     1,
       3,     2,     3,     3,     3,     3,     3,     3,     3,     1,
       3,     1,     1,     3,     1,     4
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = KAFEL_YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == KAFEL_YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (&yylloc, ctxt, scanner, YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use KAFEL_YYerror or KAFEL_YYUNDEF. */
#define YYERRCODE KAFEL_YYUNDEF

/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#ifndef YYLLOC_DEFAULT
# define YYLLOC_DEFAULT(Current, Rhs, N)                                \
    do                                                                  \
      if (N)                                                            \
        {                                                               \
          (Current).first_line   = YYRHSLOC (Rhs, 1).first_line;        \
          (Current).first_column = YYRHSLOC (Rhs, 1).first_column;      \
          (Current).last_line    = YYRHSLOC (Rhs, N).last_line;         \
          (Current).last_column  = YYRHSLOC (Rhs, N).last_column;       \
        }                                                               \
      else                                                              \
        {                                                               \
          (Current).first_line   = (Current).last_line   =              \
            YYRHSLOC (Rhs, 0).last_line;                                \
          (Current).first_column = (Current).last_column =              \
            YYRHSLOC (Rhs, 0).last_column;                              \
        }                                                               \
    while (0)
#endif

#define YYRHSLOC(Rhs, K) ((Rhs)[K])


/* Enable debugging if requested.  */
#if KAFEL_YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)


/* YYLOCATION_PRINT -- Print the location on the stream.
   This macro was not mandated originally: define only if we know
   we won't break user code: when these are the locations we know.  */

# ifndef YYLOCATION_PRINT

#  if defined YY_LOCATION_PRINT

   /* Temporary convenience wrapper in case some people defined the
      undocumented and private YY_LOCATION_PRINT macros.  */
#   define YYLOCATION_PRINT(File, Loc)  YY_LOCATION_PRINT(File, *(Loc))

#  elif defined KAFEL_YYLTYPE_IS_TRIVIAL && KAFEL_YYLTYPE_IS_TRIVIAL

/* Print *YYLOCP on YYO.  Private, do not rely on its existence. */

YY_ATTRIBUTE_UNUSED
static int
yy_location_print_ (FILE *yyo, YYLTYPE const * const yylocp)
{
  int res = 0;
  int end_col = 0 != yylocp->last_column ? yylocp->last_column - 1 : 0;
  if (0 <= yylocp->first_line)
    {
      res += YYFPRINTF (yyo, "%d", yylocp->first_line);
      if (0 <= yylocp->first_column)
        res += YYFPRINTF (yyo, ".%d", yylocp->first_column);
    }
  if (0 <= yylocp->last_line)
    {
      if (yylocp->first_line < yylocp->last_line)
        {
          res += YYFPRINTF (yyo, "-%d", yylocp->last_line);
          if (0 <= end_col)
            res += YYFPRINTF (yyo, ".%d", end_col);
        }
      else if (0 <= end_col && yylocp->first_column < end_col)
        res += YYFPRINTF (yyo, "-%d", end_col);
    }
  return res;
}

#   define YYLOCATION_PRINT  yy_location_print_

    /* Temporary convenience wrapper in case some people defined the
       undocumented and private YY_LOCATION_PRINT macros.  */
#   define YY_LOCATION_PRINT(File, Loc)  YYLOCATION_PRINT(File, &(Loc))

#  else

#   define YYLOCATION_PRINT(File, Loc) ((void) 0)
    /* Temporary convenience wrapper in case some people defined the
       undocumented and private YY_LOCATION_PRINT macros.  */
#   define YY_LOCATION_PRINT  YYLOCATION_PRINT

#  endif
# endif /* !defined YYLOCATION_PRINT */


# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value, Location, ctxt, scanner); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp, struct kafel_ctxt* ctxt, yyscan_t scanner)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  YY_USE (yylocationp);
  YY_USE (ctxt);
  YY_USE (scanner);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp, struct kafel_ctxt* ctxt, yyscan_t scanner)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  YYLOCATION_PRINT (yyo, yylocationp);
  YYFPRINTF (yyo, ": ");
  yy_symbol_value_print (yyo, yykind, yyvaluep, yylocationp, ctxt, scanner);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp, YYLTYPE *yylsp,
                 int yyrule, struct kafel_ctxt* ctxt, yyscan_t scanner)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)],
                       &(yylsp[(yyi + 1) - (yynrhs)]), ctxt, scanner);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, yylsp, Rule, ctxt, scanner); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !KAFEL_YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !KAFEL_YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif


/* Context of a parse error.  */
typedef struct
{
  yy_state_t *yyssp;
  yysymbol_kind_t yytoken;
  YYLTYPE *yylloc;
} yypcontext_t;

/* Put in YYARG at most YYARGN of the expected tokens given the
   current YYCTX, and return the number of tokens stored in YYARG.  If
   YYARG is null, return the number of expected tokens (guaranteed to
   be less than YYNTOKENS).  Return YYENOMEM on memory exhaustion.
   Return 0 if there are more than YYARGN expected tokens, yet fill
   YYARG up to YYARGN. */
static int
yypcontext_expected_tokens (const yypcontext_t *yyctx,
                            yysymbol_kind_t yyarg[], int yyargn)
{
  /* Actual size of YYARG. */
  int yycount = 0;
  int yyn = yypact[+*yyctx->yyssp];
  if (!yypact_value_is_default (yyn))
    {
      /* Start YYX at -YYN if negative to avoid negative indexes in
         YYCHECK.  In other words, skip the first -YYN actions for
         this state because they are default actions.  */
      int yyxbegin = yyn < 0 ? -yyn : 0;
      /* Stay within bounds of both yycheck and yytname.  */
      int yychecklim = YYLAST - yyn + 1;
      int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
      int yyx;
      for (yyx = yyxbegin; yyx < yyxend; ++yyx)
        if (yycheck[yyx + yyn] == yyx && yyx != YYSYMBOL_YYerror
            && !yytable_value_is_error (yytable[yyx + yyn]))
          {
            if (!yyarg)
              ++yycount;
            else if (yycount == yyargn)
              return 0;
            else
              yyarg[yycount++] = YY_CAST (yysymbol_kind_t, yyx);
          }
    }
  if (yyarg && yycount == 0 && 0 < yyargn)
    yyarg[0] = YYSYMBOL_YYEMPTY;
  return yycount;
}




#ifndef yystrlen
# if defined __GLIBC__ && defined _STRING_H
#  define yystrlen(S) (YY_CAST (YYPTRDIFF_T, strlen (S)))
# else
/* Return the length of YYSTR.  */
static YYPTRDIFF_T
yystrlen (const char *yystr)
{
  YYPTRDIFF_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
# endif
#endif

#ifndef yystpcpy
# if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#  define yystpcpy stpcpy
# else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
static char *
yystpcpy (char *yydest, const char *yysrc)
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
# endif
#endif

#ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYPTRDIFF_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYPTRDIFF_T yyn = 0;
      char const *yyp = yystr;
      for (;;)
        switch (*++yyp)
          {
          case '\'':
          case ',':
            goto do_not_strip_quotes;

          case '\\':
            if (*++yyp != '\\')
              goto do_not_strip_quotes;
            else
              goto append;

          append:
          default:
            if (yyres)
              yyres[yyn] = *yyp;
            yyn++;
            break;

          case '"':
            if (yyres)
              yyres[yyn] = '\0';
            return yyn;
          }
    do_not_strip_quotes: ;
    }

  if (yyres)
    return yystpcpy (yyres, yystr) - yyres;
  else
    return yystrlen (yystr);
}
#endif


static int
yy_syntax_error_arguments (const yypcontext_t *yyctx,
                           yysymbol_kind_t yyarg[], int yyargn)
{
  /* Actual size of YYARG. */
  int yycount = 0;
  /* There are many possibilities here to consider:
     - If this state is a consistent state with a default action, then
       the only way this function was invoked is if the default action
       is an error action.  In that case, don't check for expected
       tokens because there are none.
     - The only way there can be no lookahead present (in yychar) is if
       this state is a consistent state with a default action.  Thus,
       detecting the absence of a lookahead is sufficient to determine
       that there is no unexpected or expected token to report.  In that
       case, just report a simple "syntax error".
     - Don't assume there isn't a lookahead just because this state is a
       consistent state with a default action.  There might have been a
       previous inconsistent state, consistent state with a non-default
       action, or user semantic action that manipulated yychar.
     - Of course, the expected token list depends on states to have
       correct lookahead information, and it depends on the parser not
       to perform extra reductions after fetching a lookahead from the
       scanner and before detecting a syntax error.  Thus, state merging
       (from LALR or IELR) and default reductions corrupt the expected
       token list.  However, the list is correct for canonical LR with
       one exception: it will still contain any token that will not be
       accepted due to an error action in a later state.
  */
  if (yyctx->yytoken != YYSYMBOL_YYEMPTY)
    {
      int yyn;
      if (yyarg)
        yyarg[yycount] = yyctx->yytoken;
      ++yycount;
      yyn = yypcontext_expected_tokens (yyctx,
                                        yyarg ? yyarg + 1 : yyarg, yyargn - 1);
      if (yyn == YYENOMEM)
        return YYENOMEM;
      else
        yycount += yyn;
    }
  return yycount;
}

/* Copy into *YYMSG, which is of size *YYMSG_ALLOC, an error message
   about the unexpected token YYTOKEN for the state stack whose top is
   YYSSP.

   Return 0 if *YYMSG was successfully written.  Return -1 if *YYMSG is
   not large enough to hold the message.  In that case, also set
   *YYMSG_ALLOC to the required number of bytes.  Return YYENOMEM if the
   required number of bytes is too large to store.  */
static int
yysyntax_error (YYPTRDIFF_T *yymsg_alloc, char **yymsg,
                const yypcontext_t *yyctx)
{
  enum { YYARGS_MAX = 5 };
  /* Internationalized format string. */
  const char *yyformat = YY_NULLPTR;
  /* Arguments of yyformat: reported tokens (one for the "unexpected",
     one per "expected"). */
  yysymbol_kind_t yyarg[YYARGS_MAX];
  /* Cumulated lengths of YYARG.  */
  YYPTRDIFF_T yysize = 0;

  /* Actual size of YYARG. */
  int yycount = yy_syntax_error_arguments (yyctx, yyarg, YYARGS_MAX);
  if (yycount == YYENOMEM)
    return YYENOMEM;

  switch (yycount)
    {
#define YYCASE_(N, S)                       \
      case N:                               \
        yyformat = S;                       \
        break
    default: /* Avoid compiler warnings. */
      YYCASE_(0, YY_("syntax error"));
      YYCASE_(1, YY_("syntax error, unexpected %s"));
      YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
      YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
      YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
      YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
#undef YYCASE_
    }

  /* Compute error message size.  Don't count the "%s"s, but reserve
     room for the terminator.  */
  yysize = yystrlen (yyformat) - 2 * yycount + 1;
  {
    int yyi;
    for (yyi = 0; yyi < yycount; ++yyi)
      {
        YYPTRDIFF_T yysize1
          = yysize + yytnamerr (YY_NULLPTR, yytname[yyarg[yyi]]);
        if (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM)
          yysize = yysize1;
        else
          return YYENOMEM;
      }
  }

  if (*yymsg_alloc < yysize)
    {
      *yymsg_alloc = 2 * yysize;
      if (! (yysize <= *yymsg_alloc
             && *yymsg_alloc <= YYSTACK_ALLOC_MAXIMUM))
        *yymsg_alloc = YYSTACK_ALLOC_MAXIMUM;
      return -1;
    }

  /* Avoid sprintf, as that infringes on the user's name space.
     Don't have undefined behavior even if the translation
     produced a string with the wrong number of "%s"s.  */
  {
    char *yyp = *yymsg;
    int yyi = 0;
    while ((*yyp = *yyformat) != '\0')
      if (*yyp == '%' && yyformat[1] == 's' && yyi < yycount)
        {
          yyp += yytnamerr (yyp, yytname[yyarg[yyi++]]);
          yyformat += 2;
        }
      else
        {
          ++yyp;
          ++yyformat;
        }
  }
  return 0;
}


/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep, YYLTYPE *yylocationp, struct kafel_ctxt* ctxt, yyscan_t scanner)
{
  YY_USE (yyvaluep);
  YY_USE (yylocationp);
  YY_USE (ctxt);
  YY_USE (scanner);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  switch (yykind)
    {
    case YYSYMBOL_IDENTIFIER: /* IDENTIFIER  */
#line 169 "../../src/parser.y"
            { free(((*yyvaluep).id)); }
#line 1383 "parser.c"
        break;

    case YYSYMBOL_policy: /* policy  */
#line 161 "../../src/parser.y"
            { policy_destroy(&((*yyvaluep).policy)); }
#line 1389 "parser.c"
        break;

    case YYSYMBOL_policy_statements: /* policy_statements  */
#line 163 "../../src/parser.y"
            { policy_entries_destroy(&((*yyvaluep).entries)); }
#line 1395 "parser.c"
        break;

    case YYSYMBOL_policy_statement: /* policy_statement  */
#line 162 "../../src/parser.y"
            { policy_entry_destroy(&((*yyvaluep).entry)); }
#line 1401 "parser.c"
        break;

    case YYSYMBOL_use_statement: /* use_statement  */
#line 162 "../../src/parser.y"
            { policy_entry_destroy(&((*yyvaluep).entry)); }
#line 1407 "parser.c"
        break;

    case YYSYMBOL_action_block: /* action_block  */
#line 162 "../../src/parser.y"
            { policy_entry_destroy(&((*yyvaluep).entry)); }
#line 1413 "parser.c"
        break;

    case YYSYMBOL_syscall_filters: /* syscall_filters  */
#line 165 "../../src/parser.y"
            { syscall_filters_destroy(&((*yyvaluep).filters)); }
#line 1419 "parser.c"
        break;

    case YYSYMBOL_syscall_filter: /* syscall_filter  */
#line 164 "../../src/parser.y"
            { syscall_filter_destroy(&((*yyvaluep).filter)); }
#line 1425 "parser.c"
        break;

    case YYSYMBOL_syscall_id: /* syscall_id  */
#line 167 "../../src/parser.y"
            { syscall_spec_destroy(&((*yyvaluep).syscall_spec)); }
#line 1431 "parser.c"
        break;

    case YYSYMBOL_syscall: /* syscall  */
#line 167 "../../src/parser.y"
            { syscall_spec_destroy(&((*yyvaluep).syscall_spec)); }
#line 1437 "parser.c"
        break;

    case YYSYMBOL_syscall_args: /* syscall_args  */
#line 166 "../../src/parser.y"
            { custom_syscall_args_destroy(&((*yyvaluep).syscall_args)); }
#line 1443 "parser.c"
        break;

    case YYSYMBOL_bool_expr: /* bool_expr  */
#line 168 "../../src/parser.y"
            { expr_destroy(&((*yyvaluep).expr)); }
#line 1449 "parser.c"
        break;

    case YYSYMBOL_or_bool_expr: /* or_bool_expr  */
#line 168 "../../src/parser.y"
            { expr_destroy(&((*yyvaluep).expr)); }
#line 1455 "parser.c"
        break;

    case YYSYMBOL_and_bool_expr: /* and_bool_expr  */
#line 168 "../../src/parser.y"
            { expr_destroy(&((*yyvaluep).expr)); }
#line 1461 "parser.c"
        break;

    case YYSYMBOL_primary_bool_expr: /* primary_bool_expr  */
#line 168 "../../src/parser.y"
            { expr_destroy(&((*yyvaluep).expr)); }
#line 1467 "parser.c"
        break;

    case YYSYMBOL_bit_or_expr: /* bit_or_expr  */
#line 168 "../../src/parser.y"
            { expr_destroy(&((*yyvaluep).expr)); }
#line 1473 "parser.c"
        break;

    case YYSYMBOL_bit_and_expr: /* bit_and_expr  */
#line 168 "../../src/parser.y"
            { expr_destroy(&((*yyvaluep).expr)); }
#line 1479 "parser.c"
        break;

    case YYSYMBOL_operand: /* operand  */
#line 168 "../../src/parser.y"
            { expr_destroy(&((*yyvaluep).expr)); }
#line 1485 "parser.c"
        break;

      default:
        break;
    }
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}






/*----------.
| yyparse.  |
`----------*/

int
yyparse (struct kafel_ctxt* ctxt, yyscan_t scanner)
{
/* Lookahead token kind.  */
int yychar;


/* The semantic value of the lookahead symbol.  */
/* Default value used for initialization, for pacifying older GCCs
   or non-GCC compilers.  */
YY_INITIAL_VALUE (static YYSTYPE yyval_default;)
YYSTYPE yylval YY_INITIAL_VALUE (= yyval_default);

/* Location data for the lookahead symbol.  */
static YYLTYPE yyloc_default
# if defined KAFEL_YYLTYPE_IS_TRIVIAL && KAFEL_YYLTYPE_IS_TRIVIAL
  = { 1, 1, 1, 1 }
# endif
;
YYLTYPE yylloc = yyloc_default;

    /* Number of syntax errors so far.  */
    int yynerrs = 0;

    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

    /* The location stack: array, bottom, top.  */
    YYLTYPE yylsa[YYINITDEPTH];
    YYLTYPE *yyls = yylsa;
    YYLTYPE *yylsp = yyls;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;
  YYLTYPE yyloc;

  /* The locations where the error started and ended.  */
  YYLTYPE yyerror_range[3];

  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYPTRDIFF_T yymsg_alloc = sizeof yymsgbuf;

#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N), yylsp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = KAFEL_YYEMPTY; /* Cause a token to be read.  */


/* User initialization code.  */
#line 172 "../../src/parser.y"
{
    yylloc.filename = NULL;
}

#line 1587 "parser.c"

  yylsp[0] = yylloc;
  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;
        YYLTYPE *yyls1 = yyls;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yyls1, yysize * YYSIZEOF (*yylsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
        yyls = yyls1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
        YYSTACK_RELOCATE (yyls_alloc, yyls);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;
      yylsp = yyls + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == KAFEL_YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex (&yylval, &yylloc, ctxt, scanner);
    }

  if (yychar <= KAFEL_YYEOF)
    {
      yychar = KAFEL_YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == KAFEL_YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = KAFEL_YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      yyerror_range[1] = yylloc;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END
  *++yylsp = yylloc;

  /* Discard the shifted token.  */
  yychar = KAFEL_YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];

  /* Default location. */
  YYLLOC_DEFAULT (yyloc, (yylsp - yylen), yylen);
  yyerror_range[1] = yyloc;
  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 6: /* program_stmt: policy  */
#line 190 "../../src/parser.y"
        {
          if (lookup_policy(ctxt, (yyvsp[0].policy)->name) != NULL) {
              emit_error((yylsp[0]), "Redefinition of policy `%s'", (yyvsp[0].policy)->name);
              policy_destroy(&(yyvsp[0].policy));
              YYERROR;
          }
          register_policy(ctxt, (yyvsp[0].policy));
        }
#line 1807 "parser.c"
    break;

  case 7: /* program_stmt: policy_statements  */
#line 199 "../../src/parser.y"
        {
          if (ctxt->main_policy == NULL) {
            ctxt->main_policy = policy_create("@main", &(yyvsp[0].entries));
            register_policy(ctxt, ctxt->main_policy);
          } else {
            TAILQ_CONCAT(&ctxt->main_policy->entries, &(yyvsp[0].entries), entries);
          }
        }
#line 1820 "parser.c"
    break;

  case 8: /* program_stmt: DEFAULT action  */
#line 208 "../../src/parser.y"
        {
          if (ctxt->default_action != 0) {
            emit_error((yylsp[-1]), "Redefinition of default action");
            YYERROR;
          }
          ctxt->default_action = (yyvsp[0].action);
        }
#line 1832 "parser.c"
    break;

  case 9: /* policy: POLICY IDENTIFIER '{' '}'  */
#line 218 "../../src/parser.y"
                                { (yyval.policy) = policy_create((yyvsp[-2].id), NULL); free((yyvsp[-2].id)); }
#line 1838 "parser.c"
    break;

  case 10: /* policy: POLICY IDENTIFIER '{' policy_statements '}'  */
#line 220 "../../src/parser.y"
        {
          (yyval.policy) = policy_create((yyvsp[-3].id), &(yyvsp[-1].entries));
          free((yyvsp[-3].id));
        }
#line 1847 "parser.c"
    break;

  case 11: /* policy_statements: policy_statement  */
#line 227 "../../src/parser.y"
                       { TAILQ_INIT(&(yyval.entries)); TAILQ_INSERT_TAIL(&(yyval.entries), (yyvsp[0].entry), entries); }
#line 1853 "parser.c"
    break;

  case 12: /* policy_statements: policy_statements ',' policy_statement  */
#line 229 "../../src/parser.y"
        {
          TAILQ_INIT(&(yyval.entries));
          TAILQ_CONCAT(&(yyval.entries), &(yyvsp[-2].entries), entries);
          TAILQ_INSERT_TAIL(&(yyval.entries), (yyvsp[0].entry), entries);
        }
#line 1863 "parser.c"
    break;

  case 15: /* use_statement: USE IDENTIFIER  */
#line 243 "../../src/parser.y"
        {
            struct policy* used = lookup_policy(ctxt, (yyvsp[0].id));
            if (used == NULL) {
                emit_error((yylsp[0]), "Undefined policy `%s'", (yyvsp[0].id));
                free((yyvsp[0].id)); (yyvsp[0].id) = NULL;
                YYERROR;
            }
            (yyval.entry) = policy_use_create(used);
            free((yyvsp[0].id));
        }
#line 1878 "parser.c"
    break;

  case 16: /* action_block: action '{' '}'  */
#line 256 "../../src/parser.y"
                     { (yyval.entry) = policy_action_create((yyvsp[-2].action), NULL); }
#line 1884 "parser.c"
    break;

  case 17: /* action_block: action '{' syscall_filters '}'  */
#line 257 "../../src/parser.y"
                                     { (yyval.entry) = policy_action_create((yyvsp[-3].action), &(yyvsp[-1].filters)); }
#line 1890 "parser.c"
    break;

  case 18: /* action: ALLOW  */
#line 261 "../../src/parser.y"
            { (yyval.action) = ACTION_ALLOW; }
#line 1896 "parser.c"
    break;

  case 19: /* action: LOG  */
#line 262 "../../src/parser.y"
           { (yyval.action) = ACTION_LOG; }
#line 1902 "parser.c"
    break;

  case 20: /* action: DENY  */
#line 263 "../../src/parser.y"
           { (yyval.action) = ACTION_KILL; }
#line 1908 "parser.c"
    break;

  case 21: /* action: KILL  */
#line 264 "../../src/parser.y"
           { (yyval.action) = ACTION_KILL; }
#line 1914 "parser.c"
    break;

  case 22: /* action: KILL_PROCESS  */
#line 265 "../../src/parser.y"
                   { (yyval.action) = ACTION_KILL_PROCESS; }
#line 1920 "parser.c"
    break;

  case 23: /* action: USER_NOTIF  */
#line 266 "../../src/parser.y"
                 { (yyval.action) = ACTION_USER_NOTIF; }
#line 1926 "parser.c"
    break;

  case 24: /* action: ERRNO '(' NUMBER ')'  */
#line 268 "../../src/parser.y"
        {
          if ((yyvsp[-1].number) > 0xffff) {
            emit_error((yylsp[-1]), "Errno value %"PRIu64" out of range (0-65535)", (yyvsp[-1].number));
            YYERROR;
          }
          (yyval.action) = ACTION_ERRNO | ((yyvsp[-1].number) & 0xffff);
        }
#line 1938 "parser.c"
    break;

  case 25: /* action: TRACE '(' NUMBER ')'  */
#line 276 "../../src/parser.y"
        {
          if ((yyvsp[-1].number) > 0xffff) {
            emit_error((yylsp[-1]), "Trace value %"PRIu64" out of range (0-65535)", (yyvsp[-1].number));
            YYERROR;
          }
          (yyval.action) = ACTION_TRACE | ((yyvsp[-1].number) & 0xffff);
        }
#line 1950 "parser.c"
    break;

  case 26: /* action: TRAP '(' NUMBER ')'  */
#line 284 "../../src/parser.y"
        {
          if ((yyvsp[-1].number) > 0xffff) {
            emit_error((yylsp[-1]), "Trap value %"PRIu64" out of range (0-65535)", (yyvsp[-1].number));
            YYERROR;
          }
          (yyval.action) = ACTION_TRAP | ((yyvsp[-1].number) & 0xffff);
        }
#line 1962 "parser.c"
    break;

  case 27: /* syscall_filters: syscall_filter  */
#line 294 "../../src/parser.y"
                     { TAILQ_INIT(&(yyval.filters)); TAILQ_INSERT_TAIL(&(yyval.filters), (yyvsp[0].filter), filters); }
#line 1968 "parser.c"
    break;

  case 28: /* syscall_filters: syscall_filters ',' syscall_filter  */
#line 296 "../../src/parser.y"
        {
          TAILQ_INIT(&(yyval.filters));
          TAILQ_CONCAT(&(yyval.filters), &(yyvsp[-2].filters), filters);
          TAILQ_INSERT_TAIL(&(yyval.filters), (yyvsp[0].filter), filters);
        }
#line 1978 "parser.c"
    break;

  case 29: /* syscall_filter: syscall arch_guard_opt  */
#line 305 "../../src/parser.y"
        { (yyval.filter) = syscall_filter_create((yyvsp[-1].syscall_spec), NULL, (yyvsp[0].archs)); }
#line 1984 "parser.c"
    break;

  case 30: /* syscall_filter: syscall arch_guard_opt '{' '}'  */
#line 307 "../../src/parser.y"
        { (yyval.filter) = syscall_filter_create((yyvsp[-3].syscall_spec), NULL, (yyvsp[-2].archs)); }
#line 1990 "parser.c"
    break;

  case 31: /* syscall_filter: syscall arch_guard_opt '{' bool_expr '}'  */
#line 309 "../../src/parser.y"
        { (yyval.filter) = syscall_filter_create((yyvsp[-4].syscall_spec), (yyvsp[-1].expr), (yyvsp[-3].archs)); }
#line 1996 "parser.c"
    break;

  case 32: /* arch_guard_opt: %empty  */
#line 314 "../../src/parser.y"
        { (yyval.archs) = 0; }
#line 2002 "parser.c"
    break;

  case 33: /* arch_guard_opt: ON IDENTIFIER  */
#line 316 "../../src/parser.y"
        {
          uint32_t arch = kafel_arch_lookup_by_name((yyvsp[0].id));
          if (arch == 0) {
            emit_error((yylsp[0]), "Unknown architecture `%s'", (yyvsp[0].id));
            free((yyvsp[0].id)); (yyvsp[0].id) = NULL;
            YYERROR;
          }
          (yyval.archs) = arch;
          free((yyvsp[0].id));
        }
#line 2017 "parser.c"
    break;

  case 34: /* arch_guard_opt: ON '{' arch_list '}'  */
#line 327 "../../src/parser.y"
        { (yyval.archs) = (yyvsp[-1].archs); }
#line 2023 "parser.c"
    break;

  case 35: /* arch_list: IDENTIFIER  */
#line 332 "../../src/parser.y"
        {
          uint32_t arch = kafel_arch_lookup_by_name((yyvsp[0].id));
          if (arch == 0) {
            emit_error((yylsp[0]), "Unknown architecture `%s'", (yyvsp[0].id));
            free((yyvsp[0].id)); (yyvsp[0].id) = NULL;
            YYERROR;
          }
          (yyval.archs) = arch;
          free((yyvsp[0].id));
        }
#line 2038 "parser.c"
    break;

  case 36: /* arch_list: arch_list ',' IDENTIFIER  */
#line 343 "../../src/parser.y"
        {
          uint32_t arch = kafel_arch_lookup_by_name((yyvsp[0].id));
          if (arch == 0) {
            emit_error((yylsp[0]), "Unknown architecture `%s'", (yyvsp[0].id));
            free((yyvsp[0].id)); (yyvsp[0].id) = NULL;
            YYERROR;
          }
          if ((yyvsp[-2].archs) & arch) {
            emit_error((yylsp[0]), "Duplicate architecture `%s'", (yyvsp[0].id));
            free((yyvsp[0].id)); (yyvsp[0].id) = NULL;
            YYERROR;
          }
          (yyval.archs) = (yyvsp[-2].archs) | arch;
          free((yyvsp[0].id));
        }
#line 2058 "parser.c"
    break;

  case 37: /* syscall_id: IDENTIFIER  */
#line 362 "../../src/parser.y"
        {
            uint64_t value = 0;
            if (lookup_const(ctxt, (yyvsp[0].id), &value) == 0) {
                (yyval.syscall_spec) = syscall_spec_create_custom(value);
            } else {
                (yyval.syscall_spec) = syscall_spec_create_identifier(kafel_identifier_create((yyvsp[0].id), &(yylsp[0])));
            }
            free((yyvsp[0].id));
        }
#line 2072 "parser.c"
    break;

  case 38: /* syscall_id: SYSCALL '[' NUMBER ']'  */
#line 372 "../../src/parser.y"
        {
            (yyval.syscall_spec) = syscall_spec_create_custom((yyvsp[-1].number));
        }
#line 2080 "parser.c"
    break;

  case 39: /* syscall: syscall_id  */
#line 379 "../../src/parser.y"
        {
          (yyval.syscall_spec) = (yyvsp[0].syscall_spec);
        }
#line 2088 "parser.c"
    break;

  case 40: /* syscall: syscall_id '(' ')'  */
#line 383 "../../src/parser.y"
        {
          (yyval.syscall_spec) = (yyvsp[-2].syscall_spec);
          struct custom_syscall_arg custom_syscall_args[SYSCALL_MAX_ARGS];
          memset(custom_syscall_args, 0, sizeof(custom_syscall_args));
          syscall_spec_set_custom_args((yyval.syscall_spec), custom_syscall_args);
        }
#line 2099 "parser.c"
    break;

  case 41: /* syscall: syscall_id '(' syscall_args ')'  */
#line 390 "../../src/parser.y"
        {
          (yyval.syscall_spec) = (yyvsp[-3].syscall_spec);
          syscall_spec_set_custom_args((yyval.syscall_spec), (yyvsp[-1].syscall_args)->args);
          memset((yyvsp[-1].syscall_args)->args, 0, sizeof((yyvsp[-1].syscall_args)->args));
          custom_syscall_args_destroy(&(yyvsp[-1].syscall_args));
        }
#line 2110 "parser.c"
    break;

  case 42: /* syscall_args: IDENTIFIER  */
#line 400 "../../src/parser.y"
        {
          (yyval.syscall_args) = custom_syscall_args_create();
          (yyval.syscall_args)->args[0].name = (yyvsp[0].id);
          (yyval.syscall_args)->args[0].size = 8;
          (yyval.syscall_args)->cur = 1;
        }
#line 2121 "parser.c"
    break;

  case 43: /* syscall_args: syscall_args ',' IDENTIFIER  */
#line 407 "../../src/parser.y"
        {
          (yyval.syscall_args) = (yyvsp[-2].syscall_args);
          if (custom_syscall_args_has_already((yyval.syscall_args), (yyvsp[0].id))) {
            emit_error((yylsp[0]), "Redefinition of argument `%s'", (yyvsp[0].id));
            custom_syscall_args_destroy(&(yyval.syscall_args));
            free((yyvsp[0].id)); (yyvsp[0].id) = NULL;
            YYERROR;
          }
          if ((yyval.syscall_args)->cur == SYSCALL_MAX_ARGS) {
            emit_error((yylsp[0]), "Too many arguments defined for syscall");
            custom_syscall_args_destroy(&(yyval.syscall_args));
            free((yyvsp[0].id)); (yyvsp[0].id) = NULL;
            YYERROR;
          }
          (yyval.syscall_args)->args[(yyval.syscall_args)->cur].size = 8;
          (yyval.syscall_args)->args[(yyval.syscall_args)->cur++].name = (yyvsp[0].id);
        }
#line 2143 "parser.c"
    break;

  case 44: /* bool_expr: bool_expr ',' or_bool_expr  */
#line 427 "../../src/parser.y"
                                 { checked_expr_binary((yyval.expr), (yylsp[-2]), EXPR_OR, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2149 "parser.c"
    break;

  case 45: /* bool_expr: or_bool_expr  */
#line 428 "../../src/parser.y"
                   { (yyval.expr) = (yyvsp[0].expr); }
#line 2155 "parser.c"
    break;

  case 46: /* or_bool_expr: or_bool_expr LOGIC_OR and_bool_expr  */
#line 433 "../../src/parser.y"
        { checked_expr_binary((yyval.expr), (yylsp[-2]), EXPR_OR, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2161 "parser.c"
    break;

  case 47: /* or_bool_expr: and_bool_expr  */
#line 434 "../../src/parser.y"
                    { (yyval.expr) = (yyvsp[0].expr); }
#line 2167 "parser.c"
    break;

  case 48: /* and_bool_expr: and_bool_expr LOGIC_AND primary_bool_expr  */
#line 439 "../../src/parser.y"
        { checked_expr_binary((yyval.expr), (yylsp[-2]), EXPR_AND, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2173 "parser.c"
    break;

  case 49: /* and_bool_expr: primary_bool_expr  */
#line 440 "../../src/parser.y"
                        { (yyval.expr) = (yyvsp[0].expr); }
#line 2179 "parser.c"
    break;

  case 50: /* primary_bool_expr: '(' bool_expr ')'  */
#line 444 "../../src/parser.y"
                        { (yyval.expr) = (yyvsp[-1].expr); }
#line 2185 "parser.c"
    break;

  case 51: /* primary_bool_expr: '!' primary_bool_expr  */
#line 445 "../../src/parser.y"
                            { checked_expr_unary((yyval.expr), (yylsp[-1]), EXPR_NOT, (yyvsp[0].expr)); }
#line 2191 "parser.c"
    break;

  case 52: /* primary_bool_expr: bit_or_expr GT bit_or_expr  */
#line 446 "../../src/parser.y"
                                 { checked_expr_binary((yyval.expr), (yylsp[-2]), EXPR_GT, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2197 "parser.c"
    break;

  case 53: /* primary_bool_expr: bit_or_expr LT bit_or_expr  */
#line 447 "../../src/parser.y"
                                 { checked_expr_binary((yyval.expr), (yylsp[-2]), EXPR_LT, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2203 "parser.c"
    break;

  case 54: /* primary_bool_expr: bit_or_expr GE bit_or_expr  */
#line 448 "../../src/parser.y"
                                 { checked_expr_binary((yyval.expr), (yylsp[-2]), EXPR_GE, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2209 "parser.c"
    break;

  case 55: /* primary_bool_expr: bit_or_expr LE bit_or_expr  */
#line 449 "../../src/parser.y"
                                 { checked_expr_binary((yyval.expr), (yylsp[-2]), EXPR_LE, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2215 "parser.c"
    break;

  case 56: /* primary_bool_expr: bit_or_expr EQ bit_or_expr  */
#line 450 "../../src/parser.y"
                                 { checked_expr_binary((yyval.expr), (yylsp[-2]), EXPR_EQ, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2221 "parser.c"
    break;

  case 57: /* primary_bool_expr: bit_or_expr NEQ bit_or_expr  */
#line 451 "../../src/parser.y"
                                  { checked_expr_binary((yyval.expr), (yylsp[-2]), EXPR_NEQ, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2227 "parser.c"
    break;

  case 58: /* bit_or_expr: bit_or_expr BIT_OR bit_and_expr  */
#line 455 "../../src/parser.y"
                                      { checked_expr_binary((yyval.expr), (yylsp[-2]), EXPR_BIT_OR, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2233 "parser.c"
    break;

  case 59: /* bit_or_expr: bit_and_expr  */
#line 456 "../../src/parser.y"
                   { (yyval.expr) = (yyvsp[0].expr); }
#line 2239 "parser.c"
    break;

  case 60: /* bit_and_expr: bit_and_expr BIT_AND operand  */
#line 460 "../../src/parser.y"
                                   { checked_expr_binary((yyval.expr), (yylsp[-2]), EXPR_BIT_AND, (yyvsp[-2].expr), (yyvsp[0].expr)); }
#line 2245 "parser.c"
    break;

  case 61: /* bit_and_expr: operand  */
#line 461 "../../src/parser.y"
              { (yyval.expr) = (yyvsp[0].expr); }
#line 2251 "parser.c"
    break;

  case 62: /* operand: NUMBER  */
#line 465 "../../src/parser.y"
             { (yyval.expr) = expr_create_number((yyvsp[0].number)); }
#line 2257 "parser.c"
    break;

  case 63: /* operand: '(' bit_or_expr ')'  */
#line 466 "../../src/parser.y"
                          { (yyval.expr) = (yyvsp[-1].expr); }
#line 2263 "parser.c"
    break;

  case 64: /* operand: IDENTIFIER  */
#line 468 "../../src/parser.y"
        {
          uint64_t value = 0;
          if (lookup_const(ctxt, (yyvsp[0].id), &value) == 0) {
            (yyval.expr) = expr_create_number(value);
          } else {
            (yyval.expr) = expr_create_identifier(kafel_identifier_create((yyvsp[0].id), &(yylsp[0])));
          }
          free((yyvsp[0].id));
        }
#line 2277 "parser.c"
    break;

  case 65: /* const_def: '#' DEFINE IDENTIFIER NUMBER  */
#line 481 "../../src/parser.y"
        {
          uint64_t value;
          if (lookup_const(ctxt, (yyvsp[-1].id), &value) == 0) {
            if (value != (yyvsp[0].number)) {
              emit_error((yylsp[-3]), "Redefinition of constant `%s' with different "
                         "value (was: %"PRIu64" is: %"PRIu64")", (yyvsp[-1].id), value, (yyvsp[0].number));
              free((yyvsp[-1].id)); (yyvsp[-1].id) = NULL;
              YYERROR;
            }
          } else {
            register_const(ctxt, (yyvsp[-1].id), (yyvsp[0].number));
          }
          free((yyvsp[-1].id));
        }
#line 2296 "parser.c"
    break;


#line 2300 "parser.c"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;
  *++yylsp = yyloc;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == KAFEL_YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      {
        yypcontext_t yyctx
          = {yyssp, yytoken, &yylloc};
        char const *yymsgp = YY_("syntax error");
        int yysyntax_error_status;
        yysyntax_error_status = yysyntax_error (&yymsg_alloc, &yymsg, &yyctx);
        if (yysyntax_error_status == 0)
          yymsgp = yymsg;
        else if (yysyntax_error_status == -1)
          {
            if (yymsg != yymsgbuf)
              YYSTACK_FREE (yymsg);
            yymsg = YY_CAST (char *,
                             YYSTACK_ALLOC (YY_CAST (YYSIZE_T, yymsg_alloc)));
            if (yymsg)
              {
                yysyntax_error_status
                  = yysyntax_error (&yymsg_alloc, &yymsg, &yyctx);
                yymsgp = yymsg;
              }
            else
              {
                yymsg = yymsgbuf;
                yymsg_alloc = sizeof yymsgbuf;
                yysyntax_error_status = YYENOMEM;
              }
          }
        yyerror (&yylloc, ctxt, scanner, yymsgp);
        if (yysyntax_error_status == YYENOMEM)
          YYNOMEM;
      }
    }

  yyerror_range[1] = yylloc;
  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= KAFEL_YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == KAFEL_YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval, &yylloc, ctxt, scanner);
          yychar = KAFEL_YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;

      yyerror_range[1] = *yylsp;
      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp, yylsp, ctxt, scanner);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  yyerror_range[2] = yylloc;
  ++yylsp;
  YYLLOC_DEFAULT (*yylsp, yyerror_range, 2);

  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (&yylloc, ctxt, scanner, YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != KAFEL_YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval, &yylloc, ctxt, scanner);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp, yylsp, ctxt, scanner);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
  return yyresult;
}

#line 497 "../../src/parser.y"


void yyerror(YYLTYPE * loc, struct kafel_ctxt* ctxt, yyscan_t scanner,
             const char *msg) {
  (void)scanner; /* suppress unused-parameter warning */
  if (!ctxt->lexical_error) {
    if (loc->filename != NULL) {
      append_error(ctxt, "%s:%d:%d: %s", loc->filename, loc->first_line, loc->first_column, msg);
    } else {
      append_error(ctxt, "%d:%d: %s", loc->first_line, loc->first_column, msg);
    }
  }
}

struct custom_syscall_args *custom_syscall_args_create(void) {
  struct custom_syscall_args *rv = calloc(1, sizeof(*rv));
  rv->cur = 0;
  return rv;
}

bool custom_syscall_args_has_already(struct custom_syscall_args* args, const char* name) {
  ASSERT(args != NULL);
  ASSERT(name != NULL);
  for (int i = 0; i < args->cur; ++i) {
    if (strcmp(args->args[i].name, name) == 0) {
      return true;
    }
  }
  return false;
}

void custom_syscall_args_destroy(struct custom_syscall_args **args) {
  ASSERT(args != NULL);
  ASSERT((*args) != NULL);
  for (int i = 0; i < SYSCALL_MAX_ARGS; ++i) {
    free((*args)->args[i].name);
  }
  free(*args);
  *args = NULL;
}
