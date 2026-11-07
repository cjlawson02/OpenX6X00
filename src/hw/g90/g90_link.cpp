/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - g90_link.h on libg90: the C state to a HeadEncodeRequest, the DecodedBody back.
 *
 *  Copyright (c) 2026 Chris Lawson
 */

#include "g90_link.h"

#include <g90/g90.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <new>
#include <string_view>

struct g90_link {
    std::unique_ptr<g90::De9Link> link;
    g90::Profile                  profile;
};

namespace {

void set_err(char *err, size_t len, const char *what, g90::Error e) {
    if (err && len) {
        const auto s = g90::to_string(e);
        std::snprintf(err, len, "%s: %.*s", what, static_cast<int>(s.size()), s.data());
    }
}

/* radio_mode_t 0..7 is LSB, LSB-D, USB, USB-D, CW, CW-R, AM, NFM: g90::Mode's order. SAM and WFM
 * (8, 9) go as AM and NFM, as a base without them takes them */
g90::Mode to_mode(uint8_t m) {
    switch (m) {
        case 8:  return g90::Mode::Am;
        case 9:  return g90::Mode::Nfm;
        default: return m <= 7 ? static_cast<g90::Mode>(m) : g90::Mode::Usb;
    }
}

g90::HeadVfo to_vfo(const g90_vfo_ctl_t &v) {
    g90::HeadVfo out;
    out.freq_hz        = v.freq_hz;
    out.mode           = to_mode(v.mode);
    out.filter_high_hz = v.filter_high_hz;
    out.filter_low_hz  = v.filter_low_hz;
    out.agc            = v.agc;
    return out;
}

g90::HeadEncodeRequest to_request(const g90_ctl_t &c, g90::Profile profile) {
    g90::HeadEncodeRequest req;
    req.profile = profile;
    req.volume  = std::min<uint8_t>(c.volume, 28);
    req.vfo_a   = to_vfo(c.vfo[0]);
    req.vfo_b   = to_vfo(c.vfo[1]);

    g90::HeadControls ctl;
    ctl.vfo_b_en        = c.vfo_b;
    ctl.rf_power        = std::clamp<uint8_t>(c.rf_power_w, 1, 20);
    ctl.nb_en           = c.nb;
    ctl.nb_level        = c.nb_level & 0x0F;
    ctl.nb_width        = c.nb_width & 0x0F;
    ctl.sql_level       = std::min<uint8_t>(c.sql_level, 9);
    ctl.split_en        = c.split;
    ctl.headphones      = c.headphones;
    ctl.tuner_en        = c.tuner;
    ctl.mic_compression = c.mic_comp;
    ctl.rit_hz          = std::clamp<int>(c.rit_hz, -512, 511);
    if (c.shutdown) {
        ctl.shutdown_req = true;
    }
    req.controls = ctl;

    if (c.allow_tx) {
        g90::LabExtras lab;
        lab.allow_tx     = true;
        lab.arm_ptt      = c.ptt;
        lab.arm_atu_tune = c.atu_tune;
        req.lab          = lab;
    }
    return req;
}

} // namespace

extern "C" g90_link_t *g90_link_open(const char *path, const char *profile, char *err, size_t err_len) {
    auto prof = g90::parse_profile(profile ? std::string_view(profile) : std::string_view("1.81"));
    if (!prof.ok()) {
        set_err(err, err_len, "profile", prof.err());
        return nullptr;
    }
    auto port   = std::make_unique<g90::de9::SerialPort>();
    auto opened = port->open(path);
    if (!opened.ok()) {
        set_err(err, err_len, path, opened.err());
        return nullptr;
    }
    auto *l = new (std::nothrow) g90_link;
    if (!l) {
        return nullptr;
    }
    l->profile = *prof.value;
    l->link    = std::make_unique<g90::De9Link>(std::move(port), l->profile);
    return l;
}

extern "C" void g90_link_close(g90_link_t *link) {
    delete link;
}

extern "C" bool g90_link_exchange(g90_link_t *link, const g90_ctl_t *ctl, g90_body_t *body, int timeout_ms,
                                  char *err, size_t err_len) {
    auto r = link->link->exchange(to_request(*ctl, link->profile), std::chrono::milliseconds{timeout_ms});
    if (!r.ok()) {
        set_err(err, err_len, "exchange", r.err());
        return false;
    }
    const g90::DecodedBody &b = *r.value;
    body->crc_ok      = b.crc_ok;
    body->s_meter     = static_cast<int16_t>(b.status.s_meter);
    body->po          = b.status.po;
    body->swr         = b.status.swr;
    body->alc         = b.status.alc;
    body->voltage_raw = b.status.voltage_raw;
    body->atu         = b.status.atu;
    body->ver_major   = b.status.info7;
    body->ver_minor   = b.status.info6;
    std::copy(b.fft.begin(), b.fft.end(), body->fft);
    return true;
}
