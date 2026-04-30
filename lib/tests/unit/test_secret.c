/*
 * test_secret.c — unit tests for t3_secret_* API and conformance vectors.
 * Not subject to banner-discipline (tests/, not src/).
 * Source: story 1.7 Task 8.2. Returns 0 on pass / 1 on fail.
 */

#include "t3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int hex_decode(const char *hex, uint8_t *buf, size_t cap, size_t *out_len){
    size_t l=strlen(hex);
    if(l%2!=0||l/2>cap) return -1;
    for(size_t i=0;i<l/2;i++){
        char hi=hex[i*2],lo=hex[i*2+1];
        int h=(hi>='0'&&hi<='9')?hi-'0':(hi>='a'&&hi<='f')?hi-'a'+10:(hi>='A'&&hi<='F')?hi-'A'+10:-1;
        int lv=(lo>='0'&&lo<='9')?lo-'0':(lo>='a'&&lo<='f')?lo-'a'+10:(lo>='A'&&lo<='F')?lo-'A'+10:-1;
        if(h<0||lv<0) return -1;
        buf[i]=(uint8_t)((h<<4)|lv);
    }
    *out_len=l/2; return 0;
}

static t3_result_t err_str(const char *e){
    if(!e) return T3_ERR_INTERNAL;
    if(strcmp(e,"MALFORMED")==0) return T3_ERR_MALFORMED;
    if(strcmp(e,"INVALID_ARG")==0) return T3_ERR_INVALID_ARG;
    if(strcmp(e,"UNSUPPORTED_VERSION")==0) return T3_ERR_UNSUPPORTED_VERSION;
    return T3_ERR_INTERNAL;
}

/* Non-mutating: returns malloc'd string. */
static char *jstr_alloc(const char *j, const char *k){
    char n[128]; snprintf(n,sizeof n,"\"%s\"",k);
    const char *p=strstr(j,n); if(!p) return NULL;
    p+=strlen(n);
    while(*p==' '||*p=='\t'||*p==':'||*p=='\r'||*p=='\n') p++;
    if(*p!='"') return NULL; p++;
    const char *s=p; while(*p&&*p!='"') p++;
    size_t len=(size_t)(p-s);
    char *r=(char*)malloc(len+1); if(!r) return NULL;
    memcpy(r,s,len); r[len]='\0'; return r;
}

static int jbool(const char *j, const char *k){
    char n[128]; snprintf(n,sizeof n,"\"%s\"",k);
    const char *p=strstr(j,n); if(!p) return -1;
    p+=strlen(n);
    while(*p==' '||*p=='\t'||*p==':'||*p=='\r'||*p=='\n') p++;
    if(strncmp(p,"true",4)==0) return 1;
    if(strncmp(p,"false",5)==0) return 0;
    return -1;
}

static int run_vectors(const char *path){
    FILE *f=fopen(path,"r"); if(!f){fprintf(stderr,"[vec] SKIP %s\n",path);return 0;}
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *buf=(char*)malloc(sz+1); fread(buf,1,sz,f); buf[sz]=0; fclose(f);
    char *arr=strstr(buf,"\"secret-format\""); if(!arr){free(buf);return 0;}
    arr=strchr(arr,'['); if(!arr){free(buf);return 0;} arr++;
    int pass=0,fail=0,skip=0; char *p=arr;
    uint8_t dbuf[2048];
    while(*p&&*p!=']'){
        while(*p&&*p!='{'&&*p!=']') p++;
        if(*p!='{') break;
        int d=0; char *os=p;
        while(*p){
            if(*p=='"'){p++;while(*p&&*p!='"'){if(*p=='\\')p++;p++;}}
            else if(*p=='{') d++;
            else if(*p=='}'){d--;if(!d) break;}
            p++;
        }
        char *oe=p; if(*p=='}') p++;
        size_t ol=(size_t)(oe-os)+1;
        char *obj=(char*)malloc(ol+1); memcpy(obj,os,ol); obj[ol]=0;
        char *id=jstr_alloc(obj,"id");
        char *eb=strstr(obj,"\"expect\"");
        int ok=eb?jbool(eb,"ok"):-1;
        if(!id||ok<0){free(id);free(obj);skip++;continue;}
        char *es=ok?NULL:(eb?jstr_alloc(eb,"error"):NULL);
        char hex[2048]={0};
        const char *as=strstr(obj,"\"args\"");
        if(as){as=strchr(as,'[');if(as){as++;const char *ap=as;
            while(*ap&&*ap!=']'){
                while(*ap&&*ap!='"'&&*ap!=']') ap++;
                if(*ap=='"'){ap++;const char *hs=ap;while(*ap&&*ap!='"') ap++;
                    size_t hl=(size_t)(ap-hs);if(hl+strlen(hex)<sizeof(hex)-1) strncat(hex,hs,hl);
                    if(*ap=='"') ap++;}
            }
        }}
        size_t bl=0; t3_secret_t *s=NULL;
        if(strlen(hex)>0&&hex_decode(hex,dbuf,sizeof(dbuf),&bl)!=0){
            fprintf(stderr,"[vec] %s bad hex\n",id);free(id);free(es);free(obj);fail++;continue;}
        t3_result_t r=t3_secret_parse(bl>0?dbuf:NULL,bl,&s);
        if(ok){
            if(r==T3_OK&&s){printf("[vec] PASS %s\n",id);pass++;t3_secret_free(s);}
            else{fprintf(stderr,"[vec] FAIL %s exp ok got %s\n",id,t3_strerror(r));fail++;if(s)t3_secret_free(s);}
        } else {
            t3_result_t ex=err_str(es);
            if(r==ex){printf("[vec] PASS %s (%s)\n",id,es?es:"?");pass++;}
            else{fprintf(stderr,"[vec] FAIL %s exp %s got %s\n",id,es?es:"?",t3_strerror(r));fail++;}
            if(s) t3_secret_free(s);
        }
        free(id); free(es); free(obj);
    }
    free(buf);
    printf("[vectors] secret-format: %d pass, %d fail, %d skip\n",pass,fail,skip);
    return fail>0?1:0;
}

static int smoke(void){
    int rc=0; t3_secret_t *s=NULL;
    uint8_t valid[18]; memset(valid,0,18); valid[0]=0xFF; valid[17]='x';
    if(t3_secret_parse(valid,18,NULL)!=T3_ERR_INVALID_ARG){fprintf(stderr,"[s] null out\n");rc=1;}
    if(t3_secret_parse(NULL,1,&s)!=T3_ERR_INVALID_ARG){fprintf(stderr,"[s] null buf\n");rc=1;}
    if(t3_secret_parse(valid,1,&s)!=T3_ERR_MALFORMED){fprintf(stderr,"[s] short\n");rc=1;}
    uint8_t bad[18]; memset(bad,0,18); bad[0]=0xFE; bad[17]='x';
    if(t3_secret_parse(bad,18,&s)!=T3_ERR_MALFORMED){fprintf(stderr,"[s] marker\n");rc=1;}
    if(t3_secret_parse(valid,18,&s)!=T3_OK||!s){fprintf(stderr,"[s] valid\n");rc=1;}
    else t3_secret_free(s);
    if(t3_secret_validate_host("")!=T3_ERR_HOST_EMPTY){fprintf(stderr,"[s] empty\n");rc=1;}
    if(t3_secret_validate_host("example.com")!=T3_OK){fprintf(stderr,"[s] host\n");rc=1;}
    if(t3_secret_validate_path(NULL)!=T3_OK){fprintf(stderr,"[s] nil path\n");rc=1;}
    if(t3_secret_validate_path("x")!=T3_ERR_PATH_MISSING_LEADING_SLASH){fprintf(stderr,"[s] slash\n");rc=1;}
    if(t3_secret_validate_path("/ok")!=T3_OK){fprintf(stderr,"[s] path ok\n");rc=1;}
    t3_secret_free(NULL);
    if(!rc) printf("[smoke] PASS\n"); return rc;
}

int main(int argc, char **argv){
    int rc=smoke();
    const char *p=argc>1?argv[1]:"../../conformance/vectors/unit.json";
    rc|=run_vectors(p); return rc;
}
