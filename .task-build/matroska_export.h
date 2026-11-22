
#ifndef MATROSKA_DLL_API_H
#define MATROSKA_DLL_API_H

#ifdef MATROSKA_STATIC_DEFINE
#  define MATROSKA_DLL_API
#  define TASK_PRODUCTION_NO_EXPORT
#else
#  ifndef MATROSKA_DLL_API
#    ifdef task_production_EXPORTS
        /* We are building this library */
#      define MATROSKA_DLL_API 
#    else
        /* We are using this library */
#      define MATROSKA_DLL_API 
#    endif
#  endif

#  ifndef TASK_PRODUCTION_NO_EXPORT
#    define TASK_PRODUCTION_NO_EXPORT 
#  endif
#endif

#ifndef TASK_PRODUCTION_DEPRECATED
#  define TASK_PRODUCTION_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef TASK_PRODUCTION_DEPRECATED_EXPORT
#  define TASK_PRODUCTION_DEPRECATED_EXPORT MATROSKA_DLL_API TASK_PRODUCTION_DEPRECATED
#endif

#ifndef TASK_PRODUCTION_DEPRECATED_NO_EXPORT
#  define TASK_PRODUCTION_DEPRECATED_NO_EXPORT TASK_PRODUCTION_NO_EXPORT TASK_PRODUCTION_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef TASK_PRODUCTION_NO_DEPRECATED
#    define TASK_PRODUCTION_NO_DEPRECATED
#  endif
#endif

#endif /* MATROSKA_DLL_API_H */
