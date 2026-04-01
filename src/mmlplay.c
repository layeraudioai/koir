#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <unistd.h>
#include <time.h>

#if defined(USE_ALSA)
#include <alsa/asoundlib.h>
static snd_pcm_t *pcm_handle = NULL;
#elif defined(USE_SDL3)
#include <SDL3/SDL.h>
static SDL_AudioStream *audio_stream = NULL;
#elif defined(USE_SDL)
#include <SDL/SDL.h>
static short *sdl1_buf = NULL;
static int sdl1_len = 0;
#elif defined(USE_SDL2)
#include <SDL2/SDL.h>
static SDL_AudioDeviceID audio_device;
#elif defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#include <conio.h>
static HWAVEOUT hWaveOut = NULL;
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE 96000
#define BUFFER_WINDOW (SAMPLE_RATE * 2 * 10) // Stereo 10 second window

#ifdef _WIN32
static CRITICAL_SECTION lock;
#define LOCK_INIT() InitializeCriticalSection(&lock)
#define LOCK() EnterCriticalSection(&lock)
#define UNLOCK() LeaveCriticalSection(&lock)
#else
#include <pthread.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK_INIT()
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
#endif

static float *mix_buffer = NULL;
static float *echo_send_buffer = NULL; 
static uint32_t play_cursor = 0;
static uint32_t write_cursor = 0;

// Master Effects State
static float master_lpf_state = 0.0f;
static float dc_block_in = 0.0f, dc_block_out = 0.0f;
static float *echo_buffer = NULL;
static uint32_t echo_ptr = 0;

float global_tempo_scale = 1.0f;
int audio_running = 1;
int layer_mute = 0;
int rng_enabled = 0;
static int global_riff_echo = 0;

// Track state for randomization
uint32_t track_pos[64] = {0};
int track_instr[64] = {0};
int track_pan[64] = {64};
float track_vol_mod[64] = {1.0f};
int track_oct_mod[64] = {0};
float track_skip_chance[64] = {0.0f};
static int target_track_count = 4;

void reset_rng_state() {
    for (int i = 0; i < 64; i++) {
        track_instr[i] = 0;
        track_vol_mod[i] = 1.0f;
        track_oct_mod[i] = 0;
        track_skip_chance[i] = 0.0f;
    }
}

typedef struct {
    uint32_t pos;
    int track;
    int midi;
    int octave;
    char note;
    char sharp;
    int is_rest;
} note_event_t;

static note_event_t *event_queue = NULL;
static int event_count = 0;
static int event_capacity = 100000;
static int play_event_idx = 0;

// Helper for SmileBasic style RND(min, max)
int rnd_range(int min, int max) {
    if (max <= min) return min;
    return min + rand() % (max - min + 1);
}

void display_event(note_event_t *e) {
    if (e->is_rest) {
        printf("T%02d R   ---- [", e->track);
        for (int i = 36; i < 96; i++) printf(i % 12 == 0 ? "|" : " ");
    } else {
        printf("T%02d %3d O%d %c%c [", e->track, e->midi, e->octave, e->note, (e->sharp > 0 ? '#' : (e->sharp < 0 ? 'b' : ' ')));
        for (int i = 36; i < 96; i++) {
            if (i == e->midi) printf("#");
            else if (i % 12 == 0) printf("|");
            else printf(".");
        }
    }
    printf("]\n");
}

#if defined(USE_SDL)
void sdl1_callback(void *userdata, Uint8 *stream, int len) {
    int to_copy = (sdl1_len < len) ? sdl1_len : len;
    if (to_copy > 0 && sdl1_buf) {
        memcpy(stream, sdl1_buf, to_copy);
        sdl1_buf += (to_copy / sizeof(short));
        sdl1_len -= to_copy;
    } else {
        memset(stream, 0, len);
    }
}
#endif

void* mixer_thread_func(void* arg) {
    (void)arg;
    while (audio_running) {
        LOCK();
        uint32_t available = write_cursor - play_cursor;
        UNLOCK();

        if (available == 0) {
            usleep(10000);
            continue;
        }

        // Use 50ms chunks for responsive visualization and low-latency audio dispatch
        uint32_t chunk = SAMPLE_RATE / 20; 
        if (chunk > available) chunk = available;

        short *out = malloc(chunk * 2 * sizeof(short));
        LOCK();
        
        // Sync visualizer: Display events that occur within the current chunk window
        while (play_event_idx < event_count && event_queue[play_event_idx].pos <= play_cursor + chunk) {
            display_event(&event_queue[play_event_idx]);
            play_event_idx++;
        }

        // Shift processed events out of the queue to keep memory usage stable without losing data
        if (play_event_idx > event_capacity / 2) {
            int remaining = event_count - play_event_idx;
            if (remaining > 0) memmove(event_queue, &event_queue[play_event_idx], remaining * sizeof(note_event_t));
            event_count = remaining;
            play_event_idx = 0;
        }

        for (uint32_t i = 0; i < chunk; i++) {
            float raw_sl = mix_buffer[((play_cursor + i) * 2 + 0) % BUFFER_WINDOW];
            float raw_sr = mix_buffer[((play_cursor + i) * 2 + 1) % BUFFER_WINDOW];
            
            // --- Master Effects Chain ---
            
            // 1. Feedback Delay (Echo) - approx 500ms
            uint32_t delay_len = SAMPLE_RATE / 2;
            float send_s = echo_send_buffer[(play_cursor + i) % BUFFER_WINDOW];
            float echo_sig = echo_buffer[echo_ptr];
            
            float raw_s = (raw_sl + raw_sr) * 0.5f;
            float s = raw_s + echo_sig * 0.35f; // 35% feedback
            echo_buffer[echo_ptr] = send_s + echo_sig * 0.35f;
            echo_ptr = (echo_ptr + 1) % delay_len;
            echo_send_buffer[(play_cursor + i) % BUFFER_WINDOW] = 0; // Clear after processing

            // 2. DC Blocker (High Pass at ~10Hz)
            dc_block_out = s - dc_block_in + 0.995f * dc_block_out;
            dc_block_in = s;
            s = dc_block_out;

            // 3. One-Pole Low Pass Filter (Smooths out "buzzing" harmonics)
            // Alpha of 0.4 at 384kHz provides a gentle roll-off
            master_lpf_state += 0.4f * (s - master_lpf_state);
            s = master_lpf_state;

            // Soft clipping to handle high track counts without harsh distortion
            float limited = (s > 1.0f) ? 1.0f : (s < -1.0f ? -1.0f : s);
            float shaped = 1.5f * limited - 0.5f * limited * limited * limited;
            
            // Final output is stereo: Preserve original spatial balance
            float gain_comp = (fabs(raw_s) < 1e-6f) ? 0 : shaped / raw_s;
            out[i * 2 + 0] = (short)(raw_sl * gain_comp * 32767.0f);
            out[i * 2 + 1] = (short)(raw_sr * gain_comp * 32767.0f);
            mix_buffer[((play_cursor + i) * 2 + 0) % BUFFER_WINDOW] = 0;
            mix_buffer[((play_cursor + i) * 2 + 1) % BUFFER_WINDOW] = 0;
        }
        play_cursor += chunk;
        UNLOCK();

#if defined(USE_ALSA)
        snd_pcm_writei(pcm_handle, out, chunk);
#elif defined(USE_SDL2)
        SDL_QueueAudio(audio_device, out, chunk * 2 * sizeof(short));
        usleep(10000);
#elif defined(_WIN32)
        WAVEHDR hdr = { (LPSTR)out, chunk * 2 * sizeof(short), 0, 0, 0, 0, NULL, 0 };
        waveOutPrepareHeader(hWaveOut, &hdr, sizeof(WAVEHDR));
        waveOutWrite(hWaveOut, &hdr, sizeof(WAVEHDR));
        while (!(hdr.dwFlags & WHDR_DONE)) Sleep(1);
        waveOutUnprepareHeader(hWaveOut, &hdr, sizeof(WAVEHDR));
#endif
        free(out);
    }
    return NULL;
}

void init_audio() {
    LOCK_INIT();
    mix_buffer = calloc(BUFFER_WINDOW, sizeof(float)); 
    echo_send_buffer = calloc(BUFFER_WINDOW, sizeof(float)); 
    echo_buffer = calloc(SAMPLE_RATE, sizeof(float)); // 1 second echo capacity
    event_queue = malloc(event_capacity * sizeof(note_event_t));
    reset_rng_state();
    printf("Attempting to open audio at %d Hz...\n", SAMPLE_RATE);
#if defined(USE_ALSA)
    if (snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) { perror("ALSA Open"); exit(1); }
    // Request 2 channels (Stereo) and lower ALSA latency to 100ms for better visual sync
    if (snd_pcm_set_params(pcm_handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, SAMPLE_RATE, 1, 100000) < 0) { perror("ALSA Params (Rate too high?)"); exit(1); }
#elif defined(USE_SDL3)
    if (!SDL_Init(SDL_INIT_AUDIO)) exit(1);
    SDL_AudioSpec spec = { SDL_AUDIO_S16LE, 2, SAMPLE_RATE };
    audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!audio_stream) exit(1);
    SDL_ResumeAudioStreamDevice(audio_stream);
#elif defined(USE_SDL)
    if (SDL_Init(SDL_INIT_AUDIO) < 0) exit(1);
    SDL_AudioSpec wanted;
    wanted.freq = SAMPLE_RATE;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 1024;
    wanted.callback = sdl1_callback;
    wanted.userdata = NULL;
    if (SDL_OpenAudio(&wanted, NULL) < 0) exit(1);
    SDL_PauseAudio(0);
#elif defined(USE_SDL2)
    if (SDL_Init(SDL_INIT_AUDIO) < 0) exit(1);
    SDL_AudioSpec wanted;
    memset(&wanted, 0, sizeof(wanted));
    wanted.freq = SAMPLE_RATE;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 4096;
    audio_device = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
    if (audio_device == 0) { printf("SDL2 Error: %s\n", SDL_GetError()); exit(1); }
    SDL_PauseAudioDevice(audio_device, 0);
#elif defined(_WIN32)
    WAVEFORMATEX wfx = {WAVE_FORMAT_PCM, 2, SAMPLE_RATE, SAMPLE_RATE * 4, 4, 16, 0};
    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) exit(1);
#else
    printf("Audio initialized in DUMMY mode (no sound).\n");
#endif

#ifndef _WIN32
    pthread_t tid;
    pthread_create(&tid, NULL, mixer_thread_func, NULL);
#else
    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)mixer_thread_func, NULL, 0, NULL);
#endif
}

void close_audio() {
    audio_running = 0;
    usleep(100000);
    free(echo_buffer);
    free(echo_send_buffer);
#if defined(USE_ALSA)
    if (pcm_handle) snd_pcm_drain(pcm_handle);
    if (pcm_handle) snd_pcm_close(pcm_handle);
#elif defined(USE_SDL3)
    SDL_DestroyAudioStream(audio_stream);
    SDL_Quit();
#elif defined(USE_SDL)
    SDL_CloseAudio();
    SDL_Quit();
#elif defined(USE_SDL2)
    SDL_CloseAudioDevice(audio_device);
    SDL_Quit();
#elif defined(_WIN32)
    if (hWaveOut) {
        waveOutReset(hWaveOut);
        waveOutClose(hWaveOut);
    }
#endif
}

void play_note(int track_id, char note, int sharp, int octave, double length, int tempo, int volume, int use_echo) {
    // Calculate duration based on MML rules: (240 / tempo) / length
    // This assumes 4/4 time where a whole note is 4 beats.
    double scaled_tempo = (double)tempo * global_tempo_scale;
    double duration = (240.0 / scaled_tempo) / length; 
    int num_samples = (int)(duration * SAMPLE_RATE);
    if (track_id >= 64) track_id = 0;

    LOCK();
    // Block if the track is getting too far ahead of the play cursor to prevent overwriting unplayed audio
    while (track_pos[track_id] > play_cursor + BUFFER_WINDOW - SAMPLE_RATE) {
        UNLOCK();
        usleep(10000);
        LOCK();
    }

    uint32_t start = track_pos[track_id];
    if (start + num_samples > write_cursor) write_cursor = start + num_samples;
    
    if (event_count < event_capacity) {
        note_event_t *e = &event_queue[event_count++];
        e->pos = start; e->track = track_id; e->note = note; e->sharp = sharp; e->octave = octave;
        e->is_rest = (toupper(note) == 'R');
        if (!e->is_rest) {
            int note_map[] = {9, 11, 0, 2, 4, 5, 7}; // A, B, C, D, E, F, G
            e->midi = (octave + 1) * 12 + note_map[toupper(note) - 'A'] + sharp + 7;
        } else {
            e->midi = 0;
        }
    }
    
    if (toupper(note) == 'R') {
        track_pos[track_id] += num_samples;
        UNLOCK();
        return;
    }

    // Map note to semitones from C
    int note_map[] = {9, 11, 0, 2, 4, 5, 7}; // A, B, C, D, E, F, G
    int semitone = note_map[toupper(note) - 'A'];
    semitone += sharp;
    
    // Frequency relative to A4 (440Hz)
    // Formula: f = 440 * 2^((n - 69) / 12) where n is MIDI key number
    // MIDI C4 is 60. A4 is 69.
    int midi_note = (octave + 1) * 12 + semitone;
    double freq = 440.0 * pow(2.0, (double)(midi_note - 69) / 12.0);
    float jitter = rng_enabled ? (float)rnd_range(90, 100) / 100.0f : 1.0f;

    // Perceived loudness scaling (Equal-loudness compensation for "6 lums" at 0dB)
    // Human hearing sensitivity peaks around 3-4kHz; we boost the extremes to compensate.
    float loudness_scale = (float)(0.35f + 0.55f * pow(1000.0 / freq, 0.35) + 0.15f * pow(freq / 5000.0, 0.3));
    
    // Adjusted amplitude to provide more headroom (divide by target_track_count + 6)
    float amplitude = ((6.0f*volume) / 127.0f) * track_vol_mod[track_id] * jitter * loudness_scale / (float)(target_track_count + 6);

    // Linear panning gains
    float gain_l = (float)(127 - track_pan[track_id]) / 127.0f;
    float gain_r = (float)track_pan[track_id] / 127.0f;

    // Shorten note and gate the end (85% active duration)
    int active_samples = (int)(num_samples * 0.85);
    
    // Increased attack time (25ms) and sharp release (10ms)
    int attack_samples = (int)(SAMPLE_RATE * 0.025);
    int release_samples = (int)(SAMPLE_RATE * 0.010);

    if (attack_samples + release_samples > active_samples) {
        attack_samples = (int)(active_samples * 0.6);
        release_samples = (int)(active_samples * 0.4);
    }

    double phase_inc = 2.0 * M_PI * freq / SAMPLE_RATE;
    for (int i = 0; i < num_samples; i++) {
        if (i >= active_samples) break; // Gating: remaining duration is silence

        double phase = phase_inc * i;
        double sample = 0;
        switch(track_instr[track_id] % 8) {
            case 1: sample = (sin(phase) > 0) ? 0.5 : -0.5; break; // Square
            case 2: sample = (fmod(phase, 2.0 * M_PI) < 0.5 * M_PI) ? 0.5 : -0.5; break; // Pulse (25%)
            case 3: sample = (2.0 / M_PI) * asin(sin(phase)); break; // Triangle
            case 4: sample = (fmod(phase, 2.0 * M_PI) / M_PI) - 1.0; break; // Saw
            case 5: sample = (((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f) * 0.5f; break; // Noise
            case 6: sample = (sin(phase) + 0.5 * sin(phase * 2.0) + 0.25 * sin(phase * 4.0)) / 1.75; break; // Organ
            case 7: sample = sin(phase + 1.5 * sin(phase * 2.01)); break; // FM Metal
            default: sample = sin(phase); break; // Sine (0)
        }

        // Smooth rounded envelope for cleaner transients
        double env = 1.0;
        if (i < attack_samples) 
            env = sin(((double)i / attack_samples) * (M_PI / 2.0));
        else if (i > active_samples - release_samples) 
            env = sin(((double)(active_samples - i) / release_samples) * (M_PI / 2.0));

        float s = (float)(amplitude * env * sample);
        mix_buffer[((start + i) * 2 + 0) % BUFFER_WINDOW] += s * gain_l;
        mix_buffer[((start + i) * 2 + 1) % BUFFER_WINDOW] += s * gain_r;
        if (use_echo) {
            echo_send_buffer[(start + i) % BUFFER_WINDOW] += s;
        }
    }

    track_pos[track_id] += num_samples;
    UNLOCK();
}

void parse_and_play_mml(int track_id, const char* mml, int volume) {
    int octave = 5;
    int tempo = 120; 
    int default_length = 4;
    int note_count = 0;
    int is_muted = 0;
    const char* p = mml;

    while (*p) {
        char cmd = toupper(*p);
        if (cmd == 'T') {
            p++; tempo = 0;
            while (isdigit(*p)) tempo = tempo * 10 + (*p++ - '0');
            if (tempo == 0) tempo = 120;
        } else if (cmd == '@') {
            p++; track_instr[track_id] = 0;
            while (isdigit(*p)) track_instr[track_id] = track_instr[track_id] * 10 + (*p++ - '0');
        } else if (cmd == 'P') {
            p++; track_pan[track_id] = 0;
            while (isdigit(*p)) track_pan[track_id] = track_pan[track_id] * 10 + (*p++ - '0');
        } else if (cmd == 'V') {
            p++; volume = 0;
            while (isdigit(*p)) volume = volume * 10 + (*p++ - '0');
            if (volume > 127) volume = 127;
        } else if (cmd == 'O') {
            p++; if (isdigit(*p)) octave = (*p++ - '0');
        } else if (cmd == '<') {
            octave--; p++;
        } else if (cmd == '>') {
            octave++; p++;
        } else if (cmd == 'L') {
            p++; default_length = 0;
            while (isdigit(*p)) default_length = default_length * 10 + (*p++ - '0');
        } else if ((cmd >= 'A' && cmd <= 'G') || cmd == 'R') {
            char note = cmd; p++;
            int sharp = 0;
            if (*p == '+') { sharp = 1; p++; }
            else if (*p == '#') { sharp = 1; p++; }
            else if (*p == '-') { sharp = -1; p++; }
            
            // Every 64 notes (~16 bars), potentially switch the "riff" parameters
            if (rng_enabled && (note_count % 64 == 0)) {
                if (track_id == 0) {
                    int r = rnd_range(1, 1000);
                    if (r <= 5) target_track_count = 1;                    // 0.5% chance of soloist
                    else if (r <= 30) target_track_count = rnd_range(2, 3);
                    else if (r <= 150) target_track_count = rnd_range(6, 12); // "more than usual range"
                    else target_track_count = 4;

                    // 1% chance of applying echo to this riff sequence
                    global_riff_echo = (rnd_range(1, 100) == 1);
                }

                if (rnd_range(0, 10) > 6) track_instr[track_id] = rand() % 8;
                track_skip_chance[track_id] = (rnd_range(0, 10) > 2) ? (float)rnd_range(40, 75) : (float)rnd_range(0, 15);
                if (rnd_range(0, 10) > 6) track_vol_mod[track_id] = (float)rnd_range(70, 100) / 100.0f;
                if (rnd_range(0, 10) > 8) track_oct_mod[track_id] = rnd_range(-1, 1);
                
                is_muted = (track_id >= target_track_count);
                
                // Optional tempo drift
                if (rnd_range(0, 10) > 8) tempo = rnd_range(100, 220);
            }
            note_count++;

            int length = 0;
            while (isdigit(*p)) length = length * 10 + (*p++ - '0');
            if (length == 0) length = default_length;
            double d_len = (double)length;
            while (*p == '.') { d_len = d_len * (2.0/3.0); p++; }

            int final_octave = octave + track_oct_mod[track_id];
            if (rng_enabled) {
                // Octaves under 3 have only a 1% chance; otherwise shift up
                if (final_octave < 3 && rnd_range(1, 100) > 1) final_octave += 3;
                else if (rnd_range(1, 1000) > 999) final_octave += 1;
                else if (rnd_range(1, 1000) > 999) final_octave -= 1;
                else if (rnd_range(1, 10000) > 9999) final_octave += 2;
                else if (rnd_range(1, 10000) > 9999) final_octave -= 2;
                else if (rnd_range(1, 100000) > 99999) final_octave += 3;
            } else {
                // Without RNG, raise every octave by 1 for playback
                if (rnd_range(1, 1000000) > 999999) final_octave += 1;
                else if (rnd_range(1, 1000000) > 999999) final_octave -= 1;
                else if (rnd_range(1, 1000000) > 999999) final_octave += 2;
                else if (rnd_range(1, 1000000) > 999999) final_octave -= 2;
                else if (rnd_range(1, 1000000) > 999999) final_octave += 3;
            }

            // Additional 1% chance of flipping the echo effect for this specific note
            int note_echo = global_riff_echo;
            if (rng_enabled && rnd_range(1, 100) == 1) {
                note_echo = !note_echo;
            }

            int skip = (rng_enabled && (rnd_range(0, 100) < track_skip_chance[track_id]));
            if (!is_muted && !skip) {
                play_note(track_id, note, sharp, final_octave, d_len, tempo, volume, note_echo);
            } else {
                play_note(track_id, 'R', 0, final_octave, d_len, tempo, 0, 0);
            }
        } else {
            p++;
        }
    }
}

void play_track(int track_id, const char* mml, int volume) {
    LOCK();
    printf("\n>> TRK %d (VOL %d)\n", track_id, volume);
    UNLOCK();
    parse_and_play_mml(track_id, mml, volume);
}

#ifdef STANDALONE_PLAYER
int main(int argc, char** argv) {
    FILE* input = stdin;
    if (argc > 1 && strcmp(argv[1], "-") != 0) {
        input = fopen(argv[1], "r");
        if (!input) { perror("Error opening file"); return 1; }
    }
    init_audio();
    printf("MML Standalone Player ready.\n");
    char line[4096];
    while (fgets(line, sizeof(line), input)) {
        char* mml_ptr = strstr(line, "MML: ");
        if (mml_ptr) {
            mml_ptr += 5;
            char* nl = strchr(mml_ptr, '\n'); 
            if (nl) *nl = '\0';
            parse_and_play_mml(mml_ptr, 127);
        } else if (strstr(line, "--- Sequence Step")) {
            printf("\n%s", line);
        }
    }
    close_audio();
    return 0;
}
#endif
