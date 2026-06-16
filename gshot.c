/* Host render of a real page through the browser's ACTUAL layout code: fetches
 * HTML with curl, runs user/browser html.c (dom.c + css.c) to produce the styled
 * text/links/images exactly as the browser does, then draws the result with
 * user/lib/font + the same blit/wrap/styled-line math as main.c. Output: a PPM.
 *
 * It is not a live OwOS screenshot (this sandbox can't drive QEMU) — it is the
 * browser's own parser/layout, drawn on the host. No JS (the browser has none
 * either); images are shown as the browser's pre-load alt boxes. */
#include "browser.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- the globals/allocators main.c normally provides ----------------------- */
const unsigned palette[16] = {
	0x202830, 0x1a4fbf, 0xc01818, 0x108010, 0x9020a0, 0x9a6020, 0x107c90,
	0x586070, 0x16387a, 0x000000, 0x707000, 0xd07000, 0x808890, 0x304060,
	0x2050d0, 0xa0a0a0,
};
static struct browser binst;
struct browser *B = &binst;
/* Host stubs for the kernel FS calls html.c's @font-face local loader uses: report
 * "absent" so the /etc/fonts monospace path is skipped on the host. */
int stat(const char *p, struct statinfo *o) { (void)p; (void)o; return -1; }
int readfile_at(const char *n, unsigned off, void *b, unsigned l) { (void)n; (void)off; (void)b; (void)l; return -1; }
void blk_lock(void) {}
void blk_unlock(void) {}

/* The shared UTF-8 codec (user/lib/lib.c) — provided here since the harness does
 * not link lib.c (it pulls in syscalls). Exact copies so html.c behaves as built. */
int utf8_seqlen(unsigned char c){ if(c<0x80)return 1; if((c&0xE0)==0xC0)return 2;
	if((c&0xF0)==0xE0)return 3; if((c&0xF8)==0xF0)return 4; return 1; }
int utf8_decode(const char*s,const char*end,unsigned*cp){ unsigned char c=(unsigned char)s[0];
	if(c<0x80){*cp=c;return 1;}
	if((c&0xE0)==0xC0&&s+1<end&&(s[1]&0xC0)==0x80){*cp=((unsigned)(c&0x1F)<<6)|(unsigned char)(s[1]&0x3F);return 2;}
	if((c&0xF0)==0xE0&&s+2<end&&(s[1]&0xC0)==0x80&&(s[2]&0xC0)==0x80){*cp=((unsigned)(c&0x0F)<<12)|((unsigned)(s[1]&0x3F)<<6)|(unsigned char)(s[2]&0x3F);return 3;}
	if((c&0xF8)==0xF0&&s+3<end&&(s[1]&0xC0)==0x80&&(s[2]&0xC0)==0x80&&(s[3]&0xC0)==0x80){*cp=((unsigned)(c&0x07)<<18)|((unsigned)(s[1]&0x3F)<<12)|((unsigned)(s[2]&0x3F)<<6)|(unsigned char)(s[3]&0x3F);return 4;}
	*cp=0xFFFD;return 1; }
int utf8_encode(unsigned cp,char*o){ if(cp<0x80){o[0]=cp;return 1;}
	if(cp<0x800){o[0]=0xC0|(cp>>6);o[1]=0x80|(cp&0x3F);return 2;}
	if(cp<0x10000){o[0]=0xE0|(cp>>12);o[1]=0x80|((cp>>6)&0x3F);o[2]=0x80|(cp&0x3F);return 3;}
	o[0]=0xF0|(cp>>18);o[1]=0x80|((cp>>12)&0x3F);o[2]=0x80|((cp>>6)&0x3F);o[3]=0x80|(cp&0x3F);return 4; }

static int grow_cap(int cap, int init, int need)
{
	int nc = cap > 0 ? cap : init;
	while (nc < need) nc += nc / 2 + 1;
	return nc;
}
int ensure_raw(int need){ if(need<=B->raw_cap)return 1; int nc=grow_cap(B->raw_cap,4096,need);
	char*p=realloc(B->raw,nc); if(p){B->raw=p;B->raw_cap=nc;} return p!=0; }
int ensure_text(int need){ if(need<=B->text_cap)return 1; int nc=grow_cap(B->text_cap,4096,need);
	char*t=realloc(B->text,nc); unsigned char*a=realloc(B->attr,nc);
	if(t)B->text=t; if(a)B->attr=a; if(t&&a)B->text_cap=nc; return t&&a; }
int ensure_links(int need){ if(need<=B->links_cap)return 1; int nc=grow_cap(B->links_cap,32,need);
	struct link*p=realloc(B->links,nc*sizeof*p); if(p){B->links=p;B->links_cap=nc;} return p!=0; }
int ensure_imgs(int need){ if(need<=B->imgs_cap)return 1; int nc=grow_cap(B->imgs_cap,16,need);
	struct inl_img*p=realloc(B->imgs,nc*sizeof*p); if(p){B->imgs=p;B->imgs_cap=nc;} return p!=0; }
int ensure_lines(int need){ if(need<=B->line_cap)return 1; int nc=grow_cap(B->line_cap,64,need);
	int*o=realloc(B->line_off,nc*sizeof*o); int*l=realloc(B->line_len,nc*sizeof*l);
	if(o)B->line_off=o; if(l)B->line_len=l; if(o&&l)B->line_cap=nc; return o&&l; }

/* --- canvas + font drawing (mirrors main.c body_draw/blit_glyph) ------------ */
#define CW    900
#define CH    2600
#define LINE_H 18
static unsigned char canvas[CH][CW][3];
static struct font *g_body;
static int g_baseline;

static void px(int x,int y,unsigned c){ if(x<0||x>=CW||y<0||y>=CH)return;
	canvas[y][x][0]=(c>>16)&0xff; canvas[y][x][1]=(c>>8)&0xff; canvas[y][x][2]=c&0xff; }
static void fillrect(int x,int y,int w,int h,unsigned c){ for(int j=0;j<h;j++)for(int i=0;i<w;i++)px(x+i,y+j,c); }
static void rect(int x,int y,int w,int h,unsigned c){ for(int i=0;i<w;i++){px(x+i,y,c);px(x+i,y+h-1,c);}
	for(int j=0;j<h;j++){px(x,y+j,c);px(x+w-1,y+j,c);} }

#define u8 utf8_decode

static void blit(int x,int baseline,const unsigned char*bm,int w,int h,int ox,int oy,unsigned fg){
	if(!bm)return; unsigned fr=(fg>>16)&0xff,fgc=(fg>>8)&0xff,fbb=fg&0xff;
	for(int gy=0;gy<h;gy++){ int py=baseline+oy+gy; if(py<0||py>=CH)continue;
		for(int gx=0;gx<w;gx++){ int pxx=x+ox+gx; if(pxx<0||pxx>=CW)continue;
			unsigned a=bm[gy*w+gx]; if(!a)continue; unsigned ia=255-a;
			canvas[py][pxx][0]=(fr*a+canvas[py][pxx][0]*ia)/255;
			canvas[py][pxx][1]=(fgc*a+canvas[py][pxx][1]*ia)/255;
			canvas[py][pxx][2]=(fbb*a+canvas[py][pxx][2]*ia)/255; } } }

/* Draw body text through user/lib/font, exactly as the browser does: the page's
 * @font-face face (B->body_font) when one loaded, else the default UI sans. */
static struct font *g_sans;                       /* default = Liberation Sans */
static struct font *active(void) { return B->body_font ? B->body_font : g_sans; }

static int cp_adv(unsigned cp){ struct font*f=active(); if(!f)return 8;
	int gi=font_glyph_index(f,(int)cp); int w,h,ox,oy,adv;
	unsigned char*bm=font_render_glyph(f,gi,15.0f,&w,&h,&ox,&oy,&adv); free(bm); return adv; }
static int draw_run(int x,int y,const char*s,int n,unsigned fg){ struct font*f=active();
	if(!f) return x; const char*end=s+n; unsigned cp; int asc=0; font_vmetrics(f,15.0f,&asc,0,0);
	while(s<end){ s+=u8(s,end,&cp); int gi=font_glyph_index(f,(int)cp);
		int w,h,ox,oy,adv; unsigned char*bm=font_render_glyph(f,gi,15.0f,&w,&h,&ox,&oy,&adv);
		blit(x,y+asc,bm,w,h,ox,oy,fg); free(bm); x+=adv; }
	return x; }
static int run_w(const char*s,int n){ const char*end=s+n; unsigned cp; int w=0;
	while(s<end){ s+=u8(s,end,&cp); w+=cp_adv(cp);} return w; }

/* --- the curl-backed sub-resource fetch hook (CSS / fonts) ------------------ */
static unsigned char *page_get(const char *url, int *len)
{
	char cmd[2200];
	snprintf(cmd,sizeof cmd,"curl -fsSL --max-time 12 -A 'OwOS' '%s' -o /tmp/gshot_res.bin 2>/dev/null",url);
	if(system(cmd)) { *len=0; return 0; }
	FILE*f=fopen("/tmp/gshot_res.bin","rb"); if(!f){*len=0;return 0;}
	fseek(f,0,SEEK_END); long L=ftell(f); fseek(f,0,SEEK_SET);
	if(L<=0){fclose(f);*len=0;return 0;}
	unsigned char*b=malloc(L+1); if(fread(b,1,L,f)!=(size_t)L){fclose(f);free(b);*len=0;return 0;}
	b[L]=0; fclose(f); *len=(int)L; return b;
}
static void fetch_batch(char *const *urls,int n,unsigned char **bodies,int *lens){
	for(int i=0;i<n;i++) bodies[i]=page_get(urls[i],&lens[i]);
}

/* --- inline image decode (the real browser fetches+decodes; mirror that) ---- */
static unsigned char *g_img_rgba[64]; static int g_img_w[64], g_img_h[64];
struct memrd { const unsigned char*d; int pos,len; };
static int memrd_get(void*c){ struct memrd*m=c; return m->pos<m->len ? m->d[m->pos++] : -1; }
struct rowctx { unsigned char*buf; unsigned w,h; };
static int row_cb(void*c,unsigned y,const unsigned char*rgba,unsigned w){
	struct rowctx*r=c; if(y<r->h) memcpy(r->buf+(size_t)y*w*4, rgba, (size_t)w*4); return 0; }

static void decode_img(int q,const char*absurl){
	if(q<0||q>=64) return;
	int n=0; unsigned char*png=page_get(absurl,&n); if(!png||n<8){ free(png); return; }
	struct memrd m={png,0,n}; unsigned w=0,h=0;
	if(upng_dimensions(memrd_get,&m,&w,&h)!=UPNG_OK||!w||!h||w>4000||h>4000){ free(png); return; }
	unsigned char*buf=malloc((size_t)w*h*4); if(!buf){ free(png); return; }
	struct memrd m2={png,0,n}; struct rowctx rc={buf,w,h};
	if(upng_decode(memrd_get,&m2,row_cb,&rc,0,0)==UPNG_OK){
		g_img_rgba[q]=buf; g_img_w[q]=(int)w; g_img_h[q]=(int)h;
		B->imgs[q].ok=1; B->imgs[q].iw=w; B->imgs[q].ih=h;
	} else free(buf);
	free(png);
}

/* display size of image q within pane width paw (honours cw/ch like img_disp_size) */
static void img_size(int q,int paw,int*dw,int*dh){
	struct inl_img*im=&B->imgs[q]; int nat=im->ok&&im->iw&&im->ih;
	int w=0,h=0;
	if(im->cw&&im->ch){ w=im->cw; h=im->ch; }
	else if(im->cw){ w=im->cw; h=nat?(int)((long)im->cw*im->ih/im->iw):im->cw; }
	else if(im->ch){ h=im->ch; w=nat?(int)((long)im->ch*im->iw/im->ih):im->ch; }
	else if(nat){ w=im->iw; h=im->ih; } else { *dw=0;*dh=0; return; }
	if(w>paw){ h=(int)((long)h*paw/(w?w:1)); w=paw; }
	*dw=w<1?1:w; *dh=h<1?1:h;
}
static void blit_img(int x,int y,int q,int dw,int dh){
	unsigned char*src=g_img_rgba[q]; if(!src)return; int sw=g_img_w[q],sh=g_img_h[q];
	for(int j=0;j<dh;j++)for(int i=0;i<dw;i++){
		int sx=i*sw/dw, sy=j*sh/dh; const unsigned char*pix=src+((size_t)sy*sw+sx)*4;
		unsigned a=pix[3]; int X=x+i,Y=y+j; if(X<0||X>=CW||Y<0||Y>=CH||!a)continue;
		canvas[Y][X][0]=(pix[0]*a+canvas[Y][X][0]*(255-a))/255;
		canvas[Y][X][1]=(pix[1]*a+canvas[Y][X][1]*(255-a))/255;
		canvas[Y][X][2]=(pix[2]*a+canvas[Y][X][2]*(255-a))/255; }
}

/* --- the same word-wrap as main.c rewrap (text only; images = 1 alt row) ---- */
static void wrap(int paw){
	B->nlines=0; int i=0;
	for(int q=0;q<B->n_imgs;q++)B->imgs[q].start_row=-1;
	while(i<B->text_len){ int j=i; while(j<B->text_len&&B->text[j]!='\n')j++;
		if(j==i){ int img=-1; for(int q=0;q<B->n_imgs;q++) if(B->imgs[q].text_pos==i){img=q;break;}
			int rows=1;
			if(img>=0){ int dw=0,dh=0; img_size(img,paw,&dw,&dh);
				rows=dh>0?(dh+LINE_H-1)/LINE_H:1; if(rows<1)rows=1;
				B->imgs[img].start_row=B->nlines; B->imgs[img].rows=rows; }
			for(int rr=0;rr<rows;rr++){ if(!ensure_lines(B->nlines+1))return;
				B->line_off[B->nlines]=i; B->line_len[B->nlines]=0; B->nlines++; }
		} else { int ls=i; while(ls<j){ int w=0,e=ls,lastsp=-1;
				while(e<j){ unsigned cp; int nb=u8(&B->text[e],&B->text[j],&cp); int cw=cp_adv(cp);
					if(w+cw>paw&&e>ls)break; if(cp==' ')lastsp=e; w+=cw; e+=nb; }
				int brk=(e<j&&lastsp>ls)?lastsp:e;
				if(!ensure_lines(B->nlines+1))return; B->line_off[B->nlines]=ls; B->line_len[B->nlines]=brk-ls; B->nlines++;
				ls=brk; while(ls<j&&B->text[ls]==' ')ls++; } }
		i=j+1; }
}

/* mirror of draw_styled_line: same-attr runs, bold overstrike, underline rule */
static void draw_line(int x,int y,int idx){
	int off=B->line_off[idx], len=B->line_len[idx], i=0;
	/* an image row? draw the decoded bitmap (or an alt box if it didn't load) */
	for(int q=0;q<B->n_imgs;q++) if(B->imgs[q].start_row==idx){
		int dw=0,dh=0; img_size(q,CW-2*x,&dw,&dh);
		if(B->imgs[q].ok && g_img_rgba[q] && dw>0 && dh>0){ blit_img(x,y,q,dw,dh); return; }
		const char*t=B->imgs[q].alt[0]?B->imgs[q].alt:"[image]";
		int w=run_w(t,strlen(t))+10; if(w<40)w=40;
		fillrect(x,y,w,LINE_H,0xe6e9ef); rect(x,y,w,LINE_H,0xb4bac4);
		draw_run(x+5,y,t,strlen(t),0x7a828c); return; }
	while(i<len){ unsigned char a=B->attr[off+i]; int rl=0; int rs=off+i;
		while(i<len&&B->attr[off+i]==a){ int nb=1; unsigned char c=(unsigned char)B->text[off+i];
			if(c>=0xC0)nb=(c>=0xF0)?4:(c>=0xE0)?3:2; if(i+nb>len)nb=1; rl+=nb; i+=nb; }
		unsigned col=palette[a&ST_COLOR];
		draw_run(x,y,&B->text[rs],rl,col);
		if(a&ST_BOLD)draw_run(x+1,y,&B->text[rs],rl,col);
		int rw=run_w(&B->text[rs],rl);
		if(a&ST_UNDER)fillrect(x,y+LINE_H-4,rw,1,col);
		x+=rw; }
}

int main(int argc,char**argv){
	const char *url = argc>1 ? argv[1] : "https://www.google.com/";

	for(int y=0;y<CH;y++)for(int x=0;x<CW;x++){canvas[y][x][0]=canvas[y][x][1]=canvas[y][x][2]=0xf4;}

	/* fetch the page HTML */
	int rawlen=0; unsigned char*html=page_get(url,&rawlen);
	if(!html){ fprintf(stderr,"fetch failed: %s\n",url); return 1; }
	ensure_raw(rawlen+1); memcpy(B->raw,html,rawlen); B->raw[rawlen]=0; B->raw_len=rawlen;
	free(html);

	/* mimic fetch.c: hand html.c the HTTP Content-Type so charset is honoured */
	char ctcmd[2200], ct[256]={0};
	snprintf(ctcmd,sizeof ctcmd,"curl -fsSI --max-time 12 -A 'OwOS' '%s' 2>/dev/null | grep -i '^content-type:' | head -1",url);
	FILE*cp=popen(ctcmd,"r"); if(cp){ if(fgets(ct,sizeof ct,cp)){} pclose(cp); }
	html_set_charset(ct[0]?ct:0);

	/* THE BROWSER'S REAL LAYOUT */
	html_to_text(url, fetch_batch);

	/* decode inline images (the real browser does this on its fetch threads) */
	for(int q=0;q<B->n_imgs;q++){
		char abs[1024]; resolve_url(url,B->imgs[q].href,abs,sizeof abs);
		decode_img(q,abs);
	}

	/* Default body font = the shipped Liberation Sans (what gfx_ui loads at
	 * runtime). If the page's font-family resolved to an @font-face we could
	 * load (TTF/OTF), html_to_text set B->body_font and active() uses it. */
	{ FILE*f=fopen("fonts/LiberationSans-Regular.ttf","rb");
	  if(f){ fseek(f,0,SEEK_END); long L=ftell(f); fseek(f,0,SEEK_SET);
	    unsigned char*b=malloc(L); if(fread(b,1,L,f)){} fclose(f); g_sans=font_load(b,(int)L); } }

	const char *title = B->doc_title[0] ? B->doc_title : url;

	/* chrome: tab strip + address bar */
	fillrect(0,0,CW,30,0x2b3340);
	fillrect(8,5,200,20,0xf4f5f7); draw_run(16,7,title,strlen(title),0x202830);
	fillrect(0,30,CW,34,0xe9ecf1);
	fillrect(70,37,CW-150,20,0xffffff); rect(70,37,CW-150,20,0xc2c8d2);
	draw_run(78,39,url,strlen(url),0x202830);
	draw_run(14,39,"<  >",4,0x6a727c);

	/* page content */
	int pane_x=16, pane_top=74, paw=CW-2*pane_x;
	wrap(paw);
	int y=pane_top, maxy=pane_top;
	for(int r=0;r<B->nlines && y<CH-LINE_H;r++){ draw_line(pane_x,y,r); y+=LINE_H; maxy=y; }

	int outh=maxy+12; if(outh>CH)outh=CH;
	FILE*o=fopen("gshot.ppm","wb"); fprintf(o,"P6\n%d %d\n255\n",CW,outh);
	for(int yy=0;yy<outh;yy++) fwrite(canvas[yy],1,CW*3,o);
	fclose(o);
	fprintf(stderr,"title=\"%s\"  text_len=%d  links=%d  imgs=%d  lines=%d  height=%d\n",
		B->doc_title,B->text_len,B->nlinks,B->n_imgs,B->nlines,outh);
	return 0;
}
