/* Host audio (XAudio2) behind the DirectSound hooks (hooks_dsound.c): each
   guest sound buffer is a source voice playing a host copy of its data
   (Xbox ADPCM decoded to 16-bit PCM). */
#pragma once
#include <stdint.h>

/* the guest's WAVEFORMATEX */
typedef struct { uint16_t tag, channels; uint32_t rate, avg_bytes; uint16_t block_align, bits; } AuFormat;
#define AU_FORMAT_PCM 1
#define AU_FORMAT_XBOX_ADPCM 0x69

int  au_init(void);                         /* 0 when there is no audio device */
int  au_buffer_create(const AuFormat *f, int is3d); /* an id > 0, or 0 for a format it cannot play */
void au_buffer_data(int id, const uint8_t *data, uint32_t bytes); /* the buffer's contents (Unlock) */
void au_buffer_free(int id);
void au_buffer_play(int id, int loop);
void au_buffer_stop(int id);
uint32_t au_buffer_status(int id);          /* DSBSTATUS: 1 playing, 4 looping */
uint32_t au_buffer_cursor(int id);          /* play position in bytes of the guest data */
void au_buffer_set_cursor(int id, uint32_t bytes);
void au_buffer_volume(int id, int32_t hundredths_db);
void au_buffer_frequency(int id, uint32_t hz); /* 0: the format's own rate */
void au_buffer_loop_region(int id, uint32_t start, uint32_t length); /* bytes; length 0: the whole buffer */
void au_buffer_position(int id, float x, float y, float z);
void au_buffer_distances(int id, float min, float max); /* negative: unchanged */
void au_listener(const float *position, const float *front, const float *top); /* NULL: unchanged */

/* Streams (music, speech): packets of data queued on a voice. done(ctx, 0)
   runs on the audio thread once a packet has played, done(ctx, 1) when it
   was flushed. The volume, frequency and position calls above work on a
   stream's id as well. */
typedef void (*AuPacketDone)(void *ctx, int flushed);
int  au_stream_create(const AuFormat *f, int is3d);
int  au_stream_submit(int id, const uint8_t *data, uint32_t bytes, AuPacketDone done, void *ctx); /* 0: not queued */
void au_stream_flush(int id);
void au_stream_pause(int id, int pause);
int  au_stream_busy(int id);                /* packets queued */
void au_stream_free(int id);
