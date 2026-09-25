/*
 * mnn_runtime.cpp — the MNN runtime behind the super-resolution seam.
 *
 * mnn.c is plain C and stays runtime-free; everything that needs MNN's C++ API
 * lives here, behind four C-callable entry points.  The build only compiles
 * this file when an MNN runtime is present (HAVE_MNN), so a host without one
 * gets the refusing seam described in mnn.h.
 *
 * The conversion is not guessed.  It is taken from the vendor's own per-frame
 * code (libmnnmodel.so, `sr2` at 0x66fc), which was read instruction by
 * instruction and then confirmed empirically against the same function running
 * on this host:
 *
 *     input   ldrb  w8, [in + i*2]        # the LOW byte of each uint16 sample
 *             dst[i] = (float)(w8 / 255.0)
 *
 *     output  s0 = out_f[i]
 *             s0 = s0 + 32768.0f          # fadd 0x47000000
 *             out[i] = (uint16_t)trunc(s0) # fcvtzu
 *
 * The divisor 255.0 is the double at .rodata 0x2d80; the bias is 32768.0.
 * Feeding the vendor's `mnn_run_2` constant planes and reading the uint16 back
 * shows out - 32768 == in to within a rounding step, i.e. the network has unit
 * DC gain and the bias is a pure offset.
 *
 * THE LAYOUT IS THE TRAP.  The session's input tensor is NC4HW4 even though it
 * reports getDimensionType() == CAFFE: for a 1-channel 192x256 tensor it
 * reports elementSize() == 196608 and dim[0].stride == 196608, i.e. four times
 * the 49152 that the shape implies, because the channel dim is padded to 4.
 * Writing `host<float>()[i] = v` for i in 0..49151 therefore does NOT fill
 * pixels 0..49151 -- it fills pixels 0..12287 across all four channels, and the
 * output is garbage.  Measured against the vendor on the same input that naive
 * write gets 188657 of 196608 samples wrong with a peak error of 255, while the
 * correct path below matches the vendor to within one LSB.
 *
 * A constant test plane cannot see this, which is why the check is done with a
 * non-constant plane (tools/mnn_diff/, and the differential in mnn_test).
 *
 * The fix is to hand MNN an explicit plain-NCHW host tensor and let it do the
 * conversion:
 *
 *     g_input->copyFromHostTensor(host_in)     # CAFFE -> NC4HW4
 *     g_output->copyToHostTensor(host_out)     # NC4HW4 -> CAFFE
 *
 * build: see the HAVE_MNN rule in the Makefile
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <MNN/Interpreter.hpp>
#include <MNN/Tensor.hpp>

extern "C" {
#include "mnn.h"
}

namespace {

MNN::Interpreter *g_net = nullptr;
MNN::Session     *g_session = nullptr;
MNN::Tensor      *g_input = nullptr;
MNN::Tensor      *g_output = nullptr;

/* Plain-NCHW host tensors, allocated once at load.  These are what the caller's
 * bytes are staged into; MNN converts between them and the session's NC4HW4
 * tensors.  See the layout note at the top of this file. */
std::shared_ptr<MNN::Tensor> g_host_in;
std::shared_ptr<MNN::Tensor> g_host_out;

const dyt_mnn_contract_t *C(void)
{
    return dyt_mnn_contract();
}

/* The model is small and fixed; reading it once at load keeps the buffer alive
 * for as long as the interpreter needs it. */
std::vector<uint8_t> g_model;

}  // namespace

extern "C" void dyt_mnn_runtime_unload(void);

extern "C" int dyt_mnn_runtime_available(void)
{
    return g_session != nullptr ? 1 : 0;
}

extern "C" int dyt_mnn_runtime_load(const char *path)
{
    if (g_session)
        return 0;  // already loaded

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "mnn: cannot open %s\n", path);
        return DYT_MNN_UNAVAILABLE;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        fprintf(stderr, "mnn: %s is empty\n", path);
        return DYT_MNN_UNAVAILABLE;
    }
    g_model.resize((size_t)n);
    if (fread(g_model.data(), 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        g_model.clear();
        fprintf(stderr, "mnn: short read on %s\n", path);
        return DYT_MNN_UNAVAILABLE;
    }
    fclose(f);

    /* Check the file before handing it to MNN, so a wrong or truncated model
     * fails with a useful message rather than deep inside the runtime. */
    if (dyt_mnn_model_check(g_model.data(), g_model.size()) != 0) {
        g_model.clear();
        return DYT_MNN_UNAVAILABLE;
    }

    g_net = MNN::Interpreter::createFromBuffer(g_model.data(), g_model.size());
    if (!g_net) {
        fprintf(stderr, "mnn: createFromBuffer failed\n");
        g_model.clear();
        return DYT_MNN_UNAVAILABLE;
    }

    MNN::ScheduleConfig cfg;
    cfg.type = MNN_FORWARD_CPU;
    cfg.numThread = 4;

    MNN::BackendConfig backend;
    backend.precision = MNN::BackendConfig::Precision_High;
    cfg.backendConfig = &backend;

    g_session = g_net->createSession(cfg);
    if (!g_session) {
        fprintf(stderr, "mnn: createSession failed\n");
        MNN::Interpreter::destroy(g_net);
        g_net = nullptr;
        g_model.clear();
        return DYT_MNN_UNAVAILABLE;
    }

    /* The input geometry is not baked into the flatbuffer -- the vendor sets it
     * with resizeTensor([1,1,192,256]) at load time, and so do we. */
    MNN::Tensor *in = g_net->getSessionInput(g_session, "image_input");
    g_net->resizeTensor(in, {1, 1, C()->in_h, C()->in_w});
    g_net->resizeSession(g_session);
    g_input  = g_net->getSessionInput(g_session, "image_input");
    g_output = g_net->getSessionOutput(g_session, "image_output");

    /* Check the logical shape, not elementSize(): the session tensor is NC4HW4,
     * so elementSize() is 4x the sample count.  See the layout note above. */
    if (!g_input || !g_output ||
        g_input->batch() != 1 || g_input->channel() != 1 ||
        g_input->height() != C()->in_h || g_input->width() != C()->in_w) {
        fprintf(stderr, "mnn: input tensor is not %dx%d\n", C()->in_w, C()->in_h);
        dyt_mnn_runtime_unload();
        return DYT_MNN_UNAVAILABLE;
    }
    if (g_output->batch() != 1 || g_output->channel() != 1 ||
        g_output->height() != C()->out_h || g_output->width() != C()->out_w) {
        fprintf(stderr, "mnn: output tensor is not %dx%d\n", C()->out_w,
                C()->out_h);
        dyt_mnn_runtime_unload();
        return DYT_MNN_UNAVAILABLE;
    }

    g_host_in.reset(MNN::Tensor::create<float>(
        {1, 1, C()->in_h, C()->in_w}, nullptr, MNN::Tensor::CAFFE));
    g_host_out.reset(MNN::Tensor::create<float>(
        {1, 1, C()->out_h, C()->out_w}, nullptr, MNN::Tensor::CAFFE));
    if (!g_host_in || !g_host_out) {
        fprintf(stderr, "mnn: cannot allocate host tensors\n");
        dyt_mnn_runtime_unload();
        return DYT_MNN_UNAVAILABLE;
    }
    return 0;
}

extern "C" void dyt_mnn_runtime_unload(void)
{
    if (g_net) {
        if (g_session)
            g_net->releaseSession(g_session);
        MNN::Interpreter::destroy(g_net);
    }
    g_net = nullptr;
    g_session = nullptr;
    g_input = nullptr;
    g_output = nullptr;
    g_host_in.reset();
    g_host_out.reset();
    g_model.clear();
}

extern "C" int dyt_mnn_runtime_zoom2(const uint8_t *in, uint8_t *out,
                                     int out_cap, int *out_n)
{
    const dyt_mnn_contract_t *c = C();

    if (!g_session || !g_input || !g_output || !g_host_in || !g_host_out)
        return DYT_MNN_UNAVAILABLE;
    if (!in || !out || !out_n || out_cap < c->out_bytes)
        return -1;

    /* Input: the low byte of each uint16 sample, normalised by 255.  This goes
     * into the plain-NCHW host tensor; copyFromHostTensor does the NC4HW4
     * conversion, which a direct write into g_input->host<float>() would get
     * wrong.  See the layout note at the top of this file. */
    float *src = g_host_in->host<float>();
    for (int i = 0; i < c->in_samples; i++)
        src[i] = (float)(in[i * 2] / 255.0);
    g_input->copyFromHostTensor(g_host_in.get());

    if (g_net->runSession(g_session) != MNN::NO_ERROR) {
        fprintf(stderr, "mnn: runSession failed\n");
        return -1;
    }

    g_output->copyToHostTensor(g_host_out.get());
    const float *dst = g_host_out->host<float>();

    /* Output: the vendor's fadd 32768.0 then truncate-to-unsigned. */
    uint16_t *o = reinterpret_cast<uint16_t *>(out);
    for (int i = 0; i < c->out_samples; i++)
        o[i] = (uint16_t)(dst[i] + 32768.0f);

    *out_n = c->out_bytes;
    return 0;
}
