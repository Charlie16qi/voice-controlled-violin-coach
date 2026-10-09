(() => {
  const $=id=>document.getElementById(id);
  const STRING_ID_TO_CN={1:"一弦 E",2:"二弦 A",3:"三弦 D",4:"四弦 G"};
  const MODE_LABEL={pitch:"音准",rhythm:"节奏",full:"完整"};
  const VOICE_PHASE={ready:'等待“你好小智”',unknown:'等待唤醒',manual_trigger:'网页已触发',wake_detected:'已唤醒',listening:'正在听命令…',recognizing:'讯飞识别中…',recognized:'已识别',no_speech:'没有听到命令',mic_error:'麦克风错误',wifi_error:'Wi-Fi错误',time_error:'时间同步错误',asr_error:'讯飞ASR错误',busy:'语音通道忙',cancelled:'旧识别已取消',unknown_command:'未识别成操作，请重说'};
  let polling=false,lastState=null,stateReceivedAt=performance.now(),audioCtx=null,lastClickBeat=null,pendingMode=null,pendingModeAt=0,pendingSong=null;

  async function jsonFetch(url,opt={}){const r=await fetch(url,opt),d=await r.json();if(!r.ok||!d.ok)throw new Error(d.error||d.message||`HTTP ${r.status}`);return d}
  function showError(m){const b=$("errorBox");b.hidden=false;b.textContent=m}function clearError(){$("errorBox").hidden=true}
  function escapeHtml(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
  function modeLabel(m){return MODE_LABEL[m]||m||'—'}
  function noteDurationName(beats){const b=Number(beats||0);if(Math.abs(b-4)<.05)return'全音符 · 4拍';if(Math.abs(b-3)<.05)return'附点二分 · 3拍';if(Math.abs(b-2)<.05)return'二分音符 · 2拍';if(Math.abs(b-1.5)<.05)return'附点四分 · 1.5拍';if(Math.abs(b-1)<.05)return'四分音符 · 1拍';if(Math.abs(b-.75)<.04)return'附点八分 · 0.75拍';if(Math.abs(b-.5)<.04)return'八分音符 · 0.5拍';if(Math.abs(b-.25)<.03)return'十六分音符 · 0.25拍';return`${b.toFixed(2)}拍`}

  async function refreshPorts(){try{const d=await jsonFetch('/api/coach/ports'),s=$("portSelect"),prev=s.value;s.innerHTML='';if(!d.ports.length){s.add(new Option(d.serial_available?'未检测到串口':'未安装 pyserial',''));return}d.ports.forEach(p=>s.add(new Option(`${p.device}${p.description?` · ${p.description}`:''}`,p.device)));if([...s.options].some(o=>o.value===prev))s.value=prev}catch(e){showError(e.message)}}
  async function refreshSongs(){try{const d=await jsonFetch('/api/songs'),s=$("songSelect");s.innerHTML='<option value="">请选择曲目</option>';d.songs.forEach(song=>s.add(new Option(`${song.title} · ${song.note_count}音 · ${Number(song.tempo_bpm).toFixed(0)} BPM`,song.title)))}catch(e){showError(e.message)}}
  async function connect(){const port=$("portSelect").value;if(!port){showError('请选择串口');return}try{clearError();await jsonFetch('/api/coach/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({port,baud:Number($("baudSelect").value)})});await poll(true)}catch(e){showError(`${e.message}。如果是拒绝访问，请关闭 idf.py monitor。`)}}
  async function disconnect(){try{await jsonFetch('/api/coach/disconnect',{method:'POST'});await poll(true)}catch(e){showError(e.message)}}
  async function command(command){try{clearError();ensureAudio();await jsonFetch('/api/coach/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({command})});setTimeout(()=>poll(true),70)}catch(e){showError(e.message)}}
  async function switchMode(mode){
    try{
      clearError();
      if(!lastState||!lastState.connected){showError('ESP32还没有连接，模式命令无法发送。');return}
      pendingMode=String(mode).toLowerCase();
      pendingModeAt=performance.now();
      await jsonFetch('/api/coach/command',{
        method:'POST',
        headers:{'Content-Type':'application/json'},
        body:JSON.stringify({command:`MODE:${String(mode).toUpperCase()}`})
      });
      setTimeout(()=>poll(true),80);
      setTimeout(async()=>{
        await poll(true);
        if(!pendingMode)return;
        const current=String(lastState?.practice_mode||'').toLowerCase();
        const fw=String(lastState?.firmware||'');
        const ack=lastState?.last_control_ack||{};
        if(current!==pendingMode){
          if(!lastState?.mode_control_supported){
            showError(`ESP32没有确认模式切换。当前固件：${fw||'未知/旧版'}。请检查新固件是否已烧录、串口是否连接。`);
          }else{
            showError(`模式命令未生效。当前=${current||'未知'}，目标=${pendingMode}，最近ACK=${ack.command||'无'} ${ack.value||''}`);
          }
        }
        pendingMode=null;
      },650);
    }catch(e){pendingMode=null;showError(e.message)}
  }
  async function startSong(){
    const q=$("songSelect").value;if(!q){showError('请先选择曲目');return}
    if(pendingSong)return;
    if(!lastState?.control_rx_confirmed){showError('板子尚未确认USB控制。请确认已烧录V10.7.0，并连接此前能接收命令的UART/USB口；部分原配置下原生USB只输出日志。');return;}
    try{
      clearError();ensureAudio();$("startSongButton").disabled=true;
      const d=await jsonFetch('/api/coach/start-song',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({query:q})});
      pendingSong={query:d.query,baseline:Number(d.baseline_seq||0),deadline:performance.now()+15000};
      await poll(true);
    }catch(e){pendingSong=null;$("startSongButton").disabled=false;showError(e.message)}
  }
  function checkSongResult(state){
    if(!pendingSong)return;
    const r=state.last_song_request||{},fresh=Number(state.song_request_seq||0)>pendingSong.baseline;
    if(fresh&&r.query===pendingSong.query&&['applied','unchanged','failed','cancelled'].includes(r.phase)){
      if(r.phase==='failed')showError(r.message||'板子未载入新曲，原进度保留');
      pendingSong=null;$("startSongButton").disabled=false;return;
    }
    if(performance.now()>pendingSong.deadline){showError('尚未收到板子换曲结果。请看换曲状态和串口连接；不要连续点重新开始。');pendingSong=null;$("startSongButton").disabled=false;}
  }
  async function reviewAction(kind){try{const url=kind==='start'?'/api/teacher/review/start':'/api/teacher/review/skip';await jsonFetch(url,{method:'POST'});await poll(true)}catch(e){showError(e.message)}}
  async function refreshHistory(){try{const d=await jsonFetch('/api/teacher/history?limit=10'),root=$("historyList");if(!d.sessions.length){root.textContent='暂无记录';return}root.innerHTML=d.sessions.map(s=>{const sum=s.summary||{},date=new Date(Number(s.ended_at)*1000).toLocaleString(),score=sum.overall_score==null?'—':Number(sum.overall_score).toFixed(0),rh=sum.rhythm_score==null?'—':Number(sum.rhythm_score).toFixed(0);return `<div class="history-row"><strong>${escapeHtml(s.song_title||'未命名')}</strong><small>${date}</small><span>${modeLabel(s.practice_mode||sum.practice_mode)} · 综合 ${score}</span><span>节奏 ${rh} · |cents| ${sum.mean_abs_cents==null?'—':Number(sum.mean_abs_cents).toFixed(1)}</span></div>`}).join('')}catch(e){showError(e.message)}}

  function renderSummary(t){const sum=t.summary||{},root=$("summaryCards"),scores=$("scoreSummary");if(!sum.top_notes||!sum.top_notes.length){root.innerHTML='<div class="empty-card">完成一首曲目后显示总结。</div>';scores.innerHTML='';$("summaryMeta").textContent='—';return}
    const val=(v)=>v==null?'—':Number(v).toFixed(0);
    scores.innerHTML=`<div class="score-pill"><small>综合</small><strong>${val(sum.overall_score)}</strong></div><div class="score-pill"><small>音准</small><strong>${val(sum.pitch_score)}</strong></div><div class="score-pill"><small>节奏</small><strong>${val(sum.rhythm_score)}</strong></div><div class="score-pill"><small>稳定度</small><strong>${val(sum.stability_score)}</strong></div>`;
    $("summaryMeta").textContent=`${modeLabel(sum.practice_mode)}模式 · 失败 ${sum.failure_count||0} · 跳过 ${sum.manual_skips||0}`;
    root.innerHTML=sum.top_notes.slice(0,3).map((n,i)=>`<div class="summary-card warn"><small>#${i+1} 弱点</small><strong>${escapeHtml(n.note||'—')}</strong><span>第 ${n.measure||'?'} 小节 · 音准 ${n.pitch_score==null?'—':Number(n.pitch_score).toFixed(0)} · 节奏 ${n.rhythm_score==null?'—':Number(n.rhythm_score).toFixed(0)}</span><span>${n.rhythm_onset_ms==null?'':`起音误差 ${Number(n.rhythm_onset_ms)>=0?'+':''}${Number(n.rhythm_onset_ms).toFixed(0)}ms · `}失败 ${n.failure_count||0}</span></div>`).join('');
  }

  function scoreNotes(state){return Array.isArray(state.score_notes)?state.score_notes:[]}
  function currentBeats(state){const notes=scoreNotes(state),idx=Number(state.song_index||0),e=notes[idx];return Number(e?.beats||1)}
  const beatClock=coachTrainer.createClock();
  const metroAudio=coachTrainer.createMetronome(()=>audioCtx,()=>$("metronomeSound").checked);
  function estimatedClockBeats(){return beatClock.beat();}

  function estimatedRhythmProgress(){if(!lastState)return 0;const target=Number(lastState.rhythm_target_ms||0);if(target<=0||!lastState.rhythm_started)return 0;let elapsed=Number(lastState.rhythm_elapsed_ms||0);if(lastState.app_state==='melody'&&lastState.practice_mode!=='pitch')elapsed+=Math.max(0,performance.now()-stateReceivedAt);return Math.max(0,Math.min(1,elapsed/target))}
  function ensureAudio(){try{if(!audioCtx)audioCtx=new (window.AudioContext||window.webkitAudioContext)();if(audioCtx.state==='suspended')audioCtx.resume()}catch(e){}}

  function clockLoop(){if(lastState){const meter=coachTrainer.meter(lastState),b=estimatedClockBeats()/meter.unit,beatInt=Math.floor(b),beat=(beatInt%meter.n+meter.n)%meter.n;coachTrainer.dots($("controlBeatDots"),meter.n);[...$("controlBeatDots").children].forEach((d,i)=>{d.classList.toggle('active',i===beat);d.classList.toggle('accent',i===0)});$("beatLabel").textContent=`${lastState.trainer_phase==='count_in'?'预备 · ':''}第 ${beat+1} 拍`;$("controlRhythmProgress").style.width=`${estimatedRhythmProgress()*100}%`;
      
    }requestAnimationFrame(clockLoop)}

  function render(state){lastState=state;stateReceivedAt=performance.now();beatClock.update(state);metroAudio.update(state,beatClock);const t=state.teacher||{},online=!!state.connected,mode=state.practice_mode||'pitch';$("connectionBadge").textContent=online?(state.control_rx_confirmed?`已连接 ${state.port}`:'串口已打开，等待板子控制确认'):'等待 ESP32';$("connectionBadge").classList.toggle('online',online);if($("firmwareStatus")){$("firmwareStatus").textContent=`ESP32固件：${state.firmware||'未知/旧版'}`;$("firmwareStatus").style.color=(state.firmware||'').includes('V10.7.0')?'#15803d':'#b45309';}
    const phrase=coachTrainer.feedback(state);
    if(phrase){t.teacher_text=phrase.text;t.teacher_subtext=phrase.sub;t.teacher_level=phrase.level;}
    checkSongResult(state);
    $("songSwitchStatus").textContent=coachTrainer.requestText(state);
    $("phraseRepeat").disabled=$("phraseSlowRepeat").disabled=mode==='pitch'||!state.song_active;
    $("phraseNext").disabled=mode==='pitch'||!state.song_active||!state.phrase_has_next;
    $("previousButton").textContent=mode==='pitch'?'← 上一音':'← 上一段';
    $("nextButton").textContent=mode==='pitch'?'下一音 / 跳过 →':'下一段 →';
    $("teacherText").textContent=t.teacher_text||'—';$("teacherSubtext").textContent=t.teacher_subtext||'—';$("teacherLevel").textContent=({good:'稳定',warn:'需要调整',bad:'明显错误',review:'强化',neutral:'提示'})[t.teacher_level]||t.teacher_level||'—';
    $("targetNote").textContent=state.target_note||'—';$("pitchHz").textContent=state.pitch_hz==null?'—':`${Number(state.pitch_hz).toFixed(2)}Hz`;$("centsText").textContent=state.pitch_cents==null?'—':`${Number(state.pitch_cents)>=0?'+':''}${Number(state.pitch_cents).toFixed(1)}c`;$("tempoText").textContent=state.tempo_bpm==null?'—':`${Number(state.tempo_bpm).toFixed(0)} BPM`;$("modeText").textContent=modeLabel(mode);$("modeBadge").textContent=`${modeLabel(mode)}模式`;
    ['modePitch','modeRhythm','modeFull'].forEach(id=>$(id).classList.remove('active'));$({pitch:'modePitch',rhythm:'modeRhythm',full:'modeFull'}[mode]||'modePitch').classList.add('active');if(pendingMode===mode){pendingMode=null;clearError();}
    if(document.activeElement!==$("tempoInput")&&state.tempo_bpm)$("tempoInput").value=Math.round(Number(state.tempo_bpm));if(document.activeElement!==$("rhythmTolerance"))$("rhythmTolerance").value=String(Number(state.rhythm_tolerance_ms||140));
    $("metronomeToggle").textContent=state.metronome_enabled?'关闭节拍器':'打开节拍器';$("metronomeToggle").classList.toggle('success',!!state.metronome_enabled);
    const beats=currentBeats(state),targetMs=Number(state.rhythm_target_ms||((60000/Number(state.tempo_bpm||90))*beats));const waiting=mode!=='pitch'&&!state.rhythm_started&&state.target_note!=='REST';$("noteDurationText").textContent=waiting?`${state.trainer_phase==='count_in'?'预备拍':'按拍推进'} · ${noteDurationName(beats)} · ${Math.round(targetMs)}ms`:`${noteDurationName(beats)} · ${Math.round(targetMs)}ms`;
    const count=Number(state.song_count||0),idx=Number(state.song_index||0),cur=count?Math.min(idx+1,count):0;$("songTitle").textContent=state.song_title||'未开始';$("songProgress").textContent=count?`${cur}/${count}`:'—';$("progressBar").style.width=count?`${cur/count*100}%`:'0%';
    const sid=Number(state.target_string_id||0),f=Number(state.target_finger);$("stringText").textContent=`弦：${sid?(STRING_ID_TO_CN[sid]||sid):'自动'}`;$("fingerText").textContent=`指法：${Number.isFinite(f)&&f>=0?(f===0?'空弦':`${f}指`):'—'}`;$("measureText").textContent=`小节：${state.target_measure||'—'} · 拍 ${state.target_beat||'—'}`;
    $("asrText").textContent=state.last_asr||'—';const i=state.last_intent||{};$("intentText").textContent=`Intent：${i.kind||'—'}${i.tempo_bpm?` · ${Number(i.tempo_bpm).toFixed(0)} BPM`:''}`;
    $("voicePhase").textContent=VOICE_PHASE[state.voice_phase]||state.voice_phase||'等待唤醒';$("voiceModel").textContent=`WakeNet ${state.voice_wake_model||'—'}`;
    const phase={playing:'练习中',review_countdown:`${t.review_countdown_s??''}s 后强化`,review:'强化中',review_done:'完成',review_skipped:'已跳过',idle:'待机'}[t.phase]||t.phase||'待机';$("reviewPhase").textContent=phase;$("reviewDescription").textContent=t.review_segment?`弱点段：第 ${t.review_segment.measure||'?'} 小节附近，索引 ${t.review_segment.start_index+1}–${t.review_segment.end_index+1}；慢速 ${Math.round((t.review_segment.speed_ratio||.7)*100)}%。`:'完成后可明确选择强化；短句模式用上方重练按钮。';
    const errors=state.rhythm_events||[];
    $("rhythmEvents").innerHTML=errors.slice(-12).map(e=>`<tr><td>${Number(e.index)+1}</td><td>${escapeHtml(e.note||'—')}</td><td>${e.onset_detected?`${Number(e.onset_ms)>=0?'+':''}${Number(e.onset_ms)} ms`:'未检测到清楚起音'}</td><td>${({good:'跟上',early:'早',late:'晚',missed:'未检测到',rest_noise:'休止时有声音'})[e.timing_status]||'—'}</td></tr>`).join('');
    if(document.activeElement!==$("rhythmOffset"))$("rhythmOffset").value=Number(state.rhythm_offset_ms||0);
    renderSummary(t);$("debugJson").textContent=JSON.stringify(state,null,2)
  }
  async function poll(force=false){if(polling)return;polling=true;try{const d=await jsonFetch('/api/coach/state');render(d.state)}catch(e){showError(e.message)}finally{polling=false}}

  $("metronomeSound").onchange=()=>{if($("metronomeSound").checked)ensureAudio();};
  $("refreshPorts").onclick=refreshPorts;$("connectButton").onclick=connect;$("disconnectButton").onclick=disconnect;$("startSongButton").onclick=startSong;
  $("phraseRepeat").onclick=()=>command('PHRASE_REPEAT');$("phraseSlowRepeat").onclick=()=>command('PHRASE_SLOW_REPEAT');$("phraseNext").onclick=()=>command('PHRASE_NEXT');$("restartSong").onclick=()=>command('RESTART');
  $("pauseButton").onclick=()=>command('PAUSE');$("continueButton").onclick=()=>command('CONTINUE');$("endButton").onclick=()=>command('END');$("previousButton").onclick=()=>command('PREVIOUS');$("nextButton").onclick=()=>command('NEXT');
  $("tempoDownButton").onclick=()=>command('TEMPO_DOWN');$("tempoUpButton").onclick=()=>command('TEMPO_UP');$("tempoResetButton").onclick=()=>command('TEMPO_RESET');$("voiceTestButton").onclick=()=>command('VOICE');
  $("tempoSetButton").onclick=()=>{const v=Math.max(40,Math.min(220,Number($("tempoInput").value||90)));command(`TEMPO:${v}`)};
  $("modePitch").onclick=()=>switchMode('pitch');$("modeRhythm").onclick=()=>switchMode('rhythm');$("modeFull").onclick=()=>switchMode('full');
  $("rhythmOffsetSet").onclick=()=>command(`RHYTHM_OFFSET:${Math.max(-300,Math.min(300,Number($("rhythmOffset").value)||0))}`);
  $("metronomeToggle").onclick=()=>command(lastState?.metronome_enabled?'METRONOME:OFF':'METRONOME:ON');$("metronomeSound").onchange=ensureAudio;$("rhythmTolerance").onchange=()=>command(`RHYTHM_TOLERANCE:${$("rhythmTolerance").value}`);
  $("startReviewButton").onclick=()=>reviewAction('start');$("skipReviewButton").onclick=()=>reviewAction('skip');$("refreshHistory").onclick=refreshHistory;
  document.addEventListener('keydown',e=>{if(['INPUT','SELECT','TEXTAREA'].includes(document.activeElement?.tagName))return;if(e.key==='ArrowRight'){e.preventDefault();command('NEXT')}else if(e.key==='ArrowLeft'){e.preventDefault();command('PREVIOUS')}else if(e.key===' '){e.preventDefault();command(lastState?.app_state==='paused'?'CONTINUE':'PAUSE')}});

  refreshPorts();refreshSongs();refreshHistory();poll(true);setInterval(poll,150);requestAnimationFrame(clockLoop);
})();
