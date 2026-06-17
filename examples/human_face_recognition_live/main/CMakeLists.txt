idf_component_register(
    SRCS "app_main.cpp" "face_processor.cpp" "ui.cpp" "yunet_detect.cpp"
    INCLUDE_DIRS "."
    REQUIRES human_face_recognition
             esp-dl
             spiflash_fatfs
             bsp_display
             bsp_camera
             esp_lvgl_port
             lvgl
    EMBED_FILES "human_face.jpg" "yunet_640_p4.espdl")
