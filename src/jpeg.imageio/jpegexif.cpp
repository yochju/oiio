/*
  Copyright 2008 Larry Gritz and the other authors and contributors.
  All Rights Reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:
  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
  * Neither the name of the software's owners nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  (This is the Modified BSD License)
*/


#include <cassert>
#include <ctype.h>
#include <cstdio>
#include <iostream>
#include <vector>
#include <map>

#include <boost/scoped_array.hpp>
#include <boost/foreach.hpp>

extern "C" {
#include "tiff.h"
}

#include "imageio.h"
using namespace OpenImageIO;
#include "fmath.h"
#include "jpeg_pvt.h"


#define DEBUG_EXIF_READ  0
#define DEBUG_EXIF_WRITE 0


namespace Jpeg_imageio_pvt {


// Sizes of TIFFDataType members
static size_t tiff_data_sizes[] = {
    0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4
};

static int
tiff_data_size (const TIFFDirEntry &dir)
{
    return tiff_data_sizes[(int)dir.tdir_type] * dir.tdir_count;
}



struct EXIF_tag_info {
    int tifftag;            // TIFF tag used for this info
    const char *name;       // Attribute name we use
    TIFFDataType tifftype;  // Data type that TIFF wants
};

static const EXIF_tag_info exif_tag_table[] = {
    // Skip ones handled by the usual JPEG code
    { TIFFTAG_IMAGEWIDTH,	"Exif:ImageWidth",	TIFF_NOTYPE },
    { TIFFTAG_IMAGELENGTH,	"Exif:ImageLength",	TIFF_NOTYPE },
    { TIFFTAG_BITSPERSAMPLE,	"Exif:BitsPerSample",	TIFF_NOTYPE },
    { TIFFTAG_COMPRESSION,	"Exif:Compression",	TIFF_NOTYPE },
    { TIFFTAG_PHOTOMETRIC,	"Exif:Photometric",	TIFF_NOTYPE },
    { TIFFTAG_SAMPLESPERPIXEL,	"Exif:SamplesPerPixel",	TIFF_NOTYPE },
    { TIFFTAG_PLANARCONFIG,	"Exif:PlanarConfig",	TIFF_NOTYPE },
    { TIFFTAG_YCBCRSUBSAMPLING,	"Exif:YCbCrSubsampling",TIFF_SHORT },
    { TIFFTAG_YCBCRPOSITIONING,	"Exif:YCbCrPositioning",TIFF_SHORT },
    // TIFF tags we may come across
    { TIFFTAG_ORIENTATION,	"Orientation",	TIFF_SHORT },
    { TIFFTAG_XRESOLUTION,	"XResolution",	TIFF_RATIONAL },
    { TIFFTAG_YRESOLUTION,	"YResolution",	TIFF_RATIONAL },
    { TIFFTAG_RESOLUTIONUNIT,	"ResolutionUnit",TIFF_SHORT },
    { TIFFTAG_IMAGEDESCRIPTION,	"ImageDescription",	TIFF_ASCII },
    { TIFFTAG_MAKE,	        "Make",	        TIFF_ASCII },
    { TIFFTAG_MODEL,	        "Model",	TIFF_ASCII },
    { TIFFTAG_SOFTWARE,	        "Software",	TIFF_ASCII },
    { TIFFTAG_ARTIST,	        "Artist",	TIFF_ASCII },
    { TIFFTAG_COPYRIGHT,	"Copyright",	TIFF_ASCII },
    { TIFFTAG_DATETIME,	        "DateTime",	TIFF_ASCII },
    { TIFFTAG_EXIFIFD,          "Exif:ExifIFD", TIFF_NOTYPE },
    { TIFFTAG_INTEROPERABILITYIFD, "Exif:InteroperabilityIFD", TIFF_NOTYPE },

    // EXIF tags we may come across
    { EXIFTAG_EXPOSURETIME,	"ExposureTime",	TIFF_RATIONAL },
    { EXIFTAG_FNUMBER,	        "FNumber",	TIFF_RATIONAL },
    { EXIFTAG_EXPOSUREPROGRAM,	"Exif:ExposureProgram",	TIFF_SHORT }, // ?? translate to ascii names?
    { EXIFTAG_SPECTRALSENSITIVITY,	"Exif:SpectralSensitivity",	TIFF_ASCII },
    { EXIFTAG_ISOSPEEDRATINGS,	"Exif:ISOSpeedRatings",	TIFF_SHORT },
    { EXIFTAG_OECF,	        "Exif:OECF",	TIFF_NOTYPE },	 // skip it
    { EXIFTAG_EXIFVERSION,	"Exif:ExifVersion",	TIFF_NOTYPE },	 // skip it
    { EXIFTAG_DATETIMEORIGINAL,	"Exif:DateTimeOriginal",	TIFF_ASCII },
    { EXIFTAG_DATETIMEDIGITIZED,"Exif:DateTimeDigitized",	TIFF_ASCII },
    { EXIFTAG_COMPONENTSCONFIGURATION,	"Exif:ComponentsConfiguration",	TIFF_UNDEFINED },
    { EXIFTAG_COMPRESSEDBITSPERPIXEL,	"Exif:CompressedBitsPerPixel",	TIFF_RATIONAL },
    { EXIFTAG_SHUTTERSPEEDVALUE,"Exif:ShutterSpeedValue",	TIFF_SRATIONAL }, // APEX units
    { EXIFTAG_APERTUREVALUE,	"Exif:ApertureValue",	TIFF_RATIONAL },	// APEX units
    { EXIFTAG_BRIGHTNESSVALUE,	"Exif:BrightnessValue",	TIFF_SRATIONAL },
    { EXIFTAG_EXPOSUREBIASVALUE,"Exif:ExposureBiasValue",	TIFF_SRATIONAL },
    { EXIFTAG_MAXAPERTUREVALUE,	"Exif:MaxApertureValue",TIFF_RATIONAL },
    { EXIFTAG_SUBJECTDISTANCE,	"Exif:SubjectDistance",	TIFF_RATIONAL },
    { EXIFTAG_METERINGMODE,	"Exif:MeteringMode",	TIFF_SHORT },
    { EXIFTAG_LIGHTSOURCE,	"Exif:LightSource",	TIFF_SHORT },
    { EXIFTAG_FLASH,	        "Exif:Flash",	        TIFF_SHORT },
    { EXIFTAG_FOCALLENGTH,	"Exif:FocalLength",	TIFF_RATIONAL }, // mm
    { EXIFTAG_SUBJECTAREA,	"Exif:SubjectArea",	TIFF_NOTYPE }, // skip
    { EXIFTAG_MAKERNOTE,	"Exif:MakerNote",	TIFF_NOTYPE },	 // skip it
    { EXIFTAG_USERCOMMENT,	"Exif:UserComment",	TIFF_NOTYPE },	// skip it
    { EXIFTAG_SUBSECTIME,	"Exif:SubsecTime",	        TIFF_ASCII },
    { EXIFTAG_SUBSECTIMEORIGINAL,"Exif:SubsecTimeOriginal",	TIFF_ASCII },
    { EXIFTAG_SUBSECTIMEDIGITIZED,"Exif:SubsecTimeDigitized",	TIFF_ASCII },
    { EXIFTAG_FLASHPIXVERSION,	"Exif:FlashPixVersion",	TIFF_NOTYPE },	// skip "Exif:FlashPixVesion",	TIFF_NOTYPE },
    { EXIFTAG_COLORSPACE,	"Exif:ColorSpace",	TIFF_SHORT },
    { EXIFTAG_PIXELXDIMENSION,	"Exif:PixelXDimension",	TIFF_LONG },
    { EXIFTAG_PIXELYDIMENSION,	"Exif:PixelYDimension",	TIFF_LONG },
    { EXIFTAG_RELATEDSOUNDFILE,	"Exif:RelatedSoundFile", TIFF_NOTYPE },	// skip
    { EXIFTAG_FLASHENERGY,	"Exif:FlashEnergy",	TIFF_RATIONAL },
    { EXIFTAG_SPATIALFREQUENCYRESPONSE,	"Exif:SpatialFrequencyResponse",	TIFF_NOTYPE },
    { EXIFTAG_FOCALPLANEXRESOLUTION,	"Exif:FocalPlaneXResolution",	TIFF_RATIONAL },
    { EXIFTAG_FOCALPLANEYRESOLUTION,	"Exif:FocalPlaneYResolution",	TIFF_RATIONAL },
    { EXIFTAG_FOCALPLANERESOLUTIONUNIT,	"Exif:FocalPlaneResolutionUnit",	TIFF_SHORT }, // Symbolic?
    { EXIFTAG_SUBJECTLOCATION,	"Exif:SubjectLocation",	TIFF_SHORT }, // FIXME: short[2]
    { EXIFTAG_EXPOSUREINDEX,	"Exif:ExposureIndex",	TIFF_RATIONAL },
    { EXIFTAG_SENSINGMETHOD,	"Exif:SensingMethod",	TIFF_SHORT },
    { EXIFTAG_FILESOURCE,	"Exif:FileSource",	TIFF_SHORT },
    { EXIFTAG_SCENETYPE,	"Exif:SceneType",	TIFF_SHORT },
    { EXIFTAG_CFAPATTERN,	"Exif:CFAPattern",	TIFF_NOTYPE },
    { EXIFTAG_CUSTOMRENDERED,	"Exif:CustomRendered",	TIFF_SHORT },
    { EXIFTAG_EXPOSUREMODE,	"Exif:ExposureMode",	TIFF_SHORT },
    { EXIFTAG_WHITEBALANCE,	"Exif:WhiteBalance",	TIFF_SHORT },
    { EXIFTAG_DIGITALZOOMRATIO,	"Exif:DigitalZoomRatio",TIFF_RATIONAL },
    { EXIFTAG_FOCALLENGTHIN35MMFILM,	"Exif:FocalLengthIn35mmFilm",	TIFF_SHORT },
    { EXIFTAG_SCENECAPTURETYPE,	"Exif:SceneCaptureType",TIFF_SHORT },
    { EXIFTAG_GAINCONTROL,	"Exif:GainControl",	TIFF_RATIONAL },
    { EXIFTAG_CONTRAST,	        "Exif:Contrast",	TIFF_SHORT },
    { EXIFTAG_SATURATION,	"Exif:Saturation",	TIFF_SHORT },
    { EXIFTAG_SHARPNESS,	"Exif:Sharpness",	TIFF_SHORT },
    { EXIFTAG_DEVICESETTINGDESCRIPTION,	"Exif:DeviceSettingDescription",	TIFF_NOTYPE },
    { EXIFTAG_SUBJECTDISTANCERANGE,	"Exif:SubjectDistanceRange",	TIFF_SHORT },
    { EXIFTAG_IMAGEUNIQUEID,	"Exif:ImageUniqueID",	TIFF_ASCII }
};



class TagMap {
    typedef std::map<int, const EXIF_tag_info *> tagmap_t;
    typedef std::map<std::string, const EXIF_tag_info *> namemap_t;
public:
    TagMap (void) {
        int n = sizeof(exif_tag_table)/sizeof(exif_tag_table[0]);
        for (int i = 0;  i < n;  ++i) {
            const EXIF_tag_info *eti = &exif_tag_table[i];
            m_tagmap[eti->tifftag] = eti;
            if (eti->name)
                m_namemap[eti->name] = eti;
        }
    }

    const EXIF_tag_info * find (int tag) const {
        tagmap_t::const_iterator i = m_tagmap.find (tag);
        return i == m_tagmap.end() ? NULL : i->second;
    }

    const char * name (int tag) const {
        tagmap_t::const_iterator i = m_tagmap.find (tag);
        return i == m_tagmap.end() ? NULL : i->second->name;
    }

    TIFFDataType tifftype (int tag) const {
        tagmap_t::const_iterator i = m_tagmap.find (tag);
        return i == m_tagmap.end() ? TIFF_NOTYPE : i->second->tifftype;
    }

    int tag (const std::string &name) const {
        namemap_t::const_iterator i = m_namemap.find (name);
        return i == m_namemap.end() ? -1 : i->second->tifftag;
    }

private:
    tagmap_t m_tagmap;
    namemap_t m_namemap;
};

static TagMap tagmap;




static void
print_dir_entry (const TIFFDirEntry &dir, const char *datastart)
{
    int len = tiff_data_size (dir);
    const char *mydata = (len <= 4) ? (const char *)&dir.tdir_offset 
                                    : (datastart + dir.tdir_offset);
    const char *name = tagmap.name(dir.tdir_tag);
    std::cerr << "tag=" << dir.tdir_tag
              << " (" << (name ? name : "unknown") << ")"
              << ", type=" << dir.tdir_type
              << ", count=" << dir.tdir_count
              << ", offset=" << dir.tdir_offset << " = " ;
    switch (dir.tdir_type) {
    case TIFF_ASCII :
        std::cerr << "'" << (char *)mydata << "'";
        break;
    case TIFF_RATIONAL :
        std::cerr << ((unsigned int *)mydata)[0] << "/"
                  << ((unsigned int *)mydata)[1] << " = "
                  << (float)((int *)mydata)[0]/((int *)mydata)[1];
        break;
    case TIFF_SRATIONAL :
        std::cerr << ((int *)mydata)[0] << "/"
                  << ((int *)mydata)[1] << " = "
                  << (float)((int *)mydata)[0]/((int *)mydata)[1];
        break;
    case TIFF_SHORT :
        std::cerr << ((unsigned short *)mydata)[0];
        break;
    case TIFF_LONG :
        std::cerr << ((unsigned int *)mydata)[0];
        break;
    case TIFF_UNDEFINED :
    case TIFF_NOTYPE :
        for (size_t i = 0;  i < dir.tdir_count;  ++i)
            std::cerr << (int)((unsigned char *)mydata)[i] << ' ';
    default:
        break;
    }
    std::cerr << "\n";
}



/// Add one EXIF directory entry's data to spec under the given 'name'.
/// The directory entry is in *dirp, buf points to the beginning of the
/// TIFF "file", i.e. all TIFF tag offsets are relative to buf.  If swab
/// is true, the endianness of the file doesn't match the endianness of
/// the host CPU, therefore all integer and float data embedded in buf
/// needs to be byte-swapped.  Note that *dirp HAS already been swapped,
/// if necessary, so no byte swapping on *dirp is necessary.
static void
add_exif_item_to_spec (ImageSpec &spec, const char *name,
                       const TIFFDirEntry *dirp, const char *buf, bool swab)
{
    if (dirp->tdir_type == TIFF_SHORT && dirp->tdir_count == 1) {
        unsigned short d;
        d = * (const unsigned short *) &dirp->tdir_offset;  // short stored in offset itself
        ((unsigned short *)&dirp->tdir_offset)[1] = 0; // clear unused half
        if (swab)
            swap_endian (&d);
        spec.attribute (name, (unsigned int)d);
    } else if (dirp->tdir_type == TIFF_LONG && dirp->tdir_count == 1) {
        unsigned int d;
        d = * (const unsigned int *) &dirp->tdir_offset;  // int stored in offset itself
        if (swab)
            swap_endian (&d);
        spec.attribute (name, (unsigned int)d);
    } else if (dirp->tdir_type == TIFF_RATIONAL && dirp->tdir_count == 1) {
        unsigned int num, den;
        num = ((const unsigned int *) &(buf[dirp->tdir_offset]))[0];
        den = ((const unsigned int *) &(buf[dirp->tdir_offset]))[1];
        if (swab) {
            swap_endian (&num);
            swap_endian (&den);
        }
        double db = (double)num / (double)den;
        spec.attribute (name, (float)db);
    } else if (dirp->tdir_type == TIFF_SRATIONAL && dirp->tdir_count == 1) {
        unsigned int num, den;
        num = ((const unsigned int *) &(buf[dirp->tdir_offset]))[0];
        den = ((const unsigned int *) &(buf[dirp->tdir_offset]))[1];
        if (swab) {
            swap_endian (&num);
            swap_endian (&den);
        }
        double db = (double)num / (double)den;
        spec.attribute (name, (float)db);
    } else if (dirp->tdir_type == TIFF_ASCII) {
        int len = tiff_data_size (*dirp);
        const char *mydata = (len <= 4) ? (const char *)&dirp->tdir_offset 
                                        : (buf + dirp->tdir_offset);
        spec.attribute (name, mydata);
    } else if (dirp->tdir_type == TIFF_UNDEFINED) {
        // Add it as bytes
#if 0
        const void *addr = dirp->tdir_count <= 4 ? (const void *) &dirp->tdir_offset 
                                                 : (const void *) &buf[dirp->tdir_offset];
        spec.attribute (name, TypeDesc::UINT8, dirp->tdir_count, addr);
#endif
    } else {
        std::cerr << "didn't know how to process type " 
                  << dirp->tdir_type << " x " << dirp->tdir_count << "\n";
    }
}



#if 0
static std::string
resunit_tag (const TIFFDirEntry *dirp, const char *buf, bool swab)
{
    if (dirp->tdir_type != TIFF_SHORT)
        return "none";
    short s = add_exif_item_to_spec (spec, dirp, buf, swab);
    if (s == RESUNIT_INCH)
        return "in";
    else if (s == RESUNIT_CENTIMETER)
        return "cm";
    return "none";
}
#endif



/// Process a single TIFF directory entry embedded in the JPEG 'APP1'
/// data.  The directory entry is in *dirp, buf points to the beginning
/// of the TIFF "file", i.e. all TIFF tag offsets are relative to buf.
/// The goal is to decode the tag and put the data into appropriate
/// attribute slots of spec.  If swab is true, the endianness of the
/// file doesn't match the endianness of the host CPU, therefore all
/// integer and float data embedded in buf needs to be byte-swapped.
/// Note that *dirp has not been swapped, and so is still in the native
/// endianness of the file.
static void
read_exif_tag (ImageSpec &spec, const TIFFDirEntry *dirp,
               const char *buf, bool swab)
{
    // Make a copy of the pointed-to TIFF directory, swab the components
    // if necessary.
    TIFFDirEntry dir = *dirp;
    if (swab) {
        swap_endian (&dir.tdir_tag);
        swap_endian (&dir.tdir_type);
        swap_endian (&dir.tdir_count);
        swap_endian (&dir.tdir_offset);
    }

    const char *name = tagmap.name (dir.tdir_tag);
#if DEBUG_EXIF_READ
    std::cerr << "Read ";
    print_dir_entry (dir, buf);
#endif

    if (dir.tdir_tag == TIFFTAG_EXIFIFD) {
        // Special case: It's a pointer to a private EXIF directory.
        // Handle the whole thing recursively.
        unsigned int offset = dirp->tdir_offset;  // int stored in offset itself
        if (swab)
            swap_endian (&offset);
        const unsigned char *ifd = ((const unsigned char *)buf + offset);
        unsigned short ndirs = *(const unsigned short *)ifd;
        if (swab)
            swap_endian (&ndirs);
#if DEBUG_EXIF_READ
        std::cerr << "exifid has type " << dir.tdir_type << ", offset " << dir.tdir_offset << "\n";
        std::cerr << "EXIF Number of directory entries = " << ndirs << "\n";
#endif
        for (int d = 0;  d < ndirs;  ++d)
            read_exif_tag (spec, (const TIFFDirEntry *)(ifd+2+d*sizeof(TIFFDirEntry)),
                           (const char *)buf, swab);
#if DEBUG_EXIF_READ
        std::cerr << "> End EXIF\n";
#endif
    } else if (dir.tdir_tag == TIFFTAG_INTEROPERABILITYIFD) {
        // Special case: It's a pointer to a private EXIF directory.
        // Handle the whole thing recursively.
        unsigned int offset = dirp->tdir_offset;  // int stored in offset itself
        if (swab)
            swap_endian (&offset);
        const unsigned char *ifd = ((const unsigned char *)buf + offset);
        unsigned short ndirs = *(const unsigned short *)ifd;
        if (swab)
            swap_endian (&ndirs);
#if DEBUG_EXIF_READ
        std::cerr << "\n\nInteroperability has type " << dir.tdir_type << ", offset " << dir.tdir_offset << "\n";
        std::cerr << "Interoperability Number of directory entries = " << ndirs << "\n";
#endif
        for (int d = 0;  d < ndirs;  ++d)
            read_exif_tag (spec, (const TIFFDirEntry *)(ifd+2+d*sizeof(TIFFDirEntry)),
                           (const char *)buf, swab);
#if DEBUG_EXIF_READ
        std::cerr << "> End Interoperability\n\n";
#endif
    } else {
        // Everything else -- use our table to handle the general case
        const char *name = tagmap.name (dir.tdir_tag);
        if (name) {
            add_exif_item_to_spec (spec, name, &dir, buf, swab);
        } else {
#if DEBUG_EXIF_READ
            std::cerr << "Dir : tag=" << dir.tdir_tag
                      << ", type=" << dir.tdir_type
                      << ", count=" << dir.tdir_count
                      << ", offset=" << dir.tdir_offset << "\n";
#endif
        }
    }
}



class tagcompare {
public:
    int operator() (const TIFFDirEntry &a, const TIFFDirEntry &b) {
        return (a.tdir_tag < b.tdir_tag);
    }
};



// Decode a raw Exif data block and save all the metadata in an
// ImageSpec.  Return true if all is ok, false if the exif block was
// somehow malformed.
bool
decode_exif (const void *exif, int length, ImageSpec &spec)
{
    const unsigned char *buf = (const unsigned char *) exif;

#if DEBUG_EXIF_READ
    std::cerr << "Exif dump:\n";
    for (int i = 0;  i < length;  ++i) {
        if (buf[i] >= ' ')
            std::cerr << (char)buf[i] << ' ';
        std::cerr << "(" << (int)(unsigned char)buf[i] << ") ";
    }
    std::cerr << "\n";
#endif

#if 1
    // APP1 blob doesn't have to be exif info.  Look for the exif marker,
    // which near as I can tell is just the letters "Exif" at the start.
    if (strncmp ((const char *)buf, "Exif\0", 5))
        return false;

    buf += 6;   // ...and two nulls follow the "Exif"
    length -= 6;
#endif

    // The next item should be a standard TIFF header.  Note that HERE,
    // not the start of the Exif blob, is where all TIFF offsets are
    // relative to.  The header should have the right magic number (which
    // also tells us the endianness of the data) and an offset to the
    // first TIFF directory.
    //
    // N.B. Just read libtiff's "tiff.h" for info on the structure 
    // layout of TIFF headers and directory entries.  The TIFF spec
    // itself is also helpful in this area.
    const TIFFHeader *head = (const TIFFHeader *)buf;
    if (head->tiff_magic != 0x4949 && head->tiff_magic != 0x4d4d)
        return false;
    bool host_little = littleendian();
    bool file_little = (head->tiff_magic == 0x4949);
    bool swab = (host_little != file_little);
    if (swab)
        swap_endian (&head->tiff_diroff);

    // Read the directory that the header pointed to.  It should contain
    // some number of directory entries containing tags to process.
    const unsigned char *ifd = (buf + head->tiff_diroff);
    unsigned short ndirs = *(const unsigned short *)ifd;
    if (swab)
        swap_endian (&ndirs);
    for (int d = 0;  d < ndirs;  ++d)
        read_exif_tag (spec, (const TIFFDirEntry *) (ifd+2+d*sizeof(TIFFDirEntry)),
                       (const char *)buf, swab);

    // A few tidbits to look for
    ImageIOParameter *p;
    if ((p = spec.find_attribute ("Exif:ColorSpace")) ||
        (p = spec.find_attribute ("ColorSpace"))) {
        int cs = -1;
        if (p->type() == TypeDesc::UINT) 
            cs = *(const unsigned int *)p->data();
        else if (p->type() == TypeDesc::INT) 
            cs = *(const int *)p->data();
        else if (p->type() == TypeDesc::UINT16) 
            cs = *(const unsigned short *)p->data();
        else if (p->type() == TypeDesc::INT16) 
            cs = *(const short *)p->data();
        if (cs == 1)
            spec.linearity = ImageSpec::sRGB;
    }
    return true;
}



static void
float_to_rational (float f, unsigned int &num, unsigned int &den,
                   bool srational=false)
{
    if ((int)f == f) {
        num = (int)f;
        den = 1;
        return;
    }
    if ((int)(1.0/f) == (1.0/f)) {
        num = 1;
        den = (int)f;
        return;
    }
    // Basic algorithm borrowed from libtiff
    float fv = f;
    int sign = 1;
    if (fv < 0) {
        if (!srational) {
            // std::cerr << "Lost sign\n";
        } else {
            fv = -fv, sign = -1;
        }
    }
    den = 1L;
    if (fv > 0) {
        while (fv < 1L<<(31-3) && den < 1L<<(31-3))
            fv *= 1<<3, den *= 1L<<3;
    }
    // Collapse whole numbers so that denominator is 1
    num = (unsigned int) (sign * (fv + 0.5));
}



static void
append_dir_entry (std::vector<TIFFDirEntry> &dirs, std::vector<char> &data,
                  int tag, TIFFDataType type, int count, const void *mydata)
{
    TIFFDirEntry dir;
    dir.tdir_tag = tag;
    dir.tdir_type = type;
    dir.tdir_count = count;
    int len = tiff_data_sizes[(int)type] * count;
    if (len <= 4) {
        dir.tdir_offset = 0;
        memcpy (&dir.tdir_offset, mydata, len);
    } else {
        dir.tdir_offset = data.size();
        data.insert (data.end(), (char *)mydata, (char *)mydata + len);
    }
#if DEBUG_EXIF_WRITE
    std::cerr << "Adding ";
    print_dir_entry (dir, &data[0]);
#endif
    // Don't double-add
    BOOST_FOREACH (TIFFDirEntry &d, dirs) {
        if (d.tdir_tag == tag) {
            d = dir;
            return;
        }
    }
    dirs.push_back (dir);
}



/// Helper: For param that needs to be added as a tag, create a TIFF
/// directory entry for it in dirs and add its data in data.  Set the
/// directory's offset just to the position within data where it will
/// reside.  Don't worry about it being relative to the start of some
/// TIFF structure.
static void
encode_exif_entry (const ImageIOParameter &p, int tag,
                   std::vector<TIFFDirEntry> &dirs,
                   std::vector<char> &data)
{
    TIFFDataType type = tagmap.tifftype (tag);

    switch (type) {
    case TIFF_ASCII :
        if (p.type() == TypeDesc::STRING) {
            const char *s = *(const char **) p.data();
            int len = strlen(s) + 1;
            append_dir_entry (dirs, data, tag, type, len, s);
            return;
        }
        break;
    case TIFF_RATIONAL :
    case TIFF_SRATIONAL :
        if (p.type() == TypeDesc::FLOAT) {
            unsigned int rat[2];  // num, den
            float f = *(const float *)p.data();
            float_to_rational (f, rat[0], rat[1], type == TIFF_SRATIONAL);
            append_dir_entry (dirs, data, tag, type, 1, &rat);
            return;
        }
        break;
    case TIFF_SHORT :
        if (p.type() == TypeDesc::UINT || p.type() == TypeDesc::INT ||
                p.type() == TypeDesc::UINT16 || p.type() == TypeDesc::INT16) {
            unsigned short i;
            switch (p.type().basetype) {
            case TypeDesc::UINT:   i = (unsigned short) *(unsigned int *)p.data(); break;
            case TypeDesc::INT:    i = (unsigned short) *(int *)p.data();   break;
            case TypeDesc::UINT16: i = *(unsigned short *)p.data();         break;
            case TypeDesc::INT16:  i = (unsigned short) *(short *)p.data(); break;
            }
            append_dir_entry (dirs, data, tag, type, 1, &i);
            return;
        }
        break;
    case TIFF_LONG :
        if (p.type() == TypeDesc::UINT || p.type() == TypeDesc::INT ||
                p.type() == TypeDesc::UINT16 || p.type() == TypeDesc::INT16) {
            unsigned int i;
            switch (p.type().basetype) {
            case TypeDesc::UINT:   i = (unsigned short) *(unsigned int *)p.data(); break;
            case TypeDesc::INT:    i = (unsigned short) *(int *)p.data();   break;
            case TypeDesc::UINT16: i = *(unsigned short *)p.data();         break;
            case TypeDesc::INT16:  i = (unsigned short) *(short *)p.data(); break;
            }
            append_dir_entry (dirs, data, tag, type, 1, &i);
            return;
        }
        break;
    default:
        break;
    }
#if DEBUG_EXIF_WRITE
    std::cerr << "  Don't know how to add " << p.name() << ' ' << tag << ' ' << type << ' ' << (int)p.type().basetype << "\n";
#endif
}



// Construct an Exif data block from the ImageSpec, writing the Exif 
// data as a big blob to the char vector.
void
encode_exif (const ImageSpec &spec, std::vector<char> &exif)
{
    // Clear the buffer and reserve maximum space that an APP1 can take
    // in a JPEG file, so we can push_back to our heart's content and
    // know that no offsets or pointers to the exif vector's memory will
    // change due to reallocation.
    exif.clear ();
    exif.reserve (0xffff);

    // Layout:
    //                     "Exif\0\0"
    //    (tiffstart)      TIFFHeader
    //                     number of top dir entries 
    //                     top dir entry 0
    //                     ...
    //                     top dir entry (point to Exif IFD)
    //                     data for top dir entries (except Exif)

    //                     Exif IFD number of dir entries (n)
    //                     Exif IFD entry 0
    //                     ...
    //                     Exif IFD entry n-1
    //                     ...More Data for Exif entries...

    // Start the exif blob with "Exif" and two nulls.  That's how it
    // always is in the JPEG files I've examined.
    exif.push_back ('E');
    exif.push_back ('x');
    exif.push_back ('i');
    exif.push_back ('f');
    exif.push_back (0);
    exif.push_back (0);

    // Here is where the TIFF info starts.  All TIFF tag offsets are
    // relative to this position within the blob.
    int tiffstart = exif.size();

    // Handy macro -- declare a variable positioned at the current end of
    // exif, and grow exif to accommodate it.
#define EXIF_ADD(vartype, varname)                          \
    vartype & varname (* (vartype *) &exif[exif.size()]);   \
    exif.resize (exif.size() + sizeof(vartype));
    
    // Put a TIFF header
    EXIF_ADD (TIFFHeader, head);
    bool host_little = littleendian();
    head.tiff_magic = host_little ? 0x4949 : 0x4d4d;
    head.tiff_version = 42;
    head.tiff_diroff = exif.size() - tiffstart;

    // Placeholder for number of directories
    EXIF_ADD (unsigned short, ndirs);
    ndirs = 0;

    std::vector<TIFFDirEntry> tiffdirs, exifdirs, interopdirs;
    std::vector<char> data;    // Put data here

    // Add TIFF, non-EXIF info here, then go back
    // ...
#if DEBUG_EXIF_WRITE
    std::cerr << "Non-exif tags\n";
#endif
    BOOST_FOREACH (const ImageIOParameter &p, spec.extra_attribs) {
        int tag = tagmap.tag (p.name().string());
        if (tag < EXIFTAG_EXPOSURETIME || tag > EXIFTAG_IMAGEUNIQUEID) {
            encode_exif_entry (p, tag, tiffdirs, data);
        } else {
            encode_exif_entry (p, tag, exifdirs, data);
        }
    }

#if DEBUG_EXIF_WRITE
    std::cerr << "Exif header size " << exif.size() << "\n";
    std::cerr << "tiff tags: " << tiffdirs.size() << "\n";
    std::cerr << "exif tags: " << exifdirs.size() << "\n";
#endif

    if (exifdirs.size()) {
        append_dir_entry (exifdirs, data, 
                          EXIFTAG_EXIFVERSION, TIFF_UNDEFINED, 4, "0220");
        append_dir_entry (exifdirs, data, 
                          EXIFTAG_FLASHPIXVERSION, TIFF_UNDEFINED, 4, "0100");
        char componentsconfig[] = { 1, 2, 3, 0 };
        append_dir_entry (exifdirs, data, 
                          EXIFTAG_COMPONENTSCONFIGURATION, TIFF_UNDEFINED, 4, componentsconfig);
        std::sort (exifdirs.begin(), exifdirs.end(), tagcompare());

        // If we had exif info, add one more main dir entry to point to
        // the private exif tag directory.
        unsigned int size = (unsigned int) data.size();
        append_dir_entry (tiffdirs, data, TIFFTAG_EXIFIFD, TIFF_LONG, 1, &size);

        // Create interop directory boilerplate
        append_dir_entry (interopdirs, data, 1, TIFF_ASCII, 4, "R98");
        append_dir_entry (interopdirs, data, 2, TIFF_UNDEFINED, 4, "0100");
        std::sort (interopdirs.begin(), interopdirs.end(), tagcompare());

#if 0
        // FIXME -- is this necessary?  If so, it's not completed.
        // Add the interop directory IFD entry to the main IFD
        size = (unsigned int) data.size();
        append_dir_entry (tiffdirs, data,
                          TIFFTAG_INTEROPERABILITYIFD, TIFF_LONG, 1, &size);
        std::sort (tiffdirs.begin(), tiffdirs.end(), tagcompare());
#endif
    }

    // Where will the data begin (we need this to adjust the directory
    // offsets once we append data to the exif blob)?
    size_t datastart = exif.size() - tiffstart + 
                       tiffdirs.size() * sizeof(TIFFDirEntry) +
                       4 /* end marker */;

    BOOST_FOREACH (TIFFDirEntry &dir, tiffdirs) {
        const char *name = tagmap.name (dir.tdir_tag);
        if (tiff_data_size (dir) <= 4 &&
            dir.tdir_tag != TIFFTAG_EXIFIFD) {
#if DEBUG_EXIF_WRITE
            std::cerr << "    NO re-offsett entry " << (name ? name : "") << " tag " << dir.tdir_tag << " to " << dir.tdir_offset << '\n';
#endif
            continue;
        }
        dir.tdir_offset += datastart;
#if DEBUG_EXIF_WRITE
        std::cerr << "    re-offsetting entry " << (name ? name : "") << " tag " << dir.tdir_tag << " to " << dir.tdir_offset << '\n';
#endif
    }
    ndirs = tiffdirs.size();
    exif.insert (exif.end(), (char *)&tiffdirs[0],
                 (char *)&tiffdirs[tiffdirs.size()]);
    // 4 bytes of 0 follow the last entry
    int endmarker = 0;
    exif.insert (exif.end(), (char *)&endmarker, (char *)&endmarker + sizeof(int));

    if (exifdirs.size()) {
        BOOST_FOREACH (TIFFDirEntry &dir, exifdirs) {
            const char *name = tagmap.name (dir.tdir_tag);
            if (tiff_data_size (dir) <= 4 &&
                dir.tdir_tag != TIFFTAG_EXIFIFD) {
#if DEBUG_EXIF_WRITE
                std::cerr << "    NO re-offset of exif entry " << " tag " << dir.tdir_tag << " " << (name ? name : "") << " to " << dir.tdir_offset << '\n';
#endif
                continue;
            }
            dir.tdir_offset += datastart;
#if DEBUG_EXIF_WRITE
            std::cerr << "    re-offsetting exif entry " << " tag " << dir.tdir_tag << " " << (name ? name : "") << " to " << dir.tdir_offset << '\n';
#endif
        }
        unsigned short nd = exifdirs.size();
#if DEBUG_EXIF_WRITE
        std::cerr << "WRITING " << nd << " exif directories\n";
#endif
        data.insert (data.end(), (char *)&nd, (char *)&nd + sizeof(nd));
        data.insert (data.end(), (char *)&exifdirs[0], (char *)&exifdirs[exifdirs.size()]);
        // 4 bytes of 0 follow the last entry
        data.insert (data.end(), (char *)&endmarker, (char *)&endmarker + sizeof(int));
    }

    exif.insert (exif.end(), data.begin(), data.end());

#if DEBUG_EXIF_WRITE
    std::cerr << "resulting exif block is a total of " << exif.size() << "\n";

#if 0
    std::cerr << "APP1 dump:\n";
    BOOST_FOREACH (char c, exif) {
        if (c >= ' ')
            std::cerr << c << ' ';
        std::cerr << "(" << (int)c << ") ";
    }
#endif
#endif
}


};  // namespace Jpeg_imageio_pvt

