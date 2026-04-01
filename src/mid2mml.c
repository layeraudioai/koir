#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(path) mkdir(path, 0777)
#endif

// MIDI Constants
#define MIDI_NOTE_ON 0x90
#define MIDI_NOTE_OFF 0x80
#define MIDI_META 0xFF
#define MIDI_SET_TEMPO 0x51

typedef struct {
    uint8_t type;
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    uint32_t delta_time;
    uint32_t absolute_time;
    uint32_t duration;
} MidiEvent;

typedef struct {
    MidiEvent* events;
    size_t event_count;
    size_t capacity;
    int is_drum_track;
    int instrument;
    int panning;
} Track;

// Helper to read Big Endian integers
uint32_t read_u32(FILE* f) {
    uint8_t b[4];
    fread(b, 1, 4, f);
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}

uint16_t read_u16(FILE* f) {
    uint8_t b[2];
    fread(b, 1, 2, f);
    return (b[0] << 8) | b[1];
}

// Read MIDI Variable Length Quantity
uint32_t read_vlq(FILE* f) {
    uint32_t value = 0;
    uint8_t byte;
    do {
        byte = fgetc(f);
        value = (value << 7) | (byte & 0x7F);
    } while (byte & 0x80);
    return value;
}

const char* NOTE_NAMES[] = {"C", "C+", "D", "D+", "E", "F", "F+", "G", "G+", "A", "A+", "B"};

char* convert_to_mml(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return NULL;

    // Read Header
    char chunk_id[4];
    fread(chunk_id, 1, 4, f);
    if (strncmp(chunk_id, "MThd", 4) != 0) { fclose(f); return NULL; }
    read_u32(f); // header size
    read_u16(f); // format
    uint16_t num_tracks = read_u16(f);
    uint16_t ticks_per_beat = read_u16(f);

    int bpm = 120;
    size_t mml_capacity = 1024 * 1024;
    char* mml_out = malloc(mml_capacity);
    mml_out[0] = '\0';

    for (int t = 0; t < num_tracks; t++) {
        fread(chunk_id, 1, 4, f);
        uint32_t track_len = read_u32(f);
        long track_start = ftell(f);

        Track track = { malloc(sizeof(MidiEvent) * 100), 0, 100, 0, 0, 64 };
        uint32_t current_abs_time = 0;
        uint8_t running_status = 0;

        while (ftell(f) < track_start + track_len) {
            uint32_t delta = read_vlq(f);
            current_abs_time += delta;
            uint8_t status = fgetc(f);

            if (!(status & 0x80)) {
                ungetc(status, f);
                status = running_status;
            } else {
                running_status = status;
            }

            uint8_t event_type = status & 0xF0;
            uint8_t channel = status & 0x0F;

            if (status == MIDI_META) {
                uint8_t type = fgetc(f);
                uint32_t len = read_vlq(f);
                if (type == MIDI_SET_TEMPO) {
                    uint32_t tempo = 0;
                    for (int i = 0; i < 3; i++) tempo = (tempo << 8) | fgetc(f);
                    bpm = 60000000 / tempo;
                } else {
                    fseek(f, len, SEEK_CUR);
                }
            } else if (event_type == MIDI_NOTE_ON || event_type == MIDI_NOTE_OFF) {
                uint8_t note = fgetc(f);
                uint8_t vel = fgetc(f);
                
                if (channel == 9) track.is_drum_track = 1;

                if (track.event_count >= track.capacity) {
                    track.capacity *= 2;
                    track.events = realloc(track.events, sizeof(MidiEvent) * track.capacity);
                }

                MidiEvent* ev = &track.events[track.event_count++];
                ev->type = (event_type == MIDI_NOTE_ON && vel > 0) ? MIDI_NOTE_ON : MIDI_NOTE_OFF;
                ev->channel = channel;
                ev->note = note;
                ev->velocity = vel;
                ev->delta_time = delta;
                ev->absolute_time = current_abs_time;
                ev->duration = 0;
            } else if (event_type == 0xB0) { // CC
                uint8_t cc = fgetc(f);
                uint8_t val = fgetc(f);
                if (cc == 10) track.panning = val;
            } else if (event_type == 0xC0) { // Program Change
                track.instrument = fgetc(f);
            } else if (event_type == 0xC0 || event_type == 0xD0) {
                fgetc(f);
            } else {
                fgetc(f); fgetc(f);
            }
        }

        // Process Track to MML string
        if (track.event_count > 0) {
            char track_buf[1024 * 100] = {0};
            char temp[128];
            sprintf(temp, "{%s}\nT%d@%dP%d", track.is_drum_track ? "Drums" : "Track", bpm, track.instrument, track.panning);
            strcat(track_buf, temp);

            int current_octave = -1;
            int current_volume = -1;
            for (size_t i = 0; i < track.event_count; i++) {
                if (track.events[i].type == MIDI_NOTE_ON) {
                    // Handle Rests
                    if (track.events[i].delta_time > 0) {
                        int g = (int)((double)track.events[i].delta_time / ticks_per_beat * 4);
                        if (g >= 1) {
                            int r_val = (g < 16) ? (16 / g) : 64;
                            if (r_val < 1) r_val = 1;
                            sprintf(temp, "R%d", r_val);
                            strcat(track_buf, temp);
                        }
                    }

                    // Handle Octave
                    int octave = (track.events[i].note / 12) - 1;
                    if (octave != current_octave) {
                        sprintf(temp, "O%d", octave);
                        strcat(track_buf, temp);
                        current_octave = octave;
                    }

                    // Handle Volume
                    int vel = track.events[i].velocity;
                    if (vel != current_volume) {
                        sprintf(temp, "V%d", vel);
                        strcat(track_buf, temp);
                        current_volume = vel;
                    }

                    // Calculate Duration (look ahead for note off)
                    uint32_t duration_ticks = ticks_per_beat; // default
                    for (size_t j = i + 1; j < track.event_count; j++) {
                        if (track.events[j].note == track.events[i].note) {
                            duration_ticks = track.events[j].absolute_time - track.events[i].absolute_time;
                            break;
                        }
                    }

                    double e = (double)duration_ticks / ticks_per_beat;
                    int b = 16;
                    if (e >= 3.5) b = 1;
                    else if (e >= 1.5) b = 2;
                    else if (e >= 0.75) b = 4;
                    else if (e >= 0.35) b = 8;
                    
                    sprintf(temp, "%s%d", NOTE_NAMES[track.events[i].note % 12], b);
                    strcat(track_buf, temp);
                }
            }
            strcat(mml_out, track_buf);
            strcat(mml_out, "\n");
        }
        free(track.events);
    }

    fclose(f);
    return mml_out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <midi_file>\n", argv[0]);
        return 1;
    }

    const char* input_path = argv[1];
    
    // Extract filename without extension
    char base_name[256];
    const char* last_slash = strrchr(input_path, '/');
    if (!last_slash) last_slash = strrchr(input_path, '\\');
    const char* file_part = last_slash ? last_slash + 1 : input_path;
    strcpy(base_name, file_part);
    char* dot = strrchr(base_name, '.');
    if (dot) *dot = '\0';

    // Create directory
    MKDIR("mml");

    char output_path[512];
    sprintf(output_path, "mml/%s.mml", base_name);

    char* mml_content = convert_to_mml(input_path);
    if (mml_content) {
        FILE* out_f = fopen(output_path, "w");
        if (out_f) {
            fprintf(out_f, "%s", mml_content);
            fclose(out_f);
            printf("Generated %s\n", output_path);
        } else {
            printf("Error writing to file %s\n", output_path);
        }
        free(mml_content);
    } else {
        printf("Error converting MIDI: Could not parse file.\n");
        return 1;
    }

    return 0;
}
