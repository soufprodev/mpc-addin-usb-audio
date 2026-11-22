// UsbOut.cpp - "USB Out": sends the audio passing through this insert slot to its own channel of the
// MPC USB Audio device (the mpc-addin-usb-audio addin), for multitrack recording on a computer.
//
// SPDX-License-Identifier: MIT
//
// Put it last in a track's (or pad's, program's, submix's) insert chain. The audio passes through
// unchanged unless Through is Off. USB channels 1-2 are MPC's main mix; this plugin uses 3 and up.
//
// The addin lives in the same MPC process (LD_PRELOAD). It exports mpcua_send_v1; a weak reference
// finds it at load time, and is null when the addin isn't installed: the plugin then just passes
// audio through and says "NoAddin" on screen.

#include "mpc_plugin.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int mpcua_send_v1(unsigned first, unsigned nch, const float* const* bufs, unsigned frames,
                             uint32_t* cursor) __attribute__((weak));

namespace {

enum ParamIndex { kChannel = 0, kMode, kThrough, kNumParams };

constexpr int kFirstUsbChannel = 3;    // USB 1-2 = main mix
constexpr int kLastUsbChannel = 27;    // the kernel's UAC2 limit per direction

class UsbOut : public mpc::Plugin
{
public:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "USB Out";
        i.vendor = "MPC USB Audio";
        i.uniqueId = MPC_FOURCC('U', 's', 'b', 'O');   // == "uid" in plugin.json
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.tailSamples = 1;                              // no tail
        return i;
    }

    UsbOut() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(intParam("ch", "USB Ch", kFirstUsbChannel, kLastUsbChannel, kFirstUsbChannel));
        addParam(choiceParam("mode", "Mode", {"Stereo", "Mono"}, 0));
        addParam(boolParam("thru", "Through", true));
        if (numParams() != kNumParams) mpc::log("UsbOut: parameter count mismatch");
    }

    void prepare(double, int maxBlockSize) override
    {
        mono_.assign((size_t)(maxBlockSize > 0 ? maxBlockSize : 512), 0.0f);
    }

    void reset() override { cursor_ = 0; }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        if (n <= 0 || ctx.numInputs < 1 || ctx.inputs == nullptr) return;
        const float* l = ctx.inputs[0];
        const float* r = ctx.numInputs > 1 ? ctx.inputs[1] : ctx.inputs[0];

        if (mpcua_send_v1 != nullptr)
        {
            // USB channel N is send channel N-2 (USB 1-2 are the main mix).
            const unsigned send = (unsigned)(getInt(kChannel) - 2);
            int rc;
            if (getInt(kMode) == 1 && (size_t)n <= mono_.size())
            {
                for (int i = 0; i < n; ++i) mono_[(size_t)i] = 0.5f * (l[i] + r[i]);
                const float* one[1] = { mono_.data() };
                rc = mpcua_send_v1(send, 1, one, (unsigned)n, &cursor_);
            }
            else
            {
                const float* two[2] = { l, r };
                rc = mpcua_send_v1(send, 2, two, (unsigned)n, &cursor_);
            }
            linked_.store(rc == 0 ? 1 : 2, std::memory_order_relaxed);
        }

        // On the MPC inputs and outputs are the same buffers, so leaving them alone is "through";
        // copy only where a host hands out separate ones.
        const bool thru = getBool(kThrough);
        for (int c = 0; c < ctx.numOutputs; ++c)
        {
            float* out = ctx.outputs[c];
            const float* in = c < ctx.numInputs ? ctx.inputs[c] : nullptr;
            if (!thru || in == nullptr) { for (int i = 0; i < n; ++i) out[i] = 0.0f; }
            else if (out != in) { for (int i = 0; i < n; ++i) out[i] = in[i]; }
        }
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index != kChannel) return false;
        if (mpcua_send_v1 == nullptr) { out = "NoAddin"; return true; }
        const int ch = mpc::clamp((int)plain, kFirstUsbChannel, kLastUsbChannel);
        char buf[32];
        if (getInt(kMode) == 0 && ch < kLastUsbChannel) std::snprintf(buf, sizeof buf, "%d-%d", ch, ch + 1);
        else std::snprintf(buf, sizeof buf, "%d", ch);
        out = buf;
        if (linked_.load(std::memory_order_relaxed) == 2) out += " !";   // the addin isn't carrying it
        return true;
    }

private:
    std::vector<float> mono_;
    uint32_t cursor_ = 0;
    std::atomic<int> linked_{0};
};

} // namespace

MPC_PLUGIN_ENTRY(UsbOut)
