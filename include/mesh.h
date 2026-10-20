#ifndef MESH_H
#define MESH_H

typedef struct {
    unsigned short idx[3];
    unsigned int color;   /* baked sRGB 0xRRGGBB */
    float n[3];           /* flat face normal (model space) */
} MeshTri;

typedef struct {
    const float *verts;   /* xyz triples */
    const MeshTri *tris;
    int nverts;
    int ntris;
    float min[3], max[3]; /* model-space bounds */
} Mesh;

#endif
