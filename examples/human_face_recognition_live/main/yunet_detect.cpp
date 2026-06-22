#include "yunet_detect.hpp"

#include "dl_model_base.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_tensor_base.hpp"
#include "dl_define.hpp"
#include "esp_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace dl;
using namespace dl::detect;

static const char *TAG = "yunet";

// Embedded quantized model(s) — both 4:3 (match the 4:3 camera crop -> no aspect distortion).
// YUNET_USE_384: resolution A/B study (NEXT_STEPS section A). 1 = 384x288 (bigger: more range +
// sharper landmarks, affordable now that PPA freed core 1); 0 = 256x192 (committed default). Both
// dims /32 so the decode (cols=W/s, rows=H/s) is unchanged — 256x192 grids 32x24/16x12/8x6 = 768/192/48;
// 384x288 grids 48x36/24x18/12x9 = 1728/432/108.
// YuNet input-resolution selector (study, NEXT_STEPS A). Valid 4:3 + /32 sizes: 128, 256, 384, 512.
// Findings: 256 = clean baseline (Test 007/012); 384 = too heavy (TWDT overload, Test 019); 512 strictly
// worse than 384 -> skip; 128 = lightest, for the COMPACT CLOSE-RANGE device. Set to 128 / 256 / 384.
#define YUNET_RES 256 // LOCKED (Test 020): 256 = sweet spot. 128 recognition too noisy (coarse landmarks
                       // -> genuine 0.51-0.88 + REJECTs); 384 too heavy (overload); 512 ruled out. 256 wins.
#if YUNET_RES == 128
extern const uint8_t g_yunet_espdl[] asm("_binary_yunet_128x96_p4_espdl_start");
#define YUNET_W 128
#define YUNET_H 96
#elif YUNET_RES == 384
extern const uint8_t g_yunet_espdl[] asm("_binary_yunet_384x288_p4_espdl_start");
#define YUNET_W 384
#define YUNET_H 288
#else // 256 = baseline default
extern const uint8_t g_yunet_espdl[] asm("_binary_yunet_256x192_p4_espdl_start");
#define YUNET_W 256
#define YUNET_H 192
#endif
static const int STRIDES[3] = {8, 16, 32}; // anchor-free detection strides

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

YuNetDetect::YuNetDetect(float score_thr, float nms_thr, int top_k) :
    m_score_thr(score_thr), m_nms_thr(nms_thr), m_top_k(top_k)
{
    m_model = new Model((const char *)g_yunet_espdl, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    m_model->minimize();
    // YuNet wants RAW 0-255 BGR: mean 0 / std 1 => no normalization. rgb_swap=false keeps the
    // camera's BGR order. If YuNet never detects, the colour order is the first thing to flip.
    m_pre = new image::ImagePreprocessor(m_model, {0, 0, 0}, {1, 1, 1}, false);
    ESP_LOGI(TAG, "YuNet loaded (input %dx%d)", YUNET_W, YUNET_H);
}

YuNetDetect::~YuNetDetect()
{
    delete m_model;
    delete m_pre;
}

Detect &YuNetDetect::set_score_thr(float score_thr, int)
{
    m_score_thr = score_thr;
    return *this;
}

Detect &YuNetDetect::set_nms_thr(float nms_thr, int)
{
    m_nms_thr = nms_thr;
    return *this;
}

Model *YuNetDetect::get_raw_model(int)
{
    return m_model;
}

// IoU NMS over a score-sorted list (mirrors dl::detect::DetectPostprocessor::nms()).
static void nms_list(std::list<result_t> &lst, float nms_thr, int top_k)
{
    int kept = 0;
    for (auto it = lst.begin(); it != lst.end(); ++it) {
        if (++kept >= top_k) {
            lst.erase(std::next(it), lst.end());
            break;
        }
        int a = (it->box[2] - it->box[0] + 1) * (it->box[3] - it->box[1] + 1);
        for (auto o = std::next(it); o != lst.end();) {
            int ix = DL_MAX(it->box[0], o->box[0]), iy = DL_MAX(it->box[1], o->box[1]);
            int rx = DL_MIN(it->box[2], o->box[2]), ry = DL_MIN(it->box[3], o->box[3]);
            int iw = rx - ix + 1, ih = ry - iy + 1;
            if (iw > 0 && ih > 0) {
                int ob = (o->box[2] - o->box[0] + 1) * (o->box[3] - o->box[1] + 1);
                int inter = iw * ih;
                if ((float)inter / (a + ob - inter) > nms_thr) {
                    o = lst.erase(o);
                    continue;
                }
            }
            ++o;
        }
    }
}

std::list<result_t> &YuNetDetect::run(const dl::image::img_t &img)
{
    m_results.clear();
    m_pre->preprocess(img);
    m_model->run();

    // Map model(256x192) coords back to the source image (no letterbox -> border 0).
    const float inv_sx = m_pre->get_resize_scale_x(true);
    const float inv_sy = m_pre->get_resize_scale_y(true);
    const int bl = m_pre->get_border_left();
    const int bt = m_pre->get_border_top();

    for (int si = 0; si < 3; si++) {
        const int s = STRIDES[si];
        const int cols = YUNET_W / s; // rows = YUNET_H/s implied via num = rows*cols
        char n_cls[8], n_obj[8], n_box[8], n_kps[8];
        snprintf(n_cls, sizeof(n_cls), "cls_%d", s);
        snprintf(n_obj, sizeof(n_obj), "obj_%d", s);
        snprintf(n_box, sizeof(n_box), "bbox_%d", s);
        snprintf(n_kps, sizeof(n_kps), "kps_%d", s);
        TensorBase *t_cls = m_model->get_output(n_cls);
        TensorBase *t_obj = m_model->get_output(n_obj);
        TensorBase *t_box = m_model->get_output(n_box);
        TensorBase *t_kps = m_model->get_output(n_kps);
        if (!t_cls || !t_obj || !t_box || !t_kps) {
            ESP_LOGW(TAG, "missing output for stride %d", s);
            continue;
        }
        const int num = t_cls->shape[1]; // [1, N, 1]
        const float sc_cls = DL_SCALE(t_cls->exponent), sc_obj = DL_SCALE(t_obj->exponent);
        const float sc_box = DL_SCALE(t_box->exponent), sc_kps = DL_SCALE(t_kps->exponent);
        const int8_t *p_cls = (const int8_t *)t_cls->data;
        const int8_t *p_obj = (const int8_t *)t_obj->data;
        const int8_t *p_box = (const int8_t *)t_box->data;
        const int8_t *p_kps = (const int8_t *)t_kps->data;

        for (int i = 0; i < num; i++) {
            // cls/obj are already post-sigmoid in [0,1]; score = sqrt(cls * obj).
            float c = clamp01(dequantize(p_cls[i], sc_cls));
            float o = clamp01(dequantize(p_obj[i], sc_obj));
            float score = sqrtf(c * o);
            if (score < m_score_thr) {
                continue;
            }
            int row = i / cols, col = i % cols;
            float dx = dequantize(p_box[i * 4 + 0], sc_box);
            float dy = dequantize(p_box[i * 4 + 1], sc_box);
            float dw = dequantize(p_box[i * 4 + 2], sc_box);
            float dh = dequantize(p_box[i * 4 + 3], sc_box);
            float cx = (col + dx) * s, cy = (row + dy) * s;
            float w = expf(dw) * s, h = expf(dh) * s;

            result_t r;
            r.category = 0;
            r.score = score;
            r.box = {(int)(((cx - w * 0.5f) - bl) * inv_sx), (int)(((cy - h * 0.5f) - bt) * inv_sy),
                     (int)(((cx + w * 0.5f) - bl) * inv_sx), (int)(((cy + h * 0.5f) - bt) * inv_sy)};

            // esp-dl's recognizer aligns to s_std_ldks_112, which fills its 5 slots BY IMAGE SIDE:
            // [img-left-eye, img-left-mouth, nose, img-right-eye, img-right-mouth] (slot0 is at x=38 =
            // image-left). On-device DBG shows the raw YuNet indices land as: 0=img-left-eye,
            // 1=img-right-eye, 2=nose, 3=img-left-mouth, 4=img-right-mouth. So fill BY IMAGE SIDE:
            // slot0<-0, slot1<-3, slot2<-2, slot3<-1, slot4<-4. (The earlier {1,4,2,0,3} mapped by
            // anatomical L/R and fed the image-RIGHT eye into the image-LEFT slot -> a reflected
            // correspondence esp-dl's similarity transform can't represent, which collapsed all
            // identities together [YuNet impostor sim ~0.8 vs MSRMNP ~0.2]; see TEST_LOG Test 006.)
            int lx[5], ly[5];
            for (int k = 0; k < 5; k++) {
                float kx = dequantize(p_kps[i * 10 + 2 * k], sc_kps);
                float ky = dequantize(p_kps[i * 10 + 2 * k + 1], sc_kps);
                lx[k] = (int)((((kx + col) * s) - bl) * inv_sx);
                ly[k] = (int)((((ky + row) * s) - bt) * inv_sy);
            }
            r.keypoint = {lx[0], ly[0], lx[3], ly[3], lx[2], ly[2], lx[1], ly[1], lx[4], ly[4]};

            m_results.insert(std::upper_bound(m_results.begin(), m_results.end(), r, greater_box), r);
        }
    }
    nms_list(m_results, m_nms_thr, m_top_k);
    return m_results;
}
