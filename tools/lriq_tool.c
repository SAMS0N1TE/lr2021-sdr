/* Native C CLI. Windows serial transport; analysis and library are portable. */
#include "lriq.h"
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#ifdef _WIN32
static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig; stopping = 1; }
#endif
static int unpack_fifo(const char *input, const char *output)
{
    FILE *in=fopen(input,"r"); if(!in)return perror(input),1;
    uint8_t *data=malloc(125512); if(!data){fclose(in);return 1;}
    lriq_fifo_parser p; lriq_fifo_init(&p,data,125512);
    char line[1024];
    while(fgets(line,sizeof(line),in)) {
        size_t n=strlen(line);
        if(n==sizeof(line)-1 && line[n-1]!='\n'){p.failed=1;break;}
        line[strcspn(line,"\r\n")]=0;
        if(lriq_fifo_line(&p,line)<0)break;
    }
    int bad=ferror(in) || p.failed || !p.complete; fclose(in);
    if(bad){free(data);fprintf(stderr,"Incomplete, corrupt or rejected FIFO capture\n");return 1;}
    FILE *out=fopen(output,"wbx"); if(!out){free(data);return perror(output),1;}
    bad=fwrite(data,1,p.bytes,out)!=p.bytes;
    if(fclose(out))bad=1;
    free(data);
    if(!bad)printf("Validated %zu bytes (%zu IQ16 pairs); sample rate supplied separately.\n",p.bytes,p.bytes/4);
    return bad ? 1 : 0;
}
/* One second of signed IQ8 at 2 MS/s. Repeat with hackrf_transfer -R. */
static int make_tone(const char *name, const char *frequency)
{
    char *end; double hz = strtod(frequency, &end);
    if (!*frequency || *end || !isfinite(hz) || fabs(hz) >= 1000000) return 2;
    FILE *f = fopen(name, "wbx"); if (!f) return perror(name), 1;
    for (unsigned base = 0; base < 2000000; base += 1000) {
        int8_t buf[2000];
        for (unsigned j = 0; j < 1000; j++) {
            double angle = 6.2831853071795864769*hz*(base+j)/2000000;
            buf[2*j] = (int8_t)lround(32*cos(angle));
            buf[2*j+1] = (int8_t)lround(32*sin(angle));
        }
        if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) { fclose(f); return 1; }
    }
    return fclose(f) ? 1 : 0;
}
static int analyze(const char *name)
{
    lriq_capture c = {0}; lriq_tone tone;
    FILE *f = fopen(name, "rb");
    if (!f) return perror(name), 1;
    size_t n = fread(c.data, 1, sizeof(c.data), f);
    int extra = fgetc(f), bad = ferror(f); fclose(f);
    if (n != LRIQ_BYTES || extra != EOF || bad) return fprintf(stderr, "Expected exactly 1024 bytes\n"), 1;
    c.pairs = LRIQ_PAIRS;
    if (lriq_measure_tone(&c, &tone)) return fprintf(stderr, "No measurable signal\n"), 1;
    printf("{\"cycles_per_pair\":%.9f,\"peak_to_mean_db\":%.3f,\"rms_a\":%.3f,\"rms_b\":%.3f}\n",
           tone.cycles_per_pair, tone.peak_to_mean_db, tone.rms_a, tone.rms_b);
    return 0;
}
#ifdef _WIN32
static int save(const char *prefix, unsigned index, const lriq_capture *c)
{
    char name[1024];
    if (snprintf(name, sizeof(name), "%s-%04u.bin", prefix, index) >= (int)sizeof(name)) return -1;
    FILE *f = fopen(name, "wbx");
    if (!f) return perror(name), -1;
    int bad = fwrite(c->data, 1, LRIQ_BYTES, f) != LRIQ_BYTES;
    if (fclose(f)) bad = 1;
    if (bad) return -1;
    if (snprintf(name, sizeof(name), "%s-%04u.csv", prefix, index) >= (int)sizeof(name)) return -1;
    f = fopen(name, "wx"); if (!f) return perror(name), -1;
    fprintf(f, "%s", c->iq_validated ? "pair,i,q\n" : "pair,component_a,component_b\n");
    for (size_t i = 0; i < c->pairs; i++) {
        int16_t a, b; lriq_components(c, i, &a, &b);
        fprintf(f, "%zu,%d,%d\n", i, (int)a, (int)b);
    }
    bad = ferror(f); if (fclose(f)) bad = 1;
    return bad ? -1 : 0;
}
static int capture(const char *port, const char *prefix, unsigned count)
{
    char device[64], path[1024];
    if (strncmp(port, "COM", 3) || strspn(port+3, "0123456789") != strlen(port+3) || !port[3] || strlen(port) > 12)
        return fprintf(stderr, "Expected an explicit COM port\n"), 1;
    snprintf(device, sizeof(device), "\\\\.\\%s", port);
    HANDLE h = CreateFileA(device, GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return fprintf(stderr, "Cannot exclusively open %s: %lu\n", port, GetLastError()), 1;
    DCB dcb = {0}; dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return 1; }
    dcb.BaudRate = 115200; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE; dcb.fParity = FALSE; dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE; dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = FALSE; dcb.fInX = FALSE; dcb.fDsrSensitivity = FALSE; dcb.fAbortOnError = FALSE;
    COMMTIMEOUTS timeout = {MAXDWORD, 0, 100, 0, 1000};
    if (!SetCommState(h, &dcb) || !SetCommTimeouts(h, &timeout)) { CloseHandle(h); return 1; }
    if (snprintf(path, sizeof(path), "%s.jsonl", prefix) >= (int)sizeof(path)) { CloseHandle(h); return 1; }
    FILE *meta = fopen(path, "wx");
    if (!meta) { perror(path); CloseHandle(h); return 1; }
    if (snprintf(path, sizeof(path), "%s.log", prefix) >= (int)sizeof(path)) { fclose(meta); CloseHandle(h); return 1; }
    FILE *log = fopen(path, "wx");
    if (!log) { perror(path); fclose(meta); CloseHandle(h); return 1; }
    signal(SIGINT, stop);
    char chunk[1024], line[1024]; DWORD n;
    ULONGLONG drain = GetTickCount64()+1000;
    while (GetTickCount64() < drain) if (!ReadFile(h, chunk, sizeof(chunk), &n, NULL)) break;
    int rc = 0;
    for (unsigned index = 0; !stopping && (!count || index < count); index++) {
        lriq_parser p; lriq_parser_init(&p); size_t used = 0;
        ULONGLONG start = GetTickCount64();
        if (!WriteFile(h, "lora lrcap\r", 11, &n, NULL) || n != 11) { rc = 1; break; }
        while (!p.complete && !p.failed && GetTickCount64()-start < 15000) {
            if (!ReadFile(h, chunk, sizeof(chunk), &n, NULL)) { p.failed = 1; break; }
            if (fwrite(chunk, 1, n, log) != n) { p.failed = 1; break; }
            for (DWORD i = 0; i < n && !p.complete && !p.failed; i++) {
                if (chunk[i] == '\r') continue;
                if (chunk[i] == '\n') {
                    line[used] = 0; lriq_parser_line(&p, line); used = 0;
                } else if (used < sizeof(line)-1) line[used++] = chunk[i];
                else { p.failed = 1; break; }
            }
        }
        ULONGLONG elapsed = GetTickCount64()-start;
        if (!p.complete || p.failed || save(prefix, index, &p.capture)) {
            fprintf(meta, "{\"index\":%u,\"complete\":false,\"host_elapsed_ms\":%llu}\n", index, (unsigned long long)elapsed);
            fprintf(stderr, "Capture %u failed; see log. No subsequent capture attempted.\n", index);
            rc = 1; break;
        }
        char rate[32] = "null";
        if (p.capture.sample_rate_hz) snprintf(rate, sizeof(rate), "%u", p.capture.sample_rate_hz);
        fprintf(meta, "{\"index\":%u,\"complete\":true,\"frequency_hz\":%u,\"source\":2,\"pairs\":256,\"encoding\":\"two signed little-endian int16 components\",\"bandwidth_hz\":%u,\"sample_rate_hz\":%s,\"iq_validated\":%s,\"continuous\":false,\"host_start_ms\":%llu,\"host_elapsed_ms\":%llu}\n",
                index, p.capture.frequency_hz, p.capture.bandwidth_hz, rate, p.capture.iq_validated ? "true" : "false", (unsigned long long)start, (unsigned long long)elapsed);
        if (fflush(meta) || fflush(log)) { rc = 1; break; }
        printf("capture %u: %u Hz, 256 pairs, %llu ms (gaps between captures)\n", index, p.capture.frequency_hz, (unsigned long long)elapsed);
        fflush(stdout);
    }
    if (fclose(meta)) rc = 1;
    if (fclose(log)) rc = 1;
    CloseHandle(h); return rc;
}
#endif
int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "unpack-fifo")) return unpack_fifo(argv[2],argv[3]);
    if (argc == 4 && !strcmp(argv[1], "tone")) return make_tone(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "analyze")) return analyze(argv[2]);
#ifdef _WIN32
    if (argc == 5 && !strcmp(argv[1], "capture")) {
        char *end; errno = 0; unsigned long count = strtoul(argv[4], &end, 10);
        if (errno || !*argv[4] || *end || argv[4][0] == '-' || count > 1000000) return 2;
        return capture(argv[2], argv[3], (unsigned)count);
    }
#endif
    fprintf(stderr, "lriq_tool tone output.iq8 offset_hz (2 MS/s, amplitude 32)\n"
            "lriq_tool unpack-fifo capture.log output.iq16\n"
            "lriq_tool analyze capture.bin\n"
            "lriq_tool capture COM3 output-prefix count\n"
            "  count=0 repeats until Ctrl+C; each burst has gaps.\n"
            "  Ctrl+C finishes the current transaction before closing.\n"
            "  Select the correct board's COM port explicitly. Requires lrcap firmware.\n");
    return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
}
