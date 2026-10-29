// Board checks for the lr2021 SoapySDR module (driver=lr2021), through the public
// SoapySDR API only.
//   rx  <args> <rate> <secs> [gain_at_s] [retune_at_s retune_hz]
//       readStream for secs: overflows, timeouts, other errors, sample count against
//       rate*secs, timestamp steps (expected 1e9/rate ns per sample within a buffer and
//       across buffers when there is no overflow), and what happens at a live gain step
//       or a retune.
//   tx  <args> <hz> <rate> <tone_hz> <samples> [gain_db]
//       one CF32 burst (a complex tone at tone_hz, magnitude 0.9) via writeStream with
//       END_BURST, then readStreamStatus for the burst result.
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;

static int rx(const std::string &args, double rate, double secs, double gain_at, double retune_at, double retune_hz)
{
    SoapySDR::Device *d = SoapySDR::Device::make(args);
    d->setSampleRate(SOAPY_SDR_RX, 0, rate);
    d->setFrequency(SOAPY_SDR_RX, 0, 152.6e6);
    d->setGainMode(SOAPY_SDR_RX, 0, true);
    SoapySDR::Stream *s = d->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16);
    const size_t mtu = d->getStreamMTU(s);
    std::vector<int16_t> buf(2 * mtu);
    void *bufs[] = {buf.data()};
    d->activateStream(s);
    const double step_ns = 1e9 / rate;
    long long n = 0, last_end = -1, ts_bad = 0, overflows = 0, timeouts = 0, other = 0, reads = 0;
    double max_ts_err = 0;
    bool gained = false, retuned = false;
    long long n_at_event = -1;
    std::vector<std::string> events;
    auto t0 = clk::now();
    for (;;) {
        double el = std::chrono::duration<double>(clk::now() - t0).count();
        if (el > secs) break;
        if (gain_at >= 0 && !gained && el > gain_at) {
            d->setGainMode(SOAPY_SDR_RX, 0, false);
            d->setGain(SOAPY_SDR_RX, 0, "STEP", 9);
            gained = true; n_at_event = n;
            events.push_back("gain STEP 9 at " + std::to_string(el) + " s");
        }
        if (retune_at >= 0 && !retuned && el > retune_at) {
            d->setFrequency(SOAPY_SDR_RX, 0, retune_hz);
            retuned = true;
            events.push_back("retune to " + std::to_string(retune_hz / 1e6) + " MHz at " + std::to_string(el) + " s");
        }
        int flags = 0; long long tns = 0;
        int r = d->readStream(s, bufs, mtu, flags, tns, 200000);
        reads++;
        if (r == SOAPY_SDR_OVERFLOW) {
            overflows++; last_end = -1;
            events.push_back("OVERFLOW at " + std::to_string(el) + " s");
            continue;
        }
        if (r == SOAPY_SDR_TIMEOUT) { timeouts++; continue; }
        if (r < 0) { other++; events.push_back(std::string("error ") + SoapySDR::errToStr(r)); continue; }
        if (!(flags & SOAPY_SDR_HAS_TIME)) ts_bad++;
        else if (last_end >= 0) {
            double err = std::fabs((double)(tns - last_end));
            if (err > max_ts_err) max_ts_err = err;
            if (err > step_ns) ts_bad++;
        }
        last_end = tns + (long long)llround(r * step_ns);
        n += r;
    }
    double el = std::chrono::duration<double>(clk::now() - t0).count();
    d->deactivateStream(s);
    d->closeStream(s);
    SoapySDR::Device::unmake(d);
    std::printf("rx rate=%.0f secs=%.1f mtu=%zu samples=%lld (%.2f%% of rate*elapsed) reads=%lld overflows=%lld timeouts=%lld "
                "errors=%lld timestamp_steps_off=%lld max_step_error=%.0f ns (one sample %.0f ns)\n",
                rate, el, mtu, n, 100.0 * n / (rate * el), reads, overflows, timeouts, other, ts_bad, max_ts_err, step_ns);
    for (auto &e : events) std::printf("  event: %s\n", e.c_str());
    (void)n_at_event;
    return overflows || other ? 2 : 0;
}

static int tx(const std::string &args, double hz, double rate, double tone, size_t samples, double gain)
{
    SoapySDR::Device *d = SoapySDR::Device::make(args);
    d->setSampleRate(SOAPY_SDR_TX, 0, rate);
    d->setFrequency(SOAPY_SDR_TX, 0, hz);
    if (!std::isnan(gain)) d->setGain(SOAPY_SDR_TX, 0, gain);
    std::printf("tx: %.4f MHz rate=%.0f gain=%.1f dB tone=%+.0f Hz samples=%zu (%.1f ms)\n", hz / 1e6, rate,
                d->getGain(SOAPY_SDR_TX, 0), tone, samples, 1e3 * samples / rate);
    SoapySDR::Stream *s = d->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32);
    std::vector<std::complex<float>> b(samples);
    for (size_t i = 0; i < samples; i++) b[i] = std::polar(0.9f, (float)(2 * M_PI * tone * i / rate));
    d->activateStream(s);
    size_t off = 0;
    auto t0 = clk::now();
    while (off < samples) {
        const void *p[] = {b.data() + off};
        int flags = off + d->getStreamMTU(s) >= samples ? SOAPY_SDR_END_BURST : 0;
        size_t n = std::min(d->getStreamMTU(s), samples - off);
        int r = d->writeStream(s, p, n, flags, 0, 2000000);
        if (r < 0) { std::printf("writeStream: %s\n", SoapySDR::errToStr(r)); break; }
        off += (size_t)r;
    }
    size_t ch = 0; int flags = 0; long long tns = 0;
    int st = d->readStreamStatus(s, ch, flags, tns, 5000000);
    double el = std::chrono::duration<double>(clk::now() - t0).count();
    std::printf("tx: wrote %zu samples, readStreamStatus=%s flags=0x%x%s after %.2f s\n", off, SoapySDR::errToStr(st),
                flags, flags & SOAPY_SDR_END_BURST ? " END_BURST" : "", el);
    d->deactivateStream(s);
    d->closeStream(s);
    SoapySDR::Device::unmake(d);
    return st ? 2 : 0;
}

// timed <args> <hz> <rate> <tone_hz> <samples> <delay_s>: one burst with HAS_TIME at
// getHardwareTime() + delay; prints the requested time and when writeStream returned.
static int timed(const std::string &args, double hz, double rate, double tone, size_t samples, double delay)
{
    SoapySDR::Device *d = SoapySDR::Device::make(args);
    d->setSampleRate(SOAPY_SDR_TX, 0, rate);
    d->setFrequency(SOAPY_SDR_TX, 0, hz);
    SoapySDR::Stream *s = d->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32);
    std::vector<std::complex<float>> b(samples);
    for (size_t i = 0; i < samples; i++) b[i] = std::polar(0.9f, (float)(2 * M_PI * tone * i / rate));
    d->activateStream(s);
    long long now = d->getHardwareTime(), at = now + (long long)(delay * 1e9);
    auto t0 = clk::now();
    size_t off = 0;
    while (off < samples) {
        const void *p[] = {b.data() + off};
        size_t n = std::min(d->getStreamMTU(s), samples - off);
        int flags = (off == 0 ? SOAPY_SDR_HAS_TIME : 0) | (off + n >= samples ? SOAPY_SDR_END_BURST : 0);
        int r = d->writeStream(s, p, n, flags, at, 20000000);
        if (r < 0) { std::printf("writeStream: %s\n", SoapySDR::errToStr(r)); break; }
        off += (size_t)r;
    }
    double wrote = std::chrono::duration<double>(clk::now() - t0).count();
    size_t ch = 0; int flags = 0; long long tns = 0;
    int st = d->readStreamStatus(s, ch, flags, tns, 20000000);
    double done = std::chrono::duration<double>(clk::now() - t0).count();
    std::printf("timed: device time %lld ns, burst requested at +%.3f s; writeStream done +%.3f s; status %d (%s) flags=0x%x at +%.3f s, status time %lld ns (%+.3f ms from request)\n",
                now, delay, wrote, st, st ? SoapySDR::errToStr(st) : "OK", flags, done, tns,
                (flags & SOAPY_SDR_HAS_TIME) ? (tns - at) / 1e6 : 0.0);
    d->deactivateStream(s); d->closeStream(s); SoapySDR::Device::unmake(d);
    return st ? 2 : 0;
}

// rxtx <args> <rate> <secs> <tx_at_s> <tx_hz>: RX at 152.6 MHz, one TX burst on the
// same device mid-stream, then RX again: overflows and samples after the burst.
static int rxtx(const std::string &args, double rate, double secs, double tx_at, double tx_hz)
{
    SoapySDR::Device *d = SoapySDR::Device::make(args);
    d->setSampleRate(SOAPY_SDR_RX, 0, rate); d->setFrequency(SOAPY_SDR_RX, 0, 152.6e6);
    d->setSampleRate(SOAPY_SDR_TX, 0, 125000); d->setFrequency(SOAPY_SDR_TX, 0, tx_hz);
    SoapySDR::Stream *rx = d->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16);
    SoapySDR::Stream *tx = d->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32);
    const size_t mtu = d->getStreamMTU(rx);
    std::vector<int16_t> buf(2 * mtu); void *bufs[] = {buf.data()};
    std::vector<std::complex<float>> burst(16000);
    for (size_t i = 0; i < burst.size(); i++) burst[i] = std::polar(0.9f, (float)(2 * M_PI * 10000.0 * i / 125000));
    d->activateStream(rx); d->activateStream(tx);
    long long before = 0, after = 0, overflows = 0, errors = 0; bool sent = false; double resume = -1;
    std::vector<std::string> ev;
    auto t0 = clk::now();
    for (;;) {
        double el = std::chrono::duration<double>(clk::now() - t0).count();
        if (el > secs) break;
        if (!sent && el > tx_at) {
            size_t off = 0;
            while (off < burst.size()) {
                const void *p[] = {burst.data() + off};
                size_t n = std::min(d->getStreamMTU(tx), burst.size() - off);
                int wf = off + n >= burst.size() ? SOAPY_SDR_END_BURST : 0;
                int r = d->writeStream(tx, p, n, wf, 0, 5000000);
                if (r < 0) { ev.push_back(std::string("TX writeStream ") + SoapySDR::errToStr(r)); break; }
                off += (size_t)r;
            }
            size_t ch = 0; int fl = 0; long long tn = 0;
            int st = d->readStreamStatus(tx, ch, fl, tn, 5000000);
            ev.push_back("TX burst at " + std::to_string(el) + " s, status " + std::to_string(st) +
                         " flags " + std::to_string(fl));
            sent = true;
        }
        int flags = 0; long long tns = 0;
        int r = d->readStream(rx, bufs, mtu, flags, tns, 200000);
        if (r == SOAPY_SDR_OVERFLOW) { overflows++; ev.push_back("RX OVERFLOW at " + std::to_string(el) + " s"); continue; }
        if (r == SOAPY_SDR_TIMEOUT) continue;
        if (r < 0) { errors++; ev.push_back(std::string("RX ") + SoapySDR::errToStr(r)); continue; }
        if (sent) { if (resume < 0) resume = el; after += r; } else before += r;
    }
    d->deactivateStream(rx); d->deactivateStream(tx); d->closeStream(rx); d->closeStream(tx);
    SoapySDR::Device::unmake(d);
    std::printf("rxtx: RX samples before burst %lld, after %lld (first data after it at %.2f s), overflows %lld, errors %lld\n",
                before, after, resume, overflows, errors);
    for (auto &e : ev) std::printf("  event: %s\n", e.c_str());
    return errors ? 2 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 8 && std::string(argv[1]) == "timed")
        return timed(argv[2], atof(argv[3]), atof(argv[4]), atof(argv[5]), (size_t)atol(argv[6]), atof(argv[7]));
    if (argc >= 7 && std::string(argv[1]) == "rxtx")
        return rxtx(argv[2], atof(argv[3]), atof(argv[4]), atof(argv[5]), atof(argv[6]));
    if (argc >= 5 && std::string(argv[1]) == "rx")
        return rx(argv[2], atof(argv[3]), atof(argv[4]), argc > 5 ? atof(argv[5]) : -1,
                  argc > 7 ? atof(argv[6]) : -1, argc > 7 ? atof(argv[7]) : 0);
    if (argc >= 7 && std::string(argv[1]) == "tx")
        return tx(argv[2], atof(argv[3]), atof(argv[4]), atof(argv[5]), (size_t)atol(argv[6]),
                  argc > 7 ? atof(argv[7]) : NAN);
    std::fprintf(stderr, "usage: rx <args> <rate> <secs> [gain_at_s] [retune_at_s retune_hz] | tx <args> <hz> <rate> <tone_hz> <samples> [gain_db]\n");
    return 1;
}
