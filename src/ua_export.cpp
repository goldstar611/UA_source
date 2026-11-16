#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <map>
#include <set>
#include <vector>
#include <utils.h>

#include "system/fsmgr.h"
#include "system/system.h"
#include "system/inivals.h"
#include "env.h"
#include "includes.h"

#include "nucleas.h"
#include "rsrc.h"
#include "bitmap.h"
#include "skeleton.h"
#include "sklt.h"
#include "ilbm.h"
#include "ade.h"
#include "area.h"
#include "base.h"
#include "bmpAnm.h"
#include "amesh.h"
#include "particle.h"
#include "embed.h"
#include "image.h"
#include "obj3d.h"
#include "3ds.h"
#include "loaders.h"

// Globals normally defined in main.cpp (referenced by engine code)
TInputState input_states;
base_64arg world_update_arg;
NC_STACK_ypaworld *ypaworld = NULL;

static void RegisterClasses()
{
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_nucleus>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_rsrc>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_bitmap>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_skeleton>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_ilbm>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_sklt>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_ade>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_area>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_base>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_bmpanim>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_amesh>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_particle>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_embed>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_image>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_Obj3D>() );
    Nucleus::ClassList.push_back( Nucleus::MakeClassDescr<NC_STACK_3ds>() );
}

static std::string DirName(const std::string &path)
{
    size_t pos = path.find_last_of("/\\");
    if ( pos == std::string::npos )
        return std::string();
    if ( pos == 0 )
        return "/";
    return path.substr(0, pos);
}

static std::string BaseName(const std::string &path)
{
    size_t pos = path.find_last_of("/\\");
    if ( pos == std::string::npos )
        return path;
    return path.substr(pos + 1);
}

// mkdir -p equivalent (POSIX), for the export output dir which lives outside
// the FSMgr base dir
static void MakeDirPath(const std::string &path)
{
    size_t pos = 0;
    while ( pos != std::string::npos )
    {
        size_t next = path.find('/', pos);
        size_t len = (next == std::string::npos) ? path.size() : next;
        
        if ( len > pos )
        {
            std::string part = path.substr(0, len);
            mkdir(part.c_str(), 0755);
        }
        
        pos = (next == std::string::npos) ? next : next + 1;
    }
}

// ---------------------------------------------------------------------------
// Task 3: attachment / pool export
// ---------------------------------------------------------------------------

// rsrc names may carry trailing NULs (EMRS readStr quirk, see obj3d.cpp)
static std::string TrimName(const std::string &s)
{
    size_t n = s.find('\0');
    return (n != std::string::npos) ? s.substr(0, n) : s;
}

static std::string LowerName(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
        c = (char)tolower((unsigned char)c);
    return out;
}

// Strip any leading path components ("Skeleton/VP_NFOX.sklt" -> "VP_NFOX.sklt")
static std::string BaseFileName(const std::string &s)
{
    size_t pos = s.find_last_of("/\\:");
    return (pos != std::string::npos) ? s.substr(pos + 1) : s;
}

// Replace the file extension (if any) with 'ext': "Skeleton/X.sklt" -> "X.skl",
// "PANZERKETTEN" -> "PANZERKETTEN.anm", "BRGBLTZ.ANM" -> "BRGBLTZ.anm"
static std::string StemWithExt(const std::string &s, const char *ext)
{
    std::string b = BaseFileName(s);
    size_t pos = b.rfind('.');
    if (pos != std::string::npos && pos != 0)
        b.resize(pos);
    return b + ext;
}

// Make a string safe to use as a file name component
static std::string SanitizeFileName(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if ((unsigned char)c < 0x20 || c == '/' || c == '\\' || c == ':' ||
            c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            out += '_';
        else
            out += c;
    }
    return out;
}

// Assigns one unique output file name per source name. On collision (two
// different source names mapping to the same wanted file name) a numeric
// suffix is appended and the collision is reported.
class FileNameMap
{
public:
    std::string Map(const std::string &orig, const std::string &want)
    {
        auto it = _m.find(orig);
        if (it != _m.end())
            return it->second;

        std::string name = want;
        int n = 2;
        while (_used.count(name))
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "_%d", n++);
            name = want + buf;
        }
        if (name != want)
            fprintf(stderr, "COLLISION: output name '%s' is used twice (second source gets '%s')\n",
                    want.c_str(), name.c_str());
        _used.insert(name);
        _m[orig] = name;
        return name;
    }

private:
    std::map<std::string, std::string> _m;
    std::set<std::string> _used;
};

static std::string JsonEscape(const std::string &str)
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

// Minimal JSON well-formedness checker (no semantic analysis)
class JsonChecker
{
public:
    static bool Validate(const std::string &text)
    {
        JsonChecker c(text.c_str(), text.c_str() + text.size());
        c.value();
        c.ws();
        if (!c.fail && c.p != c.e)
            c.fail = true;
        return !c.fail;
    }

private:
    JsonChecker(const char *b, const char *e) : p(b), e(e) {}

    const char *p;
    const char *e;
    bool fail = false;
    int depth = 0;

    void ws()
    {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
    }

    void expect(const char *word)
    {
        size_t n = strlen(word);
        if (e - p < (ptrdiff_t)n || strncmp(p, word, n) != 0)
            fail = true;
        else
            p += n;
    }

    bool value()
    {
        if (fail) return false;
        ws();
        if (p >= e) { fail = true; return false; }

        if (++depth > 64) { fail = true; return false; }

        char c = *p;
        if (c == '{')      object();
        else if (c == '[') array();
        else if (c == '"') string();
        else if (c == 't') expect("true");
        else if (c == 'f') expect("false");
        else if (c == 'n') expect("null");
        else               number();

        --depth;
        return !fail;
    }

    void object()
    {
        ++p;
        ws();
        if (p < e && *p == '}') { ++p; return; }
        while (p < e)
        {
            ws();
            if (p >= e || *p != '"') { fail = true; return; }
            string();
            ws();
            if (p >= e || *p != ':') { fail = true; return; }
            ++p;
            if (!value()) return;
            ws();
            if (p >= e) { fail = true; return; }
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return; }
            fail = true;
            return;
        }
        fail = true;
    }

    void array()
    {
        ++p;
        ws();
        if (p < e && *p == ']') { ++p; return; }
        while (p < e)
        {
            if (!value()) return;
            ws();
            if (p >= e) { fail = true; return; }
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; return; }
            fail = true;
            return;
        }
        fail = true;
    }

    void string()
    {
        ++p;
        while (p < e)
        {
            char c = *p;
            if (c == '"') { ++p; return; }
            if (c == '\\')
            {
                ++p;
                if (p >= e) { fail = true; return; }
                ++p;
            }
            else if ((unsigned char)c < 0x20)
            {
                fail = true;
                return;
            }
            else
                ++p;
        }
        fail = true;
    }

    void number()
    {
        const char *start = p;
        if (p < e && (*p == '-' || *p == '+')) ++p;
        while (p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                         *p == '-' || *p == '+'))
            ++p;
        if (p == start)
            fail = true;
    }
};

struct PoolSkl
{
    std::string orig;                 // original rsrc name ("Skeleton/X.sklt")
    NC_STACK_sklt *skltObj = NULL;    // sklt.class pool entry (write via rsrc_func66)
    UAskeleton::Data *data = NULL;    // model skeleton data (manual FORM SKLT write)
};

struct PoolAnm
{
    std::string orig;
    NC_STACK_bmpanim *anm = NULL;
};

struct PoolBmp
{
    std::string orig;
    NC_STACK_bitmap *bitm = NULL;
};

struct PoolCtx
{
    std::vector<PoolSkl> skls;
    std::map<std::string, size_t> sklIdx;   // lower(orig) -> skls index
    std::vector<PoolAnm> anms;
    std::map<std::string, size_t> anmIdx;
    std::vector<PoolBmp> bmps;
    std::map<std::string, size_t> bmpIdx;

    FileNameMap sklNames, anmNames, pngNames;

    std::map<std::string, std::string> sklOut;   // lower(orig) -> output file name
    std::map<std::string, std::string> anmOut;
    std::map<std::string, std::string> pngOut;

    std::map<std::string, std::vector<uint8_t> > sklBytes;  // lower(orig) -> file bytes
    std::map<std::string, std::vector<uint8_t> > anmBytes;

    int nSkl = 0, nAnm = 0, nBmp = 0, fails = 0;
};

static void PoolAddSkl(PoolCtx &ctx, const std::string &origName, NC_STACK_sklt *obj, UAskeleton::Data *data)
{
    std::string name = TrimName(origName);
    if (name.empty())
    {
        fprintf(stderr, "POOL: skeleton resource with empty name skipped\n");
        ctx.fails++;
        return;
    }
    std::string key = LowerName(name);
    if (ctx.sklIdx.count(key))
        return;
    ctx.sklIdx[key] = ctx.skls.size();
    ctx.skls.push_back({name, obj, data});
}

static void PoolAddAnm(PoolCtx &ctx, const std::string &origName, NC_STACK_bmpanim *obj)
{
    std::string name = TrimName(origName);
    if (name.empty())
    {
        fprintf(stderr, "POOL: bmpanim resource with empty name skipped\n");
        ctx.fails++;
        return;
    }
    std::string key = LowerName(name);
    if (ctx.anmIdx.count(key))
        return;
    ctx.anmIdx[key] = ctx.anms.size();
    ctx.anms.push_back({name, obj});
}

static void PoolAddBmp(PoolCtx &ctx, const std::string &origName, NC_STACK_bitmap *obj)
{
    std::string name = TrimName(origName);
    if (name.empty())
        return;
    std::string key = LowerName(name);
    if (ctx.bmpIdx.count(key))
        return;
    ctx.bmpIdx[key] = ctx.bmps.size();
    ctx.bmps.push_back({name, obj});
}

// Collect the EMBD embed pool items (SKLT/VANM/VBMP) plus every model's own
// skeleton resource; deduped by resource name (case-insensitive, like rsrc)
static void CollectPoolFromBase(NC_STACK_base *b, PoolCtx &ctx)
{
    if (b->_embed)
    {
        if (NC_STACK_embed *emb = dynamic_cast<NC_STACK_embed *>(b->_embed))
        {
            for (NC_STACK_rsrc *res : emb->_resources)
            {
                if (!res)
                    continue;

                std::string cname = res->ClassName();
                std::string name = TrimName(res->getRsrc_name());

                if (cname == NC_STACK_sklt::__ClassName)
                    PoolAddSkl(ctx, name, dynamic_cast<NC_STACK_sklt *>(res), NULL);
                else if (cname == NC_STACK_bmpanim::__ClassName)
                    PoolAddAnm(ctx, name, dynamic_cast<NC_STACK_bmpanim *>(res));
                else if (cname == NC_STACK_ilbm::__ClassName || cname == NC_STACK_bitmap::__ClassName)
                    PoolAddBmp(ctx, name, dynamic_cast<NC_STACK_bitmap *>(res));
                else
                    fprintf(stderr, "POOL: unknown embedded class '%s' (name '%s')\n",
                            cname.c_str(), name.c_str());
            }
        }
    }

    if (b->GetSkeleton())
        PoolAddSkl(ctx, TrimName(b->GetSkeleton()->getRsrc_name()), NULL,
                   b->GetSkeleton()->GetSkelet());

    for (NC_STACK_base *kid : b->GetKidList())
        CollectPoolFromBase(kid, ctx);
}

// Also collect every bmpanim referenced by an ADE (some are not in the embed
// pool) and its embedded texture sheets, so the pool is complete
static void CollectPoolBitmap(PoolCtx &ctx, NC_STACK_bitmap *bm)
{
    if (!bm)
        return;

    if (bm->IsDynamic())
    {
        NC_STACK_bmpanim *ba = dynamic_cast<NC_STACK_bmpanim *>(bm);
        if (!ba)
            return;

        PoolAddAnm(ctx, TrimName(ba->getRsrc_name()), ba);

        if (ba->_pData)
        {
            for (size_t i = 0; i < ba->_pData->Bitmaps.size(); ++i)
            {
                NC_STACK_bitmap *sheet = ba->_pData->Bitmaps[i];
                if (!sheet)
                    continue;

                std::string nm = TrimName(sheet->getRsrc_name());
                if (nm.empty() && i < ba->_pData->BitmapNames.size())
                    nm = TrimName(ba->_pData->BitmapNames[i]);
                PoolAddBmp(ctx, nm, sheet);
            }
        }
    }
    else
        PoolAddBmp(ctx, TrimName(bm->getRsrc_name()), bm);
}

static void CollectPoolFromAdes(NC_STACK_base *b, PoolCtx &ctx)
{
    for (NC_STACK_ade *ade : *b->GetAdeList())
    {
        if (NC_STACK_area *ar = dynamic_cast<NC_STACK_area *>(ade))
        {
            CollectPoolBitmap(ctx, ar->_texImg);
            CollectPoolBitmap(ctx, ar->_tracyImg);
        }
    }

    for (NC_STACK_base *kid : b->GetKidList())
        CollectPoolFromAdes(kid, ctx);
}

static bool ReadFileBytes(const std::string &vpath, std::vector<uint8_t> *out)
{
    FSMgr::FileHandle *f = FSMgr::iDir::openFileAlloc(vpath, "rb");
    if (!f)
        return false;

    f->seek(0, SEEK_END);
    size_t sz = f->tell();
    f->seek(0, SEEK_SET);
    out->resize(sz);
    size_t rd = f->read(out->data(), sz);
    delete f;
    return rd == sz && sz > 0;
}

// Manual FORM SKLT writer for model skeletons that are not sklt.class pool
// entries; byte-for-byte identical to NC_STACK_sklt::rsrc_func66 output
static bool WriteSklData(UAskeleton::Data *sklt, const std::string &vpath)
{
    if (!sklt)
        return false;

    IFFile out(FSMgr::iDir::openFileAlloc(vpath, "wb"));
    if (!out.OK())
        return false;

    if (out.pushChunk(TAG_SKLT, TAG_FORM, -1))
        return false;

    if (!sklt->POO.empty())
    {
        out.pushChunk(0, TAG_POO2, 12 * sklt->POO.size());
        for (size_t i = 0; i < sklt->POO.size(); i++)
        {
            out.writeFloatB((float)sklt->POO[i].x);
            out.writeFloatB((float)sklt->POO[i].y);
            out.writeFloatB((float)sklt->POO[i].z);
        }
        out.popChunk();
    }

    if (!sklt->SEN.empty())
    {
        out.pushChunk(0, TAG_SEN2, 12 * sklt->SEN.size());
        for (size_t i = 0; i < sklt->SEN.size(); i++)
        {
            out.writeFloatB((float)sklt->SEN[i].x);
            out.writeFloatB((float)sklt->SEN[i].y);
            out.writeFloatB((float)sklt->SEN[i].z);
        }
        out.popChunk();
    }

    if (!sklt->polygons.empty())
    {
        out.pushChunk(0, TAG_POL2, -1);
        out.writeS32B(sklt->polygons.size());
        for (size_t i = 0; i < sklt->polygons.size(); i++)
        {
            out.writeU16B(sklt->polygons[i].num_vertices);
            for (int j = 0; j < sklt->polygons[i].num_vertices; j++)
                out.writeU16B(sklt->polygons[i].v[j]);
        }
        out.popChunk();
    }

    return out.popChunk() == IFFile::IFF_ERR_OK;
}

// Standalone .ade writer. File layout (exactly what a minimal Task-4 loader
// driven by NC_STACK_nucleus::LoadObjectFromIFF needs, mirroring how
// NC_STACK_base::ReadIFFTagADES reads the real files):
//
//   FORM ADES {
//     FORM OBJT {
//       CLID "<amesh.class|area.class|particle.class|ade.class>"
//       <class content>
//     }
//   }
//
// where <class content> is the chunk tree the class's own save path emits:
//   amesh:    FORM AMSH { FORM AREA {...}, ATTS, OLPL? }
//   area:     FORM AREA { FORM ADE {...}, STRC, OBJT?... }
//   particle: FORM PTCL { FORM ADE {...}, ATTS, OBJT... (life stages) }
//   ade:      FORM ADE { FORM ROOT { NAME }, STRC }
static bool WriteAdeCommon(NC_STACK_ade *ade, IFFile *out)
{
    if (out->pushChunk(TAG_ADE, TAG_FORM, -1))
        return false;

    bool ok = true;

    // NC_STACK_nucleus::SavingIntoIFF equivalent: FORM ROOT { NAME }
    if (out->pushChunk(TAG_ROOT, TAG_FORM, -1))
        ok = false;

    if (ok && !ade->getName().empty() && ade->getName().length() <= 32)
    {
        if (out->pushChunk(0, TAG_NAME, -1))
            ok = false;
        else
        {
            out->write(ade->getName().c_str(), ade->getName().length());
            ok = out->popChunk() == IFFile::IFF_ERR_OK;
        }
    }

    if (ok)
        ok = out->popChunk() == IFFile::IFF_ERR_OK;

    if (ok)
    {
        out->pushChunk(0, TAG_STRC, -1);
        out->writeS16B(1);
        out->writeS8(0);
        out->writeS8(ade->flags & (NC_STACK_ade::ADE_FLAG_BKCHECK | NC_STACK_ade::ADE_FLAG_DPTHFADE));
        out->writeS16B(ade->_pointID);
        out->writeS16B(ade->_polyID);
        out->writeS16B(0);
        ok = out->popChunk() == IFFile::IFF_ERR_OK;
    }

    if (ok)
        ok = out->popChunk() == IFFile::IFF_ERR_OK;

    return ok;
}

// Texture OBJT inside an area ADE. bmpanim textures are stored like in the
// original SET.BAS (BANI/STRC only - the class save would first try to write
// the ANM payload to rsrc:rsrcpool/<name> on disk, which must not happen here)
static bool WriteTexObj(NC_STACK_bitmap *bm, IFFile *out)
{
    if (!bm)
        return false;

    if (bm->IsDynamic())
    {
        NC_STACK_bmpanim *ba = dynamic_cast<NC_STACK_bmpanim *>(bm);
        if (!ba)
            return false;

        std::string name = TrimName(ba->getRsrc_name());

        if (out->pushChunk(TAG_OBJT, TAG_FORM, -1))
            return false;

        bool ok = true;
        std::string clsname = ba->ClassName();

        if (out->pushChunk(0, TAG_CLID, clsname.length() + 1))
            ok = false;
        else
        {
            out->write(clsname.c_str(), clsname.length() + 1);
            ok = out->popChunk() == IFFile::IFF_ERR_OK;
        }

        if (ok && out->pushChunk(TAG_BANI, TAG_FORM, -1))
            ok = false;

        if (ok)
        {
            // NOTE: bmpanim's loader (bmpAnm.cpp LoadingFromIFF) reads a fixed
            // 250 bytes from this STRC chunk, so it must be zero-padded to
            // 250 like in the original data files
            out->pushChunk(0, TAG_STRC, 250);
            out->writeS16B(1);  // version
            out->writeS16B(6);  // offset of name
            out->writeS16B(ba->getBANM_animtype());
            out->write(name.c_str(), name.size() + 1);
            size_t written = 6 + name.size() + 1;
            if (written < 250)
            {
                std::vector<char> zpad(250 - written, 0);
                out->write(zpad.data(), zpad.size());
            }
            ok = out->popChunk() == IFFile::IFF_ERR_OK;
        }

        if (ok)
            ok = out->popChunk() == IFFile::IFF_ERR_OK;

        if (ok)
            ok = out->popChunk() == IFFile::IFF_ERR_OK;

        return ok;
    }

    // ilbm/bitmap: the class's own SaveObjectIntoIFF path (OBJT > CLID >
    // FORM CIBO { NAM2, OTL2? })
    return bm->SaveObjectIntoIFF(out);
}

static bool WriteAreaContent(NC_STACK_area *ar, IFFile *out)
{
    if (out->pushChunk(TAG_AREA, TAG_FORM, -1))
        return false;

    bool ok = WriteAdeCommon(ar, out);

    if (ok)
    {
        out->pushChunk(0, TAG_STRC, -1);

        uint16_t tmpflg = 0;
        if (ar->flags & NC_STACK_ade::ADE_FLAG_BKCHECK)
            tmpflg |= NC_STACK_area::AREA_FLAG_BKCHECK;
        if (ar->flags & NC_STACK_ade::ADE_FLAG_DPTHFADE)
            tmpflg |= NC_STACK_area::AREA_FLAG_DPTHFADE;

        out->writeS16B(1);  // version
        out->writeU16B(tmpflg);
        out->writeU16B(ar->_polflags);
        out->writeU8(0);
        out->writeU8(ar->_colorVal);
        out->writeU8(ar->_tracyVal);
        out->writeU8(ar->_shadeVal);

        ok = out->popChunk() == IFFile::IFF_ERR_OK;
    }

    if (ok && (ar->_polflags & NC_STACK_area::AREA_POL_FLAG_TEXUTRED))
    {
        if (!ar->_texImg)
            ok = false;
        else
            ok = WriteTexObj(ar->_texImg, out);
    }

    if (ok && (ar->_polflags & NC_STACK_area::AREA_POL_FLAG_TRACYMAPPED) == NC_STACK_area::AREA_POL_FLAG_TRACYMAPPED)
    {
        if (!ar->_tracyImg)
            ok = false;
        else
            ok = WriteTexObj(ar->_tracyImg, out);
    }

    if (ok)
        ok = out->popChunk() == IFFile::IFF_ERR_OK;

    return ok;
}

static bool WriteAdeContent(NC_STACK_ade *ade, IFFile *out);

static bool WriteAdeObjt(NC_STACK_ade *ade, IFFile *out)
{
    if (out->pushChunk(TAG_OBJT, TAG_FORM, -1))
        return false;

    bool ok = true;
    std::string clsname = ade->ClassName();

    if (out->pushChunk(0, TAG_CLID, clsname.length() + 1))
        ok = false;
    else
    {
        out->write(clsname.c_str(), clsname.length() + 1);
        ok = out->popChunk() == IFFile::IFF_ERR_OK;
    }

    if (ok)
        ok = WriteAdeContent(ade, out);

    if (ok)
        ok = out->popChunk() == IFFile::IFF_ERR_OK;

    return ok;
}

static bool WriteAdeContent(NC_STACK_ade *ade, IFFile *out)
{
    if (NC_STACK_amesh *am = dynamic_cast<NC_STACK_amesh *>(ade))
    {
        // FORM AMSH is consumed by LoadObjectFromIFF as the entry form; the
        // amesh loader then sees FORM AREA / ATTS / OLPL inside it
        if (out->pushChunk(TAG_AMSH, TAG_FORM, -1))
            return false;

        bool ok = WriteAreaContent(am, out);

        if (ok)
        {
            out->pushChunk(0, TAG_ATTS, -1);
            for (int i = 0; i < am->polyCnt; i++)
            {
                NC_STACK_amesh::ATTS &att = am->atts.at(i);
                out->writeS16B(att.polyID);
                out->writeU8(att.colorVal);
                out->writeU8(att.shadeVal);
                out->writeU8(att.tracyVal);
                out->writeU8(att.pad);
            }
            ok = out->popChunk() == IFFile::IFF_ERR_OK;
        }

        if (ok && !am->texCoords.empty())
        {
            out->pushChunk(0, TAG_OLPL, -1);
            for (const std::vector<tUtV> &arr : am->texCoords)
            {
                out->writeS16B(arr.size());
                for (tUtV uv : arr)
                {
                    out->writeU8(uv.tu * 256.0);
                    out->writeU8(uv.tv * 256.0);
                }
            }
            ok = out->popChunk() == IFFile::IFF_ERR_OK;
        }

        if (ok)
            ok = out->popChunk() == IFFile::IFF_ERR_OK;

        return ok;
    }

    if (NC_STACK_area *ar = dynamic_cast<NC_STACK_area *>(ade))
        return WriteAreaContent(ar, out);

    if (NC_STACK_particle *pt = dynamic_cast<NC_STACK_particle *>(ade))
    {
        if (out->pushChunk(TAG_PTCL, TAG_FORM, -1))
            return false;

        bool ok = WriteAdeCommon(pt, out);

        if (ok)
        {
            out->pushChunk(0, TAG_ATTS, -1);
            out->writeS16B(1);  // version
            TF::Engine.Vec3dWriteIFF(pt->_accelStart, out, true);
            TF::Engine.Vec3dWriteIFF(pt->_accelEnd, out, true);
            TF::Engine.Vec3dWriteIFF(pt->_magnifyStart, out, true);
            TF::Engine.Vec3dWriteIFF(pt->_magnifyEnd, out, true);
            out->writeS32B(0);
            out->writeS32B(pt->_startSpeed);
            out->writeS32B(pt->_numContexts);
            out->writeS32B(pt->_genLifeTime);
            out->writeS32B(pt->_genStart);
            out->writeS32B(pt->_genEnd);
            out->writeS32B(pt->_genRate);
            out->writeS32B(pt->_lifeTime);
            out->writeS32B(pt->_scaleStart);
            out->writeS32B(pt->_scaleEnd);
            out->writeS32B(pt->_noisePower * 10.0);
            ok = out->popChunk() == IFFile::IFF_ERR_OK;
        }

        // Life stages are written as full OBJTs (the engine's own save writes
        // them via SaveObjectIntoIFF, which loses area data for stage ADEs
        // because area has no SavingIntoIFF override)
        if (ok)
        {
            for (NC_STACK_ade *stage : pt->_lifeStagesAdes)
            {
                if (stage && !WriteAdeObjt(stage, out))
                {
                    ok = false;
                    break;
                }
            }
        }

        if (ok)
            ok = out->popChunk() == IFFile::IFF_ERR_OK;

        return ok;
    }

    return WriteAdeCommon(ade, out);
}

static bool WriteAdeStandalone(NC_STACK_ade *ade, const std::string &vpath)
{
    IFFile out(FSMgr::iDir::openFileAlloc(vpath, "wb"));
    if (!out.OK())
        return false;

    if (out.pushChunk(TAG_ADES, TAG_FORM, -1))
        return false;

    bool ok = WriteAdeObjt(ade, &out);

    if (ok)
        ok = out.popChunk() == IFFile::IFF_ERR_OK;

    return ok;
}

// Loader for standalone .ade files: an exact replica of
// NC_STACK_nucleus::LoadObjectFromIFF, kept local so the tool documents the
// minimal loader recipe Task 4 needs (CLID -> class lookup -> FORM ->
// class LoadingFromIFF)
static NC_STACK_nucleus *LoadAdeObjectFromIFF(IFFile *mfile)
{
    Nucleus::TClassList::iterator clss = Nucleus::ClassList.end();
    NC_STACK_nucleus *obj = NULL;

    while ( 1 )
    {
        int v4 = mfile->parse();

        if ( v4 == IFFile::IFF_ERR_EOC )
            break;

        if ( v4 )
            return NULL;

        const IFFile::Context &chunk = mfile->GetCurrentChunk();

        if ( chunk.Is(TAG_CLID) )
        {
            std::string classname = mfile->readStr(256);

            if ( classname.empty() )
                return NULL;

            clss = Nucleus::ClassList.find(classname);
            if ( clss == Nucleus::ClassList.end() )
                return NULL;

            mfile->parse();
        }
        else if ( chunk.Is(TAG_FORM) )
        {
            IFFile *v11 = mfile;

            obj = clss->_newinstance();

            if ( !obj->LoadingFromIFF(&v11) )
            {
                delete obj;
                return NULL;
            }
        }
        else
        {
            if ( !mfile->skipChunk() )
                return NULL;
        }
    }
    return obj;
}

// Round-trip check: re-open a written .ade and feed it through the same
// loader chain Task 4 will use (mirrors NC_STACK_base::ReadIFFTagADES: enter
// FORM ADES context, then LoadObjectFromIFF per FORM OBJT), then compare the
// relevant fields
static bool VerifyAdeFile(const std::string &vpath, NC_STACK_ade *orig)
{
    IFFile f = IFFile::UAOpenIFFile(vpath, "rb");
    if (!f.OK())
        return false;

    // Consume FORM ADES like ReadIFFTagADES context
    if (f.parse())
        return false;
    if (!f.GetCurrentChunk().Is(TAG_FORM, TAG_ADES))
        return false;

    NC_STACK_nucleus *obj = NULL;

    while ( 1 )
    {
        int v = f.parse();

        if ( v == IFFile::IFF_ERR_EOC )
            break;

        if ( v )
            return false;

        if ( f.GetCurrentChunk().Is(TAG_FORM, TAG_OBJT) )
            obj = LoadAdeObjectFromIFF(&f);
        else
            f.skipChunk();

        if ( !obj )
            return false;
    }

    if (!obj)
        return false;

    bool ok = true;

    NC_STACK_ade *loaded = dynamic_cast<NC_STACK_ade *>(obj);
    if (!loaded || loaded->ClassName() != orig->ClassName())
        ok = false;

    if (ok && (loaded->_pointID != orig->_pointID || loaded->_polyID != orig->_polyID ||
               loaded->flags != orig->flags))
        ok = false;

    if (ok)
    {
        NC_STACK_area *lo = dynamic_cast<NC_STACK_area *>(loaded);
        NC_STACK_area *oo = dynamic_cast<NC_STACK_area *>(orig);
        if (lo && oo)
        {
            if (lo->_polflags != oo->_polflags || lo->_colorVal != oo->_colorVal ||
                lo->_tracyVal != oo->_tracyVal || lo->_shadeVal != oo->_shadeVal)
                ok = false;

            if (ok && (bool)lo->_texImg != (bool)oo->_texImg)
                ok = false;

            if (ok && lo->_texImg && TrimName(lo->_texImg->getRsrc_name()) != TrimName(oo->_texImg->getRsrc_name()))
                ok = false;

            if (ok && (bool)lo->_tracyImg != (bool)oo->_tracyImg)
                ok = false;

            if (ok && lo->_tracyImg && TrimName(lo->_tracyImg->getRsrc_name()) != TrimName(oo->_tracyImg->getRsrc_name()))
                ok = false;
        }

        NC_STACK_amesh *lm = dynamic_cast<NC_STACK_amesh *>(loaded);
        NC_STACK_amesh *om = dynamic_cast<NC_STACK_amesh *>(orig);
        if (ok && lm && om)
        {
            if (lm->polyCnt != om->polyCnt || lm->atts.size() != om->atts.size() ||
                lm->texCoords.size() != om->texCoords.size())
                ok = false;

            if (ok)
            {
                for (int i = 0; ok && i < lm->polyCnt; i++)
                {
                    NC_STACK_amesh::ATTS &a = lm->atts.at(i);
                    NC_STACK_amesh::ATTS &b = om->atts.at(i);
                    if (a.polyID != b.polyID || a.colorVal != b.colorVal ||
                        a.shadeVal != b.shadeVal || a.tracyVal != b.tracyVal || a.pad != b.pad)
                        ok = false;
                }
            }
        }

        NC_STACK_particle *lp = dynamic_cast<NC_STACK_particle *>(loaded);
        NC_STACK_particle *op = dynamic_cast<NC_STACK_particle *>(orig);
        if (ok && lp && op)
        {
            if (lp->_startSpeed != op->_startSpeed || lp->_numContexts != op->_numContexts ||
                lp->_genRate != op->_genRate || lp->_lifeTime != op->_lifeTime ||
                lp->_scaleStart != op->_scaleStart || lp->_scaleEnd != op->_scaleEnd ||
                lp->_noisePower != op->_noisePower ||
                lp->_lifeStagesAdes.size() != op->_lifeStagesAdes.size())
                ok = false;
        }
    }

    obj->Delete();

    return ok;
}

static bool WritePoolFiles(PoolCtx &ctx, const std::string &vdir)
{
    for (PoolSkl &e : ctx.skls)
    {
        std::string key = LowerName(e.orig);
        std::string file = ctx.sklNames.Map(key, StemWithExt(e.orig, ".skl"));
        ctx.sklOut[key] = file;
        std::string vpath = vdir + "/" + file;

        bool ok = false;

        if (e.skltObj)
        {
            IFFile out(FSMgr::iDir::openFileAlloc(vpath, "wb"));
            if (out.OK())
            {
                rsrc_func66_arg arg;
                arg.filename = NULL;
                arg.file = &out;
                arg.OpenedStream = 0;
                e.skltObj->rsrc_func66(&arg);
                ok = out.OK();
            }
        }
        else
            ok = WriteSklData(e.data, vpath);

        std::vector<uint8_t> bytes;
        if (ok)
            ok = ReadFileBytes(vpath, &bytes);

        if (ok)
        {
            ctx.sklBytes[key] = bytes;
            ctx.nSkl++;
        }
        else
        {
            fprintf(stderr, "POOL: FAILED to write skeleton '%s' as %s\n", e.orig.c_str(), file.c_str());
            ctx.fails++;
        }
    }

    for (PoolAnm &e : ctx.anms)
    {
        std::string key = LowerName(e.orig);
        std::string file = ctx.anmNames.Map(key, StemWithExt(e.orig, ".anm"));
        ctx.anmOut[key] = file;
        std::string vpath = vdir + "/" + file;

        bool ok = false;

        if (e.anm && e.anm->_pData)
        {
            IFFile out(FSMgr::iDir::openFileAlloc(vpath, "wb"));
            ok = out.OK() && NC_STACK_bmpanim::WriteToFile(e.anm->_pData, e.orig, &out);
        }

        std::vector<uint8_t> bytes;
        if (ok)
            ok = ReadFileBytes(vpath, &bytes);

        if (ok)
        {
            ctx.anmBytes[key] = bytes;
            ctx.nAnm++;
        }
        else
        {
            fprintf(stderr, "POOL: FAILED to write ANM '%s' as %s\n", e.orig.c_str(), file.c_str());
            ctx.fails++;
        }
    }

    for (PoolBmp &e : ctx.bmps)
    {
        std::string key = LowerName(e.orig);
        std::string file = ctx.pngNames.Map(key, BaseFileName(e.orig) + ".png");
        ctx.pngOut[key] = file;
        std::string vpath = vdir + "/" + file;

        ResBitmap *rb = e.bitm ? e.bitm->GetBitmap() : NULL;
        bool ok = rb && NC_STACK_image::SavePng(rb, vpath);

        if (ok)
            ctx.nBmp++;
        else
        {
            fprintf(stderr, "POOL: FAILED to write bitmap '%s' as %s\n", e.orig.c_str(), file.c_str());
            ctx.fails++;
        }
    }

    printf("POOL %d/%d/%d\n", ctx.nSkl, ctx.nAnm, ctx.nBmp);

    return ctx.fails == 0;
}

// Per-model attachment export ----------------------------------------------------------------

struct AttBmpanimInfo
{
    bool present = false;
    std::string origAnm;
    std::string file;
    int animType = 0;
    std::vector<std::string> sheets;
};

struct AttEntry
{
    int index = 0;
    std::string className;
    std::string file;
    bool hasPoly = false;
    int polyId = 0;
    bool hasPoint = false;
    int pointId = 0;
    AttBmpanimInfo bmpanim;
    AttBmpanimInfo bmpanimTracy;
    std::vector<std::string> origTextures;
    // strc payload as a pre-rendered JSON fragment
    std::string strcJson;
};

static bool ExportModelAttachments(NC_STACK_base *kid, const std::string &vdir,
                                   const std::string &modelRaw, const std::string &model,
                                   PoolCtx &pool)
{
    std::vector<std::string> referencedFiles;

    // --- collect textures and bmpanims referenced by this model ---
    struct ModelTex
    {
        std::string orig;
        NC_STACK_bitmap *bitm = NULL;
    };

    std::map<std::string, ModelTex> texMap;         // lower(orig) -> entry
    std::map<std::string, NC_STACK_bmpanim *> anmMap; // lower(orig) -> bmpanim

    auto addTex = [&](NC_STACK_bitmap *bm)
    {
        if (!bm)
            return;
        std::string name = TrimName(bm->getRsrc_name());
        if (name.empty())
            return;
        std::string key = LowerName(name);
        if (texMap.count(key))
            return;
        texMap[key] = {name, bm};
    };

    auto addBmpanim = [&](NC_STACK_bmpanim *ba)
    {
        if (!ba)
            return;
        std::string name = TrimName(ba->getRsrc_name());
        if (name.empty())
        {
            fprintf(stderr, "ATTACH %s: bmpanim with empty rsrc name skipped\n", modelRaw.c_str());
            return;
        }
        std::string key = LowerName(name);
        anmMap[key] = ba;
        if (ba->_pData)
        {
            for (size_t i = 0; i < ba->_pData->Bitmaps.size(); ++i)
            {
                NC_STACK_bitmap *sheet = ba->_pData->Bitmaps[i];
                if (!sheet)
                    continue;
                std::string nm = TrimName(sheet->getRsrc_name());
                if (nm.empty() && i < ba->_pData->BitmapNames.size())
                    nm = TrimName(ba->_pData->BitmapNames[i]);
                addTex(sheet);
                if (nm.empty())
                    fprintf(stderr, "ATTACH %s: ANM '%s' sheet %zu has no name\n",
                            modelRaw.c_str(), name.c_str(), i);
            }
        }
    };

    auto addTextureObj = [&](NC_STACK_bitmap *bm)
    {
        if (!bm)
            return;
        if (bm->IsDynamic())
            addBmpanim(dynamic_cast<NC_STACK_bmpanim *>(bm));
        else
            addTex(bm);
    };

    for (NC_STACK_ade *ade : *kid->GetAdeList())
    {
        if (NC_STACK_area *ar = dynamic_cast<NC_STACK_area *>(ade))
        {
            addTextureObj(ar->_texImg);
            addTextureObj(ar->_tracyImg);
        }
    }

    // --- skeleton ---
    if (!kid->GetSkeleton())
    {
        fprintf(stderr, "ATTACH %s: model has meshes but no skeleton\n", modelRaw.c_str());
        return false;
    }

    std::string sklOrig = TrimName(kid->GetSkeleton()->getRsrc_name());
    if (sklOrig.empty())
    {
        fprintf(stderr, "ATTACH %s: skeleton has empty rsrc name\n", modelRaw.c_str());
        return false;
    }

    std::string sklKey = LowerName(sklOrig);
    std::string sklFile;
    auto sklIt = pool.sklOut.find(sklKey);
    if (sklIt != pool.sklOut.end())
        sklFile = model + "_" + sklIt->second;
    else
        sklFile = model + "_" + StemWithExt(sklOrig, ".skl");

    std::string sklVpath = vdir + "/" + sklFile;
    {
        auto bytesIt = pool.sklBytes.find(sklKey);
        bool ok = false;
        if (bytesIt != pool.sklBytes.end())
        {
            FSMgr::FileHandle *f = FSMgr::iDir::openFileAlloc(sklVpath, "wb");
            ok = f && f->write(bytesIt->second.data(), bytesIt->second.size()) == bytesIt->second.size();
            delete f;
        }
        else
            ok = WriteSklData(kid->GetSkeleton()->GetSkelet(), sklVpath);

        if (!ok)
        {
            fprintf(stderr, "ATTACH %s: FAILED to write skeleton %s\n", modelRaw.c_str(), sklFile.c_str());
            return false;
        }
    }
    referencedFiles.push_back(sklFile);

    // --- textures ---
    std::vector<std::pair<std::string, std::string> > texList;  // (orig, file), sorted by orig
    for (auto &kv : texMap)
    {
        std::string pngFile;
        auto it = pool.pngOut.find(kv.first);
        if (it != pool.pngOut.end())
            pngFile = model + "_" + it->second;
        else
            pngFile = model + "_" + BaseFileName(kv.second.orig) + ".png";

        ResBitmap *rb = kv.second.bitm ? kv.second.bitm->GetBitmap() : NULL;
        if (!rb || !NC_STACK_image::SavePng(rb, vdir + "/" + pngFile))
        {
            fprintf(stderr, "ATTACH %s: FAILED to write texture '%s' as %s\n",
                    modelRaw.c_str(), kv.second.orig.c_str(), pngFile.c_str());
            return false;
        }

        texList.push_back({kv.second.orig, pngFile});
        referencedFiles.push_back(pngFile);
    }

    // --- ANM copies ---
    std::vector<std::pair<std::string, std::string> > anmList;  // (orig, file)
    for (auto &kv : anmMap)
    {
        std::string anmFile;
        auto it = pool.anmOut.find(kv.first);
        if (it != pool.anmOut.end())
            anmFile = model + "_" + it->second;
        else
            anmFile = model + "_" + StemWithExt(kv.second->getRsrc_name(), ".anm");

        std::string anmVpath = vdir + "/" + anmFile;
        bool ok = false;
        auto bytesIt = pool.anmBytes.find(kv.first);
        if (bytesIt != pool.anmBytes.end())
        {
            FSMgr::FileHandle *f = FSMgr::iDir::openFileAlloc(anmVpath, "wb");
            ok = f && f->write(bytesIt->second.data(), bytesIt->second.size()) == bytesIt->second.size();
            delete f;
        }
        else if (kv.second->_pData)
        {
            IFFile out(FSMgr::iDir::openFileAlloc(anmVpath, "wb"));
            ok = out.OK() && NC_STACK_bmpanim::WriteToFile(kv.second->_pData,
                                                           TrimName(kv.second->getRsrc_name()), &out);
        }

        if (!ok)
        {
            fprintf(stderr, "ATTACH %s: FAILED to write ANM '%s' as %s\n",
                    modelRaw.c_str(), TrimName(kv.second->getRsrc_name()).c_str(), anmFile.c_str());
            return false;
        }

        anmList.push_back({TrimName(kv.second->getRsrc_name()), anmFile});
        referencedFiles.push_back(anmFile);
    }

    // --- per-ADE files and JSON attachment entries ---
    std::vector<AttEntry> atts;
    int idx = 0;

    for (NC_STACK_ade *ade : *kid->GetAdeList())
    {
        AttEntry e;
        e.index = idx;
        e.className = ade->ClassName();

        char buf[64];
        snprintf(buf, sizeof(buf), "%s_ade%d.ade", model.c_str(), idx);
        e.file = buf;

        std::string vpath = vdir + "/" + e.file;

        bool wok = WriteAdeStandalone(ade, vpath);
        bool vok = wok && VerifyAdeFile(vpath, ade);
        if (!wok || !vok)
        {
            fprintf(stderr, "ATTACH %s: FAILED to write/verify %s\n", modelRaw.c_str(), e.file.c_str());
            return false;
        }
        referencedFiles.push_back(e.file);

        // Bindings
        if (NC_STACK_particle *pt = dynamic_cast<NC_STACK_particle *>(ade))
        {
            e.hasPoint = true;
            e.pointId = pt->_pointID;
        }
        else
        {
            e.hasPoly = true;
            e.polyId = ade->_polyID;
        }

        // bmpanim info + orig textures
        if (NC_STACK_area *ar = dynamic_cast<NC_STACK_area *>(ade))
        {
            auto fillBmpanimInfo = [&](NC_STACK_bitmap *bm, AttBmpanimInfo &info)
            {
                if (!bm || !bm->IsDynamic())
                    return;
                NC_STACK_bmpanim *ba = dynamic_cast<NC_STACK_bmpanim *>(bm);
                if (!ba)
                    return;

                info.present = true;
                info.origAnm = TrimName(ba->getRsrc_name());
                for (auto &kv : anmList)
                {
                    if (StriCmp(kv.first, info.origAnm) == 0)
                        info.file = kv.second;
                }
                info.animType = ba->getBANM_animtype();
                if (ba->_pData)
                {
                    for (const std::string &nm : ba->_pData->BitmapNames)
                    {
                        std::string t = TrimName(nm);
                        if (!t.empty())
                            info.sheets.push_back(t);
                    }
                }
            };

            fillBmpanimInfo(ar->_texImg, e.bmpanim);
            fillBmpanimInfo(ar->_tracyImg, e.bmpanimTracy);

            if (ar->_texImg && !ar->_texImg->IsDynamic())
                e.origTextures.push_back(TrimName(ar->_texImg->getRsrc_name()));
            if (ar->_tracyImg && !ar->_tracyImg->IsDynamic())
                e.origTextures.push_back(TrimName(ar->_tracyImg->getRsrc_name()));
        }

        // strc JSON fragment
        {
            std::string s = "{\n";
            s += fmt::sprintf("        \"version\": 1,\n");
            s += fmt::sprintf("        \"point\": %d,\n", (int)ade->_pointID);
            s += fmt::sprintf("        \"poly\": %d,\n", (int)ade->_polyID);
            s += fmt::sprintf("        \"flags\": %d,\n",
                              (int)(ade->flags & (NC_STACK_ade::ADE_FLAG_BKCHECK | NC_STACK_ade::ADE_FLAG_DPTHFADE)));

            if (NC_STACK_area *ar = dynamic_cast<NC_STACK_area *>(ade))
            {
                s += fmt::sprintf("        \"polFlags\": %d,\n", (int)ar->_polflags);
                s += fmt::sprintf("        \"clrVal\": %d,\n", (int)ar->_colorVal);
                s += fmt::sprintf("        \"trcVal\": %d,\n", (int)ar->_tracyVal);
                s += fmt::sprintf("        \"shdVal\": %d", (int)ar->_shadeVal);

                if (NC_STACK_amesh *am = dynamic_cast<NC_STACK_amesh *>(ade))
                {
                    s += fmt::sprintf(",\n        \"numpoly\": %d", (int)am->polyCnt);
                    s += ",\n        \"atts\": [";
                    for (int i = 0; i < am->polyCnt; i++)
                    {
                        NC_STACK_amesh::ATTS &att = am->atts.at(i);
                        s += fmt::sprintf("%s{\"polyID\": %d, \"colorVal\": %d, \"shadeVal\": %d, \"tracyVal\": %d, \"pad\": %d}",
                                          i ? ", " : "", (int)att.polyID, (int)att.colorVal,
                                          (int)att.shadeVal, (int)att.tracyVal, (int)att.pad);
                    }
                    s += "]";

                    s += ",\n        \"olpl\": [";
                    bool firstGroup = true;
                    for (const std::vector<tUtV> &arr : am->texCoords)
                    {
                        if (!firstGroup)
                            s += ", ";
                        firstGroup = false;
                        s += "[";
                        for (size_t j = 0; j < arr.size(); j++)
                        {
                            int u = (int)(arr[j].tu * 256.0);
                            int v = (int)(arr[j].tv * 256.0);
                            s += fmt::sprintf("%s[%d, %d]", j ? ", " : "", u, v);
                        }
                        s += "]";
                    }
                    s += "]";
                }
            }
            else if (NC_STACK_particle *pt = dynamic_cast<NC_STACK_particle *>(ade))
            {
                auto v3 = [](const vec3d &v)
                {
                    return fmt::sprintf("[%.9g, %.9g, %.9g]", v.x, v.y, v.z);
                };

                s += fmt::sprintf("        \"atts\": {\n");
                s += fmt::sprintf("          \"accelStart\": %s,\n", v3(pt->_accelStart).c_str());
                s += fmt::sprintf("          \"accelEnd\": %s,\n", v3(pt->_accelEnd).c_str());
                s += fmt::sprintf("          \"magnifyStart\": %s,\n", v3(pt->_magnifyStart).c_str());
                s += fmt::sprintf("          \"magnifyEnd\": %s,\n", v3(pt->_magnifyEnd).c_str());
                s += fmt::sprintf("          \"startSpeed\": %d,\n", (int)pt->_startSpeed);
                s += fmt::sprintf("          \"numContexts\": %d,\n", (int)pt->_numContexts);
                s += fmt::sprintf("          \"genLifeTime\": %d,\n", (int)pt->_genLifeTime);
                s += fmt::sprintf("          \"genStart\": %d,\n", (int)pt->_genStart);
                s += fmt::sprintf("          \"genEnd\": %d,\n", (int)pt->_genEnd);
                s += fmt::sprintf("          \"genRate\": %d,\n", (int)pt->_genRate);
                s += fmt::sprintf("          \"lifeTime\": %d,\n", (int)pt->_lifeTime);
                s += fmt::sprintf("          \"scaleStart\": %.9g,\n", (float)pt->_scaleStart);
                s += fmt::sprintf("          \"scaleEnd\": %.9g,\n", (float)pt->_scaleEnd);
                s += fmt::sprintf("          \"noisePower\": %.9g\n", (float)pt->_noisePower);
                s += "        }";
            }

            s += "\n      }";
            e.strcJson = s;
        }

        atts.push_back(e);
        ++idx;
    }

    // --- .uattach JSON ---
    std::string jsonPath = vdir + "/" + model + ".uattach";

    // anm_textures: sheet name -> per-model png (deduped by sheet name)
    std::vector<std::pair<std::string, std::string> > anmTex;
    {
        std::set<std::string> seen;
        for (auto &kv : anmMap)
        {
            if (!kv.second->_pData)
                continue;
            for (size_t i = 0; i < kv.second->_pData->Bitmaps.size(); ++i)
            {
                NC_STACK_bitmap *sheet = kv.second->_pData->Bitmaps[i];
                if (!sheet)
                    continue;
                std::string nm = TrimName(sheet->getRsrc_name());
                if (nm.empty() && i < kv.second->_pData->BitmapNames.size())
                    nm = TrimName(kv.second->_pData->BitmapNames[i]);
                if (nm.empty())
                    continue;

                if (!seen.insert(LowerName(nm)).second)
                    continue;

                std::string file;
                for (auto &t : texList)
                {
                    if (StriCmp(t.first, nm) == 0)
                    {
                        file = t.second;
                        break;
                    }
                }
                if (!file.empty())
                    anmTex.push_back({nm, file});
            }
        }
    }

    {
        FSMgr::FileHandle *j = FSMgr::iDir::openFileAlloc(jsonPath, "w");
        if (!j || !j->OK())
        {
            fprintf(stderr, "ATTACH %s: FAILED to open %s\n", modelRaw.c_str(), jsonPath.c_str());
            return false;
        }

        j->puts("{\n");
        j->printf("  \"model\": \"%s\",\n", JsonEscape(modelRaw).c_str());
        j->printf("  \"skeleton\": \"%s\",\n", JsonEscape(sklFile).c_str());
        j->printf("  \"skeleton_orig\": \"%s\",\n", JsonEscape(sklOrig).c_str());

        j->puts("  \"anm_textures\": {");
        bool first = true;
        for (auto &kv : anmTex)
        {
            j->printf("%s\n    \"%s\": \"%s\"", first ? "" : ",",
                      JsonEscape(kv.first).c_str(), JsonEscape(kv.second).c_str());
            first = false;
        }
        j->puts(first ? "},\n" : "\n  },\n");

        j->puts("  \"attachments\": [\n");
        for (size_t i = 0; i < atts.size(); i++)
        {
            const AttEntry &e = atts[i];

            j->printf("    {\n");
            j->printf("      \"index\": %d,\n", e.index);
            j->printf("      \"class\": \"%s\",\n", JsonEscape(e.className).c_str());
            j->printf("      \"file\": \"%s\",\n", JsonEscape(e.file).c_str());
            if (e.hasPoly)
                j->printf("      \"poly_id\": %d,\n", e.polyId);
            else
                j->puts("      \"poly_id\": null,\n");
            if (e.hasPoint)
                j->printf("      \"point_id\": %d,\n", e.pointId);
            else
                j->puts("      \"point_id\": null,\n");

            auto writeBmpanim = [&](const char *key, const AttBmpanimInfo &info)
            {
                if (!info.present)
                {
                    j->printf("      \"%s\": null,\n", key);
                    return;
                }
                j->printf("      \"%s\": {\n", key);
                j->printf("        \"orig_anm\": \"%s\",\n", JsonEscape(info.origAnm).c_str());
                j->printf("        \"file\": \"%s\",\n", JsonEscape(info.file).c_str());
                j->printf("        \"anim_type\": %d,\n", info.animType);
                j->printf("        \"version\": 1,\n");
                j->printf("        \"offset\": 6,\n");
                j->printf("        \"sheets\": [");
                for (size_t k = 0; k < info.sheets.size(); k++)
                    j->printf("%s\"%s\"", k ? ", " : "", JsonEscape(info.sheets[k]).c_str());
                j->puts("]\n");
                j->printf("      },\n");
            };

            writeBmpanim("bmpanim", e.bmpanim);
            writeBmpanim("bmpanim_tracy", e.bmpanimTracy);

            j->puts("      \"orig_textures\": [");
            for (size_t k = 0; k < e.origTextures.size(); k++)
                j->printf("%s\"%s\"", k ? ", " : "", JsonEscape(e.origTextures[k]).c_str());
            j->puts("],\n");

            j->puts("      \"strc\": ");
            j->puts(e.strcJson);
            j->printf("%s\n", (i + 1 < atts.size()) ? "    }," : "    }");
        }
        j->puts("  ],\n");

        // kids: STRC transforms of model kids (normally empty)
        j->puts("  \"kids\": [");
        bool firstKid = true;
        for (NC_STACK_base *k : kid->GetKidList())
        {
            if (!firstKid)
                j->puts(",");
            firstKid = false;
            TF::TForm3D &tf = k->TForm();
            j->printf("\n    {\"name\": \"%s\", \"pos\": [%.9g, %.9g, %.9g], \"scale\": [%.9g, %.9g, %.9g], "
                      "\"euler\": [%d, %d, %d], \"visLimit\": %d, \"ambientLight\": %d}",
                      JsonEscape(k->getName()).c_str(),
                      tf.Pos.x, tf.Pos.y, tf.Pos.z,
                      tf.Scale.x, tf.Scale.y, tf.Scale.z,
                      (int)tf.ax, (int)tf.ay, (int)tf.az,
                      (int)k->GetVisLimit(), (int)k->GetAmbientLight());
        }
        j->puts(firstKid ? "]\n" : "\n  ]\n");
        j->puts("}\n");

        delete j;
    }

    // --- self-check ---
    // 1) uattach parses as JSON
    std::string jsonText;
    {
        FSMgr::FileHandle *f = FSMgr::iDir::openFileAlloc(jsonPath, "rb");
        if (!f)
        {
            fprintf(stderr, "ATTACH %s: self-check FAILED, cannot re-open %s\n", modelRaw.c_str(), jsonPath.c_str());
            return false;
        }
        f->seek(0, SEEK_END);
        size_t sz = f->tell();
        f->seek(0, SEEK_SET);
        jsonText.resize(sz);
        size_t rd = f->read(&jsonText[0], sz);
        delete f;
        if (rd != sz)
        {
            fprintf(stderr, "ATTACH %s: self-check FAILED, short read of %s\n", modelRaw.c_str(), jsonPath.c_str());
            return false;
        }
    }

    if (!JsonChecker::Validate(jsonText))
    {
        fprintf(stderr, "ATTACH %s: self-check FAILED, %s is not valid JSON\n", modelRaw.c_str(), jsonPath.c_str());
        return false;
    }

    // 2) attachment count matches ADES count
    if (atts.size() != kid->GetAdeList()->size())
    {
        fprintf(stderr, "ATTACH %s: self-check FAILED, attachments %zu != ADES %zu\n",
                modelRaw.c_str(), atts.size(), kid->GetAdeList()->size());
        return false;
    }

    // 3) every referenced file exists
    for (const std::string &fref : referencedFiles)
    {
        if (!FSMgr::iDir::fileExist(vdir + "/" + fref))
        {
            fprintf(stderr, "ATTACH %s: self-check FAILED, referenced file missing: %s\n",
                    modelRaw.c_str(), fref.c_str());
            return false;
        }
    }

    // 4) anm_textures keys must be names embedded in the referenced ANMs
    for (auto &kv : anmTex)
    {
        bool found = false;
        for (auto &av : anmMap)
        {
            if (!av.second->_pData)
                continue;
            for (size_t i = 0; i < av.second->_pData->BitmapNames.size(); ++i)
            {
                if (StriCmp(TrimName(av.second->_pData->BitmapNames[i]), kv.first) == 0)
                {
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
        if (!found)
        {
            fprintf(stderr, "ATTACH %s: self-check FAILED, anm_textures key '%s' not embedded in any referenced ANM\n",
                    modelRaw.c_str(), kv.first.c_str());
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Task 4: --verify2 - losslessness check of the uattach reconstruction
// ---------------------------------------------------------------------------

struct TexSig
{
    bool present = false;
    int w = 0;
    int h = 0;
    uint64_t sum = 0;
};

static TexSig TexSignature(ResBitmap *b)
{
    TexSig s;
    if (!b || !b->swTex)
        return s;

    SDL_Surface *sf = b->swTex;

    s.present = true;
    s.w = sf->w;
    s.h = sf->h;

    SDL_LockSurface(sf);
    int bpp = sf->format->BytesPerPixel;
    int rowBytes = sf->w * bpp;
    const uint8_t *base = (const uint8_t *)sf->pixels;

    uint64_t sum = 0;
    for (int y = 0; y < sf->h; ++y)
    {
        const uint8_t *row = base + (size_t)y * sf->pitch;
        for (int i = 0; i < rowBytes; ++i)
            sum += row[i];
    }
    SDL_UnlockSurface(sf);

    s.sum = sum;
    return s;
}

struct Verify2Ctx
{
    std::string model;
    std::vector<std::string> diffs;
    int sharedHits = 0;

    void Fail(const char *fmt, ...)
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (diffs.size() < 8)
            diffs.push_back(buf);
    }
};

static bool FuzzyEq(float a, float b, float eps = 1e-4)
{
    return fabs(a - b) <= eps;
}

// bmpanim: animType, frame table and the bound sheet bitmaps (by dimension +
// pixel checksum). Also audits that the reconstruction does NOT share the
// original's resources (zero-sharing guarantee).
static void Verify2Bmpanim(Verify2Ctx *c, NC_STACK_bmpanim *lo, NC_STACK_bmpanim *lr)
{
    if (!lo || !lr)
        return;

    if (lo->getBANM_animtype() != lr->getBANM_animtype())
        c->Fail("animType %d != %d", lo->getBANM_animtype(), lr->getBANM_animtype());

    if (!lo->_pData || !lr->_pData)
    {
        if ((bool)lo->_pData != (bool)lr->_pData)
            c->Fail("anm payload missing on one side");
        return;
    }

    if (lo->_pData == lr->_pData)
    {
        c->Fail("SHARED anm payload '%s' with original", lo->getRsrc_name().c_str());
        c->sharedHits++;
    }

    ResBmpAnm *po = lo->_pData;
    ResBmpAnm *pr = lr->_pData;

    if (po->FrameData.size() != pr->FrameData.size())
        c->Fail("anm frames %zu != %zu", po->FrameData.size(), pr->FrameData.size());

    size_t n = MMIN(po->FrameData.size(), pr->FrameData.size());
    for (size_t i = 0; i < n; ++i)
    {
        if (po->FrameData[i].FrameTime != pr->FrameData[i].FrameTime ||
            po->FrameData[i].FrameID != pr->FrameData[i].FrameID ||
            po->FrameData[i].TexCoordsID != pr->FrameData[i].TexCoordsID)
            c->Fail("anm frame %zu data differs", i);
    }

    if (po->Bitmaps.size() != pr->Bitmaps.size())
        c->Fail("anm sheets %zu != %zu", po->Bitmaps.size(), pr->Bitmaps.size());

    n = MMIN(po->Bitmaps.size(), pr->Bitmaps.size());
    for (size_t i = 0; i < n; ++i)
    {
        NC_STACK_bitmap *so = po->Bitmaps[i];
        NC_STACK_bitmap *sr = pr->Bitmaps[i];

        if ((bool)so != (bool)sr)
        {
            c->Fail("anm sheet %zu missing on one side", i);
            continue;
        }
        if (!so)
            continue;

        if (so->GetBitmap() == sr->GetBitmap())
        {
            c->Fail("SHARED anm sheet %zu '%s' with original", i, so->getRsrc_name().c_str());
            c->sharedHits++;
        }

        TexSig a = TexSignature(so->GetBitmap());
        TexSig b = TexSignature(sr->GetBitmap());

        if (a.present != b.present || a.w != b.w || a.h != b.h || a.sum != b.sum)
            c->Fail("anm sheet %zu pixels differ (orig %dx%d sum%llu vs rec %dx%d sum%llu)",
                    i, a.w, a.h, (unsigned long long)a.sum, b.w, b.h, (unsigned long long)b.sum);
    }
}

static void Verify2Texture(Verify2Ctx *c, NC_STACK_bitmap *lo, NC_STACK_bitmap *lr,
                           const char *what)
{
    bool dyn_o = lo && lo->IsDynamic();
    bool dyn_r = lr && lr->IsDynamic();

    if (!lo || !lr)
    {
        if ((bool)lo != (bool)lr)
            c->Fail("%s: texture missing on one side", what);
        return;
    }

    if (dyn_o != dyn_r)
    {
        c->Fail("%s: dynamic flag %d != %d", what, (int)dyn_o, (int)dyn_r);
        return;
    }

    if (dyn_o)
        Verify2Bmpanim(c, dynamic_cast<NC_STACK_bmpanim *>(lo), dynamic_cast<NC_STACK_bmpanim *>(lr));
    else
    {
        if (lo->GetBitmap() == lr->GetBitmap())
        {
            c->Fail("%s: SHARED static texture '%s' with original", what, lo->getRsrc_name().c_str());
            c->sharedHits++;
        }

        TexSig a = TexSignature(lo->GetBitmap());
        TexSig b = TexSignature(lr->GetBitmap());

        if (a.present != b.present || a.w != b.w || a.h != b.h || a.sum != b.sum)
            c->Fail("%s: pixels differ (orig %dx%d sum%llu vs rec %dx%d sum%llu)",
                    what, a.w, a.h, (unsigned long long)a.sum, b.w, b.h, (unsigned long long)b.sum);

        // outline (OTL2) binding
        const std::vector<tUtV> &oo = lo->GetOutline();
        const std::vector<tUtV> &rr = lr->GetOutline();

        if (oo.size() != rr.size())
            c->Fail("%s: outline %zu != %zu entries", what, oo.size(), rr.size());

        size_t n = MMIN(oo.size(), rr.size());
        for (size_t i = 0; i < n; ++i)
        {
            if (!FuzzyEq(oo[i].tu, rr[i].tu, 0.0) || !FuzzyEq(oo[i].tv, rr[i].tv, 0.0))
            {
                c->Fail("%s: outline %zu differs", what, i);
                break;
            }
        }
    }
}

// Compare one ADE (recursively for particle life stages)
static void Verify2Ade(Verify2Ctx *c, NC_STACK_ade *lo, NC_STACK_ade *lr, const char *prefix)
{
    if (!lo || !lr)
    {
        c->Fail("%s: ade missing on one side", prefix);
        return;
    }

    if (StriCmp(lo->ClassName(), lr->ClassName()) != 0)
        c->Fail("%s.class %s != %s", prefix, lo->ClassName().c_str(), lr->ClassName().c_str());

    if (lo->_pointID != lr->_pointID)
        c->Fail("%s.pointID %d != %d", prefix, (int)lo->_pointID, (int)lr->_pointID);
    if (lo->_polyID != lr->_polyID)
        c->Fail("%s.polyID %d != %d", prefix, (int)lo->_polyID, (int)lr->_polyID);

    int commonFlags = NC_STACK_ade::ADE_FLAG_BKCHECK | NC_STACK_ade::ADE_FLAG_DPTHFADE;
    if ((lo->flags & commonFlags) != (lr->flags & commonFlags))
        c->Fail("%s.flags %d != %d", prefix, (int)(lo->flags & commonFlags), (int)(lr->flags & commonFlags));

    NC_STACK_area *ao = dynamic_cast<NC_STACK_area *>(lo);
    NC_STACK_area *ar = dynamic_cast<NC_STACK_area *>(lr);

    if (ao || ar)
    {
        if (!ao || !ar)
        {
            c->Fail("%s: area class mismatch", prefix);
            return;
        }

        if (ao->_polflags != ar->_polflags)
            c->Fail("%s.polFlags %d != %d", prefix, (int)ao->_polflags, (int)ar->_polflags);
        if (ao->_colorVal != ar->_colorVal)
            c->Fail("%s.clrVal %d != %d", prefix, (int)ao->_colorVal, (int)ar->_colorVal);
        if (ao->_tracyVal != ar->_tracyVal)
            c->Fail("%s.trcVal %d != %d", prefix, (int)ao->_tracyVal, (int)ar->_tracyVal);
        if (ao->_shadeVal != ar->_shadeVal)
            c->Fail("%s.shdVal %d != %d", prefix, (int)ao->_shadeVal, (int)ar->_shadeVal);

        Verify2Texture(c, ao->_texImg, ar->_texImg, fmt::sprintf("%s.tex", prefix).c_str());
        Verify2Texture(c, ao->_tracyImg, ar->_tracyImg, fmt::sprintf("%s.tracy", prefix).c_str());
    }

    NC_STACK_amesh *mo = dynamic_cast<NC_STACK_amesh *>(lo);
    NC_STACK_amesh *mr = dynamic_cast<NC_STACK_amesh *>(lr);

    if (mo || mr)
    {
        if (!mo || !mr)
        {
            c->Fail("%s: amesh class mismatch", prefix);
            return;
        }

        if (mo->polyCnt != mr->polyCnt)
            c->Fail("%s.numpoly %d != %d", prefix, (int)mo->polyCnt, (int)mr->polyCnt);

        if (mo->atts.size() != mr->atts.size())
            c->Fail("%s.atts count differs", prefix);

        size_t n = MMIN(mo->atts.size(), mr->atts.size());
        for (size_t i = 0; i < n; ++i)
        {
            const NC_STACK_amesh::ATTS &a = mo->atts[i];
            const NC_STACK_amesh::ATTS &b = mr->atts[i];
            if (a.polyID != b.polyID || a.colorVal != b.colorVal ||
                a.shadeVal != b.shadeVal || a.tracyVal != b.tracyVal || a.pad != b.pad)
            {
                c->Fail("%s.atts[%zu] differs", prefix, i);
                break;
            }
        }

        if (mo->texCoords.size() != mr->texCoords.size())
            c->Fail("%s.olpl groups %zu != %zu", prefix, mo->texCoords.size(), mr->texCoords.size());

        n = MMIN(mo->texCoords.size(), mr->texCoords.size());
        for (size_t i = 0; i < n; ++i)
        {
            const std::vector<tUtV> &a = mo->texCoords[i];
            const std::vector<tUtV> &b = mr->texCoords[i];

            if (a.size() != b.size())
            {
                c->Fail("%s.olpl[%zu] size differs", prefix, i);
                continue;
            }

            for (size_t j = 0; j < a.size(); ++j)
            {
                // compare quantized (the on-disk/exported resolution)
                if ((int)(a[j].tu * 256.0) != (int)(b[j].tu * 256.0) ||
                    (int)(a[j].tv * 256.0) != (int)(b[j].tv * 256.0))
                {
                    c->Fail("%s.olpl[%zu][%zu] differs", prefix, i, j);
                    break;
                }
            }
        }
    }

    NC_STACK_particle *po = dynamic_cast<NC_STACK_particle *>(lo);
    NC_STACK_particle *pr = dynamic_cast<NC_STACK_particle *>(lr);

    if (po || pr)
    {
        if (!po || !pr)
        {
            c->Fail("%s: particle class mismatch", prefix);
            return;
        }

        if (po->_startSpeed != pr->_startSpeed || po->_numContexts != pr->_numContexts ||
            po->_genLifeTime != pr->_genLifeTime || po->_genStart != pr->_genStart ||
            po->_genEnd != pr->_genEnd || po->_genRate != pr->_genRate ||
            po->_lifeTime != pr->_lifeTime)
            c->Fail("%s: particle scalar params differ", prefix);

        if (!FuzzyEq(po->_scaleStart, pr->_scaleStart) || !FuzzyEq(po->_scaleEnd, pr->_scaleEnd) ||
            !FuzzyEq(po->_noisePower, pr->_noisePower))
            c->Fail("%s: particle float params differ", prefix);

        auto v3eq = [](const vec3d &a, const vec3d &b) {
            return FuzzyEq(a.x, b.x) && FuzzyEq(a.y, b.y) && FuzzyEq(a.z, b.z);
        };

        if (!v3eq(po->_accelStart, pr->_accelStart) || !v3eq(po->_accelEnd, pr->_accelEnd) ||
            !v3eq(po->_magnifyStart, pr->_magnifyStart) || !v3eq(po->_magnifyEnd, pr->_magnifyEnd))
            c->Fail("%s: particle accel/magnify differ", prefix);

        if (po->_lifeStagesAdes.size() != pr->_lifeStagesAdes.size())
            c->Fail("%s: stages %zu != %zu", prefix, po->_lifeStagesAdes.size(), pr->_lifeStagesAdes.size());

        size_t n = MMIN(po->_lifeStagesAdes.size(), pr->_lifeStagesAdes.size());
        for (size_t i = 0; i < n; ++i)
            Verify2Ade(c, po->_lifeStagesAdes[i], pr->_lifeStagesAdes[i],
                       fmt::sprintf("%s.stage%zu", prefix, i).c_str());
    }
}

static void Verify2Skeleton(Verify2Ctx *c, NC_STACK_base *lo, NC_STACK_base *lr)
{
    NC_STACK_skeleton *so = lo->GetSkeleton();
    NC_STACK_skeleton *sr = lr->GetSkeleton();

    if (!so || !sr)
    {
        if ((bool)so != (bool)sr)
            c->Fail("skeleton missing on one side");
        return;
    }

    UAskeleton::Data *do_ = so->GetSkelet();
    UAskeleton::Data *dr = sr->GetSkelet();

    if (!do_ || !dr)
    {
        c->Fail("skeleton data missing");
        return;
    }

    if (do_->POO.size() != dr->POO.size())
        c->Fail("skl.POONUM %zu != %zu", do_->POO.size(), dr->POO.size());

    size_t n = MMIN(do_->POO.size(), dr->POO.size());
    for (size_t i = 0; i < n; ++i)
    {
        if (do_->POO[i].x != dr->POO[i].x || do_->POO[i].y != dr->POO[i].y || do_->POO[i].z != dr->POO[i].z)
        {
            c->Fail("skl.POO[%zu] differs", i);
            break;
        }
    }

    if (do_->SEN.size() != dr->SEN.size())
        c->Fail("skl.SENNUM %zu != %zu", do_->SEN.size(), dr->SEN.size());

    if (do_->polygons.size() != dr->polygons.size())
        c->Fail("skl.POLNUM %zu != %zu", do_->polygons.size(), dr->polygons.size());

    n = MMIN(do_->polygons.size(), dr->polygons.size());
    for (size_t i = 0; i < n; ++i)
    {
        const UAskeleton::Polygon &a = do_->polygons[i];
        const UAskeleton::Polygon &b = dr->polygons[i];

        if (a.num_vertices != b.num_vertices)
        {
            c->Fail("skl.poly[%zu].numv %d != %d", i, (int)a.num_vertices, (int)b.num_vertices);
            continue;
        }

        bool same = true;
        for (int j = 0; j < a.num_vertices; ++j)
            if (a.v[j] != b.v[j])
                same = false;

        if (!same)
            c->Fail("skl.poly[%zu].indexes differ", i);
        else if (a.num_vertices >= 3)
        {
            if (!FuzzyEq(a.A, b.A, 1e-5) || !FuzzyEq(a.B, b.B, 1e-5) ||
                !FuzzyEq(a.C, b.C, 1e-5) || !FuzzyEq(a.D, b.D, 1e-4))
                c->Fail("skl.poly[%zu].plane differs", i);
        }
    }
}

static void Verify2Meshes(Verify2Ctx *c, NC_STACK_base *lo, NC_STACK_base *lr)
{
    std::list<GFX::TMesh> *mo = &lo->Meshes;
    std::list<GFX::TMesh> *mr = &lr->Meshes;

    if (mo->size() != mr->size())
    {
        c->Fail("meshes %zu != %zu", mo->size(), mr->size());
        return;
    }

    auto ito = mo->begin();
    auto itr = mr->begin();

    for (size_t mi = 0; ito != mo->end() && itr != mr->end(); ++ito, ++itr, ++mi)
    {
        GFX::TMesh &a = *ito;
        GFX::TMesh &b = *itr;

        if (a.Mat.Flags != b.Mat.Flags)
            c->Fail("mesh[%zu].flags %x != %x", mi, a.Mat.Flags, b.Mat.Flags);

        if (memcmp(&a.Mat.Color, &b.Mat.Color, sizeof(GFX::TGLColor)) != 0)
            c->Fail("mesh[%zu].color differs", mi);

        if (a.Vertexes.size() != b.Vertexes.size())
        {
            c->Fail("mesh[%zu].vertexes %zu != %zu", mi, a.Vertexes.size(), b.Vertexes.size());
            continue;
        }

        if (a.Indixes.size() != b.Indixes.size())
            c->Fail("mesh[%zu].indixes %zu != %zu", mi, a.Indixes.size(), b.Indixes.size());

        bool posFail = false, tcFail = false, clrFail = false, tcidFail = false;

        for (size_t vi = 0; vi < a.Vertexes.size(); ++vi)
        {
            const GFX::TVertex &va = a.Vertexes[vi];
            const GFX::TVertex &vb = b.Vertexes[vi];

            if (!FuzzyEq(va.Pos.x, vb.Pos.x) || !FuzzyEq(va.Pos.y, vb.Pos.y) || !FuzzyEq(va.Pos.z, vb.Pos.z))
                posFail = true;

            if (va.TexCoordId != vb.TexCoordId)
                tcidFail = true;

            if (memcmp(&va.Color, &vb.Color, sizeof(GFX::TGLColor)) != 0)
                clrFail = true;

            if (!FuzzyEq(va.TexCoord.tu, vb.TexCoord.tu, 0.0) ||
                !FuzzyEq(va.TexCoord.tv, vb.TexCoord.tv, 0.0))
                tcFail = true;
        }

        if (posFail)
            c->Fail("mesh[%zu].positions differ", mi);
        if (tcidFail)
            c->Fail("mesh[%zu].texcoordids differ", mi);
        if (clrFail)
            c->Fail("mesh[%zu].vertex colors differ", mi);
        if (tcFail)
            c->Fail("mesh[%zu].texcoords differ", mi);

        bool dyn = (a.Mat.Flags & GFX::RFLAGS_DYNAMIC_TEXTURE) != 0;

        if (dyn)
        {
            if (a.CoordsCache.size() != b.CoordsCache.size())
                c->Fail("mesh[%zu].coordcache %zu != %zu", mi, a.CoordsCache.size(), b.CoordsCache.size());

            size_t n = MMIN(a.CoordsCache.size(), b.CoordsCache.size());
            for (size_t fi = 0; fi < n; ++fi)
            {
                TexSig ta = TexSignature(a.CoordsCache[fi].Tex);
                TexSig tb = TexSignature(b.CoordsCache[fi].Tex);

                if (ta.present != tb.present || ta.w != tb.w || ta.h != tb.h || ta.sum != tb.sum)
                    c->Fail("mesh[%zu].frame[%zu] texture differs", mi, fi);

                const std::vector<tUtV> &ca = a.CoordsCache[fi].Coords;
                const std::vector<tUtV> &cb = b.CoordsCache[fi].Coords;

                if (ca.size() != cb.size())
                {
                    c->Fail("mesh[%zu].frame[%zu] coords size differs", mi, fi);
                    continue;
                }

                for (size_t j = 0; j < ca.size(); ++j)
                {
                    if (!FuzzyEq(ca[j].tu, cb[j].tu, 0.0) || !FuzzyEq(ca[j].tv, cb[j].tv, 0.0))
                    {
                        c->Fail("mesh[%zu].frame[%zu] coords differ", mi, fi);
                        break;
                    }
                }
            }
        }
    }
}

static bool RunVerify2(NC_STACK_base *rootBase, const std::string &outDir, const std::string &only)
{
    // Mount the export dir as "export" (scanned, so it can be read)
    FSMgr::iDir *root = FSMgr::iDir::GetRoot();
    root->addNode(new FSMgr::iDir("export", outDir));
    FSMgr::iDir::replacePath("export", outDir);

    const std::string vdir = "export";

    int nOK = 0;
    int nTotal = 0;
    int nSkippedEmpty = 0;

    for ( NC_STACK_base *group : rootBase->GetKidList() )
    {
        for ( NC_STACK_base *kid : group->GetKidList() )
        {
            std::string kidName = kid->getName();

            if (!only.empty() && StriCmp(kidName, only) != 0)
                continue;

            if ( kid->Meshes.empty() )
            {
                ++nSkippedEmpty;
                continue;
            }

            ++nTotal;

            Verify2Ctx c;
            c.model = kidName;

            NC_STACK_Obj3D *recon = NC_STACK_Obj3D::LoadObjWithAttachments(vdir + "/" + kidName + ".obj");

            if (!recon)
            {
                c.Fail("LOADFAIL");
            }
            else
            {
                Verify2Skeleton(&c, kid, recon);
                Verify2Meshes(&c, kid, recon);

                std::vector<NC_STACK_ade *> ao(kid->GetAdeList()->begin(), kid->GetAdeList()->end());
                std::vector<NC_STACK_ade *> ar(recon->GetAdeList()->begin(), recon->GetAdeList()->end());

                if (ao.size() != ar.size())
                    c.Fail("ades %zu != %zu", ao.size(), ar.size());

                size_t n = MMIN(ao.size(), ar.size());
                for (size_t i = 0; i < n; ++i)
                    Verify2Ade(&c, ao[i], ar[i], fmt::sprintf("ade%zu", i).c_str());

                recon->Delete();
            }

            if ( c.diffs.empty() )
            {
                ++nOK;
                printf("VERIFY2 %s: OK\n", kidName.c_str());
            }
            else
            {
                std::string joined;
                for (size_t i = 0; i < c.diffs.size(); ++i)
                    joined += (i ? "; " : "") + c.diffs[i];
                printf("VERIFY2 %s: DIFF %s\n", kidName.c_str(), joined.c_str());
            }

            fflush(stdout);
        }
    }

    printf("VERIFY2 %d/%d (empty models skipped: %d)\n", nOK, nTotal, nSkippedEmpty);
    fflush(stdout);

    return nOK == nTotal;
}

// Task 7: --renderdump - render-node level comparison
// ---------------------------------------------------------------------------
//
// For every listed model this loads it TWICE in one process:
//   (a) the original kid straight from the SET.BAS blob (rootBase),
//   (b) the converted game-path model via Utils::ProxyLoadBase.
// For both it drives NC_STACK_base::Render exactly the way the lego path in
// yw_game.cpp does (several variants) and dumps every GFX::TRenderNode that
// lands in the GFXEngine queues, field by field. Mismatches between (a) and
// (b) are printed as "RD <model> <field>: orig=X conv=Y".
//
// Pointer identities are compared by CONTENT (texture dims + pixel hash,
// mesh contents); Distance is not compared (viewpoint is fixed anyway).

struct RDNodeDump
{
    std::string Type;
    std::string Flags;
    std::string Color;
    std::string Tex;        // incl. hwTex null/non-null
    std::string CoordsID;
    std::string CoordsCacheCnt;
    std::string MeshIdent;  // shared vs LocalMesh
    std::string TForm;      // rounded
    std::string Fog;
    std::string Counts;     // vcount/icount
    std::string Verts;      // first 3 verts
};

static std::string RDFlagsNames(uint32_t flags)
{
    static const struct { uint32_t bit; const char *name; } bits[] =
    {
        { GFX::RFLAGS_TEXTURED,        "TEXTURED" },
        { GFX::RFLAGS_DYNAMIC_TEXTURE, "DYNTex" },
        { GFX::RFLAGS_SHADED,          "SHADED" },
        { GFX::RFLAGS_FOG,             "FOG" },
        { GFX::RFLAGS_ZEROTRACY,       "ZEROTRACY" },
        { GFX::RFLAGS_LUMTRACY,        "LUMTRACY" },
        { GFX::RFLAGS_SKY,             "SKY" },
        { GFX::RFLAGS_FALLOFF,         "FALLOFF" },
        { GFX::RFLAGS_IGNORE_FALLOFF,  "IGFALLOFF" },
        { GFX::RFLAGS_COMPUTED_COLOR,  "COMPCOLOR" },
        { GFX::RFLAGS_DISABLE_ZWRITE,  "NOZWRITE" },
    };

    std::string out;
    for (const auto &b : bits)
    {
        if (flags & b.bit)
        {
            if (!out.empty())
                out += "|";
            out += b.name;
        }
    }

    uint32_t unknown = flags;
    for (const auto &b : bits)
        unknown &= ~b.bit;
    if (unknown)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "|UNKNOWN=0x%x", unknown);
        out += buf;
    }

    return out;
}

static uint64_t RDHash64(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static std::string RDTexDump(ResBitmap *tex)
{
    if (!tex)
        return "null";

    char buf[256];
    char palbuf[64] = "";
    uint64_t hash = 0;
    int bpp = 0;

    if (tex->swTex)
    {
        SDL_LockSurface(tex->swTex);
        bpp = tex->swTex->format->BytesPerPixel;
        // first 64 bytes of the first pixel row
        size_t n = MMIN((size_t)64, (size_t)tex->swTex->pitch);
        hash = RDHash64((const uint8_t *)tex->swTex->pixels, n);
        SDL_UnlockSurface(tex->swTex);

        // palette hash: the renderer consumes the surface palette via
        // AllocTexture/ConvertSDLSurface when building hwTex, and VERIFY2's
        // pixel checksums never compared it
        if (tex->swTex->format->palette)
        {
            SDL_Palette *pal = tex->swTex->format->palette;
            int cnt = MMIN(pal->ncolors, 256);
            std::string a0list;
            int yellow = 0;
            for (int i = 0; i < cnt; ++i)
            {
                if (pal->colors[i].a == 0)
                {
                    char e[32];
                    snprintf(e, sizeof(e), "%s%d(r%u,g%u,b%u)", a0list.empty() ? "" : ",", i,
                             (unsigned)pal->colors[i].r, (unsigned)pal->colors[i].g,
                             (unsigned)pal->colors[i].b);
                    a0list += e;
                }
                if (pal->colors[i].r == 255 && pal->colors[i].g == 255 && pal->colors[i].b == 0)
                    ++yellow;
            }
            snprintf(palbuf, sizeof(palbuf), " pal=%d:a0=[%s] yellow=%d", cnt, a0list.c_str(), yellow);
        }

        if (tex->palette)
        {
            const SDL_Color &c0 = (*tex->palette)[0];
            snprintf(palbuf + strlen(palbuf), sizeof(palbuf) - strlen(palbuf),
                     " bpal0=(%u,%u,%u,%u)", (unsigned)c0.r, (unsigned)c0.g,
                     (unsigned)c0.b, (unsigned)c0.a);
        }
    }

    snprintf(buf, sizeof(buf), "%dx%d bpp=%d hash=%016llx hw=%u%s",
             tex->swTex ? tex->swTex->w : -1,
             tex->swTex ? tex->swTex->h : -1,
             bpp,
             (unsigned long long)hash,
             (unsigned)tex->hwTex ? 1 : 0,
             palbuf);
    return buf;
}

static std::string RDColorDump(const GFX::TGLColor &c)
{
    uint32_t r, g, b, a;
    memcpy(&r, &c.r, 4); memcpy(&g, &c.g, 4);
    memcpy(&b, &c.b, 4); memcpy(&a, &c.a, 4);
    char buf[128];
    snprintf(buf, sizeof(buf), "r=%08x g=%08x b=%08x a=%08x", r, g, b, a);
    return buf;
}

static std::string RDTFormDump(const mat4x4 &m)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "[%d %d %d %d; %d %d %d %d; %d %d %d %d; %d %d %d %d]",
             (int)llround(m.m00), (int)llround(m.m01), (int)llround(m.m02), (int)llround(m.m03),
             (int)llround(m.m10), (int)llround(m.m11), (int)llround(m.m12), (int)llround(m.m13),
             (int)llround(m.m20), (int)llround(m.m21), (int)llround(m.m22), (int)llround(m.m23),
             (int)llround(m.m30), (int)llround(m.m31), (int)llround(m.m32), (int)llround(m.m33));
    return buf;
}

static std::string RDVertsDump(GFX::TMesh *mesh)
{
    char buf[512];
    std::string out;

    size_t n = MMIN((size_t)3, mesh->Vertexes.size());
    for (size_t i = 0; i < n; ++i)
    {
        const GFX::TVertex &v = mesh->Vertexes[i];
        uint32_t r, g, b, a;
        memcpy(&r, &v.Color.r, 4); memcpy(&g, &v.Color.g, 4);
        memcpy(&b, &v.Color.b, 4); memcpy(&a, &v.Color.a, 4);
        snprintf(buf, sizeof(buf), "V%zu=(%d,%d,%d) clr=%08x/%08x/%08x/%08x uv=(%.4f,%.4f) ",
                 i,
                 (int)llround(v.Pos.x), (int)llround(v.Pos.y), (int)llround(v.Pos.z),
                 r, g, b, a, v.TexCoord.tu, v.TexCoord.tv);
        out += buf;
    }
    return out;
}

static RDNodeDump RDNodeCapture(GFX::TRenderNode *nod)
{
    RDNodeDump d;

    d.Type = fmt::sprintf("%d", (int)nod->Type);
    d.Flags = fmt::sprintf("0x%x (%s)", nod->Flags, RDFlagsNames(nod->Flags).c_str());
    d.Color = RDColorDump(nod->Color);
    d.Tex = RDTexDump(nod->Tex);
    d.CoordsID = fmt::sprintf("%d", nod->coordsID);

    GFX::TMesh *mesh = nod->Mesh;
    d.CoordsCacheCnt = fmt::sprintf("%zu", mesh ? mesh->CoordsCache.size() : (size_t)0);
    d.MeshIdent = (nod->Mesh == &nod->LocalMesh) ? "local" : "shared";
    d.TForm = RDTFormDump(nod->TForm);
    d.Fog = fmt::sprintf("%.2f/%.2f", nod->FogStart, nod->FogLength);
    d.Counts = fmt::sprintf("v=%zu i=%zu", mesh ? mesh->Vertexes.size() : (size_t)0,
                            mesh ? mesh->Indixes.size() : (size_t)0);
    d.Verts = mesh ? RDVertsDump(mesh) : "nomesh";

    return d;
}

// Replicates the state logic of GFXEngine::RenderingMesh (gfx.cpp) for a
// node, using the REAL default game configuration:
//   gfx.blending=0    -> can_srcblend=1, can_destblend=0, can_stippling=0
//   gfx.zbuf_when_tracy=false -> _zbuf_when_tracy = 0
//   pixfmt bpp = 4 (32bit ARGB screen)
// The VBO (_vbo) branch is RenderingMesh; RenderingMeshOld is the fallback.
static void RDStateTable(const char *model, GFX::TRenderNode *nod, const char *tag)
{
    GFX::TMesh *mesh = nod->Mesh;
    uint32_t flags = nod->Flags;

    bool can_srcblend = true, can_destblend = false, can_stippling = false;
    bool zbuf_when_tracy = false;
    int pixfmt_bpp = 4;

    // defaults (top of RenderingMesh)
    std::string shaded = "0", stipple = "0", texblend = "REPLACE(0)";
    std::string srcblend = "GL_ONE", dstblend = "GL_ZERO";
    std::string alphablend = "0", zwrite = "1", fog = "0", atest = "0";
    std::string linear = "(_filter!=0)";

    std::string tex = "0";
    if ((flags & GFX::RFLAGS_TEXTURED) && nod->Tex)
        tex = fmt::sprintf("hwTex=%u(%s)", (unsigned)nod->Tex->hwTex ? 1 : 0,
                           (unsigned)nod->Tex->hwTex ? "uploaded" : "NULL!");

    if (flags & GFX::RFLAGS_SHADED)
    {
        texblend = "MODULATE(2)";
        shaded = "1";
    }

    if (flags & GFX::RFLAGS_FOG)
        fog = fmt::sprintf("1 start=%.1f len=%.1f", nod->FogStart, nod->FogLength);

    if (flags & GFX::RFLAGS_LUMTRACY)
    {
        if (!zbuf_when_tracy)
            zwrite = "0";

        // blending=0: can_srcblend only
        alphablend = "1";
        texblend = "MODULATEALPHA(1)";
        srcblend = "GL_SRC_ALPHA";
        dstblend = "GL_ONE_MINUS_SRC_ALPHA";
        shaded = "0";
    }
    else if (flags & GFX::RFLAGS_ZEROTRACY)
    {
        atest = "1";
        if (pixfmt_bpp != 1)
        {
            srcblend = "GL_SRC_ALPHA";
            dstblend = "GL_ONE_MINUS_SRC_ALPHA";
        }
        alphablend = "1";
        linear = "0";
        texblend = "MODULATE(2)";
    }

    std::string falloffExtra;
    if (!(flags & GFX::RFLAGS_SKY) && (flags & GFX::RFLAGS_FALLOFF))
    {
        zwrite = "1";
        alphablend = "1";
        texblend = "MODULATE(2)";
        srcblend = "GL_SRC_ALPHA";
        dstblend = (flags & GFX::RFLAGS_LUMTRACY) ? "GL_ONE" : "GL_ONE_MINUS_SRC_ALPHA";
        shaded = "1";
        stipple = "0";
        fog = "0";
        falloffExtra = fmt::sprintf(" AFog=1 AFogStart=GfxSkyDistance AFogLen=GfxSkyLength");
    }

    if (flags & GFX::RFLAGS_DISABLE_ZWRITE)
        zwrite = "0";

    printf("STATE %s [%s] mesh flags=0x%x (%s)\n", model, tag, flags, RDFlagsNames(flags).c_str());
    printf("STATE   Tex=%s TexBlend=%s Shaded=%s Stipple=%s AlphaBlend=%s SrcBlend=%s DstBlend=%s Zwrite=%s Fog=%s ATest=%s LinearFilter=%s%s\n",
           tex.c_str(), texblend.c_str(), shaded.c_str(), stipple.c_str(),
           alphablend.c_str(), srcblend.c_str(), dstblend.c_str(),
           zwrite.c_str(), fog.c_str(), atest.c_str(), linear.c_str(),
           falloffExtra.c_str());
    printf("STATE   queue: %s\n",
           (flags & GFX::RFLAGS_SKY) ? "SkyBoxList" :
           (flags & GFX::RFLAGS_ZEROTRACY) ? "ZeroTracyList" :
           (flags & GFX::RFLAGS_LUMTRACY) ? "LumaTracyList" : "SolidList");
    printf("STATE   mesh: v=%zu i=%zu glDataBuf=%u glIndexBuf=%u VBOpath=%s\n",
           mesh->Vertexes.size(), mesh->Indixes.size(),
           (unsigned)mesh->glDataBuf ? 1 : 0, (unsigned)mesh->glIndexBuf ? 1 : 0,
           "_vbo(gfx.vbo default true) -> RenderingMesh (shader/VBO)");
    fflush(stdout);
}

// Collect the render nodes that base->Render() queued since `before`
static std::vector<GFX::TRenderNode *> RDCollectQueued(size_t before[4])
{
    std::vector<GFX::TRenderNode *> out;
    GFX::GFXEngine::Instance._renderSolidList;
    auto grab = [&out](std::list<GFX::TRenderNode *> &l, size_t b)
    {
        size_t i = 0;
        for (GFX::TRenderNode *n : l)
            if (i++ >= b)
                out.push_back(n);
    };

    grab(GFX::GFXEngine::Instance._renderSolidList, before[0]);
    grab(GFX::GFXEngine::Instance._renderSkyBoxList, before[1]);
    grab(GFX::GFXEngine::Instance._renderZeroTracyList, before[2]);
    grab(GFX::GFXEngine::Instance._renderLumaTracyList, before[3]);
    return out;
}

static void RDSnapshotCounts(size_t out[4])
{
    out[0] = GFX::GFXEngine::Instance._renderSolidList.size();
    out[1] = GFX::GFXEngine::Instance._renderSkyBoxList.size();
    out[2] = GFX::GFXEngine::Instance._renderZeroTracyList.size();
    out[3] = GFX::GFXEngine::Instance._renderLumaTracyList.size();
}

static void RDDropQueued(std::vector<GFX::TRenderNode *> &nodes)
{
    for (GFX::TRenderNode *n : nodes)
    {
        GFX::GFXEngine::Instance._renderSolidList.remove(n);
        GFX::GFXEngine::Instance._renderSkyBoxList.remove(n);
        GFX::GFXEngine::Instance._renderZeroTracyList.remove(n);
        GFX::GFXEngine::Instance._renderLumaTracyList.remove(n);
    }
}

struct RDVariant
{
    const char *name;
    bool doCopy;
    bool useInstance;
};

static const RDVariant RDVariants[] =
{
    { "plain",  false, false }, // normal lego path: static + SetPosition only
    { "elev",   false, false }, // constructing path: SetStatic(false) + Y scale 1.7
    { "copy",   true,  false }, // same, doCopy=true (LocalMesh copy)
    { "inst",   false, true  }, // CheckOpts + Render(arg, inst)
};

static const char *RDDefaultModels[] =
{
    // broken in-game: road / mountain sector lego models
    // sector 59 "GS BigBerg" -> subsector 25 -> lego 60
    "GS_BIG",
    // road lego models referenced by sectors 66-72 (set.sdf sektors section):
    //   lego 29/71 ST_STRE1 (N-S road), 30/72 ST_STRE3 (ramp N),
    //   73/74 ST_STRE4/5, 245 ST_NSTR0, 21 ST_STREA, 25 ST_STREB,
    //   184 ST_STRE7 (subsec 76), 152-156 ST_E_B1..C2 (road landscape parts)
    "ST_STRE1", "ST_STRE3", "ST_STRE4", "ST_STRE5",
    "ST_NSTR0", "ST_STREA", "ST_STREB", "ST_STRE7",
    "ST_E_B1", "ST_E_B2", "ST_E_B3", "ST_E_C1", "ST_E_C2",
    // known-good controls
    "GS_EMPTY",
    "S00H",       // slurp filler
    "ST_SLUM2",
};

static void RDRenderOne(NC_STACK_base *bas, const RDVariant &v,
                        const vec3d &pos, baseRender_msg &arg,
                        std::vector<GFX::TRenderNode *> *out)
{
    size_t before[4];
    RDSnapshotCounts(before);

    bas->SetPosition(pos);

    if (strcmp(v.name, "plain") == 0)
    {
        // yw_parse_lego does SetStatic(true) once; normal sector render
        // only calls SetPosition (yw_game.cpp else-branch)
        bas->SetStatic(true);
    }
    else
    {
        // constructing path (yw_game.cpp:1560-1571)
        bas->SetStatic(false);
        bas->SetScale(vec3d(1.7), NC_STACK_base::UF_Y);
    }

    if (v.useInstance)
    {
        NC_STACK_base::Instance *inst = NULL;
        NC_STACK_base::CheckOpts(&inst, bas);
        bas->Render(&arg, inst);
        delete inst;
    }
    else
    {
        bas->Render(&arg, NULL, v.doCopy);
    }

    *out = RDCollectQueued(before);
}

static bool RunRenderDump(NC_STACK_base *rootBase, const std::vector<std::string> &models)
{
    // Fixed viewpoint so Distance/TForm are comparable
    TF::Engine.Init();

    static TF::TForm3D view;
    view.Scale = vec3d(1.0, 1.0, 1.0);
    view.SclRot = mat3x3::Ident();
    view.Pos = vec3d(8300.0, 500.0, -4900.0);
    view.CalcGlobal();
    TF::Engine.SetViewPoint(&view);

    const vec3d pos(7800.0, 0.0, -5400.0);

    baseRender_msg arg;
    arg.frameTime = 16;
    arg.globTime = 1000;
    arg.minZ = 1.0;
    arg.maxZ = 3500.0;
    arg.flags = 0;

    int nOK = 0;
    int nTotal = 0;

    for (const std::string &name : models)
    {
        printf("==== RD %s ====\n", name.c_str());

        // (a) original kid from the blob
        NC_STACK_base *orig = NULL;
        for (NC_STACK_base *group : rootBase->GetKidList())
        {
            for (NC_STACK_base *kid : group->GetKidList())
            {
                if (StriCmp(kid->getName(), name) == 0)
                {
                    if (!kid->Meshes.empty())
                    {
                        orig = kid;
                        break;
                    }
                    printf("RD %s: blob kid empty, skipped\n", name.c_str());
                }
            }
            if (orig)
                break;
        }

        // (b) converted game-path model
        NC_STACK_base *conv = Utils::ProxyLoadBase(
            fmt::sprintf("rsrc:objects/%s.obj", name));
        if (conv && conv->Meshes.empty())
        {
            printf("RD %s: converted model empty\n", name.c_str());
            conv->Delete();
            conv = NULL;
        }

        if (!orig || !conv)
        {
            printf("RD %s: LOAD orig=%p conv=%p SKIPPED\n", name.c_str(), (void *)orig, (void *)conv);
            if (conv)
                conv->Delete();
            ++nTotal;
            continue;
        }

        ++nTotal;
        int mismatches = 0;

        for (const RDVariant &v : RDVariants)
        {
            std::vector<GFX::TRenderNode *> no, nc;
            RDRenderOne(orig, v, pos, arg, &no);
            RDRenderOne(conv, v, pos, arg, &nc);

            printf("RD %s [%s]: orig nodes=%zu conv nodes=%zu\n",
                   name.c_str(), v.name, no.size(), nc.size());

            if (no.size() != nc.size())
            {
                printf("RD %s nodecount: orig=%zu conv=%zu\n", name.c_str(), no.size(), nc.size());
                mismatches++;
            }

            size_t n = MMIN(no.size(), nc.size());
            for (size_t i = 0; i < n; ++i)
            {
                RDNodeDump a = RDNodeCapture(no[i]);
                RDNodeDump b = RDNodeCapture(nc[i]);

                // full dump (orig side; conv printed only on mismatch)
                printf("RD %s [%s] node%zu orig: TYPE=%s FLAGS=%s COLOR=%s TEX=%s CID=%s CC=%s MESH=%s TF=%s FOG=%s %s %s\n",
                       name.c_str(), v.name, i, a.Type.c_str(), a.Flags.c_str(),
                       a.Color.c_str(), a.Tex.c_str(), a.CoordsID.c_str(),
                       a.CoordsCacheCnt.c_str(), a.MeshIdent.c_str(),
                       a.TForm.c_str(), a.Fog.c_str(), a.Counts.c_str(), a.Verts.c_str());

                const struct { const char *n; const std::string &ao, &ac; } fields[] =
                {
                    { "TYPE", a.Type, b.Type },
                    { "Flags", a.Flags, b.Flags },
                    { "Color", a.Color, b.Color },
                    { "Tex", a.Tex, b.Tex },
                    { "coordsID", a.CoordsID, b.CoordsID },
                    { "CoordsCacheCnt", a.CoordsCacheCnt, b.CoordsCacheCnt },
                    { "MeshIdent", a.MeshIdent, b.MeshIdent },
                    { "TForm", a.TForm, b.TForm },
                    { "Fog", a.Fog, b.Fog },
                    { "Counts", a.Counts, b.Counts },
                    { "Verts", a.Verts, b.Verts },
                };

                for (const auto &f : fields)
                {
                    if (f.ao != f.ac)
                    {
                        printf("RD %s [%s] node%zu %s: orig=%s conv=%s\n",
                               name.c_str(), v.name, i, f.n, f.ao.c_str(), f.ac.c_str());
                        mismatches++;
                    }
                }
            }

            // state table for the first node (plain-ish variants only, once per model below)
            if (!no.empty() && strcmp(v.name, "plain") == 0)
                RDStateTable(name.c_str(), no[0], "orig");
            if (!nc.empty() && strcmp(v.name, "plain") == 0)
                RDStateTable(name.c_str(), nc[0], "conv");

            RDDropQueued(no);
            RDDropQueued(nc);
        }

        orig->SetScale(vec3d(1.0, 1.0, 1.0), NC_STACK_base::UF_Y);
        conv->SetScale(vec3d(1.0, 1.0, 1.0), NC_STACK_base::UF_Y);

        if (mismatches)
            printf("RD %s: %d MISMATCH(ES)\n", name.c_str(), mismatches);
        else
            printf("RD %s: no mismatches\n", name.c_str());

        ++nOK;
        fflush(stdout);

        conv->Delete();
    }

    printf("RD DONE %d/%d models processed\n", nOK, nTotal);
    fflush(stdout);
    return true;
}

// ---------------------------------------------------------------------------
// Task 8: --renderpng - real PIXEL rendering comparison
//
// Same setup as --renderdump (blob kid vs game-path converted model), but the
// queued TRenderNodes are actually DRAWN through the engine's real frame
// path:
//
//   GFX::Engine.BeginFrame()              (clear + viewport, gfx.cpp:1857)
//   <queue: NC_STACK_base::Render>        (exactly the lego render path)
//   GFX::Engine.BeginScene()              (state + projection, gfx.cpp:1803)
//   GFX::Engine.Rasterize()               (real draw pass, gfx.cpp:1430)
//   GFX::Engine.EndScene()                (gfx.cpp:1814)
//   glReadPixels -> PNG
//
// This is the same order the game uses (yw_game.cpp:1947-1954: meshes are
// queued by RenderSector, then BeginScene/Rasterize/EndScene).
//
// View setup: the same view point as --renderdump (8300,500,-4900), rotated
// with the engine's euler angles so the camera actually LOOKS AT the model
// (ax=215.26deg / ay=-45deg map the camera->model direction to the engine's
// +Z-forward view space and keep world +Y pointing at the top of the screen;
// derived from TForm3D::scale_rot_3 + UAFrustum in matrix.h:668).
// ---------------------------------------------------------------------------

static const char *RPDefaultModels[] =
{
    "GS_EMPTY",
    "GS_STMF1",
    "ST_NSTR0",
    "ST_STRE1",
    "S00H",
};

// GL init for the tool: SDL video driver from UA_EXPORT_VIDEO (default
// "offscreen" for this mode), real window + GL context via System::Init, then
// the engine's own GFX::Engine.Init (window surface, shaders, palette).
static bool RPInitGL()
{
    const char *vid = getenv("UA_EXPORT_VIDEO");
    setenv("SDL_VIDEODRIVER", (vid && *vid) ? vid : "offscreen", 1);

    System::IniConf::ReadFromNucleusIni();
    bool gfxVbo = System::IniConf::GfxVBO.Get<bool>();

    // same call order as the game (main.cpp:817-819)
    System::Init(!gfxVbo);

    SDL_GLContext ctx = SDL_GL_GetCurrentContext();
    const char *glver = (const char *)glGetString(GL_VERSION);
    if (!ctx || SDL_GL_GetCurrentWindow() == NULL || !glver)
    {
        fprintf(stderr,
                "RPNG: FAILED to get a GL context (SDL video driver '%s', error: %s)\n"
                "RPNG: hints: try UA_EXPORT_VIDEO=offscreen with LIBGL_ALWAYS_SOFTWARE=1,\n"
                "RPNG:      or EGL_PLATFORM=surfaceless (Mesa swrast).\n",
                getenv("SDL_VIDEODRIVER") ? getenv("SDL_VIDEODRIVER") : "?",
                SDL_GetError());
        return false;
    }

    printf("RPNG: GL context OK, driver=%s renderer=%s GL=%s\n",
           getenv("SDL_VIDEODRIVER") ? getenv("SDL_VIDEODRIVER") : "?",
           (const char *)glGetString(GL_RENDERER), glver);

    GFX::Engine.Init();

    // Culling state = the engine's own (CCW front, cull on, gfx.cpp:355).
    // The earlier glFrontFace(GL_CW) "calibration" was compensating for a
    // wrong-side view basis; with the corrected basis the engine default
    // renders every model correctly. UA_EXPORT_NOCULL=1 disables culling
    // entirely (debug).
    if ( !getenv("UA_EXPORT_NOCULL") )
        glEnable(GL_CULL_FACE);

    if ( !GFX::Engine.LoadPalette("palette/standard.pal") )
        fprintf(stderr, "RPNG: warning: could not load palette/standard.pal\n");

    TF::Engine.Init();

    return true;
}

struct RPStats
{
    size_t nonBg = 0;
    double mean[4] = {0, 0, 0, 0};
};

static RPStats RPStatsCalc(const std::vector<uint8_t> &buf, int w, int h)
{
    RPStats st;
    uint64_t sum[4] = {0, 0, 0, 0};

    // background = clear color; the offscreen default framebuffer reports
    // alpha=255 everywhere, so detect background by RGB only
    for (int i = 0; i < w * h; ++i)
    {
        const uint8_t *p = &buf[i * 4];
        if (p[0] | p[1] | p[2])
        {
            st.nonBg++;
            for (int c = 0; c < 4; ++c)
                sum[c] += p[c];
        }
    }

    for (int c = 0; c < 4; ++c)
        st.mean[c] = st.nonBg ? (double)sum[c] / st.nonBg : 0.0;

    return st;
}

static size_t RPDiffCount(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
    size_t n = MMIN(a.size(), b.size());
    size_t diff = 0;
    for (size_t i = 0; i < n; i += 4)
    {
        if (a[i] != b[i] || a[i+1] != b[i+1] || a[i+2] != b[i+2] || a[i+3] != b[i+3])
            ++diff;
    }
    return diff;
}

// glReadPixels gives bottom-up rows; flip into an SDL surface and save
static bool RPSavePng(const std::vector<uint8_t> &buf, int w, int h, const std::string &vpath)
{
    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!sf)
        return false;

    SDL_LockSurface(sf);
    for (int y = 0; y < h; ++y)
        memcpy((uint8_t *)sf->pixels + (size_t)y * sf->pitch,
               &buf[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    SDL_UnlockSurface(sf);

    ResBitmap rb;
    rb.width = w;
    rb.height = h;
    rb.swTex = sf;

    bool ok = NC_STACK_image::SavePng(&rb, vpath);
    SDL_FreeSurface(sf);
    return ok;
}

// One real frame: queue the given entities (each with its own position and
// variant), then run the engine draw pass, then read the pixels back.
struct RPEntity
{
    NC_STACK_base *bas;
    vec3d pos;
    const RDVariant *var;
};

static void RPDrawFrame(const std::vector<RPEntity> &ents, const baseRender_msg &tmpl,
                        std::vector<uint8_t> *outPix, int *ow, int *oh)
{
    bool trace = getenv("UA_EXPORT_TRACE") != NULL;
    if (trace) fprintf(stderr, "TRACE: BeginFrame\n");
    GFX::Engine.BeginFrame();
    if (trace) fprintf(stderr, "TRACE: post-BeginFrame GLerr=0x%x\n", (unsigned)glGetError());

    baseRender_msg arg = tmpl;
    arg.adeCount = 0;

    for (const RPEntity &e : ents)
    {
        std::vector<GFX::TRenderNode *> nodes;
        if (trace) fprintf(stderr, "TRACE: Render %p\n", (void *)e.bas);
        RDRenderOne(e.bas, *e.var, e.pos, arg, &nodes);
        if (trace && !nodes.empty())
            fprintf(stderr, "TRACE: node0 TForm=%s Distance=%.1f\n",
                    RDTFormDump(nodes[0]->TForm).c_str(), nodes[0]->Distance);
        // nodes stay queued; Rasterize consumes them (EndScene clears the rest)
    }

    if (trace) fprintf(stderr, "TRACE: BeginScene\n");
    GFX::Engine.BeginScene();
    if (trace) fprintf(stderr, "TRACE: pre-draw GLerr=0x%x\n", (unsigned)glGetError());

    if (trace) fprintf(stderr, "TRACE: Rasterize\n");
    GFX::Engine.Rasterize();
    if (trace) fprintf(stderr, "TRACE: post-rasterize GLerr=0x%x\n", (unsigned)glGetError());
    GFX::Engine.EndScene();
    if (trace) fprintf(stderr, "TRACE: readpixels\n");

    Common::Point res = System::GetResolution();
    outPix->resize((size_t)res.x * res.y * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, res.x, res.y, GL_RGBA, GL_UNSIGNED_BYTE, outPix->data());
    *ow = res.x;
    *oh = res.y;
    if (trace) fprintf(stderr, "TRACE: frame done\n");
}

static void RPPrintStats(const char *name, const std::vector<uint8_t> &buf, int w, int h)
{
    RPStats st = RPStatsCalc(buf, w, h);
    printf("RPNG %-24s %dx%d nonbg=%zu mean=(%.1f,%.1f,%.1f,%.1f)\n",
           name, w, h, st.nonBg, st.mean[0], st.mean[1], st.mean[2], st.mean[3]);

    // 8x8 band occupancy (pixels with any nonzero RGB) to locate content
    for (int by = 0; by < 8; ++by)
    {
        printf("RPNG   row%-2d ", by);
        for (int bx = 0; bx < 8; ++bx)
        {
            size_t cnt = 0;
            for (int y = by * h / 8; y < (by + 1) * h / 8; ++y)
            {
                for (int x = bx * w / 8; x < (bx + 1) * w / 8; ++x)
                {
                    const uint8_t *p = &buf[((size_t)y * w + x) * 4];
                    if (p[0] | p[1] | p[2])
                        ++cnt;
                }
            }
            printf("%7zu", cnt);
        }
        printf("\n");
    }
}

// Build a view TForm looking from camPos at target, replicating the engine's
// camera convention (ypabact.cpp:637-650: view SclRot = camera basis matrix;
// base::Render applies the model offset UNROTATED in view space, so the
// model center lands exactly at (target - camPos) in view coords).
//
// View space: +Z is forward (clip flags + UAFrustum use z>0 as visible), and
// the projection maps ndc_y = -y_view / z, so world-up must map to NEGATIVE
// view y to appear at the top of the screen.
// Live-toggleable orientation state (viewer).
// USER-VERIFIED default combo (golden look on MT_POWR2/VP_NFOX):
// unflipped basis + CW front faces + culling on + camera raised (steep pitch).
static int g_viewFlipY = 0;     // 1 = mirrored up-row basis (F toggles)
static int g_cwFront = 1;       // 1 = glFrontFace(GL_CW) (W toggles)
static int g_noCull = 0;        // 1 = culling disabled (C toggles)

static void RPSetViewLookAt(TF::TForm3D &view, const vec3d &camPos, const vec3d &target)
{
    vec3d f = target - camPos;
    double fl = f.length();
    f.x /= fl; f.y /= fl; f.z /= fl;

    // world up projected perpendicular to forward
    vec3d up(0.0, 1.0, 0.0);
    vec3d u = up - f * up.dot(f);
    double ul = u.length();
    u.x /= ul; u.y /= ul; u.z /= ul;

    // right = forward x up' ; view basis rows: [right, ±up', forward]
    vec3d r = f * u;

    // view basis rows: [right, ±up', forward]; SclRot IS the standard
    // world->view rotation (column convention, base.cpp:495-500 composes
    // view->CalcSclRot * translate(Δ) so v_view = SclRot * (v + Δ))
    // The engine's projection mirrors Y on the way to window coords
    // (UAFrustum row2 = -1, matrix.h:668). Basis up-row sign is
    // live-toggleable (viewer key F / UA_EXPORT_FLIPY / --flipy).
    float us = g_viewFlipY ? 1.0f : -1.0f;
    mat3x3 m( r.x,  r.y,  r.z,
             us * u.x, us * u.y, us * u.z,
              f.x,  f.y,  f.z );

    view.SclRot = m;
    view.Scale = vec3d(1.0, 1.0, 1.0);
    view.Pos = camPos;
    view.CalcGlobal();
}

static bool RunRenderPng(NC_STACK_base *rootBase, const std::vector<std::string> &models,
                         const std::string &outDir)
{
    MakeDirPath(outDir);

    FSMgr::iDir *root = FSMgr::iDir::GetRoot();
    root->addNode(new FSMgr::iDir("render", outDir));
    FSMgr::iDir::replacePath("render", outDir);

    // View: same camera point as --renderdump, aimed at the model
    TF::Engine.Init();

    static TF::TForm3D view;
    RPSetViewLookAt(view, vec3d(8300.0, 500.0, -4900.0), vec3d(7800.0, 0.0, -5400.0));

    // experiment hook: "1" = identity rotation, camera straight in front
    // of the model looking down its +Z view axis
    if ( getenv("UA_EXPORT_VIEW") && strcmp(getenv("UA_EXPORT_VIEW"), "1") == 0 )
        RPSetViewLookAt(view, vec3d(7800.0, 500.0, -6400.0), vec3d(7800.0, 0.0, -5400.0));

    TF::Engine.SetViewPoint(&view);

    const vec3d pos(7800.0, 0.0, -5400.0);
    const vec3d posBlob(7400.0, 0.0, -5400.0);   // "both" frame: blob -400 X
    const vec3d posConv(8200.0, 0.0, -5400.0);   // "both" frame: conv +400 X

    baseRender_msg tmpl;
    tmpl.frameTime = 16;
    tmpl.globTime = 1000;
    tmpl.minZ = 1.0;
    tmpl.maxZ = 3500.0;
    tmpl.flags = 0;

    // debug hooks: advance the animation clock / override clipping
    if ( const char *gt = getenv("UA_EXPORT_GLOBTIME") )
        tmpl.globTime = atoi(gt);
    if ( const char *ft = getenv("UA_EXPORT_FRAMETIME") )
        tmpl.frameTime = atoi(ft);
    if ( const char *mz = getenv("UA_EXPORT_MAXZ") )
        tmpl.maxZ = (float)atof(mz);

    static const RDVariant varPlain = { "plain", false, false };
    static const RDVariant varElev  = { "elev",  false, false };

    int nOK = 0;
    int nTotal = 0;

    for (const std::string &name : models)
    {
        printf("==== RPNG %s ====\n", name.c_str());

        NC_STACK_base *orig = NULL;
        for (NC_STACK_base *group : rootBase->GetKidList())
        {
            for (NC_STACK_base *kid : group->GetKidList())
            {
                if (StriCmp(kid->getName(), name) == 0)
                {
                    if (!kid->Meshes.empty())
                    {
                        orig = kid;
                        break;
                    }
                    printf("RPNG %s: blob kid empty, skipped\n", name.c_str());
                }
            }
            if (orig)
                break;
        }

        NC_STACK_base *conv = Utils::ProxyLoadBase(
            fmt::sprintf("rsrc:objects/%s.obj", name));
        if (conv && conv->Meshes.empty())
        {
            printf("RPNG %s: converted model empty\n", name.c_str());
            conv->Delete();
            conv = NULL;
        }

        if (!orig || !conv)
        {
            printf("RPNG %s: LOAD orig=%p conv=%p SKIPPED\n", name.c_str(),
                   (void *)orig, (void *)conv);
            if (conv)
                conv->Delete();
            ++nTotal;
            continue;
        }

        ++nTotal;

        // The game converts loaded meshes into GL buffers after load
        // (yw_funcs.cpp:3044 model->MakeVBO / yw_funcs.cpp:1045 _setData->
        // MakeVBO); without it the VBO draw path has no buffers to draw from
        orig->MakeVBO();
        conv->MakeVBO();

        for (int vi = 0; vi < 2; ++vi)
        {
            const RDVariant &var = vi ? varElev : varPlain;
            const char *suffix = vi ? "_elev" : "";

            std::vector<uint8_t> pixBlob, pixConv, pixBoth;
            int w, h;

            // blob alone
            {
                std::vector<RPEntity> ents = { { orig, pos, &var } };
                RPDrawFrame(ents, tmpl, &pixBlob, &w, &h);
                std::string fn = fmt::sprintf("%s_blob%s.png", name, suffix);
                RPPrintStats(fn.c_str(), pixBlob, w, h);
                RPSavePng(pixBlob, w, h, "render/" + fn);
            }

            // conv alone (same camera/grid -> direct diff)
            {
                std::vector<RPEntity> ents = { { conv, pos, &var } };
                RPDrawFrame(ents, tmpl, &pixConv, &w, &h);
                std::string fn = fmt::sprintf("%s_conv%s.png", name, suffix);
                RPPrintStats(fn.c_str(), pixConv, w, h);
                RPSavePng(pixConv, w, h, "render/" + fn);
            }

            size_t diff = RPDiffCount(pixBlob, pixConv);
            printf("RPNGDIFF %-20s blob vs conv: %zu differing pixels\n",
                   fmt::sprintf("%s%s", name, suffix).c_str(), diff);

            // both side by side
            {
                std::vector<RPEntity> ents = { { orig, posBlob, &var },
                                               { conv, posConv, &var } };
                RPDrawFrame(ents, tmpl, &pixBoth, &w, &h);
                std::string fn = fmt::sprintf("%s_both%s.png", name, suffix);
                RPPrintStats(fn.c_str(), pixBoth, w, h);
                RPSavePng(pixBoth, w, h, "render/" + fn);
            }
        }

        // reset the elev scale
        orig->SetScale(vec3d(1.0, 1.0, 1.0), NC_STACK_base::UF_Y);
        conv->SetScale(vec3d(1.0, 1.0, 1.0), NC_STACK_base::UF_Y);
        orig->SetStatic(true);
        conv->SetStatic(true);

        ++nOK;
        fflush(stdout);

        conv->Delete();
    }

    printf("RPNG DONE %d/%d models rendered, PNGs in %s\n", nOK, nTotal, outDir.c_str());
    fflush(stdout);
    return nOK == nTotal;
}

// Task 6: game-path proof. Same comparator as RunVerify2, but every model is
// loaded through the REAL game chain: Utils::ProxyLoadBase("rsrc:objects/
// <name>.obj") with the game prefix layout (rsrc -> data:set1) and the
// converted working dir mounted as the FSMgr base. The original kids come
// from the unconverted SET.BAS blob, loaded in the same process.
static bool RunVerify3(NC_STACK_base *rootBase, const std::string &only)
{
    int nOK = 0;
    int nTotal = 0;
    int nSkippedEmpty = 0;

    for ( NC_STACK_base *group : rootBase->GetKidList() )
    {
        for ( NC_STACK_base *kid : group->GetKidList() )
        {
            std::string kidName = kid->getName();

            if (!only.empty() && StriCmp(kidName, only) != 0)
                continue;

            if ( kid->Meshes.empty() )
            {
                ++nSkippedEmpty;
                continue;
            }

            ++nTotal;

            Verify2Ctx c;
            c.model = kidName;

            // the exact call the game's single-load chain makes
            // (yw_funcs.cpp sub_44AD8C -> Utils::ProxyLoadBase)
            NC_STACK_base *reconBase = Utils::ProxyLoadBase(
                fmt::sprintf("rsrc:objects/%s.obj", kidName));

            NC_STACK_Obj3D *recon = dynamic_cast<NC_STACK_Obj3D *>(reconBase);

            if (!recon)
            {
                if (reconBase)
                    reconBase->Delete();
                c.Fail("LOADFAIL");
            }
            else
            {
                Verify2Skeleton(&c, kid, recon);
                Verify2Meshes(&c, kid, recon);

                std::vector<NC_STACK_ade *> ao(kid->GetAdeList()->begin(), kid->GetAdeList()->end());
                std::vector<NC_STACK_ade *> ar(recon->GetAdeList()->begin(), recon->GetAdeList()->end());

                if (ao.size() != ar.size())
                    c.Fail("ades %zu != %zu", ao.size(), ar.size());

                size_t n = MMIN(ao.size(), ar.size());
                for (size_t i = 0; i < n; ++i)
                    Verify2Ade(&c, ao[i], ar[i], fmt::sprintf("ade%zu", i).c_str());

                recon->Delete();
            }

            if ( c.diffs.empty() )
            {
                ++nOK;
                printf("VERIFY3 %s: OK\n", kidName.c_str());
            }
            else
            {
                std::string joined;
                for (size_t i = 0; i < c.diffs.size(); ++i)
                    joined += (i ? "; " : "") + c.diffs[i];
                printf("VERIFY3 %s: DIFF %s\n", kidName.c_str(), joined.c_str());
            }

            fflush(stdout);
        }
    }

    printf("VERIFY3 %d/%d (empty models skipped: %d)\n", nOK, nTotal, nSkippedEmpty);
    fflush(stdout);

    return nOK == nTotal;
}

static int RunExport(NC_STACK_base *rootBase, const std::string &sourceFile, const std::string &outDir, bool verify)
{
    // Mount the output dir into the FSMgr tree as "export" so all engine
    // write paths (uaOpenFile/SavePng/openFileAlloc) can use "export/<name>"
    FSMgr::iDir *root = FSMgr::iDir::GetRoot();
    root->addNode(new FSMgr::iDir("export", outDir));
    // re-create the node as a scanned directory so files can also be READ
    // back (needed by the verify/self-check paths)
    FSMgr::iDir::replacePath("export", outDir);
    
    const std::string vdir = "export";
    
    int total = 0;
    int exported = 0;
    int emptyModels = 0;
    int warnCount = 0;
    int attachOK = 0;
    int attachTotal = 0;

    // Task 3: pool export (once per set)
    PoolCtx pool;
    CollectPoolFromBase(rootBase, pool);
    CollectPoolFromAdes(rootBase, pool);
    bool poolOK = WritePoolFiles(pool, vdir);

    // Duplicate kid names in the source collapse onto one file set (the last
    // instance overwrites the previous ones); report them
    std::set<std::string> seenModels;

    for ( NC_STACK_base *group : rootBase->GetKidList() )
    {
        for ( NC_STACK_base *kid : group->GetKidList() )
        {
            ++total;
            std::string kidName = kid->getName();
            std::string vpath = vdir + "/" + kidName;
            
            if ( kid->Meshes.empty() )
            {
                // Model with zero meshes: write marker, count as exported
                FSMgr::FileHandle *mk = FSMgr::iDir::openFileAlloc(vpath + ".EMPTY", "w");
                if ( mk )
                {
                    mk->puts("model has no meshes\n");
                    delete mk;
                    ++exported;
                    ++emptyModels;
                    fprintf(stderr, "EMPTY: %s: model has no meshes, wrote %s.EMPTY\n", kidName.c_str(), vpath.c_str());
                }
                else
                    fprintf(stderr, "FAILED: %s: empty model, failed to write %s.EMPTY\n", kidName.c_str(), vpath.c_str());
                continue;
            }
            
            if ( NC_STACK_Obj3D::ExportObj(kid, vpath + ".obj", sourceFile) )
            {
                ++exported;

                // Task 3: per-model attachments (pngs/anms/ades/.uattach)
                ++attachTotal;
                std::string model = SanitizeFileName(kidName);
                if ( !seenModels.insert(model).second )
                    fprintf(stderr, "NOTE: %s: duplicate model name, per-model files are overwritten by this instance\n", kidName.c_str());
                if ( ExportModelAttachments(kid, vdir, kidName, model, pool) )
                    ++attachOK;
                else
                    fprintf(stderr, "ATTACH: %s: attachment export FAILED\n", kidName.c_str());
            }
            else
            {
                fprintf(stderr, "FAILED: %s: ExportObj failed\n", kidName.c_str());
                continue;
            }
            
            if ( verify )
            {
                size_t origMeshes = kid->Meshes.size();
                size_t origVerts = 0;
                for ( GFX::TMesh &m : kid->Meshes )
                    origVerts += m.Vertexes.size();
                
                // 'v' lines actually written to the .obj (must always match)
                size_t objVerts = 0;
                {
                    FSMgr::FileHandle vf = uaOpenFile(vpath + ".obj", "r");
                    std::string ln;
                    while ( vf.ReadLine(&ln) )
                    {
                        if ( ln.size() >= 2 && ln[0] == 'v' && (ln[1] == ' ' || ln[1] == '\t') )
                            ++objVerts;
                    }
                }
                
                NC_STACK_Obj3D *loaded = NC_STACK_Obj3D::LoadObj3D(vpath + ".obj");
                if ( !loaded )
                {
                    fprintf(stderr, "WARN: %s: re-load of %s.obj failed\n", kidName.c_str(), vpath.c_str());
                    ++warnCount;
                }
                else
                {
                    size_t loadMeshes = loaded->Meshes.size();
                    size_t loadVerts = 0;
                    for ( GFX::TMesh &m : loaded->Meshes )
                        loadVerts += m.Vertexes.size();
                    
                    printf("VERIFY %s: objv %zu, meshes %zu -> %zu, verts %zu -> %zu\n",
                           kidName.c_str(), objVerts, origMeshes, loadMeshes, origVerts, loadVerts);
                    
                    if ( objVerts != origVerts )
                    {
                        fprintf(stderr, "WARN: %s: obj vertex count mismatch (orig %zu, obj %zu)\n",
                                kidName.c_str(), origVerts, objVerts);
                        ++warnCount;
                    }
                    
                    if ( loadMeshes != origMeshes || loadVerts != origVerts )
                    {
                        fprintf(stderr, "WARN: %s: mesh/vertex mismatch (meshes %zu -> %zu, verts %zu -> %zu)\n",
                                kidName.c_str(), origMeshes, loadMeshes, origVerts, loadVerts);
                        ++warnCount;
                    }
                    
                    loaded->Delete();
                }
            }
        }
    }
    
    printf("EXPORTED %d/%d\n", exported, total);
    if ( emptyModels )
        printf("EMPTY models (marker files written): %d\n", emptyModels);
    printf("ATTACH %d/%d\n", attachOK, attachTotal);
    if ( pool.fails )
        fprintf(stderr, "POOL export failures: %d\n", pool.fails);
    if ( warnCount )
        fprintf(stderr, "VERIFY warnings: %d\n", warnCount);

    return (exported == total && attachOK == attachTotal && poolOK) ? 0 : 1;
}


// ---------------------------------------------------------------------------
// Task 9: --view <NAME> - interactive single-model viewer
//
//   ua_export --view <NAME> [--set N] [--gamepath <workdir>] [--blob <SET.BAS>]
//
// Opens a real 640x480 windowed window, loads one model by NAME (converted
// game path "rsrc:objects/<NAME>.obj" with the uattach loader, or the
// original blob kid with --blob), and renders it through the real engine
// frame pipeline. Orbit camera: mouse drag / arrow keys rotate, mouse wheel
// zooms, R resets, ESC or Q quits. Animations run (globTime advances).
// ---------------------------------------------------------------------------
static bool RunAlphaProbe(const std::string &pngPath)
{
    // debug: load a png through the engine's image loader and report
    // where the transparency (tRNS / colorkey / palette alpha) survives
    ResBitmap *bmp = NC_STACK_image::LoadImageFromPath(pngPath, true);
    if (!bmp || !bmp->swTex)
    {
        fprintf(stderr, "APROBE: cannot load %s\n", pngPath.c_str());
        return false;
    }
    SDL_Surface *s = bmp->swTex;
    printf("APROBE %s: %dx%d fmt=%s\n", pngPath.c_str(), s->w, s->h,
           SDL_GetPixelFormatName(s->format->format));
    if (s->format->palette)
    {
        const SDL_Color &c0 = s->format->palette->colors[0];
        printf("APROBE   palette entry0 = (%d,%d,%d,%d)\n", c0.r, c0.g, c0.b, c0.a);
        int ntr = 0;
        for (int i = 0; i < s->format->palette->ncolors; i++)
            if (s->format->palette->colors[i].a != 255) ntr++;
        printf("APROBE   non-opaque palette entries: %d\n", ntr);
    }
    if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);
    const uint8_t *px = (const uint8_t *)s->pixels;
    int bpp = s->format->BytesPerPixel;
    const uint8_t *p00 = px;
    printf("APROBE   pixel(0,0) bytes:");
    for (int i = 0; i < bpp; i++) printf(" %02x", p00[i]);
    Uint32 ckey = 0;
    printf("\nAPROBE   colorkey set: %d\n", SDL_GetColorKey(s, &ckey) == 0);
    if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
    return true;
}

static bool RunView(const std::string &name, const std::string &blobPath, int setNum)
{
    if ( name == "@probe" )
        return RunAlphaProbe(blobPath);

    // env fallbacks for the orientation state (CLI flags take precedence
    // because they are applied later in main)
    if ( getenv("UA_EXPORT_FLIPY") ) g_viewFlipY = 1;
    if ( getenv("UA_EXPORT_CWFRONT") ) g_cwFront = 1;
    if ( getenv("UA_EXPORT_NOCULL") ) g_noCull = 1;

    // real window: do NOT force a dummy/offscreen video driver
    if ( !getenv("UA_EXPORT_VIDEO") )
        unsetenv("SDL_VIDEODRIVER");
    unsetenv("SDL_AUDIODRIVER");

    System::IniConf::ReadFromNucleusIni();
    bool gfxVbo = System::IniConf::GfxVBO.Get<bool>();

    System::Init(!gfxVbo);

    SDL_GLContext ctx = SDL_GL_GetCurrentContext();
    if (!ctx || !SDL_GL_GetCurrentWindow())
    {
        fprintf(stderr, "VIEW: no GL context (%s)\n", SDL_GetError());
        return false;
    }

    GFX::Engine.Init();

    // Culling state is live-toggleable (viewer keys W/C; also CLI/env)
    if ( !g_noCull )
        glEnable(GL_CULL_FACE);
    glFrontFace(g_cwFront ? GL_CW : GL_CCW);

    Common::Env.SetPrefix("data", "data");
    Common::Env.SetPrefix("rsrc", fmt::sprintf("data:set%d", setNum));

    RegisterClasses();

    if ( !GFX::Engine.LoadPalette("palette/standard.pal") )
        fprintf(stderr, "VIEW: warning: could not load palette/standard.pal\n");

    TF::Engine.Init();

    NC_STACK_base *model = NULL;

    if ( !blobPath.empty() )
    {
        // mount the blob's directory (absolute paths outside the FSMgr
        // base tree don't resolve - same approach as --verify3's origblob)
        char fbResolved[8192];
        if ( !realpath(blobPath.c_str(), fbResolved) )
        {
            fprintf(stderr, "VIEW: blob '%s' does not exist\n", blobPath.c_str());
            return false;
        }

        std::string blobDir = DirName(fbResolved);
        std::string blobName = BaseName(fbResolved);

        FSMgr::iDir *froot = FSMgr::iDir::GetRoot();
        froot->addNode(new FSMgr::iDir("viewblob", blobDir));
        FSMgr::iDir::replacePath("viewblob", blobDir);

        NC_STACK_base *rootBase = NC_STACK_base::LoadBaseFromFile("viewblob/" + blobName);
        if ( !rootBase )
        {
            fprintf(stderr, "VIEW: cannot load blob '%s'\n", blobPath.c_str());
            return false;
        }

        for (NC_STACK_base *group : rootBase->GetKidList())
        {
            for (NC_STACK_base *kid : group->GetKidList())
            {
                if ( StriCmp(kid->getName(), name) == 0 && !kid->Meshes.empty() )
                    model = kid;
            }
        }

        if ( !model )
        {
            fprintf(stderr, "VIEW: model '%s' not found (or empty) in blob\n", name.c_str());
            rootBase->Delete();
            return false;
        }
        printf("VIEW: loaded blob kid '%s'\n", name.c_str());
    }
    else
    {
        model = Utils::ProxyLoadBase(fmt::sprintf("rsrc:objects/%s.obj", name));
        if ( !model )
        {
            fprintf(stderr, "VIEW: cannot load 'rsrc:objects/%s.obj' (converted assets not found? run from the openua-obj dir or pass --gamepath)\n", name.c_str());
            return false;
        }
        if ( model->Meshes.empty() )
        {
            fprintf(stderr, "VIEW: model '%s' has no meshes\n", name.c_str());
            model->Delete();
            return false;
        }
        printf("VIEW: loaded converted model '%s'\n", name.c_str());
    }

    model->MakeVBO();

    // bounding box -> place the model center at the world origin
    vec3d bmin( 1e9,  1e9,  1e9);
    vec3d bmax(-1e9, -1e9, -1e9);
    for (const GFX::TMesh &m : model->Meshes)
    {
        bmin.x = MMIN(bmin.x, m.BoundBox.Min.x);
        bmin.y = MMIN(bmin.y, m.BoundBox.Min.y);
        bmin.z = MMIN(bmin.z, m.BoundBox.Min.z);
        bmax.x = MMAX(bmax.x, m.BoundBox.Max.x);
        bmax.y = MMAX(bmax.y, m.BoundBox.Max.y);
        bmax.z = MMAX(bmax.z, m.BoundBox.Max.z);
    }
    vec3d center = (bmin + bmax) * 0.5;
    float radius = (bmax - bmin).length() * 0.5f;
    if (radius < 1.0f)
        radius = 1.0f;

    const vec3d modelPos = vec3d(0.0, 0.0, 0.0) - center;

    // orbit camera state
    float dist   = radius * 2.0f;
    float yaw    = 0.6f;
    float pitch  = 1.45f;   // camera raised, pointing down (verified golden)
    if ( const char *pe = getenv("UA_EXPORT_PITCH") )
        pitch = atof(pe);
    if ( const char *ye = getenv("UA_EXPORT_YAW") )
        yaw = atof(ye);
    const float pitchMax = 1.45f;

    static TF::TForm3D view;
    TF::Engine.SetViewPoint(&view);

    baseRender_msg arg;
    arg.frameTime = 16;
    arg.globTime  = 1000;
    arg.minZ = 1.0f;
    arg.maxZ = 3500.0f;
    arg.flags = 0;

    printf("\nVIEW controls: mouse-drag / arrows = orbit, wheel = zoom,\n"
           "               PgUp/PgDn = pitch, R = reset, ESC or Q = quit\n"
           "model: %s  bbox=%.0fx%.0fx%.0f\n\n",
           name.c_str(),
           bmax.x - bmin.x, bmax.y - bmin.y, bmax.z - bmin.z);
    fflush(stdout);

    bool dragging = false;
    bool quit = false;
    uint32_t lastTicks = SDL_GetTicks();

    while ( !quit )
    {
        SDL_Event ev;
        while ( SDL_PollEvent(&ev) )
        {
            switch ( ev.type )
            {
            case SDL_QUIT:
                quit = true;
                break;

            case SDL_WINDOWEVENT:
                if ( ev.window.event == SDL_WINDOWEVENT_CLOSE )
                    quit = true;
                break;

            case SDL_MOUSEBUTTONDOWN:
                if ( ev.button.button == SDL_BUTTON_LEFT )
                    dragging = true;
                break;

            case SDL_MOUSEBUTTONUP:
                if ( ev.button.button == SDL_BUTTON_LEFT )
                    dragging = false;
                break;

            case SDL_MOUSEMOTION:
                if ( dragging )
                {
                    yaw   += ev.motion.xrel * 0.008f;
                    pitch += ev.motion.yrel * 0.008f;
                }
                break;

            case SDL_MOUSEWHEEL:
                dist *= (ev.wheel.y > 0) ? 0.85f : 1.18f;
                break;

            case SDL_KEYDOWN:
                switch ( ev.key.keysym.sym )
                {
                case SDLK_LEFT:  yaw   -= 0.15f; break;
                case SDLK_RIGHT: yaw   += 0.15f; break;
                case SDLK_UP:    pitch += 0.12f; break;
                case SDLK_DOWN:  pitch -= 0.12f; break;
                case SDLK_PAGEUP:   dist *= 0.85f; break;
                case SDLK_PAGEDOWN: dist *= 1.18f; break;
                case SDLK_r:
                    dist = radius * 2.0f; yaw = 0.6f; pitch = 1.45f;
                    break;
                case SDLK_f:
                    g_viewFlipY = !g_viewFlipY;
                    printf("VIEW: flipY=%d (F)\n", g_viewFlipY);
                    break;
                case SDLK_w:
                    g_cwFront = !g_cwFront;
                    glFrontFace(g_cwFront ? GL_CW : GL_CCW);
                    printf("VIEW: cwFront=%d (W)\n", g_cwFront);
                    break;
                case SDLK_c:
                    g_noCull = !g_noCull;
                    if ( g_noCull )
                        glDisable(GL_CULL_FACE);
                    else
                        glEnable(GL_CULL_FACE);
                    printf("VIEW: noCull=%d (C)\n", g_noCull);
                    break;
                case SDLK_ESCAPE:
                case SDLK_q:
                    quit = true;
                    break;
                default:
                    break;
                }
                break;

            default:
                break;
            }
        }

        if ( pitch >  pitchMax ) pitch =  pitchMax;
        if ( pitch < -pitchMax ) pitch = -pitchMax;
        float dmin = radius * 0.3f;
        float dmax = radius * 30.0f;
        if ( dist < dmin ) dist = dmin;
        if ( dist > dmax ) dist = dmax;

        vec3d camPos( dist * cosf(pitch) * sinf(yaw),
                      dist * sinf(pitch),
                      dist * cosf(pitch) * cosf(yaw) );

        RPSetViewLookAt(view, camPos, vec3d(0.0, 0.0, 0.0));

        arg.globTime += 16;
        arg.maxZ = dist + radius * 2.0f + 500.0f;

        GFX::Engine.BeginFrame();

        model->SetStatic(true);
        model->SetPosition(modelPos);
        model->Render(&arg, NULL);

        GFX::Engine.BeginScene();
        GFX::Engine.Rasterize();
        GFX::Engine.EndScene();

        // debug: UA_EXPORT_SNAPSHOT=n saves a PNG after n frames and exits
        {
            static int snapTick = 0;
            const char *snapEnv = getenv("UA_EXPORT_SNAPSHOT");
            if ( snapEnv && *snapEnv )
            {
                int snapN = atoi(snapEnv);
                if ( ++snapTick >= snapN )
                {
                    FSMgr::iDir *froot = FSMgr::iDir::GetRoot();
                    if ( !FSMgr::iDir::findNode("viewshot") )
                    {
                        froot->addNode(new FSMgr::iDir("viewshot", "viewshots"));
                        FSMgr::iDir::replacePath("viewshot", "viewshots");
                    }
                    MakeDirPath("viewshots");

                    Common::Point res = System::GetResolution();
                    std::vector<uint8_t> pix((size_t)res.x * res.y * 4);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, res.x, res.y, GL_RGBA, GL_UNSIGNED_BYTE, pix.data());
                    RPSavePng(pix, res.x, res.y, fmt::sprintf("viewshot/%s.png", name));
                    printf("VIEW: snapshot saved after %d frames\n", snapTick);
                    quit = true;
                }
            }
        }

        if ( !quit )
            System::Flip();

        // ~60 FPS pacing
        uint32_t now = SDL_GetTicks();
        if ( now - lastTicks < 15 )
            SDL_Delay(15 - (now - lastTicks));
        lastTicks = SDL_GetTicks();
    }

    if ( blobPath.empty() )
        model->Delete();

    printf("VIEW: done\n");
    return true;
}

int main(int argc, char *argv[])
{
    // Must be set before any SDL usage so the tool works headless.
    // Override with UA_EXPORT_VIDEO (e.g. "offscreen" for real GL rendering).
    // --view mode: leave the driver alone (real window) - handled in RunView.
    bool wantView = false;
    for (int i = 1; i < argc; ++i)
        if ( !strcmp(argv[i], "--view") )
            wantView = true;

    if ( !wantView )
    {
        const char *videoEnv = getenv("UA_EXPORT_VIDEO");
        setenv("SDL_VIDEODRIVER", (videoEnv && *videoEnv) ? videoEnv : "dummy", 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
    }

    std::string filePath;
    std::string outDir;
    std::string only;
    std::string gamePath;
    std::string viewName;
    std::string blobPath;
    int setNum = 1;
    bool listOnly = false;
    bool verify = false;
    bool verify2 = false;
    bool verify3 = false;
    bool renderdump = false;
    bool renderpng = false;
    std::string modelsArg;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if ( arg == "--list" )
            listOnly = true;
        else if ( arg == "--verify" )
            verify = true;
        else if ( arg == "--verify2" )
            verify2 = true;
        else if ( arg == "--verify3" )
            verify3 = true;
        else if ( arg == "--renderdump" )
            renderdump = true;
        else if ( arg == "--renderpng" )
            renderpng = true;
        else if ( arg == "--view" )
        {
            if ( i + 1 >= argc )
            {
                fprintf(stderr, "ua_export: --view requires a model name argument\n");
                return 1;
            }
            viewName = argv[++i];
        }
        else if ( arg == "--set" )
        {
            if ( i + 1 >= argc )
            {
                fprintf(stderr, "ua_export: --set requires a set number argument\n");
                return 1;
            }
            setNum = atoi(argv[++i]);
        }
        else if ( arg == "--flipy" )
            g_viewFlipY = 1;
        else if ( arg == "--cwfront" )
            g_cwFront = 1;
        else if ( arg == "--nocull" )
            g_noCull = 1;
        else if ( arg == "--blob" )
        {
            if ( i + 1 >= argc )
            {
                fprintf(stderr, "ua_export: --blob requires a SET.BAS path argument\n");
                return 1;
            }
            blobPath = argv[++i];
        }
        else if ( arg == "--models" )
        {
            if ( i + 1 >= argc )
            {
                fprintf(stderr, "ua_export: --models requires a comma-separated list argument\n");
                return 1;
            }
            modelsArg = argv[++i];
        }
        else if ( arg == "--gamepath" )
        {
            if ( i + 1 >= argc )
            {
                fprintf(stderr, "ua_export: --gamepath requires a directory argument\n");
                return 1;
            }
            gamePath = argv[++i];
        }
        else if ( arg == "--only" )
        {
            if ( i + 1 >= argc )
            {
                fprintf(stderr, "ua_export: --only requires a model name argument\n");
                return 1;
            }
            only = argv[++i];
        }
        else if ( arg == "--outdir" )
        {
            if ( i + 1 >= argc )
            {
                fprintf(stderr, "ua_export: --outdir requires a directory argument\n");
                return 1;
            }
            outDir = argv[++i];
        }
        else if ( !arg.empty() && arg[0] == '-' )
            fprintf(stderr, "ua_export: ignoring unknown option '%s'\n", arg.c_str());
        else if ( filePath.empty() )
            filePath = arg;
    }

    if ( viewName == "__alphaprobe" )
    {
        // internal: ua_export --alphaprobe handled via --view __alphaprobe <path hack>
    }

    if ( wantView )
    {
        if ( viewName.empty() )
        {
            fprintf(stderr, "ua_export: --view requires a model name\n");
            return 1;
        }

        std::string gp = gamePath;
        if ( gp.empty() )
            gp = ".";   // run from inside the converted working dir

        char gpResolved[8192];
        if ( !realpath(gp.c_str(), gpResolved) )
        {
            fprintf(stderr, "ua_export: gamepath '%s' does not exist\n", gp.c_str());
            return 1;
        }
        FSMgr::iDir::setBaseDir(gpResolved);

        return RunView(viewName, blobPath, setNum) ? 0 : 2;
    }

    if ( filePath.empty() )
    {
        fprintf(stderr, "Usage: ua_export <path-to-SET.BAS> [--list] [--outdir <dir>] [--verify] "
                        "[--verify2 [--only <model>]] [--verify3 --gamepath <workdir>] "
                        "[--renderdump --gamepath <workdir> [--models A,B,C]] "
                        "[--renderpng --gamepath <workdir> [--models A,B,C] [--outdir <dir>]]\n");
        return 1;
    }
    
    if ( (verify || verify2) && outDir.empty() )
    {
        fprintf(stderr, "ua_export: --verify/--verify2 require --outdir <dir>\n");
        return 1;
    }

    for (int i = 0; i < argc; ++i)
        System::AddCmdLine( std::string(argv[i]) );

    System::IniConf::Init();

    // ------------------------------------------------------------------
    // Task 6/7/8: --verify3 / --renderdump / --renderpng game-path modes
    //
    //   ua_export <SET.BAS> --verify3 --gamepath <converted-workdir> [--only <model>]
    //   ua_export <SET.BAS> --renderdump --gamepath <converted-workdir> [--models A,B,C]
    //   ua_export <SET.BAS> --renderpng --gamepath <converted-workdir> [--models A,B,C] [--outdir <dir>]
    //
    // Sets up the FSMgr/prefixes exactly like the game does in yw_LoadSet
    // (base dir = converted working dir, data -> data, rsrc -> data:set1,
    // set default palette). --verify3 loads every non-empty blob kid through
    // the REAL game chain Utils::ProxyLoadBase("rsrc:objects/<name>.obj")
    // and compares it against the original blob kid (same comparator as
    // --verify2). --renderdump drives the actual renderer input path for a
    // model list and dumps/diffs the produced TRenderNodes. --renderpng
    // additionally initializes GL (SDL offscreen / EGL) and runs the real
    // frame path, saving PNGs of the actual pixels. The original blob is
    // mounted from its own directory ("origblob" node), independent of the
    // game base dir.
    // ------------------------------------------------------------------
    if ( verify3 || renderdump || renderpng )
    {
        const char *mode = verify3 ? "--verify3" : (renderdump ? "--renderdump" : "--renderpng");
        if ( gamePath.empty() )
        {
            fprintf(stderr, "ua_export: %s requires --gamepath <converted-workdir>\n", mode);
            return 1;
        }

        char gpResolved[8192];
        if ( !realpath(gamePath.c_str(), gpResolved) )
        {
            fprintf(stderr, "ua_export: --gamepath '%s' does not exist\n", gamePath.c_str());
            return 1;
        }

        // base dir = the converted working dir, scanned like the game does
        FSMgr::iDir::setBaseDir(gpResolved);

        // game prefix layout (yw.cpp / yw_LoadSet)
        Common::Env.SetPrefix("data", "data");
        Common::Env.SetPrefix("rsrc", "data:set1");

        RegisterClasses();

        // --renderpng: SDL offscreen window + real GL context + engine gfx
        // init BEFORE any model load (textures are uploaded to GL at draw)
        if ( renderpng && !RPInitGL() )
            return 2;

        // like yw_LoadSet: set default palette after rsrc -> data:set1
        if ( !GFX::Engine.LoadPalette("palette/standard.pal") )
            fprintf(stderr, "ua_export: warning: could not load palette/standard.pal, texture colors may be wrong\n");

        // Mount the directory holding the original SET.BAS blob
        char fbResolved[8192];
        if ( !realpath(filePath.c_str(), fbResolved) )
        {
            fprintf(stderr, "ua_export: blob '%s' does not exist\n", filePath.c_str());
            return 1;
        }

        std::string blobDir = DirName(fbResolved);
        std::string blobName = BaseName(fbResolved);

        FSMgr::iDir *root = FSMgr::iDir::GetRoot();
        root->addNode(new FSMgr::iDir("origblob", blobDir));
        FSMgr::iDir::replacePath("origblob", blobDir);

        NC_STACK_base *rootBase = NC_STACK_base::LoadBaseFromFile("origblob/" + blobName);
        if ( !rootBase )
        {
            fprintf(stderr, "ua_export: failed to load original blob '%s'\n", fbResolved);
            return 1;
        }

        printf("Loaded: %s (gamepath %s)\n", fbResolved, gpResolved);

        if ( verify3 )
        {
            bool res = RunVerify3(rootBase, only);
            rootBase->Delete();
            fflush(stdout);
            return res ? 0 : 1;
        }

        // --renderdump/--renderpng: model list from --models or the built-in
        // default set
        std::vector<std::string> models;
        if ( !modelsArg.empty() )
        {
            size_t pos = 0;
            while ( pos != std::string::npos )
            {
                size_t next = modelsArg.find(',', pos);
                std::string m = modelsArg.substr(pos, next == std::string::npos ?
                                                 std::string::npos : next - pos);
                if ( !m.empty() )
                    models.push_back(m);
                pos = (next == std::string::npos) ? next : next + 1;
            }
        }
        else if ( renderpng )
        {
            for (const char *m : RPDefaultModels)
                models.push_back(m);
        }
        else
        {
            for (const char *m : RDDefaultModels)
                models.push_back(m);
        }

        if ( renderpng )
        {
            std::string pngOut = outDir.empty() ? "/work/render" : outDir;
            RunRenderPng(rootBase, models, pngOut);
        }
        else
            RunRenderDump(rootBase, models);

        rootBase->Delete();
        fflush(stdout);
        return 0;
    }

    // Resolve to absolute path
    char resolved[8192];
    if ( realpath(filePath.c_str(), resolved) )
        filePath = resolved;

    std::string fileDir = DirName(filePath);
    if ( fileDir.empty() )
        fileDir = ".";

    // Find the game data root: nearest ancestor dir that contains a "data" dir
    // (fileExist only matches files, so use findNode for the dir check)
    std::string gameRoot;
    std::string cur = fileDir;
    for (int up = 0; up < 5; ++up)
    {
        FSMgr::iDir::setBaseDir(cur);

        FSMgr::iNode *dataNode = FSMgr::iDir::findNode("data");
        if ( dataNode && dataNode->getType() == FSMgr::iNode::NTYPE_DIR )
        {
            gameRoot = cur;
            break;
        }

        std::string parent = DirName(cur);
        if ( parent == cur || parent.empty() )
            break;
        cur = parent;
    }

    if ( gameRoot.empty() )
        gameRoot = fileDir;

    FSMgr::iDir::setBaseDir(gameRoot);

    // Usual engine prefix layout, relative to the FS root
    Common::Env.SetPrefix("data", "data");

    std::string setDir = BaseName(fileDir);
    std::string parentOfSet = DirName(fileDir);
    if ( !setDir.empty() && StriCmp(setDir, "objects") == 0 )
        setDir = BaseName(parentOfSet);

    Common::Env.SetPrefix("rsrc", "data:" + setDir);

    // Path of the file relative to the FS root
    std::string loadPath = filePath;
    if ( gameRoot != "/" && loadPath.length() > gameRoot.length() )
        loadPath = loadPath.substr(gameRoot.length() + 1);
    else
        loadPath = BaseName(filePath);

    RegisterClasses();

    // Load the set default palette before any model load, so ILBM textures
    // without an embedded CMAP pick up the right colors (like the game does:
    // GFX::Engine.LoadPalette("palette/standard.pal"), see yw_funcs.cpp)
    if ( !GFX::Engine.LoadPalette("palette/standard.pal") )
        fprintf(stderr, "ua_export: warning: could not load palette/standard.pal, texture colors may be wrong\n");

    NC_STACK_base *rootBase = NC_STACK_base::LoadBaseFromFile(loadPath);
    if ( !rootBase )
    {
        fprintf(stderr, "ua_export: failed to load '%s'\n", filePath.c_str());
        return 1;
    }


    printf("Loaded: %s\n", filePath.c_str());


    const BaseList &groups = rootBase->GetKidList();

    if ( !outDir.empty() && verify2 )
    {
        MakeDirPath(outDir);

        bool res = RunVerify2(rootBase, outDir, only);

        rootBase->Delete();
        fflush(stdout);
        return res ? 0 : 1;
    }

    if ( !outDir.empty() )
    {
        // Create the output dir on the host, then export every leaf kid
        MakeDirPath(outDir);
        
        int res = RunExport(rootBase, filePath, outDir, verify);
        
        rootBase->Delete();
        fflush(stdout);
        return res;
    }

    int groupIdx = 0;
    for ( NC_STACK_base *group : groups )
    {
        BaseList &kids = group->GetKidList();
        printf("GROUP %d: %d kids\n", groupIdx++, (int)kids.size());

        int kidIdx = 0;
        for ( NC_STACK_base *kid : kids )
        {
            printf("  [%d] %s\n", kidIdx++, kid->getName().c_str());
        }
    }

    rootBase->Delete();
    fflush(stdout);

    return 0;
}
