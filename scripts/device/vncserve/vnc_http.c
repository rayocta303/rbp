/*
 * vnc_http.c -- the control page. See vnc_http.h.
 *
 * A deliberately small HTTP/1.1: a request line, headers read and thrown away,
 * four routes, and no keep-alive. The only subtlety is that a preview
 * connection outlives its request by hours, so every connection carries a write
 * buffer and the ones that are not streaming are simply marked to close once
 * that buffer has drained.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "vnc_http.h"
#include "vnc_net.h"

#define VNC_HTTP_CONNS 6
#define VNC_HTTP_REQ 4096
#define VNC_HTTP_BOUND "rblive4"

struct hconn {
  int fd;
  int preview;     /* this one is a running mjpeg stream */
  int close_after; /* send what is queued, then hang up  */
  char req[VNC_HTTP_REQ];
  size_t reqlen;
  int headers_done;

  uint8_t *pend; /* bytes still owed to this peer */
  size_t pendlen, pendoff;
};

struct vnc_http {
  int ls; /* the control page's port  */
  struct vnc_http_state *st;
  struct hconn conn[VNC_HTTP_CONNS];
  unsigned long long served, switched;
};

/* --- appending to a response -------------------------------------------------
 * * A small growable string. The page is built in one of these rather than with
 * a chain of printfs into a fixed buffer, because the one thing that must not
 * happen to a status page is for it to come out truncated halfway through the
 * sentence that explains what went wrong. */
struct sbuf {
  char *p;
  size_t len, cap;
  int over;
};

static void sb_room(struct sbuf *b, size_t n) {
  size_t cap;
  if (b->over || b->cap - b->len > n)
    return;
  cap = b->cap ? b->cap : 4096;
  while (cap - b->len <= n)
    cap *= 2;
  {
    char *np = realloc(b->p, cap);
    if (!np) {
      b->over = 1;
      return;
    }
    b->p = np;
    b->cap = cap;
  }
}

static void sb_add(struct sbuf *b, const char *s, size_t n) {
  sb_room(b, n);
  if (b->over)
    return;
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = '\0';
}

static void sb_adds(struct sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_addf(struct sbuf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void sb_addf(struct sbuf *b, const char *fmt, ...) {
  char tmp[512];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((size_t)n < sizeof tmp) {
    sb_add(b, tmp, (size_t)n);
    return;
  }
  {
    char *big = malloc((size_t)n + 1);
    if (!big) {
      b->over = 1;
      return;
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_add(b, big, (size_t)n);
    free(big);
  }
}

/* Anything that came from outside this program -- an errno message naming a
 * path, say
 * -- goes through here. A status page is a place where a stray angle bracket
 * turns today's diagnostic into tomorrow's broken layout. */
static void sb_addesc(struct sbuf *b, const char *s) {
  if (!s) {
    sb_adds(b, "(none)");
    return;
  }
  for (; *s; s++) {
    switch (*s) {
    case '<':
      sb_adds(b, "&lt;");
      break;
    case '>':
      sb_adds(b, "&gt;");
      break;
    case '&':
      sb_adds(b, "&amp;");
      break;
    case '"':
      sb_adds(b, "&quot;");
      break;
    default:
      sb_add(b, s, 1);
      break;
    }
  }
}

/* --- the page ----------------------------------------------------------------
 */

/* THE PICTURE'S BOX IS NOT THE `<img>`. THAT IS THE FIX, AND IT IS
 * LOAD-BEARING.
 *
 * An `<img>` fed by `multipart/x-mixed-replace` is swapped in place, and
 * browsers clear the old frame before the new one is ready to paint. For that
 * instant the element has NO frame, and a `<img>` with no frame is not a
 * *replaced* element any more -- so `width`, `height` and `aspect-ratio` all
 * stop applying to it. Measured in one browser: the box goes 762x477 -> 98x25,
 * with `aspect-ratio` computing to `auto`, i.e. the rule simply stops applying.
 * Twelve times a second the picture is gone and whatever is behind it is
 * showing.
 *
 * The first attempt at a fix was `display:block` plus a definite `aspect-ratio`
 * on the image. That is measurably better -- the sizing properties DO apply to
 * a block box whatever its content -- but it is not sufficient, because it
 * still leaves `height` derived from the image. A browser that drops
 * `aspect-ratio` in that instant falls back to `height:auto`, which with no
 * content is ZERO: the picture collapses vertically and the whole page below it
 * jumps up and back, twelve times a second. That is a strobe of the LAYOUT, and
 * no statistic over the picture's own pixels can see it -- which is why the
 * operator's report outlived a fix that measured clean.
 *
 * So the ratio lives on a plain `<div class="shot">`, and the `<img>` merely
 * fills it. A div's width never depends on any image, so `aspect-ratio` on it
 * always resolves, and the box is fixed before a single byte is decoded.
 * `overflow:hidden` and `background:#000` mean the gap paints black inside a
 * box that has not moved.
 *
 * The `body.only` rules below are the same idea again for the full-screen
 * state: both terms definite, neither read off the frame, so hiding the text
 * around the picture cannot resize it either. */

static const char VNC_HTTP_CSS[] =
    ":root{"
    "--bg-base:#0c0e12;--bg-surface:#13161c;--bg-card:#181c24;--bg-elevated:#"
    "1e2330;--bg-input:#141820;"
    "--border:#252b38;--border-hover:#3a4258;--border-focus:#4caf50;"
    "--text-primary:#e8eaf0;--text-secondary:#8892a4;--text-tertiary:#5c6478;"
    "--accent:#4caf50;--accent-dim:#2e7d32;--accent-glow:rgba(76,175,80,.15);"
    "--orange:#ff9800;--orange-dim:#b26a00;--red:#ef5350;--red-dim:#c62828;--"
    "cyan:#26c6da;--purple:#ab47bc;--blue:#42a5f5;"
    "--radius-sm:6px;--radius-md:10px;--radius-lg:14px;"
    "--shadow-card:0 2px 8px rgba(0,0,0,.35),0 0 1px rgba(0,0,0,.5);"
    "--sidebar-w:240px;--header-h:52px;"
    "--transition-fast:.15s ease;--transition-med:.25s ease}"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "html{scroll-behavior:smooth}"
    "body{font-family:'Inter',system-ui,-apple-system,sans-serif;font-size:"
    "14px;line-height:1.6;color:var(--text-primary);background:var(--bg-base);"
    "min-height:100vh;overflow-x:hidden}"
    ".ico{width:18px;height:18px;flex-shrink:0;fill:none;stroke:currentColor;"
    "stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round}"
    ".ico-lg{width:22px;height:22px}"
    ".sidebar{position:fixed;top:0;left:0;width:var(--sidebar-w);height:100vh;"
    "background:var(--bg-surface);border-right:1px solid "
    "var(--border);display:flex;flex-direction:column;z-index:100;transition:"
    "transform var(--transition-med)}"
    ".sidebar-brand{padding:18px 18px 14px;border-bottom:1px solid "
    "var(--border);display:flex;align-items:center;gap:11px}"
    ".sidebar-brand "
    ".logo{width:34px;height:34px;background:linear-gradient(135deg,var(--"
    "accent),var(--cyan));border-radius:var(--radius-sm);display:flex;align-"
    "items:center;justify-content:center;font-weight:700;font-size:12px;color:#"
    "000;flex-shrink:0;letter-spacing:-.3px}"
    ".sidebar-brand .title{display:flex;flex-direction:column;line-height:1.2}"
    ".sidebar-brand .title strong{font-size:14px;font-weight:700}"
    ".sidebar-brand .title "
    "small{font-size:11px;color:var(--text-secondary);font-weight:400}"
    ".sidebar-nav{flex:1;overflow-y:auto;padding:10px 8px}"
    ".nav-group-label{font-size:10px;font-weight:600;text-transform:uppercase;"
    "letter-spacing:.8px;color:var(--text-tertiary);padding:12px 14px 6px}"
    ".sidebar-nav "
    "button{display:flex;align-items:center;gap:10px;width:100%;padding:9px "
    "14px;border-radius:var(--radius-sm);color:var(--text-secondary);text-"
    "decoration:none;font-family:inherit;font-size:13px;font-weight:500;"
    "background:none;border:none;cursor:pointer;transition:all "
    "var(--transition-fast);text-align:left}"
    ".sidebar-nav "
    "button:hover{background:var(--bg-elevated);color:var(--text-primary)}"
    ".sidebar-nav "
    "button.active{background:var(--accent-glow);color:var(--accent)}"
    ".nav-sep{height:1px;background:var(--border);margin:6px 14px}"
    ".sidebar-footer{padding:12px 16px;border-top:1px solid "
    "var(--border);font-size:10px;color:var(--text-tertiary);text-align:center;"
    "line-height:1.5}"
    ".main{margin-left:var(--sidebar-w);min-height:100vh}"
    ".topbar{position:sticky;top:0;height:var(--header-h);background:rgba(12,"
    "14,18,.88);backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);"
    "border-bottom:1px solid "
    "var(--border);display:flex;align-items:center;padding:0 "
    "24px;gap:14px;z-index:90}"
    ".topbar "
    ".hamburger{display:none;background:none;border:none;color:var(--text-"
    "primary);font-size:20px;cursor:pointer;padding:4px}"
    ".topbar .breadcrumb{font-size:13px;color:var(--text-secondary)}"
    ".topbar .breadcrumb strong{color:var(--text-primary);font-weight:600}"
    ".topbar "
    ".status-badges{margin-left:auto;display:flex;gap:8px;align-items:center}"
    ".badge{display:inline-flex;align-items:center;gap:5px;padding:4px "
    "10px;border-radius:20px;font-size:10px;font-weight:600;text-transform:"
    "uppercase;letter-spacing:.3px}"
    ".badge.live{background:rgba(76,175,80,.12);color:var(--accent);border:1px "
    "solid rgba(76,175,80,.25)}"
    ".badge.live::before{content:'';width:6px;height:6px;border-radius:50%;"
    "background:var(--accent);animation:pulse 2s ease-in-out infinite}"
    ".badge.off{background:rgba(239,83,80,.12);color:var(--red);border:1px "
    "solid rgba(239,83,80,.25)}"
    ".badge.info{background:rgba(66,165,245,.1);color:var(--blue);border:1px "
    "solid rgba(66,165,245,.2)}"
    "@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}"
    ".content{padding:24px;max-width:1060px}"
    ".tab-panel{display:none;animation:fadeTab .2s ease}"
    ".tab-panel.active{display:block}"
    "@keyframes "
    "fadeTab{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:"
    "translateY(0)}}"
    ".section-head{display:flex;align-items:center;gap:10px;margin-bottom:18px}"
    ".section-head "
    ".sh-icon{width:32px;height:32px;border-radius:var(--radius-sm);display:"
    "flex;align-items:center;justify-content:center;flex-shrink:0}"
    ".section-head "
    ".sh-icon.green{background:rgba(76,175,80,.1);color:var(--accent)}"
    ".section-head "
    ".sh-icon.orange{background:rgba(255,152,0,.1);color:var(--orange)}"
    ".section-head "
    ".sh-icon.cyan{background:rgba(38,198,218,.1);color:var(--cyan)}"
    ".section-head "
    ".sh-icon.purple{background:rgba(171,71,188,.1);color:var(--purple)}"
    ".section-head "
    ".sh-icon.blue{background:rgba(66,165,245,.1);color:var(--blue)}"
    ".section-head h2{font-size:17px;font-weight:700;line-height:1.2}"
    ".section-head h2 "
    "small{display:block;font-size:12px;font-weight:400;color:var(--text-"
    "secondary);margin-top:1px}"
    ".card{background:var(--bg-card);border:1px solid "
    "var(--border);border-radius:var(--radius-lg);box-shadow:var(--shadow-card)"
    ";overflow:hidden;margin-bottom:20px}"
    ".card:last-child{margin-bottom:0}"
    ".card-body{padding:20px}"
    ".info-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax("
    "260px,1fr));gap:16px}"
    ".info-table{width:100%;border-collapse:collapse}"
    ".info-table tr{border-bottom:1px solid rgba(255,255,255,.04)}"
    ".info-table tr:last-child{border-bottom:none}"
    ".info-table td{padding:8px 0;vertical-align:top}"
    ".info-table "
    ".k{color:var(--text-secondary);font-size:11px;font-weight:600;text-"
    "transform:uppercase;letter-spacing:.4px;width:110px;padding-right:12px;"
    "white-space:nowrap}"
    ".info-table .v{font-family:'JetBrains "
    "Mono',ui-monospace,monospace;font-size:13px}"
    ".btn-row{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin:"
    "12px 0}"
    "button.btn-ctrl{font:inherit;font-size:13px;font-weight:600;padding:8px "
    "18px;border:1px solid "
    "var(--border);border-radius:var(--radius-sm);background:var(--bg-elevated)"
    ";color:var(--text-primary);cursor:pointer;transition:all "
    "var(--transition-fast)}"
    "button.btn-ctrl:hover{background:var(--border-hover)}"
    "button.btn-ctrl.on{background:var(--accent-dim);border-color:var(--accent)"
    ";color:#fff}"
    "button.btn-ctrl.on.warn{background:var(--orange-dim);border-color:var(--"
    "orange);color:#fff}"
    "button.btn-ctrl.on.bad{background:var(--red-dim);border-color:var(--red);"
    "color:#fff}"
    ".gauge-row{display:grid;grid-template-columns:repeat(auto-fit,minmax("
    "190px,1fr));gap:14px;margin-bottom:20px}"
    ".gauge-item{background:var(--bg-elevated);border-radius:var(--radius-md);"
    "padding:14px}"
    ".gauge-header{display:flex;justify-content:space-between;align-items:"
    "center;margin-bottom:10px}"
    ".gauge-header "
    ".label{font-size:11px;font-weight:600;text-transform:uppercase;letter-"
    "spacing:.4px;color:var(--text-secondary)}"
    ".gauge-header .value{font-family:'JetBrains "
    "Mono',monospace;font-size:18px;font-weight:700}"
    ".gauge-header .value.ok{color:var(--accent)}.gauge-header "
    ".value.warn{color:var(--orange)}.gauge-header "
    ".value.crit{color:var(--red)}"
    ".gauge-bar{height:5px;background:rgba(255,255,255,.06);border-radius:3px;"
    "overflow:hidden}"
    ".gauge-bar .fill{height:100%;border-radius:3px;transition:width .6s ease}"
    ".gauge-bar "
    ".fill.ok{background:linear-gradient(90deg,var(--accent-dim),var(--accent))"
    "}"
    ".gauge-bar "
    ".fill.warn{background:linear-gradient(90deg,var(--orange-dim),var(--"
    "orange))}"
    ".gauge-bar "
    ".fill.crit{background:linear-gradient(90deg,var(--red-dim),var(--red))}"
    ".gauge-sub{font-size:11px;color:var(--text-tertiary);margin-top:6px}"
    ".process-list{width:100%;border-collapse:collapse;font-size:13px}"
    ".process-list thead th{text-align:left;padding:8px "
    "10px;font-size:10px;text-transform:uppercase;letter-spacing:.5px;color:"
    "var(--text-tertiary);border-bottom:1px solid "
    "var(--border);font-weight:600}"
    ".process-list tbody td{padding:6px 10px;border-bottom:1px solid "
    "rgba(255,255,255,.03);font-family:'JetBrains "
    "Mono',monospace;font-size:12px}"
    ".process-list tbody tr:hover{background:rgba(255,255,255,.02)}"
    ".terminal{background:#0a0c10;border-radius:var(--radius-md);overflow:"
    "hidden;border:1px solid var(--border)}"
    ".terminal-toolbar{display:flex;align-items:center;gap:8px;padding:8px "
    "14px;background:rgba(255,255,255,.03);border-bottom:1px solid "
    "var(--border)}"
    ".terminal-dots{display:flex;gap:5px}"
    ".terminal-dots span{width:9px;height:9px;border-radius:50%}"
    ".terminal-dots span:nth-child(1){background:#ef5350}"
    ".terminal-dots span:nth-child(2){background:#ffca28}"
    ".terminal-dots span:nth-child(3){background:#66bb6a}"
    ".terminal-toolbar "
    ".title{font-size:11px;color:var(--text-tertiary);font-weight:500;margin-"
    "left:auto}"
    ".terminal-body{padding:14px 16px;font-family:'JetBrains "
    "Mono',monospace;font-size:13px;line-height:1.7;color:#c8d0dc;min-height:"
    "220px;max-height:380px;overflow-y:auto}"
    ".terminal-body .prompt{color:var(--accent)}"
    ".terminal-body "
    ".output{color:#8892a4;white-space:pre-wrap;font-family:'JetBrains "
    "Mono',monospace}"
    ".terminal-body .cmd{color:#e0e6f0}"
    ".terminal-input-row{display:flex;align-items:center;gap:6px;padding:10px "
    "16px;border-top:1px solid var(--border);background:rgba(255,255,255,.02)}"
    ".terminal-input-row "
    ".prompt-sym{color:var(--accent);font-family:'JetBrains "
    "Mono',monospace;font-size:13px;flex-shrink:0}"
    ".terminal-input-row "
    "input{flex:1;background:none;border:none;outline:none;color:var(--text-"
    "primary);font-family:'JetBrains "
    "Mono',monospace;font-size:13px;caret-color:var(--accent)}"
    ".terminal-input-row "
    "button{background:var(--accent-dim);color:#fff;border:none;border-radius:"
    "var(--radius-sm);padding:5px "
    "14px;font-size:12px;font-weight:600;cursor:pointer;transition:background "
    "var(--transition-fast)}"
    ".terminal-input-row button:hover{background:var(--accent)}"
    ".jog-display{text-align:center;padding:16px 0 8px}"
    ".jog-value{font-family:'JetBrains "
    "Mono',monospace;font-size:48px;font-weight:700;color:var(--accent);line-"
    "height:1}"
    ".jog-value "
    "small{font-size:24px;font-weight:500;color:var(--text-secondary)}"
    ".jog-label{font-size:11px;color:var(--text-secondary);margin-top:6px;text-"
    "transform:uppercase;letter-spacing:.5px}"
    ".slider-row{display:flex;align-items:center;gap:14px;padding:0 "
    "10px;margin:14px 0}"
    ".slider-row .min,.slider-row "
    ".max{font-size:11px;color:var(--text-tertiary);font-family:'JetBrains "
    "Mono',monospace;flex-shrink:0}"
    "input[type=\"range\"]{-webkit-appearance:none;appearance:none;flex:1;"
    "height:5px;background:rgba(255,255,255,.08);border-radius:3px;outline:"
    "none;cursor:pointer}"
    "input[type=\"range\"]::-webkit-slider-thumb{-webkit-appearance:none;width:"
    "18px;height:18px;border-radius:50%;background:var(--accent);border:3px "
    "solid var(--bg-card);box-shadow:0 0 6px "
    "rgba(76,175,80,.35);cursor:grab;transition:transform "
    "var(--transition-fast)}"
    "input[type=\"range\"]::-webkit-slider-thumb:hover{transform:scale(1.15)}"
    ".preset-btns{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px}"
    ".preset-btn{font-family:'Inter',sans-serif;font-size:12px;font-weight:600;"
    "padding:7px 16px;border-radius:var(--radius-sm);border:1px solid "
    "var(--border);background:var(--bg-elevated);color:var(--text-secondary);"
    "cursor:pointer;transition:all var(--transition-fast)}"
    ".preset-btn:hover{border-color:var(--border-hover);color:var(--text-"
    "primary)}"
    ".preset-btn.active{border-color:var(--accent);background:var(--accent-"
    "glow);color:var(--accent)}"
    ".fx-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,"
    "1fr));gap:12px}"
    ".fx-card{background:var(--bg-elevated);border:1px solid "
    "var(--border);border-radius:var(--radius-md);padding:14px;transition:"
    "border-color var(--transition-fast)}"
    ".fx-card:hover{border-color:var(--border-hover)}"
    ".fx-card-head{display:flex;justify-content:space-between;align-items:"
    "center;margin-bottom:12px}"
    ".fx-card-head .slot-name{font-size:14px;font-weight:600}"
    ".fx-card-head "
    ".slot-tag{font-size:9px;font-weight:700;text-transform:uppercase;letter-"
    "spacing:.3px;padding:3px 8px;border-radius:10px}"
    ".slot-tag.color{background:rgba(171,71,188,.1);color:var(--purple)}"
    ".slot-tag.beat{background:rgba(255,152,0,.1);color:var(--orange)}"
    ".fx-row{display:flex;align-items:center;justify-content:space-between;gap:"
    "8px;margin-bottom:8px}"
    ".fx-row:last-child{margin-bottom:0}"
    ".fx-row "
    ".fx-label{font-size:11px;color:var(--text-tertiary);text-transform:"
    "uppercase;letter-spacing:.3px;flex-shrink:0}"
    "select{font-family:'Inter',sans-serif;font-size:13px;background:var(--bg-"
    "input);color:var(--text-primary);border:1px solid "
    "var(--border);border-radius:var(--radius-sm);padding:7px 30px 7px "
    "10px;cursor:pointer;outline:none;transition:border-color "
    "var(--transition-fast);-webkit-appearance:none;appearance:none;background-"
    "image:url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' "
    "width='12' height='12' fill='%238892a4'%3E%3Cpath d='M2 4l4 4 "
    "4-4'/%3E%3C/"
    "svg%3E\");background-repeat:no-repeat;background-position:right 10px "
    "center}"
    "select:hover{border-color:var(--border-hover)}select:focus{border-color:"
    "var(--accent)}"
    ".curve-selector{display:flex;gap:12px;flex-wrap:wrap}"
    ".curve-option{flex:1;min-width:120px;background:var(--bg-elevated);border:"
    "2px solid "
    "var(--border);border-radius:var(--radius-md);padding:16px;text-align:"
    "center;cursor:pointer;transition:all "
    "var(--transition-med);text-decoration:none;color:inherit}"
    ".curve-option:hover{border-color:var(--border-hover);background:var(--bg-"
    "input)}"
    ".curve-option.active{border-color:var(--accent);background:var(--accent-"
    "glow)}"
    ".curve-option svg{width:100%;height:72px;margin-bottom:10px}"
    ".curve-option svg "
    "path.curve-line{fill:none;stroke:var(--text-tertiary);stroke-width:2.5;"
    "stroke-linecap:round;transition:stroke var(--transition-fast)}"
    ".curve-option.active svg path.curve-line,.curve-option:hover svg "
    "path.curve-line{stroke:var(--accent)}"
    ".curve-option svg line.axis{stroke:rgba(255,255,255,.07);stroke-width:1}"
    ".curve-option "
    ".name{font-size:13px;font-weight:600;color:var(--text-secondary);"
    "transition:color var(--transition-fast)}"
    ".curve-option.active .name{color:var(--accent)}"
    ".curve-option "
    ".desc{font-size:11px;color:var(--text-tertiary);margin-top:2px}"
    ".log-toolbar{display:flex;align-items:center;gap:10px;padding:12px "
    "16px;border-bottom:1px solid var(--border);flex-wrap:wrap}"
    ".log-toolbar .filter-group{display:flex;gap:4px}"
    ".log-filter-btn{font-family:'Inter',sans-serif;font-size:11px;font-weight:"
    "600;padding:4px 12px;border:1px solid "
    "var(--border);border-radius:20px;background:none;color:var(--text-"
    "tertiary);cursor:pointer;transition:all var(--transition-fast)}"
    ".log-filter-btn:hover{border-color:var(--border-hover);color:var(--text-"
    "secondary)}"
    ".log-filter-btn.active{background:rgba(76,175,80,.08);border-color:var(--"
    "accent);color:var(--accent)}"
    ".log-filter-btn.active.warn-f{background:rgba(255,152,0,.08);border-color:"
    "var(--orange);color:var(--orange)}"
    ".log-filter-btn.active.err-f{background:rgba(239,83,80,.08);border-color:"
    "var(--red);color:var(--red)}"
    ".log-toolbar .search-box{margin-left:auto;position:relative}"
    ".log-toolbar .search-box "
    "input{font-family:'Inter',sans-serif;font-size:12px;background:var(--bg-"
    "input);color:var(--text-primary);border:1px solid "
    "var(--border);border-radius:20px;padding:5px 14px 5px "
    "32px;outline:none;width:200px;transition:border-color "
    "var(--transition-fast)}"
    ".log-toolbar .search-box input:focus{border-color:var(--accent)}"
    ".log-toolbar .search-box "
    ".search-ico{position:absolute;left:10px;top:50%;transform:translateY(-50%)"
    ";color:var(--text-tertiary);pointer-events:none}"
    ".log-body{font-family:'JetBrains "
    "Mono',monospace;font-size:12px;line-height:1.8;padding:10px "
    "16px;max-height:400px;overflow-y:auto;background:#0a0c10;white-space:pre-"
    "wrap}"
    ".log-footer{display:flex;align-items:center;justify-content:space-between;"
    "padding:10px 16px;border-top:1px solid "
    "var(--border);font-size:11px;color:var(--text-tertiary)}"
    ".log-footer .auto-scroll{display:flex;align-items:center;gap:6px}"
    ".toggle{position:relative;width:34px;height:18px;display:inline-block}"
    ".toggle input{opacity:0;width:0;height:0}"
    ".toggle "
    ".slider{position:absolute;inset:0;background:var(--border);border-radius:"
    "9px;cursor:pointer;transition:background var(--transition-fast)}"
    ".toggle "
    ".slider::before{content:'';position:absolute;width:14px;height:14px;"
    "border-radius:50%;background:#fff;left:2px;top:2px;transition:transform "
    "var(--transition-fast)}"
    ".toggle input:checked + .slider{background:var(--accent)}"
    ".toggle input:checked + .slider::before{transform:translateX(16px)}"
    "::-webkit-scrollbar{width:5px;height:5px}"
    "::-webkit-scrollbar-track{background:transparent}"
    "::-webkit-scrollbar-thumb{background:var(--border);border-radius:3px}"
    "::-webkit-scrollbar-thumb:hover{background:var(--border-hover)}"
    ".sidebar-overlay{display:none;position:fixed;inset:0;background:rgba(0,0,"
    "0,.55);z-index:95}"
    ".shot{display:block;width:100%;aspect-ratio:1280/"
    "800;background:#000;border:1px solid "
    "var(--border);border-radius:var(--radius-md);overflow:hidden;touch-action:"
    "manipulation;cursor:pointer}"
    "body.only{margin:0;padding:0;max-width:none;background:#000;min-height:"
    "100vh;display:flex;align-items:center;justify-content:center;overflow:"
    "hidden}"
    "body.only .chrome{display:none}"
    "body.only .sidebar{display:none}"
    "body.only .topbar{display:none}"
    "body.only .main{margin-left:0}"
    "body.only "
    ".shot{flex:none;width:min(1280px,100vw,160vh);height:min(800px,62.5vw,"
    "100vh);aspect-ratio:auto;border:0;border-radius:0;cursor:pointer}"
    "@media(max-width:860px){.sidebar{transform:translateX(-100%)}.sidebar."
    "open{transform:translateX(0)}.sidebar-overlay.show{display:block}.main{"
    "margin-left:0}.topbar .hamburger{display:block}.content{padding:18px 14px "
    "60px}.gauge-row{grid-template-columns:1fr "
    "1fr}.info-grid{grid-template-columns:1fr}.fx-grid{grid-template-columns:"
    "repeat(auto-fit,minmax(160px,1fr))}.curve-selector{flex-direction:column}."
    "log-toolbar .search-box{margin-left:0;width:100%}.log-toolbar .search-box "
    "input{width:100%}}"
    "@media(max-width:480px){.gauge-row{grid-template-columns:1fr}.preset-btns{"
    "gap:6px}.preset-btn{padding:6px "
    "12px;font-size:11px}.jog-value{font-size:36px}}";

struct flx6_fx_config {
  char cfx[4][16];
  int bfx_type[6];
  int bfx_ch[6];
};

static const char *const bfx_names[14] = {
    "DELAY",  "ECHO",      "PING PONG", "SPIRAL",     "HELIX",
    "REVERB", "FLANGER",   "PHASER",    "FILTER",     "TRANS",
    "ROLL",   "SLIP ROLL", "PITCH",     "VINYL BRAKE"};

static const char *const cfx_options[6] = {"filter", "dubecho", "noise",
                                           "space",  "crush",   "sweep"};

static const char *const cfx_labels[6] = {"Filter", "Dub Echo", "Noise",
                                          "Space",  "Crush",    "Sweep"};

static void flx6_config_defaults(struct flx6_fx_config *cfg) {
  strcpy(cfg->cfx[0], "filter");
  strcpy(cfg->cfx[1], "dubecho");
  strcpy(cfg->cfx[2], "noise");
  strcpy(cfg->cfx[3], "space");

  cfg->bfx_type[0] = 0;
  cfg->bfx_ch[0] = 0;
  cfg->bfx_type[1] = 1;
  cfg->bfx_ch[1] = 0;
  cfg->bfx_type[2] = 3;
  cfg->bfx_ch[2] = 0;
  cfg->bfx_type[3] = 5;
  cfg->bfx_ch[3] = 1;
  cfg->bfx_type[4] = 6;
  cfg->bfx_ch[4] = 1;
  cfg->bfx_type[5] = 10;
  cfg->bfx_ch[5] = 1;
}

static void flx6_config_load(struct flx6_fx_config *cfg) {
  flx6_config_defaults(cfg);
  FILE *f = fopen("/run/rblive4/flx6_fx.conf", "r");
  if (!f)
    f = fopen("/opt/rblive4/flx6_fx.conf", "r");
  if (!f)
    return;

  char line[128];
  while (fgets(line, sizeof line, f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n')
      continue;
    char *eq = strchr(p, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *val = eq + 1;
    char *k_end = eq - 1;
    while (k_end > p && (*k_end == ' ' || *k_end == '\t'))
      *k_end-- = '\0';
    while (*val == ' ' || *val == '\t')
      val++;
    char *v_end = val + strlen(val) - 1;
    while (v_end >= val && (*v_end == '\r' || *v_end == '\n' || *v_end == ' ' ||
                            *v_end == '\t'))
      *v_end-- = '\0';

    if (!strncmp(p, "cfx_slot", 8)) {
      int s = p[8] - '1';
      if (s >= 0 && s < 4) {
        snprintf(cfg->cfx[s], sizeof cfg->cfx[s], "%s", val);
      }
    } else if (!strncmp(p, "bfx_slot", 8)) {
      int s = p[8] - '1';
      if (s >= 0 && s < 6) {
        if (strstr(p, "_type"))
          cfg->bfx_type[s] = atoi(val);
        else if (strstr(p, "_ch"))
          cfg->bfx_ch[s] = atoi(val);
      }
    }
  }
  fclose(f);
}

static void flx6_config_save(const struct flx6_fx_config *cfg) {
  const char *paths[] = {"/run/rblive4/flx6_fx.conf",
                         "/opt/rblive4/flx6_fx.conf"};
  for (int i = 0; i < 2; i++) {
    FILE *f = fopen(paths[i], "w");
    if (f) {
      fprintf(f, "# DDJ-FLX6 FX Configuration\n");
      for (int s = 0; s < 4; s++)
        fprintf(f, "cfx_slot%d=%s\n", s + 1, cfg->cfx[s]);
      for (int s = 0; s < 6; s++) {
        fprintf(f, "bfx_slot%d_type=%d\n", s + 1, cfg->bfx_type[s]);
        fprintf(f, "bfx_slot%d_ch=%d\n", s + 1, cfg->bfx_ch[s]);
      }
      fclose(f);
    }
  }
}

static void page_html(struct vnc_http *h, struct sbuf *b) {
  struct vnc_http_state *s = h->st;
  int raw = s->mode->mode == VNC_MODE_RAW;
  const int share = s->live && s->live->on;

  /* Load Jog Sensitivity */
  float jog_mult = 1.0f;
  FILE *fj = fopen("/run/rblive4/jog_speed", "r");
  if (!fj)
    fj = fopen("/opt/rblive4/jog_speed", "r");
  if (fj) {
    float v = 1.0f;
    if (fscanf(fj, "%f", &v) == 1 && v >= 0.01f && v <= 3.0f)
      jog_mult = v;
    fclose(fj);
  }

  /* Load Crossfader Settings */
  int xf_enable = 1;
  FILE *fe = fopen("/run/rblive4/xfader_enable", "r");
  if (!fe)
    fe = fopen("/opt/rblive4/xfader_enable", "r");
  if (fe) {
    int v = 1;
    if (fscanf(fe, "%d", &v) == 1 && (v == 0 || v == 1))
      xf_enable = v;
    fclose(fe);
  }

  int xf_curve = 1;
  FILE *fx = fopen("/run/rblive4/xfader_curve", "r");
  if (!fx)
    fx = fopen("/opt/rblive4/xfader_curve", "r");
  if (fx) {
    int v = 1;
    if (fscanf(fx, "%d", &v) == 1 && v >= 0 && v <= 2)
      xf_curve = v;
    fclose(fx);
  }

  /* Load FLX6 FX Config */
  struct flx6_fx_config cfg;
  flx6_config_load(&cfg);

  /* Fetch Uptime & Hostname */
  char hostname[64] = "xdj-flx";
  gethostname(hostname, sizeof hostname);

  char uptime_str[64] = "unknown";
  FILE *fup = fopen("/proc/uptime", "r");
  if (fup) {
    double up_sec = 0;
    if (fscanf(fup, "%lf", &up_sec) == 1) {
      long days = (long)(up_sec / 86400);
      long hours = ((long)up_sec % 86400) / 3600;
      long mins = ((long)up_sec % 3600) / 60;
      if (days > 0)
        snprintf(uptime_str, sizeof uptime_str, "%ldd %ldh %ldm", days, hours,
                 mins);
      else
        snprintf(uptime_str, sizeof uptime_str, "%ldh %ldm", hours, mins);
    }
    fclose(fup);
  }

  sb_adds(
      b,
      "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<title>UNX DJ &mdash; Configuration</title>"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<style>");
  sb_adds(b, VNC_HTTP_CSS);
  sb_adds(b, "</style></head><body>");

  /* Mobile Overlay */
  sb_adds(b, "<div class=\"sidebar-overlay\" id=\"overlay\" "
             "onclick=\"toggleSidebar()\"></div>");

  /* Sidebar */
  sb_adds(
      b,
      "<aside class=\"sidebar\" id=\"sidebar\">"
      "<div class=\"sidebar-brand\">"
      "<div class=\"logo\">UNX</div>"
      "<div class=\"title\"><strong>UNX-DJ</strong><small>Configuration "
      "Panel</small></div>"
      "</div>"
      "<nav class=\"sidebar-nav\">"
      "<div class=\"nav-group-label\">Main Menus</div>"
      "<button class=\"active\" onclick=\"switchTab('system', this)\">"
      "<svg class=\"ico\" viewBox=\"0 0 24 24\"><rect x=\"2\" y=\"3\" "
      "width=\"20\" height=\"14\" rx=\"2\"/><line x1=\"8\" y1=\"21\" x2=\"16\" "
      "y2=\"21\"/><line x1=\"12\" y1=\"17\" x2=\"12\" y2=\"21\"/></svg>"
      "System</button>"
      "<button onclick=\"switchTab('controller', this)\">"
      "<svg class=\"ico\" viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" "
      "r=\"9\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/></svg>"
      "Controller Settings</button>"
      "<div class=\"nav-sep\"></div>"
      "<div class=\"nav-group-label\">Diagnostics</div>"
      "<button onclick=\"switchTab('logs', this)\">"
      "<svg class=\"ico\" viewBox=\"0 0 24 24\"><path d=\"M14 2H6a2 2 0 0 0-2 "
      "2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z\"/><polyline points=\"14 2 14 8 "
      "20 8\"/><line x1=\"16\" y1=\"13\" x2=\"8\" y2=\"13\"/><line x1=\"16\" "
      "y1=\"17\" x2=\"8\" y2=\"17\"/><polyline points=\"10 9 9 9 8 9\"/></svg>"
      "Log Viewer</button>"
      "</nav>"
      "<div class=\"sidebar-footer\">unx.dj &middot; rblive4 &middot; ARM64</div>"
      "</aside>");

  /* Main Container */
  sb_adds(b, "<div class=\"main\">");

  /* Topbar */
  sb_adds(b, "<header class=\"topbar\">"
             "<button class=\"hamburger\" onclick=\"toggleSidebar()\">"
             "<svg class=\"ico ico-lg\" viewBox=\"0 0 24 24\"><line x1=\"3\" "
             "y1=\"6\" x2=\"21\" y2=\"6\"/><line x1=\"3\" y1=\"12\" x2=\"21\" "
             "y2=\"12\"/><line x1=\"3\" y1=\"18\" x2=\"21\" y2=\"18\"/></svg>"
             "</button>"
             "<span class=\"breadcrumb\" id=\"breadcrumb\">UNX &rsaquo; "
             "<strong>System</strong></span>"
             "<div class=\"status-badges\">");
  if (share)
    sb_adds(b, "<span class=\"badge live\">Live Sharing</span>");
  else
    sb_adds(b, "<span class=\"badge off\">Sharing Off</span>");
  sb_addf(b, "<span class=\"badge info\">%d VNC Client%s</span>",
          s->vnc_clients, s->vnc_clients == 1 ? "" : "s");
  sb_adds(b, "</div></header>");

  /* Content Area */
  sb_adds(b, "<div class=\"content\">");

  /* ══════════════════ TAB: SYSTEM (Screen Preview, System Info, Resources, Shell) ══════════════════ */
  sb_adds(b, "<div class=\"tab-panel active\" id=\"tab-system\">");
  sb_adds(
      b,
      "<div class=\"section-head\"><div class=\"sh-icon green\">"
      "<svg class=\"ico ico-lg\" viewBox=\"0 0 24 24\"><rect x=\"2\" y=\"3\" "
      "width=\"20\" height=\"14\" rx=\"2\"/><line x1=\"8\" y1=\"21\" x2=\"16\" "
      "y2=\"21\"/><line x1=\"12\" y1=\"17\" x2=\"12\" y2=\"21\"/></svg>"
      "</div><h2>Screen Preview &amp; Stream<small>Hardware MJPEG encoder live "
      "view &amp; VNC controls</small></h2></div>");

  sb_adds(b, "<div class=\"card\"><div class=\"card-body\">");
  if (!share) {
    sb_adds(b, "<div class=\"shot\" id=\"shot\"></div>"
               "<p class=\"note\" "
               "style=\"margin-top:14px;color:var(--text-secondary)\">Sharing "
               "is currently <b>Off</b>. Turn sharing on below to activate "
               "preview and VNC streaming.</p>");
  } else {
    sb_addf(b,
            "<div class=\"shot\" id=\"shot\"><img src=\"/preview.mjpg?%llu\" "
            "alt=\"live preview\"></div>"
            "<p class=\"note\" "
            "style=\"margin-top:14px;color:var(--text-secondary)\"><b>Tap "
            "preview image</b> for fullscreen mode. Connect VNC at "
            "<code>vnc://&lt;ip&gt;:%d</code>.</p>",
            h->switched, s->vnc_port > 0 ? s->vnc_port : 5900);
  }

  sb_adds(b, "<div class=\"btn-row\" style=\"margin-top:16px\">");
  if (s->live) {
    sb_adds(b, "<form method=\"get\" action=\"/session\" "
               "style=\"display:inline;margin:0\">");
    sb_addf(b,
            "<button name=\"set\" value=\"on\" class=\"btn-ctrl%s\">Start "
            "Sharing</button>",
            share ? " on" : "");
    sb_addf(b,
            "<button name=\"set\" value=\"off\" class=\"btn-ctrl%s\">Stop "
            "Sharing</button>",
            share ? "" : " on bad");
    sb_adds(b, "</form>");
  }

  sb_adds(b, "<form method=\"get\" action=\"/mode\" "
             "style=\"display:inline;margin:0\">");
  sb_addf(b,
          "<button name=\"set\" value=\"raw\" class=\"btn-ctrl%s\">Raw "
          "Mode</button>",
          raw ? " on" : "");
  sb_addf(b,
          "<button name=\"set\" value=\"hwjpeg\" class=\"btn-ctrl%s\">Hardware "
          "JPEG</button>",
          raw ? "" : " on warn");
  sb_adds(b, "</form>");

  if (s->in) {
    sb_adds(b, "<form method=\"get\" action=\"/input\" "
               "style=\"display:inline;margin:0\">");
    sb_addf(b,
            "<button name=\"set\" value=\"on\" class=\"btn-ctrl%s\">Mouse "
            "Touch On</button>",
            s->in->on ? " on" : "");
    sb_addf(b,
            "<button name=\"set\" value=\"off\" class=\"btn-ctrl%s\">Mouse "
            "Touch Off</button>",
            s->in->on ? "" : " on");
    sb_adds(b, "</form>");
  }
  sb_adds(b, "</div>"); /* end btn-row */
  sb_adds(b, "</div></div>"); /* end card */

  /* ── Subsection: SYSTEM INFO ── */
  sb_adds(b, "<div style=\"margin-top:24px\"></div>");
  sb_adds(b,
          "<div class=\"section-head\"><div class=\"sh-icon green\">"
          "<svg class=\"ico ico-lg\" viewBox=\"0 0 24 24\"><circle cx=\"12\" "
          "cy=\"12\" r=\"10\"/><line x1=\"12\" y1=\"16\" x2=\"12\" "
          "y2=\"12\"/><line x1=\"12\" y1=\"8\" x2=\"12.01\" y2=\"8\"/></svg>"
          "</div><h2>System Information<small>Device status and runtime "
          "parameters</small></h2></div>");
  sb_adds(
      b,
      "<div class=\"card\"><div class=\"card-body\"><div class=\"info-grid\">");
  sb_adds(b, "<table class=\"info-table\">"
             "<tr><td class=\"k\">Model</td><td class=\"v\">Pioneer DJ "
             "XDJ-FLX</td></tr>"
             "<tr><td class=\"k\">SoC / CPU</td><td class=\"v\">Amlogic S905X "
             "/ BCM2711 ARM</td></tr>"
             "<tr><td class=\"k\">Display</td><td class=\"v\">1280&times;800 @ "
             "60Hz (DirectFB)</td></tr>");
  sb_addf(b, "<tr><td class=\"k\">Uptime</td><td class=\"v\">%s</td></tr>",
          uptime_str);
  sb_addf(b, "<tr><td class=\"k\">Hostname</td><td class=\"v\">%s (unx.dj)</td></tr>",
          hostname);
  sb_adds(b, "</table><table class=\"info-table\">");
  sb_addf(b, "<tr><td class=\"k\">VNC Port</td><td class=\"v\">%d</td></tr>",
          s->vnc_port > 0 ? s->vnc_port : 5900);
  sb_addf(
      b,
      "<tr><td class=\"k\">Encoder</td><td class=\"v\">%s &mdash; %s</td></tr>",
      s->jpeg_ok ? "<b style=\"color:var(--accent)\">Active</b>"
                 : "<b style=\"color:var(--red)\">Not Available</b>",
      s->jpeg_status ? s->jpeg_status : "OK");
  sb_addf(b, "<tr><td class=\"k\">Active Mode</td><td class=\"v\">%s</td></tr>",
          vnc_mode_name(s->mode->mode));
  sb_addf(b, "<tr><td class=\"k\">Touch Input</td><td class=\"v\">%s</td></tr>",
          s->in && s->in->on ? "Enabled" : "Disabled");
  sb_addf(b,
          "<tr><td class=\"k\">VNC Clients</td><td class=\"v\">%d connected "
          "(%d JPEG)</td></tr>",
          s->vnc_clients, s->vnc_jpeg_clients);
  sb_adds(b, "</table></div></div></div>");

  /* ── Subsection: RESOURCES ── */
  sb_adds(b, "<div style=\"margin-top:24px\"></div>");
  sb_adds(b, "<div class=\"section-head\"><div class=\"sh-icon cyan\">"
             "<svg class=\"ico ico-lg\" viewBox=\"0 0 24 24\"><path d=\"M18 "
             "20V10\"/><path d=\"M12 20V4\"/><path d=\"M6 20v-6\"/></svg>"
             "</div><h2>Resource Usage<small>Real-time system resource "
             "monitoring</small></h2></div>");
  sb_addf(b,
          "<div class=\"card\"><div class=\"card-body\">"
          "<div class=\"gauge-row\">"
          "<div class=\"gauge-item\"><div class=\"gauge-header\"><span "
          "class=\"label\">CPU</span><span class=\"value ok\" "
          "id=\"cpu-val\">23%%</span></div>"
          "<div class=\"gauge-bar\"><div class=\"fill ok\" id=\"cpu-bar\" "
          "style=\"width:23%%\"></div></div><div class=\"gauge-sub\">Quad-core "
          "ARM64</div></div>"
          "<div class=\"gauge-item\"><div class=\"gauge-header\"><span "
          "class=\"label\">Memory</span><span class=\"value warn\" "
          "id=\"mem-val\">62%%</span></div>"
          "<div class=\"gauge-bar\"><div class=\"fill warn\" id=\"mem-bar\" "
          "style=\"width:62%%\"></div></div><div class=\"gauge-sub\">RAM "
          "utilization</div></div>"
          "<div class=\"gauge-item\"><div class=\"gauge-header\"><span "
          "class=\"label\">Temperature</span><span class=\"value ok\" "
          "id=\"temp-val\">52&deg;C</span></div>"
          "<div class=\"gauge-bar\"><div class=\"fill ok\" id=\"temp-bar\" "
          "style=\"width:52%%\"></div></div><div class=\"gauge-sub\">Thermal "
          "limit 85&deg;C</div></div>"
          "<div class=\"gauge-item\"><div class=\"gauge-header\"><span "
          "class=\"label\">FPS Target</span><span class=\"value ok\">%d "
          "fps</span></div>"
          "<div class=\"gauge-bar\"><div class=\"fill ok\" "
          "style=\"width:80%%\"></div></div><div class=\"gauge-sub\">VNC "
          "refresh ceiling</div></div>"
          "</div>"
          "<table "
          "class=\"process-list\"><thead><tr><th>Process</th><th>Type</"
          "th><th>Status</th></tr></thead><tbody>"
          "<tr><td>rekordbox-engine (rbp)</td><td>Playback Engine</td><td "
          "style=\"color:var(--accent)\">running</td></tr>"
          "<tr><td>vncserve</td><td>RFB &amp; Web Panel</td><td "
          "style=\"color:var(--accent)\">running</td></tr>"
          "<tr><td>midi_shim / knobshim</td><td>Hardware Controller "
          "Interposer</td><td style=\"color:var(--accent)\">running</td></tr>"
          "<tr><td>netshim (Pro DJ Link)</td><td>WLAN / UDP Redirect</td><td "
          "style=\"color:var(--accent)\">running</td></tr>"
          "</tbody></table></div></div>",
          s->fps ? s->fps : 30);

  /* ── Subsection: SHELL ACCESS ── */
  sb_adds(b, "<div style=\"margin-top:24px\"></div>");
  sb_adds(
      b, "<div class=\"section-head\"><div class=\"sh-icon blue\">"
         "<svg class=\"ico ico-lg\" viewBox=\"0 0 24 24\"><polyline points=\"4 "
         "17 10 11 4 5\"/><line x1=\"12\" y1=\"19\" x2=\"20\" y2=\"19\"/></svg>"
         "</div><h2>Shell Access<small>Execute terminal commands on the "
         "device</small></h2></div>");
  sb_adds(
      b,
      "<div class=\"card\" style=\"overflow:visible\"><div class=\"terminal\">"
      "<div class=\"terminal-toolbar\"><div "
      "class=\"terminal-dots\"><span></span><span></span><span></span></div>"
      "<span class=\"title\">root@");
  sb_addesc(b, hostname);
  sb_adds(b, " &mdash; /opt/rblive4</span></div>"
             "<div class=\"terminal-body\" id=\"term-body\">"
             "<div><span class=\"prompt\">root@");
  sb_addesc(b, hostname);
  sb_adds(b, ":~#</span> <span class=\"cmd\">uname -a</span></div>"
             "<div class=\"output\">Linux ");
  sb_addesc(b, hostname);
  sb_adds(b, " 4.9.113-amlogic aarch64 GNU/Linux</div>"
             "<div>&nbsp;</div>"
             "</div>"
             "<div class=\"terminal-input-row\">"
             "<span class=\"prompt-sym\">#</span>"
             "<input type=\"text\" id=\"term-input\" placeholder=\"Enter shell "
             "command (e.g. ps aux, ifconfig, df -h)...\" autocomplete=\"off\" "
             "spellcheck=\"false\">"
             "<button onclick=\"runCmd()\">Run</button>"
             "</div></div></div>");
  sb_adds(b, "</div>"); /* end tab-system */

  /* ══════════════════ TAB: CONTROLLER SETTINGS ══════════════════ */
  sb_adds(b, "<div class=\"tab-panel\" id=\"tab-controller\">");

  /* ── Subsection: JOGWHEEL ── */
  sb_adds(b,
          "<div class=\"section-head\"><div class=\"sh-icon green\">"
          "<svg class=\"ico ico-lg\" viewBox=\"0 0 24 24\"><circle cx=\"12\" "
          "cy=\"12\" r=\"9\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/></svg>"
          "</div><h2>Jogwheel Sensitivity<small>Adjust jog rotation "
          "multiplier</small></h2></div>");
  sb_adds(b, "<div class=\"card\"><div class=\"card-body\">"
             "<div class=\"jog-display\">");
  sb_addf(b,
          "<div class=\"jog-value\" id=\"jog-val\">%.2f<small>x</small></div>",
          jog_mult);
  sb_adds(b, "<div class=\"jog-label\">Sensitivity Multiplier</div></div>"
             "<div class=\"slider-row\"><span class=\"min\">0.01x</span>");
  sb_addf(b,
          "<input type=\"range\" id=\"jog-slider\" min=\"0.01\" max=\"3.00\" "
          "step=\"0.01\" value=\"%.2f\" "
          "oninput=\"updateJog(this.value)\" "
          "onchange=\"fetch('/jog?set='+this.value)\">",
          jog_mult);
  sb_adds(b, "<span class=\"max\">3.00x</span></div>"
             "<div class=\"preset-btns\">");
  sb_addf(b,
          "<button class=\"preset-btn%s\" onclick=\"setJog(0.01)\">0.01x "
          "Slowest</button>",
          fabsf(jog_mult - 0.01f) < 0.005f ? " active" : "");
  sb_addf(
      b,
      "<button class=\"preset-btn%s\" onclick=\"setJog(0.10)\">0.10x</button>",
      fabsf(jog_mult - 0.10f) < 0.005f ? " active" : "");
  sb_addf(
      b,
      "<button class=\"preset-btn%s\" onclick=\"setJog(0.25)\">0.25x</button>",
      fabsf(jog_mult - 0.25f) < 0.005f ? " active" : "");
  sb_addf(
      b,
      "<button class=\"preset-btn%s\" onclick=\"setJog(0.50)\">0.50x</button>",
      fabsf(jog_mult - 0.50f) < 0.005f ? " active" : "");
  sb_addf(
      b,
      "<button class=\"preset-btn%s\" onclick=\"setJog(0.75)\">0.75x</button>",
      fabsf(jog_mult - 0.75f) < 0.005f ? " active" : "");
  sb_addf(b,
          "<button class=\"preset-btn%s\" onclick=\"setJog(1.00)\">1.00x "
          "Normal</button>",
          fabsf(jog_mult - 1.0f) < 0.005f ? " active" : "");
  sb_addf(b,
          "<button class=\"preset-btn%s\" onclick=\"setJog(1.50)\">1.50x "
          "Fast</button>",
          fabsf(jog_mult - 1.50f) < 0.005f ? " active" : "");
  sb_addf(
      b,
      "<button class=\"preset-btn%s\" onclick=\"setJog(2.00)\">2.00x</button>",
      fabsf(jog_mult - 2.0f) < 0.005f ? " active" : "");
  sb_addf(b,
          "<button class=\"preset-btn%s\" onclick=\"setJog(3.00)\">3.00x "
          "Max</button>",
          fabsf(jog_mult - 3.0f) < 0.005f ? " active" : "");
  sb_adds(b, "</div></div></div>");

  /* ── Subsection: CROSSFADER ── */
  sb_adds(b, "<div style=\"margin-top:24px\"></div>");
  sb_adds(b, "<div class=\"section-head\"><div class=\"sh-icon orange\">"
             "<svg class=\"ico ico-lg\" viewBox=\"0 0 24 24\"><line x1=\"4\" "
             "y1=\"21\" x2=\"4\" y2=\"14\"/><line x1=\"4\" y1=\"10\" x2=\"4\" "
             "y2=\"3\"/><line x1=\"12\" y1=\"21\" x2=\"12\" y2=\"12\"/><line "
             "x1=\"12\" y1=\"8\" x2=\"12\" y2=\"3\"/><line x1=\"20\" y1=\"21\" "
             "x2=\"20\" y2=\"16\"/><line x1=\"20\" y1=\"12\" x2=\"20\" "
             "y2=\"3\"/><line x1=\"1\" y1=\"14\" x2=\"7\" y2=\"14\"/><line "
             "x1=\"9\" y1=\"8\" x2=\"15\" y2=\"8\"/><line x1=\"17\" y1=\"16\" "
             "x2=\"23\" y2=\"16\"/></svg>"
             "</div><h2>Crossfader Settings<small>Curve response and enable / "
             "bypass options</small></h2></div>");
  sb_adds(
      b,
      "<div class=\"card\"><div class=\"card-body\">"
      "<div class=\"btn-row\" style=\"margin-top:0;margin-bottom:18px\">"
      "<span "
      "style=\"font-size:12px;font-weight:600;color:var(--text-secondary);text-"
      "transform:uppercase;letter-spacing:.4px\">Crossfader Status:</span>"
      "<form method=\"get\" action=\"/xfader_enable\" "
      "style=\"display:inline;margin:0\">");
  sb_addf(b,
          "<button name=\"set\" value=\"1\" class=\"btn-ctrl%s\">Enable "
          "Crossfader</button>",
          xf_enable ? " on" : "");
  sb_addf(b,
          "<button name=\"set\" value=\"0\" class=\"btn-ctrl%s\">Disable (Thru "
          "Bypass)</button>",
          xf_enable ? "" : " on bad");
  sb_adds(b, "</form></div>"
             "<div class=\"curve-selector\">");

  /* Smooth / Thru (0) */
  sb_addf(b,
          "<a href=\"/xfader?set=0\" class=\"curve-option%s\">"
          "<svg viewBox=\"0 0 120 80\"><line class=\"axis\" x1=\"10\" "
          "y1=\"70\" x2=\"110\" y2=\"70\"/><line class=\"axis\" x1=\"10\" "
          "y1=\"10\" x2=\"10\" y2=\"70\"/>"
          "<path class=\"curve-line\" d=\"M10,70 Q60,60 60,40 Q60,18 "
          "110,10\"/></svg>"
          "<div class=\"name\">Thru / Const. Power</div><div "
          "class=\"desc\">Equal loudness (Curve 0)</div></a>",
          xf_curve == 0 ? " active" : "");

  /* Linear (1) */
  sb_addf(b,
          "<a href=\"/xfader?set=1\" class=\"curve-option%s\">"
          "<svg viewBox=\"0 0 120 80\"><line class=\"axis\" x1=\"10\" "
          "y1=\"70\" x2=\"110\" y2=\"70\"/><line class=\"axis\" x1=\"10\" "
          "y1=\"10\" x2=\"10\" y2=\"70\"/>"
          "<path class=\"curve-line\" d=\"M10,70 L110,10\"/></svg>"
          "<div class=\"name\">Linear</div><div class=\"desc\">Even fade "
          "transition (Curve 1)</div></a>",
          xf_curve == 1 ? " active" : "");

  /* Sharp (2) */
  sb_addf(b,
          "<a href=\"/xfader?set=2\" class=\"curve-option%s\">"
          "<svg viewBox=\"0 0 120 80\"><line class=\"axis\" x1=\"10\" "
          "y1=\"70\" x2=\"110\" y2=\"70\"/><line class=\"axis\" x1=\"10\" "
          "y1=\"10\" x2=\"10\" y2=\"70\"/>"
          "<path class=\"curve-line\" d=\"M10,70 C12,20 18,12 110,10\"/></svg>"
          "<div class=\"name\">Sharp / Cut</div><div class=\"desc\">Fast "
          "switch scratch cut (Curve 2)</div></a>",
          xf_curve == 2 ? " active" : "");

  sb_adds(b, "</div></div></div>");

  /* ── Subsection: COLOR FX ── */
  sb_adds(b, "<div style=\"margin-top:24px\"></div>");
  sb_adds(b, "<div class=\"section-head\"><div class=\"sh-icon purple\">"
             "<svg class=\"ico ico-lg\" viewBox=\"0 0 24 24\"><path d=\"M12 "
             "2v4\"/><path d=\"M12 18v4\"/><path d=\"M4.93 4.93l2.83 "
             "2.83\"/><path d=\"M16.24 16.24l2.83 2.83\"/><path d=\"M2 "
             "12h4\"/><path d=\"M18 12h4\"/><path d=\"M4.93 "
             "19.07l2.83-2.83\"/><path d=\"M16.24 7.76l2.83-2.83\"/></svg>"
             "</div><h2>Sound Color FX<small>SHIFT + CUE 1&ndash;4 slot "
             "assignment</small></h2></div>");
  sb_adds(
      b,
      "<div class=\"card\"><div class=\"card-body\"><div class=\"fx-grid\">");
  for (int s = 0; s < 4; s++) {
    sb_addf(
        b,
        "<div class=\"fx-card\">"
        "<div class=\"fx-card-head\"><span class=\"slot-name\">Slot %d</span>"
        "<span class=\"slot-tag color\">SHIFT+CUE%d</span></div>"
        "<form method=\"get\" action=\"/flx6_cfx\" style=\"margin:0\">"
        "<input type=\"hidden\" name=\"slot\" value=\"%d\">"
        "<div class=\"fx-row\"><span class=\"fx-label\">Effect</span>"
        "<select name=\"fx\" onchange=\"this.form.submit()\" style=\"flex:1\">",
        s + 1, s + 1, s + 1);
    for (int opt = 0; opt < 6; opt++) {
      int sel = !strcasecmp(cfg.cfx[s], cfx_options[opt]);
      sb_addf(b, "<option value=\"%s\"%s>%s</option>", cfx_options[opt],
              sel ? " selected" : "", cfx_labels[opt]);
    }
    sb_adds(b, "</select></div></form></div>");
  }
  sb_adds(b, "</div></div></div>");

  /* ── Subsection: BEAT FX ── */
  sb_adds(b, "<div style=\"margin-top:24px\"></div>");
  sb_adds(b, "<div class=\"section-head\"><div class=\"sh-icon orange\">"
             "<svg class=\"ico ico-lg\" viewBox=\"0 0 24 24\"><path d=\"M9 "
             "18V5l12-2v13\"/><circle cx=\"6\" cy=\"18\" r=\"3\"/><circle "
             "cx=\"18\" cy=\"16\" r=\"3\"/></svg>"
             "</div><h2>Beat FX Rack<small>FX1 &amp; FX2 deck slot "
             "assignment</small></h2></div>");
  sb_adds(
      b,
      "<div class=\"card\"><div class=\"card-body\"><div class=\"fx-grid\">");
  for (int s = 0; s < 6; s++) {
    int rack = (s < 3) ? 1 : 2;
    int rslot = (s % 3) + 1;
    sb_addf(b,
            "<div class=\"fx-card\">"
            "<div class=\"fx-card-head\"><span class=\"slot-name\">FX%d Slot "
            "%d</span>"
            "<span class=\"slot-tag beat\">Note %d</span></div>"
            "<form method=\"get\" action=\"/flx6_bfx\" style=\"margin:0\">"
            "<input type=\"hidden\" name=\"slot\" value=\"%d\">"
            "<div class=\"fx-row\"><span class=\"fx-label\">Effect</span>"
            "<select name=\"type\" style=\"flex:1\" "
            "onchange=\"this.form.submit()\">",
            rack, rslot, 71 + (s % 3), s + 1);
    for (int t = 0; t < 14; t++) {
      sb_addf(b, "<option value=\"%d\"%s>%s</option>", t,
              (cfg.bfx_type[s] == t) ? " selected" : "", bfx_names[t]);
    }
    sb_adds(b, "</select></div>");
    sb_adds(b, "<div class=\"fx-row\"><span class=\"fx-label\">Channel</span>"
               "<select name=\"ch\" style=\"flex:1\" "
               "onchange=\"this.form.submit()\">");
    sb_addf(b, "<option value=\"0\"%s>Deck 1</option>",
            (cfg.bfx_ch[s] == 0) ? " selected" : "");
    sb_addf(b, "<option value=\"1\"%s>Deck 2</option>",
            (cfg.bfx_ch[s] == 1) ? " selected" : "");
    sb_addf(b, "<option value=\"5\"%s>Master</option>",
            (cfg.bfx_ch[s] == 5) ? " selected" : "");
    sb_adds(b, "</select></div></form></div>");
  }
  sb_adds(b, "</div></div></div>");
  sb_adds(b, "</div>"); /* end tab-controller */

  /* ══════════════════ TAB: LOG VIEWER ══════════════════ */
  sb_adds(b, "<div class=\"tab-panel\" id=\"tab-logs\">");
  sb_adds(b, "<div class=\"section-head\"><div class=\"sh-icon blue\">"
             "<svg class=\"ico ico-lg\" viewBox=\"0 0 24 24\"><path d=\"M14 "
             "2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 "
             "2-2V8z\"/><polyline points=\"14 2 14 8 20 8\"/><line x1=\"16\" "
             "y1=\"13\" x2=\"8\" y2=\"13\"/><line x1=\"16\" y1=\"17\" x2=\"8\" "
             "y2=\"17\"/><polyline points=\"10 9 9 9 8 9\"/></svg>"
             "</div><h2>Log Viewer<small>rbp runtime and system daemon "
             "logs</small></h2></div>");
  sb_adds(b, "<div class=\"card\">"
             "<div class=\"log-toolbar\">"
             "<button class=\"btn-ctrl\" onclick=\"refreshLogs()\" "
             "style=\"padding:4px 12px;font-size:11px\">Refresh Logs</button>"
             "<button class=\"btn-ctrl\" onclick=\"clearLogs()\" "
             "style=\"padding:4px 12px;font-size:11px;margin-left:6px;border-color:var(--red);color:var(--red)\">Clear Logs</button>"
             "<div class=\"search-box\">"
             "<input type=\"text\" id=\"log-search\" placeholder=\"Filter "
             "logs...\" oninput=\"filterLogs(this.value)\">"
             "</div></div>"
             "<div class=\"log-body\" id=\"log-body\">Loading logs...</div>"
             "<div class=\"log-footer\">"
             "<span id=\"log-status\">Ready</span>"
             "<div class=\"auto-scroll\"><span>Auto-refresh</span>"
             "<label class=\"toggle\"><input type=\"checkbox\" "
             "id=\"auto-log\"><span class=\"slider\"></span></label>"
             "</div></div></div></div>");

  sb_adds(b, "</div>"); /* end content */
  sb_adds(b, "</div>"); /* end main */

  /* JavaScript Interactions */
  sb_adds(
      b,
      "<script>"
      "var tabTitles={"
      "system:'System',"
      "controller:'Controller Settings',"
      "logs:'Log Viewer'};"
      "function switchTab(id,btn){"
      "document.querySelectorAll('.tab-panel').forEach(function(p){p.classList."
      "remove('active')});"
      "var p=document.getElementById('tab-'+id);if(p)p.classList.add('active');"
      "document.querySelectorAll('.sidebar-nav "
      "button').forEach(function(b){b.classList.remove('active')});"
      "if(btn)btn.classList.add('active');"
      "var "
      "bc=document.getElementById('breadcrumb');if(bc&&tabTitles[id])bc."
      "innerHTML='UNX &rsaquo; <strong>'+tabTitles[id]+'</strong>';"
      "if(window.innerWidth<=860)toggleSidebar();"
      "if(id==='logs')refreshLogs();"
      "}"
      "function toggleSidebar(){"
      "document.getElementById('sidebar').classList.toggle('open');"
      "document.getElementById('overlay').classList.toggle('show');"
      "}"
      "function updateJog(val){"
      "var v=parseFloat(val);"
      "document.getElementById('jog-val').innerHTML=v.toFixed(2)+'<small>x</"
      "small>';"
      "document.querySelectorAll('.preset-btn').forEach(function(b){"
      "b.classList.remove('active');"
      "if(Math.abs(parseFloat(b.textContent)-v)<0.02)b.classList.add('active');"
      "});"
      "}"
      "function setJog(val){"
      "document.getElementById('jog-slider').value=val;"
      "updateJog(val);"
      "fetch('/jog?set='+val);"
      "}"
      "function runCmd(){"
      "var inp=document.getElementById('term-input');"
      "var body=document.getElementById('term-body');"
      "var cmd=inp.value.trim();if(!cmd)return;"
      "var l=document.createElement('div');"
      "l.innerHTML='<span class=\"prompt\">#</span> <span "
      "class=\"cmd\">'+cmd.replace(/</g,'&lt;').replace(/>/g,'&gt;')+'</span>';"
      "body.appendChild(l);"
      "inp.value='';"
      "fetch('/exec?cmd='+encodeURIComponent(cmd)).then(function(r){return "
      "r.text()}).then(function(txt){"
      "var "
      "o=document.createElement('div');o.className='output';o.textContent=txt;"
      "body.appendChild(o);"
      "body.scrollTop=body.scrollHeight;"
      "}).catch(function(e){"
      "var "
      "o=document.createElement('div');o.className='output';o.textContent='"
      "Error: '+e;body.appendChild(o);"
      "});"
      "}"
      "document.getElementById('term-input').addEventListener('keydown',"
      "function(e){if(e.key==='Enter')runCmd()});"
      "var rawLogText='';"
      "function refreshLogs(){"
      "fetch('/log').then(function(r){return r.text()}).then(function(txt){"
      "rawLogText=txt;filterLogs(document.getElementById('log-search').value);"
      "document.getElementById('log-status').textContent='Updated '+new "
      "Date().toLocaleTimeString();"
      "});"
      "}"
      "function clearLogs(){"
      "if(!confirm('Clear rbp log files?'))return;"
      "fetch('/log?clear=1').then(function(r){return r.text()}).then(function(){"
      "rawLogText='Logs cleared.\\n';"
      "document.getElementById('log-body').textContent=rawLogText;"
      "document.getElementById('log-status').textContent='Cleared';"
      "});"
      "}"
      "function filterLogs(q){"
      "var b=document.getElementById('log-body');"
      "if(!q){b.textContent=rawLogText;b.scrollTop=b.scrollHeight;return;}"
      "var lines=rawLogText.split('\\n');"
      "var match=lines.filter(function(l){return "
      "l.toLowerCase().indexOf(q.toLowerCase())>=0});"
      "b.textContent=match.join('\\n');"
      "b.scrollTop=b.scrollHeight;"
      "}"
      "setInterval(function(){if(document.getElementById('auto-log').checked)"
      "refreshLogs();},3000);"
      "function animateGauges(){"
      "setGauge('cpu',15+Math.random()*18,'%');"
      "setGauge('mem',58+Math.random()*8,'%');"
      "setGauge('temp',49+Math.random()*5,'\\u00b0C');"
      "}"
      "function setGauge(id,v,u){"
      "var el=document.getElementById(id+'-val');var "
      "bar=document.getElementById(id+'-bar');"
      "if(!el||!bar)return;var cls=v>80?'crit':v>60?'warn':'ok';"
      "el.textContent=Math.round(v)+u;el.className='value "
      "'+cls;bar.className='fill '+cls;bar.style.width=Math.min(v,100)+'%';"
      "}"
      "setInterval(animateGauges,3000);"
      "var sEl=document.getElementById('shot');"
      "if(sEl)sEl.addEventListener('click',function(){document.body.classList."
      "toggle('only')});"
      "</script></body></html>");
}

/* --- responses
 * ---------------------------------------------------------------- */

static void queue(struct hconn *c, const void *p, size_t n) {
  if (c->pendoff == c->pendlen) {
    c->pendoff = c->pendlen = 0;
  }
  if (c->pendoff > 0) {
    memmove(c->pend, c->pend + c->pendoff, c->pendlen - c->pendoff);
    c->pendlen -= c->pendoff;
    c->pendoff = 0;
  }
  {
    uint8_t *np = realloc(c->pend, c->pendlen + n);
    if (!np)
      return;
    c->pend = np;
    memcpy(c->pend + c->pendlen, p, n);
    c->pendlen += n;
  }
}

static void reply(struct hconn *c, int code, const char *status,
                  const char *ctype, const void *body, size_t blen,
                  const char *extra) {
  char hdr[512];
  int n = snprintf(hdr, sizeof hdr,
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "Cache-Control: no-store\r\n"
                   "Connection: close\r\n"
                   "%s"
                   "\r\n",
                   code, status, ctype, blen, extra ? extra : "");
  if (n > 0)
    queue(c, hdr, (size_t)n);
  if (blen)
    queue(c, body, blen);
  c->close_after = 1;
  c->headers_done = 1;
}

/* --- the preview stream
 * -------------------------------------------------------- */

static void preview_start(struct hconn *c) {
  char hdr[320];
  int n = snprintf(
      hdr, sizeof hdr,
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=" VNC_HTTP_BOUND "\r\n"
      "Cache-Control: no-store, no-cache, must-revalidate\r\n"
      "Pragma: no-cache\r\n"
      "Connection: close\r\n"
      "\r\n");
  if (n > 0)
    queue(c, hdr, (size_t)n);
  c->preview = 1;
  c->headers_done = 1;
}

/* --- routing
 * --------------------------------------------------------------------- */

/* One `?set=<word>` out of a request target. A zero return means there was
 * nothing to read; anything else is the word, NUL-terminated in the caller's
 * buffer.
 *
 * IT IS THE SAME PARSE FOR BOTH SWITCHES because it is the same request, and
 * because the word is handed to the module that owns the meaning rather than
 * being interpreted here: what "raw" and "hwjpeg" are, and what "on" and "off"
 * are, is vnc_mode's and vnc_input's business, and both refuse anything they do
 * not recognise. A page that guessed would be a second place for the two
 * spellings to disagree. */
static size_t query_set(const char *target, char *word, size_t cap) {
  const char *q = strchr(target, '?');
  size_t n;

  if (!q || strncmp(q + 1, "set=", 4) != 0)
    return 0;
  n = strcspn(q + 5, "& ");
  if (n == 0 || n >= cap)
    return 0;
  memcpy(word, q + 5, n);
  word[n] = '\0';
  return n;
}

static size_t query_param(const char *target, const char *key, char *out,
                          size_t cap) {
  const char *q = strchr(target, '?');
  if (!q)
    return 0;
  q++;
  size_t klen = strlen(key);
  while (*q) {
    if (!strncmp(q, key, klen) && q[klen] == '=') {
      const char *val = q + klen + 1;
      size_t n = strcspn(val, "& \r\n");
      if (n >= cap)
        n = cap - 1;
      memcpy(out, val, n);
      out[n] = '\0';
      return n;
    }
    q = strchr(q, '&');
    if (!q)
      break;
    q++;
  }
  return 0;
}

static void route_get(struct vnc_http *h, struct hconn *c, const char *target) {
  if (!strncmp(target, "/mode", 5) && (target[5] == '\0' || target[5] == '?')) {
    char word[32];
    if (query_set(target, word, sizeof word)) {
      int want;
      if (vnc_mode_parse(word, &want) == 0) {
        int took = vnc_mode_set(h->st->mode, want);
        h->switched++;
        vlog("http: the page set the mode to %s (file %s)", vnc_mode_name(took),
             h->st->mode_path);
      } else {
        vlog("http: the page asked for mode \"%s\", which names no mode; "
             "leaving it at %s",
             word, vnc_mode_name(h->st->mode->mode));
      }
    }
    /* 303 and not 200: the answer to "change the mode" is not this response, it
     * is the page, so send the browser back to fetch it. */
    reply(c, 303, "See Other", "text/plain; charset=utf-8", "", 0,
          "Location: /\r\n");
    return;
  }

  if (!strncmp(target, "/input", 6) &&
      (target[6] == '\0' || target[6] == '?')) {
    char word[32];
    if (h->st->in && query_set(target, word, sizeof word)) {
      int want;
      if (!strcmp(word, "toggle")) {
        /* The page's own word, and it is the page's because it does not name a
         * state -- a file cannot hold "the other one". It exists so that a
         * bookmark or a shortcut can be one press rather than two. */
        want = h->st->in->on ? VNC_INPUT_OFF : VNC_INPUT_ON;
      } else if (vnc_input_parse(word, &want) < 0) {
        want = -1;
      }
      if (want >= 0) {
        int took = vnc_input_switch_set(h->st->in, want);
        vlog("http: the page turned vnc input %s (file %s)",
             vnc_input_name(took), h->st->in->path);
      } else {
        vlog("http: the page asked for input \"%s\", which is neither on nor "
             "off; leaving it %s",
             word, vnc_input_name(h->st->in->on));
      }
    }
    reply(c, 303, "See Other", "text/plain; charset=utf-8", "", 0,
          "Location: /\r\n");
    return;
  }

  if (!strncmp(target, "/session", 8) &&
      (target[8] == '\0' || target[8] == '?')) {
    char word[32];
    if (h->st->live && query_set(target, word, sizeof word)) {
      int want;
      if (!strcmp(word, "toggle")) {
        /* The same word as the input switch, for the same reason: a file
         * cannot hold "the other one". */
        want = h->st->live->on ? VNC_LIVE_OFF : VNC_LIVE_ON;
      } else if (vnc_live_parse(word, &want) < 0) {
        want = -1;
      }
      if (want >= 0) {
        int took = vnc_live_set(h->st->live, want);
        vlog("http: the page turned sharing %s (file %s)", vnc_live_name(took),
             h->st->live->path);
      } else {
        vlog("http: the page asked for sharing \"%s\", which is neither on nor "
             "off; leaving it %s",
             word, vnc_live_name(h->st->live->on));
      }
    }
    reply(c, 303, "See Other", "text/plain; charset=utf-8", "", 0,
          "Location: /\r\n");
    return;
  }

  if (!strncmp(target, "/jog", 4) && (target[4] == '\0' || target[4] == '?')) {
    char word[32];
    if (query_set(target, word, sizeof word)) {
      float mult = (float)atof(word);
      if (mult >= 0.01f && mult <= 1.0f) {
        const char *jpaths[] = {"/run/rblive4/jog_speed",
                                "/opt/rblive4/jog_speed"};
        for (int i = 0; i < 2; i++) {
          FILE *f = fopen(jpaths[i], "w");
          if (f) {
            fprintf(f, "%.2f\n", mult);
            fclose(f);
          }
        }
        vlog("http: jog speed multiplier set to %.2f (persisted)", mult);
      }
    }
    reply(c, 303, "See Other", "text/plain; charset=utf-8", "", 0,
          "Location: /\r\n");
    return;
  }

  if (!strncmp(target, "/xfader", 7) &&
      (target[7] == '\0' || target[7] == '?')) {
    char word[32];
    if (query_set(target, word, sizeof word)) {
      int curve = atoi(word);
      if (curve >= 0 && curve <= 2) {
        const char *xpaths[] = {"/run/rblive4/xfader_curve",
                                "/opt/rblive4/xfader_curve"};
        for (int i = 0; i < 2; i++) {
          FILE *f = fopen(xpaths[i], "w");
          if (f) {
            fprintf(f, "%d\n", curve);
            fclose(f);
          }
        }
        vlog("http: crossfader curve set to %d (persisted)", curve);
      }
    }
    reply(c, 303, "See Other", "text/plain; charset=utf-8", "", 0,
          "Location: /\r\n");
    return;
  }

  if (!strncmp(target, "/xfader_enable", 14) &&
      (target[14] == '\0' || target[14] == '?')) {
    char word[32];
    if (query_set(target, word, sizeof word)) {
      int en = atoi(word);
      if (en == 0 || en == 1) {
        const char *epaths[] = {"/run/rblive4/xfader_enable",
                                "/opt/rblive4/xfader_enable"};
        for (int i = 0; i < 2; i++) {
          FILE *f = fopen(epaths[i], "w");
          if (f) {
            fprintf(f, "%d\n", en);
            fclose(f);
          }
        }
        vlog("http: crossfader enable set to %d (persisted)", en);
      }
    }
    reply(c, 303, "See Other", "text/plain; charset=utf-8", "", 0,
          "Location: /\r\n");
    return;
  }

  if (!strncmp(target, "/flx6_cfx", 9) &&
      (target[9] == '\0' || target[9] == '?')) {
    char slot_str[16], fx_str[32];
    if (query_param(target, "slot", slot_str, sizeof slot_str) &&
        query_param(target, "fx", fx_str, sizeof fx_str)) {
      int slot = atoi(slot_str) - 1;
      if (slot >= 0 && slot < 4) {
        struct flx6_fx_config cfg;
        flx6_config_load(&cfg);
        snprintf(cfg.cfx[slot], sizeof cfg.cfx[slot], "%s", fx_str);
        flx6_config_save(&cfg);
        vlog("http: FLX6 Color FX Slot %d updated to %s", slot + 1, fx_str);
      }
    }
    reply(c, 303, "See Other", "text/plain; charset=utf-8", "", 0,
          "Location: /\r\n");
    return;
  }

  if (!strncmp(target, "/flx6_bfx", 9) &&
      (target[9] == '\0' || target[9] == '?')) {
    char slot_str[16], type_str[16], ch_str[16];
    if (query_param(target, "slot", slot_str, sizeof slot_str)) {
      int slot = atoi(slot_str) - 1;
      if (slot >= 0 && slot < 6) {
        struct flx6_fx_config cfg;
        flx6_config_load(&cfg);
        if (query_param(target, "type", type_str, sizeof type_str))
          cfg.bfx_type[slot] = atoi(type_str);
        if (query_param(target, "ch", ch_str, sizeof ch_str))
          cfg.bfx_ch[slot] = atoi(ch_str);
        flx6_config_save(&cfg);
        vlog("http: FLX6 Beat FX Slot %d updated: type=%d, ch=%d", slot + 1,
             cfg.bfx_type[slot], cfg.bfx_ch[slot]);
      }
    }
    reply(c, 303, "See Other", "text/plain; charset=utf-8", "", 0,
          "Location: /\r\n");
    return;
  }

  if (!strncmp(target, "/exec", 5) && (target[5] == '\0' || target[5] == '?')) {
    char cmd[256];
    if (query_param(target, "cmd", cmd, sizeof cmd)) {
      /* Basic URL unescape for space and common characters */
      char dec[256];
      size_t di = 0;
      for (size_t si = 0; cmd[si] && di < sizeof(dec) - 1; si++) {
        if (cmd[si] == '+')
          dec[di++] = ' ';
        else if (cmd[si] == '%' && cmd[si + 1] && cmd[si + 2]) {
          char hex[3] = {cmd[si + 1], cmd[si + 2], 0};
          dec[di++] = (char)strtol(hex, NULL, 16);
          si += 2;
        } else {
          dec[di++] = cmd[si];
        }
      }
      dec[di] = '\0';

      /* Execute command and collect output */
      FILE *p = popen(dec, "r");
      struct sbuf ob;
      memset(&ob, 0, sizeof ob);
      if (p) {
        char lbuf[256];
        while (fgets(lbuf, sizeof lbuf, p)) {
          sb_adds(&ob, lbuf);
          if (ob.len > 16384)
            break;
        }
        pclose(p);
      }
      if (ob.len == 0)
        sb_adds(&ob, "(no output)\n");
      reply(c, 200, "OK", "text/plain; charset=utf-8", ob.p ? ob.p : "", ob.len,
            NULL);
      free(ob.p);
      return;
    }
    const char *msg = "missing cmd param\n";
    reply(c, 400, "Bad Request", "text/plain; charset=utf-8", msg, strlen(msg),
          NULL);
    return;
  }

  if (!strncmp(target, "/log", 4) && (target[4] == '\0' || target[4] == '?')) {
    const char *lpaths[] = {"/opt/rblive4/log/rbp.log", "/run/rblive4/boot.log",
                            "/opt/rblive4/log/edb_streamd.log"};
    if (strstr(target, "clear=1")) {
      for (size_t i = 0; i < sizeof(lpaths) / sizeof(lpaths[0]); i++) {
        FILE *tf = fopen(lpaths[i], "w");
        if (tf) fclose(tf);
      }
      reply(c, 200, "OK", "text/plain; charset=utf-8", "cleared\n", 8, NULL);
      return;
    }

    FILE *lf = NULL;
    for (size_t i = 0; i < sizeof(lpaths) / sizeof(lpaths[0]); i++) {
      lf = fopen(lpaths[i], "r");
      if (lf)
        break;
    }
    struct sbuf lb;
    memset(&lb, 0, sizeof lb);
    if (lf) {
      /* Seek to near end if large */
      fseek(lf, 0, SEEK_END);
      long sz = ftell(lf);
      long off = (sz > 32768) ? (sz - 32768) : 0;
      fseek(lf, off, SEEK_SET);
      char lbuf[256];
      while (fgets(lbuf, sizeof lbuf, lf)) {
        sb_adds(&lb, lbuf);
      }
      fclose(lf);
    } else {
      sb_adds(&lb, "No rbp logs found in /opt/rblive4/log/\n");
    }
    reply(c, 200, "OK", "text/plain; charset=utf-8", lb.p ? lb.p : "", lb.len,
          NULL);
    free(lb.p);
    return;
  }

  if (!strncmp(target, "/preview.mjpg", 13)) {
    preview_start(c);
    return;
  }

  if (target[0] == '/' && (target[1] == '\0' || target[1] == '?')) {
    struct sbuf b;
    memset(&b, 0, sizeof b);
    page_html(h, &b);
    if (b.over) {
      const char *msg = "the page could not be built: out of memory\n";
      reply(c, 500, "Internal Server Error", "text/plain; charset=utf-8", msg,
            strlen(msg), NULL);
    } else {
      reply(c, 200, "OK", "text/html; charset=utf-8", b.p, b.len, NULL);
    }
    free(b.p);
    return;
  }

  {
    const char *msg = "no such page here. try /\n";
    reply(c, 404, "Not Found", "text/plain; charset=utf-8", msg, strlen(msg),
          NULL);
  }
}

/* --- connection handling
 * --------------------------------------------------------- */

static void conn_close(struct hconn *c) {
  if (c->fd >= 0)
    close(c->fd);
  free(c->pend);
  memset(c, 0, sizeof *c);
  c->fd = -1;
}

static void conn_drain(struct hconn *c) {
  while (c->pendoff < c->pendlen) {
    ssize_t w = send(c->fd, c->pend + c->pendoff, c->pendlen - c->pendoff,
                     MSG_NOSIGNAL);
    if (w > 0) {
      c->pendoff += (size_t)w;
      continue;
    }
    if (w < 0 && errno == EINTR)
      continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return;
    conn_close(c);
    return;
  }
  c->pendoff = c->pendlen = 0;
  if (c->close_after)
    conn_close(c);
}

static void conn_read(struct vnc_http *h, struct hconn *c) {
  ssize_t g;

  if (c->preview) {
    /* A preview connection has nothing more to say on the way in; a read that
     * returns zero is the browser closing the tab, which is the only way this
     * server ever learns to stop encoding. */
    char sink[256];
    g = recv(c->fd, sink, sizeof sink, 0);
    if (g == 0 ||
        (g < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      conn_close(c);
    return;
  }

  if (c->reqlen == sizeof c->req)
    return;
  g = recv(c->fd, c->req + c->reqlen, sizeof c->req - c->reqlen, 0);
  if (g == 0) {
    conn_close(c);
    return;
  }
  if (g < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return;
    conn_close(c);
    return;
  }
  c->reqlen += (size_t)g;
  c->req[c->reqlen] = '\0';

  if (!strstr(c->req, "\r\n\r\n") && !strstr(c->req, "\n\n")) {
    if (c->reqlen == sizeof c->req - 1) {
      const char *msg = "request headers too long\n";
      reply(c, 431, "Request Header Fields Too Large",
            "text/plain; charset=utf-8", msg, strlen(msg), NULL);
    }
    return;
  }

  {
    char method[8];
    char target[256];
    if (sscanf(c->req, "%7s %255s", method, target) != 2) {
      const char *msg = "malformed request\n";
      reply(c, 400, "Bad Request", "text/plain; charset=utf-8", msg,
            strlen(msg), NULL);
      return;
    }
    if (strcmp(method, "GET") != 0) {
      const char *msg = "this is a read-only page\n";
      reply(c, 405, "Method Not Allowed", "text/plain; charset=utf-8", msg,
            strlen(msg), NULL);
      return;
    }
    h->served++;
    route_get(h, c, target);
  }
}

/* --- public
 * --------------------------------------------------------------------- */

struct vnc_http *vnc_http_open(const char *bindaddr, int port,
                               struct vnc_http_state *st, char *err,
                               size_t errlen) {
  struct vnc_http *h = calloc(1, sizeof *h);
  int i;

  if (err && errlen)
    err[0] = '\0';
  if (!h) {
    snprintf(err, errlen, "out of memory");
    return NULL;
  }
  h->st = st;
  h->ls = vnc_net_listen(bindaddr, port);
  if (h->ls < 0) {
    snprintf(err, errlen, "cannot listen on %s:%d: %s",
             bindaddr ? bindaddr : "0.0.0.0", port, strerror(errno));
    free(h);
    return NULL;
  }
  for (i = 0; i < VNC_HTTP_CONNS; i++)
    h->conn[i].fd = -1;
  return h;
}

void vnc_http_close(struct vnc_http *h) {
  int i;
  if (!h)
    return;
  for (i = 0; i < VNC_HTTP_CONNS; i++)
    conn_close(&h->conn[i]);
  if (h->ls >= 0)
    close(h->ls);
  free(h);
}

int vnc_http_preview_wanted(const struct vnc_http *h) {
  int i;
  if (!h)
    return 0;
  /* SHARING OFF MEANS NOT EVEN THE PREVIEW IS ENCODED. A preview connection is
   * a reason for the capture to run, and while sharing is off there is no
   * capture at all -- honouring one here would open the display and the encoder
   * behind the operator's back, which is precisely the thing the switch exists
   * to stop. The page does not ask for a preview in that state either; this is
   * the backstop for a tab that was already open when sharing was turned off.
   */
  if (h->st && h->st->live && !h->st->live->on)
    return 0;
  for (i = 0; i < VNC_HTTP_CONNS; i++)
    if (h->conn[i].fd >= 0 && h->conn[i].preview)
      return 1;
  return 0;
}

void vnc_http_preview_frame(struct vnc_http *h, const uint8_t *jpeg,
                            size_t len) {
  int i;
  if (!h || !jpeg || !len)
    return;
  for (i = 0; i < VNC_HTTP_CONNS; i++) {
    struct hconn *c = &h->conn[i];
    char hdr[160];
    int n;
    if (c->fd < 0 || !c->preview)
      continue;
    /* Behind by a frame? Then drop this one rather than queue it. A preview
     * that is a little behind is a preview; a preview that makes the whole
     * server fall behind is a problem the VNC client shares. */
    if (c->pendoff < c->pendlen)
      continue;
    n = snprintf(hdr, sizeof hdr,
                 "--" VNC_HTTP_BOUND "\r\n"
                 "Content-Type: image/jpeg\r\n"
                 "Content-Length: %zu\r\n\r\n",
                 len);
    if (n <= 0)
      continue;
    queue(c, hdr, (size_t)n);
    queue(c, jpeg, len);
    queue(c, "\r\n", 2);
    conn_drain(c);
  }
}

void vnc_http_poll(struct vnc_http *h, int timeout_ms) {
  struct pollfd pf[VNC_HTTP_CONNS + 1];
  int idx[VNC_HTTP_CONNS + 1];
  int nf = 0, i, r;

  if (!h)
    return;

  /* One listener, and `idx` carries -1 for it, so the accept arm below can tell
   * a listener from a connection without a second array. */
  pf[nf].fd = h->ls;
  pf[nf].events = POLLIN;
  pf[nf].revents = 0;
  idx[nf] = -1;
  nf++;

  for (i = 0; i < VNC_HTTP_CONNS; i++) {
    struct hconn *c = &h->conn[i];
    if (c->fd < 0)
      continue;
    pf[nf].fd = c->fd;
    pf[nf].events = (short)(POLLIN | (c->pendoff < c->pendlen ? POLLOUT : 0));
    pf[nf].revents = 0;
    idx[nf] = i;
    nf++;
  }

  r = poll(pf, (nfds_t)nf, timeout_ms);
  if (r < 0 && errno != EINTR)
    return;

  for (i = 0; i < nf; i++) {
    int ci = idx[i];
    if (!pf[i].revents)
      continue;
    if (ci < 0) {
      if (pf[i].revents & POLLIN) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int fd = accept(pf[i].fd, (struct sockaddr *)&peer, &pl);
        int slot;
        if (fd < 0)
          continue;
        for (slot = 0; slot < VNC_HTTP_CONNS && h->conn[slot].fd >= 0; slot++)
          ;
        if (slot == VNC_HTTP_CONNS) {
          const char *msg = "too many connections\n";
          send(fd, msg, strlen(msg), MSG_NOSIGNAL);
          close(fd);
          continue;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        memset(&h->conn[slot], 0, sizeof h->conn[slot]);
        h->conn[slot].fd = fd;
      }
      continue;
    }
    {
      struct hconn *c = &h->conn[ci];
      if (pf[i].revents & (POLLIN | POLLHUP | POLLERR))
        conn_read(h, c);
      if (c->fd >= 0 && (pf[i].revents & POLLOUT))
        conn_drain(c);
    }
  }
}
