/*
 * Mini Browser 2.1 - Linux text-only edition
 *
 * Linux companion to the WHY2025 BadgeVMS Mini Browser 2.1.
 * Single-file C program using libcurl.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o mini-browser mini_browser_linux.c -lcurl
 *
 * Run:
 *   ./mini-browser
 *   ./mini-browser https://wiby.me/
 */

#include <curl/curl.h>
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define VERSION "2.1"
#define MAX_BYTES (64 * 1024)
#define URL_MAX 256
#define MAX_LINKS 128
#define MAX_ACTIONS 160
#define MAX_FORMS 4
#define MAX_FORM_FIELDS 8
#define FORM_VALUE_MAX 128
#define HISTORY_MAX 32
#define BOOKMARK_MAX 32
#define HOME_URL "https://minibrowser.macip.net"

typedef struct { char *buf; size_t len; } mem_t;
typedef struct { char href[URL_MAX]; } link_t;

typedef struct {
    char name[64];
    char value[FORM_VALUE_MAX];
    char label[64];
    char type[16];
    bool disabled;
} form_field_t;

typedef struct {
    char action[URL_MAX];
    char method[8];
    form_field_t fields[MAX_FORM_FIELDS];
    int field_count;
} form_t;

typedef enum { ACTION_LINK, ACTION_FORM_FIELD, ACTION_FORM_SUBMIT } action_type_t;

typedef struct {
    action_type_t type;
    int link_index, form_index, field_index;
} page_action_t;

typedef struct {
    char *text, *text_template;
    link_t links[MAX_LINKS];
    int link_count;
    page_action_t actions[MAX_ACTIONS];
    int action_count;
    char base[URL_MAX], title[128];
    form_t forms[MAX_FORMS];
    int form_count;
} page_t;

typedef struct { char url[URL_MAX], title[128]; } bookmark_t;

static bookmark_t bookmarks[BOOKMARK_MAX];
static int bookmark_count;
static char history[HISTORY_MAX][URL_MAX];
static int history_len, history_pos = -1;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    size_t n=size*nmemb, keep=n; mem_t *m=ud;
    if(m->len>=MAX_BYTES) return n;
    if(m->len+keep>MAX_BYTES) keep=MAX_BYTES-m->len;
    char *p=realloc(m->buf,m->len+keep+1); if(!p) return 0;
    m->buf=p; memcpy(m->buf+m->len,ptr,keep); m->len+=keep; m->buf[m->len]=0; return n;
}
static void trim(char *s) {
    size_t n=strlen(s),a=0,b=n; while(a<n&&isspace((unsigned char)s[a]))a++; while(b>a&&isspace((unsigned char)s[b-1]))b--;
    if(a||b<n){memmove(s,s+a,b-a);s[b-a]=0;}
}
static bool has_scheme(const char *u){return u&&strstr(u,"://");}
static bool http_url(const char *u){return u&&(!strncmp(u,"http://",7)||!strncmp(u,"https://",8));}
static void normalize_url(char *u){trim(u);if(u[0]&&!has_scheme(u)){char t[URL_MAX];snprintf(t,sizeof t,"%s",u);snprintf(u,URL_MAX,"https://%s",t);}}
static void origin(const char *u,char *o,size_t c){const char*p=strstr(u,"://");if(!p){*o=0;return;}p+=3;const char*s=strchr(p,'/');size_t n=s?(size_t)(s-u):strlen(u);if(n>=c)n=c-1;memcpy(o,u,n);o[n]=0;}
static void directory(const char *u,char *o,size_t c){const char*p=strrchr(u,'/');if(!p){*o=0;return;}size_t n=(size_t)(p-u)+1;if(n>=c)n=c-1;memcpy(o,u,n);o[n]=0;}
static void cut_url(const char*u,char*o,size_t c,bool hash_only){size_t n=strlen(u),z=n;for(size_t i=0;i<n;i++)if(u[i]=='#'||(!hash_only&&u[i]=='?')){z=i;break;}if(z>=c)z=c-1;memcpy(o,u,z);o[z]=0;}
static void scheme(const char*u,char*o,size_t c){const char*p=u?strstr(u,"://"):NULL;if(!p){snprintf(o,c,"https");return;}size_t n=p-u;if(n>=c)n=c-1;memcpy(o,u,n);o[n]=0;}
static void resolve_url(const char*base,const char*href,char*out,size_t cap){
    if(!href||!*href){*out=0;return;} if(strstr(href,"://")){snprintf(out,cap,"%s",href);return;}
    if(href[0]=='/'&&href[1]=='/'){char s[16];scheme(base,s,sizeof s);snprintf(out,cap,"%s:%s",s,href);return;}
    if(href[0]=='/'){char o[URL_MAX];origin(base,o,sizeof o);snprintf(out,cap,"%s%s",o,href);return;}
    if(href[0]=='?'){char b[URL_MAX];cut_url(base,b,sizeof b,false);snprintf(out,cap,"%s%s",b,href);return;}
    if(href[0]=='#'){char b[URL_MAX];cut_url(base,b,sizeof b,true);snprintf(out,cap,"%s%s",b,href);return;}
    if(href[0]=='.'&&href[1]=='/')href+=2; char d[URL_MAX];directory(base,d,sizeof d);snprintf(out,cap,"%s%s",d,href);
}
static bool supported_href(const char*h){return h&&*h&&h[0]!='#'&&strncasecmp(h,"javascript:",11)&&strncasecmp(h,"mailto:",7)&&strncasecmp(h,"data:",5);}
static const char *find_ci(const char*h,const char*n){size_t l=strlen(n);for(const char*p=h;p&&*p;p++)if(!strncasecmp(p,n,l))return p;return NULL;}
static const char *tag_end(const char*s,const char*e){char q=0;for(const char*p=s;p<e;p++){if(q){if(*p==q)q=0;}else if(*p=='"'||*p=='\'')q=*p;else if(*p=='>')return p;}return NULL;}
static int attribute(const char*s,const char*e,const char*w,char*out,size_t cap){
    size_t wl=strlen(w);const char*p=s;
    while(p<e){while(p<e&&(isspace((unsigned char)*p)||*p=='/'))p++;const char*n=p;while(p<e&&(isalnum((unsigned char)*p)||*p=='-'||*p=='_'))p++;size_t nl=p-n;if(!nl){p++;continue;}while(p<e&&isspace((unsigned char)*p))p++;
        const char*v=NULL;size_t vl=0;if(p<e&&*p=='='){p++;while(p<e&&isspace((unsigned char)*p))p++;if(p<e&&(*p=='"'||*p=='\'')){char q=*p++;v=p;while(p<e&&*p!=q)p++;vl=p-v;if(p<e)p++;}else{v=p;while(p<e&&!isspace((unsigned char)*p)&&*p!='>')p++;vl=p-v;}}
        if(nl==wl&&!strncasecmp(n,w,wl)){if(out&&cap&&v){if(vl>=cap)vl=cap-1;memcpy(out,v,vl);out[vl]=0;}return 1;}}
    return 0;
}
static void lower(char*s){for(;s&&*s;s++)*s=(char)tolower((unsigned char)*s);}
static bool appendn(char*o,size_t cap,size_t*u,const char*s,size_t n){if(*u+n>=cap)return false;memcpy(o+*u,s,n);*u+=n;o[*u]=0;return true;}
static bool appends(char*o,size_t cap,size_t*u,const char*s){return appendn(o,cap,u,s,strlen(s));}
static void newline(char*o,size_t cap,size_t*u){if(*u&&o[*u-1]!='\n')appends(o,cap,u,"\n");}
static int add_action(page_t*p,action_type_t t,int li,int fi,int fld){if(p->action_count>=MAX_ACTIONS)return-1;int i=p->action_count++;p->actions[i]=(page_action_t){t,li,fi,fld};return i;}
static void add_marker(char*o,size_t cap,size_t*u,int i){char b[8];snprintf(b,sizeof b,"\001%03d\002",i);appends(o,cap,u,b);}
static void extract_title(const char*h,char*o,size_t c){*o=0;const char*s=find_ci(h,"<title");if(!s||(s=strchr(s,'>'))==NULL)return;s++;const char*e=find_ci(s,"</title>");if(!e)return;size_t n=e-s;if(n>=c)n=c-1;memcpy(o,s,n);o[n]=0;trim(o);}
static void extract_button(const char*s,const char*e,char*o,size_t c){size_t u=0;bool sp=false;for(const char*p=s;p<e&&u+1<c;p++){if(*p=='<'){const char*x=tag_end(p+1,e);if(!x)break;p=x;}else if(isspace((unsigned char)*p))sp=u>0;else{if(sp&&u+1<c)o[u++]=' ';sp=false;o[u++]=*p;}}o[u]=0;trim(o);}
static const char *decode_entity(const char*h,char*o,size_t*u,size_t cap){
    struct ent{const char*n;char c;} es[]={{"&amp;",'&'},{"&lt;",'<'},{"&gt;",'>'},{"&quot;",'"'},{"&#39;",'\''},{"&apos;",'\''},{"&nbsp;",' '}};
    for(size_t i=0;i<sizeof es/sizeof es[0];i++){size_t n=strlen(es[i].n);if(!strncmp(h,es[i].n,n)){if(*u<cap)o[(*u)++]=es[i].c;return h+n;}}
    if(h[0]=='&'&&h[1]=='#'){const char*p=h+2;int base=10;if(*p=='x'||*p=='X'){base=16;p++;}const char*d=p;unsigned long v=0;while(*p&&*p!=';'){int x;if(isdigit((unsigned char)*p))x=*p-'0';else if(base==16&&*p>='a'&&*p<='f')x=10+*p-'a';else if(base==16&&*p>='A'&&*p<='F')x=10+*p-'A';else return NULL;if(x>=base||v>(ULONG_MAX-(unsigned)x)/(unsigned)base)return NULL;v=v*base+(unsigned)x;p++;}if(p!=d&&*p==';'){if(*u<cap)o[(*u)++]=(v>=32&&v<=126)?(char)v:'?';return p+1;}}
    return NULL;
}
static bool refresh_text(page_t*p){
    size_t cap=strlen(p->text_template)+(size_t)p->action_count*(FORM_VALUE_MAX+96)+1,u=0;char*r=calloc(1,cap);if(!r)return false;
    for(const char*q=p->text_template;*q;){if((unsigned char)q[0]==1&&isdigit((unsigned char)q[1])&&isdigit((unsigned char)q[2])&&isdigit((unsigned char)q[3])&&(unsigned char)q[4]==2){int ai=(q[1]-'0')*100+(q[2]-'0')*10+(q[3]-'0');char b[FORM_VALUE_MAX+96]="";
            if(ai>=0&&ai<p->action_count){page_action_t*a=&p->actions[ai];if(a->type==ACTION_FORM_FIELD){form_field_t*f=&p->forms[a->form_index].fields[a->field_index];snprintf(b,sizeof b,"[%d] %s: %s\n",ai+1,f->name,f->value);}else if(a->type==ACTION_FORM_SUBMIT){form_field_t*f=&p->forms[a->form_index].fields[a->field_index];snprintf(b,sizeof b,"[%d] [%s]\n",ai+1,f->label[0]?f->label:"Submit");}}
            if(!appends(r,cap,&u,b)){free(r);return false;}q+=5;}else{if(!appendn(r,cap,&u,q,1)){free(r);return false;}q++;}}
    free(p->text);p->text=r;return true;
}
static page_t *html_to_page(const char*html,const char*base){
    size_t L=strlen(html),cap=L+(size_t)MAX_ACTIONS*8+1,u=0;char*t=calloc(1,cap);page_t*p=calloc(1,sizeof*p);if(!t||!p){free(t);free(p);return NULL;}
    snprintf(p->base,sizeof p->base,"%s",base?base:"");extract_title(html,p->title,sizeof p->title);
    const char*end=html+L;bool head=false,script=false,style=false,pre=false;int cf=-1,ol=0,item=0;
    for(const char*cur=html;cur<end&&*cur;){
        if(*cur!='<'){if(!head&&!script&&!style){if(*cur=='&'){const char*n=decode_entity(cur,t,&u,cap-1);if(n){cur=n;continue;}}if(pre){if(*cur!='\r')appendn(t,cap,&u,cur,1);}else if(isspace((unsigned char)*cur)){if(u&&t[u-1]!=' '&&t[u-1]!='\n')appends(t,cap,&u," ");}else appendn(t,cap,&u,cur,1);}cur++;continue;}
        if(cur+4<=end&&!strncmp(cur,"<!--",4)){const char*x=strstr(cur+4,"-->");cur=x?x+3:end;continue;}if(cur+1<end&&(cur[1]=='!'||cur[1]=='?')){const char*x=tag_end(cur+2,end);cur=x?x+1:end;continue;}
        const char*q=cur+1;bool closing=false;if(q<end&&*q=='/'){closing=true;q++;}while(q<end&&isspace((unsigned char)*q))q++;char tag[16];size_t z=0;while(q<end&&z+1<sizeof tag&&isalpha((unsigned char)*q))tag[z++]=(char)tolower((unsigned char)*q++);tag[z]=0;const char*attrs=q,*te=tag_end(attrs,end);if(!te)break;const char*after=te+1;
        if(closing){if(!strcmp(tag,"head"))head=false;else if(!strcmp(tag,"script"))script=false;else if(!strcmp(tag,"style"))style=false;else if(!strcmp(tag,"form")){cf=-1;newline(t,cap,&u);}else if(!strcmp(tag,"pre")){pre=false;newline(t,cap,&u);}else if(!strcmp(tag,"ol")){if(ol>0)ol--;}else if(!strcmp(tag,"code"))appends(t,cap,&u,"`");else if(!strcmp(tag,"strong")||!strcmp(tag,"b"))appends(t,cap,&u,"**");else if(!strcmp(tag,"em")||!strcmp(tag,"i"))appends(t,cap,&u,"_");else if(!strcmp(tag,"p")||!strcmp(tag,"div")||!strcmp(tag,"section")||!strcmp(tag,"article")||!strcmp(tag,"main")||!strcmp(tag,"header")||!strcmp(tag,"footer")||!strcmp(tag,"nav")||!strcmp(tag,"aside")||!strcmp(tag,"blockquote")||!strcmp(tag,"address")||!strcmp(tag,"li")||!strcmp(tag,"h1")||!strcmp(tag,"h2")||!strcmp(tag,"h3")||!strcmp(tag,"h4")||!strcmp(tag,"h5")||!strcmp(tag,"h6")||!strcmp(tag,"tr")||!strcmp(tag,"table"))newline(t,cap,&u);cur=after;continue;}
        if(!strcmp(tag,"head"))head=true;else if(!strcmp(tag,"script"))script=true;else if(!strcmp(tag,"style"))style=true;else if(!strcmp(tag,"pre")){newline(t,cap,&u);pre=true;}else if(!strcmp(tag,"br"))newline(t,cap,&u);
        else if(!strcmp(tag,"p")||!strcmp(tag,"div")||!strcmp(tag,"section")||!strcmp(tag,"article")||!strcmp(tag,"main")||!strcmp(tag,"header")||!strcmp(tag,"footer")||!strcmp(tag,"nav")||!strcmp(tag,"aside")||!strcmp(tag,"blockquote")||!strcmp(tag,"address")||!strcmp(tag,"tr"))newline(t,cap,&u);
        else if(tag[0]=='h'&&tag[1]>='1'&&tag[1]<='6'&&!tag[2]){newline(t,cap,&u);appends(t,cap,&u,"= ");}else if(!strcmp(tag,"ul"))ol=0;else if(!strcmp(tag,"ol")){ol++;item=0;}else if(!strcmp(tag,"li")){newline(t,cap,&u);if(ol){char b[16];snprintf(b,sizeof b,"%d. ",++item);appends(t,cap,&u,b);}else appends(t,cap,&u,"* ");}
        else if(!strcmp(tag,"code"))appends(t,cap,&u,"`");else if(!strcmp(tag,"strong")||!strcmp(tag,"b"))appends(t,cap,&u,"**");else if(!strcmp(tag,"em")||!strcmp(tag,"i"))appends(t,cap,&u,"_");else if(!strcmp(tag,"hr")){newline(t,cap,&u);appends(t,cap,&u,"--------------------------------\n");}else if(!strcmp(tag,"td")||!strcmp(tag,"th")){if(u&&t[u-1]!='\n'&&t[u-1]!=' ')appends(t,cap,&u," | ");}
        else if(!strcmp(tag,"form")){newline(t,cap,&u);if(p->form_count<MAX_FORMS){cf=p->form_count++;form_t*f=&p->forms[cf];snprintf(f->method,sizeof f->method,"get");attribute(attrs,te,"action",f->action,sizeof f->action);if(attribute(attrs,te,"method",f->method,sizeof f->method))lower(f->method);}else cf=-1;}
        else if(!strcmp(tag,"input")&&cf>=0){form_t*f=&p->forms[cf];if(f->field_count<MAX_FORM_FIELDS){form_field_t x={0};snprintf(x.type,sizeof x.type,"text");attribute(attrs,te,"name",x.name,sizeof x.name);attribute(attrs,te,"value",x.value,sizeof x.value);if(attribute(attrs,te,"type",x.type,sizeof x.type))lower(x.type);x.disabled=attribute(attrs,te,"disabled",NULL,0);bool ed=!strcmp(x.type,"text")||!strcmp(x.type,"search")||!strcmp(x.type,"url"),hidden=!strcmp(x.type,"hidden"),submit=!strcmp(x.type,"submit");if((ed&&x.name[0])||(hidden&&x.name[0])||submit){int fi=f->field_count++;f->fields[fi]=x;if(submit){snprintf(f->fields[fi].label,sizeof f->fields[fi].label,"%s",x.value[0]?x.value:"Submit");if(!x.disabled){int a=add_action(p,ACTION_FORM_SUBMIT,-1,cf,fi);if(a>=0)add_marker(t,cap,&u,a);}}else if(ed&&!x.disabled){int a=add_action(p,ACTION_FORM_FIELD,-1,cf,fi);if(a>=0)add_marker(t,cap,&u,a);}}}}
        else if(!strcmp(tag,"button")&&cf>=0){form_t*f=&p->forms[cf];char ty[16]="submit";attribute(attrs,te,"type",ty,sizeof ty);lower(ty);const char*close=find_ci(after,"</button>");if(!strcmp(ty,"submit")&&f->field_count<MAX_FORM_FIELDS){int fi=f->field_count++;form_field_t*x=&f->fields[fi];snprintf(x->type,sizeof x->type,"submit");attribute(attrs,te,"name",x->name,sizeof x->name);attribute(attrs,te,"value",x->value,sizeof x->value);x->disabled=attribute(attrs,te,"disabled",NULL,0);if(close)extract_button(after,close,x->label,sizeof x->label);if(!x->label[0])snprintf(x->label,sizeof x->label,"%s",x->value[0]?x->value:"Submit");if(!x->disabled){int a=add_action(p,ACTION_FORM_SUBMIT,-1,cf,fi);if(a>=0)add_marker(t,cap,&u,a);}}if(close){const char*ce=strchr(close,'>');cur=ce?ce+1:end;continue;}}
        else if(!strcmp(tag,"a")){char h[URL_MAX]="";if(attribute(attrs,te,"href",h,sizeof h)&&supported_href(h)&&p->link_count<MAX_LINKS){char abs[URL_MAX];resolve_url(p->base,h,abs,sizeof abs);if(supported_href(abs)){int li=p->link_count++;snprintf(p->links[li].href,sizeof p->links[li].href,"%s",abs);int a=add_action(p,ACTION_LINK,li,-1,-1);if(a>=0){char b[16];snprintf(b,sizeof b,"[%d]",a+1);appends(t,cap,&u,b);}}}}
        cur=after;
    }
    t[u]=0;p->text_template=t;if(!refresh_text(p)){free(t);free(p);return NULL;}return p;
}
static void free_page(page_t*p){if(p){free(p->text);free(p->text_template);free(p);}}

static bool urlencode(const char*s,char*o,size_t cap){static const char H[]="0123456789ABCDEF";size_t u=0;if(!cap)return false;for(size_t i=0;s&&s[i];i++){unsigned char c=s[i];if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~'){if(u+1>=cap)return false;o[u++]=(char)c;}else if(c==' '){if(u+1>=cap)return false;o[u++]='+';}else{if(u+3>=cap)return false;o[u++]='%';o[u++]=H[c>>4];o[u++]=H[c&15];}}o[u]=0;return true;}
static bool scat(char*o,size_t cap,const char*s){size_t a=strlen(o),b=strlen(s);if(a+b>=cap)return false;memcpy(o+a,s,b+1);return true;}
static int build_get(const page_t*p,int fi,int submit,char*out,size_t cap){
    if(fi<0||fi>=p->form_count)return 0;const form_t*f=&p->forms[fi];if(strcasecmp(f->method[0]?f->method:"get","get"))return-1;if(f->action[0])resolve_url(p->base,f->action,out,cap);else snprintf(out,cap,"%s",p->base);char*h=strchr(out,'#');if(h)*h=0;bool has=strchr(out,'?')!=NULL,first=true;
    for(int i=0;i<f->field_count;i++){const form_field_t*x=&f->fields[i];if(x->disabled||!x->name[0])continue;bool sub=!strcmp(x->type,"submit");bool ok=!strcmp(x->type,"text")||!strcmp(x->type,"search")||!strcmp(x->type,"url")||!strcmp(x->type,"hidden")||(sub&&i==submit);if(!ok)continue;char n[193],v[FORM_VALUE_MAX*3+1];if(!urlencode(x->name,n,sizeof n)||!urlencode(x->value,v,sizeof v))return 0;size_t l=strlen(out);char sep[2]={0};if(first){if(!l||(out[l-1]!='?'&&out[l-1]!='&'))sep[0]=has?'&':'?';}else if(l&&out[l-1]!='?'&&out[l-1]!='&')sep[0]='&';if(!scat(out,cap,sep)||!scat(out,cap,n)||!scat(out,cap,"=")||!scat(out,cap,v))return 0;first=false;has=true;}return 1;
}
static int fetch(const char*url,mem_t*m,long*status,char*effective,size_t ec){
    CURL*c=curl_easy_init();if(!c)return CURLE_FAILED_INIT;m->buf=NULL;m->len=0;*status=0;effective[0]=0;curl_easy_setopt(c,CURLOPT_URL,url);curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);curl_easy_setopt(c,CURLOPT_MAXREDIRS,5L);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,20L);curl_easy_setopt(c,CURLOPT_TIMEOUT,35L);curl_easy_setopt(c,CURLOPT_LOW_SPEED_LIMIT,10L);curl_easy_setopt(c,CURLOPT_LOW_SPEED_TIME,20L);curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,write_cb);curl_easy_setopt(c,CURLOPT_WRITEDATA,m);curl_easy_setopt(c,CURLOPT_USERAGENT,"Mozilla/5.0 (Linux; rv:2.1) Gecko/20100101 (compatible; MiniBrowser/2.1; +https://github.com/mactjaap/mini_browser/)");
    struct curl_slist*h=NULL;h=curl_slist_append(h,"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");h=curl_slist_append(h,"Accept-Language: en-US,en;q=0.5");h=curl_slist_append(h,"Accept-Encoding: identity");curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);CURLcode r=curl_easy_perform(c);curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,status);char*eu=NULL;if(r==CURLE_OK&&curl_easy_getinfo(c,CURLINFO_EFFECTIVE_URL,&eu)==CURLE_OK&&eu)snprintf(effective,ec,"%s",eu);curl_slist_free_all(h);curl_easy_cleanup(c);return r;
}
static int term_width(void){struct winsize w;if(!ioctl(STDOUT_FILENO,TIOCGWINSZ,&w)&&w.ws_col>=40)return w.ws_col;return 80;}
static char *wrap(const char*in,int cols){size_t n=strlen(in),cap=n+n/(cols?cols:1)+32,o=0;char*out=malloc(cap);if(!out)return NULL;int col=0,blank=0;for(size_t i=0;i<n;i++){char c=in[i];if(c=='\r')continue;if(c=='\n'){if(col==0){if(blank)continue;blank=1;}else blank=0;out[o++]='\n';col=0;continue;}if(c==' '&&(col==0||(o&&out[o-1]==' ')))continue;if(cols&&col>=cols){out[o++]='\n';col=0;if(c==' ')continue;}out[o++]=c;col++;blank=0;}out[o]=0;return out;}
static void hist_push(const char*u){if(!u||!*u)return;if(history_pos>=0&&!strncmp(history[history_pos],u,URL_MAX))return;if(history_pos<history_len-1)history_len=history_pos+1;if(history_len<HISTORY_MAX){snprintf(history[history_len],URL_MAX,"%s",u);history_pos=history_len++;}else{memmove(history,history+1,sizeof history[0]*(HISTORY_MAX-1));snprintf(history[HISTORY_MAX-1],URL_MAX,"%s",u);history_pos=HISTORY_MAX-1;}}
static bool hist_back(char*out){if(history_pos<=0)return false;snprintf(out,URL_MAX,"%s",history[--history_pos]);return true;}
static bool hist_forward(char*out){if(history_pos<0||history_pos+1>=history_len)return false;snprintf(out,URL_MAX,"%s",history[++history_pos]);return true;}
static const char *bookmark_path(void){static char p[1024];const char*h=getenv("HOME");snprintf(p,sizeof p,"%s/.mini_browser_bookmarks.txt",h?h:".");return p;}
static int bm_find(const char*u){for(int i=0;i<bookmark_count;i++)if(!strncmp(bookmarks[i].url,u,URL_MAX))return i;return-1;}
static bool bm_add(const char*u,const char*t){if(!u||!*u||bm_find(u)>=0||bookmark_count>=BOOKMARK_MAX)return false;snprintf(bookmarks[bookmark_count].url,URL_MAX,"%s",u);snprintf(bookmarks[bookmark_count].title,128,"%s",(t&&*t)?t:u);bookmark_count++;return true;}
static bool bm_remove(const char*u){int i=bm_find(u);if(i<0)return false;if(i<bookmark_count-1)memmove(&bookmarks[i],&bookmarks[i+1],sizeof bookmarks[0]*(bookmark_count-i-1));bookmark_count--;return true;}
static void bm_save(void){FILE*f=fopen(bookmark_path(),"w");if(!f){perror("bookmarks");return;}for(int i=0;i<bookmark_count;i++)fprintf(f,"%s\n%s\n",bookmarks[i].url,bookmarks[i].title);fclose(f);}
static void bm_load(void){FILE*f=fopen(bookmark_path(),"r");if(!f)return;char u[URL_MAX],t[128];while(bookmark_count<BOOKMARK_MAX&&fgets(u,sizeof u,f)&&fgets(t,sizeof t,f)){u[strcspn(u,"\r\n")]=0;t[strcspn(t,"\r\n")]=0;if(u[0])bm_add(u,t);}fclose(f);}
static void bm_show(void){printf("\nBookmarks (%d/%d)\n",bookmark_count,BOOKMARK_MAX);for(int i=0;i<bookmark_count;i++)printf("  %d. %s\n     %s\n",i+1,bookmarks[i].title,bookmarks[i].url);if(!bookmark_count)puts("  (none)");}
static void help(void){puts("\nCommands:\n  NUMBER       activate link/form field/submit\n  b            Back\n  fwd          Forward\n  g URL        go to URL (g alone prompts)\n  h            Home\n  r            Reload\n  l            list actions\n  f            add/remove bookmark\n  m            show bookmarks\n  o NUMBER     open bookmark\n  ?            help\n  q            quit");}
static void actions(const page_t*p){if(!p||!p->action_count){puts("No actions.");return;}puts("\nActions:");for(int i=0;i<p->action_count;i++){page_action_t*a=&p->actions[i];if(a->type==ACTION_LINK)printf(" [%d] LINK   %s\n",i+1,p->links[a->link_index].href);else{form_field_t*x=&p->forms[a->form_index].fields[a->field_index];if(a->type==ACTION_FORM_FIELD)printf(" [%d] FIELD  %s = %s\n",i+1,x->name,x->value);else printf(" [%d] SUBMIT %s\n",i+1,x->label[0]?x->label:"Submit");}}}
static void display(const page_t*p,const char*u,long status){int w=term_width();char*x=wrap(p&&p->text?p->text:"",w);printf("\033[2J\033[HMini Browser %s - Linux text-only\nURL: %s\n",VERSION,u);if(p&&p->title[0])printf("Title: %s\n",p->title);printf("HTTP: %ld   Actions: %d   Links: %d   Forms: %d   Limit: %d KiB\n",status,p?p->action_count:0,p?p->link_count:0,p?p->form_count:0,MAX_BYTES/1024);for(int i=0;i<w&&i<120;i++)putchar('-');putchar('\n');if(x)printf("%s\n",x);for(int i=0;i<w&&i<120;i++)putchar('-');putchar('\n');puts("NUMBER=activate  b=back  fwd=forward  g=URL  h=home  r=reload");puts("f=bookmark  m=bookmarks  o N=open bookmark  l=actions  ?=help  q=quit");free(x);}
static bool load(const char*request,page_t**pp,char*current,size_t cc,long*status,bool push){mem_t m={0};long st=0;char eff[URL_MAX]="";printf("Loading %s ...\n",request);fflush(stdout);int rc=fetch(request,&m,&st,eff,sizeof eff);if(rc!=CURLE_OK){fprintf(stderr,"CONNECTION FAILED: %s\n",curl_easy_strerror((CURLcode)rc));free(m.buf);return false;}if(st>=400){fprintf(stderr,"HTTP ERROR %ld\n",st);free(m.buf);return false;}const char*base=eff[0]?eff:request;page_t*p=html_to_page(m.buf?m.buf:"",base);free(m.buf);if(!p){puts("PARSE ERROR");return false;}free_page(*pp);*pp=p;snprintf(current,cc,"%s",base);*status=st;if(push)hist_push(current);display(p,current,st);return true;}
static int activate(page_t*p,int ai,char*nav,size_t nc){if(!p||ai<0||ai>=p->action_count)return 0;page_action_t*a=&p->actions[ai];if(a->type==ACTION_LINK){snprintf(nav,nc,"%s",p->links[a->link_index].href);return 1;}form_t*f=&p->forms[a->form_index];form_field_t*x=&f->fields[a->field_index];if(a->type==ACTION_FORM_FIELD){char b[FORM_VALUE_MAX];printf("%s [%s]: ",x->name,x->value);fflush(stdout);if(!fgets(b,sizeof b,stdin))return 0;b[strcspn(b,"\r\n")]=0;snprintf(x->value,sizeof x->value,"%s",b);if(!refresh_text(p))puts("Could not refresh form display.");return 2;}int r=build_get(p,a->form_index,a->field_index,nav,nc);if(r<0){puts("POST FORMS NOT SUPPORTED");return 0;}if(!r){puts("FORM URL TOO LONG");return 0;}return 1;}

int main(int argc,char**argv){
    char current[URL_MAX],requested[URL_MAX],cmd[1024];page_t*page=NULL;long status=0;snprintf(current,sizeof current,"%s",argc>1?argv[1]:HOME_URL);normalize_url(current);
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK){fputs("Could not initialize libcurl.\n",stderr);return 1;}bm_load();snprintf(requested,sizeof requested,"%s",current);if(!load(requested,&page,current,sizeof current,&status,true))puts("Type 'g URL' to try another address, or q to quit.");
    while(1){printf("\nmini-browser> ");fflush(stdout);if(!fgets(cmd,sizeof cmd,stdin))break;cmd[strcspn(cmd,"\r\n")]=0;trim(cmd);if(!cmd[0])continue;
        if(!strcmp(cmd,"q")||!strcmp(cmd,"quit"))break;else if(!strcmp(cmd,"?"))help();else if(!strcmp(cmd,"l"))actions(page);else if(!strcmp(cmd,"m"))bm_show();
        else if(!strcmp(cmd,"f")){if(!page||!http_url(current))puts("No HTTP page to bookmark.");else if(bm_find(current)>=0){bm_remove(current);bm_save();puts("BOOKMARK REMOVED");}else if(bm_add(current,page->title)){bm_save();puts("BOOKMARK ADDED");}else puts("Could not add bookmark.");}
        else if(!strncmp(cmd,"o ",2)){char*e;long n=strtol(cmd+2,&e,10);if(e==cmd+2||*e||n<1||n>bookmark_count)puts("Invalid bookmark number.");else load(bookmarks[n-1].url,&page,current,sizeof current,&status,true);}
        else if(!strcmp(cmd,"h"))load(HOME_URL,&page,current,sizeof current,&status,true);
        else if(!strcmp(cmd,"r"))load(current,&page,current,sizeof current,&status,false);
        else if(!strcmp(cmd,"b")){if(!hist_back(requested))puts("No previous page.");else load(requested,&page,current,sizeof current,&status,false);}
        else if(!strcmp(cmd,"fwd")||!strcmp(cmd,">")){if(!hist_forward(requested))puts("No forward page.");else load(requested,&page,current,sizeof current,&status,false);}
        else if(!strcmp(cmd,"g")){printf("URL: ");fflush(stdout);if(fgets(requested,sizeof requested,stdin)){requested[strcspn(requested,"\r\n")]=0;normalize_url(requested);if(requested[0])load(requested,&page,current,sizeof current,&status,true);}}
        else if(!strncmp(cmd,"g ",2)){snprintf(requested,sizeof requested,"%s",cmd+2);normalize_url(requested);if(requested[0])load(requested,&page,current,sizeof current,&status,true);}
        else{char*e;long n=strtol(cmd,&e,10);if(e!=cmd&&!*e){if(!page||n<1||n>page->action_count)puts("Invalid action number.");else{int r=activate(page,(int)n-1,requested,sizeof requested);if(r==1)load(requested,&page,current,sizeof current,&status,true);else if(r==2)display(page,current,status);}}else puts("Unknown command. Type ? for help.");}}
    free_page(page);curl_global_cleanup();puts("Bye.");return 0;
}
