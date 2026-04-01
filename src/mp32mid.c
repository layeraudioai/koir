#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mpg123.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FFT_SIZE 4096
#define HOP_SIZE 1024

#define MAX_TRACKS 4

typedef struct {
    int notes[MAX_TRACKS];
    float mags[MAX_TRACKS];
    uint8_t pans[MAX_TRACKS];
    float total_energy;
} FrameData;

typedef struct {
    double r, i;
} complex_t;

typedef struct {
    size_t frame_idx;
    int track_idx;
    float mag;
} NoteRef;

static int compare_notes(const void* a, const void* b) {
    const NoteRef* r1 = (const NoteRef*)a;
    const NoteRef* r2 = (const NoteRef*)b;
    if (r1->mag < r2->mag) return -1;
    if (r1->mag > r2->mag) return 1;
    return 0;
}

// Musical Theory Tables
typedef struct {
    int root; // 0-11
    int is_minor; // 0=Major, 1=Minor
} KeyInfo;

// Krumhansl-Schmuckler Profiles
static const float K_S_MAJOR[] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
static const float K_S_MINOR[] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
static const int SCALE_MAP[2][7] = {
    {0, 2, 4, 5, 7, 9, 11}, // Major
    {0, 2, 3, 5, 7, 8, 10}  // Minor
};

KeyInfo identify_key(float* profile) {
    KeyInfo best_key = {0, 0};
    float max_corr = -1e10;

    for (int is_minor = 0; is_minor < 2; is_minor++) {
        const float* ref = is_minor ? K_S_MINOR : K_S_MAJOR;
        for (int root = 0; root < 12; root++) {
            float corr = 0;
            for (int i = 0; i < 12; i++) {
                corr += profile[(root + i) % 12] * ref[i];
            }
            if (corr > max_corr) {
                max_corr = corr;
                best_key.root = root;
                best_key.is_minor = is_minor;
            }
        }
    }
    return best_key;
}

const char* KEY_NAMES[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

void fft(complex_t* v, int n, complex_t* tmp) {
    if (n <= 1) return;
    for (int i = 0; i < n / 2; i++) {
        tmp[i] = v[2 * i];
        tmp[i + n / 2] = v[2 * i + 1];
    }
    fft(tmp, n / 2, v);
    fft(tmp + n / 2, n / 2, v);
    for (int i = 0; i < n / 2; i++) {
        double angle = -2.0 * M_PI * i / n;
        complex_t w = {cos(angle), sin(angle)};
        complex_t t = {w.r * tmp[i + n / 2].r - w.i * tmp[i + n / 2].i,
                       w.r * tmp[i + n / 2].i + w.i * tmp[i + n / 2].r};
        v[i].r = tmp[i].r + t.r;
        v[i].i = tmp[i].i + t.i;
        v[i + n / 2].r = tmp[i].r - t.r;
        v[i + n / 2].i = tmp[i].i - t.i;
    }
}

void write_varlen(FILE* f, uint32_t val) {
    uint32_t buffer = val & 0x7F;
    while ((val >>= 7) > 0) {
        buffer <<= 8;
        buffer |= 0x80 | (val & 0x7F);
    }
    while (1) {
        fputc(buffer & 0xFF, f);
        if (buffer & 0x80) buffer >>= 8;
        else break;
    }
}

void write_u32(FILE* f, uint32_t val) {
    fputc((val >> 24) & 0xFF, f);
    fputc((val >> 16) & 0xFF, f);
    fputc((val >> 8) & 0xFF, f);
    fputc(val & 0xFF, f);
}

void write_u16(FILE* f, uint16_t val) {
    fputc((val >> 8) & 0xFF, f);
    fputc(val & 0xFF, f);
}

float get_bin_mag(float* samples, int n, int k, int channels, int channel_idx) {
    double r = 0, i = 0;
    for (int j = 0; j < n; j++) {
        double window = 0.5 * (1.0 - cos(2.0 * M_PI * j / (n - 1)));
        double angle = -2.0 * M_PI * k * j / n;
        r += samples[j * channels + channel_idx] * window * cos(angle);
        i += samples[j * channels + channel_idx] * window * sin(angle);
    }
    return (float)sqrt(r * r + i * i);
}

int detect_bpm(FrameData* frames, int count, int rate) {
    int min_lag = (rate * 60) / (220 * HOP_SIZE); // 220 BPM
    int max_lag = (rate * 60) / (60 * HOP_SIZE);  // 60 BPM
    float max_corr = -1;
    int best_lag = (rate * 60) / (120 * HOP_SIZE);
    for (int lag = min_lag; lag < max_lag; lag++) {
        float corr = 0;
        for (int i = 0; i < count - lag; i++) corr += frames[i].total_energy * frames[i+lag].total_energy;
        // Normalize by number of samples and apply a slight bias towards human-centered tempos (110 BPM)
        float normalized_corr = corr / (float)(count - lag);
        double bpm_at_lag = (60.0 * rate) / (lag * HOP_SIZE);
        float bias = 1.0f + 0.3f * expf(-powf(bpm_at_lag - 110.0f, 2.0f) / 400.0f);
        if (normalized_corr * bias > max_corr) { max_corr = normalized_corr * bias; best_lag = lag; }
    }
    return (int)((60.0 * rate) / (best_lag * HOP_SIZE));
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <input.mp3> [output.mid]\n", argv[0]);
        return 1;
    }

    mpg123_init();
    int err;
    mpg123_handle* mh = mpg123_new(NULL, &err);
    if (mpg123_open(mh, argv[1]) != MPG123_OK) {
        fprintf(stderr, "Error opening MP3: %s\n", mpg123_strerror(mh));
        return 1;
    }

    long rate;
    int channels, encoding;
    mpg123_getformat(mh, &rate, &channels, &encoding);
    mpg123_format_none(mh);
    mpg123_format(mh, rate, channels, MPG123_ENC_SIGNED_16);

    size_t buffer_size = mpg123_outblock(mh);
    unsigned char* buffer = malloc(buffer_size);

    char out_name[256];
    if (argc > 2) {
        strncpy(out_name, argv[2], 255);
    } else { snprintf(out_name, 255, "output.mid"); }

    complex_t* fft_in = calloc(FFT_SIZE, sizeof(complex_t));
    complex_t* fft_tmp = calloc(FFT_SIZE, sizeof(complex_t));
    float* pcm_window = calloc(FFT_SIZE * channels, sizeof(float));
    
    size_t frame_cap = 10000;
    FrameData* frames = malloc(frame_cap * sizeof(FrameData));
    size_t frame_count = 0;

    size_t done;
    int sample_count = 0;

    while (mpg123_read(mh, buffer, buffer_size, &done) == MPG123_OK) {
        int num_samples = done / (sizeof(int16_t) * channels);
        int16_t* pcm = (int16_t*)buffer;
        for (int i = 0; i < num_samples; i++) {
            for (int c = 0; c < channels; c++) pcm_window[sample_count * channels + c] = pcm[i * channels + c];
            sample_count++;

            if (sample_count == FFT_SIZE) {
                float mono_energy = 0;
                for (int j = 0; j < FFT_SIZE; j++) {
                    float mono = 0;
                    for(int c=0; c<channels; c++) mono += pcm_window[j*channels + c];
                    mono /= channels;
                    double window = 0.5 * (1.0 - cos(2.0 * M_PI * j / (FFT_SIZE - 1)));
                    fft_in[j].r = mono * window;
                    fft_in[j].i = 0;
                    mono_energy += mono * mono;
                }
                fft(fft_in, FFT_SIZE, fft_tmp);

                double mags[FFT_SIZE / 2];
                for (int j = 1; j < FFT_SIZE / 2; j++) {
                    mags[j] = fft_in[j].r * fft_in[j].r + fft_in[j].i * fft_in[j].i;
                }

                if (frame_count >= frame_cap) {
                    frame_cap *= 2;
                    frames = realloc(frames, frame_cap * sizeof(FrameData));
                }

                frames[frame_count].total_energy = mono_energy;
                for (int t = 0; t < MAX_TRACKS; t++) {
                    double max_v = 0;
                    int peak_idx = -1;
                    for (int j = 1; j < FFT_SIZE / 2; j++) {
                        if (mags[j] > max_v) { max_v = mags[j]; peak_idx = j; }
                    }

                    // Permissive threshold to capture a wider pool of notes for user selection
                    if (max_v > 500000.0 && max_v > mono_energy * 0.05) {
                        double freq = (double)peak_idx * rate / FFT_SIZE;
                        frames[frame_count].notes[t] = (int)(round(12.0 * log2(freq / 440.0) + 69.0));
                        frames[frame_count].mags[t] = (float)max_v;
                        
                        float magL = get_bin_mag(pcm_window, FFT_SIZE, peak_idx, channels, 0);
                        float magR = (channels > 1) ? get_bin_mag(pcm_window, FFT_SIZE, peak_idx, channels, 1) : magL;
                        frames[frame_count].pans[t] = (uint8_t)(64 + 63 * (magR - magL) / (magR + magL + 1e-6));

                        // Wider masking to prevent "harmonic chatter"
                        int mask_start = peak_idx - 24; if (mask_start < 0) mask_start = 0;
                        int mask_end = peak_idx + 24; if (mask_end > FFT_SIZE/2) mask_end = FFT_SIZE/2;
                        for (int m = mask_start; m < mask_end; m++) mags[m] = 0;
                    } else {
                        frames[frame_count].notes[t] = -1;
                    }
                }
                frame_count++;

                memmove(pcm_window, pcm_window + HOP_SIZE * channels, (FFT_SIZE - HOP_SIZE) * channels * sizeof(float));
                sample_count = FFT_SIZE - HOP_SIZE;
            }
        }
    }

    int bpm = detect_bpm(frames, frame_count, rate);
    printf("Detected BPM: %d\n", bpm);

    // --- Pass 2: Per-Riff Harmonic and Tempo Analysis ---
    int frames_per_riff = (int)((16.0 * 60.0 * rate) / (bpm * HOP_SIZE)); // 4 bars approx
    if (frames_per_riff < 50) frames_per_riff = 50;

    printf("\n--- Segment Analysis ---\n");
    for (size_t i = 0; i < frame_count; i += frames_per_riff) {
        float profile[12] = {0};
        int end = (i + frames_per_riff > frame_count) ? (int)frame_count : (int)(i + frames_per_riff);
        
        for (int f = i; f < end; f++) {
            for (int t = 0; t < MAX_TRACKS; t++) {
                if (frames[f].notes[t] != -1) profile[frames[f].notes[t] % 12] += frames[f].mags[t];
            }
        }

        int local_bpm = detect_bpm(frames + i, end - i, rate);
        KeyInfo local_key = identify_key(profile);
        printf("Time %3ds | Local BPM: %3d | Detected Key: %s %s\n", 
            (int)((i * HOP_SIZE) / rate), local_bpm, KEY_NAMES[local_key.root], local_key.is_minor ? "Minor" : "Major");
    }

    // --- User Global Constraints ---
    printf("\n--- Global Song Configuration ---\n");
    int min_oct = 11, max_oct = 0;
    for (size_t f = 0; f < frame_count; f++) {
        for (int t = 0; t < MAX_TRACKS; t++) {
            if (frames[f].notes[t] != -1) {
                int o = frames[f].notes[t] / 12;
                if (o < min_oct) min_oct = o;
                if (o > max_oct) max_oct = o;
            }
        }
    }

    int target_root = 0, target_is_minor = 0;
    printf("Enter Target Key Root (0:C, 1:C#, ..., 11:B): ");
    if (scanf("%d", &target_root) != 1) target_root = 0;
    printf("Enter Target Scale Type (0:Major, 1:Minor): ");
    if (scanf("%d", &target_is_minor) != 1) target_is_minor = 0;

    printf("Suggested BPM: %d. Enter Target Tempo (0 to keep): ", bpm);
    int input_bpm;
    if (scanf("%d", &input_bpm) == 1 && input_bpm > 0) bpm = input_bpm;

    printf("Detected Octave Range: O%d to O%d\n", min_oct, max_oct);
    int t_min, t_max;
    printf("Enter Target Min Octave (0-10): "); if (scanf("%d", &t_min) != 1) t_min = min_oct;
    printf("Enter Target Max Octave (0-10): "); if (scanf("%d", &t_max) != 1) t_max = max_oct;

    printf("Apply scale locking and octave clamping (normalization)? (1:Yes, 0:No): ");
    int do_norm = 1;
    if (scanf("%d", &do_norm) != 1) do_norm = 1;

    if (do_norm) {
        // Apply Scale Locking and Octave Clamping Globally based on user choice
        for (size_t f = 0; f < frame_count; f++) {
            for (int t = 0; t < MAX_TRACKS; t++) {
                if (frames[f].notes[t] == -1) continue;
                int note = frames[f].notes[t];
                int pc = note % 12;
                int octave = note / 12;
                int in_scale = 0, best_fit_pc = -1, min_dist = 100;
                for (int s = 0; s < 7; s++) {
                    int scale_pc = (target_root + SCALE_MAP[target_is_minor][s]) % 12;
                    if (pc == scale_pc) { in_scale = 1; break; }
                    int dist = abs(pc - scale_pc);
                    if (dist > 6) dist = 12 - dist;
                    if (dist < min_dist) { min_dist = dist; best_fit_pc = scale_pc; }
                }
                if (!in_scale) {
                    int new_note = (octave * 12) + best_fit_pc;
                    if (abs(new_note - note) > 6) new_note += (new_note < note) ? 12 : -12;
                    frames[f].notes[t] = new_note;
                }

                // Final Octave Range Adjustment
                int final_n = frames[f].notes[t];
                int pc_f = final_n % 12;
                int oct_f = final_n / 12;
                if (oct_f < t_min) oct_f = t_min;
                if (oct_f > t_max) oct_f = t_max;
                frames[f].notes[t] = (oct_f * 12) + pc_f;
            }
        }
    }

    float max_mag_overall = 0;
    for (size_t f = 0; f < frame_count; f++) {
        for (int t = 0; t < MAX_TRACKS; t++) {
            if (frames[f].notes[t] != -1 && frames[f].mags[t] > max_mag_overall)
                max_mag_overall = frames[f].mags[t];
        }
    }

    float vol_scale = 1.0f;
    if (max_mag_overall > 0) {
        printf("Peak Spectral Magnitude detected: %.2f\n", max_mag_overall);
        printf("Enter Volume/Velocity Scale (0.1 - 2.0, default 1.0): ");
        if (scanf("%f", &vol_scale) != 1) vol_scale = 1.0f;
    }

    int current_note_count = 0;
    for (size_t f = 0; f < frame_count; f++) {
        for (int t = 0; t < MAX_TRACKS; t++) {
            if (frames[f].notes[t] != -1) current_note_count++;
        }
    }

    printf("\nTotal notes extracted: %d. Enter Quantification Target (0 to keep all): ", current_note_count);
    int target_count = 0;
    if (scanf("%d", &target_count) != 1) target_count = 0;

    if (target_count > 0 && target_count < current_note_count) {
        printf("Throttling note density to %d... (Removing low-energy noise)\n", target_count);
        
        NoteRef* all_active = malloc(current_note_count * sizeof(NoteRef));
        int active_ptr = 0;
        for (size_t f = 0; f < frame_count; f++) {
            for (int t = 0; t < MAX_TRACKS; t++) {
                if (frames[f].notes[t] != -1) {
                    all_active[active_ptr].frame_idx = f;
                    all_active[active_ptr].track_idx = t;
                    all_active[active_ptr].mag = frames[f].mags[t];
                    active_ptr++;
                }
            }
        }

        qsort(all_active, current_note_count, sizeof(NoteRef), compare_notes);

        int to_remove = current_note_count - target_count;
        for (int i = 0; i < to_remove; i++) {
            frames[all_active[i].frame_idx].notes[all_active[i].track_idx] = -1;
        }
        free(all_active);
    }

    FILE* mid = fopen(out_name, "wb");
    fwrite("MThd", 1, 4, mid);
    write_u32(mid, 6);
    write_u16(mid, 1); // Format 1
    write_u16(mid, 1 + MAX_TRACKS); 
    write_u16(mid, 480); 

    // Track 0: Meta (Tempo)
    fwrite("MTrk", 1, 4, mid);
    write_u32(mid, 11);
    fputc(0x00, mid); fputc(0xFF, mid); fputc(0x51, mid); fputc(0x03, mid);
    uint32_t mpqn = 60000000 / bpm;
    fputc((mpqn >> 16) & 0xFF, mid); fputc((mpqn >> 8) & 0xFF, mid); fputc(mpqn & 0xFF, mid);
    write_varlen(mid, 0);
    fputc(0xFF, mid); fputc(0x2F, mid); fputc(0x00, mid);

    for (int t = 0; t < MAX_TRACKS; t++) {
        fwrite("MTrk", 1, 4, mid);
        long t_size_pos = ftell(mid);
        write_u32(mid, 0);

        uint32_t last_t = 0;
        uint32_t cur_tick = 0;
        int last_n = -1;
        int deb = 0;
        uint8_t last_p = 255;

        uint8_t instruments[] = {0, 19, 40, 71}; 
        write_varlen(mid, 0); fputc(0xC0 | t, mid); fputc(instruments[t % 4], mid);

        for (size_t f = 0; f < frame_count; f++) {
            int note = frames[f].notes[t];
            uint8_t pan = frames[f].pans[t];

            if (note != last_n) {
                deb++;
                if (deb > 2) {
                    if (last_n != -1) {
                        write_varlen(mid, cur_tick - last_t);
                        fputc(0x80 | t, mid); fputc(last_n, mid); fputc(0, mid);
                        last_t = cur_tick;
                    }
                    if (note != -1) {
                        if (abs((int)pan - (int)last_p) > 5) {
                            write_varlen(mid, cur_tick - last_t);
                            fputc(0xB0 | t, mid); fputc(10, mid); fputc(pan, mid);
                            last_t = cur_tick;
                            last_p = pan;
                        }
                        write_varlen(mid, cur_tick - last_t);
                        
                        int velocity = (max_mag_overall > 0) ? (int)((frames[f].mags[t] / max_mag_overall) * 127 * vol_scale) : 64;
                        if (velocity > 127) velocity = 127;
                        if (velocity < 1) velocity = 1;
                        fputc(0x90 | t, mid); fputc(note, mid); fputc(velocity, mid);
                        last_t = cur_tick;
                    }
                    last_n = note; deb = 0;
                }
            } else deb = 0;
            cur_tick += (uint32_t)(((double)HOP_SIZE * bpm * 480) / (rate * 60.0));
        }
        if (last_n != -1) { write_varlen(mid, 0); fputc(0x80 | t, mid); fputc(last_n, mid); fputc(0, mid); }
        write_varlen(mid, 0); fputc(0xFF, mid); fputc(0x2F, mid); fputc(0x00, mid);

        long e_pos = ftell(mid);
        fseek(mid, t_size_pos, SEEK_SET);
        write_u32(mid, e_pos - t_size_pos - 4);
        fseek(mid, e_pos, SEEK_SET);
    }

    fclose(mid);
    free(buffer); free(fft_in); free(fft_tmp); free(pcm_window);
    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();

    return 0;
}
