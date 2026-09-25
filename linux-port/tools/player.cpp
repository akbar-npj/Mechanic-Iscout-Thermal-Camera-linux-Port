/*
 * player.cpp — see player.h.
 */
#include "player.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

struct dyt_player {
    std::string   path;
    cv::VideoCapture cap;
    int           width = 0, height = 0;
    double        fps = 0.0;
    long long     count = 0;    /* 0 when the container does not say */
    long long     pos = 0;      /* index of the next frame to deliver */
    bool          delivered = false;
};

dyt_player_t *dyt_player_open(const char *path)
{
    if (!path || !path[0])
        return nullptr;

    dyt_player_t *p = new (std::nothrow) dyt_player_t;
    if (!p)
        return nullptr;

    p->path = path;
    if (!p->cap.open(path)) {
        std::fprintf(stderr, "player: cannot open %s\n", path);
        delete p;
        return nullptr;
    }

    p->width  = (int)p->cap.get(cv::CAP_PROP_FRAME_WIDTH);
    p->height = (int)p->cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    if (p->width <= 0 || p->height <= 0) {
        std::fprintf(stderr, "player: %s carries no video stream\n", path);
        p->cap.release();
        delete p;
        return nullptr;
    }

    p->fps = p->cap.get(cv::CAP_PROP_FPS);
    if (!(p->fps > 0.0))
        p->fps = 0.0;           /* some containers report NaN or a negative */

    /* A count of 0 or -1 means "unknown" here, not "empty" — the stream is
     * open and readable either way, so keep 0 as the sentinel the header
     * documents rather than propagating a negative. */
    p->count = (long long)p->cap.get(cv::CAP_PROP_FRAME_COUNT);
    if (p->count < 0)
        p->count = 0;

    std::fprintf(stderr, "player: %dx%d @ %.2f fps, %lld frames <- %s\n",
                 p->width, p->height, p->fps, p->count, path);
    return p;
}

int dyt_player_size(const dyt_player_t *p, int *w, int *h)
{
    if (!p || !w || !h)
        return -1;
    *w = p->width;
    *h = p->height;
    return 0;
}

double dyt_player_fps(const dyt_player_t *p)
{
    return p ? p->fps : 0.0;
}

long long dyt_player_count(const dyt_player_t *p)
{
    return p ? p->count : 0;
}

int dyt_player_next(dyt_player_t *p, uint8_t *rgb, int cap, long long *pos)
{
    cv::Mat bgr;

    if (!p || !rgb || cap <= 0)
        return -1;

    if (!p->cap.read(bgr)) {
        /* End of stream — or a decode failure.  They are the same to the
         * caller, and the useful thing to do with a short clip is to keep
         * playing it, so rewind and try once more.  Only a second failure
         * (an empty or undecodable stream) is reported as an error. */
        if (!p->delivered || !p->cap.set(cv::CAP_PROP_POS_FRAMES, 0) ||
            !p->cap.read(bgr)) {
            std::fprintf(stderr, "player: cannot decode a frame from %s\n",
                         p->path.c_str());
            return -1;
        }
        p->pos = 0;
    }

    const int need = bgr.cols * bgr.rows * 3;
    if (bgr.empty() || need > cap) {
        std::fprintf(stderr, "player: frame is %dx%d but the buffer holds %d\n",
                     bgr.cols, bgr.rows, cap);
        return -1;
    }

    /* OpenCV decodes to BGR; the port speaks RGB.  The writer's
     * COLOR_RGB2BGR is the other half of this pair. */
    const cv::Mat dst(bgr.rows, bgr.cols, CV_8UC3, rgb);
    cv::cvtColor(bgr, dst, cv::COLOR_BGR2RGB);

    if (pos)
        *pos = p->pos;
    p->pos++;
    p->delivered = true;
    return 0;
}

void dyt_player_close(dyt_player_t *p)
{
    if (!p)
        return;
    p->cap.release();
    delete p;
}
