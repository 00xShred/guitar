#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <locale.h>
#include <jack/jack.h>
#include <raylib.h>

// ── DSP globals ───────────────────────────────────────────────────────────────

jack_port_t   *input_port;
jack_port_t   *output_port;
jack_nframes_t sample_rate = 48000;

volatile float bit_depth  = 8.0f;
volatile int   reduction  = 1;
volatile float input_gain = 1.0f;
volatile float drive      = 0.0f;
volatile float tone       = 1.0f;
volatile float mix        = 1.0f;
volatile float trem_rate  = 0.0f;
volatile float trem_depth = 0.5f;

volatile int   bypass     = 0;
volatile float vu_in      = 0.0f;   // peak input level  (set by DSP, read by UI)
volatile float vu_out     = 0.0f;   // peak output level (set by DSP, read by UI)

// ── Preset data ───────────────────────────────────────────────────────────────

#define MAX_PRESETS 32
#define NAME_LEN    32

typedef struct {
    char  name[NAME_LEN];
    float bit, rate, gain, drive, tone, mix, trem, depth;
} Preset;

static Preset          presets[MAX_PRESETS];
static int             n_presets     = 0;
static pthread_mutex_t presets_lock  = PTHREAD_MUTEX_INITIALIZER;
static volatile int    reload_flag   = 0;
static time_t          presets_mtime = 0;
static char            presets_path[512];
static char            script_dir[512];

// ── Sweep subprocess ──────────────────────────────────────────────────────────

static pid_t       sweep_pid  = -1;
static const char *sweep_modes[] = {"bit", "rate", "drive", "tone", "all"};
#define N_SWEEP_MODES 5
static int         sweep_midx = 0;

// ── Status message ────────────────────────────────────────────────────────────

static char   status_msg[128] = "";
static time_t status_time     = 0;

// ── Parameter table ───────────────────────────────────────────────────────────

typedef struct { const char *key; float min, max, step; int is_int; } ParamDef;

static ParamDef param_defs[] = {
    {"bit",   1.0f, 24.0f, 0.5f,  0},
    {"rate",  1.0f, 64.0f, 1.0f,  1},
    {"gain",  0.1f,  4.0f, 0.1f,  0},
    {"drive", 0.0f, 10.0f, 0.5f,  0},
    {"tone",  0.0f,  1.0f, 0.05f, 0},
    {"mix",   0.0f,  1.0f, 0.05f, 0},
    {"trem",  0.0f, 20.0f, 0.5f,  0},
    {"depth", 0.0f,  1.0f, 0.05f, 0},
};
#define N_PARAMS 8

static float get_param(int i) {
    switch (i) {
        case 0: return (float)bit_depth;
        case 1: return (float)reduction;
        case 2: return input_gain;
        case 3: return drive;
        case 4: return tone;
        case 5: return mix;
        case 6: return trem_rate;
        case 7: return trem_depth;
    }
    return 0.0f;
}

static void set_param(int i, float v) {
    switch (i) {
        case 0: bit_depth  = v;      break;
        case 1: reduction  = (int)v; break;
        case 2: input_gain = v;      break;
        case 3: drive      = v;      break;
        case 4: tone       = v;      break;
        case 5: mix        = v;      break;
        case 6: trem_rate  = v;      break;
        case 7: trem_depth = v;      break;
    }
}

// ── Pipe listener thread ──────────────────────────────────────────────────────

static void *pipe_listener(void *arg) {
    (void)arg;
    const char *pipe_path = "/tmp/crush_pipe";
    mkfifo(pipe_path, 0666);

    while (1) {
        FILE *fp = fopen(pipe_path, "r");
        if (fp) {
            char cmd[32];
            float val;
            if (fscanf(fp, "%31s %f", cmd, &val) == 2) {
                if      (strcmp(cmd, "bit")   == 0 && val >= 1.0f && val <= 24.0f)
                    bit_depth = val;
                else if (strcmp(cmd, "rate")  == 0 && val >= 1    && val <= 64)
                    reduction = (int)val;
                else if (strcmp(cmd, "gain")  == 0 && val > 0.0f  && val <= 4.0f)
                    input_gain = val;
                else if (strcmp(cmd, "drive") == 0 && val >= 0.0f && val <= 10.0f)
                    drive = val;
                else if (strcmp(cmd, "tone")  == 0 && val >= 0.0f && val <= 1.0f)
                    tone = val;
                else if (strcmp(cmd, "mix")   == 0 && val >= 0.0f && val <= 1.0f)
                    mix = val;
                else if (strcmp(cmd, "trem")  == 0 && val >= 0.0f && val <= 20.0f)
                    trem_rate = val;
                else if (strcmp(cmd, "depth") == 0 && val >= 0.0f && val <= 1.0f)
                    trem_depth = val;
            }
            fclose(fp);
        }
    }
    return NULL;
}

// ── DSP process callback ──────────────────────────────────────────────────────

int process(jack_nframes_t nframes, void *arg) {
    (void)arg;
    jack_default_audio_sample_t *in  = jack_port_get_buffer(input_port,  nframes);
    jack_default_audio_sample_t *out = jack_port_get_buffer(output_port, nframes);

    static float last_val     = 0.0f;
    static int   sample_count = 0;
    static float tone_prev    = 0.0f;
    static float trem_phase   = 0.0f;

    float peak_in  = 0.0f;
    float peak_out = 0.0f;

    // Bypass: pass audio through unprocessed
    if (bypass) {
        for (int i = 0; i < (int)nframes; i++) {
            float a = fabsf(in[i]);
            if (a > peak_in) peak_in = a;
            out[i] = in[i];
        }
        vu_in  = peak_in;
        vu_out = peak_in;
        return 0;
    }

    float steps      = powf(2.0f, bit_depth);
    float gain       = input_gain;
    int   rate       = reduction;
    float drv        = drive;
    float tone_coeff = tone;
    float wet        = mix;
    float tr_rate    = trem_rate;
    float tr_depth   = trem_depth;
    float sr         = (float)sample_rate;

    float trem_inc = (tr_rate > 0.0f) ? (2.0f * (float)M_PI * tr_rate / sr) : 0.0f;

    for (int i = 0; i < (int)nframes; i++) {
        float dry = in[i];
        float a_in = fabsf(dry);
        if (a_in > peak_in) peak_in = a_in;

        sample_count++;
        if (sample_count >= rate) {
            float x = dry * gain;

            float scale = 1.0f + drv;
            x = tanhf(x * scale) / scale;

            x = roundf(x * steps) / steps;

            tone_prev = tone_prev + tone_coeff * (x - tone_prev);
            x = tone_prev;

            last_val = x;
            sample_count = 0;
        }

        float trem_gain  = 1.0f - tr_depth * (0.5f - 0.5f * cosf(trem_phase));
        float wet_sample = last_val * trem_gain;
        float output     = dry * (1.0f - wet) + wet_sample * wet;

        if (output >  1.0f) output =  1.0f;
        if (output < -1.0f) output = -1.0f;
        out[i] = output;

        float a_out = fabsf(output);
        if (a_out > peak_out) peak_out = a_out;

        trem_phase += trem_inc;
        if (trem_phase > 2.0f * (float)M_PI)
            trem_phase -= 2.0f * (float)M_PI;
    }

    vu_in  = peak_in;
    vu_out = peak_out;
    return 0;
}

// ── JSON preset loader ────────────────────────────────────────────────────────

static int load_presets(void) {
    struct stat st;
    if (stat(presets_path, &st) != 0) return 0;

    FILE *f = fopen(presets_path, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t nread = fread(buf, 1, sz, f);
    buf[nread] = '\0';
    fclose(f);

    Preset tmp[MAX_PRESETS];
    int count = 0;
    int depth = 0;
    const char *p = buf;

    while (*p && count < MAX_PRESETS) {
        if (*p == '{') { depth++; p++; continue; }
        if (*p == '}') { depth--; p++; continue; }

        if (*p == '"' && depth == 1) {
            // Preset name
            p++;
            char *np = tmp[count].name;
            int nlen = 0;
            while (*p && *p != '"' && nlen < NAME_LEN - 1) { *np++ = *p++; nlen++; }
            *np = '\0';
            if (*p == '"') p++;

            // Skip to opening '{' of preset object
            while (*p && *p != '{' && *p != '}') p++;
            if (*p != '{') continue;
            depth++; p++;

            // Defaults (gain not stored in presets — keep 1.0 as placeholder)
            tmp[count].bit   = 8.0f;
            tmp[count].rate  = 1.0f;
            tmp[count].gain  = 1.0f;
            tmp[count].drive = 0.0f;
            tmp[count].tone  = 1.0f;
            tmp[count].mix   = 1.0f;
            tmp[count].trem  = 0.0f;
            tmp[count].depth = 0.5f;

            // Parse field:value pairs
            while (*p && depth == 2) {
                if (*p == '}') { depth--; p++; break; }
                if (*p == '"') {
                    p++;
                    char field[16]; int fl = 0;
                    while (*p && *p != '"' && fl < 15) field[fl++] = *p++;
                    field[fl] = '\0';
                    if (*p == '"') p++;
                    while (*p && *p != ':') p++;
                    if (*p == ':') p++;
                    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                    float val = strtof(p, (char **)&p);
                    if      (!strcmp(field, "bit"))   tmp[count].bit   = val;
                    else if (!strcmp(field, "rate"))  tmp[count].rate  = val;
                    else if (!strcmp(field, "gain"))  tmp[count].gain  = val;
                    else if (!strcmp(field, "drive")) tmp[count].drive = val;
                    else if (!strcmp(field, "tone"))  tmp[count].tone  = val;
                    else if (!strcmp(field, "mix"))   tmp[count].mix   = val;
                    else if (!strcmp(field, "trem"))  tmp[count].trem  = val;
                    else if (!strcmp(field, "depth")) tmp[count].depth = val;
                } else {
                    p++;
                }
            }
            count++;
        } else {
            p++;
        }
    }

    free(buf);

    pthread_mutex_lock(&presets_lock);
    memcpy(presets, tmp, sizeof(Preset) * count);
    n_presets     = count;
    presets_mtime = st.st_mtime;
    pthread_mutex_unlock(&presets_lock);
    return count;
}

// ── Hotreload thread ──────────────────────────────────────────────────────────

static void *hotreload_thread(void *arg) {
    (void)arg;
    while (1) {
        usleep(500000);
        struct stat st;
        if (stat(presets_path, &st) == 0 && st.st_mtime != presets_mtime) {
            if (load_presets() > 0)
                reload_flag = 1;
        }
    }
    return NULL;
}

// ── Sweep helpers ─────────────────────────────────────────────────────────────

static void kill_sweep(void) {
    if (sweep_pid > 0) {
        kill(sweep_pid, SIGTERM);
        waitpid(sweep_pid, NULL, 0);
        sweep_pid = -1;
    }
}

static void start_sweep(void) {
    kill_sweep();
    char sweep_path[600];
    snprintf(sweep_path, sizeof(sweep_path), "%s/sweep.sh", script_dir);
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("bash", "bash", sweep_path, sweep_modes[sweep_midx], "mid", NULL);
        _exit(1);
    } else if (pid > 0) {
        sweep_pid = pid;
    }
}

// ── Apply preset (writes directly to volatile globals) ───────────────────────

static void apply_preset(int idx) {
    pthread_mutex_lock(&presets_lock);
    if (idx < 0 || idx >= n_presets) { pthread_mutex_unlock(&presets_lock); return; }
    Preset p = presets[idx];
    pthread_mutex_unlock(&presets_lock);

    bit_depth  = p.bit;
    reduction  = (int)p.rate;
    // gain not stored in presets — leave as-is
    drive      = p.drive;
    tone       = p.tone;
    mix        = p.mix;
    trem_rate  = p.trem;
    trem_depth = p.depth;
}

// ── Save presets to JSON ──────────────────────────────────────────────────────

static int save_presets(void) {
    FILE *f = fopen(presets_path, "w");
    if (!f) return 0;
    fprintf(f, "{\n");
    pthread_mutex_lock(&presets_lock);
    for (int i = 0; i < n_presets; i++) {
        Preset *p = &presets[i];
        fprintf(f, "  \"%s\": {\n", p->name);
        fprintf(f, "    \"bit\": %.1f,\n",   p->bit);
        fprintf(f, "    \"rate\": %.0f,\n",  p->rate);
        fprintf(f, "    \"gain\": %.2f,\n",  p->gain);
        fprintf(f, "    \"drive\": %.1f,\n", p->drive);
        fprintf(f, "    \"tone\": %.2f,\n",  p->tone);
        fprintf(f, "    \"mix\": %.2f,\n",   p->mix);
        fprintf(f, "    \"trem\": %.1f,\n",  p->trem);
        fprintf(f, "    \"depth\": %.2f\n",  p->depth);
        fprintf(f, "  }%s\n", (i < n_presets - 1) ? "," : "");
    }
    pthread_mutex_unlock(&presets_lock);
    fprintf(f, "}\n");
    fclose(f);

    // Update mtime so hotreload doesn't re-trigger
    struct stat st;
    if (stat(presets_path, &st) == 0) presets_mtime = st.st_mtime;
    return 1;
}

// ── Adjust helpers ────────────────────────────────────────────────────────────

static void adjust_param(int sel, float delta) {
    ParamDef *d = &param_defs[sel];
    float v = get_param(sel) + delta * d->step;
    if (v < d->min) v = d->min;
    if (v > d->max) v = d->max;
    if (d->is_int) v = roundf(v);
    else           v = roundf(v * 10000.0f) / 10000.0f;
    set_param(sel, v);
}

static void fine_adjust(int sel, float delta) {
    ParamDef *d = &param_defs[sel];
    float fine = d->step / 5.0f;
    float v = get_param(sel) + delta * fine;
    if (v < d->min) v = d->min;
    if (v > d->max) v = d->max;
    if (d->is_int) v = roundf(v);
    else           v = roundf(v * 100000.0f) / 100000.0f;
    set_param(sel, v);
}

// ── Signal flag ───────────────────────────────────────────────────────────────

static volatile int quit_flag = 0;
static void handle_signal(int sig) { (void)sig; quit_flag = 1; }

// ── GUI colors ───────────────────────────────────────────────────────────────

#define BG_COLOR       CLITERAL(Color){20, 20, 35, 255}
#define PANEL_COLOR    CLITERAL(Color){30, 30, 50, 255}
#define TRACK_COLOR    CLITERAL(Color){18, 18, 30, 255}
#define TEXT_COLOR     CLITERAL(Color){190, 190, 210, 255}
#define DIM_COLOR      CLITERAL(Color){100, 100, 120, 255}
#define HELP_BG        CLITERAL(Color){15, 15, 28, 255}
#define STATUS_COLOR   CLITERAL(Color){255, 220, 60, 255}
#define SWEEP_COLOR    CLITERAL(Color){60, 220, 100, 255}
#define PRESET_BG      CLITERAL(Color){40, 40, 65, 255}
#define PRESET_ACTIVE  CLITERAL(Color){180, 80, 220, 255}

static Color slider_colors[N_PARAMS] = {
    {0, 190, 255, 255},    // bit   - cyan
    {0, 220, 120, 255},    // rate  - green
    {255, 200, 40, 255},   // gain  - gold
    {255, 70, 50, 255},    // drive - red
    {180, 100, 255, 255},  // tone  - purple
    {80, 200, 80, 255},    // mix   - green
    {255, 130, 180, 255},  // trem  - pink
    {120, 180, 255, 255},  // depth - light blue
};

// ── GUI layout constants ─────────────────────────────────────────────────────

#define WIN_W       800
#define WIN_H       520
#define MARGIN      24
#define SLIDER_H    36
#define SLIDER_GAP  6
#define LABEL_W     80
#define VALUE_W     60
#define TRACK_H     14
#define CORNER_R    0.3f

// ── main ──────────────────────────────────────────────────────────────────────

int main(void) {
    // Resolve script directory from /proc/self/exe
    {
        ssize_t len = readlink("/proc/self/exe", script_dir, sizeof(script_dir) - 1);
        if (len > 0) {
            script_dir[len] = '\0';
            char *sl = strrchr(script_dir, '/');
            if (sl) *sl = '\0';
        } else {
            strncpy(script_dir, ".", sizeof(script_dir) - 1);
        }
    }
    snprintf(presets_path, sizeof(presets_path), "%s/presets.json", script_dir);

    // Load presets before UI starts
    load_presets();

    // JACK setup
    jack_client_t *client = jack_client_open("BitCrusher", JackNullOption, NULL);
    if (!client) {
        fprintf(stderr, "Could not connect to JACK server\n");
        return 1;
    }
    sample_rate = jack_get_sample_rate(client);
    input_port  = jack_port_register(client, "input",  JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput,  0);
    output_port = jack_port_register(client, "output", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    jack_set_process_callback(client, process, 0);
    jack_activate(client);

    // Start background threads
    pthread_t pipe_tid, reload_tid;
    pthread_create(&pipe_tid,   NULL, pipe_listener,  NULL);
    pthread_create(&reload_tid, NULL, hotreload_thread, NULL);

    // Signal handling
    signal(SIGTERM, handle_signal);
    signal(SIGINT,  handle_signal);
    signal(SIGCHLD, SIG_DFL);

    // raylib init
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(WIN_W, WIN_H, "BitCrusher");
    SetTargetFPS(60);

    // Load nerd mono font
    Font font = LoadFontEx("/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf", 48, NULL, 0);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);

    int selected   = 0;
    int preset_idx = 0;
    int dragging   = -1;  // which slider is being mouse-dragged (-1 = none)
    float vu_in_smooth  = 0.0f;
    float vu_out_smooth = 0.0f;

    // ── UI loop ───────────────────────────────────────────────────────────────
    while (!quit_flag && !WindowShouldClose()) {

        // Handle preset hotreload notification
        if (reload_flag) {
            reload_flag = 0;
            snprintf(status_msg, sizeof(status_msg), "presets reloaded");
            status_time = time(NULL);
        }

        // Expire status message after 2s
        if (status_msg[0] && time(NULL) - status_time >= 2)
            status_msg[0] = '\0';

        // Reap sweep child if it exited on its own
        if (sweep_pid > 0) {
            pid_t r = waitpid(sweep_pid, NULL, WNOHANG);
            if (r == sweep_pid) sweep_pid = -1;
        }

        int ww = GetScreenWidth();
        int wh = GetScreenHeight();

        // ── Input: keyboard ──────────────────────────────────────────────────
        if (IsKeyPressed(KEY_Q)) { quit_flag = 1; break; }

        if (IsKeyPressed(KEY_UP)   || IsKeyPressedRepeat(KEY_UP))    selected = (selected - 1 + N_PARAMS) % N_PARAMS;
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN))  selected = (selected + 1) % N_PARAMS;
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT))  adjust_param(selected, -1.0f);
        if (IsKeyPressed(KEY_RIGHT)|| IsKeyPressedRepeat(KEY_RIGHT)) adjust_param(selected, +1.0f);
        if (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressedRepeat(KEY_LEFT_BRACKET))   fine_adjust(selected, -1.0f);
        if (IsKeyPressed(KEY_RIGHT_BRACKET)|| IsKeyPressedRepeat(KEY_RIGHT_BRACKET))  fine_adjust(selected, +1.0f);

        // Preset number keys 1-9, 0
        for (int k = KEY_ONE; k <= KEY_NINE; k++) {
            if (IsKeyPressed(k)) {
                int idx = k - KEY_ONE;
                pthread_mutex_lock(&presets_lock);
                int np = n_presets;
                pthread_mutex_unlock(&presets_lock);
                if (idx < np) {
                    apply_preset(idx);
                    pthread_mutex_lock(&presets_lock);
                    snprintf(status_msg, sizeof(status_msg),
                             "preset > %s", presets[idx].name);
                    pthread_mutex_unlock(&presets_lock);
                    status_time = time(NULL);
                    preset_idx  = idx;
                }
            }
        }
        if (IsKeyPressed(KEY_ZERO)) {
            pthread_mutex_lock(&presets_lock);
            int np = n_presets;
            pthread_mutex_unlock(&presets_lock);
            if (9 < np) {
                apply_preset(9);
                pthread_mutex_lock(&presets_lock);
                snprintf(status_msg, sizeof(status_msg),
                         "preset > %s", presets[9].name);
                pthread_mutex_unlock(&presets_lock);
                status_time = time(NULL);
                preset_idx = 9;
            }
        }

        // Cycle presets: P (shift check)
        if (IsKeyPressed(KEY_P)) {
            pthread_mutex_lock(&presets_lock);
            int np = n_presets;
            pthread_mutex_unlock(&presets_lock);
            if (np > 0) {
                if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
                    preset_idx = (preset_idx - 1 + np) % np;
                else
                    preset_idx = (preset_idx + 1) % np;
                apply_preset(preset_idx);
                pthread_mutex_lock(&presets_lock);
                snprintf(status_msg, sizeof(status_msg),
                         "preset > %s", presets[preset_idx].name);
                pthread_mutex_unlock(&presets_lock);
                status_time = time(NULL);
            }
        }

        // Sweep: S
        if (IsKeyPressed(KEY_S)) {
            if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                kill_sweep();
                snprintf(status_msg, sizeof(status_msg), "sweep stopped");
                status_time = time(NULL);
            } else {
                if (sweep_pid > 0) {
                    sweep_midx = (sweep_midx + 1) % N_SWEEP_MODES;
                    kill_sweep();
                } else {
                    sweep_midx = 0;
                }
                start_sweep();
                snprintf(status_msg, sizeof(status_msg),
                         "sweep > %s", sweep_modes[sweep_midx]);
                status_time = time(NULL);
            }
        }

        // Bypass toggle
        if (IsKeyPressed(KEY_B) && !IsKeyDown(KEY_LEFT_SHIFT) && !IsKeyDown(KEY_RIGHT_SHIFT)) {
            bypass = !bypass;
            snprintf(status_msg, sizeof(status_msg),
                     bypass ? "BYPASS ON" : "BYPASS OFF");
            status_time = time(NULL);
        }

        // Save current settings to active preset (W) or new preset (Shift+W)
        if (IsKeyPressed(KEY_W)) {
            char saved_name[NAME_LEN];
            pthread_mutex_lock(&presets_lock);
            int np = n_presets;
            int target = preset_idx;
            if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                // New preset
                if (np < MAX_PRESETS) {
                    target = np;
                    snprintf(presets[target].name, NAME_LEN, "user_%d", target + 1);
                    n_presets = np + 1;
                }
            }
            if (target < n_presets) {
                presets[target].bit   = bit_depth;
                presets[target].rate  = (float)reduction;
                presets[target].gain  = input_gain;
                presets[target].drive = drive;
                presets[target].tone  = tone;
                presets[target].mix   = mix;
                presets[target].trem  = trem_rate;
                presets[target].depth = trem_depth;
                preset_idx = target;
            }
            strncpy(saved_name, presets[preset_idx].name, NAME_LEN - 1);
            saved_name[NAME_LEN - 1] = '\0';
            pthread_mutex_unlock(&presets_lock);
            save_presets();
            snprintf(status_msg, sizeof(status_msg), "saved > %s", saved_name);
            status_time = time(NULL);
        }

        // ── Dynamic layout ────────────────────────────────────────────────────
        // Reserve space for title, bottom bar, presets, sweep
        int title_size   = wh > 400 ? 32 : 24;
        int title_top    = MARGIN;
        int title_bottom = title_top + title_size + 12;

        int help_h       = 36;
        int bottom_zone  = 110; // presets + sweep + help bar
        int slider_zone  = wh - title_bottom - bottom_zone;
        int slider_h     = slider_zone / N_PARAMS;
        if (slider_h < 30) slider_h = 30;
        if (slider_h > 80) slider_h = 80;
        int slider_total = slider_h * N_PARAMS;
        int slider_top   = title_bottom + (slider_zone - slider_total) / 2;

        int label_size   = slider_h > 50 ? 22 : 18;
        int value_size   = slider_h > 50 ? 18 : 16;
        int track_h      = slider_h * 2 / 5;
        if (track_h < 10) track_h = 10;
        if (track_h > 24) track_h = 24;
        int thumb_r      = track_h / 2 + 2;

        int track_x = MARGIN + LABEL_W + 12;
        int track_w = ww - track_x - MARGIN - VALUE_W - 12;
        if (track_w < 100) track_w = 100;

        int div_y    = slider_top + slider_total + 8;
        int preset_y = div_y + 14;
        int sweep_y  = preset_y + 44;

        // ── Input: mouse ─────────────────────────────────────────────────────
        Vector2 mouse = GetMousePosition();

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            // Check slider rows
            for (int i = 0; i < N_PARAMS; i++) {
                int row_y = slider_top + i * slider_h;
                if (mouse.y >= row_y && mouse.y < row_y + slider_h) {
                    selected = i;
                    if (mouse.x >= track_x && mouse.x <= track_x + track_w) {
                        dragging = i;
                    }
                }
            }

            // Check preset buttons
            if (mouse.y >= preset_y && mouse.y < preset_y + 36) {
                pthread_mutex_lock(&presets_lock);
                int np = n_presets;
                pthread_mutex_unlock(&presets_lock);
                int bx = MARGIN;
                for (int j = 0; j < np && j < 10; j++) {
                    pthread_mutex_lock(&presets_lock);
                    int tw = MeasureText(presets[j].name, 16) + 20;
                    pthread_mutex_unlock(&presets_lock);
                    if (mouse.x >= bx && mouse.x < bx + tw) {
                        apply_preset(j);
                        preset_idx = j;
                        pthread_mutex_lock(&presets_lock);
                        snprintf(status_msg, sizeof(status_msg),
                                 "preset > %s", presets[j].name);
                        pthread_mutex_unlock(&presets_lock);
                        status_time = time(NULL);
                        break;
                    }
                    bx += tw + 6;
                }
            }
        }

        // Dragging slider
        if (dragging >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float frac = (mouse.x - (float)track_x) / (float)track_w;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            ParamDef *d = &param_defs[dragging];
            float v = d->min + frac * (d->max - d->min);
            if (d->is_int) v = roundf(v);
            else           v = roundf(v * 10000.0f) / 10000.0f;
            if (v < d->min) v = d->min;
            if (v > d->max) v = d->max;
            set_param(dragging, v);
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) dragging = -1;

        // ── VU smoothing (peak hold with decay) ──────────────────────────────
        float dt = GetFrameTime();
        float raw_in  = vu_in;
        float raw_out = vu_out;
        if (raw_in  > vu_in_smooth)  vu_in_smooth  = raw_in;
        else                         vu_in_smooth  -= vu_in_smooth * 8.0f * dt;
        if (raw_out > vu_out_smooth) vu_out_smooth = raw_out;
        else                         vu_out_smooth -= vu_out_smooth * 8.0f * dt;
        if (vu_in_smooth  < 0.001f) vu_in_smooth  = 0.0f;
        if (vu_out_smooth < 0.001f) vu_out_smooth = 0.0f;

        // ── Render ───────────────────────────────────────────────────────────
        BeginDrawing();
        ClearBackground(BG_COLOR);

        // Helper: spacing = fontSize / 10
        #define DTEX(text, x, y, sz, col) \
            DrawTextEx(font, (text), (Vector2){(x),(y)}, (sz), (sz)/10.0f, (col))
        #define MTEX(text, sz) \
            MeasureTextEx(font, (text), (sz), (sz)/10.0f)

        // Title
        const char *title = "BitCrusher";
        Vector2 tsz = MTEX(title, title_size);
        DTEX(title, (ww - tsz.x) / 2, title_top, title_size, TEXT_COLOR);

        // Bypass indicator
        if (bypass) {
            Color bypass_col = {255, 80, 60, 255};
            DTEX("BYPASS", MARGIN, title_top + (float)(title_size - 18) / 2, 18, bypass_col);
        }

        // VU meters (right side of title bar)
        {
            int vu_w = 100;
            int vu_h = 8;
            int vu_x = ww - MARGIN - vu_w;
            int vu_y1 = title_top + 4;
            int vu_y2 = vu_y1 + vu_h + 4;
            float in_lvl  = vu_in_smooth  > 1.0f ? 1.0f : vu_in_smooth;
            float out_lvl = vu_out_smooth > 1.0f ? 1.0f : vu_out_smooth;

            DTEX("IN", vu_x - 30, vu_y1 - 1, 10, DIM_COLOR);
            DrawRectangleRounded((Rectangle){vu_x, vu_y1, vu_w, vu_h}, 0.5f, 4, TRACK_COLOR);
            if (in_lvl > 0.0f) {
                Color c = in_lvl > 0.9f ? (Color){255,60,40,255} : (Color){0,200,120,255};
                DrawRectangleRounded((Rectangle){vu_x, vu_y1, (int)(in_lvl * vu_w), vu_h}, 0.5f, 4, c);
            }

            DTEX("OUT", vu_x - 34, vu_y2 - 1, 10, DIM_COLOR);
            DrawRectangleRounded((Rectangle){vu_x, vu_y2, vu_w, vu_h}, 0.5f, 4, TRACK_COLOR);
            if (out_lvl > 0.0f) {
                Color c = out_lvl > 0.9f ? (Color){255,60,40,255} : (Color){0,180,255,255};
                DrawRectangleRounded((Rectangle){vu_x, vu_y2, (int)(out_lvl * vu_w), vu_h}, 0.5f, 4, c);
            }
        }

        // Title divider
        DrawRectangle(MARGIN, title_bottom - 4, ww - 2 * MARGIN, 1, DIM_COLOR);

        // Parameter sliders
        for (int i = 0; i < N_PARAMS; i++) {
            ParamDef *d   = &param_defs[i];
            float     val = get_param(i);
            int       y   = slider_top + i * slider_h;
            int is_sel = (i == selected);

            // Row background on select
            if (is_sel) {
                DrawRectangleRounded(
                    (Rectangle){MARGIN - 4, y, ww - 2 * MARGIN + 8, slider_h},
                    CORNER_R, 4, PANEL_COLOR);
            }

            // Label
            Color label_col = is_sel ? slider_colors[i] : DIM_COLOR;
            char label_upper[8];
            for (int c = 0; d->key[c] && c < 6; c++)
                label_upper[c] = (d->key[c] >= 'a' && d->key[c] <= 'z')
                    ? d->key[c] - 32 : d->key[c];
            label_upper[strlen(d->key) < 6 ? strlen(d->key) : 6] = '\0';
            DTEX(label_upper, MARGIN, y + (float)(slider_h - label_size) / 2, label_size, label_col);

            // Slider track
            int ty = y + (slider_h - track_h) / 2;
            DrawRectangleRounded(
                (Rectangle){track_x, ty, track_w, track_h},
                0.5f, 4, TRACK_COLOR);

            // Filled portion
            float frac = (d->max > d->min)
                ? (val - d->min) / (d->max - d->min) : 0.0f;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            int fill_w = (int)(frac * (float)track_w);
            if (fill_w > 0) {
                Color fill = slider_colors[i];
                if (!is_sel) {
                    fill.r = fill.r * 2 / 3;
                    fill.g = fill.g * 2 / 3;
                    fill.b = fill.b * 2 / 3;
                }
                DrawRectangleRounded(
                    (Rectangle){track_x, ty, fill_w, track_h},
                    0.5f, 4, fill);
            }

            // Thumb indicator
            int thumb_x = track_x + fill_w;
            if (is_sel) {
                DrawCircle(thumb_x, ty + track_h / 2, thumb_r, slider_colors[i]);
                DrawCircle(thumb_x, ty + track_h / 2, thumb_r - 3, BG_COLOR);
            }

            // Value text
            char fmt[16];
            if (d->is_int) snprintf(fmt, sizeof(fmt), "%d",   (int)val);
            else           snprintf(fmt, sizeof(fmt), "%.2f", val);
            int vx = track_x + track_w + 12;
            Color val_col = is_sel ? TEXT_COLOR : DIM_COLOR;
            DTEX(fmt, vx, y + (float)(slider_h - value_size) / 2, value_size, val_col);
        }

        // Divider below params
        DrawRectangle(MARGIN, div_y, ww - 2 * MARGIN, 1, DIM_COLOR);

        // Preset buttons
        {
            pthread_mutex_lock(&presets_lock);
            int np = n_presets;
            char pnames[MAX_PRESETS][NAME_LEN];
            for (int j = 0; j < np; j++)
                strncpy(pnames[j], presets[j].name, NAME_LEN - 1);
            pthread_mutex_unlock(&presets_lock);

            int pfont = 18;
            int ph = 36;
            int bx = MARGIN;
            for (int j = 0; j < np && j < 10; j++) {
                char tag[NAME_LEN + 4];
                char key = (j < 9) ? (char)('1' + j) : '0';
                snprintf(tag, sizeof(tag), "%c:%s", key, pnames[j]);
                Vector2 tsz2 = MTEX(tag, pfont);
                int tw = (int)tsz2.x + 24;
                Color bg = (j == preset_idx) ? PRESET_ACTIVE : PRESET_BG;
                Color fg = (j == preset_idx) ? (Color){255,255,255,255} : DIM_COLOR;
                DrawRectangleRounded(
                    (Rectangle){bx, preset_y, tw, ph}, 0.4f, 4, bg);
                DTEX(tag, bx + 12, preset_y + (float)(ph - pfont) / 2, pfont, fg);
                bx += tw + 8;
                if (bx > ww - MARGIN) break;
            }
        }

        // Sweep status
        {
            int sweep_active = (sweep_pid > 0);
            char label[128];
            Color col;
            if (sweep_active) {
                snprintf(label, sizeof(label),
                         "SWEEP: %s  [s] next  [S] stop", sweep_modes[sweep_midx]);
                col = SWEEP_COLOR;
                float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 4.0f);
                DrawCircle(MARGIN + 4, sweep_y + 9, 4,
                           (Color){60, (unsigned char)(220 * pulse), 100, 255});
                DTEX(label, MARGIN + 14, sweep_y, 16, col);
            } else {
                snprintf(label, sizeof(label), "SWEEP: off  [s] start");
                col = DIM_COLOR;
                DTEX(label, MARGIN, sweep_y, 16, col);
            }
        }

        // Status flash message
        if (status_msg[0]) {
            Vector2 msz = MTEX(status_msg, 16);
            DTEX(status_msg, ww - MARGIN - msz.x, sweep_y, 16, STATUS_COLOR);
        }

        // Help bar at bottom
        {
            int hfont = 16;
            DrawRectangle(0, wh - help_h, ww, help_h, HELP_BG);
            const char *help = "LEFT/RIGHT: adjust  UP/DOWN: select  [ ]: fine  p/P: preset  w: save  W: save new  s/S: sweep  b: bypass  q: quit";
            DTEX(help, MARGIN, wh - help_h + (float)(help_h - hfont) / 2, hfont, DIM_COLOR);
        }

        #undef DTEX
        #undef MTEX
        EndDrawing();
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    UnloadFont(font);
    CloseWindow();
    kill_sweep();
    jack_deactivate(client);
    jack_client_close(client);
    return 0;
}
