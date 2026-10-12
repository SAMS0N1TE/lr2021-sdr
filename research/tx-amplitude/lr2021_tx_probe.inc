/* Local ROM-1.24 research diagnostic. Fixed 915 MHz, -9 dBm, <=150 ms.
 * No raw command/register interface and no claim of arbitrary IQ support. */
static int tx_probe_command(int argc, char **argv)
{
    const char *mode = argc >= 3 ? argv[2] : "";
    bool magnitude = !strncmp(mode, "mag", 3);
    if (magnitude) mode += 3;
    bool ook = !strncmp(mode, "ook", 3);
    unsigned depth = !strncmp(mode, "ook20", 5) ? 1 : 0;
    if (ook) mode += depth ? 5 : 3;
    bool cw = argc >= 3 && !strcmp(mode, "cw");
    bool packet = argc >= 3 && !strcmp(mode, "packet");
    bool dry = argc >= 3 && !strncmp(mode, "config", 6);
    int raw = -1;
    if (argc >= 3 && ((!strncmp(mode, "raw", 3) && strlen(mode) == 4) ||
                     (dry && strlen(mode) == 7))) {
        char c = mode[dry ? 6 : 3];
        if (c >= '0' && c <= '2') raw = c - '0';
    }
    uint32_t pattern = 0, rate = 9600;
    if (argc < 3 || argc > 5 || (!cw && !packet && raw < 0) ||
        (argc >= 4 && !probe_number(argv[3], 11, &pattern)) ||
        (argc >= 5 && !probe_number(argv[4], 19200, &rate)) ||
        (rate != 4800 && rate != 9600 && rate != 19200)) {
        puts("usage: lora lrtx-probe cw|packet|config0|config1|config2|raw0|raw1|raw2 [pattern 0..11]; prefix ook or ook20 for OOK setup [rate 4800|9600|19200]");
        return 1;
    }
    if (lr20xx_stream_source2_active()) return 1;
    static const uint32_t addr[] = {0xF30800,0xF30830,0xF30838,0xF30904,0xF30910,0xF30914,0xF30930,0xF30934,0xF30900,0xF30908,0xF3090C,0xF40140,0xF40124};
    enum { REG_COUNT = sizeof(addr)/sizeof(addr[0]) };
    uint32_t before[REG_COUNT] = {0}, configured[REG_COUNT] = {0}, started[REG_COUNT] = {0};
    uint8_t payload[128], flags[2] = {0}, level[2] = {0};
    const unsigned payload_len = raw == 0 ? 128 : 64;
    uint32_t x = 0x13579BDF, irq = 0;
    for (unsigned i = 0; i < sizeof(payload); ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        payload[i] = (uint8_t)x ^ (pattern == 1 ? 0xFF : 0);
    }
    if (pattern == 2) {
        static const int8_t steps[] = {-96,-64,-32,0,32,64,96,0};
        for (unsigned i=0;i<64;++i) {
            if (raw == 0) { payload[2*i]=0; payload[2*i+1]=(uint8_t)steps[i/8]; }
            else payload[i]=(uint8_t)steps[i/8];
        }
    }
    if (pattern == 5) {
        if (raw != 0) return 1;
        for (int i=63;i>=0;--i) { payload[2*i]=payload[2*(i/4)]; payload[2*i+1]=payload[2*(i/4)+1]; }
    }
    if (pattern == 3 || pattern == 4) {
        if (raw != 0) return 1;
        static const int8_t circle[8][2]={{64,0},{45,45},{0,64},{-45,45},{-64,0},{-45,-45},{0,-64},{45,-45}};
        static const int8_t amplitude[8]={16,32,48,64,80,96,112,32};
        for (unsigned i=0;i<64;++i) {
            payload[2*i]=(uint8_t)(pattern==3 ? circle[i/8][0] : amplitude[i/8]);
            payload[2*i+1]=(uint8_t)(pattern==3 ? circle[i/8][1] : 0);
        }
    }
    if (pattern == 6) {
        if (raw != 0) return 1;
        static const int8_t samples[128] = {64,0,64,0,64,0,64,0,64,0,64,0,64,0,64,0,-63,11,-63,11,-63,11,-63,11,-63,11,-63,11,-63,11,-63,11,64,0,64,0,64,0,64,0,64,0,64,0,64,0,64,0,-64,6,-64,6,-64,6,-64,6,-64,6,-64,6,-64,6,-64,6,64,0,64,0,64,0,64,0,64,0,64,0,64,0,64,0,-64,1,-64,1,-64,1,-64,1,-64,1,-64,1,-64,1,-64,1,64,0,64,0,64,0,64,0,64,0,64,0,64,0,64,0,-64,0,-64,0,-64,0,-64,0,-64,0,-64,0,-64,0,-64,0};
        memcpy(payload, samples, sizeof(samples));
    }
    if (pattern == 7) {
        if (raw != 0) return 1;
        static const int8_t samples[128] = {64,0,64,0,64,0,64,0,64,0,64,0,64,0,64,0,47,43,6,64,-39,51,-63,11,-63,11,-63,11,-63,11,-63,11,-39,51,6,64,47,43,64,0,64,0,64,0,64,0,64,0,46,44,3,64,-42,48,-64,6,-64,6,-64,6,-64,6,-64,6,-42,48,3,64,46,44,64,0,64,0,64,0,64,0,64,0,45,45,1,64,-45,46,-64,1,-64,1,-64,1,-64,1,-64,1,-45,46,1,64,45,45,64,0,64,0,64,0,64,0,64,0,45,45,0,64,-45,45,-64,0,-64,0,-64,0,-64,0,-64,0};
        memcpy(payload, samples, sizeof(samples));
    }
    if (pattern == 8) {
        if (raw != 0) return 1;
        static const int8_t amplitude[8]={0,8,16,32,64,96,127,0};
        for (unsigned i=0;i<64;++i) { payload[2*i]=(uint8_t)amplitude[i/8]; payload[2*i+1]=0; }
    }
    if (pattern == 9) {
        if (!packet) return 1;
        for (unsigned i=0;i<64;++i) payload[i]=(i/4)%2 ? 0xFF : 0;
    }
    if (pattern == 10) {
        if (raw != 0) return 1;
        static const int8_t samples[128]={8,0,8,0,8,0,8,0,8,0,8,0,8,0,8,0,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,0,24,0,24,0,24,0,24,0,24,0,24,0,24,0,24,-23,23,-23,23,-23,23,-23,23,-23,23,-23,23,-23,23,-23,23,-40,0,-40,0,-40,0,-40,0,-40,0,-40,0,-40,0,-40,0,-34,-34,-34,-34,-34,-34,-34,-34,-34,-34,-34,-34,-34,-34,-34,-34,0,-56,0,-56,0,-56,0,-56,0,-56,0,-56,0,-56,0,-56,11,-11,11,-11,11,-11,11,-11,11,-11,11,-11,11,-11,11,-11};
        memcpy(payload,samples,sizeof(samples));
    }
    if (pattern == 11) {
        if (raw != 0) return 1;
        static const int8_t samples[128]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,0,8,0,8,0,8,0,8,0,8,0,8,0,8,0,16,0,16,0,16,0,16,0,16,0,16,0,16,0,16,0,24,0,24,0,24,0,24,0,24,0,24,0,24,0,24,0,32,0,32,0,32,0,32,0,32,0,32,0,32,0,32,0,40,0,40,0,40,0,40,0,40,0,40,0,40,0,40,0,48,0,48,0,48,0,48,0,48,0,48,0,48,0,48,0,56,0,56,0,56,0,56,0,56,0,56,0,56,0,56,0};
        memcpy(payload,samples,sizeof(samples));
    }
    const char *phase = "preflight";
    uint16_t errors = 0, final_errors = 0;
    esp_err_t stop = ESP_OK, recovery = ESP_OK;
    int64_t elapsed = 0;
    ls_lora_hw_lock();
    const bool rx = lr.rx_mode || lr.rearm;
    esp_err_t err = s_bound && s_info.lr2021 && s_info.fw_major == 1 &&
        s_info.fw_minor == 24 && lr.cfg_valid && !lr.tx_busy && !lr.scanning &&
        !lr.fsk_active && !ms.active && !s_exp_active ? ESP_OK : ESP_ERR_INVALID_STATE;
    uint32_t marker = 0;
    if (err == ESP_OK) err = lr20xx_read_reg32(0x800FF8, &marker);
    if (err == ESP_OK && marker == 0x600DB002) err = ESP_ERR_INVALID_STATE;
    bool touched = err == ESP_OK;
    if (err == ESP_OK) {
        phase = "setup";
        err = step_ok(lr20xx_set_standby(false), "TX probe standby");
    }
    if (err == ESP_OK) err = setup_radio(ook ? LR20XX_PKT_TYPE_OOK : LR20XX_PKT_TYPE_GFSK, 915000000);
    if (err == ESP_OK) err = step_ok((ook ? lr20xx_ook_set_modulation(rate, 0, lr20xx_fsk_rx_bw(100000).index, depth) : lr20xx_gfsk_set_modulation(rate, 0, lr20xx_fsk_rx_bw(100000).index, 10000)), "TX probe modulation");
    if (err == ESP_OK) err = lr20xx_dcdc_workaround_set();
    if (err == ESP_OK) err = step_ok((ook ? lr20xx_ook_set_packet_fixed(32, payload_len, 0) : lr20xx_gfsk_set_packet_fixed(32, 0, payload_len)), "TX probe packet");
    if (err == ESP_OK) err = step_ok((ook ? lr20xx_ook_set_sync_none() : lr20xx_gfsk_set_syncword(0xD391DA26, 32)), "TX probe sync");
    if (err == ESP_OK) err = lr20xx_set_pa_lf(-9);
    if (err == ESP_OK) err = step_ok(lr20xx_set_fallback_standby_rc(), "TX probe fallback");
    for (unsigned i=0; err == ESP_OK && i<REG_COUNT; ++i) err=lr20xx_read_reg32(addr[i], &before[i]);
    /* Isolate the two byte fields written by OOK ROM routine B7E4.
     * Keep ordinary GFSK setup and change only these magnitude fields. */
    if (err == ESP_OK && magnitude)
        err=step_ok(lr20xx_write_reg_mask32(0xF30910,0x00FF00FF,0), "TX probe magnitude route");
    if (err == ESP_OK && raw >= 0) {
        phase = "configure-0293";
        /* Exact seven-byte ROM handler layout; bounded 64-byte input. */
        const uint8_t a[7] = {0,64,(uint8_t)(rate>>24),(uint8_t)(rate>>16),(uint8_t)(rate>>8),(uint8_t)rate,(uint8_t)raw};
        err = step_ok(lr20xx_write(0x0293, a, sizeof(a)), "TX probe 0293");
    }
    for (unsigned i=0; err == ESP_OK && i<REG_COUNT; ++i) err=lr20xx_read_reg32(addr[i], &configured[i]);
    if (err == ESP_OK && !dry) {
        phase = "fifo";
        err = step_ok(lr20xx_clear_tx_fifo(), "TX probe clear");
        if (err == ESP_OK) err = step_ok(lr20xx_clear_irq(LR20XX_IRQ_ALL), "TX probe irq");
        const uint8_t clear[2] = {0x3F,0x3F};
        if (err == ESP_OK) err = step_ok(lr20xx_write(0x0114, clear, 2), "TX probe flags");
        if (err == ESP_OK) err = lr20xx_write_tx_fifo(payload, payload_len);
        if (err == ESP_OK) {
            phase = "transmit";
            lr.tx_busy = true;
            const uint8_t a[3] = {0,0x12,0xC0}; /* 4800 x 31.25 us = 150 ms */
            const uint8_t mode = 2;
            int64_t t = esp_timer_get_time();
            err = step_ok(lr20xx_write(cw ? 0x020E : 0x020D, cw ? &mode : a, cw ? 1 : 3), "TX probe start");
            for (unsigned i=0; err == ESP_OK && i<REG_COUNT; ++i) err=lr20xx_read_reg32(addr[i], &started[i]);
            while (err == ESP_OK && esp_timer_get_time()-t < 150000) {
                lr20xx_status_t status;
                err = lr20xx_get_status(&status);
                if (err == ESP_OK) {
                    irq = status.irq;
                    if (!cw && (irq & LR20XX_IRQ_TX_DONE)) break;
                }
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            if (err == ESP_OK) err=lr20xx_read(0x011B,NULL,0,flags,2);
            if (err == ESP_OK) err=lr20xx_read(0x011D,NULL,0,level,2);
            elapsed = esp_timer_get_time()-t;
            /* Always stop locally, including failed command/status reads. */
            stop = step_ok(lr20xx_set_standby(false), "TX probe stop");
            if (stop != ESP_OK) (void)ls_lora_hw_reset();
            lr.tx_busy = false;
        }
    }
    if (touched) {
        (void)lr20xx_get_errors(&errors);
        s_recovered = false;
        recovery = recover_locked("TX probe complete");
        if (recovery == ESP_OK) {
            lr.rearm = false;
            recovery = program_lora();
            if (recovery == ESP_OK && rx) recovery = lr_receive();
        }
        if (recovery == ESP_OK) recovery = lr20xx_get_errors(&final_errors);
    }
    ls_lora_hw_unlock();
    for (unsigned i=0;i<REG_COUNT;++i) printf("lrtx-reg: %06lX %08lX %08lX %08lX\n",(unsigned long)addr[i],(unsigned long)before[i],(unsigned long)configured[i],(unsigned long)started[i]);
    printf("lrtx: mode=%s pattern=%lu rate=%lu hz=915000000 dbm=-9 elapsed_us=%lld irq=%08lX rxflags=%02X txflags=%02X txlevel=%u errors=%04X final_errors=%04X phase=%s result=%s stop=%s recovery=%s\n",
        argv[2],(unsigned long)pattern,(unsigned long)rate,(long long)elapsed,(unsigned long)irq,flags[0],flags[1],(unsigned)((level[0]<<8)|level[1]),errors,final_errors,phase,esp_err_to_name(err),esp_err_to_name(stop),esp_err_to_name(recovery));
    return err == ESP_OK && stop == ESP_OK && recovery == ESP_OK && !final_errors ? 0 : 1;
}
