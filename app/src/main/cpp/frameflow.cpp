#include <jni.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "FrameFlow", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "FrameFlow", __VA_ARGS__)

namespace {
std::mutex gMutex;
ANativeWindow* gWindow = nullptr;
int gW = 0, gH = 0;
std::vector<uint8_t> gPrev;
std::vector<uint8_t> gCurr;
std::vector<uint8_t> gOut;
bool gHavePrev = false;

inline int clampi(int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); }

inline float lum(const uint8_t* p) {
    return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
}

float blockError(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                 int w, int h, int cx, int cy, int dx, int dy) {
    float e = 0.0f;
    for (int oy = 0; oy <= 1; ++oy) {
        const int ay = clampi(cy + oy, 0, h - 1);
        const int by = clampi(cy + dy + oy, 0, h - 1);
        for (int ox = 0; ox <= 1; ++ox) {
            const int ax = clampi(cx + ox, 0, w - 1);
            const int bx = clampi(cx + dx + ox, 0, w - 1);
            const float d = lum(&a[(ay * w + ax) * 4]) - lum(&b[(by * w + bx) * 4]);
            e += d * d;
        }
    }
    return e;
}

void interpolate(const uint8_t* cur, int w, int h) {
    const size_t bytes = static_cast<size_t>(w) * h * 4;
    if (!gHavePrev) {
        gPrev.assign(cur, cur + bytes);
        gOut.assign(cur, cur + bytes);
        gHavePrev = true;
        return;
    }
    gCurr.assign(cur, cur + bytes);
    gOut.resize(bytes);

    // One motion estimate per 8x8 block. The same vector is reused by its pixels.
    constexpr int BS = 8;
    constexpr int R = 3;
    for (int by = 0; by < h; by += BS) {
        for (int bx = 0; bx < w; bx += BS) {
            const int cx = std::min(bx + BS / 2, w - 1);
            const int cy = std::min(by + BS / 2, h - 1);
            float best = 1e30f;
            int bestDx = 0, bestDy = 0;
            float zero = blockError(gPrev, gCurr, w, h, cx, cy, 0, 0);
            for (int dy = -R; dy <= R; ++dy) {
                for (int dx = -R; dx <= R; ++dx) {
                    const float e = blockError(gPrev, gCurr, w, h, cx, cy, dx, dy);
                    if (e < best) { best = e; bestDx = dx; bestDy = dy; }
                }
            }
            const float confidence = 1.0f - std::clamp(best / std::max(zero, 1.0f), 0.0f, 1.0f);
            const float trust = std::clamp((confidence - 0.05f) / 0.30f, 0.0f, 1.0f);

            for (int y = by; y < std::min(by + BS, h); ++y) {
                for (int x = bx; x < std::min(bx + BS, w); ++x) {
                    const int px = clampi(static_cast<int>(x + bestDx * 0.5f + 0.5f), 0, w - 1);
                    const int py = clampi(static_cast<int>(y + bestDy * 0.5f + 0.5f), 0, h - 1);
                    const int qx = clampi(static_cast<int>(x - bestDx * 0.5f + 0.5f), 0, w - 1);
                    const int qy = clampi(static_cast<int>(y - bestDy * 0.5f + 0.5f), 0, h - 1);
                    const uint8_t* a = &gPrev[(py * w + px) * 4];
                    const uint8_t* b = &gCurr[(qy * w + qx) * 4];
                    uint8_t* o = &gOut[(y * w + x) * 4];
                    const uint8_t* fallbackA = &gPrev[(y * w + x) * 4];
                    const uint8_t* fallbackB = &gCurr[(y * w + x) * 4];
                    for (int c = 0; c < 4; ++c) {
                        const float motionValue = 0.5f * (a[c] + b[c]);
                        const float fallback = 0.5f * (fallbackA[c] + fallbackB[c]);
                        o[c] = static_cast<uint8_t>(std::clamp(motionValue * trust + fallback * (1.0f - trust), 0.0f, 255.0f));
                    }
                }
            }
        }
    }
    gPrev.swap(gCurr);
}

bool readHardwareBuffer(JNIEnv* env, jobject hbObj) {
    AHardwareBuffer* hb = AHardwareBuffer_fromHardwareBuffer(env, hbObj);
    if (!hb) return false;
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(hb, &d);
    if (d.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM || d.width != static_cast<uint32_t>(gW) || d.height != static_cast<uint32_t>(gH)) {
        LOGE("Unexpected buffer: %ux%u format=%u", d.width, d.height, d.format);
        return false;
    }
    void* ptr = nullptr;
    const int rc = AHardwareBuffer_lock(hb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &ptr);
    if (rc != 0 || !ptr) {
        LOGE("AHardwareBuffer_lock failed: %d", rc);
        return false;
    }
    interpolate(static_cast<const uint8_t*>(ptr), gW, gH);
    AHardwareBuffer_unlock(hb, nullptr);
    return true;
}

void present() {
    if (!gWindow || gOut.empty()) return;
    ANativeWindow_Buffer buf{};
    if (ANativeWindow_lock(gWindow, &buf, nullptr) != 0) return;
    const int copyW = std::min(gW, buf.width);
    const int copyH = std::min(gH, buf.height);
    for (int y = 0; y < copyH; ++y) {
        std::memcpy(static_cast<uint8_t*>(buf.bits) + static_cast<size_t>(y) * buf.stride * 4,
                    gOut.data() + static_cast<size_t>(y) * gW * 4,
                    static_cast<size_t>(copyW) * 4);
    }
    ANativeWindow_unlockAndPost(gWindow);
}
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_frameflow_MainActivity_nativeInit(JNIEnv* env, jobject, jobject surface, jint width, jint height) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gWindow) { ANativeWindow_release(gWindow); gWindow = nullptr; }
    gWindow = ANativeWindow_fromSurface(env, surface);
    gW = width; gH = height;
    gPrev.clear(); gCurr.clear(); gOut.clear(); gHavePrev = false;
    if (!gWindow) return JNI_FALSE;
    ANativeWindow_setBuffersGeometry(gWindow, width, height, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
    LOGI("FrameFlow initialized %dx%d", width, height);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_frameflow_MainActivity_nativeSubmitFrame(JNIEnv* env, jobject, jobject hardwareBuffer) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gWindow || !hardwareBuffer) return;
    if (readHardwareBuffer(env, hardwareBuffer)) present();
}

extern "C" JNIEXPORT void JNICALL
Java_com_frameflow_MainActivity_nativeStop(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lock(gMutex);
    gPrev.clear(); gCurr.clear(); gOut.clear(); gHavePrev = false;
    if (gWindow) { ANativeWindow_release(gWindow); gWindow = nullptr; }
}
