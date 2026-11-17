/*
 * td5mods.c - mod loader for Test Drive 5, built as a 32-bit ASI plugin.
 *
 * The plugin runs once while the game starts and patches it in memory. The exe on
 * disk is never modified, so taking the plugin out restores the stock game.
 * It supports TD5_d3d.exe (DirectX) and VOODOO2.exe (Glide). Tables and code sites
 * are found by their structure, not by fixed addresses, and the plugin does nothing
 * at all if the exe is not one it recognises.
 *
 * What it can do (everything is configured in mods\mods.ini)
 *
 *   1. Extra car slots   [extra_cars]
 *        New cars are appended after the original 37 (the first new car gets ID 37);
 *        nothing is replaced. List one name per line, each needing mods\cars\NAME.zip,
 *        or set  extra_cars_auto = 1  to take every unknown zip found in mods\cars\.
 *        Up to 128 extra cars (165 in total); the method itself tops out at 256 cars.
 *        The new cars appear in the car list after the police cars. In a save where
 *        every original car is already unlocked they are available immediately.
 *
 *   2. Replacing existing slots
 *        [cars]    roster_name = new_name    e.g.  sp1 = jeep
 *        [levels]  old_number  = new_number  e.g.  39  = 7
 *        [options] donors = sp1,sp2,sp3      fill these roster slots, in alphabetical
 *                                            order, with the zips found in mods\cars\
 *                  level_donors = 39,37      the same for mods\levelNNN.zip (unused numbers)
 *
 *   3. File redirection   [options] redirect = 1 (default)
 *        When the game opens cars\x.zip or levelNNN.zip and the same path exists under
 *        mods\, the copy in mods\ is opened instead. Your files can live in the mods
 *        folder and the game folder stays clean.
 *
 * Diagnostics
 *   Every action is written to mods\td5mods.log; please attach it when reporting a
 *   problem. Setting  debug = 1  under [options] additionally logs the zips the game
 *   opens, the car-select cursor and the data parsed for the new cars.
 *
 * Install
 *   - Ultimate ASI Loader (x86), renamed to dsound.dll, in the game folder.
 *     (winmm.dll is often taken by music wrappers, so it is not a good choice.)
 *   - td5mods.asi in the game folder (or in a  scripts  subfolder).
 *   - mods\mods.ini, mods\cars\*.zip and mods\levelNNN.zip as needed.
 *
 * Build (the game is 32-bit, so the plugin has to be as well)
 *   i686-w64-mingw32-gcc -O2 -s -shared -o td5mods.asi td5mods.c
 *
 * Self-test of the portable core (any host with gcc). It applies the patches to an
 * in-memory copy of an exe and writes the result out so it can be inspected:
 *   gcc -DTD5MODS_TEST -Wall -Wextra -o td5test td5mods.c
 *   ./td5test TD5_d3d.exe <game_dir> out.exe
 */
#if defined(_WIN64)
#error Build this as a 32-bit DLL: Test Drive 5 is a 32-bit game.
#endif

#include <string.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define CAR_COUNT          37
#define ENTRY_SIZE         16
#define LEVEL_COUNT        19
#define MAX_LEVEL          39
#define LEVEL_BEFORE_CARS  0x18C   /* level table sits this far before the car pointer array */
#define MAX_SECS           24
#define MAX_STR            64
#define MAX_MODS           160

static u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

/* ------------------------------------------------------------------ types */
typedef struct {
    u8 *mem;                 /* the exe image in memory layout (on Windows: the module itself) */
    u32 base;                /* address that RVA 0 has in the exe's own pointers */
    u32 size;
    int nsec;
    u32 srva[MAX_SECS], svsz[MAX_SECS];
    int sexec[MAX_SECS];
} Image;

typedef struct { u8 *entry; char name[12]; } Car;
typedef struct { u8 *slot; int number; } Level;
typedef struct { Car car[CAR_COUNT]; Level lv[LEVEL_COUNT]; u8 *car_tbl; u8 *lv_tbl; } Tables;

typedef struct {
    int  (*exists)(const char *rel);              /* rel exists in mods\ or in the game folder */
    int  (*poke)(void *dst, const void *src, u32 n);   /* write bytes (handles page protection + icache) */
    int  (*alloc)(u32 size, void **p, u32 *va);        /* zeroed memory; va = its address as the exe sees it */
    int  (*alloc_exec)(u32 size, void **p, u32 *va);   /* same, but executable (for small code stubs) */
    int  (*list_cars)(char (*out)[32], int max);  /* base names of mods\cars\*.zip */
    int  (*list_levels)(int *out, int max);       /* numbers of mods\levelNNN.zip */
    void (*log)(const char *msg);
} Host;

/* --------------------------------------------------------- small helpers */
/* Case-insensitive (ASCII) string equality. */
static int ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}
/* Characters allowed in a car file name: letters, digits and underscore. */
static int is_alnum_(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
/* Small int-to-decimal routine, so the log messages need no printf. */
static void itoa10(int v, char *o) {
    char t[12]; int n = 0, i = 0;
    if (v < 0) { *o++ = '-'; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) o[i++] = t[--n];
    o[i] = 0;
}
/* Joins up to three strings into one log line; over-long lines are cut off. */
static void logm(const Host *h, const char *a, const char *b, const char *c) {
    char buf[300]; int n = 0, i; const char *parts[3]; const char *p;
    parts[0] = a; parts[1] = b; parts[2] = c;
    for (i = 0; i < 3; i++) { p = parts[i]; while (p && *p && n < 290) buf[n++] = *p++; }
    buf[n] = 0;
    h->log(buf);
}
/* Same as logm, for five parts. */
static void logm5(const Host *h, const char *a, const char *b, const char *c, const char *d, const char *e) {
    char buf[400]; int n = 0, i; const char *parts[5]; const char *p;
    parts[0] = a; parts[1] = b; parts[2] = c; parts[3] = d; parts[4] = e;
    for (i = 0; i < 5; i++) { p = parts[i]; while (p && *p && n < 390) buf[n++] = *p++; }
    buf[n] = 0;
    h->log(buf);
}
static void level_rel(int n, char *out) {      /* "level007.zip" */
    strcpy(out, "level");
    out[5] = (char)('0' + (n / 100) % 10); out[6] = (char)('0' + (n / 10) % 10); out[7] = (char)('0' + n % 10);
    strcpy(out + 8, ".zip");
}
static void car_rel(const char *name, char *out) {   /* "cars\jeep.zip" */
    strcpy(out, "cars\\"); strcat(out, name); strcat(out, ".zip");
}

/* ------------------------------------------------------------- PE / image */
/* Reads the PE headers of the image: where each section lives, how large it is and
 * whether it holds code. Returns 0 if the buffer is not a valid PE file. */
static int img_init(Image *im, u8 *mem, u32 base) {
    u32 pe; int i, nsec; u32 optsz, sh;
    if (mem[0] != 'M' || mem[1] != 'Z') return 0;
    pe = rd32(mem + 0x3C);
    if (memcmp(mem + pe, "PE\0\0", 4) != 0) return 0;
    nsec = rd16(mem + pe + 6);
    optsz = rd16(mem + pe + 20);
    im->mem = mem; im->base = base;
    im->size = rd32(mem + pe + 24 + 56);
    if (nsec > MAX_SECS) nsec = MAX_SECS;
    im->nsec = nsec;
    sh = pe + 24 + optsz;
    for (i = 0; i < nsec; i++) {
        u32 vsz = rd32(mem + sh + 40 * i + 8), rva = rd32(mem + sh + 40 * i + 12);
        if (vsz == 0) vsz = rd32(mem + sh + 40 * i + 16);
        if (rva >= im->size) vsz = 0; else if (rva + vsz > im->size) vsz = im->size - rva;
        im->srva[i] = rva; im->svsz[i] = vsz;
        im->sexec[i] = (rd32(mem + sh + 40 * i + 36) & 0x20000000u) != 0;
    }
    return 1;
}

/* Looks up func in the import table entry for dll and returns the address of its
 * IAT slot (the pointer the exe calls through), or 0 if the exe does not import it. */
static u8 *find_iat_slot(const Image *im, const char *dll, const char *func) {
    const u8 *m = im->mem; u32 pe = rd32(m + 0x3C), drva, d;
    drva = rd32(m + pe + 24 + 96 + 8);
    if (!drva) return 0;
    for (d = drva; rd32(m + d + 12) != 0; d += 20) {
        u32 oft = rd32(m + d), ft = rd32(m + d + 16), i;
        if (!ieq((const char *)m + rd32(m + d + 12), dll)) continue;
        for (i = 0;; i++) {
            u32 v = rd32(m + (oft ? oft : ft) + 4 * i);
            if (!v) break;
            if (v & 0x80000000u) continue;
            if (strcmp((const char *)m + v + 2, func) == 0) return im->mem + ft + 4 * i;
        }
    }
    return 0;
}

/* Finds the two tables the loader needs, purely from their structure:
 *   - the car table: a run of exactly 37 pointers, each pointing at a "cars\xxx.zip" string
 *   - the level table: 19 level numbers at a fixed distance in front of it
 * Anything that does not look exactly right returns 0, which makes the plugin do nothing. */
static int find_tables(const Image *im, Tables *tb) {
    struct { u32 va; u8 *p; char name[12]; } st[MAX_STR];
    int nst = 0, s, i, nruns = 0, slot;
    u32 run_rva = 0; int run_len = 0;
    for (s = 0; s < im->nsec; s++) {
        u32 off, vsz = im->svsz[s];
        for (off = 0; off + 16 <= vsz; off++) {
            u8 *p = im->mem + im->srva[s] + off; int n = 5;
            if (p[0] != 'c' || memcmp(p, "cars\\", 5) != 0) continue;
            while (n < 15 && is_alnum_((char)p[n])) n++;
            if (n - 5 < 1 || n - 5 > 10 || memcmp(p + n, ".zip", 5) != 0) continue;
            if (nst < MAX_STR) {
                memcpy(st[nst].name, p + 5, (size_t)(n - 5)); st[nst].name[n - 5] = 0;
                st[nst].p = p; st[nst].va = im->base + im->srva[s] + off; nst++;
            }
        }
    }
    if (nst < CAR_COUNT) return 0;
    for (s = 0; s < im->nsec; s++) {
        u32 off = 0, vsz = im->svsz[s];
        while (off + 4 <= vsz) {
            u32 v = rd32(im->mem + im->srva[s] + off); int hit = 0;
            for (i = 0; i < nst; i++) if (st[i].va == v) { hit = 1; break; }
            if (!hit) { off += 4; continue; }
            { u32 start = off;
              for (;;) {
                  u32 w; int h2 = 0;
                  if (off + 4 > vsz) break;
                  w = rd32(im->mem + im->srva[s] + off);
                  for (i = 0; i < nst; i++) if (st[i].va == w) { h2 = 1; break; }
                  if (!h2) break;
                  off += 4;
              }
              if ((off - start) / 4 >= CAR_COUNT) { nruns++; run_rva = im->srva[s] + start; run_len = (int)((off - start) / 4); }
            }
        }
    }
    if (nruns != 1 || run_len != CAR_COUNT) return 0;
    tb->car_tbl = im->mem + run_rva;
    for (slot = 0; slot < CAR_COUNT; slot++) {
        u32 v = rd32(tb->car_tbl + 4 * slot); int found = 0;
        for (i = 0; i < nst; i++) if (st[i].va == v) { tb->car[slot].entry = st[i].p; strcpy(tb->car[slot].name, st[i].name); found = 1; break; }
        if (!found) return 0;
    }
    for (slot = 0; slot < CAR_COUNT; slot++) for (i = slot + 1; i < CAR_COUNT; i++) if (ieq(tb->car[slot].name, tb->car[i].name)) return 0;
    if (run_rva < LEVEL_BEFORE_CARS) return 0;
    tb->lv_tbl = im->mem + run_rva - LEVEL_BEFORE_CARS;
    for (slot = 0; slot < LEVEL_COUNT; slot++) {
        int n = (int)rd32(tb->lv_tbl + 4 * slot);
        if (n < 0 || n > 99) return 0;
        tb->lv[slot].slot = tb->lv_tbl + 4 * slot; tb->lv[slot].number = n;
    }
    for (slot = 0; slot < LEVEL_COUNT; slot++) for (i = slot + 1; i < LEVEL_COUNT; i++) if (tb->lv[slot].number == tb->lv[i].number) return 0;
    return 1;
}

/* -------------------------------------------------------------------- INI */
typedef struct { char key[40]; char val[96]; } KV;

/* Collects the key/value lines of one [section]. Lines starting with ; or # are
 * comments. A line without '=' is kept as a key with an empty value, which is how
 * the plain name list of [extra_cars] is read. Returns the number of entries. */
static int ini_section(const char *t, const char *want, KV *out, int max) {
    int n = 0, in = 0;
    while (*t) {
        char line[160]; int L = 0; char *a, *eq, *e;
        while (*t && *t != '\n' && *t != '\r') { if (L < 158) line[L++] = *t; t++; }
        while (*t == '\n' || *t == '\r') t++;
        line[L] = 0; a = line;
        while (*a == ' ' || *a == '\t') a++;
        if (*a == ';' || *a == '#' || !*a) continue;
        if (*a == '[') {
            char *r = strchr(a, ']'); in = 0;
            if (r) { *r = 0; in = ieq(a + 1, want); }
            continue;
        }
        if (!in) continue;
        eq = strchr(a, '=');
        if (n >= max) continue;
        if (!eq) {                                     /* key-only line */
            e = a + strlen(a) - 1; while (e >= a && (*e == ' ' || *e == '\t')) { *e = 0; e--; }
            if (strlen(a) < sizeof(out[n].key)) { strcpy(out[n].key, a); out[n].val[0] = 0; n++; }
            continue;
        }
        *eq = 0;
        e = eq - 1; while (e >= a && (*e == ' ' || *e == '\t')) { *e = 0; e--; }
        eq++; while (*eq == ' ' || *eq == '\t') eq++;
        e = eq + strlen(eq) - 1; while (e >= eq && (*e == ' ' || *e == '\t')) { *e = 0; e--; }
        if (strlen(a) < sizeof(out[n].key) && strlen(eq) < sizeof(out[n].val)) {
            strcpy(out[n].key, a); strcpy(out[n].val, eq); n++;
        }
    }
    return n;
}
/* Looks a key up (case-insensitive); returns its value or 0 if it is absent. */
static const char *ini_get(const KV *kv, int n, const char *key) {
    int i; for (i = 0; i < n; i++) if (ieq(kv[i].key, key)) return kv[i].val;
    return 0;
}
/* Splits a list such as "a, b c" on commas and blanks. */
static int split_list(const char *s, char (*out)[40], int max) {
    int n = 0;
    while (*s && n < max) {
        int L = 0;
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        while (*s && *s != ',' && *s != ' ' && *s != '\t') { if (L < 38) out[n][L++] = *s; s++; }
        if (L) { out[n][L] = 0; n++; }
    }
    return n;
}
/* Strict digit helpers: bad input in the INI is rejected, never silently read as 0. */
static int atoi_(const char *s) { int v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; } return v; }
static int digits3(const char *p) { return p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' && p[2] >= '0' && p[2] <= '9'; }
static int all_digits(const char *s) { if (!*s) return 0; while (*s) { if (*s < '0' || *s > '9') return 0; s++; } return 1; }

/* ---------------------------------------------------------------- patches */
static int name_in_roster(const Tables *tb, const char *nm) {
    int i; for (i = 0; i < CAR_COUNT; i++) if (ieq(tb->car[i].name, nm)) return 1;
    return 0;
}
static int level_in_use(const Tables *tb, int n) {
    int i; for (i = 0; i < LEVEL_COUNT; i++) if (tb->lv[i].number == n) return 1;
    return 0;
}

/* Re-points one roster slot at a different zip by overwriting its 16-byte path string
 * ("cars\xxx.zip"). The name and the file are validated first. Returns 1 on success. */
static int apply_car(Tables *tb, const Host *h, const char *oldn, const char *newn) {
    int slot = -1, i, L = (int)strlen(newn); char rel[64]; u8 buf[ENTRY_SIZE];
    for (i = 0; i < CAR_COUNT; i++) if (ieq(tb->car[i].name, oldn)) slot = i;
    if (slot < 0) { logm(h, "car: no roster entry named '", oldn, "' - skipped"); return 0; }
    if (L < 1 || L > 6) { logm(h, "car: '", newn, "' must be 1-6 characters - skipped"); return 0; }
    for (i = 0; i < L; i++) if (!is_alnum_(newn[i])) { logm(h, "car: bad character in '", newn, "' - skipped"); return 0; }
    if (name_in_roster(tb, newn) && !ieq(oldn, newn)) { logm(h, "car: '", newn, "' is already a roster entry - skipped"); return 0; }
    car_rel(newn, rel);
    if (!h->exists(rel)) { logm(h, "car: file not found (mods\\ or game folder): ", rel, " - skipped"); return 0; }
    memset(buf, 0, sizeof buf);
    memcpy(buf, "cars\\", 5); memcpy(buf + 5, newn, (size_t)L); memcpy(buf + 5 + L, ".zip", 4);
    if (!h->poke(tb->car[slot].entry, buf, ENTRY_SIZE)) { h->log("car: could not write memory - skipped"); return 0; }
    logm5(h, "car: cars\\", oldn, ".zip  ->  ", rel, "");
    strcpy(tb->car[slot].name, newn);
    return 1;
}

/* Changes which level file a track loads by rewriting its entry in the level table.
 * The new number must be unused and its levelNNN.zip must exist. Returns 1 on success. */
static int apply_level(Tables *tb, const Host *h, int oldn, int newn) {
    int slot = -1, i; char rel[16], a[12], b[12];
    itoa10(oldn, a); itoa10(newn, b);
    for (i = 0; i < LEVEL_COUNT; i++) if (tb->lv[i].number == oldn) slot = i;
    if (slot < 0) { logm(h, "level: no track loads level number ", a, " - skipped"); return 0; }
    if (newn < 0 || newn > MAX_LEVEL) { logm(h, "level: new number ", b, " must be 0-39 - skipped"); return 0; }
    if (level_in_use(tb, newn) && newn != oldn) { logm(h, "level: number ", b, " is already used by a track - skipped"); return 0; }
    level_rel(newn, rel);
    if (!h->exists(rel)) { logm(h, "level: file not found (mods\\ or game folder): ", rel, " - skipped"); return 0; }
    { u8 nb[4]; wr32(nb, (u32)newn);
      if (!h->poke(tb->lv[slot].slot, nb, 4)) { h->log("level: could not write memory - skipped"); return 0; } }
    tb->lv[slot].number = newn;
    logm5(h, "level: ", a, "  ->  ", rel, "");
    return 1;
}

/* Case-insensitive insertion sort; gives donors and automatic extra cars a stable,
 * alphabetical order. */
static void sort_names(char (*v)[32], int n) {
    int i, j; char t[32];
    for (i = 1; i < n; i++) for (j = i; j > 0; j--) {
        const char *x = v[j - 1], *y = v[j]; int c = 0;
        for (;; x++, y++) { char p = *x, q = *y; if (p >= 'A' && p <= 'Z') p = (char)(p + 32); if (q >= 'A' && q <= 'Z') q = (char)(q + 32); if (p != q) { c = p > q; break; } if (!p) break; }
        if (!c) break;
        strcpy(t, v[j - 1]); strcpy(v[j - 1], v[j]); strcpy(v[j], t);
    }
}


/* ============================================================ extra car slots
 *
 * The game keeps its car data in fixed-size tables and loops over them with
 * hard-coded limits (37 cars, IDs 0-36). To make room for more cars the plugin:
 *   1. copies these tables into newly allocated memory that has space for the new cars
 *        - pointer array   car slot -> "cars\xxx.zip" path string
 *        - ID table        car ID -> slot byte (this is the order of the car list)
 *        - config records  17 text lines x 48 bytes per car (0x330 bytes)
 *        - locked flags    one byte per car, 1 = locked (40 bytes in the original)
 *   2. re-points every code reference at the new copies;
 *   3. raises the hard-coded limits: the end of the zip loader loop, the wrap-around
 *      constants of the car list (36 -> N-1 and 37 -> N) and the car count;
 *   4. appends the new path strings and IDs.
 *
 * About the car count: the game stores the number of selectable cars in a variable
 * that is saved in Config.td5, and the car list only shows IDs below it. The unlock-all
 * routine is changed to set it to N, and the save loader is redirected through a small
 * stub that raises it to N whenever the save has all 37 original cars unlocked. Saves
 * with fewer cars unlocked are left alone.
 *
 * Safety: every site is located by byte pattern and counted before anything is
 * written. If a single pattern or count is not what we expect, nothing is changed.
 * If a write fails half-way, the writes already made are undone.
 */
#define MAX_SITES 256     /* patch sites collected in one run */
#define MAX_REFS  96      /* references found for a single table address */
#define MAX_EXTRA 128     /* most extra cars supported */
#define REC_SIZE  0x330   /* bytes of config text per car */
/* Kinds of patch site:
 *   K_PA   pointer array address        K_END  its end address (loader loop compare)
 *   K_ID   ID table address             K_REC  config record addresses
 *   K_DEC / K_INC  wrap-around constants of the car list (N-1 and N)
 *   K_IDOP movsx -> movzx opcode, needed only above 127 cars
 *   K_CNT  car count in the unlock-all routine
 *   K_FLG  locked-flag table addresses */
enum { K_PA, K_END, K_ID, K_REC, K_DEC, K_INC, K_IDOP, K_CNT, K_FLG, K_KINDS };
typedef struct { u8 *p; int kind; int len; u32 old; } Site;
typedef struct { Site s[MAX_SITES]; int n; int cnt[K_KINDS]; u32 PA, IB, RB, CUR, CNT, FB; u8 *pa_p, *ib_p, *flg_p, *ld_p; } Plan;

/* Records one site (address, kind, patch length, original value). The per-kind
 * counters let build_plan verify the numbers before anything is written. */
static void add_site(Plan *pl, u8 *p, int kind, int len, u32 old) {
    if (pl->n >= MAX_SITES) return;
    pl->s[pl->n].p = p; pl->s[pl->n].kind = kind; pl->s[pl->n].len = len; pl->s[pl->n].old = old;
    pl->n++; pl->cnt[kind]++;
}
/* Returns the address of every 4-byte occurrence of value inside executable
 * sections - that is, the code that refers to a table by its address. */
static int find_refs(const Image *im, u32 value, u8 **out, int max) {
    int s, n = 0; u8 pat[4];
    wr32(pat, value);
    for (s = 0; s < im->nsec; s++) {
        u32 i, vsz = im->svsz[s]; u8 *b = im->mem + im->srva[s];
        if (!im->sexec[s]) continue;
        for (i = 0; i + 4 <= vsz; i++)
            if (b[i] == pat[0] && b[i + 1] == pat[1] && b[i + 2] == pat[2] && b[i + 3] == pat[3] && n < max) out[n++] = b + i;
    }
    return n;
}
/* Counts data dwords equal to lo + n*step inside [lo, hi). Used to make sure no data
 * structure points into a table that is about to be moved. */
static int data_has_ptr(const Image *im, u32 lo, u32 hi, u32 step) {
    int s, n = 0;
    for (s = 0; s < im->nsec; s++) {
        u32 i, vsz = im->svsz[s]; u8 *b = im->mem + im->srva[s];
        if (im->sexec[s]) continue;
        for (i = 0; i + 4 <= vsz; i += 4) { u32 v = rd32(b + i); if (v >= lo && v < hi && (v - lo) % step == 0) n++; }
    }
    return n;
}
/* The form_* checks look at the bytes in front of a 4-byte address and accept it only
 * if it is the operand of an instruction we know how to patch, so a coincidental
 * byte sequence is never modified. */
static int form_pa(const u8 *p) {
    if (p[-3] == 0x8B && (p[-2] & 0xC7) == 0x04 && (p[-1] >> 6) == 2 && (p[-1] & 7) == 5) return 1;  /* mov r,[idx*4+imm] */
    if (p[-4] == 0xC7 && p[-3] == 0x44 && p[-2] == 0x24) return 1;                                  /* mov [esp+d],imm   */
    if (p[-1] >= 0xB8 && p[-1] <= 0xBF) return 1;                                                   /* mov r,imm         */
    return 0;
}
static int form_id(const u8 *p) { return p[-3] == 0x0F && p[-2] == 0xBE && (p[-1] & 0xC0) == 0x80; } /* movsx r,byte[r+imm] */
static int form_rec(const u8 *p) {
    if (p[-1] == 0x05) return 1;                                      /* add eax,imm   */
    if (p[-2] == 0x81 && (p[-1] & 0xF8) == 0xC0) return 1;            /* add r,imm     */
    if (p[-1] >= 0xB8 && p[-1] <= 0xBF) return 1;                     /* mov r,imm     */
    if (p[-2] == 0x8D && (p[-1] & 0xC0) == 0x80) return 1;            /* lea r,[r+imm] */
    return 0;
}
/* Finds the address of the car-list cursor (the currently highlighted car ID). Two
 * handlers - the full list and the police block - each contain  "jge ..; mov [X],36",
 * so an address that shows up in two such sites is the cursor. Requiring two matches
 * keeps this independent of instruction order. Returns 0 if the result is ambiguous. */
static u32 find_cursor(const Image *im) {
    u32 cand[16]; int cnt[16], nc = 0, i, s, best = -1, ties = 0;
    for (s = 0; s < im->nsec; s++) {
        u32 o, vsz = im->svsz[s]; u8 *b = im->mem + im->srva[s];
        if (!im->sexec[s]) continue;
        for (o = 0; o + 12 <= vsz; o++) {
            u8 *c = b + o;
            if (c[0] == 0x7D && c[2] == 0xC7 && c[3] == 0x05 && rd32(c + 8) == 0x24) {
                u32 v = rd32(c + 4); int q, hit = -1;
                if (v < im->base || v >= im->base + im->size) continue;
                for (q = 0; q < nc; q++) if (cand[q] == v) hit = q;
                if (hit < 0 && nc < 16) { cand[nc] = v; cnt[nc] = 0; hit = nc++; }
                if (hit >= 0) cnt[hit]++;
            }
        }
    }
    for (i = 0; i < nc; i++) if (cnt[i] >= 2) { if (best < 0 || cnt[i] > cnt[best]) { best = i; ties = 0; } else if (cnt[i] == cnt[best]) ties++; }
    return (best >= 0 && !ties) ? cand[best] : 0;
}

/* Locates everything that has to change, without changing anything yet:
 *   1. every reference to the pointer array and to its end address (the loader loop)
 *   2. the ID table (a permutation of 0..36) and its references
 *   3. the car cursor, and the 0x330-byte config record array with its references
 *   4. the wrap-around constants of the car list
 *   5. the car-count variable (unlock-all routine and save loader) and the locked-flag table
 * Returns 0 and logs the reason as soon as something does not match. */
static int build_plan(const Image *im, const Tables *tb, const Host *h, Plan *pl) {
    u8 *refs[MAX_REFS], *endsite = 0, *ibcand[8];
    int nr, i, k, good, nib = 0, s, best = -1, bestn = -1;
    memset(pl, 0, sizeof *pl);
    pl->pa_p = tb->car_tbl; pl->PA = im->base + (u32)(tb->car_tbl - im->mem);

    nr = find_refs(im, pl->PA, refs, MAX_REFS);
    for (good = 0, i = 0; i < nr; i++) if (form_pa(refs[i])) { add_site(pl, refs[i], K_PA, 4, pl->PA); good++; }
    if (nr != good || good < 5) { h->log("extra: car pointer array references not as expected - nothing changed"); return 0; }

    nr = find_refs(im, pl->PA + 4 * CAR_COUNT, refs, MAX_REFS);
    for (good = 0, i = 0; i < nr; i++) if (refs[i][-1] == 0x3D) { add_site(pl, refs[i], K_END, 4, pl->PA + 4 * CAR_COUNT); endsite = refs[i] - 1; good++; }
    if (good != 1) { h->log("extra: car loop end compare not found exactly once - nothing changed"); return 0; }

    for (s = 0; s < im->nsec; s++) {             /* ID->slot table: 37 bytes that are a permutation of 0..36 */
        u32 o, vsz = im->svsz[s]; u8 *b = im->mem + im->srva[s];
        if (im->sexec[s]) continue;
        for (o = 0; o + CAR_COUNT <= vsz; o++) {
            u8 seen[CAR_COUNT]; int q, ok = 1;
            if (b[o] >= CAR_COUNT) continue;
            memset(seen, 0, sizeof seen);
            for (q = 0; q < CAR_COUNT; q++) { u8 v = b[o + q]; if (v >= CAR_COUNT || seen[v]) { ok = 0; break; } seen[v] = 1; }
            if (ok && nib < 8) ibcand[nib++] = b + o;
        }
    }
    for (k = 0; k < nib; k++) {
        u32 va = im->base + (u32)(ibcand[k] - im->mem); int g = 0;
        nr = find_refs(im, va, refs, MAX_REFS);
        for (i = 0; i < nr; i++) if (form_id(refs[i])) g++;
        if (g > bestn) { bestn = g; best = k; }
    }
    if (best < 0 || bestn < 10) { h->log("extra: ID->slot table not found - nothing changed"); return 0; }
    pl->ib_p = ibcand[best]; pl->IB = im->base + (u32)(pl->ib_p - im->mem);
    nr = find_refs(im, pl->IB, refs, MAX_REFS);
    for (good = 0, i = 0; i < nr; i++) if (form_id(refs[i])) { add_site(pl, refs[i], K_ID, 4, pl->IB); add_site(pl, refs[i] - 2, K_IDOP, 1, 0xBE); good++; }
    if (nr != good) { h->log("extra: unrecognised reference to the ID table - nothing changed"); return 0; }

    pl->CUR = find_cursor(im);
    if (!pl->CUR) { h->log("extra: car cursor not found - nothing changed"); return 0; }

    { /* record array: the loader adds 0x330 to a record pointer just before the end compare */
      int reg = -1; u8 *q; u32 init = 0;
      for (q = endsite - 6; q >= endsite - 24; q--) if (q[0] == 0x81 && (q[1] & 0xF8) == 0xC0 && rd32(q + 2) == REC_SIZE) { reg = q[1] & 7; break; }
      if (reg < 0) { h->log("extra: record stride not found - nothing changed"); return 0; }
      for (q = endsite; q > endsite - 0x800; q--) if (q[0] == 0xB8 + reg) {
          u32 v = rd32(q + 1);
          if (v >= im->base && v < im->base + im->size && v > pl->PA) { init = v; break; }
      }
      if (!init) { h->log("extra: record array start not found - nothing changed"); return 0; }
      pl->RB = init - 0x270;
    }
    for (k = 0; k <= REC_SIZE / 0x30; k++) {
        u32 v = pl->RB + (u32)k * 0x30;
        nr = find_refs(im, v, refs, MAX_REFS);
        for (i = 0; i < nr; i++) {
            if (!form_rec(refs[i])) { h->log("extra: unrecognised reference to the record array - nothing changed"); return 0; }
            add_site(pl, refs[i], K_REC, 4, v);
        }
    }
    if (pl->cnt[K_REC] < 8) { h->log("extra: too few record references - nothing changed"); return 0; }
    if (data_has_ptr(im, pl->RB, pl->RB + CAR_COUNT * REC_SIZE, 0x30) || data_has_ptr(im, pl->PA, pl->PA + 4 * CAR_COUNT, 4) || data_has_ptr(im, pl->IB, pl->IB + CAR_COUNT, 1)) {
        h->log("extra: data pointing into the car tables - nothing changed"); return 0;
    }
    for (s = 0; s < im->nsec; s++) {             /* the wrap constants of the full list */
        u32 o, vsz = im->svsz[s]; u8 *b = im->mem + im->srva[s];
        if (!im->sexec[s]) continue;
        for (o = 0; o + 16 <= vsz; o++) {
            u8 *c = b + o; u8 cp[4]; int q;
            wr32(cp, pl->CUR);
            if (c[0] == 0x83 && c[1] == 0xF8 && c[2] == 0x25 && c[3] == 0x75) {                         /* cmp eax,37 ; jne ; ...store 0 */
                for (q = 5; q + 4 <= 13; q++) if (memcmp(c + q, cp, 4) == 0) { add_site(pl, c + 2, K_INC, 1, 0x25); break; }
            } else if (c[0] == 0x3B && c[1] == 0xC3 && c[2] == 0x7D && c[4] == 0xB8 && rd32(c + 5) == 0x24) {  /* cmp eax,ebx ; jge ; mov eax,36 */
                for (q = 9; q + 4 <= 9 + 14; q++) if (memcmp(c + q, cp, 4) == 0) { add_site(pl, c + 5, K_DEC, 4, 0x24); break; }
            } else if (c[0] == 0x3B && c[1] == 0xC3 && c[2] == 0x7D && c[4] == 0xC7 && c[5] == 0x05 && memcmp(c + 6, cp, 4) == 0 && rd32(c + 10) == 0x24) {
                add_site(pl, c + 10, K_DEC, 4, 0x24);                                                    /* ... mov [cursor],36 */
            }
        }
    }
    if (pl->cnt[K_INC] != 2 || pl->cnt[K_DEC] != 2) { h->log("extra: list wrap constants not found as expected - nothing changed"); return 0; }

    {   /* (1) the car-count variable: the game only lists cars with ID < count, and the save loader overwrites it.
         *     unlock-all:  mov eax,37 ; (cmp ecx,eax ; mov [tracks],19 | mov [tracks],19 ; cmp ecx,eax) ; jge .. ; mov [COUNT],eax
         *     save loader: ... mov [COUNT],ecx   (the only 'mov [COUNT],ecx')                                     */
        int s2, kk, ii, nn, nua = 0, nld = 0; u8 *ua = 0, *ldc = 0;
        for (s2 = 0; s2 < im->nsec; s2++) {
            u32 o, vsz = im->svsz[s2]; u8 *bb = im->mem + im->srva[s2];
            if (!im->sexec[s2]) continue;
            for (o = 0; o + 24 <= vsz; o++) {
                u8 *c = bb + o;
                if (c[0] == 0xB8 && rd32(c + 1) == CAR_COUNT && c[17] == 0x7D && c[18] == 0x05 && c[19] == 0xA3 &&
                    ((c[5] == 0x3B && c[6] == 0xC8 && c[7] == 0xC7 && c[8] == 0x05 && rd32(c + 13) == 0x13) ||      /* DirectX build: cmp first   */
                     (c[5] == 0xC7 && c[6] == 0x05 && rd32(c + 11) == 0x13 && c[15] == 0x3B && c[16] == 0xC8)))     /* Voodoo2 build: store first */
                    { ua = c; nua++; }
            }
        }
        if (nua != 1) { h->log("extra: unlock-all routine not found as expected - nothing changed"); return 0; }
        pl->CNT = rd32(ua + 20);
        nn = find_refs(im, pl->CNT, refs, MAX_REFS);
        for (ii = 0; ii < nn; ii++) if (refs[ii][-2] == 0x89 && refs[ii][-1] == 0x0D) { ldc = refs[ii] - 2; nld++; }
        if (nld != 1) { h->log("extra: save loader of the car count not found as expected - nothing changed"); return 0; }
        add_site(pl, ua + 1, K_CNT, 4, CAR_COUNT); pl->ld_p = ldc;
        /* (2) the per-car 'locked' flag table: 40 bytes (37 cars + 3 padding) right behind the ID table.
         *     Code indexes it by car ID, so IDs >= 40 would read the next table. It is moved and every reference follows. */
        pl->FB = pl->IB + 0x28; pl->flg_p = pl->ib_p + 0x28;
        { int ones = 0; for (kk = 0; kk < 40; kk++) { if (pl->flg_p[kk] > 1) { ones = -1000; break; } ones += pl->flg_p[kk]; }
          if (ones < 10) { h->log("extra: locked-flag table not as expected - nothing changed"); return 0; } }
        for (kk = 0; kk < 40; kk++) {
            nn = find_refs(im, pl->FB + (u32)kk, refs, MAX_REFS);
            for (ii = 0; ii < nn; ii++) {
                u8 pb = refs[ii][-1];
                if (!(pb >= 0xA0 && pb <= 0xA3) && (pb & 0xC7) != 0x05 && (pb & 0xC0) != 0x80 && !(pb >= 0xB8 && pb <= 0xBF)) {
                    h->log("extra: unrecognised reference to the locked-flag table - nothing changed"); return 0; }
                add_site(pl, refs[ii], K_FLG, 4, pl->FB + (u32)kk);
            }
        }
        if (pl->cnt[K_FLG] < 4 || data_has_ptr(im, pl->FB, pl->FB + 40, 1)) { h->log("extra: locked-flag table references not as expected - nothing changed"); return 0; }
    }
    return 1;
}

static u32 g_dbg_cur, g_dbg_rec, g_dbg_n;     /* for the debug thread: cursor address, new record array, total cars */
static u32 al16(u32 v) { return (v + 15u) & ~15u; }

/* Allocates the new tables, fills them and patches the sites found by build_plan.
 * Layout of the new block:  pointer array | ID table | new path strings | config records | locked flags
 * The code stub memory (128 bytes) holds the small trampolines: 16 bytes each for the
 * list wrap when there are more than 127 cars, and the 23-byte save-loader stub at offset 48.
 * On any failure every write already made is undone. */
static int apply_extra(Image *im, Plan *pl, const Host *h, char (*names)[32], int E) {
    int N = CAR_COUNT + E, i, j, na = 0, ninc = 0, ntr = 0, wide = (N > 127);
    u32 off_ib, off_str, off_rec, off_flg, total, va = 0, cva = 0; void *blk = 0, *cave = 0; u8 *b, *cv = 0; int ldp = 0; u8 ldold[6];
    int appl[MAX_SITES]; u8 undo[MAX_SITES][4]; u8 *incp[4]; u8 incold[4][5];
    off_ib = al16((u32)N * 4); off_str = off_ib + al16((u32)N); off_rec = off_str + (u32)E * ENTRY_SIZE; off_flg = off_rec + (u32)N * REC_SIZE; total = off_flg + al16((u32)(N > 40 ? N : 40)) + 16;
    if (!h->alloc(total, &blk, &va)) { h->log("extra: could not allocate memory - nothing changed"); return 0; }
    if (va + total < va || (va + total) >= 0x80000000u) {      /* the loop-end compare is signed */
        h->log("extra: allocated memory is above 2 GB, which would break a signed compare - nothing changed"); return 0; }
    if (!h->alloc_exec(128, &cave, &cva) || cva + 128 >= 0x80000000u) { h->log("extra: could not allocate code stub memory - nothing changed"); return 0; }
    cv = (u8 *)cave;
    if (wide) h->log("extra: more than 127 cars - using unsigned slot reads and a code stub for the list wrap");
    b = (u8 *)blk; memset(b, 0, total);
    for (i = 0; i < CAR_COUNT; i++) wr32(b + 4 * i, rd32(pl->pa_p + 4 * i));
    memcpy(b + off_ib, pl->ib_p, CAR_COUNT);
    memcpy(b + off_flg, pl->flg_p, 40);                        /* locked flags: original 40 bytes, new IDs start unlocked (0) */
    for (i = 0; i < E; i++) {
        u8 *e = b + off_str + (u32)i * ENTRY_SIZE; size_t L = strlen(names[i]);
        memcpy(e, "cars\\", 5); memcpy(e + 5, names[i], L); memcpy(e + 5 + L, ".zip", 4);
        wr32(b + 4 * (CAR_COUNT + i), va + off_str + (u32)i * ENTRY_SIZE);
        b[off_ib + CAR_COUNT + i] = (u8)(CAR_COUNT + i);
    }
    for (i = 0; i < pl->n; i++) {
        const Site *st = &pl->s[i]; u8 nb[4]; u32 nv = 0;
        if (st->kind == K_IDOP && !wide) continue;
        if (st->kind == K_INC && wide) { if (ninc < 4) incp[ninc++] = st->p - 2; continue; }
        switch (st->kind) {
            case K_PA:   nv = va; break;
            case K_END:  nv = va + 4u * (u32)N; break;
            case K_ID:   nv = va + off_ib; break;
            case K_REC:  nv = va + off_rec + (st->old - pl->RB); break;
            case K_DEC:  nv = (u32)(N - 1); break;
            case K_INC:  nv = (u32)N; break;
            case K_IDOP: nv = 0xB6; break;                     /* movsx -> movzx (same length) */
            case K_CNT:  nv = (u32)N; break;                   /* unlock-all now unlocks every car */
            case K_FLG:  nv = va + off_flg + (st->old - pl->FB); break;
        }
        wr32(nb, nv); memcpy(undo[na], st->p, 4); appl[na] = i;
        if (!h->poke(st->p, nb, (u32)st->len)) goto rollback;
        na++;
    }
    for (j = 0; j < ninc; j++) {                               /* wide list wrap: cmp eax,imm8 ; jne  ->  jmp stub */
        u8 *c = incp[j]; u32 sva = im->base + (u32)(c - im->mem), target, back = sva + 5, qva = cva + 16u * (u32)j; u8 *q = cv + 16 * j, jmp[5];
        if (c[0] != 0x83 || c[1] != 0xF8 || c[3] != 0x75) goto rollback;
        target = sva + 5 + (u32)(int)(signed char)c[4];
        q[0] = 0x3D; wr32(q + 1, (u32)N);                      /* cmp eax,N        */
        q[5] = 0x0F; q[6] = 0x85; wr32(q + 7, target - (qva + 11));   /* jne original-target */
        q[11] = 0xE9; wr32(q + 12, back - (qva + 16));         /* jmp back         */
        jmp[0] = 0xE9; wr32(jmp + 1, qva - (sva + 5));
        memcpy(incold[j], c, 5);
        if (!h->poke(c, jmp, 5)) goto rollback;
        ntr++;
    }
    {   /* save loader: after the game reads the count from Config.td5, a save with all 37 original cars unlocked gets count = N.
         *   pushfd ; cmp ecx,37 ; jl L ; mov ecx,N ; L: popfd ; mov [COUNT],ecx ; jmp back      (the original 6-byte store is replaced) */
        u8 *c = pl->ld_p, *q = cv + 48; u32 sva = im->base + (u32)(c - im->mem), qva = cva + 48; u8 jmp[6];
        if (c[0] != 0x89 || c[1] != 0x0D) goto rollback;
        q[0] = 0x9C; q[1] = 0x83; q[2] = 0xF9; q[3] = CAR_COUNT; q[4] = 0x7C; q[5] = 0x05; q[6] = 0xB9; wr32(q + 7, (u32)N);
        q[11] = 0x9D; q[12] = 0x89; q[13] = 0x0D; wr32(q + 14, pl->CNT); q[18] = 0xE9; wr32(q + 19, (sva + 6) - (qva + 23));
        jmp[0] = 0xE9; wr32(jmp + 1, qva - (sva + 5)); jmp[5] = 0x90;
        memcpy(ldold, c, 6);
        if (!h->poke(c, jmp, 6)) goto rollback;
        ldp = 1;
    }
    g_dbg_cur = pl->CUR; g_dbg_rec = va + off_rec; g_dbg_n = (u32)N;
    return 1;
rollback:
    h->log("extra: a write failed - rolling back");
    if (ldp) h->poke(pl->ld_p, ldold, 6);
    for (j = ntr - 1; j >= 0; j--) h->poke(incp[j], incold[j], 5);
    for (i = na - 1; i >= 0; i--) h->poke(pl->s[appl[i]].p, undo[i], (u32)pl->s[appl[i]].len);
    return 0;
}

/* Reads [extra_cars] (and extra_cars_auto), drops invalid, duplicate and missing
 * names, and applies the patch. Returns the number of cars added, or 0. */
static int extra_cars(Image *im, Tables *tb, const Host *h, const char *ini, const KV *opt, int no) {
    static KV ex[MAX_EXTRA + 16]; int ne = ini_section(ini, "extra_cars", ex, MAX_EXTRA + 16), i, E = 0, j;
    char names[MAX_EXTRA][32]; const char *au = ini_get(opt, no, "extra_cars_auto"); Plan pl; char rel[64], nb[12];
    for (i = 0; i < ne && E < MAX_EXTRA; i++) {
        const char *nm = ex[i].key; int L = (int)strlen(nm), bad = (L < 1 || L > 6);
        for (j = 0; j < L && !bad; j++) if (!is_alnum_(nm[j])) bad = 1;
        if (bad) { logm(h, "extra car: bad name '", nm, "' (1-6 letters/digits) - skipped"); continue; }
        strcpy(names[E++], nm);
    }
    if (au && au[0] == '1') {
        char mods[MAX_MODS][32]; int nm = h->list_cars(mods, MAX_MODS);
        sort_names(mods, nm);
        for (i = 0; i < nm && E < MAX_EXTRA; i++) {
            int L = (int)strlen(mods[i]), bad = (L < 1 || L > 6);
            for (j = 0; j < L && !bad; j++) if (!is_alnum_(mods[i][j])) bad = 1;
            if (bad || name_in_roster(tb, mods[i])) continue;
            { int d = 0; for (j = 0; j < E; j++) if (ieq(names[j], mods[i])) d = 1; if (d) continue; }
            strcpy(names[E++], mods[i]);
        }
    }
    { int w = 0;                                   /* validate, drop duplicates / missing files */
      for (i = 0; i < E; i++) {
          int dup = 0;
          for (j = 0; j < w; j++) if (ieq(names[j], names[i])) dup = 1;
          car_rel(names[i], rel);
          if (dup || name_in_roster(tb, names[i])) { logm(h, "extra car: '", names[i], "' already exists - skipped"); continue; }
          if (!h->exists(rel)) { logm(h, "extra car: file not found: ", rel, " - skipped"); continue; }
          strcpy(names[w++], names[i]);
      }
      E = w; }
    if (E == 0) return 0;
    if (!build_plan(im, tb, h, &pl)) return 0;
    itoa10(E, nb);
    { char c[5][12], m[160]; itoa10(pl.cnt[K_PA], c[0]); itoa10(pl.cnt[K_END], c[1]); itoa10(pl.cnt[K_ID], c[2]); itoa10(pl.cnt[K_REC], c[3]); itoa10(pl.cnt[K_DEC] + pl.cnt[K_INC], c[4]);
      strcpy(m, "extra: adding "); strcat(m, nb); strcat(m, " car slot(s); patch sites: ptr="); strcat(m, c[0]); strcat(m, " end="); strcat(m, c[1]);
      strcat(m, " id="); strcat(m, c[2]); strcat(m, " rec="); strcat(m, c[3]); strcat(m, " wrap="); strcat(m, c[4]); h->log(m); }
    if (!apply_extra(im, &pl, h, names, E)) return 0;
    { char t[12]; itoa10(CAR_COUNT + E, t); logm(h, "extra: car count is raised to ", t, " for saves with every original car unlocked; locked-flag table moved"); }
    for (i = 0; i < E; i++) { char id[12]; itoa10(CAR_COUNT + i, id); logm5(h, "extra car: ID ", id, " = cars\\", names[i], ".zip"); }
    return E;
}

/* Entry point of the portable core: applies [cars], [levels], the donor lists and
 * [extra_cars] to the image. Returns the number of changes made, or -1 if the exe's
 * tables were not found (meaning: not a supported exe, nothing was touched). */
static int td5mods_run(Image *im, const Host *h, const char *ini, Tables *tb) {
    KV cars[48], lvls[48], opt[16]; int nc, nl, no, i, changed = 0;
    if (!find_tables(im, tb)) { h->log("tables not found - this exe does not match, doing nothing"); return -1; }
    h->log("tables found: 37 cars, 19 tracks");
    nc = ini_section(ini, "cars", cars, 48);
    nl = ini_section(ini, "levels", lvls, 48);
    no = ini_section(ini, "options", opt, 16);
    for (i = 0; i < nc; i++) changed += apply_car(tb, h, cars[i].key, cars[i].val);
    for (i = 0; i < nl; i++) {
        if (!all_digits(lvls[i].key) || !all_digits(lvls[i].val)) { logm(h, "level: wants numbers, got ", lvls[i].key, " - skipped"); continue; }
        changed += apply_level(tb, h, atoi_(lvls[i].key), atoi_(lvls[i].val));
    }
    { const char *d = ini_get(opt, no, "donors");
      if (d) {
          char dn[16][40]; char mods[MAX_MODS][32]; int nd = split_list(d, dn, 16), nm = h->list_cars(mods, MAX_MODS), k = 0, j;
          sort_names(mods, nm);
          for (j = 0; j < nd && k < nm; j++) {
              if (!name_in_roster(tb, dn[j])) { logm(h, "donor '", dn[j], "' not in roster - skipped"); continue; }
              while (k < nm && name_in_roster(tb, mods[k])) k++;     /* already used / is a real roster name */
              if (k >= nm) break;
              if (apply_car(tb, h, dn[j], mods[k])) changed++;
              k++;
          }
      } }
    { const char *d = ini_get(opt, no, "level_donors");
      if (d) {
          char dn[16][40]; int mods[MAX_MODS], nd = split_list(d, dn, 16), nm = h->list_levels(mods, MAX_MODS), k = 0, j, a, b;
          for (a = 1; a < nm; a++) for (b = a; b > 0 && mods[b - 1] > mods[b]; b--) { int t = mods[b]; mods[b] = mods[b - 1]; mods[b - 1] = t; }
          for (j = 0; j < nd && k < nm; j++) {
              if (!all_digits(dn[j])) continue;
              while (k < nm && (level_in_use(tb, mods[k]) || mods[k] > MAX_LEVEL)) k++;
              if (k >= nm) break;
              if (apply_level(tb, h, atoi_(dn[j]), mods[k])) changed++;
              k++;
          }
      } }
    changed += extra_cars(im, tb, h, ini, opt, no);
    return changed;
}

/* ====================================================== Windows (ASI) layer */
#ifdef _WIN32
#include <windows.h>

static char g_dir[MAX_PATH + 8];          /* game folder, ends with a backslash */
static HANDLE (WINAPI *real_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static int g_redirects, g_debug, g_opens;

/* Appends one line to mods\td5mods.log. The file is opened for every call, so nothing
 * is lost if the game crashes. */
static void w_log(const char *msg) {
    char path[MAX_PATH + 32]; HANDLE f; DWORD n; char line[340];
    lstrcpyA(path, g_dir); lstrcatA(path, "mods\\td5mods.log");
    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, 0, FILE_END);
    lstrcpyA(line, msg); lstrcatA(line, "\r\n");
    WriteFile(f, line, (DWORD)lstrlenA(line), &n, 0);
    CloseHandle(f);
}
/* True if path names an existing regular file (not a directory). */
static int w_isfile(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
/* A file counts as present if it is in mods\ or in the game folder. */
static int w_exists(const char *rel) {
    char p[MAX_PATH + 64];
    lstrcpyA(p, g_dir); lstrcatA(p, "mods\\"); lstrcatA(p, rel);
    if (w_isfile(p)) return 1;
    lstrcpyA(p, g_dir); lstrcatA(p, rel);
    return w_isfile(p);
}
/* Writes into memory that may be read-only (code, import table): lifts the page
 * protection, copies, restores it and flushes the instruction cache so the CPU
 * sees the new code. */
static int w_poke(void *dst, const void *src, u32 n) {
    DWORD old, tmp;
    if (!VirtualProtect(dst, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), dst, n);
    return 1;
}
/* Executable memory for the small code stubs. */
static int w_alloc_exec(u32 size, void **p, u32 *va) {
    void *m = VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!m) return 0;
    *p = m; *va = (u32)(size_t)m;
    return 1;
}
/* Zero-filled read/write memory for the relocated tables. */
static int w_alloc(u32 size, void **p, u32 *va) {
    void *m = VirtualAlloc(0, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m) return 0;
    *p = m; *va = (u32)(size_t)m;
    return 1;
}
/* Base names of every zip in mods\cars\. */
static int w_list_cars(char (*out)[32], int max) {
    char pat[MAX_PATH + 32]; WIN32_FIND_DATAA fd; HANDLE h; int n = 0;
    lstrcpyA(pat, g_dir); lstrcatA(pat, "mods\\cars\\*.zip");
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        int L = lstrlenA(fd.cFileName) - 4;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && L >= 1 && L < 31 && n < max) { memcpy(out[n], fd.cFileName, (size_t)L); out[n][L] = 0; n++; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}
/* Numbers of every levelNNN.zip found in mods\. */
static int w_list_levels(int *out, int max) {
    char pat[MAX_PATH + 32]; WIN32_FIND_DATAA fd; HANDLE h; int n = 0;
    lstrcpyA(pat, g_dir); lstrcatA(pat, "mods\\level*.zip");
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const char *f = fd.cFileName;
        if (lstrlenA(f) == 12 && (f[0] | 32) == 'l' && digits3(f + 5) && n < max) { out[n++] = (f[5] - '0') * 100 + (f[6] - '0') * 10 + (f[7] - '0'); }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static int has_zip_ext(const char *s) {
    int L = lstrlenA(s);
    return L > 4 && s[L - 4] == '.' && (s[L - 3] | 32) == 'z' && (s[L - 2] | 32) == 'i' && (s[L - 1] | 32) == 'p';
}
/* ---- debug helpers, only used with  debug = 1  in mods.ini ---- */
static void dbg_num(const char *a, u32 v, const char *b) { char n[16], m[200]; itoa10((int)v, n); lstrcpyA(m, a); lstrcatA(m, n); lstrcatA(m, b); w_log(m); }
static void dbg_record(u32 slot) {
    u8 *r = (u8 *)(size_t)(g_dbg_rec + slot * REC_SIZE); int line, i;
    for (line = 0; line < 3; line++) {
        char txt[56], m[120], n[16]; u8 *l = r + line * 48;
        for (i = 0; i < 47 && l[i]; i++) txt[i] = (l[i] >= 32 && l[i] < 127) ? (char)l[i] : '?';
        txt[i] = 0; itoa10((int)slot, n);
        lstrcpyA(m, "debug: record slot "); lstrcatA(m, n); lstrcatA(m, " line "); itoa10(line, n); lstrcatA(m, n); lstrcatA(m, " = '"); lstrcatA(m, txt); lstrcatA(m, "'");
        w_log(m);
    }
}
/* Background thread for debug mode: logs the car cursor whenever it changes and dumps
 * the new cars' config records. It polls every 100 ms for about 20 minutes, then ends. */
static DWORD WINAPI diag_thread(LPVOID unused) {
    u32 last = 0xFFFFFFFFu, maxc = 0; int i, dumped = 0; (void)unused;
    for (i = 0; i < 12000; i++) {                     /* ~20 minutes, every 100 ms */
        u32 c = *(volatile u32 *)(size_t)g_dbg_cur;
        if (c != last) { last = c; if (c > maxc && c < 1000) maxc = c; dbg_num("debug: car cursor = ", c, ""); if (c == maxc && c >= 36) dbg_num("debug: highest cursor so far = ", maxc, ""); }
        if (!dumped && i == 150) { u32 s; dumped = 1; for (s = 36; s < g_dbg_n && s < 40; s++) dbg_record(s); }
        Sleep(100);
    }
    return 0;
}
/* Replacement for CreateFileA in the exe's import table. When the game opens a
 * relative zip (cars\x.zip, levelNNN.zip) and the same path exists under mods\, that
 * copy is opened instead. Every other call passes straight through. */
static HANDLE WINAPI hook_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_debug && name && name[0] && has_zip_ext(name) && g_opens < 800) { g_opens++; dbg_num("debug: zip open #", (u32)g_opens, ""); w_log(name); }
    if (name && name[0] && has_zip_ext(name) && name[1] != ':' && name[0] != '\\' && name[0] != '/' && lstrlenA(name) < MAX_PATH - 8) {
        char path[MAX_PATH * 2 + 16];
        lstrcpyA(path, g_dir); lstrcatA(path, "mods\\"); lstrcatA(path, name);
        if (w_isfile(path)) {
            if (g_redirects < 64) { char m[400]; g_redirects++; lstrcpyA(m, "open: "); lstrcatA(m, name); lstrcatA(m, "  ->  mods\\"); lstrcatA(m, name); w_log(m); }
            return real_CreateFileA(path, acc, share, sa, disp, flags, tmpl);
        }
    }
    return real_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
}

static char g_ini[32768];
/* Runs once from DllMain: reads mods\mods.ini, applies the patches and installs the
 * file redirect. Without a mods.ini the plugin stays completely inert. */
static void td5mods_init(void) {
    HMODULE exe = GetModuleHandleA(0);
    char path[MAX_PATH + 32]; HANDLE f; DWORD got = 0; int i, last = -1;
    Image im; Tables tb; Host h; int r; u8 *slot;
    GetModuleFileNameA(0, g_dir, MAX_PATH);
    for (i = 0; g_dir[i]; i++) if (g_dir[i] == '\\') last = i;
    g_dir[last + 1] = 0;
    lstrcpyA(path, g_dir); lstrcatA(path, "mods\\mods.ini");
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return;                       /* no mods.ini: stay completely inert */
    ReadFile(f, g_ini, sizeof(g_ini) - 1, &got, 0); CloseHandle(f); g_ini[got] = 0;
    w_log("---- td5mods start ----");
    h.exists = w_exists; h.poke = w_poke; h.alloc = w_alloc; h.alloc_exec = w_alloc_exec; h.list_cars = w_list_cars; h.list_levels = w_list_levels; h.log = w_log;
    if (!img_init(&im, (u8 *)exe, (u32)(size_t)exe)) { w_log("could not read the exe header"); return; }
    r = td5mods_run(&im, &h, g_ini, &tb);
    if (r < 0) return;
    { KV opt[16]; int no = ini_section(g_ini, "options", opt, 16); const char *rd = ini_get(opt, no, "redirect"), *dbg = ini_get(opt, no, "debug");
      if (dbg && dbg[0] == '1') {
          g_debug = 1; w_log("debug mode ON: logging every zip open, the car cursor and the new cars' records");
          if (g_dbg_cur) CreateThread(0, 0, diag_thread, 0, 0, 0);
      }
      if (rd && rd[0] == '0') { w_log("redirect disabled in mods.ini"); return; } }
    slot = find_iat_slot(&im, "KERNEL32.dll", "CreateFileA");
    if (!slot) { w_log("CreateFileA not in the exe's import table - redirect unavailable (files must be in cars\\ / game folder)"); return; }
    { DWORD old; real_CreateFileA = *(HANDLE (WINAPI **)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE))slot;
      if (VirtualProtect(slot, 4, PAGE_READWRITE, &old)) {
          *(void **)slot = (void *)hook_CreateFileA;
          VirtualProtect(slot, 4, old, &old);
          w_log("file redirect to mods\\ installed");
      } else w_log("could not hook CreateFileA"); }
}

/* ASI entry point. The ASI loader loads this DLL while the game is starting, which is
 * when all patching happens. */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(inst); td5mods_init(); }
    return TRUE;
}
#endif /* _WIN32 */

/* ============================================== native self-test (not shipped) */
#ifdef TD5MODS_TEST
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

static char T_dir[512];
static int t_file(const char *base, const char *rel) {
    char p[1100]; struct stat st; int i;
    snprintf(p, sizeof p, "%s/%s%s", T_dir, base, rel);
    for (i = 0; p[i]; i++) if (p[i] == '\\') p[i] = '/';
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}
static int t_exists(const char *rel) { return t_file("mods/", rel) || t_file("", rel); }
static u8 *T_extra; static u32 T_extra_va, T_extra_used, T_extra_cap = 1 << 22;
static int t_poke(void *dst, const void *src, u32 n) { memcpy(dst, src, n); return 1; }
static int t_alloc(u32 size, void **p, u32 *va) {
    size = (size + 15) & ~15u;
    if (T_extra_used + size > T_extra_cap) return 0;
    *p = T_extra + T_extra_used; *va = T_extra_va + T_extra_used; T_extra_used += size;
    return 1;
}
static int t_list_cars(char (*out)[32], int max) {
    DIR *d; struct dirent *e; int n = 0; char p[600];
    snprintf(p, sizeof p, "%s/mods/cars", T_dir);
    d = opendir(p); if (!d) return 0;
    while ((e = readdir(d)) && n < max) {
        size_t L = strlen(e->d_name);
        if (L > 4 && L - 4 < 31 && strcmp(e->d_name + L - 4, ".zip") == 0) { memcpy(out[n], e->d_name, L - 4); out[n][L - 4] = 0; n++; }
    }
    closedir(d); return n;
}
static int t_list_levels(int *out, int max) {
    DIR *d; struct dirent *e; int n = 0; char p[600];
    snprintf(p, sizeof p, "%s/mods", T_dir);
    d = opendir(p); if (!d) return 0;
    while ((e = readdir(d)) && n < max) {
        const char *f = e->d_name;
        if (strlen(f) == 12 && (f[0] | 32) == 'l' && digits3(f + 5) && strcmp(f + 8, ".zip") == 0) out[n++] = atoi_(f + 5);
    }
    closedir(d); return n;
}
static void t_log(const char *m) { printf("  [log] %s\n", m); }

int main(int argc, char **argv) {
    FILE *f; long sz; u8 *file, *mem; Image im; Tables tb; Host h; u32 pe, optsz, sh, base, simg, shdr; int nsec, i, r;
    char inipath[700]; static char ini[32768]; size_t got; u8 *slot;
    if (argc < 4) { fprintf(stderr, "usage: %s in.exe game_dir out.exe\n", argv[0]); return 2; }
    snprintf(T_dir, sizeof T_dir, "%s", argv[2]);
    f = fopen(argv[1], "rb"); if (!f) { perror("exe"); return 2; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    file = malloc((size_t)sz); if (fread(file, 1, (size_t)sz, f) != (size_t)sz) return 2; fclose(f);
    pe = rd32(file + 0x3C); base = rd32(file + pe + 24 + 28); simg = rd32(file + pe + 24 + 56); shdr = rd32(file + pe + 24 + 60);
    nsec = rd16(file + pe + 6); optsz = rd16(file + pe + 20); sh = pe + 24 + optsz;
    mem = calloc(1, simg); memcpy(mem, file, shdr);
    for (i = 0; i < nsec; i++) {
        u32 vsz = rd32(file + sh + 40 * i + 8), rva = rd32(file + sh + 40 * i + 12), rsz = rd32(file + sh + 40 * i + 16), rp = rd32(file + sh + 40 * i + 20);
        u32 n = rsz < vsz || vsz == 0 ? rsz : vsz;
        if (rva + n <= simg && rp + n <= (u32)sz) memcpy(mem + rva, file + rp, n);
    }
    if (!img_init(&im, mem, base)) { printf("not a PE\n"); return 1; }
    snprintf(inipath, sizeof inipath, "%s/mods/mods.ini", T_dir);
    f = fopen(inipath, "rb"); got = 0; if (f) { got = fread(ini, 1, sizeof ini - 1, f); fclose(f); } ini[got] = 0;
    T_extra = calloc(1, T_extra_cap); T_extra_va = base + simg;
    h.exists = t_exists; h.poke = t_poke; h.alloc = t_alloc; h.alloc_exec = t_alloc; h.list_cars = t_list_cars; h.list_levels = t_list_levels; h.log = t_log;
    r = td5mods_run(&im, &h, ini, &tb);
    printf("RESULT changes=%d\n", r);
    slot = find_iat_slot(&im, "KERNEL32.dll", "CreateFileA");
    printf("IAT_CreateFileA_rva=%s0x%x\n", slot ? "" : "none ", slot ? (unsigned)(slot - mem) : 0u);
    for (i = 0; i < nsec; i++) {
        u32 vsz = rd32(file + sh + 40 * i + 8), rva = rd32(file + sh + 40 * i + 12), rsz = rd32(file + sh + 40 * i + 16), rp = rd32(file + sh + 40 * i + 20);
        u32 n = rsz < vsz || vsz == 0 ? rsz : vsz;
        if (rva + n <= simg && rp + n <= (u32)sz) memcpy(file + rp, mem + rva, n);
    }
    f = fopen(argv[3], "wb"); fwrite(file, 1, (size_t)sz, f); fclose(f);
    printf("EXTRA_VA=0x%x USED=%u\n", T_extra_va, T_extra_used);
    if (T_extra_used) { char ep[800]; snprintf(ep, sizeof ep, "%s.extra", argv[3]); f = fopen(ep, "wb"); fwrite(T_extra, 1, T_extra_used, f); fclose(f); }
    return r < 0 ? 1 : 0;
}
#endif /* TD5MODS_TEST */

