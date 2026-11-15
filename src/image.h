#ifndef IMAGE_H_INCLUDED
#define IMAGE_H_INCLUDED

#include "bitmap.h"

class NC_STACK_image: public NC_STACK_bitmap
{
public:
    virtual size_t Init(IDVList &stak);
    virtual rsrc * rsrc_func64(IDVList &stak);
    
    static bool SavePng(ResBitmap *bmp, const std::string &fname);

    // Load an image file by explicit path (no name-based rsrc resolution) into
    // a standalone ResBitmap. Used by the per-model asset pipeline, which must
    // never fetch textures from the shared rsrc pool by name.
    static ResBitmap *LoadImageFromPath(const std::string &path, bool convertColor);

    NC_STACK_image();
    virtual ~NC_STACK_image();
    
    virtual const std::string ClassName() const {
        return __ClassName;
    };


    //Data
public:
    static constexpr const char * __ClassName = "image.class";
};

#endif // ILBM_H_INCLUDED
