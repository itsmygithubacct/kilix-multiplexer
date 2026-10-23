#include "kilix_mux.h"
#include <string.h>
int visible_equal(const kmx_grid *a, const kmx_grid *b) {
 if(a->rows!=b->rows || a->cols!=b->cols || a->cursor_row!=b->cursor_row || a->cursor_col!=b->cursor_col || a->cursor_visible!=b->cursor_visible) return 0;
 for(int i=0;i<a->rows*a->cols;i++) {
  const kmx_cell *x=&a->cells[i],*y=&b->cells[i];
  if(x->attrs!=y->attrs || x->width!=y->width || x->underline!=y->underline || memcmp(&x->fg,&y->fg,sizeof x->fg) || memcmp(&x->bg,&y->bg,sizeof x->bg)) return 0;
  for(int j=0;j<KMX_MAX_CHARS;j++) {
   uint32_t xc=x->chars[j],yc=y->chars[j];
   /* Erased blank and explicitly printed space are visually identical. */
   if(j==0 && x->width!=0){ if(!xc)xc=' ';if(!yc)yc=' '; }
   if(xc!=yc)return 0;
  }
 }
 return 1;
}
