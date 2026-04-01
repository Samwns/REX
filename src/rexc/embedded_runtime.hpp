// Auto-generated: embedded runtime.h content
#pragma once
#include <string>

namespace rexc_embedded {
inline const char* runtime_h_content() {
    return R"RUNTIME_H(/*
 * rexc_runtime.h  –  REXC Compiler Runtime Library
 *
 * A small C99/C11 header providing the fundamental types and helper
 * functions that every file emitted by the REXC code-generator includes.
 *
 * Provides:
 *   rexc_str   – heap-or-literal C string value type
 *   rexc_vec   – type-erased dynamic array (void* elements)
 *   I/O helpers that mirror std::cout << chaining
 *   new/delete wrappers (calloc / free)
 *   Minimal exception stub (terminates with message)
 */

#ifndef REXC_RUNTIME_H
#define REXC_RUNTIME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <math.h>
#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * rexc_str  –  C++ std::string substitute
 * ================================================================ */

typedef struct rexc_str {
    char*  data;      /* NUL-terminated character buffer           */
    size_t len;       /* number of chars (not counting NUL)        */
    size_t cap;       /* allocated capacity (0 = not heap-owned)   */
} rexc_str;

/* Wrap a string literal – no allocation, data is NOT freed */
static inline rexc_str rexc_str_from_lit(const char* s) {
    rexc_str r;
    r.data = (char*)(uintptr_t)s;   /* drop const – read-only, cap==0 guards against free */
    r.len  = s ? strlen(s) : 0;
    r.cap  = 0;
    return r;
}

/* Make a pointer to a stack rexc_str from a literal (C11 compound literal) */
#define REXC_STR_LIT(s)   (&(rexc_str){.data=(char*)(uintptr_t)("" s), .len=sizeof("" s)-1, .cap=0})

/* Allocate a copy of s */
static inline rexc_str rexc_str_new(const char* s) {
    rexc_str r;
    r.len  = s ? strlen(s) : 0;
    r.cap  = r.len + 1;
    r.data = (char*)malloc(r.cap);
    if (r.data) { memcpy(r.data, s ? s : "", r.len + 1); }
    else        { r.cap = 0; r.len = 0; }
    return r;
}

/* Allocate a copy from another rexc_str */
static inline rexc_str rexc_str_copy(const rexc_str* src) {
    return rexc_str_new(src->data);
}

/* Append b to a, return new string */
static inline rexc_str rexc_str_cat(const rexc_str* a, const rexc_str* b) {
    rexc_str r;
    r.len  = a->len + b->len;
    r.cap  = r.len + 1;
    r.data = (char*)malloc(r.cap);
    if (r.data) {
        if (a->len) memcpy(r.data,          a->data, a->len);
        if (b->len) memcpy(r.data + a->len, b->data, b->len);
        r.data[r.len] = '\0';
    }
    return r;
}

/* Append C-string literal to rexc_str */
static inline rexc_str rexc_str_cat_cstr(const rexc_str* a, const char* b) {
    rexc_str tmp = rexc_str_from_lit(b);
    return rexc_str_cat(a, &tmp);
}

/* Get read-only C-string pointer */
static inline const char* rexc_cstr(const rexc_str* s) {
    return (s && s->data) ? s->data : "";
}

/* Equality */
static inline int rexc_str_eq(const rexc_str* a, const rexc_str* b) {
    if (a->len != b->len) return 0;
    return (a->len == 0) || (memcmp(a->data, b->data, a->len) == 0);
}

/* Lexicographic compare: <0, 0, >0 */
static inline int rexc_str_cmp(const rexc_str* a, const rexc_str* b) {
    size_t min_len = a->len < b->len ? a->len : b->len;
    int r = (min_len > 0) ? memcmp(a->data, b->data, min_len) : 0;
    if (r != 0) return r;
    if (a->len < b->len) return -1;
    if (a->len > b->len) return  1;
    return 0;
}

/* Free heap-owned buffer */
static inline void rexc_str_free(rexc_str* s) {
    if (s->cap > 0 && s->data) { free(s->data); }
    s->data = NULL; s->len = 0; s->cap = 0;
}

/* Substring [start, start+length) */
static inline rexc_str rexc_str_substr(const rexc_str* s, size_t start, size_t length) {
    if (start >= s->len) return rexc_str_new("");
    if (start + length > s->len) length = s->len - start;
    rexc_str r;
    r.cap  = length + 1;
    r.len  = length;
    r.data = (char*)malloc(r.cap);
    if (r.data) { memcpy(r.data, s->data + start, length); r.data[length] = '\0'; }
    return r;
}

/* Integer/float → string */
static inline rexc_str rexc_str_from_int(int64_t n) {
    char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)n);
    return rexc_str_new(buf);
}
static inline rexc_str rexc_str_from_uint(uint64_t n) {
    char buf[32]; snprintf(buf, sizeof(buf), "%llu", (unsigned long long)n);
    return rexc_str_new(buf);
}
static inline rexc_str rexc_str_from_double(double d) {
    char buf[64]; snprintf(buf, sizeof(buf), "%g", d);
    return rexc_str_new(buf);
}

/* ================================================================
 * rexc_vec  –  C++ std::vector substitute (void* elements)
 *
 * Callers cast elements to/from void* themselves.
 * For value types (int, struct by value) the address of a temporary
 * is stored – callers must manage lifetime.
 * ================================================================ */

typedef struct rexc_vec {
    void** data;    /* array of element pointers */
    size_t size;    /* number of elements        */
    size_t cap;     /* allocated capacity        */
} rexc_vec;

static inline void rexc_vec_init(rexc_vec* v) {
    v->data = NULL; v->size = 0; v->cap = 0;
}

static inline void rexc_vec_reserve(rexc_vec* v, size_t new_cap) {
    if (new_cap <= v->cap) return;
    void** nd = (void**)realloc(v->data, new_cap * sizeof(void*));
    if (nd) { v->data = nd; v->cap = new_cap; }
}

static inline void rexc_vec_push_back(rexc_vec* v, void* item) {
    if (v->size >= v->cap) {
        size_t nc = (v->cap == 0) ? 4 : v->cap * 2;
        rexc_vec_reserve(v, nc);
    }
    v->data[v->size++] = item;
}

static inline void* rexc_vec_at(const rexc_vec* v, size_t i) {
    return (i < v->size) ? v->data[i] : NULL;
}

static inline void rexc_vec_pop_back(rexc_vec* v) {
    if (v->size > 0) v->size--;
}

static inline void rexc_vec_clear(rexc_vec* v) { v->size = 0; }

static inline void rexc_vec_free(rexc_vec* v) {
    free(v->data); v->data = NULL; v->size = 0; v->cap = 0;
}

/* ================================================================
 * rexc_ostream  –  std::cout / std::cerr substitute
 *
 * Usage: rexc_cout(&rexc_stdout_stream, value_expr);
 * A thin wrapper around printf-family so chaining works.
 * ================================================================ */

typedef struct { FILE* fp; } rexc_ostream;

static rexc_ostream rexc_stdout_stream = { NULL };   /* initialised below */
static rexc_ostream rexc_stderr_stream = { NULL };

/* ================================================================
 * rexc_istream  –  std::cin substitute
 *
 * Usage: rexc_cin_int(&rexc_stdin_stream, &var);
 * Each overload returns the stream pointer so >> can be chained.
 * ================================================================ */

typedef struct { FILE* fp; } rexc_istream;

static rexc_istream rexc_stdin_stream = { NULL };   /* initialised below */

/* Call once before main uses cout/cerr/cin */
static inline void rexc_streams_init(void) {
    rexc_stdout_stream.fp = stdout;
    rexc_stderr_stream.fp = stderr;
    rexc_stdin_stream.fp  = stdin;
}

/* Each overload returns the stream pointer so << can be chained */
static inline rexc_ostream* rexc_cout_str   (rexc_ostream* s, const rexc_str* v)  { fputs(rexc_cstr(v), s->fp); return s; }
static inline rexc_ostream* rexc_cout_str_v (rexc_ostream* s, rexc_str v)         { fputs(rexc_cstr(&v), s->fp); return s; }
static inline rexc_ostream* rexc_cout_cstr  (rexc_ostream* s, const char* v)      { if(v) fputs(v, s->fp); return s; }
static inline rexc_ostream* rexc_cout_char  (rexc_ostream* s, char v)             { fputc(v, s->fp); return s; }
static inline rexc_ostream* rexc_cout_int   (rexc_ostream* s, int64_t v)          { fprintf(s->fp, "%lld", (long long)v); return s; }
static inline rexc_ostream* rexc_cout_uint  (rexc_ostream* s, uint64_t v)         { fprintf(s->fp, "%llu", (unsigned long long)v); return s; }
static inline rexc_ostream* rexc_cout_double(rexc_ostream* s, double v)           { fprintf(s->fp, "%g",   v); return s; }
static inline rexc_ostream* rexc_cout_float (rexc_ostream* s, float v)            { fprintf(s->fp, "%g",   (double)v); return s; }
static inline rexc_ostream* rexc_cout_bool  (rexc_ostream* s, int v)              { fputs(v ? "true" : "false", s->fp); return s; }
static inline rexc_ostream* rexc_cout_ptr   (rexc_ostream* s, const void* v)      { fprintf(s->fp, "%p", v); return s; }
static inline rexc_ostream* rexc_cout_endl  (rexc_ostream* s)                     { fputc('\n', s->fp); fflush(s->fp); return s; }
static inline rexc_ostream* rexc_cout_flush (rexc_ostream* s)                     { fflush(s->fp); return s; }

/* ── rexc_istream input functions ──────────────────────────────── */

#define REXC_INPUT_BUF_SIZE 4096

/* fscanf errors are silently ignored to match C++ cin behaviour */
static inline rexc_istream* rexc_cin_int   (rexc_istream* s, int* v)      { fflush(stdout); (void)fscanf(s->fp, "%d", v); return s; }
static inline rexc_istream* rexc_cin_long  (rexc_istream* s, int64_t* v)  { fflush(stdout); long long t; if(fscanf(s->fp, "%lld", &t)==1) *v=(int64_t)t; return s; }
static inline rexc_istream* rexc_cin_uint  (rexc_istream* s, uint64_t* v) { fflush(stdout); unsigned long long t; if(fscanf(s->fp, "%llu", &t)==1) *v=(uint64_t)t; return s; }
static inline rexc_istream* rexc_cin_float (rexc_istream* s, float* v)    { fflush(stdout); (void)fscanf(s->fp, "%f", v); return s; }
static inline rexc_istream* rexc_cin_double(rexc_istream* s, double* v)   { fflush(stdout); (void)fscanf(s->fp, "%lf", v); return s; }
static inline rexc_istream* rexc_cin_char  (rexc_istream* s, char* v)     { fflush(stdout); (void)fscanf(s->fp, " %c", v); return s; }
static inline rexc_istream* rexc_cin_str   (rexc_istream* s, rexc_str* v) {
    fflush(stdout);
    char buf[REXC_INPUT_BUF_SIZE];
    if (fscanf(s->fp, "%4095s", buf) == 1) {
        rexc_str_free(v);
        *v = rexc_str_new(buf);
    }
    return s;
}

/* std::getline(cin, str) equivalent */
static inline rexc_istream* rexc_getline(rexc_istream* s, rexc_str* v) {
    fflush(stdout);
    char buf[REXC_INPUT_BUF_SIZE];
    if (fgets(buf, sizeof(buf), s->fp)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
        if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
        rexc_str_free(v);
        *v = rexc_str_new(buf);
    }
    return s;
}

/* ================================================================
 * Memory helpers  –  new / delete / new[] / delete[]
 * ================================================================ */

/* Allocate zeroed storage for one T-sized object */
#define REXC_NEW(T)       ((T*)calloc(1, sizeof(T)))
/* Allocate zeroed storage for N T-sized objects */
#define REXC_NEW_ARR(T,N) ((T*)calloc((N), sizeof(T)))
/* Free a single object */
#define REXC_DELETE(ptr)      do { free(ptr); (ptr)=NULL; } while(0)
/* Free an array */
#define REXC_DELETE_ARR(ptr)  do { free(ptr); (ptr)=NULL; } while(0)

/* ================================================================
 * Exception stub  –  throw / try / catch
 *
 * Real exception handling requires setjmp/longjmp or C++ try/catch.
 * The REXC code-generator emits rexc_throw() for throw statements and
 * rexc_try_begin / rexc_try_end macros for try blocks.  In this
 * initial implementation exceptions terminate the process.
 * ================================================================ */

typedef struct {
    int   type_id;      /* hash of type name */
    char* message;      /* heap-allocated, may be NULL */
    void* object;       /* pointer to thrown object, may be NULL */
} rexc_exception;

static rexc_exception rexc_current_exception = {0, NULL, NULL};

static inline void rexc_set_exception(int tid, const char* msg, void* obj) {
    rexc_current_exception.type_id = tid;
    if (msg) {
        free(rexc_current_exception.message);
        rexc_current_exception.message = strdup(msg);
    }
    rexc_current_exception.object = obj;
}

#define rexc_throw_msg(msg) \
    do { fprintf(stderr, "REXC exception: %s\n", (msg)); exit(1); } while(0)

#define rexc_throw_obj(tid, msg, obj) \
    do { rexc_set_exception((tid),(msg),(obj)); \
         fprintf(stderr, "REXC exception (type %d): %s\n", (tid), (msg) ? (msg) : ""); exit(1); } while(0)

/* ================================================================
 * Utility macros
 * ================================================================ */

/* static_assert equivalent */
#ifndef REXC_STATIC_ASSERT
#  if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L)
#    define REXC_STATIC_ASSERT(expr, msg) _Static_assert((expr), msg)
#  else
#    define REXC_STATIC_ASSERT(expr, msg) \
         typedef char rexc_sa_##__LINE__[(expr) ? 1 : -1]
#  endif
#endif

/* nullptr / true / false for C99 */
#ifndef nullptr
#  ifdef __cplusplus
     /* already defined */
#  else
#    define nullptr NULL
#  endif
#endif

/* ================================================================
 * ANSI Terminal Colors & Styles
 *
 * Provides C equivalents for C++ termcolor-style stream manipulators.
 * Names match the codegen output from  using namespace termcolor;
 *   fg::red   → fg_red      bg::red   → bg_red
 *   bold      → bold        reset     → reset
 * ================================================================ */

#ifdef __GNUC__
#define REXC_MAYBE_UNUSED __attribute__((unused))
#else
#define REXC_MAYBE_UNUSED
#endif

/* ── Reset ─────────────────────────────────────────────────────── */
static const char* const reset REXC_MAYBE_UNUSED = "\033[0m";

/* ── Text styles ───────────────────────────────────────────────── */
static const char* const bold          REXC_MAYBE_UNUSED = "\033[1m";
static const char* const dim           REXC_MAYBE_UNUSED = "\033[2m";
static const char* const italic        REXC_MAYBE_UNUSED = "\033[3m";
static const char* const underline     REXC_MAYBE_UNUSED = "\033[4m";
static const char* const blink         REXC_MAYBE_UNUSED = "\033[5m";
static const char* const reverse       REXC_MAYBE_UNUSED = "\033[7m";
static const char* const strikethrough REXC_MAYBE_UNUSED = "\033[9m";

/* ── Foreground colors ─────────────────────────────────────────── */
static const char* const fg_black          REXC_MAYBE_UNUSED = "\033[30m";
static const char* const fg_red            REXC_MAYBE_UNUSED = "\033[31m";
static const char* const fg_green          REXC_MAYBE_UNUSED = "\033[32m";
static const char* const fg_yellow         REXC_MAYBE_UNUSED = "\033[33m";
static const char* const fg_blue           REXC_MAYBE_UNUSED = "\033[34m";
static const char* const fg_magenta        REXC_MAYBE_UNUSED = "\033[35m";
static const char* const fg_cyan           REXC_MAYBE_UNUSED = "\033[36m";
static const char* const fg_white          REXC_MAYBE_UNUSED = "\033[37m";

static const char* const fg_bright_black   REXC_MAYBE_UNUSED = "\033[90m";
static const char* const fg_bright_red     REXC_MAYBE_UNUSED = "\033[91m";
static const char* const fg_bright_green   REXC_MAYBE_UNUSED = "\033[92m";
static const char* const fg_bright_yellow  REXC_MAYBE_UNUSED = "\033[93m";
static const char* const fg_bright_blue    REXC_MAYBE_UNUSED = "\033[94m";
static const char* const fg_bright_magenta REXC_MAYBE_UNUSED = "\033[95m";
static const char* const fg_bright_cyan    REXC_MAYBE_UNUSED = "\033[96m";
static const char* const fg_bright_white   REXC_MAYBE_UNUSED = "\033[97m";

/* ── Background colors ─────────────────────────────────────────── */
static const char* const bg_black          REXC_MAYBE_UNUSED = "\033[40m";
static const char* const bg_red            REXC_MAYBE_UNUSED = "\033[41m";
static const char* const bg_green          REXC_MAYBE_UNUSED = "\033[42m";
static const char* const bg_yellow         REXC_MAYBE_UNUSED = "\033[43m";
static const char* const bg_blue           REXC_MAYBE_UNUSED = "\033[44m";
static const char* const bg_magenta        REXC_MAYBE_UNUSED = "\033[45m";
static const char* const bg_cyan           REXC_MAYBE_UNUSED = "\033[46m";
static const char* const bg_white          REXC_MAYBE_UNUSED = "\033[47m";

static const char* const bg_bright_black   REXC_MAYBE_UNUSED = "\033[100m";
static const char* const bg_bright_red     REXC_MAYBE_UNUSED = "\033[101m";
static const char* const bg_bright_green   REXC_MAYBE_UNUSED = "\033[102m";
static const char* const bg_bright_yellow  REXC_MAYBE_UNUSED = "\033[103m";
static const char* const bg_bright_blue    REXC_MAYBE_UNUSED = "\033[104m";
static const char* const bg_bright_magenta REXC_MAYBE_UNUSED = "\033[105m";
static const char* const bg_bright_cyan    REXC_MAYBE_UNUSED = "\033[106m";
static const char* const bg_bright_white   REXC_MAYBE_UNUSED = "\033[107m";

/* ── True Color RGB helpers ────────────────────────────────────
 * Note: these use static buffers and are NOT thread-safe.
 * The rexc runtime targets single-threaded generated code.     */

static inline const char* fg_rgb(int r, int g, int b) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "\033[38;2;%d;%d;%dm", r, g, b);
    return buf;
}

static inline const char* bg_rgb(int r, int g, int b) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "\033[48;2;%d;%d;%dm", r, g, b);
    return buf;
}

/* ── 256-color palette helpers ─────────────────────────────────
 * Note: these use static buffers and are NOT thread-safe.       */

static inline const char* fg_color256(int idx) {
    static char buf[24];
    snprintf(buf, sizeof(buf), "\033[38;5;%dm", idx);
    return buf;
}

static inline const char* bg_color256(int idx) {
    static char buf[24];
    snprintf(buf, sizeof(buf), "\033[48;5;%dm", idx);
    return buf;
}

/* ── Utility: colored / styled ─────────────────────────────────── */

static inline rexc_str colored(rexc_str text, const char* color) {
    rexc_str c = rexc_str_from_lit(color);
    rexc_str r = rexc_str_from_lit(reset);
    rexc_str tmp = rexc_str_cat(&c, &text);
    rexc_str result = rexc_str_cat(&tmp, &r);
    rexc_str_free(&tmp);
    return result;
}

static inline rexc_str colored2(rexc_str text, const char* c1, const char* c2) {
    rexc_str s1 = rexc_str_from_lit(c1);
    rexc_str s2 = rexc_str_from_lit(c2);
    rexc_str r  = rexc_str_from_lit(reset);
    rexc_str prefix = rexc_str_cat(&s1, &s2);
    rexc_str tmp    = rexc_str_cat(&prefix, &text);
    rexc_str result = rexc_str_cat(&tmp, &r);
    rexc_str_free(&prefix);
    rexc_str_free(&tmp);
    return result;
}

static inline rexc_str colored3(rexc_str text, const char* c1, const char* c2, const char* c3) {
    rexc_str s1 = rexc_str_from_lit(c1);
    rexc_str s2 = rexc_str_from_lit(c2);
    rexc_str s3 = rexc_str_from_lit(c3);
    rexc_str r  = rexc_str_from_lit(reset);
    rexc_str p1     = rexc_str_cat(&s1, &s2);
    rexc_str prefix = rexc_str_cat(&p1, &s3);
    rexc_str tmp    = rexc_str_cat(&prefix, &text);
    rexc_str result = rexc_str_cat(&tmp, &r);
    rexc_str_free(&p1);
    rexc_str_free(&prefix);
    rexc_str_free(&tmp);
    return result;
}

static inline rexc_str styled(rexc_str text, const char* color, const char* style) {
    rexc_str c = rexc_str_from_lit(color);
    rexc_str s = rexc_str_from_lit(style);
    rexc_str r = rexc_str_from_lit(reset);
    rexc_str tmp1 = rexc_str_cat(&c, &s);
    rexc_str tmp2 = rexc_str_cat(&tmp1, &text);
    rexc_str result = rexc_str_cat(&tmp2, &r);
    rexc_str_free(&tmp1);
    rexc_str_free(&tmp2);
    return result;
}

static inline rexc_str styled2(rexc_str text, const char* c1, const char* c2, const char* c3) {
    rexc_str s1 = rexc_str_from_lit(c1);
    rexc_str s2 = rexc_str_from_lit(c2);
    rexc_str s3 = rexc_str_from_lit(c3);
    rexc_str r  = rexc_str_from_lit(reset);
    rexc_str p1     = rexc_str_cat(&s1, &s2);
    rexc_str prefix = rexc_str_cat(&p1, &s3);
    rexc_str tmp    = rexc_str_cat(&prefix, &text);
    rexc_str result = rexc_str_cat(&tmp, &r);
    rexc_str_free(&p1);
    rexc_str_free(&prefix);
    rexc_str_free(&tmp);
    return result;
}

static inline rexc_str styled3(rexc_str text, const char* c1, const char* c2, const char* c3, const char* c4) {
    rexc_str s1 = rexc_str_from_lit(c1);
    rexc_str s2 = rexc_str_from_lit(c2);
    rexc_str s3 = rexc_str_from_lit(c3);
    rexc_str s4 = rexc_str_from_lit(c4);
    rexc_str r  = rexc_str_from_lit(reset);
    rexc_str p1     = rexc_str_cat(&s1, &s2);
    rexc_str p2     = rexc_str_cat(&p1, &s3);
    rexc_str prefix = rexc_str_cat(&p2, &s4);
    rexc_str tmp    = rexc_str_cat(&prefix, &text);
    rexc_str result = rexc_str_cat(&tmp, &r);
    rexc_str_free(&p1);
    rexc_str_free(&p2);
    rexc_str_free(&prefix);
    rexc_str_free(&tmp);
    return result;
}

/* ── Print helpers ─────────────────────────────────────────────── */

static inline void print_error(rexc_str msg) {
    fprintf(stdout, "\033[91m\033[1m[ERRO] \033[0m\033[31m%s\033[0m\n", rexc_cstr(&msg));
}

static inline void print_success(rexc_str msg) {
    fprintf(stdout, "\033[92m\033[1m[OK] \033[0m\033[32m%s\033[0m\n", rexc_cstr(&msg));
}

static inline void print_warning(rexc_str msg) {
    fprintf(stdout, "\033[93m\033[1m[AVISO] \033[0m\033[33m%s\033[0m\n", rexc_cstr(&msg));
}

static inline void print_info(rexc_str msg) {
    fprintf(stdout, "\033[96m\033[1m[INFO] \033[0m\033[36m%s\033[0m\n", rexc_cstr(&msg));
}

/* ================================================================
 * rexc_roll  –  Dice rolling library  (roll:: namespace in C++)
 *
 * All functions prefixed with  roll_  so that the REXC codegen can
 * translate  roll::d6()  →  roll_d6()  via its normal scope-mangling.
 * ================================================================ */

#include <time.h>

static int rexc_roll_seeded_ = 0;

static inline void rexc_roll_ensure_seed(void) {
    if (!rexc_roll_seeded_) {
        srand((unsigned)time(NULL));
        rexc_roll_seeded_ = 1;
    }
}

/* ── Basic die roll ────────────────────────────────────────────── */

static inline int roll_dice(int faces) {
    rexc_roll_ensure_seed();
    return (rand() % faces) + 1;
}

static inline int roll_dice_n(int faces, int count) {
    int total = 0;
    for (int i = 0; i < count; i++) total += roll_dice(faces);
    return total;
}

/* ── Standard dice (0-arg and 1-arg overloads) ─────────────────── */

static inline int roll_d4(void)    { return roll_dice(4);   }
static inline int roll_d6(void)    { return roll_dice(6);   }
static inline int roll_d8(void)    { return roll_dice(8);   }
static inline int roll_d10(void)   { return roll_dice(10);  }
static inline int roll_d12(void)   { return roll_dice(12);  }
static inline int roll_d20(void)   { return roll_dice(20);  }
static inline int roll_d100(void)  { return roll_dice(100); }

static inline int roll_d4_n(int n)   { return roll_dice_n(4, n);   }
static inline int roll_d6_n(int n)   { return roll_dice_n(6, n);   }
static inline int roll_d8_n(int n)   { return roll_dice_n(8, n);   }
static inline int roll_d10_n(int n)  { return roll_dice_n(10, n);  }
static inline int roll_d12_n(int n)  { return roll_dice_n(12, n);  }
static inline int roll_d20_n(int n)  { return roll_dice_n(20, n);  }
static inline int roll_d100_n(int n) { return roll_dice_n(100, n); }

/* ── Detailed roll result ──────────────────────────────────────── */

typedef struct roll_result {
    int  values[64];   /* individual die results (max 64 dice) */
    int  count;        /* number of dice rolled                */
    int  total;        /* sum of all dice                      */
    int  faces;        /* number of faces per die              */
} roll_result;

static inline roll_result roll_roll_full(int faces, int count) {
    roll_result r;
    r.faces = faces;
    r.count = (count > 64) ? 64 : count;
    r.total = 0;
    for (int i = 0; i < r.count; i++) {
        r.values[i] = roll_dice(faces);
        r.total += r.values[i];
    }
    return r;
}

static inline rexc_str roll_result_to_string(const roll_result* r) {
    char buf[512];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                    "%dd%d = %d [", r->count, r->faces, r->total);
    for (int i = 0; i < r->count; i++) {
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ", ");
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%d", r->values[i]);
    }
    snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]");
    return rexc_str_new(buf);
}

static inline int roll_result_max(const roll_result* r) {
    if (r->count <= 0) return 0;
    int m = r->values[0];
    for (int i = 1; i < r->count; i++)
        if (r->values[i] > m) m = r->values[i];
    return m;
}

static inline int roll_result_min(const roll_result* r) {
    if (r->count <= 0) return 0;
    int m = r->values[0];
    for (int i = 1; i < r->count; i++)
        if (r->values[i] < m) m = r->values[i];
    return m;
}

/* ── Advantage / Disadvantage (2d20) ──────────────────────────── */

static inline int roll_advantage(void) {
    int a = roll_dice(20), b = roll_dice(20);
    return (a > b) ? a : b;
}

static inline int roll_disadvantage(void) {
    int a = roll_dice(20), b = roll_dice(20);
    return (a < b) ? a : b;
}

/* ── Roll with modifier ───────────────────────────────────────── */

static inline int roll_with_mod(int faces, int count, int mod) {
    return roll_dice_n(faces, count) + mod;
}

/* ── Character stats (4d6 drop lowest, ×6) ────────────────────── */

static inline int* roll_character_stats(void) {
    static int stats[6];
    for (int i = 0; i < 6; i++) {
        int rolls[4];
        for (int j = 0; j < 4; j++) rolls[j] = roll_dice(6);
        int min_v = rolls[0];
        for (int j = 1; j < 4; j++) if (rolls[j] < min_v) min_v = rolls[j];
        stats[i] = 0;
        for (int j = 0; j < 4; j++) stats[i] += rolls[j];
        stats[i] -= min_v;
    }
    return stats;
}

/* ── Critical / critical fail / chance ────────────────────────── */

static inline int roll_critical(void)      { return roll_dice(20) == 20; }
static inline int roll_critical_fail(void) { return roll_dice(20) == 1;  }

static inline int roll_chance(int percent) {
    rexc_roll_ensure_seed();
    return ((rand() % 100) + 1) <= percent;
}

/* ── Pick random element from rexc_vec ────────────────────────── */

static inline void* roll_pick_ptr(const rexc_vec* v) {
    if (v->size == 0) return NULL;
    rexc_roll_ensure_seed();
    return v->data[rand() % v->size];
}

/* ── Shuffle a rexc_vec in-place (Fisher-Yates) ───────────────── */

static inline void roll_shuffle_vec(rexc_vec* v) {
    rexc_roll_ensure_seed();
    for (size_t i = v->size; i > 1; i--) {
        size_t j = (size_t)(rand() % (int)i);
        void* tmp  = v->data[i - 1];
        v->data[i - 1] = v->data[j];
        v->data[j] = tmp;
    }
}

/* ── Parse dice notation  "NdF" or "NdF+M" / "NdF-M" ─────────── */

static inline int roll_parse(const char* notation) {
    int n = 0, f = 0, mod = 0;
    const char* p = notation;
    /* parse count */
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
    if (n == 0) n = 1;
    if (*p == 'd' || *p == 'D') p++;
    /* parse faces */
    while (*p >= '0' && *p <= '9') { f = f * 10 + (*p - '0'); p++; }
    if (f == 0) f = 6;
    /* parse modifier */
    if (*p == '+') { p++; while (*p >= '0' && *p <= '9') { mod = mod * 10 + (*p - '0'); p++; } }
    else if (*p == '-') { p++; int m = 0; while (*p >= '0' && *p <= '9') { m = m * 10 + (*p - '0'); p++; } mod = -m; }
    return roll_dice_n(f, n) + mod;
}

/* ================================================================
 * Runtime initialisation  –  call from generated main() preamble
 * ================================================================ */

static inline void rexc_runtime_init(void) {
    rexc_streams_init();
    rexc_roll_ensure_seed();
#ifdef _WIN32
    /* Enable UTF-8 console output on Windows 10+ */
    setlocale(LC_ALL, ".UTF-8");
#endif
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* REXC_RUNTIME_H */
)RUNTIME_H";
}
}
