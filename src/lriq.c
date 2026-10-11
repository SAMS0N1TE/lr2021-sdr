#include "lriq.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void lriq_parser_init(lriq_parser *p) { if (p) memset(p, 0, sizeof(*p)); }
int lriq_parser_line(lriq_parser *p, const char *line)
{
    if (!p || !line || p->failed || p->complete) return -1;
    const char *meta = strstr(line, "lrcap-meta: ");
    if (meta) {
        unsigned bw, rate, iq; int n = 0;
        if (p->next || p->capture.bandwidth_hz ||
            sscanf(meta, "lrcap-meta: bw=%u sample_rate=%u iq=%u%n", &bw, &rate, &iq, &n) != 3 ||
            meta[n] || iq > 1 || (iq && !rate) || (!iq && rate)) { p->failed = 1; return -1; }
        p->capture.bandwidth_hz = bw; p->capture.sample_rate_hz = rate; p->capture.iq_validated = (int)iq;
        return 0;
    }
    const char *s = strstr(line, "lrcap: ");
    if (!s) return 0;
    unsigned index, word, source, hz, status, bytes, words;
    int used = 0;
    char phase[32], result[48], restore[48], recovery[48];
    if (sscanf(s, "lrcap: source=%u hz=%u status=0x%x bytes=%u words=%u phase=%31s result=%47s restore=%47s recovery=%47s%n",
               &source, &hz, &status, &bytes, &words, phase, result, restore, recovery, &used) == 9) {
        if (s[used] || source != 2 || !hz || bytes != LRIQ_BYTES || words != LRIQ_PAIRS ||
            p->next != LRIQ_PAIRS || (status & 0x03000000u) != 0x03000000u ||
            strcmp(phase, "readback") || strcmp(result, "ESP_OK") ||
            strcmp(restore, "ESP_OK") || strcmp(recovery, "ESP_OK")) goto invalid;
        p->capture.frequency_hz = hz; p->capture.status = status;
        p->capture.pairs = p->next; p->complete = 1;
        return 1;
    }
    if (sscanf(s, "lrcap: %u 0x%x%n", &index, &word, &used) != 2 ||
        s[used] || index != p->next || index >= LRIQ_PAIRS) goto invalid;
    for (unsigned b = 0; b < 4; ++b) p->capture.data[index * 4 + b] = (uint8_t)(word >> (8 * b));
    ++p->next;
    return 0;
invalid:
    p->failed = 1;
    return -1;
}
void lriq_components(const lriq_capture *c, size_t i, int16_t *a, int16_t *b)
{
    if (!a || !b) return;
    *a = *b = 0;
    if (!c || i >= c->pairs || i >= LRIQ_PAIRS) return;
    const uint8_t *p = c->data + i * 4;
    unsigned x = p[0] | (unsigned)p[1] << 8, y = p[2] | (unsigned)p[3] << 8;
    *a = (int16_t)(x < 32768 ? (int)x : (int)x - 65536);
    *b = (int16_t)(y < 32768 ? (int)y : (int)y - 65536);
}
int lriq_measure_tone(const lriq_capture *c, lriq_tone *out)
{
    const double tau = 6.2831853071795864769;
    if (!c || !out || c->pairs != LRIQ_PAIRS) return -1;
    double a[LRIQ_PAIRS], b[LRIQ_PAIRS], power[LRIQ_PAIRS], ma = 0, mb = 0, ra = 0, rb = 0;
    for (size_t i = 0; i < LRIQ_PAIRS; ++i) {
        int16_t x, y; lriq_components(c, i, &x, &y);
        a[i] = x; b[i] = y; ma += x; mb += y; ra += (double)x*x; rb += (double)y*y;
    }
    ma /= LRIQ_PAIRS; mb /= LRIQ_PAIRS;
    for (size_t i = 0; i < LRIQ_PAIRS; ++i) {
        double w = 0.5 - 0.5 * cos(tau * (double)i / (LRIQ_PAIRS - 1));
        a[i] = (a[i] - ma)*w; b[i] = (b[i] - mb)*w;
    }
    unsigned peak = 0; double sum = 0;
    for (unsigned k = 0; k < LRIQ_PAIRS; ++k) {
        double re = 0, im = 0;
        for (unsigned i = 0; i < LRIQ_PAIRS; ++i) {
            double angle = tau * k * i / LRIQ_PAIRS, co = cos(angle), si = sin(angle);
            re += a[i]*co + b[i]*si; im += b[i]*co - a[i]*si;
        }
        power[k] = re*re + im*im; sum += power[k];
        if (power[k] > power[peak]) peak = k;
    }
    if (sum <= 1e-12) return -1;
    double left = log(power[(peak + LRIQ_PAIRS - 1) % LRIQ_PAIRS] + 1e-30);
    double mid = log(power[peak] + 1e-30), right = log(power[(peak + 1) % LRIQ_PAIRS] + 1e-30);
    double den = left - 2*mid + right, delta = fabs(den) > 1e-12 ? 0.5*(left-right)/den : 0;
    if (delta < -0.5 || delta > 0.5) delta = 0;
    double cycles = (peak + delta) / LRIQ_PAIRS;
    if (cycles >= 0.5) cycles -= 1;
    *out = (lriq_tone){cycles, 10*log10(power[peak] / (sum/LRIQ_PAIRS)),
                       sqrt(ra/LRIQ_PAIRS), sqrt(rb/LRIQ_PAIRS)};
    return 0;
}
int lriq_estimate_rate(double rf, double cycles, double *rate)
{
    if (!rate || !isfinite(rf) || !isfinite(cycles) || rf == 0 || fabs(cycles) < 1e-9 || fabs(cycles) >= 1) return -1;
    *rate = rf / cycles;
    return isfinite(*rate) ? 0 : -1;
}
