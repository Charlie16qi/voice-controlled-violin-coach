(() => {
  const $=id=>document.getElementById(id);
  const OPEN_MIDI={G:55,D:62,A:69,E:76},STRING_NAMES=["G","D","A","E"],STRING_ID_TO_NAME={1:"E",2:"A",3:"D",4:"G"},STRING_ID_TO_CN={1:"一弦 E",2:"二弦 A",3:"三弦 D",4:"四弦 G"},NOTE_BASE={C:0,D:2,E:4,F:5,G:7,A:9,B:11};
  const NUT_X=120,END_X=1390,TOP_LEFT=70,BOTTOM_LEFT=230,TOP_RIGHT=45,BOTTOM_RIGHT=255;
  const MODE_LABEL={pitch:"音准",rhythm:"连续测试",full:"完整",follow:"跟拍"};
  let lastState=null,stateReceivedAt=performance.now(),displayedLine=null,rolling=false,pendingState=null,lastAudioBeat=null,audioCtx=null,scoreKey=null,rollTimer=null,rollGeneration=0;
  const hudAudio=new URLSearchParams(location.search).get('audio')==='1';

  function noteToMidi(note){if(!note||note==='REST')return null;const m=String(note).match(/^([A-Ga-g])([#b]?)(-?\d+)$/);if(!m)return null;let s=NOTE_BASE[m[1].toUpperCase()];if(m[2]==='#')s++;if(m[2]==='b')s--;return(Number(m[3])+1)*12+s}
  const midiToHz=m=>440*Math.pow(2,(m-69)/12);
  function chooseString(note,sid){if(STRING_ID_TO_NAME[Number(sid)])return STRING_ID_TO_NAME[Number(sid)];const midi=noteToMidi(note);if(midi==null)return'G';const c=STRING_NAMES.map(n=>[n,OPEN_MIDI[n]]).filter(([,o])=>o<=midi).sort((a,b)=>b[1]-a[1]);return c.length?c[0][0]:'G'}
  const positionMm=(hz,s,L=328)=>{if(!hz||hz<=0)return null;const mm=L*(1-midiToHz(OPEN_MIDI[s])/hz);return Math.abs(mm)<.01?0:mm;};
  const centsError=(a,t)=>!a||!t?null:1200*Math.log2(a/t);
  function svg(tag,a={},text=''){const n=document.createElementNS('http://www.w3.org/2000/svg',tag);Object.entries(a).forEach(([k,v])=>n.setAttribute(k,v));if(text)n.textContent=text;return n}
  function clear(id){const n=$(id);while(n.firstChild)n.removeChild(n.firstChild);return n}
  function durationName(beats){const b=Number(beats||0);if(Math.abs(b-4)<.05)return'全音符 · 4拍';if(Math.abs(b-3)<.05)return'附点二分 · 3拍';if(Math.abs(b-2)<.05)return'二分音符 · 2拍';if(Math.abs(b-1.5)<.05)return'附点四分 · 1.5拍';if(Math.abs(b-1)<.05)return'四分音符 · 1拍';if(Math.abs(b-.75)<.04)return'附点八分 · 0.75拍';if(Math.abs(b-.5)<.04)return'八分音符 · 0.5拍';if(Math.abs(b-.25)<.03)return'十六分 · 0.25拍';return`${b.toFixed(2)}拍`}
  function scoreNotes(state){
    if(state.single_active)return [{note:state.target_note,index:0,beats:1,measure:1,beat:1,string_id:state.target_string_id}];
    const src=Array.isArray(state.score_notes)&&state.score_notes.length
      ?state.score_notes:(Array.isArray(state.song_notes)?state.song_notes:[]);
    let measure=1,beat=1;
    return src.filter(raw=>raw&&raw.note).map((raw,i)=>{
      const e={...raw,index:Number(raw.index??i)};
      e.beats=Number(e.beats)>0?Number(e.beats):1;
      if(Number(e.measure)>0)measure=Number(e.measure);
      if(Number(e.beat)>0)beat=Number(e.beat);
      e.measure=measure;e.beat=beat;
      beat+=e.beats;
      while(beat>4.0001){measure++;beat-=4;}
      return e;
    });
  }
  function staffStep(note){if(!note||note==='REST')return null;const m=String(note).match(/^([A-G])([#b]?)(-?\d+)$/);if(!m)return null;return Number(m[3])*7+({C:0,D:1,E:2,F:3,G:4,A:5,B:6})[m[1]]-(4*7+2)}
  function ledger(root,x,step,bottom,spacing){if(step<-1){for(let s=-2;s>=step;s-=2){const y=bottom-s*(spacing/2);root.appendChild(svg('line',{x1:x-14,y1:y,x2:x+14,y2:y,stroke:'#222','stroke-width':1.2}))}}if(step>9){for(let s=10;s<=step;s+=2){const y=bottom-s*(spacing/2);root.appendChild(svg('line',{x1:x-14,y1:y,x2:x+14,y2:y,stroke:'#222','stroke-width':1.2}))}}}
  function dotted(beats){return[3,1.5,.75,.375].some(v=>Math.abs(Number(beats)-v)<.035)}
  function drawFlag(root,stemX,stemY,up,count,color){for(let k=0;k<count;k++){const y=stemY+(up?k*10:-k*10);const d=up?`M ${stemX} ${y} q 20 8 18 27`:`M ${stemX} ${y} q -20 -8 -18 -27`;root.appendChild(svg('path',{d,fill:'none',stroke:color,'stroke-width':3,'stroke-linecap':'round'}))}}
  function drawNote(root,e,x,bottom,spacing,current,done){if(e.note==='REST'){root.appendChild(svg('text',{x,y:bottom-23,'text-anchor':'middle','font-size':27,fill:current?'#19a957':done?'#a5adb8':'#111820'},Number(e.beats)<1?'𝄾':'𝄽'));return}const step=staffStep(e.note);if(step==null)return;const y=bottom-step*(spacing/2),color=current?'#19a957':done?'#a5adb8':'#111820';ledger(root,x,step,bottom,spacing);if(current){root.appendChild(svg('circle',{cx:x,cy:y,r:21,fill:'#dff7e7',stroke:'#58c985','stroke-width':2.2}));root.appendChild(svg('text',{x,y:y-31,'text-anchor':'middle','font-size':13,'font-weight':800,fill:'#117b40'},`${e.note}${e.string_id?` · ${STRING_ID_TO_CN[e.string_id]}`:''}`))}
    const beats=Number(e.beats||1),whole=beats>=3.5,half=beats>=1.75&&!whole,hollow=whole||half;root.appendChild(svg('ellipse',{cx:x,cy:y,rx:10.5,ry:7.3,fill:hollow?'#fff':color,stroke:color,'stroke-width':2.3,transform:`rotate(-18 ${x} ${y})`}));
    if(!whole){const up=step<5,stemX=x+(up?8:-8),stemY=y+(up?-43:43);root.appendChild(svg('line',{x1:stemX,y1:y,x2:stemX,y2:stemY,stroke:color,'stroke-width':2.3}));if(beats<.9){drawFlag(root,stemX,stemY,up,beats<.4?2:1,color)}}
    if(dotted(beats))root.appendChild(svg('circle',{cx:x+18,cy:y-1,r:2.6,fill:color}));
    const acc=String(e.note).includes('#')?'♯':String(e.note).includes('b')?'♭':'';if(acc)root.appendChild(svg('text',{x:x-23,y:y+6,'font-size':21,fill:color},acc))
  }
  function renderScoreLine(svgId,lineIndex,notes,currentIndex){const root=clear(svgId),m0=lineIndex*4+1,m1=m0+3,left=105,right=1470,bottom=132,spacing=15,mw=(right-left)/4;for(let i=0;i<5;i++)root.appendChild(svg('line',{x1:left,y1:bottom-i*spacing,x2:right,y2:bottom-i*spacing,stroke:'#20252d','stroke-width':1.55}));root.appendChild(svg('text',{x:38,y:bottom-15,'font-size':65,fill:'#15191f'},'𝄞'));for(let mi=0;mi<=4;mi++){const x=left+mi*mw;root.appendChild(svg('line',{x1:x,y1:bottom-4*spacing,x2:x,y2:bottom+2,stroke:'#68717c','stroke-width':mi===0?1.5:1}));if(mi<4)root.appendChild(svg('text',{x:x+7,y:bottom-4*spacing-10,'font-size':13,fill:'#727d8c'},String(m0+mi)))}
    const lineEvents=notes.filter(n=>Number(n.measure)>=m0&&Number(n.measure)<=m1);if(!lineEvents.length){root.appendChild(svg('text',{x:760,y:96,'text-anchor':'middle','font-size':20,fill:'#9aa3af'},`第 ${m0}–${m1} 小节`));return}
    for(let m=m0;m<=m1;m++){const events=lineEvents.filter(n=>Number(n.measure)===m),ml=left+(m-m0)*mw,maxBeat=Math.max(4,...events.map(e=>Number(e.beat||1)+Number(e.beats||1)-1));events.forEach(e=>{const x=ml+28+(Number(e.beat||1)-1)/Math.max(1,maxBeat)*(mw-56);drawNote(root,e,x,bottom,spacing,e.index===currentIndex,e.index<currentIndex)})}
  }
  function currentLineIndex(state,notes){const idx=Math.max(0,Number(state.song_index||0)),event=notes.find(n=>n.index===idx)||notes[Math.min(idx,Math.max(0,notes.length-1))],measure=Math.max(1,Number(event?.measure||1));return Math.floor((measure-1)/4)}
  function renderTriple(line,notes,current){renderScoreLine('scoreLine0',line,notes,current);renderScoreLine('scoreLine1',line+1,notes,current);renderScoreLine('scoreLine2',line+2,notes,current);const m0=line*4+1;$("hudScorePosition").textContent=`当前 ${m0}–${m0+3} 小节 · 下一行 ${m0+4}–${m0+7}`}
  function resetRollingScore(){
    rollGeneration++;
    if(rollTimer!==null)clearTimeout(rollTimer);
    rollTimer=null;rolling=false;pendingState=null;displayedLine=null;
    const track=$("scoreTrack");
    track.style.transition='none';track.classList.remove('roll-forward');
    void track.offsetHeight;track.style.transition='';
  }
  function updateRollingScore(state){
    const key=JSON.stringify([state.single_active ? `single:${state.target_note}` : state.song_id||state.song_title||'',Number(state.song_run_seq||0)]);
    if(key!==scoreKey){scoreKey=key;resetRollingScore();}
    // Polling continues during animation. Only the newest snapshot is applied
    // after the slide settles, so rows are never shifted twice mid-animation.
    if(rolling){pendingState=state;return;}
    const notes=scoreNotes(state),current=Math.max(0,Number(state.song_index||0));
    if(!notes.length){
      displayedLine=null;renderTriple(0,[],current);
      $("hudScorePosition").textContent=state.song_active?'正在接收乐谱…':'等待开始曲目';
      return;
    }
    const wanted=currentLineIndex(state,notes);
    if(displayedLine===null||wanted===displayedLine){
      displayedLine=wanted;renderTriple(wanted,notes,current);return;
    }
    if(wanted===displayedLine+1){
      rolling=true;pendingState=state;renderTriple(displayedLine,notes,current);
      const track=$("scoreTrack"),generation=rollGeneration;
      requestAnimationFrame(()=>{
        if(generation===rollGeneration&&rolling)track.classList.add('roll-forward');
      });
      rollTimer=setTimeout(()=>{
        if(generation!==rollGeneration)return;
        const latest=pendingState||state,latestNotes=scoreNotes(latest);
        displayedLine=currentLineIndex(latest,latestNotes);
        track.style.transition='none';track.classList.remove('roll-forward');
        renderTriple(displayedLine,latestNotes,Math.max(0,Number(latest.song_index||0)));
        void track.offsetHeight;track.style.transition='';
        rolling=false;pendingState=null;rollTimer=null;
      },440);
      return;
    }
    displayedLine=wanted;renderTriple(wanted,notes,current);
  }
  function stringY(name,x){const idx=STRING_NAMES.indexOf(name),lf=[.20,.39,.60,.80],rf=[.17,.39,.61,.83],t=Math.max(0,Math.min(1,(x-NUT_X)/(END_X-NUT_X))),top=TOP_LEFT+(TOP_RIGHT-TOP_LEFT)*t,bottom=BOTTOM_LEFT+(BOTTOM_RIGHT-BOTTOM_LEFT)*t,f=lf[idx]+(rf[idx]-lf[idx])*t;return top+(bottom-top)*f}
  const xFromMm=(mm,display=220)=>NUT_X+Math.max(0,Math.min(1,mm/display))*(END_X-NUT_X);
  function renderBoard(state){const strings=clear('hudStrings'),ticks=clear('hudTicks'),target=clear('hudTargetLayer'),actual=clear('hudActualLayer'),move=clear('hudMoveLayer'),note=state.target_note||'',targetHz=Number(state.target_hz)||0,sid=Number(state.target_string_id||0),stringName=chooseString(note,sid),active=note&&note!=='REST'&&targetHz>0,displayLength=220;STRING_NAMES.forEach(n=>{const y1=stringY(n,NUT_X),y2=stringY(n,END_X),on=n===stringName&&active;strings.appendChild(svg('line',{x1:NUT_X,y1,x2:END_X,y2,stroke:on?'#fff':'#c6d0dd','stroke-width':on?4:2,opacity:on?1:.72}));strings.appendChild(svg('text',{x:82,y:y1+7,'text-anchor':'end','font-size':21,'font-weight':800,fill:on?'#fff':'#9ba9ba'},n))});for(let mm=20;mm<=displayLength;mm+=20){const x=xFromMm(mm,displayLength),t=(x-NUT_X)/(END_X-NUT_X),top=TOP_LEFT+(TOP_RIGHT-TOP_LEFT)*t;ticks.appendChild(svg('line',{x1:x,y1:top-12,x2:x,y2:top-3,stroke:'#718094','stroke-width':1}));if(mm%40===0)ticks.appendChild(svg('text',{x,y:top-18,'text-anchor':'middle','font-size':12,fill:'#8999ac'},String(mm)))}if(!active){$("hudBoardTitle").textContent=note==='REST'?'休止符 · 保持静音':'等待目标音';$("hudMovement").textContent='—';return}const targetMm=positionMm(targetHz,stringName);if(targetMm==null||targetMm<0){$("hudBoardTitle").textContent=`${note} 无法在 ${stringName} 弦得到`;return}const tx=xFromMm(targetMm,displayLength),ty=stringY(stringName,tx);target.appendChild(svg('circle',{cx:tx,cy:ty,r:16,fill:'#35d06f',stroke:'#c9ffdc','stroke-width':5,filter:'url(#hudShadow)'}));target.appendChild(svg('text',{x:tx+22,y:ty-18,'font-size':16,'font-weight':800,fill:'#83f2aa'},`目标 ${targetMm.toFixed(1)} mm`));let actualHz=null;try{if(state.pitch_hz!=null&&state.pitch_age_s!=null&&Number(state.pitch_age_s)<1.1)actualHz=Number(state.pitch_hz)}catch(e){}const label=sid?STRING_ID_TO_CN[sid]:`${stringName}弦（自动）`,f=Number(state.target_finger);$("hudBoardTitle").textContent=`${note} · ${label}${Number.isFinite(f)&&f>=0?` · ${f===0?'空弦':`${f}指`}`:''}`;if(actualHz==null){$("hudMovement").textContent='等待实际音高';return}const actualMm=positionMm(actualHz,stringName),cents=state.pitch_cents!=null?Number(state.pitch_cents):centsError(actualHz,targetHz),delta=targetMm-actualMm;if(actualMm>=0){const ax=xFromMm(actualMm,displayLength),ay=stringY(stringName,ax);actual.appendChild(svg('circle',{cx:ax,cy:ay,r:14,fill:'#ff5c62',stroke:'#ffd4d6','stroke-width':5,filter:'url(#hudShadow)'}));actual.appendChild(svg('text',{x:ax+20,y:ay+30,'font-size':15,'font-weight':800,fill:'#ff9ea2'},`实际 ${actualMm.toFixed(1)} mm`));if(Math.abs(tx-ax)>10)move.appendChild(svg('line',{x1:ax+(tx>ax?18:-18),y1:ay,x2:tx+(tx>ax?-20:20),y2:ty,stroke:'#ffd166','stroke-width':5,'stroke-linecap':'round','marker-end':'url(#hudArrow)'}))}if(Math.abs(cents)<=Number(state.tolerance_cents||30))$("hudMovement").textContent=`✓ 到位 · ${cents>=0?'+':''}${cents.toFixed(1)}c`;else $("hudMovement").textContent=`${delta>0?'向琴桥 →':'← 向琴枕'} ${Math.abs(delta).toFixed(1)} mm`}
  const beatClock=coachTrainer.createClock();
  const metroAudio=coachTrainer.createMetronome(()=>audioCtx,()=>hudAudio);
  function estimatedClockBeats(){return beatClock.beat();}

  function estimatedRhythmProgress(){if(!lastState)return 0;if(lastState.practice_mode==='follow')return coachTrainer.followView(lastState).progress;const target=Number(lastState.rhythm_target_ms||0);if(target<=0||!lastState.rhythm_started)return 0;let elapsed=Number(lastState.rhythm_elapsed_ms||0);if(lastState.app_state==='melody'&&lastState.practice_mode!=='pitch')elapsed+=Math.max(0,performance.now()-stateReceivedAt);return Math.max(0,Math.min(1,elapsed/target))}
  function currentScoreEvent(){if(!lastState)return null;const notes=scoreNotes(lastState);return notes.find(n=>n.index===Number(lastState.song_index||0))||null}
  function ensureAudio(){if(!hudAudio)return;try{if(!audioCtx)audioCtx=new(window.AudioContext||window.webkitAudioContext)();if(audioCtx.state==='suspended')audioCtx.resume()}catch(e){}}

  function animationLoop(){if(lastState){const meter=coachTrainer.meter(lastState),b=estimatedClockBeats()/meter.unit,bi=Math.floor(b),beat=(bi%meter.n+meter.n)%meter.n;coachTrainer.dots($("hudBeatDots"),meter.n);[...$("hudBeatDots").children].forEach((d,i)=>{d.classList.toggle('active',i===beat);d.classList.toggle('accent',i===0)});$("hudBeatLabel").textContent=`${lastState.trainer_phase==='count_in'?'预备 · ':''}第 ${beat+1} 拍`;$("hudNoteTimeBar").style.width=`${estimatedRhythmProgress()*100}%`;}requestAnimationFrame(animationLoop)}
  function render(state){lastState=state;stateReceivedAt=performance.now();beatClock.update(state);metroAudio.update(state,beatClock);const teacher=state.teacher||{},online=!!state.connected,mode=state.practice_mode||'pitch';$("hudConnection").textContent=online?'ESP32 已连接':'等待 ESP32';$("hudConnection").classList.toggle('online',online);const vp=state.voice_phase||'unknown',voiceMap={ready:'说“你好小智”',unknown:'说“你好小智”',manual_trigger:'准备语音',wake_detected:'✓ 已唤醒',listening:'🎙 正在听命令',recognizing:'☁ 正在识别',recognized:'✓ 命令已识别',no_speech:'没听到命令',mic_error:'麦克风错误',wifi_error:'Wi-Fi错误',time_error:'时间错误',asr_error:'ASR错误',busy:'语音忙',cancelled:'旧识别已取消',unknown_command:'命令未识别，请重说'},voiceEl=$("hudVoiceState");voiceEl.textContent=voiceMap[vp]||vp;voiceEl.classList.toggle('active',['manual_trigger','wake_detected','listening','recognizing'].includes(vp));$("hudSongTitle").textContent=state.song_title||'等待曲目';$("hudTarget").textContent=state.target_note||'—';const sid=Number(state.target_string_id||0),f=Number(state.target_finger);$("hudTargetMeta").textContent=`${sid?STRING_ID_TO_CN[sid]:'弦自动'}${Number.isFinite(f)&&f>=0?` · ${f===0?'空弦':`${f}指`}`:''}`;const count=Number(state.song_count||0),idx=Number(state.song_index||0),cur=count?Math.min(idx+1,count):0;$("hudProgress").textContent=count?`${cur}/${count}`:'—';$("hudProgressBar").style.width=count?`${Math.max(0,Math.min(100,cur/count*100))}%`:'0%';
    $("hudMode").textContent=MODE_LABEL[mode]||mode;$("hudTempo").textContent=state.tempo_bpm?`${Number(state.tempo_bpm).toFixed(0)} BPM`:'— BPM';const event=currentScoreEvent(),beats=Number(event?.beats||1),targetMs=Number(state.rhythm_target_ms||((60000/Number(state.tempo_bpm||90))*beats));const waiting=mode!=='pitch'&&!state.rhythm_started&&state.target_note!=='REST';$("hudDurationName").textContent=mode==='pitch'?(state.single_active?'单音练习：保持当前音':'音准模式：拉准即切换'):(waiting?`${state.trainer_phase==='count_in'?'预备拍':'按拍推进'} · ${durationName(beats)}`:durationName(beats));$("hudDurationMs").textContent=mode==='pitch'?(state.single_active?'语音切换目标音':'快速推进'):(waiting?'听拍点，准备起弓':`${Math.round(targetMs)} ms`);$("hudMetroState").textContent=state.metronome_enabled?'节拍器开':'节拍器关';$("hudMetroState").classList.toggle('on',!!state.metronome_enabled);
    document.body.classList.toggle('follow-mode',mode==='follow');
    coachTrainer.paintFollow(state,'hud');
    if(mode==='follow'){$("hudDurationName").textContent=durationName(beats);$("hudDurationMs").textContent=`目标 ${(Number(state.follow_target_ms||targetMs)/1000).toFixed(2)} 秒`;}
    const phrase=coachTrainer.feedback(state);if(phrase){teacher.teacher_text=phrase.text;teacher.teacher_subtext=phrase.sub;teacher.teacher_level=phrase.level;}
    $("hudSongSwitch").textContent=coachTrainer.requestText(state);
    $("hudTeacherText").textContent=teacher.teacher_text||'准备好以后开始练习';$("hudTeacherSubtext").textContent=teacher.teacher_subtext||'—';const panel=$("teacherPanel");panel.className=`teacher-zone ${teacher.teacher_level||'neutral'}`;const phase={playing:'实时指导',review_countdown:`${teacher.review_countdown_s??''}s 后强化`,review:'弱点强化',review_done:'强化完成',review_skipped:'已跳过强化',idle:'待机'}[teacher.phase]||teacher.phase||'实时指导';$("reviewStatus").textContent=phase;updateRollingScore(state);if(state.single_active){$("hudScorePosition").textContent=`单音 ${state.target_note} · 不自动推进`;clear('scoreLine1');clear('scoreLine2');}renderBoard(state)}
  async function poll(){try{const r=await fetch('/api/coach/state',{cache:'no-store'}),d=await r.json();if(d.ok)render(d.state)}catch(e){$("hudConnection").textContent='服务器连接失败'}finally{setTimeout(poll,150)}}
  poll();requestAnimationFrame(animationLoop);if(hudAudio)document.addEventListener('click',ensureAudio,{once:true});
})();
