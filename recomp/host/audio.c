/* XAudio2 implementation of audio.h.

   DirectSound buffer semantics on XAudio2 source voices: Play submits the
   host PCM from the current position (looping over the loop region), Stop
   remembers where it got to, and the play cursor is the submission's start
   plus the samples played since. 3D buffers get DirectSound 3D's distance
   model (full volume up to the minimum distance, then min/distance, no
   further fall-off beyond the maximum) and a stereo pan from the listener's
   orientation; Doppler, cones, filters and the I3DL2 reverb are not
   reproduced. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"

#pragma comment(lib, "xaudio2.lib")

static IXAudio2 *g_xa;
static IXAudio2MasteringVoice *g_master;
static CRITICAL_SECTION g_lock;

typedef struct Voice Voice;
typedef struct { IXAudio2VoiceCallback cb; Voice *v; } Callback; /* a stream's voice callback and its voice */
struct Voice {
    int used, is3d, stream, paused;
    Callback callback;
    volatile LONG gen, queued;  /* streams: flush generation, packets queued */
    AuFormat f;                 /* the guest's format */
    WAVEFORMATEX hf;            /* what the voice plays */
    IXAudio2SourceVoice *voice;
    uint8_t *pcm;               /* host copy of the data */
    uint32_t pcm_bytes, frames;
    int playing, looping;
    uint32_t start_frame;       /* where the current submission began */
    uint32_t pos_frame;         /* the position while stopped */
    uint32_t loop_start, loop_len; /* frames; length 0: the whole buffer */
    float volume, x, y, z, min_d, max_d;
};
#define VOICES 2048
#define MAX_RATIO 4.0f /* SetFrequency up to four times the format's rate */
static Voice g_v[VOICES];
static float g_lpos[3], g_lfront[3] = {0, 0, 1}, g_ltop[3] = {0, 1, 0};

/* boot.c calls this before the XBE is loaded: the engine's threads cannot be
   created afterwards (see kernel_prepare_threads) */
int au_init(void) {
    static int done, ok;
    if (done++) return ok;
    InitializeCriticalSection(&g_lock);
    if (getenv("RECOMP_NOAUDIO")) return 0;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr = XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr)) hr = IXAudio2_CreateMasteringVoice(g_xa, &g_master, 2, 48000, 0, NULL, NULL, AudioCategory_GameEffects);
    if (FAILED(hr)) {
        fprintf(stderr, "audio: no XAudio2 output (%08lx); silent\n", hr);
        if (g_xa) IXAudio2_Release(g_xa);
        g_xa = NULL;
        return 0;
    }
    ok = 1;
    return 1;
}

static Voice *get(int id) { return id > 0 && id <= VOICES && g_v[id - 1].used ? &g_v[id - 1] : NULL; }

/* ---- Xbox ADPCM ------------------------------------------------------------------ */
/* IMA ADPCM in blocks of 36 bytes per channel: a header (first sample, step
   index) and 64 four-bit codes, so 65 samples. Stereo interleaves the two
   headers, then four bytes (eight codes) of each channel in turn. */
static const int16_t k_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
    1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
    8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t k_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static int16_t adpcm_step(int *sample, int *index, int code) {
    int step = k_steps[*index], diff = step >> 3;
    if (code & 1) diff += step >> 2;
    if (code & 2) diff += step >> 1;
    if (code & 4) diff += step;
    *sample += (code & 8) ? -diff : diff;
    if (*sample > 32767) *sample = 32767;
    if (*sample < -32768) *sample = -32768;
    *index += k_index[code];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return (int16_t)*sample;
}

static void adpcm_decode(const uint8_t *src, uint32_t blocks, int ch, int16_t *out) {
    for (uint32_t b = 0; b < blocks; b++, src += 36 * ch, out += 65 * ch) {
        for (int c = 0; c < ch; c++) {
            int sample = (int16_t)(src[4 * c] | (src[4 * c + 1] << 8)), index = src[4 * c + 2];
            if (index > 88) index = 88;
            out[c] = (int16_t)sample;
            /* chunks of four bytes per channel after the headers */
            for (int k = 0; k < 8; k++) {
                const uint8_t *p = src + 4 * ch + (k * ch + c) * 4;
                for (int i = 0; i < 8; i++)
                    out[(1 + k * 8 + i) * ch + c] = adpcm_step(&sample, &index, (p[i / 2] >> (4 * (i & 1))) & 15);
            }
        }
    }
}

/* ---- buffers ------------------------------------------------------------------------ */
static uint32_t frames_of(const AuFormat *f, uint32_t bytes) {
    if (f->tag == AU_FORMAT_XBOX_ADPCM) return bytes / (36u * f->channels) * 65;
    return f->block_align ? bytes / f->block_align : 0;
}
static uint32_t bytes_of(const AuFormat *f, uint32_t frames) {
    if (f->tag == AU_FORMAT_XBOX_ADPCM) return frames / 65 * 36u * f->channels;
    return frames * f->block_align;
}

static const IXAudio2VoiceCallbackVtbl k_stream_callbacks;

static int create_voice(const AuFormat *f, int is3d, int stream) {
    if (!g_xa || !f->channels || f->channels > 2 || !f->rate) return 0;
    if (f->tag != AU_FORMAT_XBOX_ADPCM && !(f->tag == AU_FORMAT_PCM && (f->bits == 8 || f->bits == 16))) {
        static int warned;
        if (!warned++) fprintf(stderr, "audio: sound format %04x (%u bits) not played\n", f->tag, f->bits);
        return 0;
    }
    EnterCriticalSection(&g_lock);
    int id = 0;
    for (int i = 0; i < VOICES && !id; i++) if (!g_v[i].used) id = i + 1;
    if (!id) { LeaveCriticalSection(&g_lock); fprintf(stderr, "audio: out of voices\n"); return 0; }
    Voice *v = &g_v[id - 1];
    memset(v, 0, sizeof *v);
    v->f = *f;
    v->is3d = is3d;
    v->volume = 1; v->min_d = 1; v->max_d = 1e9f;
    v->stream = stream;
    v->callback.cb.lpVtbl = (IXAudio2VoiceCallbackVtbl *)&k_stream_callbacks;
    v->callback.v = v;
    int bits = f->tag == AU_FORMAT_XBOX_ADPCM ? 16 : f->bits;
    v->hf = (WAVEFORMATEX){WAVE_FORMAT_PCM, f->channels, f->rate, f->rate * f->channels * bits / 8, (WORD)(f->channels * bits / 8), (WORD)bits, 0};
    HRESULT hr = IXAudio2_CreateSourceVoice(g_xa, &v->voice, &v->hf, 0, MAX_RATIO, stream ? &v->callback.cb : NULL, NULL, NULL);
    if (FAILED(hr)) { v->voice = NULL; LeaveCriticalSection(&g_lock); fprintf(stderr, "audio: no source voice (%08lx)\n", hr); return 0; }
    v->used = 1;
    LeaveCriticalSection(&g_lock);
    return id;
}

int au_buffer_create(const AuFormat *f, int is3d) { return create_voice(f, is3d, 0); }

void au_buffer_data(int id, const uint8_t *data, uint32_t bytes) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v) {
        if (v->playing) { IXAudio2SourceVoice_Stop(v->voice, 0, XAUDIO2_COMMIT_NOW); IXAudio2SourceVoice_FlushSourceBuffers(v->voice); v->playing = 0; }
        free(v->pcm);
        v->frames = frames_of(&v->f, bytes);
        v->pcm_bytes = v->frames * v->hf.nBlockAlign;
        v->pcm = malloc(v->pcm_bytes ? v->pcm_bytes : 1);
        if (v->f.tag == AU_FORMAT_XBOX_ADPCM) adpcm_decode(data, v->frames / 65, v->f.channels, (int16_t *)v->pcm);
        else memcpy(v->pcm, data, v->pcm_bytes);
    }
    LeaveCriticalSection(&g_lock);
}

void au_buffer_free(int id) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v) {
        IXAudio2SourceVoice_DestroyVoice(v->voice); /* waits for the audio thread to let go of the data */
        free(v->pcm);
        memset(v, 0, sizeof *v);
    }
    LeaveCriticalSection(&g_lock);
}

/* where the voice has got to; a one-shot that ran out has stopped (and
   starts over next time) */
static uint32_t cursor_frame(Voice *v) {
    if (!v->playing) return v->pos_frame;
    XAUDIO2_VOICE_STATE st;
    IXAudio2SourceVoice_GetState(v->voice, &st, 0);
    if (!v->looping && st.BuffersQueued == 0) { v->playing = 0; v->pos_frame = 0; return 0; }
    uint64_t f = v->start_frame + st.SamplesPlayed;
    if (v->looping) {
        uint32_t ls = v->loop_len ? v->loop_start : 0, ll = v->loop_len ? v->loop_len : v->frames;
        if (ll && f >= ls + ll) f = ls + (f - ls - ll) % ll;
    }
    return (uint32_t)(f < v->frames ? f : 0);
}

/* the pan and distance for a 3D buffer, the volume for any */
static void spatialize(Voice *v) {
    float gain = v->volume, l = 1, r = 1;
    if (v->is3d) {
        float d[3] = {v->x - g_lpos[0], v->y - g_lpos[1], v->z - g_lpos[2]};
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist > v->min_d) gain *= v->min_d / (dist < v->max_d ? dist : v->max_d);
        if (dist > 1e-4f && v->f.channels == 1) {
            /* left-handed: right = top x front */
            const float *t = g_ltop, *fr = g_lfront;
            float right[3] = {t[1] * fr[2] - t[2] * fr[1], t[2] * fr[0] - t[0] * fr[2], t[0] * fr[1] - t[1] * fr[0]};
            float rl = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
            float pan = rl > 0 ? (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / (dist * rl) : 0;
            l = pan > 0 ? 1 - pan : 1;
            r = pan < 0 ? 1 + pan : 1;
        }
    }
    IXAudio2SourceVoice_SetVolume(v->voice, gain, XAUDIO2_COMMIT_NOW);
    if (v->f.channels == 1) {
        float m[2] = {l, r};
        IXAudio2SourceVoice_SetOutputMatrix(v->voice, NULL, 1, 2, m, XAUDIO2_COMMIT_NOW);
    }
}

void au_buffer_play(int id, int loop) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v && v->pcm && v->frames) {
        cursor_frame(v);
        if (v->playing && v->looping == loop) { LeaveCriticalSection(&g_lock); return; }
        uint32_t from = cursor_frame(v);
        IXAudio2SourceVoice_Stop(v->voice, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(v->voice);
        XAUDIO2_BUFFER b = {0};
        b.Flags = XAUDIO2_END_OF_STREAM;
        b.AudioBytes = v->pcm_bytes;
        b.pAudioData = v->pcm;
        b.PlayBegin = from < v->frames ? from : 0;
        if (loop) {
            b.LoopBegin = v->loop_len ? v->loop_start : 0;
            b.LoopLength = v->loop_len;
            if (b.LoopBegin >= v->frames) b.LoopBegin = 0;
            if (b.LoopBegin + b.LoopLength > v->frames) b.LoopLength = 0;
            b.LoopCount = XAUDIO2_LOOP_INFINITE;
        }
        spatialize(v);
        if (getenv("RECOMP_DSLOG")) {
            float vol;
            IXAudio2SourceVoice_GetVolume(v->voice, &vol);
            fprintf(stderr, "audio: play voice %d from %u of %u frames%s, gain %.3f%s\n", id, b.PlayBegin, v->frames, loop ? " looping" : "", vol,
                    v->is3d ? " (3D)" : "");
        }
        if (SUCCEEDED(IXAudio2SourceVoice_SubmitSourceBuffer(v->voice, &b, NULL))) {
            IXAudio2SourceVoice_Start(v->voice, 0, XAUDIO2_COMMIT_NOW);
            v->playing = 1;
            v->looping = loop;
            v->start_frame = b.PlayBegin;
        }
    }
    LeaveCriticalSection(&g_lock);
}

void au_buffer_stop(int id) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v && v->playing) {
        v->pos_frame = cursor_frame(v);
        IXAudio2SourceVoice_Stop(v->voice, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(v->voice);
        v->playing = 0;
    }
    LeaveCriticalSection(&g_lock);
}

uint32_t au_buffer_status(int id) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    uint32_t s = 0;
    if (v) {
        cursor_frame(v);
        s = v->playing ? 1 | (v->looping ? 4 : 0) : 0;
    }
    LeaveCriticalSection(&g_lock);
    return s;
}

uint32_t au_buffer_cursor(int id) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    uint32_t b = v ? bytes_of(&v->f, cursor_frame(v)) : 0;
    LeaveCriticalSection(&g_lock);
    return b;
}

void au_buffer_set_cursor(int id, uint32_t bytes) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v) {
        uint32_t f = frames_of(&v->f, bytes);
        v->pos_frame = f < v->frames ? f : 0;
        if (v->playing) { /* restart from there */
            int loop = v->looping;
            uint32_t pos = v->pos_frame;
            IXAudio2SourceVoice_Stop(v->voice, 0, XAUDIO2_COMMIT_NOW);
            IXAudio2SourceVoice_FlushSourceBuffers(v->voice);
            v->playing = 0;
            v->pos_frame = pos;
            LeaveCriticalSection(&g_lock);
            au_buffer_play(id, loop);
            return;
        }
    }
    LeaveCriticalSection(&g_lock);
}

void au_buffer_volume(int id, int32_t hundredths_db) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v) {
        v->volume = hundredths_db <= -10000 ? 0 : powf(10, hundredths_db / 2000.0f);
        spatialize(v);
    }
    LeaveCriticalSection(&g_lock);
}

void au_buffer_frequency(int id, uint32_t hz) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v) {
        float r = hz ? (float)hz / v->f.rate : 1;
        if (r > MAX_RATIO) r = MAX_RATIO;
        if (r < XAUDIO2_MIN_FREQ_RATIO) r = XAUDIO2_MIN_FREQ_RATIO;
        IXAudio2SourceVoice_SetFrequencyRatio(v->voice, r, XAUDIO2_COMMIT_NOW);
    }
    LeaveCriticalSection(&g_lock);
}

void au_buffer_loop_region(int id, uint32_t start, uint32_t length) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v) { v->loop_start = frames_of(&v->f, start); v->loop_len = frames_of(&v->f, length); }
    LeaveCriticalSection(&g_lock);
}

void au_buffer_position(int id, float x, float y, float z) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v) { v->x = x; v->y = y; v->z = z; spatialize(v); }
    LeaveCriticalSection(&g_lock);
}

void au_buffer_distances(int id, float min, float max) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v) {
        if (min >= 0) v->min_d = min;
        if (max >= 0) v->max_d = max;
        spatialize(v);
    }
    LeaveCriticalSection(&g_lock);
}

/* ---- streams ------------------------------------------------------------------------ */
/* Each packet is a host copy (ADPCM decoded) submitted as one XAudio2
   buffer; its context comes back in OnBufferEnd once it has played, or was
   flushed (the voice's generation moved on). The callbacks take no lock:
   DestroyVoice holds it while it waits for them. */
typedef struct { AuPacketDone done; void *ctx; uint8_t *pcm; LONG gen; } Packet;

static void STDMETHODCALLTYPE cb_pass_start(IXAudio2VoiceCallback *t, UINT32 bytes) { (void)t; (void)bytes; }
static void STDMETHODCALLTYPE cb_void(IXAudio2VoiceCallback *t) { (void)t; }
static void STDMETHODCALLTYPE cb_buffer(IXAudio2VoiceCallback *t, void *p) { (void)t; (void)p; }
static void STDMETHODCALLTYPE cb_error(IXAudio2VoiceCallback *t, void *p, HRESULT e) { (void)t; (void)p; fprintf(stderr, "audio: voice error %08lx\n", e); }
static void STDMETHODCALLTYPE cb_buffer_end(IXAudio2VoiceCallback *t, void *p) {
    Voice *v = ((Callback *)t)->v;
    Packet *k = p;
    k->done(k->ctx, k->gen != v->gen);
    free(k->pcm);
    free(k);
    InterlockedDecrement(&v->queued);
}
static const IXAudio2VoiceCallbackVtbl k_stream_callbacks = {cb_pass_start, cb_void, cb_void, cb_buffer, cb_buffer_end, cb_buffer, cb_error};

int au_stream_create(const AuFormat *f, int is3d) { return create_voice(f, is3d, 1); }

int au_stream_submit(int id, const uint8_t *data, uint32_t bytes, AuPacketDone done, void *ctx) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    int ok = 0;
    if (v && v->stream) {
        uint32_t frames = frames_of(&v->f, bytes);
        Packet *k = malloc(sizeof *k);
        k->done = done; k->ctx = ctx; k->gen = v->gen;
        k->pcm = malloc(frames * v->hf.nBlockAlign + 1);
        if (v->f.tag == AU_FORMAT_XBOX_ADPCM) adpcm_decode(data, frames / 65, v->f.channels, (int16_t *)k->pcm);
        else memcpy(k->pcm, data, frames * v->hf.nBlockAlign);
        XAUDIO2_BUFFER b = {0};
        b.AudioBytes = frames * v->hf.nBlockAlign;
        b.pAudioData = k->pcm;
        b.pContext = k;
        InterlockedIncrement(&v->queued);
        if (frames && SUCCEEDED(IXAudio2SourceVoice_SubmitSourceBuffer(v->voice, &b, NULL))) {
            ok = 1;
            if (!v->playing && !v->paused) { spatialize(v); IXAudio2SourceVoice_Start(v->voice, 0, XAUDIO2_COMMIT_NOW); v->playing = 1; }
        } else {
            InterlockedDecrement(&v->queued);
            free(k->pcm);
            free(k);
        }
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

/* drop what is queued: each packet comes back as flushed */
static void stream_flush(Voice *v) {
    InterlockedIncrement(&v->gen);
    IXAudio2SourceVoice_FlushSourceBuffers(v->voice);
}

void au_stream_flush(int id) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v && v->stream) stream_flush(v);
    LeaveCriticalSection(&g_lock);
}

void au_stream_pause(int id, int pause) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v && v->stream && v->paused != pause) {
        v->paused = pause;
        if (pause && v->playing) { IXAudio2SourceVoice_Stop(v->voice, 0, XAUDIO2_COMMIT_NOW); v->playing = 0; }
        else if (!pause) { spatialize(v); IXAudio2SourceVoice_Start(v->voice, 0, XAUDIO2_COMMIT_NOW); v->playing = 1; }
    }
    LeaveCriticalSection(&g_lock);
}

int au_stream_busy(int id) {
    Voice *v = id > 0 && id <= VOICES ? &g_v[id - 1] : NULL;
    return v && v->used && v->queued > 0;
}

/* flushed packets come back on the next processing passes; wait for them
   (briefly) so none is left pending in the game's eyes */
void au_stream_free(int id) {
    EnterCriticalSection(&g_lock);
    Voice *v = get(id);
    if (v && v->stream) {
        IXAudio2SourceVoice_Stop(v->voice, 0, XAUDIO2_COMMIT_NOW);
        stream_flush(v);
        if (!v->playing) IXAudio2SourceVoice_Start(v->voice, 0, XAUDIO2_COMMIT_NOW); /* buffers are released by processing passes */
        for (int i = 0; i < 100 && v->queued > 0; i++) Sleep(2);
    }
    LeaveCriticalSection(&g_lock);
    au_buffer_free(id);
}

void au_listener(const float *position, const float *front, const float *top) {
    EnterCriticalSection(&g_lock);
    if (position) memcpy(g_lpos, position, sizeof g_lpos);
    if (front) memcpy(g_lfront, front, sizeof g_lfront);
    if (top) memcpy(g_ltop, top, sizeof g_ltop);
    for (int i = 0; i < VOICES; i++) if (g_v[i].used && g_v[i].is3d && g_v[i].playing) spatialize(&g_v[i]);
    LeaveCriticalSection(&g_lock);
}
