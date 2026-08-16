/*
 * Web Audio API over the native engine.
 *
 * The DSP is webaudio-node's C++ engine compiled natively; this is the spec-shaped
 * surface on top of it. Games use a small, very consistent slice of Web Audio:
 * decodeAudioData, a BufferSource, a GainNode, and connect() to destination. That
 * path is implemented properly; the rest is present where cheap and throws by name
 * where it is not, so a missing feature is visible rather than silent.
 *
 * The engine's param IDs are positional (see audio_graph_simple.cpp's ParamID enum);
 * they are mirrored here rather than passed as strings, which is what the engine
 * itself does to avoid per-call string marshalling.
 */

const PARAM = {
  frequency: 0, detune: 1, gain: 2, Q: 3, delayTime: 4, pan: 5, offset: 6,
  type: 7, playbackOffset: 8, playbackDuration: 9, loop: 10,
  loopStart: 11, loopEnd: 12,
};

const OSC_TYPE = { sine: 0, square: 1, sawtooth: 2, triangle: 3 };

export function installAudio(g) {
  const host = g.__jsglq_audio;
  if (!host) return;

  class AudioParam {
    constructor(ctx, nodeId, paramId, defaultValue) {
      this._ctx = ctx;
      this._node = nodeId;
      this._id = paramId;
      this._value = defaultValue;
    }
    get value() { return this._value; }
    set value(v) {
      this._value = v;
      host.setParam(this._node, this._id, v);
    }
    setValueAtTime(v, when) {
      this._value = v;
      host.scheduleParam(this._node, this._id, 0, v, when, 0);
      return this;
    }
    linearRampToValueAtTime(v, when) {
      this._value = v;
      host.scheduleParam(this._node, this._id, 1, v, when, 0);
      return this;
    }
    exponentialRampToValueAtTime(v, when) {
      this._value = v;
      host.scheduleParam(this._node, this._id, 2, v, when, 0);
      return this;
    }
    setTargetAtTime(v, when, tc) {
      this._value = v;
      host.scheduleParam(this._node, this._id, 3, v, when, tc);
      return this;
    }
    cancelScheduledValues(when) {
      host.scheduleParam(this._node, this._id, 4, 0, when, 0);
      return this;
    }
  }

  class AudioNode {
    constructor(ctx, id) {
      this.context = ctx;
      this._id = id;
      this.numberOfInputs = 1;
      this.numberOfOutputs = 1;
      this.channelCount = 2;
    }
    connect(dest) {
      if (dest instanceof AudioParam) {
        throw new Error(
          'connect(AudioParam) is not implemented — audio-rate parameter modulation ' +
          'is a known gap in the underlying engine (connectToParam is a stub there). ' +
          'This throws rather than connecting nothing.');
      }
      const target = dest && dest._id !== undefined ? dest._id : 0;
      host.connect(this._id, target);
      return dest;
    }
    disconnect() { host.disconnect(this._id); }
  }

  class GainNode extends AudioNode {
    constructor(ctx) {
      super(ctx, host.createNode('gain'));
      this.gain = new AudioParam(ctx, this._id, PARAM.gain, 1);
    }
  }

  class OscillatorNode extends AudioNode {
    constructor(ctx) {
      super(ctx, host.createNode('oscillator'));
      this.frequency = new AudioParam(ctx, this._id, PARAM.frequency, 440);
      this.detune = new AudioParam(ctx, this._id, PARAM.detune, 0);
      this._type = 'sine';
      this.onended = null;
    }
    get type() { return this._type; }
    set type(v) {
      if (!(v in OSC_TYPE)) {
        throw new Error(`OscillatorNode.type '${v}' is not supported ` +
                        `(have: ${Object.keys(OSC_TYPE).join(', ')}). ` +
                        `'custom' needs setPeriodicWave, which the engine stubs out.`);
      }
      this._type = v;
      host.setParam(this._id, PARAM.type, OSC_TYPE[v]);
    }
    start(when = 0) { host.startNode(this._id, when); }
    stop(when = 0) { host.stopNode(this._id, when); }
    setPeriodicWave() {
      throw new Error('setPeriodicWave is not implemented (the engine stubs ' +
                      'setNodePeriodicWave). Use a standard oscillator type.');
    }
  }

  class AudioBuffer {
    constructor({ numberOfChannels, length, sampleRate, data }) {
      this.numberOfChannels = numberOfChannels;
      this.length = length;
      this.sampleRate = sampleRate;
      this.duration = length / sampleRate;
      this._data = data;          // Float32Array, interleaved
      this._bufferId = -1;
    }
    getChannelData(ch) {
      // De-interleave on demand. Games use this for waveform display far more
      // often than for playback, so a copy is fine and correctness matters more.
      const out = new Float32Array(this.length);
      for (let i = 0; i < this.length; i++) out[i] = this._data[i * this.numberOfChannels + ch];
      return out;
    }
    copyFromChannel(dest, ch, start = 0) {
      const src = this.getChannelData(ch);
      dest.set(src.subarray(start, start + dest.length));
    }
  }

  class AudioBufferSourceNode extends AudioNode {
    constructor(ctx) {
      super(ctx, host.createNode('buffersource'));
      this._buffer = null;
      this.loop = false;
      this.onended = null;
      this.playbackRate = new AudioParam(ctx, this._id, PARAM.detune, 1);
    }
    get buffer() { return this._buffer; }
    set buffer(b) {
      this._buffer = b;
      if (!b) return;
      if (b._bufferId < 0) {
        b._bufferId = host.registerBuffer(b._data, b.numberOfChannels, b.sampleRate);
      }
      host.setNodeBuffer(this._id, b._bufferId);
    }
    start(when = 0) {
      if (this.loop) host.setParam(this._id, PARAM.loop, 1);
      host.startNode(this._id, when);
      if (this.onended && this._buffer && !this.loop) {
        // Fire onended off a timer, which is what games use it for (chaining
        // sounds). Sample-accurate completion would need an engine callback.
        g.setTimeout(() => { try { this.onended({ type: 'ended' }); } catch (_) {} },
                     this._buffer.duration * 1000);
      }
    }
    stop(when = 0) { host.stopNode(this._id, when); }
  }

  class BiquadFilterNode extends AudioNode {
    constructor(ctx) {
      super(ctx, host.createNode('biquad'));
      this.frequency = new AudioParam(ctx, this._id, PARAM.frequency, 350);
      this.Q = new AudioParam(ctx, this._id, PARAM.Q, 1);
      this.gain = new AudioParam(ctx, this._id, PARAM.gain, 0);
      this.type = 'lowpass';
    }
  }

  class StereoPannerNode extends AudioNode {
    constructor(ctx) {
      super(ctx, host.createNode('stereopanner'));
      this.pan = new AudioParam(ctx, this._id, PARAM.pan, 0);
    }
  }

  class DelayNode extends AudioNode {
    constructor(ctx) {
      super(ctx, host.createNode('delay'));
      this.delayTime = new AudioParam(ctx, this._id, PARAM.delayTime, 0);
    }
  }

  class AnalyserNode extends AudioNode {
    constructor(ctx) {
      super(ctx, host.createNode('analyser'));
      this.fftSize = 2048;
      this.frequencyBinCount = 1024;
      this.smoothingTimeConstant = 0.8;
    }
    // The engine's analyser getters are unimplemented upstream (documented TODOs).
    // Returning silence would make a visualizer look broken with no explanation.
    getByteFrequencyData() {
      throw new Error('AnalyserNode data getters are not implemented in the ' +
                      'underlying engine (getFloat/ByteFrequencyData are TODOs there).');
    }
    getFloatFrequencyData() { this.getByteFrequencyData(); }
    getByteTimeDomainData() { this.getByteFrequencyData(); }
    getFloatTimeDomainData() { this.getByteFrequencyData(); }
  }

  class AudioDestinationNode extends AudioNode {
    constructor(ctx) {
      super(ctx, 0);              // node 0 is the engine's destination
      this.maxChannelCount = 2;
    }
  }

  class BaseAudioContext {
    constructor() {
      this.sampleRate = host.init();
      this.state = 'running';     // no autoplay gate: there is no browser policy here
      this.destination = new AudioDestinationNode(this);
      this.listener = { setPosition() {}, positionX: { value: 0 } };
    }
    get currentTime() { return host.currentTime(); }

    createGain() { return new GainNode(this); }
    createOscillator() { return new OscillatorNode(this); }
    createBufferSource() { return new AudioBufferSourceNode(this); }
    createBiquadFilter() { return new BiquadFilterNode(this); }
    createStereoPanner() { return new StereoPannerNode(this); }
    createDelay() { return new DelayNode(this); }
    createAnalyser() { return new AnalyserNode(this); }

    createBuffer(channels, length, sampleRate) {
      return new AudioBuffer({
        numberOfChannels: channels, length, sampleRate,
        data: new Float32Array(length * channels),
      });
    }

    async decodeAudioData(arrayBuffer, onSuccess, onError) {
      try {
        const info = g.__jsglq_decodeAudio(arrayBuffer);
        const buf = new AudioBuffer({
          numberOfChannels: info.channels,
          length: info.frames,
          sampleRate: info.sampleRate,
          data: new Float32Array(info.pcm),
        });
        if (onSuccess) onSuccess(buf);
        return buf;
      } catch (err) {
        if (onError) { onError(err); return null; }
        throw err;
      }
    }

    resume() { this.state = 'running'; return Promise.resolve(); }
    suspend() { return Promise.resolve(); }
    close() { return Promise.resolve(); }
  }

  class AudioContext extends BaseAudioContext {}

  g.AudioContext = AudioContext;
  g.webkitAudioContext = AudioContext;
  g.BaseAudioContext = BaseAudioContext;
  g.AudioNode = AudioNode;
  g.AudioParam = AudioParam;
  g.AudioBuffer = AudioBuffer;
  g.GainNode = GainNode;
  g.OscillatorNode = OscillatorNode;
  g.AudioBufferSourceNode = AudioBufferSourceNode;
  g.BiquadFilterNode = BiquadFilterNode;
  g.StereoPannerNode = StereoPannerNode;
  g.DelayNode = DelayNode;
  g.AnalyserNode = AnalyserNode;
  g.AudioDestinationNode = AudioDestinationNode;

  /*
   * HTMLAudioElement.
   *
   * Backed by the same engine as Web Audio rather than being a logging stub (which
   * is what rungame ships — its <audio> plays nothing at all). Games and engines
   * like Phaser construct one during feature detection even when they end up using
   * Web Audio, so `document.createElement('audio')` throwing stops them before
   * they start.
   */
  class HTMLAudioElement {
    constructor(src) {
      this._ctx = null;
      this._buffer = null;
      this._source = null;
      this._src = '';
      this.loop = false;
      this.autoplay = false;
      this.preload = 'auto';
      this.currentTime = 0;
      this.duration = NaN;
      this.paused = true;
      this.ended = false;
      this.readyState = 0;
      this._volume = 1;
      this._listeners = new Map();
      if (src) this.src = src;
    }

    get volume() { return this._volume; }
    set volume(v) { this._volume = Math.max(0, Math.min(1, Number(v) || 0)); }

    get src() { return this._src; }
    set src(v) {
      this._src = String(v);
      this.readyState = 0;
      this._load();
    }

    async _load() {
      try {
        this._ctx = this._ctx || new AudioContext();
        const res = await g.fetch(this._src);
        if (!res.ok) throw new Error(`audio fetch failed: ${this._src} (${res.status})`);
        this._buffer = await this._ctx.decodeAudioData(await res.arrayBuffer());
        this.duration = this._buffer.duration;
        this.readyState = 4;
        this._fire('canplaythrough');
        this._fire('loadeddata');
        if (this.autoplay) this.play();
      } catch (err) {
        console.error(`[Audio] ${this._src}: ${err.message}`);
        this._fire('error');
      }
    }

    play() {
      if (!this._buffer) {
        // Not loaded yet: play once it is, which is what a browser does.
        this.autoplay = true;
        return Promise.resolve();
      }
      const src = this._ctx.createBufferSource();
      const gain = this._ctx.createGain();
      src.buffer = this._buffer;
      src.loop = this.loop;
      gain.gain.value = this._volume;
      src.connect(gain);
      gain.connect(this._ctx.destination);
      src.start(0);
      this._source = src;
      this.paused = false;
      this.ended = false;
      return Promise.resolve();
    }

    pause() {
      if (this._source) { try { this._source.stop(0); } catch (_) {} this._source = null; }
      this.paused = true;
    }

    load() { this._load(); }
    canPlayType(type) {
      return /mpeg|mp3|ogg|wav|flac/i.test(String(type)) ? 'probably' : '';
    }
    addEventListener(type, fn) {
      if (!this._listeners.has(type)) this._listeners.set(type, []);
      this._listeners.get(type).push(fn);
    }
    removeEventListener(type, fn) {
      const l = this._listeners.get(type);
      if (!l) return;
      const i = l.indexOf(fn);
      if (i >= 0) l.splice(i, 1);
    }
    _fire(type) {
      const h = this['on' + type];
      if (typeof h === 'function') { try { h.call(this, { type }); } catch (_) {} }
      const l = this._listeners.get(type);
      if (l) for (const fn of l.slice()) { try { fn.call(this, { type }); } catch (_) {} }
    }
  }

  g.Audio = HTMLAudioElement;
  g.HTMLAudioElement = HTMLAudioElement;

  /*
   * OfflineAudioContext is NOT provided.
   *
   * The engine renders through a live device; an offline context would need a
   * separate render path. A stub that "works" but produces silence is worse than
   * an absent global a game can feature-detect.
   */
}
