# `spiflash_fatfs` — on‑flash FAT store for the face database

Mounts the wear‑levelled **FAT** partition that holds the enrolled‑face database, so enrollments survive
reboots. The recognizer writes `face_<model>.db` (e.g. `face_mfn.db`, `face_mbf.db`) under the mount point.

* Partition: `storage` (type `data`, subtype `fat`), 1 MB, at `0xa10000` (see `partitions.csv`).
* Mount point: `CONFIG_SPIFLASH_MOUNT_POINT` (default `/spiflash`), mounted read‑write with wear levelling,
  formatted automatically if the mount fails (`format_if_mount_failed=true`).
* `CONFIG_FATFS_LFN_HEAP=y` is enabled so long DB filenames work.

## API (`spiflash_fatfs.hpp`)

```c
esp_err_t fatfs_flash_mount(void);    // call once, early in app_main (before the face processor)
esp_err_t fatfs_flash_unmount(void);
```

## Capacity

Each enrolled face is `feat_len × 4 + 2` bytes (MFN/MBF feat_len = 512 → ~2 KB/face), so the 1 MB
partition holds a few hundred faces. The live dashboard's STORAGE line shows used/free KB and the
approximate max via `esp_vfs_fat_info()`. A flash write (enroll / clear DB) briefly stalls inference —
expected, infrequent.
