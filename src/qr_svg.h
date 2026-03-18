#ifndef QR_SVG_H
#define QR_SVG_H

/* Generate a QR code SVG for the given text.
   Returns a malloc'd SVG string the caller must free(), or NULL on error.
   border: number of quiet-zone modules (4 is standard) */
char *qr_svg_generate(const char *text, int border);

#endif
