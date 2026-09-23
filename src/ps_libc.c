#ifndef PS_LIBC_H
#define PS_LIBC_H

#include "core.h"

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct _FILE FILE;
struct _FILE {
    s32 fd;
    s32 eof;
    s32 err;
    /* PREAD buffer so W_AddFile in DOOM-style readers works */
    u8 *buf;
    int buf_len, buf_pos;
};

extern FILE *stdin, *stdout, *stderr;

/* init */
void  ps_libc_init(void *G, void *D, void *mmap_fn,
                   void *kopen, void *kread, void *kwrite,
                   void *kclose, void *klseek, void *kmkdir,
                   void *sendto, s32 log_fd, u8 *log_sa);
void  ps_libc_reset_pool(void);
void  ps_libc_set_error_cb(void (*cb)(const char *));

/* mem */
void *memset(void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int   memcmp(const void *a, const void *b, size_t n);

/* str */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
char  *strcat(char *d, const char *s);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strdup(const char *s);

/* fmt */
int printf(const char *fmt, ...);
int snprintf(char *b, size_t n, const char *fmt, ...);
int vsnprintf(char *b, size_t n, const char *fmt, __builtin_va_list ap);

/* heap */
void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void  free(void *p);

/* file */
FILE *fopen(const char *path, const char *mode);
size_t fread(void *p, size_t sz, size_t n, FILE *f);
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f);
int    fclose(FILE *f);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
int    feof(FILE *f);

int mkdir(const char *path, unsigned int mode);
int remove(const char *path);

/* util */
int atoi(const char *s);

#endif
