#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <lv2/core/lv2.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/atom.h>
#include <lv2/urid/urid.h>

static uint32_t s_next = 1u;
static LV2_URID simple_urid_map(LV2_URID_Map_Handle handle, const char* uri) {
    (void)handle; (void)uri; return (LV2_URID)(s_next++);
}

int main(int argc, char** argv) {
    const char* plugin_so = (argc > 1) ? argv[1] : "../atom-logger.lv2/atom-logger.so";
    void* h = dlopen(plugin_so, RTLD_NOW);
    if (!h) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    const LV2_Descriptor* (*lv2_descriptor)(uint32_t) = dlsym(h, "lv2_descriptor");
    if (!lv2_descriptor) {
        fprintf(stderr, "no lv2_descriptor\n");
        return 2;
    }
    const LV2_Descriptor* desc = lv2_descriptor(0);
    if (!desc) {
        fprintf(stderr, "descriptor null\n");
        return 3;
    }

    // Minimal URID map used by forge
    static LV2_URID_Map g_map = { NULL, simple_urid_map };
    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &g_map);

    uint8_t tmp[4096];

    // If a dump file is provided as argv[2], use its contents as the forged LV2_Atom
    if (argc > 2) {
        FILE* f = fopen(argv[2], "rb");
        if (!f) { fprintf(stderr, "failed to open dump %s\n", argv[2]); return 4; }
        size_t r = fread(tmp, 1, sizeof(tmp), f);
        fclose(f);
        if (r < sizeof(LV2_Atom)) { fprintf(stderr, "dump too small\n"); return 4; }
        // We'll treat tmp as containing a complete LV2_Atom (header+body)
    } else {
        lv2_atom_forge_set_buffer(&forge, tmp, sizeof(tmp));
        LV2_Atom_Forge_Frame frame;
        // create patch:Set object
        lv2_atom_forge_object(&forge, &frame, 0, simple_urid_map(NULL, "http://lv2plug.in/ns/ext/patch#Set"));
        lv2_atom_forge_key(&forge, simple_urid_map(NULL, "http://lv2plug.in/ns/ext/patch#subject"));
        lv2_atom_forge_uri(&forge, "http://example.org/myparam", (uint32_t)strlen("http://example.org/myparam"));
        lv2_atom_forge_key(&forge, simple_urid_map(NULL, "http://lv2plug.in/ns/ext/patch#value"));
        lv2_atom_forge_float(&forge, 0.5f);
        lv2_atom_forge_pop(&forge, &frame);

        if (forge.offset < sizeof(LV2_Atom)) {
            fprintf(stderr, "forge failed\n");
            return 4;
        }
    }

    // Build an LV2_Atom_Sequence buffer containing the forged atom event
    const size_t BUF_CAP = 2048;
    uint8_t* seqbuf = (uint8_t*)calloc(1, BUF_CAP);
    LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)seqbuf;
    seq->atom.type = simple_urid_map(NULL, "http://lv2plug.in/ns/ext/atom#Sequence");
    seq->body.unit = 0; seq->body.pad = 0;

    uint8_t* b = (uint8_t*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, seq);
    LV2_Atom_Event* ev = (LV2_Atom_Event*)b;
    ev->time.frames = 0;
    LV2_Atom* forged = (LV2_Atom*)tmp;
    uint32_t bodySize = forged->size;
    ev->body.size = bodySize;
    ev->body.type = forged->type;
    memcpy(LV2_ATOM_BODY(&ev->body), LV2_ATOM_BODY(forged), bodySize);
    size_t padded = sizeof(LV2_Atom_Event) + ((bodySize + 7) & ~7);
    seq->atom.size = sizeof(LV2_Atom_Sequence_Body) + padded;

    // Instantiate plugin
    const LV2_Feature* features[2]; features[0] = NULL;
    LV2_Handle inst = desc->instantiate(desc, 44100.0, NULL, features);
    if (!inst) { fprintf(stderr, "instantiate failed\n"); return 5; }

    // Connect port 0
    desc->connect_port(inst, 0, seq);

    // Run
    desc->run(inst, 1);

    // Cleanup
    desc->cleanup(inst);
    dlclose(h);
    free(seqbuf);

    fprintf(stderr, "Test host finished\n");
    return 0;
}
