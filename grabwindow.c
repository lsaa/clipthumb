/*
 * grabwindow.c
 *
 * Compile with:
 *   gcc grabwindow.c -o grabwindow -lX11 -lXcomposite -lXrender
 *
 * Usage:
 *   ./grabwindow <window_id> <output.bmp>
 *
 * You can get a window ID with `xwininfo` or `xdotool selectwindow`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>

static void write_bmp(const char *filename, unsigned char *data, int width, int height) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror("fopen");
        return;
    }

    int row_padded = (width * 3 + 3) & (~3);
    int filesize = 54 + row_padded * height;

    unsigned char bmpfileheader[14] = {
        'B','M',
        0,0,0,0,
        0,0,0,0,
        54,0,0,0
    };
    unsigned char bmpinfoheader[40] = {
        40,0,0,0,
        0,0,0,0,
        0,0,0,0,
        1,0,
        24,0,
        0,0,0,0,
        0,0,0,0,
        0,0,0,0,
        0,0,0,0,
        0,0,0,0
    };

    bmpfileheader[ 2] = (unsigned char)(filesize    );
    bmpfileheader[ 3] = (unsigned char)(filesize>> 8);
    bmpfileheader[ 4] = (unsigned char)(filesize>>16);
    bmpfileheader[ 5] = (unsigned char)(filesize>>24);

    bmpinfoheader[ 4] = (unsigned char)(width    );
    bmpinfoheader[ 5] = (unsigned char)(width>> 8);
    bmpinfoheader[ 6] = (unsigned char)(width>>16);
    bmpinfoheader[ 7] = (unsigned char)(width>>24);
    bmpinfoheader[ 8] = (unsigned char)(height    );
    bmpinfoheader[ 9] = (unsigned char)(height>> 8);
    bmpinfoheader[10] = (unsigned char)(height>>16);
    bmpinfoheader[11] = (unsigned char)(height>>24);

    fwrite(bmpfileheader,1,14,f);
    fwrite(bmpinfoheader,1,40,f);

    unsigned char *row = malloc(row_padded);
    for (int y = height - 1; y >= 0; y--) {
        for (int x = 0; x < width; x++) {
            unsigned char *px = data + (y * width + x) * 4;
            // Swap R and B from the original assumption
            row[x*3+0] = px[0]; // BMP B = XImage R
            row[x*3+1] = px[1]; // G stays the same
            row[x*3+2] = px[2]; // BMP R = XImage B
        }
        // pad row
        for (int p = width*3; p < row_padded; p++) row[p] = 0;
        fwrite(row, 1, row_padded, f);
    }
    free(row);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <window_id> <output.bmp>\n", argv[0]);
        return 1;
    }

    Window win = (Window)strtoul(argv[1], NULL, 0);
    const char *outfile = argv[2];

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }

    int event_base, error_base;
    if (!XCompositeQueryExtension(dpy, &event_base, &error_base)) {
        fprintf(stderr, "XComposite extension not available\n");
        return 1;
    }

    int major = 0, minor = 2;
    XCompositeQueryVersion(dpy, &major, &minor);
    if (major < 0 || (major == 0 && minor < 2)) {
        fprintf(stderr, "XComposite version too old\n");
        return 1;
    }

    XWindowAttributes attr;
    XGetWindowAttributes(dpy, win, &attr);
    int width = attr.width;
    int height = attr.height;

    // Redirect window to offscreen
    XCompositeRedirectWindow(dpy, win, CompositeRedirectAutomatic);

    Pixmap pixmap = XCompositeNameWindowPixmap(dpy, win);

    XRenderPictFormat *format = XRenderFindVisualFormat(dpy, attr.visual);
    if (!format) {
        fprintf(stderr, "No pict format for visual\n");
        return 1;
    }

    // Get image from pixmap
    XImage *img = XGetImage(dpy, pixmap, 0, 0, width, height, AllPlanes, ZPixmap);
    if (!img) {
        fprintf(stderr, "Failed to get image from pixmap\n");
        return 1;
    }

    // Save to BMP
    write_bmp(outfile, (unsigned char*)img->data, width, height);

    XDestroyImage(img);
    XFreePixmap(dpy, pixmap);
    XCloseDisplay(dpy);

    return 0;
}
