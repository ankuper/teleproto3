/*
 * test_session_header.c — unit tests for Session Header parse/serialise.
 *
 * Not subject to banner-discipline (tests/, not src/).
 * Source: story 1.7 Task 8.1 + spec/wire-format.md §3.
 * Returns 0 on pass / 1 on fail.
 */

#include "t3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int hex_decode_buf(const char *hex, uint8_t *buf, int cap) {
    int len = (int)strlen(hex);
    if (len%2!=0||len/2>cap) return -1;
    for (int i=0;i<len/2;i++){
        char hi=hex[i*2],lo=hex[i*2+1];
        int h=(hi>='0'&&hi<='9')?hi-'0':(hi>='a'&&hi<='f')?hi-'a'+10:(hi>='A'&&hi<='F')?hi-'A'+10:-1;
        int l=(lo>='0'&&lo<='9')?lo-'0':(lo>='a'&&lo<='f')?lo-'a'+10:(lo>='A'&&lo<='F')?lo-'A'+10:-1;
        if(h<0||l<0) return -1;
        buf[i]=(uint8_t)((h<<4)|l);
    }
    return len/2;
}

static t3_result_t err_to_result(const char *err){
    if(!err) return T3_ERR_INTERNAL;
    if(strcmp(err,"MALFORMED")==0) return T3_ERR_MALFORMED;
    if(strcmp(err,"UNSUPPORTED_VERSION")==0) return T3_ERR_UNSUPPORTED_VERSION;
    if(strcmp(err,"INVALID_ARG")==0) return T3_ERR_INVALID_ARG;
    return T3_ERR_INTERNAL;
}

/* Non-mutating JSON helpers. Returns malloc'd string — caller frees. */
static char *jstr_alloc(const char *json, const char *key){
    char n[128]; snprintf(n,sizeof n,"\"%s\"",key);
    const char *p=strstr(json,n); if(!p) return NULL;
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

static int test_byteorder(void){
    uint8_t wire[4]={0x01,0x01,0x00,0x00};
    t3_header_t hdr;
    if(t3_header_parse(wire,&hdr)!=T3_OK||hdr.flags!=0){fprintf(stderr,"[bo] FAIL parse\n");return 1;}
    uint8_t out[4]={0};
    if(t3_header_serialise(&hdr,out)!=T3_OK||memcmp(wire,out,4)!=0){fprintf(stderr,"[bo] FAIL rt\n");return 1;}
    t3_header_t hdr2={0x01,0x01,0x0102};
    uint8_t wire2[4]; t3_header_serialise(&hdr2,wire2);
    if(wire2[2]!=0x02||wire2[3]!=0x01){fprintf(stderr,"[bo] FAIL LE\n");return 1;}
    printf("[byteorder] PASS\n"); return 0;
}

static int run_session_header_vectors(const char *path){
    FILE *f=fopen(path,"r"); if(!f){fprintf(stderr,"[vec] SKIP %s\n",path);return 0;}
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *buf=(char*)malloc(sz+1); fread(buf,1,sz,f); buf[sz]=0; fclose(f);
    char *arr=strstr(buf,"\"session-header\""); if(!arr){free(buf);return 0;}
    arr=strchr(arr,'['); if(!arr){free(buf);return 0;} arr++;
    int pass=0,fail=0,skip=0; char *p=arr;
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

        /* Non-mutating lookups first. */
        char *id=jstr_alloc(obj,"id");
        char *eb=strstr(obj,"\"expect\"");
        int ok=eb?jbool(eb,"ok"):-1;
        if(!id||ok<0){free(id);free(obj);skip++;continue;}

        /* Extract error string. */
        char *err_str=NULL;
        if(!ok) err_str=eb?jstr_alloc(eb,"error"):NULL;

        /* Hex args. */
        char hex[64]={0};
        const char *as=strstr(obj,"\"args\"");
        if(as){as=strchr(as,'[');
            if(as){as++;const char *ap=as;
                while(*ap&&*ap!=']'){
                    while(*ap&&*ap!='"'&&*ap!=']') ap++;
                    if(*ap=='"'){ap++;const char *hs=ap;
                        while(*ap&&*ap!='"') ap++;
                        size_t hl=(size_t)(ap-hs);
                        if(hl+strlen(hex)<sizeof(hex)-1) strncat(hex,hs,hl);
                        if(*ap=='"') ap++;}
                }
            }
        }

        uint8_t hb[4]={0}; int hl=0;
        if(strlen(hex)>0) hl=hex_decode_buf(hex,hb,4);

        t3_result_t r=T3_ERR_MALFORMED;
        if(hl==4){
            t3_header_t hdr; r=t3_header_parse(hb,&hdr);
            if(r==T3_OK&&ok){
                uint8_t re[4]; t3_result_t r2=t3_header_serialise(&hdr,re);
                if(r2!=T3_OK||memcmp(hb,re,4)!=0){
                    fprintf(stderr,"[vec] FAIL %s rt\n",id);free(id);free(err_str);free(obj);fail++;continue;}
            }
        }

        if(ok){
            if(r==T3_OK){printf("[vec] PASS %s\n",id);pass++;}
            else{fprintf(stderr,"[vec] FAIL %s exp ok got %s\n",id,t3_strerror(r));fail++;}
        } else {
            t3_result_t ex=err_to_result(err_str);
            if(r==ex){printf("[vec] PASS %s (%s)\n",id,err_str?err_str:"?");pass++;}
            else{fprintf(stderr,"[vec] FAIL %s exp %s got %d\n",id,err_str?err_str:"?",r);fail++;}
        }
        free(id); free(err_str); free(obj);
    }
    free(buf);
    printf("[vectors] session-header: %d pass, %d fail, %d skip\n",pass,fail,skip);
    return fail>0?1:0;
}

int main(int argc, char **argv){
    int rc=0;
    rc|=test_byteorder();
    t3_header_t hdr; uint8_t wire[4]={1,1,0,0};
    if(t3_header_parse(NULL,&hdr)!=T3_ERR_INVALID_ARG||
       t3_header_parse(wire,NULL)!=T3_ERR_INVALID_ARG||
       t3_header_serialise(NULL,wire)!=T3_ERR_INVALID_ARG||
       t3_header_serialise(&hdr,NULL)!=T3_ERR_INVALID_ARG)
        {fprintf(stderr,"[smoke] FAIL\n");rc=1;}
    else printf("[smoke] PASS NULL guards\n");
    const char *jpath=argc>1?argv[1]:"../../conformance/vectors/unit.json";
    rc|=run_session_header_vectors(jpath);
    return rc;
}
