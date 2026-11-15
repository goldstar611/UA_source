#ifndef OBJ3D_H_INCLUDED
#define OBJ3D_H_INCLUDED

#include <list>
#include <string>
#include "system/fsmgr.h"
#include "nucleas.h"
#include "base.h"
#include "bitmap.h"


class NC_STACK_Obj3D: public NC_STACK_base
{        
protected:
    struct FaceVT
    {
        int32_t V = 0;
        int32_t T = 0;
        int32_t N = 0;
        
        FaceVT() = default;
        FaceVT(int32_t v, int32_t t, int32_t n)
        : V(v), T(t), N(n) {};
    };
    
    struct Mtl
    {
        std::string name;
        float diffuse[3] = {1.0, 1.0, 1.0};
        NC_STACK_bitmap *diffuseMap = NULL;
        float d = 1.0;
        
        ~Mtl()
        {
            if (diffuseMap)
                diffuseMap->Delete();
        }
        
        GFX::TRenderParams GenParams() const;
    };
        
public:
    virtual size_t Init(IDVList &stak);
    virtual size_t Deinit();

    NC_STACK_Obj3D();
    virtual ~NC_STACK_Obj3D();

    virtual const std::string ClassName() const {
        return __ClassName;
    };

    
    // Rebuild meshes from the attached ADEs when present (uattach models).
    // Plain OBJ models (no ADEs) keep the OBJ-parsed meshes: the base
    // implementation would clear them and rebuild nothing.
    virtual void RecalcInternal(bool kids = false) override
    {
        if ( _ADES.empty() )
            return;
        NC_STACK_base::RecalcInternal(kids);
    };

    bool LoadFromFile(const std::string &filename);
    bool LoadFromFile(FSMgr::FileHandle *file);
    
    static NC_STACK_Obj3D *LoadObj3D(const std::string &filename);

    // Load a per-model asset set: parses <fname>.obj via the normal OBJ path,
    // then - if a <fname>.uattach sidecar exists next to it - replaces the
    // OBJ-rebuilt skeleton with the sidecar skeleton, attaches every ADE from
    // the sidecar's standalone .ade files (with bmpanim payloads from the
    // model's own .anm files and all textures bound from the model's own pngs,
    // never from the shared rsrc pool) and regenerates the meshes.
    // Without a .uattach sidecar the plain OBJ load behavior is unchanged.
    static NC_STACK_Obj3D *LoadObjWithAttachments(const std::string &fname);
    
    // Export 'base' as Wavefront .obj + .mtl + texture pngs (named after the
    // texture's own rsrc name, deduped within the model) + <fname>.mat.json
    // sidecar. 'sourceFile' is recorded in the json sidecar.
    // 'fname' must be a path usable by FSMgr/uaOpenFile (e.g. "export/KID.obj").
    // Texture files are written next to the .obj. '.mat.json' is written via
    // FSMgr::iDir::openFileAlloc directly (no >3 char extension truncation).
    static bool ExportObj(NC_STACK_base *base, const std::string &fname, const std::string &sourceFile = std::string());

private:
    bool ParseObj(FSMgr::FileHandle *fil);
    FaceVT ParseFaceVT(const std::string &l);
    bool ParseMtl(const std::string &filename);
    Mtl *FindMtl(const std::string &filename);

public:
    //Data
    static constexpr const char * __ClassName = "obj3d.class";

private:
    std::vector<vec3d> _v;
    std::vector<vec3d> _vt;
    std::vector<vec3d> _vn;
    std::vector< std::array<FaceVT, GFX_MAX_VERTEX> > _f;
    std::list<Mtl> _mtls;
    
    std::string fname;
    
    Mtl *_currentMtl = NULL;
    
    int32_t faceNum = 0;
};

#endif // OBJ3D_H_INCLUDED
