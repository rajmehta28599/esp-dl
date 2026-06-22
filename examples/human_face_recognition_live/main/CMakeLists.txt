idf_component_register(
    SRCS "app_main.cpp" "face_processor.cpp" "ui.cpp" "yunet_detect.cpp" "person_db.cpp" "ppa_display.cpp"
    INCLUDE_DIRS "."
    REQUIRES human_face_recognition
             esp-dl
             spiflash_fatfs
             bsp_display
             bsp_camera
             esp_lvgl_port
             lvgl
             esp_driver_ppa
             esp_mm
    EMBED_FILES "human_face.jpg" "yunet_256x192_p4.espdl" "yunet_384x288_p4.espdl" "yunet_128x96_p4.espdl" "yunet_512x384_p4.espdl")
