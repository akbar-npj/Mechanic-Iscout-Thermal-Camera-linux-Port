/*
 * recorder.cpp — see recorder.h.
 */
#include "recorder.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

struct dyt_recorder {
    std::string     path;
    std::string     codec;
    double          fps = 0.0;
    cv::VideoWriter w;
    bool            opened = false;
    long long       frames = 0;
    int             width = 0, height = 0;
};

dyt_recorder_t *dyt_recorder_open(const char *path, double fps,
                                  const char *codec)
{
    if (!path || !path[0] || !codec || std::strlen(codec) != 4)
        return nullptr;
    if (!(fps > 0.0))
        return nullptr;

    dyt_recorder_t *r = new (std::nothrow) dyt_recorder_t;
    if (!r)
        return nullptr;

    r->path  = path;
    r->codec = codec;
    r->fps   = fps;
    return r;
}

int dyt_recorder_write(dyt_recorder_t *r, const uint8_t *rgb, int w, int h)
{
    if (!r || !rgb || w <= 0 || h <= 0)
        return -1;

    if (!r->opened) {
        r->w.open(r->path,
                  cv::VideoWriter::fourcc(r->codec[0], r->codec[1],
                                          r->codec[2], r->codec[3]),
                  r->fps, cv::Size(w, h), true);
        if (!r->w.isOpened()) {
            std::fprintf(stderr, "recorder: cannot open %s with codec '%s'\n",
                         r->path.c_str(), r->codec.c_str());
            return -1;
        }
        r->opened = true;
        r->width  = w;
        r->height = h;
        std::fprintf(stderr,
                     "recorder: %dx%d @ %.2f fps, codec %s -> %s\n",
                     w, h, r->fps, r->codec.c_str(), r->path.c_str());
    } else if (w != r->width || h != r->height) {
        /* An mp4's dimensions are fixed at open, so a frame of another size
         * cannot be written into this one.  Say so rather than letting OpenCV
         * resize it silently and record a lie. */
        std::fprintf(stderr,
                     "recorder: frame is %dx%d but the clip is %dx%d; skipped\n",
                     w, h, r->width, r->height);
        return -1;
    }

    /* dyt_session_render_rgb() produces RGB; OpenCV wants BGR. */
    const cv::Mat src(h, w, CV_8UC3, const_cast<uint8_t *>(rgb));
    cv::Mat       bgr;
    cv::cvtColor(src, bgr, cv::COLOR_RGB2BGR);
    r->w.write(bgr);
    r->frames++;
    return 0;
}

long long dyt_recorder_frames(const dyt_recorder_t *r)
{
    return r ? r->frames : 0;
}

long long dyt_recorder_close(dyt_recorder_t *r)
{
    long long n;

    if (!r)
        return -1;

    n = r->frames;
    if (r->opened)
        r->w.release();          /* finalises the container */
    delete r;
    return n > 0 ? n : -1;
}
