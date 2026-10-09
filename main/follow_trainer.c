#include "follow_trainer.h"
#include <string.h>

void ft_begin(follow_trainer_t *t,uint32_t target_ms,bool rest,uint32_t count_in_ms,int64_t now_us)
{
    memset(t,0,sizeof(*t));t->target_ms=target_ms?target_ms:1;t->rest=rest;
    t->count_in_ms=t->remaining_ms=count_in_ms;t->last_us=now_us;
    t->phase=count_in_ms?FT_COUNT_IN:FT_READY;
    /* Every pitched note requires a fresh quiet boundary, including repeats. */
    t->armed=rest;
}
const char *ft_phase_name(ft_phase_t p)
{
    switch(p){case FT_COUNT_IN:return "count_in";case FT_READY:return "ready";
    case FT_HOLDING:return "holding";case FT_RETRY:return "retry";
    case FT_COMPLETE:return "complete";case FT_DONE:return "done";default:return "ready";}
}
bool ft_tick(follow_trainer_t *t,int64_t now_us,bool sounding,bool pitch_ok)
{
    if(now_us<=t->last_us||t->phase==FT_DONE)return false;
    int64_t delta=now_us-t->last_us;t->last_us=now_us;
    t->sounding=sounding;t->pitch_ok=pitch_ok;
    /* Never credit missing audio or the time spent in a voice/paused route. */
    if(delta>250000){
        /* A dropped/scheduled-late frame is not a new practice session.
         * Never credit unobserved time or erase preparation already completed. */
        t->previous_good=false;t->release_ms=0;
        if(t->phase==FT_COUNT_IN){
            uint64_t waited=(uint64_t)delta/1000;
            if(waited>=t->remaining_ms){t->remaining_ms=0;t->phase=FT_READY;t->armed=t->rest;}
            else t->remaining_ms-=(uint32_t)waited;
        }else if(t->phase==FT_HOLDING){
            t->phase=FT_RETRY;t->elapsed_ms=0;t->bad_ms=0;
            t->retry_reason=1;t->retries++;t->armed=t->rest;
        }else if(t->phase!=FT_COMPLETE){
            t->armed=t->rest;
        }
        return false;
    }
    uint32_t dt=(uint32_t)(delta/1000);if(dt>30)dt=30;
    if(!sounding){t->release_ms+=dt;if(t->release_ms>=80)t->armed=true;}
    else t->release_ms=0;
    if(t->phase==FT_COUNT_IN){
        if(dt>=t->remaining_ms){t->remaining_ms=0;t->phase=FT_READY;t->armed=t->rest;t->release_ms=0;}
        else t->remaining_ms-=dt;
        return false;
    }
    if(t->phase==FT_COMPLETE){
        t->complete_ms+=dt;
        if(!t->rest && sounding && t->complete_ms>600){
            t->phase=FT_RETRY;t->elapsed_ms=0;t->armed=false;
            t->previous_good=false;t->bad_ms=0;t->retry_reason=4;t->retries++;
            return false;
        }
        if(t->complete_ms>=600&&(t->rest||t->release_ms>=80)){t->phase=FT_DONE;return true;}
        return false;
    }
    bool good=t->rest?!sounding:(sounding&&pitch_ok&&t->armed);
    if(t->phase==FT_READY||t->phase==FT_RETRY){
        if(good){t->phase=FT_HOLDING;t->previous_good=true;t->bad_ms=0;}
        return false;
    }
    if(good){
        if(t->previous_good)t->elapsed_ms+=dt;
        t->bad_ms=0;t->previous_good=true;
        if(t->elapsed_ms>=t->target_ms){
            t->elapsed_ms=t->target_ms;t->phase=FT_COMPLETE;t->complete_ms=0;
        }
    }else{
        t->previous_good=false;t->bad_ms+=dt;
        /* Short glitches do not accumulate time; a sustained error retries. */
        uint32_t limit=sounding&&!pitch_ok&&!t->rest?180:120;
        if(t->bad_ms>=limit){
            t->elapsed_ms=0;t->bad_ms=0;t->phase=FT_RETRY;t->retries++;
            t->retry_reason=t->rest?3:sounding?2:1;
            if(!t->rest)t->armed=t->release_ms>=80;
        }
    }
    return false;
}
