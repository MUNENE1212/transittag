#include "qr_svg.h"
#include "qrcodegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *qr_svg_generate(const char *text, int border) {
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempBuf[qrcodegen_BUFFER_LEN_MAX];

    bool ok = qrcodegen_encodeText(text, tempBuf, qrcode,
        qrcodegen_Ecc_MEDIUM,
        qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
        qrcodegen_Mask_AUTO, true);
    if (!ok) return NULL;

    int sz = qrcodegen_getSize(qrcode);
    int total = sz + border * 2;

    /* SVG: each module = 1 unit, viewport scaled to 200px */
    size_t buflen = 512 + (size_t)(sz * sz * 50);
    char *svg = malloc(buflen);
    if (!svg) return NULL;

    int pos = 0;
    pos += snprintf(svg + pos, buflen - pos,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        "viewBox=\"0 0 %d %d\" "
        "style=\"width:200px;height:200px;shape-rendering:crispEdges\">"
        "<rect width=\"%d\" height=\"%d\" fill=\"white\"/>",
        total, total, total, total);

    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            if (qrcodegen_getModule(qrcode, x, y)) {
                pos += snprintf(svg + pos, buflen - pos,
                    "<rect x=\"%d\" y=\"%d\" width=\"1\" height=\"1\" fill=\"black\"/>",
                    x + border, y + border);
            }
        }
    }

    pos += snprintf(svg + pos, buflen - pos, "</svg>");
    return svg;
}
