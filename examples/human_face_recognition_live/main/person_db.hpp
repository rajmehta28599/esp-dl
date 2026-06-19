#pragma once
#include <stdint.h>
#include <string>
#include <vector>

/*
 * Multi-template face "person" database.
 *
 * Why this exists (and why we don't use esp-dl's dl::recognition::DataBase for matching):
 *  - esp-dl's DB stores exactly ONE feature per auto-incremented id and has no notion of an
 *    identity owning several templates. Real-world accuracy needs 2-5 templates per person
 *    (different pose/lighting) fused at match time - the single dominant accuracy lever found
 *    in BENCHMARK_REPORT.md (a bad single enroll cratered accept rate to ~8%).
 *  - esp-dl's query_feat() returns the feature's LIST POSITION, not its stored id
 *    (dl_recognition_database.cpp:258 - `it->id` is commented out), so its ids are unstable
 *    after a delete. We need stable person ids for attendance.
 *
 * We therefore keep esp-dl only for FEATURE EXTRACTION (HumanFaceFeat::run -> aligned, model,
 * L2-normalised float[feat_len]) and own the storage + matching here. Features are unit vectors,
 * so cosine similarity is a plain dot product (identical to esp-dl's cal_similarity()).
 *
 * Threading: owned exclusively by the AI task (like the old g_recognizer), so no locking.
 * Persistence: a small binary file on the FAT "storage" partition, namespaced per
 * (recognizer x detector) pair because alignment is not interchangeable across detectors
 * (BENCHMARK_REPORT.md sec 3.2).
 */
class PersonDB {
public:
    struct MatchResult {
        int person_id;   // matched person id (>=1), or -1 if no match / empty db
        float sim;       // fused cosine similarity (max over that person's templates), raw
        int templates;   // number of templates the matched person has
        int second_id;   // runner-up person id, or -1 if the db has <2 people
        float second_sim; // runner-up's fused cosine, or -1 if no runner-up. The top1-vs-top2
                          // gap (sim - second_sim) is the decision margin: a small gap means the
                          // probe is nearly equidistant to two identities -> reject as ambiguous
                          // rather than risk a confident WRONG id (the id1<->id2 cross-match bug).
    };

    PersonDB() = default;

    // Point the DB at a file and feature length, then load it. A missing file is NOT an error
    // (starts empty); a feat_len/format mismatch is logged and the DB starts empty. Returns
    // true if a valid file was loaded, false if it started empty.
    bool load(const std::string &path, int feat_len);
    bool save() const; // persist to the path passed to load(); true on success

    // Create a new person; returns its (stable, monotonic) id >= 1. name may be nullptr/empty.
    int add_person(const char *name);
    // Append one template (feat_len L2-normalised floats) to an existing person.
    bool add_template(int person_id, const float *feat);

    // Best matching person for a probe feature. Fused score = max cosine over the person's
    // templates ("nearest template wins"). person_id = -1 when the DB is empty.
    MatchResult match(const float *probe) const;

    bool remove_person(int person_id);
    void clear(); // forget everyone (keeps the path/feat_len; caller should save())

    int num_persons() const { return (int)m_persons.size(); }
    int num_templates() const;
    int feat_len() const { return m_feat_len; }
    const char *person_name(int person_id) const; // "" if unknown

private:
    struct Person {
        int id;
        std::string name;
        // Each entry points to m_feat_len L2-normalised floats stored in PSRAM (heap_caps SPIRAM),
        // so the bulk feature data does not consume scarce INTERNAL RAM as the DB scales to many
        // people (e.g. 50 staff x 5 templates x 2 KB = 500 KB). Only the small pointer vector +
        // metadata live on the default heap.
        std::vector<float *> templates;
    };
    std::vector<Person> m_persons;
    std::string m_path;
    int m_feat_len = 0;
    int m_next_id = 1;

    Person *find(int person_id);
    const Person *find(int person_id) const;
    void free_person(Person &p);                                // heap_caps_free each template
    static float cosine(const float *a, const float *b, int n); // dot product (unit vectors)
};
