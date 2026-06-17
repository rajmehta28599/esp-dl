#pragma once
#include "dl_detect_base.hpp"

// YuNet face detector, ported to esp-dl via ESP-PPQ (see ../../../yunet_port/).
//
// Implements the same dl::detect::Detect interface as HumanFaceDetect, so it drops into the
// runtime detector switch. Unlike ESPDet, YuNet emits 5 facial landmarks; this wrapper REORDERS
// them from YuNet's order [right_eye, left_eye, nose, right_mouth, left_mouth] into esp-dl's
// recognizer convention [left_eye, left_mouth, nose, right_eye, right_mouth] so face alignment /
// recognition work unchanged.
//
// Model: input 1x3x640x640 NCHW, BGR, RAW 0-255 (no normalization). 12 outputs
// {cls,obj,bbox,kps} x strides {8,16,32}; decode = sqrt(cls*obj), cx=(c+dx)*s, w=exp(dw)*s,
// landmark=(k+c/r)*s. The .espdl is embedded (EMBED_FILES yunet_640_p4.espdl).
class YuNetDetect : public dl::detect::Detect {
public:
    YuNetDetect(float score_thr = 0.5f, float nms_thr = 0.3f, int top_k = 10);
    ~YuNetDetect();

    std::list<dl::detect::result_t> &run(const dl::image::img_t &img) override;
    dl::detect::Detect &set_score_thr(float score_thr, int idx = 0) override;
    dl::detect::Detect &set_nms_thr(float nms_thr, int idx = 0) override;
    dl::Model *get_raw_model(int idx = 0) override;

private:
    dl::Model *m_model = nullptr;
    dl::image::ImagePreprocessor *m_pre = nullptr;
    float m_score_thr, m_nms_thr;
    int m_top_k;
    std::list<dl::detect::result_t> m_results;
};
