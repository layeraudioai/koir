#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_escaped(FILE* out, const char* s) {
    while (*s) {
        if (*s == '"') fprintf(out, "\\\"");
        else if (*s == '\\') fprintf(out, "\\\\");
        else if (*s == '\n') fprintf(out, " "); // Flatten multi-line MML
        else if (*s == '\r') ; 
        else fputc(*s, out);
        s++;
    }
}

void write_header(FILE* out) {
    fprintf(out, "#include <stdio.h>\n#include <stdlib.h>\n");
    fprintf(out, "#ifdef _WIN32\n#include <windows.h>\n#include <conio.h>\n#else\n#include <pthread.h>\n#include <unistd.h>\n#include <termios.h>\n#include <sys/select.h>\n#endif\n\n");
    fprintf(out, "// Prototypes for mmlplay.c\n");
    fprintf(out, "void init_audio();\nvoid close_audio();\nvoid play_track(int, const char*, int);\nvoid reset_rng_state();\n");
    fprintf(out, "extern float global_tempo_scale;\nextern int layer_mute;\nextern int rng_enabled;\nextern int audio_running;\n\n");
    fprintf(out, "typedef struct { int id; const char* mml; } thread_data_t;\n\n");
    
    // Inject non-blocking input helper
    fprintf(out, "int get_input() {\n");
    fprintf(out, "#ifdef _WIN32\n    if (_kbhit()) return _getch(); return 0;\n");
    fprintf(out, "#else\n    struct timeval tv = {0, 0}; fd_set fds; FD_ZERO(&fds); FD_SET(0, &fds);\n");
    fprintf(out, "    if (select(1, &fds, NULL, NULL, &tv) > 0) {\n");
    fprintf(out, "        struct termios oldt, newt; tcgetattr(0, &oldt); newt = oldt;\n");
    fprintf(out, "        newt.c_lflag &= ~(ICANON | ECHO); tcsetattr(0, TCSANOW, &newt);\n");
    fprintf(out, "        int ch = getchar(); tcsetattr(0, TCSANOW, &oldt); return ch;\n    }\n");
    fprintf(out, "    return 0;\n#endif\n}\n\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.mml> [output.c]\n", argv[0]);
        return 1;
    }

    FILE* in = fopen(argv[1], "r");
    if (!in) { perror("Error opening input"); return 1; }

    char out_name[256];
    if (argc > 2) {
        strncpy(out_name, argv[2], 255);
    } else {
        snprintf(out_name, 255, "\"%s.c\"", argv[1]);
    }

    FILE* out = fopen(out_name, "w");
    if (!out) { perror("Error opening output"); fclose(in); return 1; }

    write_header(out);

    fprintf(out, "#ifdef _WIN32\nDWORD WINAPI play_thread(LPVOID lpParam)\n#else\nvoid* play_thread(void* lpParam)\n#endif\n{\n");
    fprintf(out, "    thread_data_t* data = (thread_data_t*)lpParam;\n");
    fprintf(out, "    play_track(data->id, data->mml, 100);\n");
    fprintf(out, "    return 0;\n}\n\n");

    fprintf(out, "int main() {\n");
    fprintf(out, "    init_audio();\n");
    fprintf(out, "    printf(\"Playing: %%s\\n\", \"%s\");\n", argv[1]);
    fprintf(out, "    printf(\"Controls: [Q]uit [W/S] Tempo [L]ayers [R]NG\\n\");\n");

    char line[2048];
    int track_count = 0;
    while (fgets(line, sizeof(line), in)) {
        if (line[0] == '{') {
            if (track_count > 0) fprintf(out, "\";\n");
            fprintf(out, "    const char* t%d = \"", track_count);
            track_count++;
        } else {
            char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
            nl = strchr(line, '\r'); if (nl) *nl = '\0';
            print_escaped(out, line);
        }
    }
    if (track_count > 0) fprintf(out, "\";\n\n");

    fprintf(out, "    thread_data_t td[%d];\n", track_count);
    fprintf(out, "#ifdef _WIN32\n    HANDLE threads[%d];\n#else\n    pthread_t threads[%d];\n#endif\n", track_count, track_count);

    for (int i = 0; i < track_count; i++) {
        fprintf(out, "    td[%d].id = %d; td[%d].mml = t%d;\n", i, i, i, i);
        fprintf(out, "#ifdef _WIN32\n    threads[%d] = CreateThread(NULL, 0, play_thread, &td[%d], 0, NULL);\n", i, i);
        fprintf(out, "#else\n    pthread_create(&threads[%d], NULL, play_thread, &td[%d]);\n#endif\n", i, i);
    }

    fprintf(out, "\n    while(audio_running) {\n");
    fprintf(out, "        int key = get_input();\n");
    fprintf(out, "        if (key == 'q' || key == 'Q') break;\n");
    fprintf(out, "        if (key == 'w' || key == 'W') global_tempo_scale *= 1.1f;\n");
    fprintf(out, "        if (key == 's' || key == 'S') global_tempo_scale *= 0.9f;\n");
    fprintf(out, "        if (key == 'l' || key == 'L') layer_mute = !layer_mute;\n");
    fprintf(out, "        if (key == 'r' || key == 'R') {\n");
    fprintf(out, "            rng_enabled = !rng_enabled;\n");
    fprintf(out, "            if (!rng_enabled) reset_rng_state();\n");
    fprintf(out, "        }\n");
    fprintf(out, "        usleep(10000);\n    }\n");

    fprintf(out, "    close_audio();\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");

    fclose(in);
    fclose(out);
    printf("Successfully converted %s to %s\n", argv[1], out_name);
    return 0;
}