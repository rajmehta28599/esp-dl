#include "person_db.hpp"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include <cstdio>
#include <cstring>
#include <unistd.h> // fsync

static const char *TAG = "person_db";

// On-disk format (little-endian native; same MCU writes+reads, so no portability concern):
//   magic   u32  = 'PDB1'
//   version u16  = 1
//   feat_len u16
//   next_id  u16
//   n_person u16
//   per person:  id u16 | name_len u16 | name[name_len] | n_tmpl u16 | float[feat_len]*n_tmpl
#define PDB_MAGIC 0x31424450u // "PDB1"
#define PDB_VERSION 1

float PersonDB::cosine(const float *a, const float *b, int n)
{
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum; // both are L2-normalised -> dot product == cosine similarity
}

PersonDB::Person *PersonDB::find(int person_id)
{
    for (auto &p : m_persons) {
        if (p.id == person_id) {
            return &p;
        }
    }
    return nullptr;
}

const PersonDB::Person *PersonDB::find(int person_id) const
{
    for (const auto &p : m_persons) {
        if (p.id == person_id) {
            return &p;
        }
    }
    return nullptr;
}

void PersonDB::free_person(Person &p)
{
    for (float *t : p.templates) {
        if (t) {
            heap_caps_free(t);
        }
    }
    p.templates.clear();
}

int PersonDB::num_templates() const
{
    int n = 0;
    for (const auto &p : m_persons) {
        n += (int)p.templates.size();
    }
    return n;
}

const char *PersonDB::person_name(int person_id) const
{
    const Person *p = find(person_id);
    return p ? p->name.c_str() : "";
}

void PersonDB::clear()
{
    for (auto &p : m_persons) {
        free_person(p);
    }
    m_persons.clear();
    m_next_id = 1;
}

int PersonDB::add_person(const char *name)
{
    Person p;
    p.id = m_next_id++;
    p.name = name ? name : "";
    m_persons.push_back(std::move(p));
    return m_persons.back().id;
}

bool PersonDB::add_template(int person_id, const float *feat)
{
    if (!feat || m_feat_len <= 0) {
        return false;
    }
    Person *p = find(person_id);
    if (!p) {
        ESP_LOGW(TAG, "add_template: unknown person %d", person_id);
        return false;
    }
    float *t = (float *)heap_caps_malloc((size_t)m_feat_len * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!t) {
        ESP_LOGE(TAG, "add_template: out of PSRAM");
        return false;
    }
    memcpy(t, feat, (size_t)m_feat_len * sizeof(float));
    p->templates.push_back(t);
    return true;
}

PersonDB::MatchResult PersonDB::match(const float *probe) const
{
    MatchResult r = {-1, 0.0f, 0};
    if (!probe || m_feat_len <= 0) {
        return r;
    }
    for (const auto &p : m_persons) {
        float best = -1.0f; // fuse templates: nearest (max cosine) wins
        for (const float *t : p.templates) {
            float s = cosine(probe, t, m_feat_len);
            if (s > best) {
                best = s;
            }
        }
        if (best > r.sim || r.person_id < 0) {
            r.sim = best;
            r.person_id = p.id;
            r.templates = (int)p.templates.size();
        }
    }
    return r;
}

bool PersonDB::remove_person(int person_id)
{
    for (auto it = m_persons.begin(); it != m_persons.end(); ++it) {
        if (it->id == person_id) {
            free_person(*it);
            m_persons.erase(it);
            return true;
        }
    }
    return false;
}

bool PersonDB::load(const std::string &path, int feat_len)
{
    m_path = path;
    m_feat_len = feat_len;
    clear(); // frees any existing templates

    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        ESP_LOGI(TAG, "no db at %s (starting empty)", path.c_str());
        return false;
    }

    uint32_t magic = 0;
    uint16_t version = 0, flen = 0, next_id = 0, n_person = 0;
    bool hdr = fread(&magic, sizeof(magic), 1, f) == 1 && fread(&version, sizeof(version), 1, f) == 1 &&
               fread(&flen, sizeof(flen), 1, f) == 1 && fread(&next_id, sizeof(next_id), 1, f) == 1 &&
               fread(&n_person, sizeof(n_person), 1, f) == 1;
    if (!hdr || magic != PDB_MAGIC || version != PDB_VERSION || flen != (uint16_t)feat_len) {
        ESP_LOGW(TAG, "db header mismatch (magic/ver/feat_len) at %s; starting empty", path.c_str());
        fclose(f);
        return false;
    }
    m_next_id = next_id < 1 ? 1 : next_id;

    bool corrupt = false;
    for (int i = 0; i < n_person && !corrupt; i++) {
        uint16_t id = 0, name_len = 0, n_tmpl = 0;
        if (fread(&id, sizeof(id), 1, f) != 1 || fread(&name_len, sizeof(name_len), 1, f) != 1) {
            corrupt = true;
            break;
        }
        Person p;
        p.id = id;
        if (name_len > 0) {
            std::vector<char> nm(name_len + 1, 0);
            if (fread(nm.data(), 1, name_len, f) != name_len) {
                corrupt = true;
                break;
            }
            p.name.assign(nm.data(), name_len);
        }
        if (fread(&n_tmpl, sizeof(n_tmpl), 1, f) != 1) {
            corrupt = true;
            break;
        }
        for (int t = 0; t < n_tmpl && !corrupt; t++) {
            float *buf = (float *)heap_caps_malloc((size_t)feat_len * sizeof(float), MALLOC_CAP_SPIRAM);
            if (!buf) {
                ESP_LOGE(TAG, "load: out of PSRAM");
                corrupt = true;
                break;
            }
            if ((int)fread(buf, sizeof(float), feat_len, f) != feat_len) {
                heap_caps_free(buf);
                corrupt = true;
                break;
            }
            p.templates.push_back(buf);
        }
        if (corrupt) {
            free_person(p); // release this partial person's PSRAM
            break;
        }
        if (id >= m_next_id) {
            m_next_id = id + 1; // keep ids monotonic past anything on disk
        }
        m_persons.push_back(std::move(p));
    }
    fclose(f);
    if (corrupt) {
        ESP_LOGE(TAG, "db corrupt at %s; starting empty", path.c_str());
        clear();
        return false;
    }
    ESP_LOGI(TAG, "loaded %d person(s), %d template(s) from %s", num_persons(), num_templates(),
             path.c_str());
    return true;
}

bool PersonDB::save() const
{
    if (m_path.empty()) {
        ESP_LOGE(TAG, "save: no path set");
        return false;
    }
    FILE *f = fopen(m_path.c_str(), "wb");
    if (!f) {
        ESP_LOGE(TAG, "save: cannot open %s", m_path.c_str());
        return false;
    }
    uint32_t magic = PDB_MAGIC;
    uint16_t version = PDB_VERSION, flen = (uint16_t)m_feat_len, next_id = (uint16_t)m_next_id;
    uint16_t n_person = (uint16_t)m_persons.size();
    bool ok = fwrite(&magic, sizeof(magic), 1, f) == 1 && fwrite(&version, sizeof(version), 1, f) == 1 &&
              fwrite(&flen, sizeof(flen), 1, f) == 1 && fwrite(&next_id, sizeof(next_id), 1, f) == 1 &&
              fwrite(&n_person, sizeof(n_person), 1, f) == 1;
    for (const auto &p : m_persons) {
        if (!ok) {
            break;
        }
        uint16_t id = (uint16_t)p.id, name_len = (uint16_t)p.name.size(), n_tmpl = (uint16_t)p.templates.size();
        ok = ok && fwrite(&id, sizeof(id), 1, f) == 1 && fwrite(&name_len, sizeof(name_len), 1, f) == 1;
        if (ok && name_len > 0) {
            ok = fwrite(p.name.data(), 1, name_len, f) == name_len;
        }
        ok = ok && fwrite(&n_tmpl, sizeof(n_tmpl), 1, f) == 1;
        for (const float *t : p.templates) {
            if (!ok) {
                break;
            }
            ok = (int)fwrite(t, sizeof(float), m_feat_len, f) == m_feat_len;
        }
    }
    if (ok) {
        // Force FATFS to commit data + FAT + directory entry to flash NOW. Without this, fclose()
        // only flushes the C stream; a power-cycle before the FAT cache is synced loses or
        // cross-links the file (observed: enrollments reloading empty/corrupted after a power-cycle).
        fflush(f);
        fsync(fileno(f));
    }
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "save: write failed for %s", m_path.c_str());
    }
    return ok;
}
