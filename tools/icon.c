// Icon tool for the kit, built against Termux's desktop raylib (Image functions only: no window needed).
//   icon res <icon.png> <resdir>        adaptive launcher icon: mipmap-*/ic_launcher_fg.png + mipmap-anydpi-v26 xml
//   icon placeholder <out.png> <seed>   a generated 1024x1024 placeholder icon (colors from the seed text)
// icon.png is the full-bleed square art. Launchers mask it to a circle/squircle showing about the middle 2/3,
// so keep the important part inside the center ~60%.
#include "raylib.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void writeFile(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static int res(const char *src, const char *dir)
{
    Image img = LoadImage(src);
    if (!img.data) { fprintf(stderr, "icon: can't load %s\n", src); return 1; }
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    int side = img.width < img.height ? img.width : img.height;
    ImageCrop(&img, (Rectangle){ (img.width - side) / 2.0f, (img.height - side) / 2.0f, side, side });
    // adaptive icon layers are 108dp
    const char *dpi[] = { "mdpi", "hdpi", "xhdpi", "xxhdpi", "xxxhdpi" };
    const int px[] = { 108, 162, 216, 324, 432 };
    char path[1024];
    for (int i = 0; i < 5; i++) {
        snprintf(path, sizeof path, "%s/mipmap-%s", dir, dpi[i]);
        mkdir(path, 0755);
        Image s = ImageCopy(img);
        ImageResize(&s, px[i], px[i]);
        snprintf(path, sizeof path, "%s/mipmap-%s/ic_launcher_fg.png", dir, dpi[i]);
        if (!ExportImage(s, path)) return 1;
        UnloadImage(s);
    }
    UnloadImage(img);
    snprintf(path, sizeof path, "%s/mipmap-anydpi-v26", dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/mipmap-anydpi-v26/ic_launcher.xml", dir);
    writeFile(path, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<adaptive-icon xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
                    "    <background android:drawable=\"@android:color/black\" />\n"
                    "    <foreground android:drawable=\"@mipmap/ic_launcher_fg\" />\n"
                    "</adaptive-icon>\n");
    return 0;
}

static Color hsv(float h, float s, float v) { return ColorFromHSV(fmodf(h + 360, 360), s, v); }

static int placeholder(const char *out, const char *seed)
{
    unsigned hash = 2166136261u;
    for (const char *p = seed; *p; p++) hash = (hash ^ (unsigned char)*p) * 16777619u;
    float hue = hash % 360;
    const int N = 1024;
    Color *px = malloc(N * N * sizeof(Color));
    Color a = hsv(hue, 0.75f, 0.35f), b = hsv(hue + 40, 0.85f, 0.12f), c = hsv(hue - 20, 0.55f, 1);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float u = (x - N / 2.0f) / N, v = (y - N / 2.0f) / N, d = sqrtf(u * u + v * v);
            Color bg = ColorLerp(a, b, fminf(1, d * 1.6f));
            // a soft-edged disc with a ring around it, inside the safe zone
            float disc = fminf(1, fmaxf(0, (0.17f - d) * N / 2));
            float ring = fminf(1, fmaxf(0, (0.012f - fabsf(d - 0.25f)) * N / 2));
            float k = fmaxf(disc, ring * 0.8f);
            px[y * N + x] = ColorLerp(bg, c, k);
        }
    Image img = { px, N, N, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    bool ok = ExportImage(img, out);
    free(px);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    SetTraceLogLevel(LOG_WARNING);
    if (argc == 4 && strcmp(argv[1], "res") == 0) return res(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "placeholder") == 0) return placeholder(argv[2], argv[3]);
    fprintf(stderr, "usage: icon res <icon.png> <resdir> | icon placeholder <out.png> <seed>\n");
    return 2;
}
