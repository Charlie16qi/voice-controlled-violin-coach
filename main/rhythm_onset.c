#include "rhythm_onset.h"
#include <math.h>
#include <string.h>

void onset_reset(rhythm_onset_t *d) { memset(d, 0, sizeof(*d)); d->armed=true; }
float onset_pcm_rms(const int16_t *pcm, size_t n)
{
    double sum=0;
    for(size_t i=0;i<n;++i){float v=(float)pcm[i]/32768.0f;sum+=v*v;}
    return n?sqrtf((float)(sum/n)):0;
}
bool onset_envelope(rhythm_onset_t *d, float rms, float noise, int64_t us)
{
    float gate=fmaxf(0.0015f, noise*2.5f);
    if(rms < gate){if(++d->quiet>=2)d->armed=true;}
    else d->quiet=0;
    /* A bow dip has to be followed by an attack. A dip itself is not an onset.
       Ordinary +/-25% envelope modulation never rearms the detector. */
    if(d->peak > gate*2 && rms < d->peak*0.40f)d->armed=true;
    bool attack=rms>=gate*1.3f && rms>d->previous*1.18f;
    bool hit=d->armed && attack && (!d->last_onset_us || us-d->last_onset_us>=80000);
    if(hit){d->last_onset_us=us;d->armed=false;d->peak=rms;}
    d->peak=fmaxf(rms,d->peak*0.985f);
    d->previous=rms;
    return hit;
}
