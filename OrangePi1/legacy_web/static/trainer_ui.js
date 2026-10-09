(() => {
  const phases = {loading:'正在加载新曲，保留原进度', applied:'已切换', unchanged:'同曲，保留当前进度', failed:'加载失败，保留原曲', cancelled:'较早的换曲请求已取消'};
  window.coachTrainer = {
    createClock() {
      let key='',anchor=0,at=0,tempo=60,running=false,last=0;
      return {
        update(s,now=performance.now()) {
          const nextKey=`${s.song_id}|${s.phrase_run||0}|${s.practice_mode}`;
          const active=s.app_state==='melody' && s.song_active;
          const bpm=Number(s.tempo_bpm)||60;
          const packet=Number(s.metronome_clock_beats??s.song_clock_beats??0)
            +(active?(Number(s.clock_age_ms||0)+Number(s.clock_lag_ms||0))*bpm/60000:0);
          const estimate=anchor+(running?(now-at)*tempo/60000:0);
          if(nextKey!==key || active!==running || bpm!==tempo){anchor=packet;last=packet;}
          else { /* Smooth packet jitter; a stale sample cannot rewind the beat. */
            const correction=Math.max(-.04,Math.min(.04,packet-estimate));
            anchor=Math.max(last,estimate+correction*.15);
          }
          at=now;key=nextKey;tempo=bpm;running=active;
        },
        beat(now=performance.now()) {
          last=Math.max(last,anchor+(running?(now-at)*tempo/60000:0));return last;
        }
      };
    },
    createMetronome(getContext,isEnabled) {
      let timer=null,nodes=[],nextTime=null,lastKey='',previousBpm=0;
      function stop(){if(timer)clearInterval(timer);timer=null;nodes.forEach(o=>{try{o.stop()}catch(e){}});nodes=[];nextTime=null;}
      return {
        update(s,clock) {
          const ac=getContext(),active=isEnabled() && ac && ac.state==='running' &&
            s.connected && s.song_active && s.metronome_enabled && s.app_state==='melody';
          const bpm=Number(s.tempo_bpm)||60,meter=window.coachTrainer.meter(s);
          const key=`${s.song_id}|${s.phrase_run||0}|${s.practice_mode}|${meter.n}/${meter.unit}`;
          if(!active){stop();return;}
          if(key!==lastKey || bpm!==previousBpm){stop();lastKey=key;previousBpm=bpm;}
          if(timer)return;
          function schedule(){
            const b=clock.beat()/meter.unit,seconds=60/bpm*meter.unit,now=ac.currentTime;
            if(nextTime===null){const tick=Math.ceil(b+0.005);nextTime={at:now+(tick-b)*seconds,tick};}
            if(nextTime.at<now-.02){const tick=Math.ceil(b+.005);nextTime={at:now+(tick-b)*seconds,tick};}
            while(nextTime.at<now+.12){
              const when=Math.max(now+.002,nextTime.at),accent=((nextTime.tick%meter.n)+meter.n)%meter.n===0;
              const o=ac.createOscillator(),g=ac.createGain();o.frequency.value=accent?1050:690;
              g.gain.setValueAtTime(.0001,when);g.gain.exponentialRampToValueAtTime(accent?.13:.08,when+.006);
              g.gain.exponentialRampToValueAtTime(.0001,when+.055);o.connect(g).connect(ac.destination);
              nodes.push(o);o.onended=()=>{nodes=nodes.filter(n=>n!==o);};o.start(when);o.stop(when+.065);
              nextTime={at:nextTime.at+seconds,tick:nextTime.tick+1};
            }
          }
          schedule();timer=setInterval(schedule,25);
        },stop
      };
    },
    requestText(s) { const r=s.last_song_request||{}; return r.phase ? `${phases[r.phase]||r.phase} · ${r.query||''}${r.message?` · ${r.message}`:''}` : '可中途换曲；同曲保留进度。要从头练，请明确点“重新开始”。'; },
    followView(s) {
      const active=s.connected && s.song_active && s.app_state==='melody';
      const target=Math.max(1,Number(s.follow_target_ms||s.rhythm_target_ms||1));
      const elapsed=Math.min(target,Math.max(0,Number(s.follow_elapsed_ms)||0));
      const phase=s.follow_phase||'ready',rest=s.target_note==='REST';
      const times=`已${rest?'安静':'拉'} ${(elapsed/1000).toFixed(2)} 秒 / 目标 ${(target/1000).toFixed(2)} 秒`;
      let text='可以起弓：拉清楚当前音',level='neutral';
      if(phase==='done'&&s.connected&&s.app_state==='paused'){text='跟拍练习完成';level='good';}
      else if(!active)text=s.app_state==='paused'?'已暂停；点继续后重新预备当前音':s.app_state==='voice'||s.app_state==='cloud'?'正在听语音；当前音暂不计时':'等待板子连接并载入曲目';
      else if(phase==='count_in')text=`预备：还有 ${(Number(s.follow_remaining_prep_ms||0)/1000).toFixed(1)} 秒，先保持安静`;
      else if(phase==='complete'){text=rest?'休止已完成，准备下一音':'已拉满！请停弓，准备下一音';level='good';}
      else if(phase==='retry'){text=rest?'休止时有声音：重新保持安静':Number(s.follow_retry_reason)===4?'超过拉满提示后仍未停弓：停弓后重拉当前音':Number(s.follow_retry_reason)===2?'音高持续未达标：停弓后重拉当前音':'提前停弓，时长未满：重拉当前音';level='warn';}
      else if(phase==='holding'){
        text=rest?(s.follow_sounding?'休止时有声音，计时暂停':'保持安静，休止尚未完成'):
          (!s.follow_sounding?'声音中断，计时暂停':!s.follow_pitch_ok?'音高未达标，计时暂停':'继续拉，还没有拉满');
        level=rest?!s.follow_sounding?'neutral':'warn':s.follow_sounding&&s.follow_pitch_ok?'neutral':'warn';
      }else if(rest)text='可以开始休止：保持安静';
      else if(s.follow_sounding&&!s.follow_armed)text='可以开始了：先停弓，再拉当前音';
      else if(s.follow_sounding&&!s.follow_pitch_ok){text='听到了声音，但音高未达标；先停弓后重拉';level='warn';}
      return {text,sub:`${times} · 不自动跳过当前音 · 每音之间停弓`,times,level,
        progress:elapsed/target,phase,retries:Number(s.follow_retries||0)};
    },
    paintFollow(s,prefix) {
      const root=document.getElementById(prefix+'FollowPanel');if(!root)return;
      root.hidden=s.practice_mode!=='follow';if(root.hidden)return;
      const v=this.followView(s);
      root.dataset.level=v.level;
      document.getElementById(prefix+'FollowPrompt').textContent=v.text;
      document.getElementById(prefix+'FollowTime').textContent=v.times;
      document.getElementById(prefix+'FollowFill').style.width=`${v.progress*100}%`;
      const bar=document.getElementById(prefix+'FollowTrack');bar.setAttribute('aria-valuenow',Math.round(v.progress*100));
      document.getElementById(prefix+'FollowRetry').textContent=`当前音重试 ${v.retries} 次`;
    },
    feedback(s) {
      if(s.practice_mode==='follow')return this.followView(s);
      if(s.practice_mode==='pitch')return null;
      if(s.trainer_phase==='count_in')return {text:'预备拍：听完一小节，再从绿色目标音起弓',sub:'节奏测量起弓相对谱面起音的早晚；请用清楚的分弓。谱子按拍继续。',level:'neutral'};
      if(s.trainer_phase==='phrase_end')return {text:`本段结束：跟上 ${s.phrase_good||0} · 早 ${s.phrase_early||0} · 晚 ${s.phrase_late||0} · 未检测到 ${s.phrase_missed||0}`,sub:s.phrase_has_next?'选择：重练本段 / 慢速重练 / 下一段':'已到最后一段：可重练、明确重新开始或结束。',level:'neutral'};
      return null;
    },
    meter(s) {const n=Number(s.time_num)||4,d=Number(s.time_den)||4;return {n,unit:4/d};},
    dots(root,n) { if(!root||root.children.length===n)return;root.innerHTML='';for(let i=0;i<n;i++)root.appendChild(document.createElement('i')); }
  };
})();
