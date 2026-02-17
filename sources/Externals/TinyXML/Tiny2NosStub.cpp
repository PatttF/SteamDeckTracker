#include "Tiny2NosStub.h"
#include "System/Console/Trace.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void fprintf(I_File *f,char *fmt,...) {
     va_list args;
     va_start(args,fmt);

     // First pass: measure required size
     va_list args_copy;
     va_copy(args_copy, args);
     int needed = vsnprintf(NULL, 0, fmt, args_copy);
     va_end(args_copy);

     if (needed < 0) {
         va_end(args);
         return;
     }

     // Use stack buffer for small strings, heap for large ones
     char stackbuf[512];
     char *buffer = stackbuf;
     if ((size_t)(needed + 1) > sizeof(stackbuf)) {
         buffer = (char *)malloc(needed + 1);
         if (!buffer) {
             va_end(args);
             return;
         }
     }

     vsnprintf(buffer, needed + 1, fmt, args);
     va_end(args);

     f->Write(buffer, 1, needed);

     if (buffer != stackbuf) {
         free(buffer);
     }
}
