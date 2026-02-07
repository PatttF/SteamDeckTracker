#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>

#define PLUGIN_URI "http://example.org/atom-logger"

typedef struct {
    const LV2_Atom_Sequence* input;
    FILE* logf;
} AtomLogger;

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                     rate,
            const char* const*         paths,
            const LV2_Feature* const*  features)
{
    (void)descriptor; (void)rate; (void)paths; (void)features;
    AtomLogger* self = (AtomLogger*)malloc(sizeof(AtomLogger));
    if (!self) return NULL;
    self->input = NULL;

    char path[256];
    snprintf(path, sizeof(path), "/tmp/atom_logger_%d.log", (int)getpid());
    self->logf = fopen(path, "a");
    if (!self->logf) self->logf = stderr;
    fprintf(self->logf, "AtomLogger: instantiated pid=%d\n", (int)getpid());
    fflush(self->logf);

    return (LV2_Handle)self;
}

static void
connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    AtomLogger* self = (AtomLogger*)instance;
    if (!self) return;
    if (port == 0) {
        self->input = (const LV2_Atom_Sequence*)data;
    }
}

static void
run(LV2_Handle instance, uint32_t n_samples)
{
    AtomLogger* self = (AtomLogger*)instance;
    if (!self || !self->input) return;

    const LV2_Atom_Sequence* seq = self->input;
    uint32_t seq_size = seq->atom.size;
    fprintf(self->logf, "AtomLogger: run called seq_size=%u\n", seq_size);

    // Walk events
    const uint8_t* it = (const uint8_t*)LV2_ATOM_BODY(&seq->body);
    size_t offset = 0;
    while (offset + sizeof(LV2_Atom_Event) <= seq->atom.size) {
        const LV2_Atom_Event* ev = (const LV2_Atom_Event*)(it + offset);
        uint32_t bsz = ev->body.size;
        uint32_t type = ev->body.type;
        fprintf(self->logf, "Event: time=%u type=%u size=%u\n", (unsigned)ev->time.frames, (unsigned)type, (unsigned)bsz);
        // Dump first bytes
        size_t show = (bsz < 32) ? bsz : 32;
        const uint8_t* body = (const uint8_t*)LV2_ATOM_BODY(&ev->body);
        for (size_t i = 0; i < show; ++i) {
            fprintf(self->logf, "%02X", body[i]);
            if (i + 1 < show) fprintf(self->logf, ",");
        }
        fprintf(self->logf, "\n");
        fflush(self->logf);

        size_t padded = sizeof(LV2_Atom_Event) + ((bsz + 7) & ~7);
        offset += padded;
        if (offset >= seq->atom.size) break;
    }
}

static void
cleanup(LV2_Handle instance)
{
    AtomLogger* self = (AtomLogger*)instance;
    if (!self) return;
    if (self->logf && self->logf != stderr) fclose(self->logf);
    free(self);
}

static const LV2_Descriptor descriptor = {
    PLUGIN_URI,
    instantiate,
    connect_port,
    NULL,
    run,
    cleanup
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
    if (index == 0) return &descriptor;
    return NULL;
}
