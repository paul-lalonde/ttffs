#include <u.h>
#include <libc.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include <bio.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static char Egreg[] = "my memory of truetype is fading";
static char Enoent[] = "not found";

static char *fontpath = "/lib/font/ttf";

int scale = 1;

enum { MAXSUB = 0x100 };

typedef struct TFont TFont;
typedef struct TTFont TTFont;
typedef struct TTChMap TTChMap;
typedef struct TSubfont TSubfont;

struct TTChMap {
	int start, end, delta;
	int *tab;
	enum {
		TTCDELTA16 = 1,
		TTCINVALID = 2,
	} flags;
	int temp;
};
struct TTFont {
	int ppem, ascentpx, descentpx, linegap;
	float ppemscale;
  	int emsize;
             /* + internals */

	stbtt_fontinfo *info;
	stbtt_fontinfo info_raw;
};

typedef struct TTGlyph TTGlyph;

struct TTGlyph {
	int width;
	int height;
	int xoff;
	int yoff;
	int advanceWidthpx, lsbpx;
	int xmin, xmax, ymin, ymax;
	int xminpx, xmaxpx, yminpx, ymaxpx;
	uchar *bitmap;
};

static int
ttfrounddiv(int a, int b)
{
	if(b < 0){ a = -a; b = -b; }
	if(a > 0)
		return (a + b/2) / b;
	else
		return (a - b/2) / b;
}


TTFont *
ttfopen(char *filename, int ppem)
{
	TTFont *f;
	
	f = mallocz(sizeof(TTFont), 1);
	if(f == nil) return nil;

	Dir *dir = dirstat(filename);
	uchar *buf = mallocz(dir->length, 1);
	
	Biobuf *b;
	b = Bopen(filename, OREAD);
	if(b == nil)
		return nil;
	int n = Bread(b, buf, dir->length);
	if (n != dir->length) {
		exits("failed read");
	}
	
	stbtt_InitFont(&f->info_raw, buf, 0);
	f->info = & f->info_raw;

	f->ppem = ppem;
	f->emsize = ttUSHORT(f->info->data + f->info->head + 18);
	int ascent, descent;
	/* PAL: Broken.
	From: https://learn.microsoft.com/en-us/typography/opentype/spec/os2#uswinascent
	 The USE_TYPO_METRICS flag (bit 7) of the fsSelection field is used 
	 to choose between using sTypo* values or usWin* values for default line
	 metrics. See fsSelection for additional details.
	if (!stbtt_GetFontVMetricsOS2(f->info, &ascent, &descent, &f->linegap)) {
		stbtt_GetFontVMetrics(f->info, &ascent, &descent, &f->linegap);
	}
	*/
	int tab = stbtt__find_table(f->info->data, f->info->fontstart, "OS/2");
   	if (!tab) {
      		return nil;
	}
  	ascent  = ttUSHORT(f->info->data+tab + 74); //usWinAscent
	descent =  ttUSHORT(f->info->data+tab + 76); //usWinDescent

	f->ascentpx = (ascent * ppem + f->emsize - 1) / (f->emsize);
	f->descentpx = (descent * ppem + f->emsize - 1) / (f->emsize);
	return f;
}

void
ttfclose(TTFont *f)
{
	free(f);
}

struct TFont {
	int ref;
	Qid qid;
	Qid fileqid;
	char *fontfile;
	int nfontfile;
	TTFont *ttf;
	char *name9p;
	char *name;
	int size;
	TFont *next, *prev;
	TSubfont *sub[256];
};

struct TSubfont {
	TFont *font;
	Rune start, end;
	Qid qid;
	char *data;
	int ndata;
	TSubfont *next;
};

typedef struct FidAux FidAux;

struct FidAux {
	enum {
		FIDROOT,
		FIDFONT,
		FIDFONTF,
		FIDSUB,
	} type;
	TFont *f;
	TSubfont *sub;
};

TFont fontl = {.next = &fontl, .prev = &fontl};

static void *
emalloc(ulong n)
{
	void *v;
	
	v = mallocz(n, 1);
	if(v == nil) sysfatal("malloc: %r");
	setmalloctag(v, getcallerpc(&n));
	return v;
}

static uvlong
qidgen(void)
{
	static uvlong x;
	
	return ++x;
}

static void
fsattach(Req *r)
{
	r->ofcall.qid = (Qid){0, 0, QTDIR};
	r->fid->qid = r->ofcall.qid;
	r->fid->aux = emalloc(sizeof(FidAux));
	respond(r, nil);
}

//stbtt_FindGlyphIndex has most of the work done.
// 

typedef int (*CmapIterator)(int *start, int *length);

// PAL: This is non-re-entrant.  We can move this into a context if needed.
static int nonextstart, nonextncp;

// Return a start codepoint and number of codepoints.
int nonext(int *startcp, int *ncp)
{
print("nonext\n");
	*startcp = nonextstart;
	*ncp = nonextncp;
	return 1;
}

static uchar *f4ends;
static uchar *f4starts;
static ushort f4segcount;
static int f4curseg;
int
format4(int *startcp, int *ncp)
{
	//print("format4: f4segcount=%d, f4starts: %p, f4ends: %p, f4curseg: %d \n", f4segcount, f4starts, f4ends, f4curseg);
	*startcp = ttUSHORT(f4starts + 2 * f4curseg);
	ushort endcp = ttUSHORT(f4ends + 2 * f4curseg);
	*ncp = endcp - *startcp;
	f4curseg++;
	return f4curseg < f4segcount;
}

static uchar *f12_13tab;
static int f12_13count;
static int f12_13curseg;
int
format12_13(int *startcp, int *ncp)
{
	*startcp = ttULONG(f12_13tab + f12_13curseg * 12);
	int endcp = ttULONG(f12_13tab + f12_13curseg * 12 + 4);
	*ncp = endcp - *startcp;
	f12_13curseg++;
	return f12_13curseg < f12_13count;
}

CmapIterator
getcmapiterator(const stbtt_fontinfo *info)
{
	uchar *data = info->data;
	uint index_map = info->index_map;

	ushort format = ttUSHORT(data + index_map + 0);
	if (format == 0){ // apple byte encoding
      		uint bytes = ttUSHORT(data + index_map + 2);
		nonextstart = 0;
		nonextncp = bytes-6;
		return nonext;
	} else if (format == 6){
		nonextstart = ttUSHORT(data + index_map + 6);
      		nonextncp = ttUSHORT(data + index_map + 8);
		return nonext;
	} else if (format == 2) {
		assert(0); // @TODO: high-byte mapping for japanese/chinese/korean
		return 0;
	} else if (format == 4) { // standard mapping for windows fonts: an ordered collection of ranges
		// segments lie from endCodesBase .. endCodesBase + segCount 
		f4segcount = ttUSHORT(data+index_map+6) >> 1;
		f4ends = data + index_map + 14;
		f4starts = f4ends + f4segcount*2 + 2; /* two bytes per segment, 2 byte pad */
		f4curseg = 0;
		return format4;
	} else if (format == 12 || format == 13) {
		f12_13count = ttULONG(data+index_map+12);
		f12_13tab = data + index_map + 12 + 4;
		f12_13curseg = 0;
		return format12_13;
	}
   	assert(0);
	return nil;
}
void
mksubfonts(TFont *f)
{
	int k;
	TSubfont *s;
	Fmt fmt;
	int got0;
	int more = 1;
	
	fmtstrinit(&fmt);
	fmtprint(&fmt, "%d\t%d\n", f->ttf->ascentpx + f->ttf->descentpx, f->ttf->ascentpx);
	got0 = 0;
	CmapIterator cmap = getcmapiterator(f->ttf->info);
	while(more) {
		int startcp, ncp;
		more = cmap(&startcp, &ncp);
// print("more: %d, startcp: %d, ncp: %d\n", more, startcp, ncp);
		for(k = startcp; k < startcp + ncp; k += MAXSUB){
			s = emalloc(sizeof(TSubfont));
			s->start = k;
			if(k == 0) got0 = 1;
			s->end = k + MAXSUB - 1;
			if(s->end > startcp + ncp)
				s->end = startcp + ncp;
			s->font = f;
			s->qid = (Qid){qidgen(), 0, 0};
			s->next = f->sub[k >> 8 & 0xff];
			f->sub[k >> 8 & 0xff] = s;
			fmtprint(&fmt, "%#.4ux\t%#.4ux\ts.%.4ux-%.4ux\n", s->start, s->end, s->start, s->end);
		}
	}

	if(!got0){
		s = emalloc(sizeof(TSubfont));
		s->start = 0;
		s->end = 0;
		s->font = f;
		s->qid = (Qid){qidgen(), 0, 0};
		s->next = f->sub[0];
		f->sub[0] = s;
		fmtprint(&fmt, "%#.4ux\t%#.4ux\ts.%.4ux-%.4ux\n", 0, 0, 0, 0);
	}
	f->fontfile = fmtstrflush(&fmt);
	f->nfontfile = strlen(f->fontfile);
}

static void
blit8(uchar *t, int x, int y, int tstride, uchar *s, int w, int h)
{
	int tx, ty, sx, sy;
	u16int b;
	uchar *tp, *sp;
	
	if(y < 0) y = 0;
	ty = y;
	sp = s;
	for(sy = 0; sy < h; sy++, ty++){
		tx = x;
		tp = t + ty * tstride + tx;
		b = 0;
		for(sx = 0; sx < w; sx ++){
			*tp++ = *sp++;
		}
	}
}

static TTGlyph *
emptyglyph(TTFont *f, int glyph)
{
	TTGlyph *g;

	g = mallocz(sizeof(TTGlyph), 1);
	if(g == nil)
		return nil;

	stbtt_GetGlyphHMetrics(f->info, glyph, &g->advanceWidthpx, &g->lsbpx);
	g->xmax = g->advanceWidthpx;
	g->advanceWidthpx = ttfrounddiv(g->advanceWidthpx * f->ppem * 64, f->emsize) >> 6;
	g->lsbpx = ttfrounddiv(g->lsbpx * f->ppem * 64, f->emsize) >> 6;
	g->xmin = 0;
	g->ymin = 0;
	g->ymax = 1;
	g->xminpx = 0;
	g->xmaxpx = g->advanceWidthpx - g->lsbpx;
	g->yminpx = 0;
	g->ymaxpx = 1;
	return g;
}

static TTGlyph*
ttfgetglyph(TTFont *f, int glyph)
{
	TTGlyph *g = malloc(sizeof(TTGlyph));
	float scale = stbtt_ScaleForMappingEmToPixels(f->info, f->ppem);
	g->bitmap = stbtt_GetGlyphBitmap(f->info, scale, scale, glyph, &g->width, &g->height, &g->xoff, &g->yoff);
	if (!g->bitmap){
		free(g);
		return emptyglyph(f, glyph);
	}
/*	stbtt_GetGlyphBitmapBox(f->info, glyph, scale, scale, &g->xmin, &g->ymin, &g->xmax, &g->ymax);
	g->xminpx = (float)g->xmin * scale;
	g->xmaxpx = (float)g->xmax * scale;
	g->yminpx = (float)g->ymin * scale;
	g->ymaxpx = (float)g->ymax * scale;
*/
	// The libttf code jumps through hoops to get these from the font points.  Needed or no?
	stbtt_vertex *pt;
	int npt = stbtt_GetGlyphShape(f->info, glyph, &pt);
	g->xmin = pt[0].x;
	g->xmax = pt[0].x;
	g->ymin = pt[0].y;
	g->ymax = pt[0].y;
	int i;
	for(i = 1; i < npt - 2; i++){
		if(pt[i].x < g->xmin)
			g->xmin = pt[i].x;
		if(pt[i].x > g->xmax)
			g->xmax = pt[i].x;
		if(pt[i].y < g->ymin)
			g->ymin = pt[i].y;
		if(pt[i].y > g->ymax)
			g->ymax = pt[i].y;
	}
	g->xminpx = ttfrounddiv(g->xmin * f->ppem * 64, f->emsize) >> 6;
if (g->xminpx < 0) g->xminpx = 0;
	g->xmaxpx = ttfrounddiv(g->xmax * f->ppem * 64, f->emsize) + 63 >> 6;
	g->yminpx = ttfrounddiv(g->ymin * f->ppem * 64, f->emsize) >> 6;
	g->ymaxpx = ttfrounddiv(g->ymax * f->ppem * 64, f->emsize) + 63 >> 6;
	stbtt_FreeShape(f->info, pt);

	stbtt_GetGlyphHMetrics(f->info, glyph, &g->advanceWidthpx, &g->lsbpx);
	g->advanceWidthpx = ttfrounddiv(g->advanceWidthpx * f->ppem * 64, f->emsize) >> 6;
	g->lsbpx = ttfrounddiv(g->lsbpx * f->ppem * 64, f->emsize) >> 6;

	return g;
}

static void
ttfputglyph(TTGlyph *g)
{
	if (g->bitmap)
		stbtt_FreeBitmap(g->bitmap, 0);
	free(g);
}

static void
dumpTextRep(uchar *bitmap, int w, int h)
{
	int i, j;
	for (j=0; j < h; ++j) {
		for (i=0; i < w; ++i)
			print("%c", " .:ioVM@"[bitmap[j*w+i]>>5]);
		print("\n");
	}
}

static void
compilesub(TFont *f, TSubfont *s)
{
	int n, i, w, x, h, g, sz;
	char *d, *p;
	TTGlyph **gs;
	TTFont *t;

	t = f->ttf;
	float scale = stbtt_ScaleForMappingEmToPixels(t->info, t->ppem);
	n = s->end - s->start + 1;
	gs = emalloc9p(sizeof(TTGlyph *) * n);
	w = 0;
	h = t->ascentpx + t->descentpx;
	for(i = 0; i < n; i++){
		if(s->start + i == 0)
			g = 0;
		else
			//g = ttffindchar(t, s->start + i);
			g = stbtt_FindGlyphIndex(t->info, s->start + i);
		if((gs[i] = ttfgetglyph(t, g)) == nil && g != 0)
			gs[i] = ttfgetglyph(t, 0);
		assert(gs[i] != nil);
	   	w += gs[i]->width;
	}
	sz = 5 * 12 + w * h + 3 * 12 + (n + 1) * 6;
	d = emalloc(sz);
	p = d + sprint(d, "%11s %11d %11d %11d %11d ", "k8", 0, 0, w, h);
	x = 0;
	for(i = 0; i < n; i++){
//print("Ascentpx = %d, gs[%d]->ymaxpx = %d\n", t->ascentpx, i, gs[i]->ymaxpx);
//print("Blit: target %p, x%d, y %d, stride %d, gsbitmap %p, width %d, height %d\n",(uchar*)p, x, t->ascentpx - gs[i]->ymaxpx, w, gs[i]->bitmap, gs[i]->width, gs[i]->height);
		blit8((uchar*)p, x, t->ascentpx - gs[i]->ymaxpx, w, gs[i]->bitmap, gs[i]->width, gs[i]->height);
		//dumpTextRep(gs[i]->bitmap, gs[i]->width, gs[i]->height);
		//print("\n");

		x += gs[i]->width;
	}
	p += w * h;
	p += sprint(p, "%11d %11d %11d ", n, h, t->ascentpx);
	x = 0;
	for(i = 0; i < n; i++){
		*p++ = x;
		*p++ = x >> 8;
		*p++ = 0;
		*p++ = h;
		*p++ = gs[i]->xminpx;
		if(gs[i]->advanceWidthpx != 0) 
			*p++ = gs[i]->advanceWidthpx - gs[i]->lsbpx;
		else
			*p++ = gs[i]->width;
		x += gs[i]->width;
	}
	*p++ = x;
	*p = x >> 8;
	s->data = d;
	s->ndata = sz;
	for(i = 0; i < n; i++){
		ttfputglyph(gs[i]);
	}
	free(gs);
}



TFont *
tryfont(char *name)
{
	TTFont *ttf;
	TFont *f;
	char *d, *buf, *p;
	int sz;
	
	for(f = fontl.next; f != &fontl; f = f->next)
		if(strcmp(f->name9p, name) == 0)
			return f;
	d = strrchr(name, '.');
	if(d == nil){
	inval:
		werrstr("invalid file name");
		return nil;
	}
	sz = strtol(d + 1, &p, 10);
	if(d[1] == 0 || *p != 0)
		goto inval;
	sz *= scale;
	buf = estrdup9p(name);
	buf[d - name] = 0;
	p = smprint("%s/%s", fontpath, buf);
	if(p == nil)
		sysfatal("smprint: %r");
	ttf = ttfopen(p, sz);
	free(p);
	if(ttf == nil){
		free(buf);
		return nil;
	}
	f = emalloc(sizeof(TFont));
	f->ttf = ttf;
	f->name9p = strdup(name);
	f->name = buf;
	f->size = sz;
	f->qid = (Qid){qidgen(), 0, QTDIR};
	f->fileqid = (Qid){qidgen(), 0, 0};
	f->next = &fontl;
	f->prev = fontl.prev;
	f->next->prev = f;
	f->prev->next = f;
	mksubfonts(f);
	return f;
}


static char *
fsclone(Fid *old, Fid *new)
{
	new->aux = emalloc(sizeof(FidAux));
	*(FidAux*)new->aux = *(FidAux*)old->aux;
	return nil;
}

static void
fsdestroyfid(Fid *f)
{
	FidAux *fa;
	
	fa = f->aux;
	free(fa);
	f->aux = nil;
}

static TSubfont *
findsubfont(TFont *f, char *name)
{
	char *p, *q;
	char buf[16];
	int a, b;
	TSubfont *s;

	if(name[0] != 's' || name[1] != '.' || name[2] == '-')
		return nil;
	a = strtol(name + 2, &p, 16);
	if(*p != '-')
		return nil;
	b = strtol(p + 1, &q, 16);
	if(p + 1 == q || *q != 0)
		return nil;
	snprint(buf, nelem(buf), "s.%.4ux-%.4ux", a, b);
	if(strcmp(buf, name) != 0)
		return nil;
	for(s = f->sub[a>>8&0xff]; s != nil; s = s->next)
		if(s->start == a && s->end == b)
			break;
	return s;
}

static char *
fswalk(Fid *fid, char *name, Qid *qid)
{
	static char errbuf[ERRMAX];
	FidAux *fa;

	fa = fid->aux;
	assert(fa != nil);
	switch(fa->type){
	case FIDROOT:
		fa->f = tryfont(name);
		if(fa->f == nil){
			rerrstr(errbuf, nelem(errbuf));
			return errbuf;
		}
		fa->f->ref++;
		fa->type = FIDFONT;
		fid->qid = fa->f->qid;
		*qid = fa->f->qid;
		return nil;
	case FIDFONT:
		if(strcmp(name, "font") == 0){
			fa->type = FIDFONTF;
			fid->qid = fa->f->fileqid;
			*qid = fa->f->fileqid;
			return nil;
		}
		fa->sub = findsubfont(fa->f, name);
		if(fa->sub == nil)
			return Enoent;
		fa->type = FIDSUB;
		fid->qid = fa->sub->qid;
		*qid = fa->sub->qid;
		return nil;
	default:
		return Egreg;
	}
}

static void
fsstat(Req *r)
{
	FidAux *fa;

	fa = r->fid->aux;
	assert(fa != nil);
	r->d.uid = estrdup9p(getuser());
	r->d.gid = estrdup9p(getuser());
	r->d.muid = estrdup9p(getuser());
	r->d.mtime = r->d.atime = time(0);
	r->d.qid = r->fid->qid;
	switch(fa->type){
	case FIDROOT:
		r->d.mode = 0777;
		r->d.name = estrdup9p("/");
		respond(r, nil);
		break;
	case FIDFONT:
		r->d.mode = 0777;
		r->d.name = estrdup9p(fa->f->name9p);
		respond(r, nil);
		break;
	case FIDFONTF:
		r->d.mode = 0666;
		r->d.name = estrdup9p("font");
		r->d.length = fa->f->nfontfile;
		respond(r, nil);
		break;
	case FIDSUB:
		r->d.mode = 0666;
		r->d.name = smprint("s.%.4ux-%.4ux", fa->sub->start, fa->sub->end);
		r->d.length = fa->sub->ndata;
		respond(r, nil);
		break;
	default:
		respond(r, Egreg);
	}
}

static int
fontdirread(int n, Dir *d, void *aux)
{
	FidAux *fa;
	
	fa = aux;
	if(n == 0){
		d->name = estrdup9p("font");
		d->uid = estrdup9p(getuser());
		d->gid = estrdup9p(getuser());
		d->muid = estrdup9p(getuser());
		d->mode = 0666;
		d->qid = fa->f->fileqid;
		d->mtime = d->atime = time(0);
		d->length = fa->f->nfontfile;
		return 0;
	}
	return -1;
}

static void
fsread(Req *r)
{
	FidAux *fa;

	fa = r->fid->aux;
	assert(fa != nil);
	switch(fa->type){
	case FIDROOT:
		respond(r, nil);
		break;
	case FIDFONT:
		dirread9p(r, fontdirread, fa);
		respond(r, nil);
		break;
	case FIDFONTF:
		readbuf(r, fa->f->fontfile, fa->f->nfontfile);
		respond(r, nil);
		break;
	case FIDSUB:
		if(fa->sub->data == nil)
			compilesub(fa->f, fa->sub);
		readbuf(r, fa->sub->data, fa->sub->ndata);
		respond(r, nil);
		break;
	default:
		respond(r, Egreg);
	}
}

Srv fssrv = {
	.attach = fsattach,
	.walk1 = fswalk,
	.clone = fsclone,
	.stat = fsstat,
	.read = fsread,
	.destroyfid = fsdestroyfid,
};

static void
usage(void)
{
	fprint(2, "usage: %s [-F fontpath]\n", argv0);
	exits("usage");
}

int
getint(char *s)
{
	if(s == nil)
		usage();
	return strtol(s, 0, 0);
}

void
main(int argc, char **argv)
{
	ARGBEGIN {
	case 'F':
		fontpath = EARGF(usage());
		break;
	case 's':
		scale = getint(ARGF());
		break;
	default:
		usage();
	} ARGEND;
	
	unmount(nil, "/n/ttf");
	postmountsrv(&fssrv, nil, "/n/ttf", 0);
	exits(nil);
}
/*
void
main(int argc, char **argv)
{
	char * testfont = "/lib/font/ttf/Go-Medium.ttf";

	TTFont *ttf = ttfopen(testfont, 12);

	if (ttf == nil) {
		print("Fail\n");
		exits("Failed to open test font");
	}

	print("ppem %d, ppemscale %f, ascent %d, descent %d, linegap %d\n",
		ttf->ppem, ttf->ppemscale, ttf->ascentpx, ttf->descentpx, ttf->linegap);
	
	ttfclose(ttf);

	TFont *tf = tryfont("Go-Regular.ttf.16");
	if (!tf) {
		print("Failed to tryfont\n");
		print("Fail\n");
		exits("Failed to tryfont test font");
	}
	print(tf->fontfile);
	
	int spaceglyph = stbtt_FindGlyphIndex(tf->ttf->info, (int)' ');
	TTGlyph *gs = ttfgetglyph(tf->ttf, spaceglyph);
	dumpTextRep(gs->bitmap, gs->width, gs->height); 
	print("Success\n");
}
*/