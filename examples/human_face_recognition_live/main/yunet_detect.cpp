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

// Embedded quantized models — all 4:3 (match the 4:3 camera crop -> no aspect distortion) + both dims
// /32 (clean decode grid). Runtime-selectable via the DET cycle (yunet_make() below): 128/256/384/512.
// 256 = recommended sweet spot (Test 020); 128 lighter but recognition noisier; 384/512 heavier and only
// the PPA display path runs them without overloading core 0.
extern const uint8_t g_yunet_128[] asm("_binary_yunet_128x96_p4_espdl_start");
extern const uint8_t g_yunet_256[] asm("_binary_yunet_256x192_p4_espdl_start");
extern const uint8_t g_yunet_384[] asm("_binary_yunet_384x288_p4_espdl_start");
extern const uint8_t g_yunet_512[] asm("_binary_yunet_512x384_p4_espdl_start");
static const int STRIDES[3] = {8, 16, 32}; // anchor-free detection strides

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

YuNetDetect::YuNetDetect(const uint8_t *model_espdl, int model_w, int model_h, float score_thr,
                         float nms_thr, int top_k) :
    m_score_thr(score_thr), m_nms_thr(nms_thr), m_top_k(top_k), m_w(model_w), m_h(model_h)
{
    m_model = new Model((const char *)model_espdl, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    m_model->minimize();
    // YuNet wants RAW 0-255 BGR: mean 0 / std 1 => no normalization. rgb_swap=false keeps the
    // camera's BGR order. If YuNet never detects, the colour order is the first thing to flip.
    m_pre = new image::ImagePreprocessor(m_model, {0, 0, 0}, {1, 1, 1}, false);
    ESP_LOGI(TAG, "YuNet loaded (input %dx%d)", m_w, m_h);
}

// Map an input width to the matching embedded model (4:3, so H = W*3/4). Used by the DET cycle.
YuNetDetect *yunet_make(int width)
{
    switch (width) {
    case 128: return new YuNetDetect(g_yunet_128, 128, 96);
    case 384: return new YuNetDetect(g_yunet_384, 384, 288);
    case 512: return new YuNetDetect(g_yunet_512, 512, 384);
    case 256:
    default: return new YuNetDetect(g_yunet_256, 256, 192);
    }
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
        const int cols = m_w / s; // rows = m_h/s implied via num = rows*cols
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
