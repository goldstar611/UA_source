#include "obj3d.h"
#include "image.h"
#include "env.h"
#include "system/inivals.h"
#include "loaders.h"
#include "fmtlib/printf.h"
#include "bmpAnm.h"
#include "skeleton.h"

#include <map>
#include <set>
#include <vector>


NC_STACK_Obj3D::NC_STACK_Obj3D()
{}

NC_STACK_Obj3D::~NC_STACK_Obj3D()
{}

size_t NC_STACK_Obj3D::Init(IDVList &stak)
{
    if ( !NC_STACK_base::Init(stak) )
        return 0;

    return 1;
}

size_t NC_STACK_Obj3D::Deinit()
{
    return NC_STACK_base::Deinit();
}

bool NC_STACK_Obj3D::LoadFromFile(const std::string &filename)
{
    FSMgr::FileHandle fil = FSMgr::iDir::openFile(filename, "r");

    if (!fil.OK())
        return false;
    
    fname = filename;

    return LoadFromFile(&fil);
}

bool NC_STACK_Obj3D::LoadFromFile(FSMgr::FileHandle *fil)
{
    if (!ParseObj(fil))
        return false;

    MakeCoordsCache();
        
    _skeleton = Nucleus::CInit<NC_STACK_skeleton>( {
        {NC_STACK_rsrc::RSRC_ATT_NAME, std::string("Obj3D_sklt")},
        {NC_STACK_rsrc::RSRC_ATT_TRYSHARED, (int32_t)2},
        {NC_STACK_skeleton::SKEL_ATT_POINTSCNT, (int32_t)_v.size()}});  
    
    UAskeleton::Data *dat = _skeleton->GetSkelet();

    for (int32_t i = 0; i < (int32_t)_v.size(); ++i)
        dat->POO[i] = _v[i];
    
    dat->polygons.resize(_f.size());

    for (int32_t i = 0; i < (int32_t)_f.size(); i++)
    {
        UAskeleton::Polygon &pol = dat->polygons[i];
        
        for (int j = 0; j < GFX_MAX_VERTEX; ++j )
        {
            FaceVT &f = _f[i].at(j);
            if (f.V == 0)
            {
                break;
            }
            
            pol.v[j] = f.V - 1;
            pol.num_vertices = j + 1;
        }
    }
    
    return true;
}

NC_STACK_Obj3D *NC_STACK_Obj3D::LoadObj3D(const std::string &filename)
{
    FSMgr::FileHandle fil = uaOpenFile(filename, "r");
    if (!fil.OK())
        return NULL;
    
    NC_STACK_Obj3D *tmp = Nucleus::CInit<NC_STACK_Obj3D>();
    tmp->fname = filename;
    if(tmp->LoadFromFile(&fil))
        return tmp;
    
    tmp->Delete();
    return NULL;
}

bool NC_STACK_Obj3D::ParseObj(FSMgr::FileHandle *fil)
{
    if (!fil->OK())
        return false;
    
    std::string line;
    while ( fil->ReadLine(&line) && !line.empty() )
    {
        size_t pos = line.find_first_of("#\n\r");
        if ( pos != std::string::npos )
            line.erase(pos);

        if (line.empty())
            continue;
        
        std::vector<std::string> tokens = Stok::Split(line, " \t");
        
        if (tokens.empty())
            continue;
        
        if (!StriCmp(tokens[0], "v"))
        {
            _v.emplace_back( std::stof(tokens[1]), -std::stof(tokens[2]), std::stof(tokens[3]) );
        }
        else if (!StriCmp(tokens[0], "vt"))
        {
            if (tokens.size() == 3)
                _vt.emplace_back( std::stof(tokens[1]), std::stof(tokens[2]), 0.0 );
            else 
                _vt.emplace_back( std::stof(tokens[1]), std::stof(tokens[2]), std::stof(tokens[3]) );
        }
        else if (!StriCmp(tokens[0], "vn"))
        {
            _vn.emplace_back( std::stof(tokens[1]), std::stof(tokens[2]), std::stof(tokens[3]) );
        }
        else if (!StriCmp(tokens[0], "f"))
        {
            _f.emplace_back();            
            auto &f = _f.back();
            
            int numvtx = tokens.size() - 1;
            
            for (int32_t i = 1; i < (int32_t)tokens.size(); ++i )
                f[i - 1] = ParseFaceVT(tokens[i]);
            
            if (numvtx >= 3)
            {
                GFX::TRenderParams prm;
                if (_currentMtl)
                    prm = _currentMtl->GenParams();
                else
                    prm.Flags |= GFX::RFLAGS_FOG | GFX::RFLAGS_SHADED;

                GFX::TMesh *msh = FindMeshByRenderParams(&Meshes, prm);
                if (!msh)
                {
                    Meshes.emplace_back();
                    msh = &Meshes.back();
                    msh->Mat = prm;
                }
                
                uint32_t fid = msh->Vertexes.size();
                
                for(int j = 0; j < numvtx; ++j)
                {
                    FaceVT &ftx = f[j];
                    msh->Vertexes.emplace_back();
                    msh->Vertexes.back().Pos = _v.at(ftx.V - 1);
                    msh->Vertexes.back().TexCoordId = j;
                    msh->Vertexes.back().Color = prm.Color;

                    msh->BoundBox.Add( _v.at(ftx.V - 1) );

                    if (ftx.T > 0)
                        msh->Vertexes.back().TexCoord = tUtV(_vt.at(ftx.T - 1).x, 1.0 - _vt.at(ftx.T - 1).y);
                }

                for(int j = 2; j < numvtx; ++j)
                {
                    msh->Indixes.push_back(fid + 0);
                    msh->Indixes.push_back(fid + j);
                    msh->Indixes.push_back(fid + j - 1);
                }
            }
        }
        else if (!StriCmp(tokens[0], "mtllib"))
        {
            ParseMtl(tokens[1]);
        }
        else if (!StriCmp(tokens[0], "usemtl"))
        {
            _currentMtl = FindMtl(tokens[1]);
        }
    }
    return true;
}


NC_STACK_Obj3D::FaceVT NC_STACK_Obj3D::ParseFaceVT(const std::string &l)
{
    int s = 0;
    std::string t[3];
    for(auto c : l)
    {
        if (c == '/')
            s++;
        else
            t[s] += c;
    }
    
    int32_t v[3] {0, 0, 0};

    for(int i = 0; i < 3; ++i)
    {
        if (!t[i].empty())
            v[i] = std::stoi(t[i]);
    }
    
    return FaceVT( v[0], v[1], v[2] );
}
        

bool NC_STACK_Obj3D::ParseMtl(const std::string &filename)
{
    std::string fl = filename;
    if (!uaFileExist(fl))
    {
        if (fl.size() > 2 && fl[0] == '.' && fl[1] == '/')
            fl = fl.substr(2);
        
        if (!uaFileExist(fl))
        {
            size_t pos = fname.find_last_of("/\\:");
            if (pos != std::string::npos)
                fl = fname.substr(0, pos + 1) + fl;
            else
                fl = fname + fl;
        }
    }
    
    FSMgr::FileHandle fil = uaOpenFile(fl, "r");
    if (!fil.OK())
        return false;
    
    Mtl *current = NULL;
    
    std::string line;
    while ( fil.ReadLine(&line) && !line.empty() )
    {
        size_t pos = line.find_first_of("#\n\r");
        if ( pos != std::string::npos )
            line.erase(pos);

        if (line.empty())
            continue;
        
        std::vector<std::string> tokens = Stok::Split(line, " \t");
        
        if (tokens.empty())
            continue;
        
        if (!StriCmp(tokens[0], "newmtl"))
        {
            _mtls.emplace_back();
            current = &_mtls.back();
            current->name = tokens[1];
        }
        else if (!StriCmp(tokens[0], "Ns"))
        {
            
        }
        else if (!StriCmp(tokens[0], "Ka"))
        {
            
        }
        else if (!StriCmp(tokens[0], "Kd"))
        {
            if (current && tokens.size() >= 4)
            {
                current->diffuse[0] = std::stof(tokens[1]);
                current->diffuse[1] = std::stof(tokens[2]);
                current->diffuse[2] = std::stof(tokens[3]);
            }
        }
        else if (!StriCmp(tokens[0], "Ks"))
        {

        }
        else if (!StriCmp(tokens[0], "Ke"))
        {

        }
        else if (!StriCmp(tokens[0], "Ni"))
        {

        }
        else if (!StriCmp(tokens[0], "Ns"))
        {

        }
        else if (!StriCmp(tokens[0], "d"))
        {
            if (current)
                current->d = std::stof(tokens[1]);
        }
        else if (!StriCmp(tokens[0], "illum"))
        {

        }
        else if (!StriCmp(tokens[0], "map_Kd"))
        {
            if (current)
            {
                std::string txname = tokens[1];
                if (!uaFileExist(std::string("rsrc:") + txname))
                {
                    txname = "objects/" + tokens[1];
                    
                    if (!uaFileExist(std::string("rsrc:") + txname))
                        txname = "rsrcpool/" + tokens[1];
                }
                
                // Model-dir fallback used by the per-model asset pipeline
                // (LoadObjWithAttachments mounts the model's directory at
                // "rsrc:modelassets"); never fires without that mount
                if (!uaFileExist(std::string("rsrc:") + txname) &&
                    uaFileExist(std::string("rsrc:modelassets/") + tokens[1]))
                    txname = "modelassets/" + tokens[1];

                current->diffuseMap = Utils::ProxyLoadImage( {
                    {NC_STACK_rsrc::RSRC_ATT_NAME, txname},
                    {NC_STACK_rsrc::RSRC_ATT_TRYSHARED, (int32_t)2},
                    {NC_STACK_bitmap::BMD_ATT_CONVCOLOR, (int32_t)1}} );
                    
                if (current->diffuseMap)
                    current->diffuseMap->PrepareTexture();
                    
            }
        }
        else if (!StriCmp(tokens[0], "map_Ks"))
        {

        }
        else if (!StriCmp(tokens[0], "map_Ke"))
        {

        }
        else if (!StriCmp(tokens[0], "norm"))
        {

        }
        else if (!StriCmp(tokens[0], "norm"))
        {

        }
        else if (!StriCmp(tokens[0], "map_bump") || !StriCmp(tokens[0], "bump"))
        {

        }
        else if (!StriCmp(tokens[0], "map_d"))
        {

        }
        else if (!StriCmp(tokens[0], "Tr"))
        {

        }
    }
    
    return true;
}

NC_STACK_Obj3D::Mtl *NC_STACK_Obj3D::FindMtl(const std::string &mtlname)
{
    for(Mtl &m : _mtls)
    {
        if (!StriCmp(m.name, mtlname))
            return &m;
    }
    return NULL;
}




GFX::TRenderParams NC_STACK_Obj3D::Mtl::GenParams() const
{
    GFX::TRenderParams tmp;
    tmp.Flags = GFX::RFLAGS_SHADED | GFX::RFLAGS_FOG;
    
    if (diffuseMap)
    {
        tmp.Flags |= GFX::RFLAGS_TEXTURED;
        
        if (diffuseMap->IsDynamic())
        {
            tmp.Flags |= GFX::RFLAGS_DYNAMIC_TEXTURE;
            tmp.TexSource = diffuseMap;
        }
        else
            tmp.Tex = diffuseMap->GetBitmap();
    }
    
    tmp.Color = GFX::TGLColor(diffuse[0], diffuse[1], diffuse[2], d);
    
    if (d < 1.0)
        tmp.Flags |= GFX::RFLAGS_LUMTRACY;
    
    return tmp;
}


namespace
{
    struct ExportObjMatInfo
    {
        std::string name;
        std::string texture;    // png file name referenced by map_Kd ("" = none)
        std::string origTexture; // original rsrc name ("" = none/unknown)
    };
    
    std::string ExportObj_TexFileName(const std::string &rsrcName)
    {
        size_t pos = rsrcName.find_last_of("/\\:");
        if (pos != std::string::npos)
            return rsrcName.substr(pos + 1);
        return rsrcName;
    }
    
    std::string ExportObj_JsonEscape(const std::string &str)
    {
        std::string out;
        out.reserve(str.size());
        
        for (char c : str)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if ((unsigned char)c < 0x20)
                        out += fmt::sprintf("\\u%04x", (unsigned char)c);
                    else
                        out += c;
            }
        }
        
        return out;
    }
}

bool NC_STACK_Obj3D::ExportObj(NC_STACK_base *base, const std::string &fname, const std::string &sourceFile)
{
    if (!base)
        return false;
    
    if (base->Meshes.empty())
        return false;
    
    // Directory part of 'fname' - texture pngs are written next to the .obj
    std::string texDir;
    size_t dpos = fname.find_last_of("/\\");
    if (dpos != std::string::npos)
        texDir = fname.substr(0, dpos);
    
    FSMgr::FileHandle mtl = uaOpenFile(fname + ".mtl", "w");
    FSMgr::FileHandle obj = uaOpenFile(fname, "w");
    
    if (!obj.OK() || !mtl.OK())
        return false;
    
    obj.puts(std::string("mtllib ") + fname + ".mtl\n\n");
    
    // Dedupe textures by their own rsrc name within this model:
    // rsrc name -> png file name, so meshes sharing one texture write one file
    std::map<std::string, std::string> texNames;
    std::set<std::string> usedPngNames;
    std::vector<ExportObjMatInfo> matInfos;
    
    int vid = 1;
    int tvid = 1;
    
    int matID = 0;
    for( GFX::TMesh &m : base->Meshes )
    {
        mtl.printf("newmtl Mat%d\n", matID);
        mtl.printf("Kd %f %f %f\n", m.Mat.Color.r, m.Mat.Color.g, m.Mat.Color.b);
        mtl.puts("\
Ka 1.000000 1.000000 1.000000\n\
Ks 0.500000 0.500000 0.500000\n\
Ke 0.000000 0.000000 0.000000\n\
Ni 1.450000\n");
        mtl.printf("d %f\n", m.Mat.Color.a);
        mtl.puts("illum 2\n");
        
        ExportObjMatInfo info;
        info.name = fmt::sprintf("Mat%d", matID);
        
        if (m.Mat.TexSource || m.Mat.Tex)
        {
            std::string pngName;
            
            if (m.Mat.TexSource)
            {
                std::string rsrcName = m.Mat.TexSource->getRsrc_name();
                
                // rsrc names of embedded resources may carry trailing NULs
                // (EMRS readStr quirk); trim at the first NUL
                size_t nul = rsrcName.find('\0');
                if (nul != std::string::npos)
                    rsrcName.resize(nul);
                
                info.origTexture = rsrcName;
                
                auto it = texNames.find(rsrcName);
                if (it != texNames.end())
                {
                    pngName = it->second;
                }
                else
                {
                    // Keep the full resource name including its own extension,
                    // e.g. "HUBI.ILBM" -> "HUBI.ILBM.png" (case preserved)
                    std::string nm = ExportObj_TexFileName(rsrcName);
                    if (!nm.empty())
                        pngName = nm + ".png";
                    
                    if (pngName.empty() || usedPngNames.count(pngName))
                        pngName = fmt::sprintf("Tex%d.png", matID); // fallback (no/ambiguous name)
                    
                    usedPngNames.insert(pngName);
                    texNames[rsrcName] = pngName;
                    
                    std::string pngPath = texDir.empty() ? pngName : texDir + "/" + pngName;
                    
                    bool saved = false;
                    if (m.Mat.Tex)
                        saved = NC_STACK_image::SavePng(m.Mat.Tex, pngPath);
                    else
                        saved = NC_STACK_image::SavePng(m.Mat.TexSource->GetBitmap(), pngPath);
                    
                    if (!saved)
                        fprintf(stderr, "ExportObj: warning: failed to save texture '%s' (material %s)\n",
                                pngPath.c_str(), info.name.c_str());
                }
            }
            else
            {
                // ResBitmap without rsrc source - no name available, numbered fallback
                pngName = fmt::sprintf("Tex%d.png", matID);
                usedPngNames.insert(pngName);
                
                std::string pngPath = texDir.empty() ? pngName : texDir + "/" + pngName;
                if (m.Mat.Tex)
                    NC_STACK_image::SavePng(m.Mat.Tex, pngPath);
            }
            
            info.texture = pngName;
            mtl.printf("map_Kd %s\n\n", pngName.c_str());
        }
        else
            mtl.puts("\n");
        
        matInfos.push_back(info);
        
        for(GFX::TVertex &v : m.Vertexes)
            obj.printf("v %f %f %f\n", v.Pos.x, -v.Pos.y, v.Pos.z);
        
        if (m.Mat.Flags & GFX::RFLAGS_TEXTURED)
        {
            for(GFX::TVertex &v : m.Vertexes)
                obj.printf("vt %f %f\n", v.TexCoord.tu, 1.0 - v.TexCoord.tv);
        }
        
        obj.printf("usemtl Mat%d\n", matID);
        
        for (uint32_t i = 0; i < m.Indixes.size(); i += 3)
        {
            if (m.Mat.Flags & GFX::RFLAGS_TEXTURED)
                obj.printf("f %d/%d %d/%d %d/%d\n", vid + m.Indixes[i], tvid + m.Indixes[i],
                        vid + m.Indixes[i + 2], tvid + m.Indixes[i + 2],
                        vid + m.Indixes[i + 1], tvid + m.Indixes[i + 1]);
            else
                obj.printf("f %d %d %d\n", vid + m.Indixes[i],
                        vid + m.Indixes[i + 2],
                        vid + m.Indixes[i + 1]);
        }
        
        vid += m.Vertexes.size();  
        if (m.Mat.Flags & GFX::RFLAGS_TEXTURED)
            tvid += m.Vertexes.size();  
        ++matID;
    }
    
    // Sidecar json with per-material texture info in MTL order.
    // Written via FSMgr::iDir::openFileAlloc directly, so the '.mat.json'
    // extension is NOT truncated by correctSeparatorAndExt (>3 chars).
    FSMgr::FileHandle *json = FSMgr::iDir::openFileAlloc(fname + ".mat.json", "w");
    if (json)
    {
        json->puts("{\n");
        json->printf("  \"model\": \"%s\",\n", ExportObj_JsonEscape(base->getName()).c_str());
        json->printf("  \"source_file\": \"%s\",\n", ExportObj_JsonEscape(sourceFile).c_str());
        json->puts("  \"materials\": [\n");
        
        for (size_t i = 0; i < matInfos.size(); ++i)
        {
            const ExportObjMatInfo &info = matInfos[i];
            
            std::string texture = info.texture.empty() ? std::string("null") :
                    "\"" + ExportObj_JsonEscape(info.texture) + "\"";
            std::string orig = info.origTexture.empty() ? std::string("null") :
                    "\"" + ExportObj_JsonEscape(info.origTexture) + "\"";
            
            json->printf("    {\"name\": \"%s\", \"texture\": %s, \"orig_texture\": %s}%s\n",
                    ExportObj_JsonEscape(info.name).c_str(), texture.c_str(), orig.c_str(),
                    (i + 1 < matInfos.size()) ? "," : "");
        }
        
        json->puts("  ]\n}\n");
        delete json;
    }
    else
        fprintf(stderr, "ExportObj: warning: failed to write '%s.mat.json'\n", fname.c_str());
    
    return true;
}
// ---------------------------------------------------------------------------
// Per-model asset pipeline (.uattach sidecar loader)
// ---------------------------------------------------------------------------

namespace
{
    // ---- minimal JSON -----------------------------------------------------
    struct UAtJson
    {
        enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
        
        Type type = NUL;
        bool b = false;
        double num = 0.0;
        std::string str;
        std::vector<UAtJson> arr;
        std::vector<std::pair<std::string, UAtJson> > obj;
        
        const UAtJson *Get(const char *key) const
        {
            if (type != OBJ)
                return NULL;
            for (const auto &kv : obj)
                if (kv.first == key)
                    return &kv.second;
            return NULL;
        }
        
        std::string GetString(const char *key, const std::string &def = std::string()) const
        {
            const UAtJson *v = Get(key);
            return (v && v->type == STR) ? v->str : def;
        }
        
        int GetInt(const char *key, int def = 0) const
        {
            const UAtJson *v = Get(key);
            return (v && v->type == NUM) ? (int)v->num : def;
        }
        
        bool GetBool(const char *key, bool def = false) const
        {
            const UAtJson *v = Get(key);
            return v ? (v->type == BOOL ? v->b : def) : def;
        }
    };
    
    class UAtJsonParser
    {
    public:
        UAtJsonParser(const char *b, const char *e) : p(b), e(e) {}
        
        bool Parse(UAtJson *out)
        {
            SkipWs();
            if (!Value(out))
                return false;
            SkipWs();
            return p == e;
        }
        
    private:
        const char *p;
        const char *e;
        int depth = 0;
        
        void SkipWs()
        {
            while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                ++p;
        }
        
        bool Consume(char c)
        {
            if (p < e && *p == c) { ++p; return true; }
            return false;
        }
        
        bool Value(UAtJson *out)
        {
            if (p >= e || depth > 64)
                return false;
            
            if (++depth > 64)
                return false;
            
            bool ok = false;
            
            switch (*p)
            {
                case '{':
                    ok = Object(out);
                    break;
                case '[':
                    ok = Array(out);
                    break;
                case '"':
                    ok = String(out);
                    break;
                case 't':
                    ok = Lit("true");
                    if (ok) { out->type = UAtJson::BOOL; out->b = true; }
                    break;
                case 'f':
                    ok = Lit("false");
                    if (ok) { out->type = UAtJson::BOOL; out->b = false; }
                    break;
                case 'n':
                    ok = Lit("null");
                    if (ok) out->type = UAtJson::NUL;
                    break;
                default:
                    ok = Number(out);
                    break;
            }
            
            --depth;
            return ok;
        }
        
        bool Lit(const char *word)
        {
            size_t n = strlen(word);
            if (e - p < (ptrdiff_t)n || strncmp(p, word, n) != 0)
                return false;
            p += n;
            return true;
        }
        
        bool Object(UAtJson *out)
        {
            out->type = UAtJson::OBJ;
            ++p;
            SkipWs();
            if (Consume('}'))
                return true;
            
            while (p < e)
            {
                SkipWs();
                UAtJson key;
                if (!String(&key))
                    return false;
                SkipWs();
                if (!Consume(':'))
                    return false;
                SkipWs();
                
                UAtJson val;
                if (!Value(&val))
                    return false;
                
                out->obj.push_back({key.str, val});
                
                SkipWs();
                if (Consume(','))
                    continue;
                if (Consume('}'))
                    return true;
                return false;
            }
            return false;
        }
        
        bool Array(UAtJson *out)
        {
            out->type = UAtJson::ARR;
            ++p;
            SkipWs();
            if (Consume(']'))
                return true;
            
            while (p < e)
            {
                SkipWs();
                UAtJson val;
                if (!Value(&val))
                    return false;
                out->arr.push_back(val);
                
                SkipWs();
                if (Consume(','))
                    continue;
                if (Consume(']'))
                    return true;
                return false;
            }
            return false;
        }
        
        bool String(UAtJson *out)
        {
            out->type = UAtJson::STR;
            ++p;
            while (p < e)
            {
                char c = *p;
                if (c == '"') { ++p; return true; }
                if (c == '\\')
                {
                    ++p;
                    if (p >= e)
                        return false;
                    char esc = *p;
                    ++p;
                    switch (esc)
                    {
                        case '"':  out->str += '"'; break;
                        case '\\': out->str += '\\'; break;
                        case '/':  out->str += '/'; break;
                        case 'b':  out->str += '\b'; break;
                        case 'f':  out->str += '\f'; break;
                        case 'n':  out->str += '\n'; break;
                        case 'r':  out->str += '\r'; break;
                        case 't':  out->str += '\t'; break;
                        case 'u':
                            if (e - p < 4)
                                return false;
                            p += 4;
                            out->str += '?';
                            break;
                        default:
                            return false;
                    }
                }
                else if ((unsigned char)c < 0x20)
                {
                    return false;
                }
                else
                {
                    out->str += c;
                    ++p;
                }
            }
            return false;
        }
        
        bool Number(UAtJson *out)
        {
            out->type = UAtJson::NUM;
            const char *start = p;
            if (p < e && (*p == '-' || *p == '+')) ++p;
            while (p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                             *p == '-' || *p == '+'))
                ++p;
            if (p == start)
                return false;
            out->num = strtod(start, NULL);
            return true;
        }
    };
    
    // ---- small path/name helpers ------------------------------------------
    std::string UAtLower(std::string s)
    {
        for (char &c : s)
            c = (char)tolower((unsigned char)c);
        return s;
    }
    
    std::string UAtBaseName(const std::string &s)
    {
        size_t pos = s.find_last_of("/\\:");
        return (pos != std::string::npos) ? s.substr(pos + 1) : s;
    }
    
    std::string UAtDirName(const std::string &s)
    {
        size_t pos = s.find_last_of("/\\");
        if (pos == std::string::npos)
            return std::string();
        if (pos == 0)
            return "/";
        return s.substr(0, pos);
    }
    
    std::string UAtJoin(const std::string &dir, const std::string &name)
    {
        if (dir.empty() || dir == "/")
            return (dir.empty() ? std::string() : dir) + name;
        return dir + "/" + name;
    }
    
    // The game keeps shared pool assets (anm payloads, pool pngs) in
    // <rsrc>/rsrcpool ("data/set1/rsrcpool" through the prefix chain), the
    // tool keeps them flat in the model dir. Pool lookups try the model dir
    // first (tool layout, and per-model overrides), then the rsrcpool dir.
    std::string UAtPoolDir()
    {
        std::string base = Common::Env.ApplyPrefix(Common::Env.GetPrefix("rsrc"));
        if (base.empty())
            return std::string("rsrcpool");
        return base + "/rsrcpool";
    }
    
    // "Skeleton/X.sklt" -> "X.anm", "BRGBLTZ.ANM" -> "BRGBLTZ.anm"
    std::string UAtStemWithExt(const std::string &s, const char *ext)
    {
        std::string b = UAtBaseName(s);
        size_t pos = b.rfind('.');
        if (pos != std::string::npos && pos != 0)
            b.resize(pos);
        return b + ext;
    }
    
    // ---- per-model context --------------------------------------------------
    struct UAtShadow
    {
        rsrc *node = NULL;
        bool isAnm = false;
        
        // Release the loader's keepalive reference. The resource stays alive
        // (and visible in the public list) as long as any object that resolved
        // it by name still holds its reference; when the last reference is
        // released the node is freed and removed, exactly like a regular
        // rsrc_deinit would do.
        void ReleaseKeepalive()
        {
            if (!node)
                return;
            
            RSRCList *lst = NC_STACK_rsrc::getRsrc_sharedList();
            
if (--node->ref_cnt <= 0)
            {
                if (isAnm)
                {
                    ResBmpAnm *pay = (ResBmpAnm *)node->data;
                    delete pay;         // deletes the bound sheet bitmaps
                }
                else
                {
                    ResBitmap *d = (ResBitmap *)node->data;
                    if (d)
                    {
                        if (d->hwTex)
                            GFX::Engine.FreeTexture(d);
                        if (d->swTex)
                            SDL_FreeSurface(d->swTex);
                        if (d->palette)
                            delete d->palette;
                        delete d;
                    }
                }
                node->data = NULL;
                lst->remove(node);
                delete node;
            }
            
            node = NULL;
        }
    };
    
    struct UAtAnmInfo
    {
        bool present = false;
        std::string origAnm;
        std::string file;
    };
    
    struct UAtAttachment
    {
        int index = 0;
        std::string cls;
        std::string file;
        UAtAnmInfo bmpanim;
        UAtAnmInfo bmpanimTracy;
        std::vector<std::string> origTextures;
    };
    
    struct UAtSidecar
    {
        std::string model;
        std::string skeleton;
        std::map<std::string, std::string> anmTex;   // lower(sheet name) -> per-model png file
        std::vector<UAtAttachment> atts;
    };
    
    struct UAtCtx
    {
        std::string dir;        // vpath of the directory holding the model files
        UAtSidecar side;
        std::vector<UAtShadow> shadows;
        std::map<std::string, bool> namesDone;               // lower(name) -> shadow created
        bool ok = true;
    };
    
    // Mount 'dir' (the directory holding the model files) under the rsrc
    // prefix as "modelassets", so image loads by name ("rsrc:modelassets/...")
    // resolve into the model's own files.
    void UAtMountAssetsDir(const std::string &dir)
    {
        if (dir.empty())
            return;
        
        std::string target = Common::Env.ApplyPrefix(Common::Env.GetPrefix("rsrc"));
        if (target.empty())
            target = "modelassets";
        else
            target += "/modelassets";
        
        if (FSMgr::iDir::findNode(target))
            return;     // already mounted
        
        FSMgr::iNode *tn = FSMgr::iDir::findNode(
            Common::Env.ApplyPrefix(Common::Env.GetPrefix("rsrc")));
        
        if (!tn || tn->getType() != FSMgr::iNode::NTYPE_DIR)
            return;
        
        // the scanned source dir must be addressed by its disk path
        FSMgr::iNode *srcNode = FSMgr::iDir::findNode(dir);
        if (!srcNode)
            return;
        std::string diskPath = srcNode->getPath();
        
        // Guarded downcast: iDir adds no virtuals over iNode
        FSMgr::iDir *tdir = (FSMgr::iDir *)tn;
        
        tdir->addNode(new FSMgr::iDir("modelassets", diskPath));
        
        // Re-create the node as a scanned directory so files can be read back
        FSMgr::iDir::replacePath(target, diskPath);
    }
    
    bool UAtReadAnmPayload(FSMgr::iFileHandle *fil,
                           std::vector<std::string> *names,
                           std::vector< std::vector<tUtV> > *coords,
                           std::vector<BAnmFrame> *frames)
    {
        // skip the FORM VANM / DATA wrapper if present (like the engine's
        // LoadFromFile does for wrapped files)
        uint8_t magic[4];
        if (fil->read(magic, 4) != 4)
            return false;
        
        if (magic[0] == 'F' && magic[1] == 'O' && magic[2] == 'R' && magic[3] == 'M')
        {
            // FORM size(4) + "VANM"(4) + "DATA"(4) + DATA size(4)
            uint8_t skip[16];
            if (fil->read(skip, 16) != 16)
                return false;
        }
        else
        {
            // raw payload file
            if (fil->seek(-4, SEEK_CUR) != 0)
                return false;
        }
        
        int16_t clsSz = fil->readS16B();
        if (fil->readErr() || clsSz <= 0)
            return false;
        
        std::vector<char> clsBuf(clsSz);
        if (fil->read(clsBuf.data(), clsSz) != (size_t)clsSz)
            return false;
        
        int16_t namesSz = fil->readS16B();
        if (fil->readErr() || namesSz < 0)
            return false;
        
        std::vector<char> namesBuf(namesSz > 0 ? namesSz : 1);
        if (namesSz > 0 && fil->read(namesBuf.data(), namesSz) != (size_t)namesSz)
            return false;
        
        *names = Stok::Split(std::string(namesBuf.data(), namesSz), std::string("\0", 1));
        if (names->empty())
            return false;
        
        // NOTE: the first count is the TOTAL number of coordinate pairs, not
        // the number of groups (the engine reader counts groups by skipping
        // 'i += ttmp' entries)
        int16_t totalCoords = fil->readS16B();
        if (fil->readErr() || totalCoords < 0)
            return false;
        
        int consumed = 0;
        while (consumed < totalCoords)
        {
            int16_t n = fil->readS16B();
            if (fil->readErr() || n < 0)
                return false;
            
            coords->emplace_back();
            std::vector<tUtV> &group = coords->back();
            group.resize(n);
            
            for (int16_t j = 0; j < n; ++j)
            {
                uint8_t uv[2];
                if (fil->read(uv, 2) != 2)
                    return false;
                group[j].tu = (float)uv[0] / 256.0;
                group[j].tv = (float)uv[1] / 256.0;
            }
            
            consumed += n;
            
            if (n == 0 && coords->size() > 65535)
                return false;   // malformed, would loop forever
        }
        
        int16_t frameCnt = fil->readS16B();
        if (fil->readErr())
            return false;
        
        frames->resize(frameCnt);
        for (int16_t i = 0; i < frameCnt; ++i)
        {
            BAnmFrame &f = frames->at(i);
            f.FrameTime = fil->readS32B();
            f.FrameID = fil->readS16B();
            f.TexCoordsID = fil->readS16B();
            
            if (fil->readErr())
                return false;
            if (f.FrameID < 0 || f.FrameID >= (int16_t)names->size())
                return false;
            if (f.TexCoordsID < 0 || f.TexCoordsID >= (int16_t)coords->size())
                return false;
        }
        
        return true;
    }
    
    // Load a sheet bitmap from the model's own pngs. A fresh object is
    // created for every call: the caller (ResBmpAnm) takes ownership of it
    // and deletes it in its destructor, so the same sheet must never be
    // shared between two ANM payloads.
    NC_STACK_bitmap *UAtGetSheetBitmap(UAtCtx *ctx, const std::string &sheetName)
    {
        // resolve the png file: sidecar map first, then pool naming
        std::string pngFile;
        auto it = ctx->side.anmTex.find(UAtLower(sheetName));
        if (it != ctx->side.anmTex.end())
            pngFile = it->second;
        else
            pngFile = UAtBaseName(sheetName) + ".png";
        
        NC_STACK_bitmap *bm = Utils::ProxyLoadImage( {
            {NC_STACK_rsrc::RSRC_ATT_NAME, std::string("modelassets/") + pngFile},
            {NC_STACK_rsrc::RSRC_ATT_TRYSHARED, (int32_t)2},
            {NC_STACK_bitmap::BMD_ATT_CONVCOLOR, (int32_t)1}} );
        
        if (!bm)
        {
            // not a per-model sheet -> shared pool png (like the engine's
            // by-name loads, which resolve through "rsrc:rsrcpool/<name>")
            bm = Utils::ProxyLoadImage( {
                {NC_STACK_rsrc::RSRC_ATT_NAME, std::string("rsrcpool/") + pngFile},
                {NC_STACK_rsrc::RSRC_ATT_TRYSHARED, (int32_t)2},
                {NC_STACK_bitmap::BMD_ATT_CONVCOLOR, (int32_t)1}} );
        }
        
        if (!bm)
        {
            fprintf(stderr, "UATTACH %s: failed to load sheet '%s' from %s\n",
                    ctx->side.model.c_str(), sheetName.c_str(), pngFile.c_str());
            ctx->ok = false;
        }
        
        return bm;
    }
    
    // Register a bmpanim shadow resource: payload from the model's own .anm
    // file (per-model file if the sidecar has one, pool file otherwise),
    // sheets bound from the model's own pngs.
    bool UAtShadowAnm(UAtCtx *ctx, const std::string &origName)
    {
        std::string key = UAtLower(origName);
        if (ctx->namesDone.count(key))
            return ctx->ok;
        ctx->namesDone[key] = true;
        
        // resolve the .anm file: per-model file from the sidecar, pool file
        // (same directory, pool naming) otherwise
        std::string anmFile;
        
        for (const UAtAttachment &att : ctx->side.atts)
        {
            if (att.bmpanim.present &&
                !StriCmp(att.bmpanim.origAnm, origName) &&
                !att.bmpanim.file.empty())
                anmFile = att.bmpanim.file;
            
            if (att.bmpanimTracy.present &&
                !StriCmp(att.bmpanimTracy.origAnm, origName) &&
                !att.bmpanimTracy.file.empty())
                anmFile = att.bmpanimTracy.file;
        }
        
        if (anmFile.empty())
            anmFile = UAtStemWithExt(origName, ".anm");
        
        std::string vpath = UAtJoin(ctx->dir, anmFile);
        
        FSMgr::FileHandle *fh = FSMgr::iDir::openFileAlloc(vpath, "rb");
        if (!fh)
        {
            // not in the model dir -> shared pool ("rsrc:rsrcpool/<name>",
            // like the engine's NC_STACK_bmpanim::LoadFromFile)
            std::string poolVPath = UAtJoin(UAtPoolDir(), anmFile);
            fh = FSMgr::iDir::openFileAlloc(poolVPath, "rb");
            if (fh)
                vpath = poolVPath;
        }
        
        if (!fh)
        {
            fprintf(stderr, "UATTACH %s: ANM payload file missing: %s\n",
                    ctx->side.model.c_str(), vpath.c_str());
            ctx->ok = false;
            return false;
        }
        
        std::vector<std::string> names;
        std::vector< std::vector<tUtV> > coords;
        std::vector<BAnmFrame> frames;
        
        bool readOK = UAtReadAnmPayload(fh, &names, &coords, &frames);
        delete fh;
        
        if (!readOK)
        {
            fprintf(stderr, "UATTACH %s: failed to read ANM payload %s\n",
                    ctx->side.model.c_str(), vpath.c_str());
            ctx->ok = false;
            return false;
        }
        
        ResBmpAnm *pay = new ResBmpAnm;
        pay->ClassName = "ilbm.class";
        pay->BitmapNames = names;
        pay->Bitmaps.resize(names.size());
        pay->TexCoords = coords;
        
        bool bound = true;
        for (size_t i = 0; i < names.size(); ++i)
        {
            NC_STACK_bitmap *sheet = UAtGetSheetBitmap(ctx, names[i]);
            if (!sheet)
            {
                bound = false;
                break;
            }
            pay->Bitmaps[i] = sheet;
        }
        
        if (bound)
        {
            pay->FrameData.resize(frames.size());
            for (size_t i = 0; i < frames.size(); ++i)
            {
                BAnmFrameCache &out = pay->FrameData[i];
                out.FrameTime = frames[i].FrameTime;
                out.FrameID = frames[i].FrameID;
                out.TexCoordsID = frames[i].TexCoordsID;
                out.pBitmap = pay->Bitmaps.at(out.FrameID);
                out.pTexCoords = &pay->TexCoords.at(out.TexCoordsID);
            }
            
            rsrc *node = new rsrc(origName, true);
            node->ref_cnt = 1;
            node->data = pay;
            NC_STACK_rsrc::getRsrc_sharedList()->push_front(node);
            
UAtShadow sh;
            sh.node = node;
            sh.isAnm = true;
            ctx->shadows.push_back(sh);
            return true;
        }
        
        delete pay;
        return false;
    }
    
    // Register a static texture shadow resource (loaded from the model's own
    // png, pool png as fallback)
    bool UAtShadowTexture(UAtCtx *ctx, const std::string &origName)
    {
        std::string key = UAtLower(origName);
        if (ctx->namesDone.count(key))
            return ctx->ok;
        ctx->namesDone[key] = true;
        
        std::string base = UAtBaseName(origName);
        
        std::string pngFile = ctx->side.model + "_" + base + ".png";
        
        ResBitmap *bmp = NC_STACK_image::LoadImageFromPath(
            UAtJoin(ctx->dir, pngFile), true);
        
        if (!bmp)
        {
            // pool fallback (e.g. stage textures not exported per-model)
            pngFile = base + ".png";
            bmp = NC_STACK_image::LoadImageFromPath(UAtJoin(ctx->dir, pngFile), true);
            
            if (!bmp)
            {
                // model dir doesn't carry it -> shared pool dir
                bmp = NC_STACK_image::LoadImageFromPath(
                    UAtJoin(UAtPoolDir(), pngFile), true);
            }
        }
        
        if (!bmp)
        {
            fprintf(stderr, "UATTACH %s: texture file missing for '%s'\n",
                    ctx->side.model.c_str(), origName.c_str());
            ctx->ok = false;
            return false;
        }
        
        rsrc *node = new rsrc(origName, true);
        node->ref_cnt = 1;
        node->data = bmp;
        NC_STACK_rsrc::getRsrc_sharedList()->push_front(node);
        
UAtShadow sh;
        sh.node = node;
        sh.isAnm = false;
        ctx->shadows.push_back(sh);
        return true;
    }
    
    // Pre-scan a standalone .ade file for resource names it will resolve by
    // name during loading: BANI STRC names (bmpanims) and CIBO NAM2 names
    // (static textures) - including particle life stages.
    bool UAtScanAdeNames(const std::string &vpath, std::set<std::string> *anmNames,
                         std::set<std::string> *texNames)
    {
        IFFile f = IFFile::UAOpenIFFile(vpath, "rb");
        if (!f.OK())
            return false;
        
        // enter the file-level FORM (ADES)
        if (f.parse())
            return false;
        
        int depth = 1;
        
        while (depth > 0)
        {
            int res = f.parse();
            
            if (res == IFFile::IFF_ERR_EOC)
            {
                depth--;
                continue;
            }
            
            if (res)
                return false;
            
            const IFFile::Context &chunk = f.GetCurrentChunk();
            
if (chunk.Is(TAG_FORM))
            {
                depth++;
                
                if (chunk.TAG_EXTENSION == TAG_BANI)
                {
                    depth--;
                    
                    // find STRC inside
                    while (1)
                    {
                        int v2 = f.parse();
if (v2 == IFFile::IFF_ERR_EOC)
                            break;
                        if (v2)
                            return false;
                        
                        if (f.GetCurrentChunk().Is(TAG_STRC))
                        {
                            f.readS16B();               // version
                            int16_t ofs = f.readS16B(); // name offset in the chunk
                            f.readS16B();               // animType
                            
                            // the payload read after the 6 header bytes starts
                            // at chunk offset 6, so the name lives at
                            // buf[offset - 6] like the engine reader does
                            std::vector<char> buf;
                            int rest = f.GetCurrentChunk().TAG_SIZE - 6;
                            if (rest > 0)
                            {
                                buf.resize(rest);
                                f.read(buf.data(), rest);
                            }
                            
if (ofs >= 6 && (size_t)(ofs - 6) < buf.size())
                            {
                                // the name may be followed by NUL padding
                                std::string nm(&buf[ofs - 6]);
                                size_t nul = nm.find('\0');
                                if (nul != std::string::npos)
                                    nm.resize(nul);
                                if (!nm.empty())
                                    anmNames->insert(nm);
                            }
                            
                            f.parse();
                        }
                        else
                        {
                            f.skipChunk();
                        }
                    }
                }
                else if (chunk.TAG_EXTENSION == TAG_CIBO)
                {
                    depth--;
                    
                    while (1)
                    {
                        int v2 = f.parse();
                        if (v2 == IFFile::IFF_ERR_EOC)
                            break;
                        if (v2)
                            return false;
                        
                        if (f.GetCurrentChunk().Is(TAG_NAM2))
                        {
                            std::string nm = f.readStr(256);
                            // the name may be followed by NUL padding
                            size_t nul = nm.find('\0');
                            if (nul != std::string::npos)
                                nm.resize(nul);
                            if (!nm.empty())
                                texNames->insert(nm);
                            f.parse();
                        }
                        else
                        {
                            f.skipChunk();
                        }
                    }
                }
            }
            else
            {
                f.skipChunk();
            }
        }
        
        return true;
    }
    
    // Load the sidecar skeleton (.skl: FORM SKLT with POO2/SEN2/POL2 chunks)
    NC_STACK_skeleton *UAtLoadSkeleton(const std::string &vpath)
    {
        FSMgr::FileHandle fh = FSMgr::iDir::openFile(vpath, "rb");
        if (!fh.OK())
            return NULL;
        
        IFFile f(std::move(fh));
        
        if (f.parse())
            return NULL;
        
        if (!f.GetCurrentChunk().Is(TAG_FORM, TAG_SKLT))
            return NULL;
        
        UAskeleton::Data dat;
        
        while (1)
        {
            int res = f.parse();
            
            if (res == IFFile::IFF_ERR_EOC)
                break;
            
            if (res)
                return NULL;
            
            const IFFile::Context &chunk = f.GetCurrentChunk();
            
            if (chunk.Is(TAG_POO2))
            {
                int n = chunk.TAG_SIZE / 12;
                dat.POO.resize(n);
                for (int i = 0; i < n; ++i)
                {
                    dat.POO[i].flags = 0;
                    dat.POO[i].x = f.readFloatB();
                    dat.POO[i].y = f.readFloatB();
                    dat.POO[i].z = f.readFloatB();
                }
                f.parse();
            }
            else if (chunk.Is(TAG_SEN2))
            {
                int n = chunk.TAG_SIZE / 12;
                dat.SEN.resize(n);
                for (int i = 0; i < n; ++i)
                {
                    dat.SEN[i].flags = 0;
                    dat.SEN[i].x = f.readFloatB();
                    dat.SEN[i].y = f.readFloatB();
                    dat.SEN[i].z = f.readFloatB();
                }
                f.parse();
            }
            else if (chunk.Is(TAG_POL2))
            {
                int cnt = f.readS32B();
                dat.polygons.resize(cnt);
                for (int i = 0; i < cnt; ++i)
                {
                    uint16_t numv = f.readU16B();
                    dat.polygons[i].num_vertices = numv;
                    for (int j = 0; j < numv; ++j)
                        dat.polygons[i].v[j] = f.readU16B();
                }
                f.parse();
            }
            else
            {
                f.skipChunk();
            }
        }
        
        if (dat.POO.empty())
            return NULL;
        
        IDVList stak {
            {NC_STACK_rsrc::RSRC_ATT_NAME, std::string("uattach_skl")},
            {NC_STACK_rsrc::RSRC_ATT_TRYSHARED, (int32_t)2},
            {NC_STACK_skeleton::SKEL_ATT_POINTSCNT, (int32_t)dat.POO.size()}};
        
        if (!dat.polygons.empty())
        {
            stak.Add(NC_STACK_skeleton::SKEL_ATT_POLYCNT, (int32_t)dat.polygons.size());
            stak.Add(NC_STACK_skeleton::SKEL_ATT_POLYPNTCNT, (int32_t)1);
        }
        
        NC_STACK_skeleton *skel = Nucleus::CInit<NC_STACK_skeleton>(stak);
        if (!skel)
            return NULL;
        
        *skel->_resData = dat;
        skel->_resData->tformedVertex.resize(dat.POO.size());
        
        // recompute polygon planes like the engine's skeleton loader does
        for (size_t i = 0; i < skel->_resData->polygons.size(); ++i)
        {
            UAskeleton::Polygon &pol = skel->_resData->polygons[i];
            const std::vector<UAskeleton::Vertex> &POO = skel->_resData->POO;
            
            if (pol.num_vertices >= 3)
            {
                float dx1 = POO[pol.v[1]].x - POO[pol.v[0]].x;
                float dy1 = POO[pol.v[1]].y - POO[pol.v[0]].y;
                float dz1 = POO[pol.v[1]].z - POO[pol.v[0]].z;
                
                float dx2 = POO[pol.v[2]].x - POO[pol.v[1]].x;
                float dy2 = POO[pol.v[2]].y - POO[pol.v[1]].y;
                float dz2 = POO[pol.v[2]].z - POO[pol.v[1]].z;
                
                float zx = dz1 * dx2 - dx1 * dz2;
                float zy = dy1 * dz2 - dy2 * dz1;
                float xy = dx1 * dy2 - dx2 * dy1;
                
                float sqwr = sqrt(zx * zx + zy * zy + xy * xy);
                
                float dzx = 0.0, dzy = 0.0, dxy = 0.0;
                
                if (sqwr != 0.0)
                {
                    dzy = zy / sqwr;
                    dzx = zx / sqwr;
                    dxy = xy / sqwr;
                }
                
                pol.A = dzy;
                pol.B = dzx;
                pol.C = dxy;
                pol.D = -(dzy * POO[pol.v[0]].x + dzx * POO[pol.v[0]].y + dxy * POO[pol.v[0]].z);
            }
        }
        
        return skel;
    }
    
    bool UAtParseSidecar(const std::string &text, UAtSidecar *out)
    {
        UAtJson root;
        UAtJsonParser parser(text.c_str(), text.c_str() + text.size());
        
        if (!parser.Parse(&root) || root.type != UAtJson::OBJ)
            return false;
        
        out->model = root.GetString("model");
        out->skeleton = root.GetString("skeleton");
        
        if (out->model.empty() || out->skeleton.empty())
            return false;
        
        if (const UAtJson *maps = root.Get("anm_textures"))
        {
            for (const auto &kv : maps->obj)
                out->anmTex[UAtLower(kv.first)] = kv.second.type == UAtJson::STR ? kv.second.str : std::string();
        }
        
        if (const UAtJson *atts = root.Get("attachments"))
        {
            for (const UAtJson &av : atts->arr)
            {
                UAtAttachment att;
                att.index = av.GetInt("index", (int)out->atts.size());
                att.cls = av.GetString("class");
                att.file = av.GetString("file");
                
                auto readAnmInfo = [&av](const char *key, UAtAnmInfo *info) {
                    if (const UAtJson *v = av.Get(key))
                    {
                        if (v->type == UAtJson::OBJ)
                        {
                            info->present = true;
                            info->origAnm = v->GetString("orig_anm");
                            info->file = v->GetString("file");
                        }
                    }
                };
                
                readAnmInfo("bmpanim", &att.bmpanim);
                readAnmInfo("bmpanim_tracy", &att.bmpanimTracy);
                
                if (const UAtJson *ot = av.Get("orig_textures"))
                    for (const UAtJson &t : ot->arr)
                        if (t.type == UAtJson::STR)
                            att.origTextures.push_back(t.str);
                
                out->atts.push_back(att);
            }
        }
        
        return !out->atts.empty();
    }
}

NC_STACK_Obj3D *NC_STACK_Obj3D::LoadObjWithAttachments(const std::string &fname)
{
    // The game hands us prefixed paths ("rsrc:objects/NAME.obj", rsrc ->
    // "data:setN"); resolve the prefix chain once so every FSMgr open below
    // uses a plain tree path. ApplyPrefix is a no-op for paths without
    // "xxx:" (e.g. the tool's "export/NAME.obj"), and unlike uaOpenFile it
    // does not run correctSeparatorAndExt, so the ".uattach" sidecar name
    // (7-char extension) survives.
    std::string path = Common::Env.ApplyPrefix(fname);
    
    FSMgr::FileHandle fil = FSMgr::iDir::openFile(path, "rb");
    if (!fil.OK())
        return NULL;
    
    NC_STACK_Obj3D *tmp = Nucleus::CInit<NC_STACK_Obj3D>();
    tmp->fname = path;
    
    if (!tmp->LoadFromFile(&fil))
    {
        tmp->Delete();
        return NULL;
    }
    
    // sidecar? (<name>.uattach next to <name>.obj)
    std::string base = path;
    if (base.size() > 4 && !StriCmp(base.substr(base.size() - 4), ".obj"))
        base.resize(base.size() - 4);
    
    FSMgr::FileHandle *jfil = FSMgr::iDir::openFileAlloc(base + ".uattach", "rb");
    if (!jfil)
        return tmp;     // no sidecar: plain OBJ model
    
    std::string jsonText;
    {
        jfil->seek(0, SEEK_END);
        size_t sz = jfil->tell();
        jfil->seek(0, SEEK_SET);
        jsonText.resize(sz);
        size_t rd = jfil->read(&jsonText[0], sz);
        delete jfil;
        
        if (rd != sz)
        {
            fprintf(stderr, "UATTACH: short read of %s.uattach\n", path.c_str());
            return tmp;
        }
    }
    
    UAtCtx ctx;
    if (!UAtParseSidecar(jsonText, &ctx.side))
    {
        fprintf(stderr, "UATTACH: invalid sidecar %s\n", base.c_str());
        return tmp;
    }
    
    tmp->setName(ctx.side.model);
    ctx.dir = UAtDirName(path);
    
    UAtMountAssetsDir(ctx.dir);
    
    // skeleton
    NC_STACK_skeleton *skel = UAtLoadSkeleton(UAtJoin(ctx.dir, ctx.side.skeleton));
    if (!skel)
    {
        fprintf(stderr, "UATTACH %s: failed to load skeleton %s\n",
                ctx.side.model.c_str(), ctx.side.skeleton.c_str());
        return tmp;
    }
    
    // SetSkeleton deletes any previously set skeleton itself
    tmp->SetSkeleton(skel);
    
    // pre-scan the .ade files and register shadow resources so the engine
    // loaders resolve every by-name resource into the model's own files
    std::set<std::string> anmNames, texNames;
    
    for (const UAtAttachment &att : ctx.side.atts)
    {
        std::string vpath = UAtJoin(ctx.dir, att.file);
        
        if (!UAtScanAdeNames(vpath, &anmNames, &texNames))
        {
            fprintf(stderr, "UATTACH %s: failed to pre-scan %s\n",
                    ctx.side.model.c_str(), att.file.c_str());
            ctx.ok = false;
        }
        
        // sidecar-declared names too (safety net)
        if (att.bmpanim.present)
            anmNames.insert(att.bmpanim.origAnm);
        if (att.bmpanimTracy.present)
            anmNames.insert(att.bmpanimTracy.origAnm);
        for (const std::string &t : att.origTextures)
            texNames.insert(t);
    }
    
    for (const std::string &nm : anmNames)
    {
        if (!UAtShadowAnm(&ctx, nm))
            break;
    }
    
    for (const std::string &nm : texNames)
    {
        if (!UAtShadowTexture(&ctx, nm))
            break;
    }
    
    if (ctx.ok)
    {
        // load all attachment ADEs (Task 3 recipe)
        std::vector<NC_STACK_ade *> ades;
        
        for (const UAtAttachment &att : ctx.side.atts)
        {
            std::string vpath = UAtJoin(ctx.dir, att.file);
            
            IFFile f = IFFile::UAOpenIFFile(vpath, "rb");
            if (!f.OK())
            {
                fprintf(stderr, "UATTACH %s: cannot open %s\n",
                        ctx.side.model.c_str(), vpath.c_str());
                ctx.ok = false;
                break;
            }
            
            if (f.parse() || !f.GetCurrentChunk().Is(TAG_FORM, TAG_ADES))
            {
                fprintf(stderr, "UATTACH %s: %s is not a FORM ADES\n",
                        ctx.side.model.c_str(), vpath.c_str());
                ctx.ok = false;
                break;
            }
            
            while (ctx.ok)
            {
                int res = f.parse();
                
                if (res == IFFile::IFF_ERR_EOC)
                    break;
                
                if (res)
                {
                    ctx.ok = false;
                    break;
                }
                
                if (f.GetCurrentChunk().Is(TAG_FORM, TAG_OBJT))
                {
                    NC_STACK_nucleus *nobj = NC_STACK_nucleus::LoadObjectFromIFF(&f);
                    NC_STACK_ade *ade = dynamic_cast<NC_STACK_ade *>(nobj);
                    
                    if (!ade)
                    {
                        if (nobj)
                            nobj->Delete();
                        fprintf(stderr, "UATTACH %s: failed to load OBJT from %s\n",
                                ctx.side.model.c_str(), vpath.c_str());
                        ctx.ok = false;
                        break;
                    }
                    
                    ades.push_back(ade);
                }
                else
                {
                    f.skipChunk();
                }
            }
            
            if (!ctx.ok)
                break;
        }
        
        if (ctx.ok && ades.size() != ctx.side.atts.size())
        {
            fprintf(stderr, "UATTACH %s: loaded %zu ADEs, sidecar has %zu\n",
                    ctx.side.model.c_str(), ades.size(), ctx.side.atts.size());
            ctx.ok = false;
        }
        
        if (ctx.ok)
        {
            for (NC_STACK_ade *ade : ades)
                tmp->SetAde(ade);
            
            // regenerate the meshes from skeleton + ADEs (NC_STACK_Obj3D
            // overrides RecalcInternal with a no-op, so call the base one
            // explicitly - this is what the engine's base loader does)
            tmp->NC_STACK_base::RecalcInternal(false);
            tmp->MakeCoordsCache();
        }
        else
        {
            for (NC_STACK_ade *ade : ades)
            {
                ade->AttachedTo = NULL;
                ade->Delete();
            }
        }
    }
    
    // release the loader's keepalive references
    for (UAtShadow &sh : ctx.shadows)
    {
        sh.ReleaseKeepalive();
    }
    
    if (!ctx.ok)
    {
        tmp->Delete();
        return NULL;
    }
    
    return tmp;
}
