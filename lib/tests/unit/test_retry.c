/*
 * test_retry.c — unit tests for FR43 retry-tier FSM.
 * Not subject to banner-discipline (tests/, not src/).
 * Source: story 1.7 Task 8.4 + spec/anti-probe.md §7.2.
 * Returns 0 on pass / 1 on fail.
 */

#include "t3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int stub_rng(void *ctx, uint8_t *buf, size_t len){(void)ctx;memset(buf,0xAB,len);return 0;}
static uint64_t stub_clock(void *ctx){(void)ctx;return 0;}
static int64_t stub_ls(void *ctx, const uint8_t *b, size_t l){(void)ctx;(void)b;(void)l;return 0;}
static int64_t stub_lr(void *ctx, uint8_t *b, size_t l){(void)ctx;(void)b;(void)l;return 0;}
static int64_t stub_fs(void *ctx, const uint8_t *b, size_t l, int f){(void)ctx;(void)b;(void)l;(void)f;return 0;}
static int64_t stub_fr(void *ctx, uint8_t *b, size_t c, int *o){(void)ctx;(void)b;(void)c;(void)o;return 0;}

static t3_session_t *make_session(void){
    uint8_t sb[18]; memset(sb,0,18); sb[0]=0xFF; sb[17]='x';
    t3_secret_t *sec=NULL;
    if(t3_secret_parse(sb,18,&sec)!=T3_OK) return NULL;
    t3_session_t *sess=NULL;
    if(t3_session_new(sec,&sess)!=T3_OK){t3_secret_free(sec);return NULL;}
    t3_callbacks_t cb; memset(&cb,0,sizeof cb);
    cb.struct_size=sizeof cb;
    cb.lower_send=stub_ls; cb.lower_recv=stub_lr;
    cb.frame_send=stub_fs; cb.frame_recv=stub_fr;
    cb.rng=stub_rng; cb.monotonic_ns=stub_clock;
    if(t3_session_bind_callbacks(sess,&cb)!=T3_OK){t3_session_free(sess);t3_secret_free(sec);return NULL;}
    return sess;
}

#define NS_S 1000000000ULL
#define ASSERT_S(sess,ts,exp,label) do{ \
    t3_retry_state_t st; \
    t3_result_t _r=t3_retry_record_close(sess,ts,&st); \
    if(_r!=T3_OK){fprintf(stderr,"[%s] record_close fail %d\n",label,_r);return 1;} \
    if(st!=(exp)){fprintf(stderr,"[%s] FAIL exp=%d got=%d\n",label,exp,st);return 1;} \
    printf("[%s] PASS state=%d\n",label,st); \
}while(0)

static int test_a(void){t3_session_t*s=make_session();if(!s)return 1;ASSERT_S(s,0,T3_RETRY_TIER1,"a");t3_session_free(s);return 0;}

static int test_b(void){
    t3_session_t*s=make_session();if(!s)return 1;
    ASSERT_S(s, 0*NS_S,T3_RETRY_TIER1,"b-1");
    ASSERT_S(s,10*NS_S,T3_RETRY_TIER1,"b-2");
    ASSERT_S(s,20*NS_S,T3_RETRY_TIER2,"b-3");
    t3_session_free(s);return 0;
}

static int test_c(void){
    t3_session_t*s=make_session();if(!s)return 1;
    for(int i=0;i<4;i++){t3_retry_state_t st;t3_retry_record_close(s,(uint64_t)i*5*NS_S,&st);}
    ASSERT_S(s,20*NS_S,T3_RETRY_TIER3,"c");
    t3_session_free(s);return 0;
}

static int test_d(void){
    t3_session_t*s=make_session();if(!s)return 1;
    ASSERT_S(s, 0*NS_S,T3_RETRY_TIER1,"d-1");
    ASSERT_S(s,10*NS_S,T3_RETRY_TIER1,"d-2");
    ASSERT_S(s,20*NS_S,T3_RETRY_TIER2,"d-tier2");
    uint64_t t5=20*NS_S+5*60*NS_S+NS_S;
    ASSERT_S(s,t5,T3_RETRY_TIER3,"d-5min");
    t3_session_free(s);return 0;
}

static int test_user_retry(void){
    t3_session_t*s=make_session();if(!s)return 1;
    for(int i=0;i<5;i++){t3_retry_state_t st;t3_retry_record_close(s,(uint64_t)i*2*NS_S,&st);}
    if(t3_retry_get_state(s)!=T3_RETRY_TIER3){fprintf(stderr,"[retry] not TIER3\n");t3_session_free(s);return 1;}
    if(t3_retry_user_retry(s)!=T3_OK){fprintf(stderr,"[retry] fail\n");t3_session_free(s);return 1;}
    if(t3_retry_get_state(s)!=T3_RETRY_OK){fprintf(stderr,"[retry] not OK\n");t3_session_free(s);return 1;}
    printf("[user_retry] PASS TIER3->OK\n");
    t3_retry_state_t st; t3_retry_record_close(s,100*NS_S,&st);
    if(t3_retry_user_retry(s)!=T3_ERR_INVALID_ARG){fprintf(stderr,"[retry] TIER1 should fail\n");t3_session_free(s);return 1;}
    printf("[user_retry] PASS TIER1->INVALID_ARG\n");
    t3_session_free(s);return 0;
}

static int test_clock_back(void){
    t3_session_t*s=make_session();if(!s)return 1;
    t3_retry_state_t st; t3_retry_record_close(s,100*NS_S,&st);
    if(t3_retry_record_close(s,50*NS_S,&st)!=T3_ERR_CLOCK_BACKWARDS){
        fprintf(stderr,"[clock] FAIL\n");t3_session_free(s);return 1;}
    printf("[clock_backwards] PASS\n");
    t3_session_free(s);return 0;
}

static int test_null(void){
    int rc=0; t3_retry_state_t st;
    if(t3_retry_record_close(NULL,0,&st)!=T3_ERR_INVALID_ARG){fprintf(stderr,"[null] 1\n");rc=1;}
    if(t3_retry_record_close((t3_session_t*)1,0,NULL)!=T3_ERR_INVALID_ARG){fprintf(stderr,"[null] 2\n");rc=1;}
    if(t3_retry_get_state(NULL)!=T3_RETRY_OK){fprintf(stderr,"[null] 3\n");rc=1;}
    if(t3_retry_user_retry(NULL)!=T3_ERR_INVALID_ARG){fprintf(stderr,"[null] 4\n");rc=1;}
    if(!rc) printf("[null_sess] PASS\n");
    return rc;
}

int main(void){
    int rc=0;
    rc|=test_null();
    rc|=test_a();
    rc|=test_b();
    rc|=test_c();
    rc|=test_d();
    rc|=test_user_retry();
    rc|=test_clock_back();
    return rc;
}
