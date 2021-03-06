/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkFreeTypeTools.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkFreeTypeTools.h"

#include "vtkTextProperty.h"
#include "vtkObjectFactory.h"
#include "vtkMath.h"
#include "vtkPath.h"
#include "vtkImageData.h"
#include "vtkSmartPointer.h"

#include "vtkStdString.h"
#include "vtkUnicodeString.h"

#define VTK_USE_FTGL 0

// FTGL
#if VTK_USE_FTGL
  #include "vtkftglConfig.h"
  #include "FTLibrary.h"
#endif // VTK_USE_FTGL

// The embedded fonts
#include "fonts/vtkEmbeddedFonts.h"

#ifndef _MSC_VER
# include <stdint.h>
#endif

#include <map>

#if VTK_USE_FTGL
#ifdef FTGL_USE_NAMESPACE
using namespace ftgl;
#endif
#endif

// Print debug info
#define VTK_FTFC_DEBUG 0
#define VTK_FTFC_DEBUG_CD 0

class vtkTextPropertyLookup
    : public std::map<unsigned long, vtkSmartPointer<vtkTextProperty> >
{
public:
  bool contains(const unsigned long id) {return this->find(id) != this->end();}
};

//----------------------------------------------------------------------------
vtkInstantiatorNewMacro(vtkFreeTypeTools);

//----------------------------------------------------------------------------
// The singleton, and the singleton cleanup
vtkFreeTypeTools* vtkFreeTypeTools::Instance = NULL;
vtkFreeTypeToolsCleanup vtkFreeTypeTools::Cleanup;

//----------------------------------------------------------------------------
// The embedded fonts
// Create a lookup table between the text mapper attributes
// and the font buffers.
struct EmbeddedFontStruct
{
  size_t length;
  unsigned char *ptr;
};

//----------------------------------------------------------------------------
// This callback will be called by the FTGLibrary singleton cleanup destructor
// if it happens to be destroyed before our singleton (this order is not
// deterministic). It will destroy our singleton, if needed.
void vtkFreeTypeToolsCleanupCallback ()
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeToolsCleanupCallback\n");
#endif
  vtkFreeTypeTools::SetInstance(NULL);
}

//----------------------------------------------------------------------------
// Create the singleton cleanup
// Register our singleton cleanup callback against the FTLibrary so that
// it might be called before the FTLibrary singleton is destroyed.
vtkFreeTypeToolsCleanup::vtkFreeTypeToolsCleanup()
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeToolsCleanup::vtkFreeTypeToolsCleanup\n");
#endif

#if VTK_USE_FTGL
  FTLibraryCleanup::AddDependency(&vtkFreeTypeToolsCleanupCallback);
#endif
}

//----------------------------------------------------------------------------
// Delete the singleton cleanup
// The callback called here might have been called by the FTLibrary singleton
// cleanup first (depending on the destruction order), but in case ours is
// destroyed first, let's call it too.
vtkFreeTypeToolsCleanup::~vtkFreeTypeToolsCleanup()
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeToolsCleanup::~vtkFreeTypeToolsCleanup\n");
#endif
  vtkFreeTypeToolsCleanupCallback();
}

//----------------------------------------------------------------------------
vtkFreeTypeTools* vtkFreeTypeTools::GetInstance()
{
  if (!vtkFreeTypeTools::Instance)
    {
    vtkFreeTypeTools::Instance = static_cast<vtkFreeTypeTools *>(
      vtkObjectFactory::CreateInstance("vtkFreeTypeTools"));
    if (!vtkFreeTypeTools::Instance)
      {
      vtkFreeTypeTools::Instance = new vtkFreeTypeTools;
      }
    }
  return vtkFreeTypeTools::Instance;
}

//----------------------------------------------------------------------------
void vtkFreeTypeTools::SetInstance(vtkFreeTypeTools* instance)
{
  if (vtkFreeTypeTools::Instance == instance)
    {
    return;
    }

  if (vtkFreeTypeTools::Instance)
    {
    vtkFreeTypeTools::Instance->Delete();
    }

  vtkFreeTypeTools::Instance = instance;

  // User will call ->Delete() after setting instance

  if (instance)
    {
    instance->Register(NULL);
    }
}

//----------------------------------------------------------------------------
vtkFreeTypeTools::vtkFreeTypeTools()
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeTools::vtkFreeTypeTools\n");
#endif
  // Force use of compiled fonts by default.
  this->ForceCompiledFonts = true;
  this->MaximumNumberOfFaces = 30; // combinations of family+bold+italic
  this->MaximumNumberOfSizes = this->MaximumNumberOfFaces * 20; // sizes
  this->MaximumNumberOfBytes = 300000UL * this->MaximumNumberOfSizes;
  this->TextPropertyLookup = new vtkTextPropertyLookup ();
  this->CacheManager = NULL;
  this->ImageCache   = NULL;
  this->CMapCache    = NULL;
  this->ScaleToPowerTwo = false;

  // initialize the freetype library instance
  this->FTLibraryInstance = new FT_Library;
  FT_Error err = FT_Init_FreeType(this->FTLibraryInstance);
  if(err)
    {
    delete this->FTLibraryInstance;
    this->FTLibraryInstance = NULL;
    }
}

//----------------------------------------------------------------------------
vtkFreeTypeTools::~vtkFreeTypeTools()
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeTools::~vtkFreeTypeTools\n");
#endif
  this->ReleaseCacheManager();
  delete TextPropertyLookup;

  // cleanup the FT_Library instance
  FT_Done_FreeType(*this->FTLibraryInstance);
  delete this->FTLibraryInstance;
  this->FTLibraryInstance = NULL;
}

//----------------------------------------------------------------------------
FT_Library* vtkFreeTypeTools::GetLibrary()
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeTools::GetLibrary\n");
#endif

#if VTK_USE_FTGL
  FTLibrary *ftgl_lib = FTLibrary::GetInstance();
  if (ftgl_lib)
    {
    return ftgl_lib->GetLibrary();
    }
#endif //VTK_USE_FTGL

  return this->FTLibraryInstance;
}

//----------------------------------------------------------------------------
FTC_Manager* vtkFreeTypeTools::GetCacheManager()
{
  if (!this->CacheManager)
    {
    this->InitializeCacheManager();
    }

  return this->CacheManager;
}

//----------------------------------------------------------------------------
FTC_ImageCache* vtkFreeTypeTools::GetImageCache()
{
  if (!this->ImageCache)
    {
    this->InitializeCacheManager();
    }

  return this->ImageCache;
}

//----------------------------------------------------------------------------
FTC_CMapCache* vtkFreeTypeTools::GetCMapCache()
{
  if (!this->CMapCache)
    {
    this->InitializeCacheManager();
    }

  return this->CMapCache;
}

//----------------------------------------------------------------------------
FT_CALLBACK_DEF(FT_Error)
vtkFreeTypeToolsFaceRequester(FTC_FaceID face_id,
                              FT_Library lib,
                              FT_Pointer request_data,
                              FT_Face* face)
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeToolsFaceRequester()\n");
#endif

  // Get a pointer to the current vtkFreeTypeTools object
  vtkFreeTypeTools *self =
    reinterpret_cast<vtkFreeTypeTools*>(request_data);

  // Map the ID to a text property
  vtkSmartPointer<vtkTextProperty> tprop =
      vtkSmartPointer<vtkTextProperty>::New();
  self->MapIdToTextProperty(reinterpret_cast<intptr_t>(face_id), tprop);

  bool faceIsSet = self->LookupFace(tprop, lib, face);

  if (!faceIsSet)
    {
    return static_cast<FT_Error>(1);
    }

  if ( tprop->GetOrientation() != 0.0 )
    {
    // FreeType documentation says that the transform should not be set
    // but we cache faces also by transform, so that there is a unique
    // (face, orientation) cache entry
    FT_Matrix matrix;
    float angle = vtkMath::RadiansFromDegrees( tprop->GetOrientation() );
    matrix.xx = (FT_Fixed)( cos(angle) * 0x10000L);
    matrix.xy = (FT_Fixed)(-sin(angle) * 0x10000L);
    matrix.yx = (FT_Fixed)( sin(angle) * 0x10000L);
    matrix.yy = (FT_Fixed)( cos(angle) * 0x10000L);
    FT_Set_Transform(*face, &matrix, NULL);
    }

  return static_cast<FT_Error>(0);
}

//----------------------------------------------------------------------------
void vtkFreeTypeTools::InitializeCacheManager()
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeTools::InitializeCacheManager()\n");
#endif

  this->ReleaseCacheManager();

  FT_Error error;

  // Create the cache manager itself
  this->CacheManager = new FTC_Manager;

  error = this->CreateFTCManager();

  if (error)
    {
    vtkErrorMacro(<< "Failed allocating a new FreeType Cache Manager");
    }

  // The image cache
  this->ImageCache = new FTC_ImageCache;
  error = FTC_ImageCache_New(*this->CacheManager, this->ImageCache);

  if (error)
    {
    vtkErrorMacro(<< "Failed allocating a new FreeType Image Cache");
    }

  // The charmap cache
  this->CMapCache = new FTC_CMapCache;
  error = FTC_CMapCache_New(*this->CacheManager, this->CMapCache);

  if (error)
    {
    vtkErrorMacro(<< "Failed allocating a new FreeType CMap Cache");
    }
}

//----------------------------------------------------------------------------
void vtkFreeTypeTools::ReleaseCacheManager()
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeTools::ReleaseCacheManager()\n");
#endif

  if (this->CacheManager)
    {
    FTC_Manager_Done(*this->CacheManager);

    delete this->CacheManager;
    this->CacheManager = NULL;
    }

  if (this->ImageCache)
    {
    delete this->ImageCache;
    this->ImageCache = NULL;
    }

  if (this->CMapCache)
    {
    delete this->CMapCache;
    this->CMapCache = NULL;
    }
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::IsBoundingBoxValid(int bbox[4])
{
  return (!bbox ||
          bbox[0] == VTK_INT_MAX || bbox[1] == VTK_INT_MIN ||
          bbox[2] == VTK_INT_MAX || bbox[3] == VTK_INT_MIN) ? false : true;
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::GetBoundingBox(vtkTextProperty *tprop,
                                      const vtkStdString& str,
                                      int bbox[4])
{
  // We need the tprop and bbox
  if (!tprop || !bbox)
    {
    vtkErrorMacro(<< "Wrong parameters, one of them is NULL or zero");
    return false;
    }

  // No string to render, bail out now
  if (str.empty())
    {
    return false;
    }

  return this->CalculateBoundingBox(tprop, str, bbox);
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::GetBoundingBox(vtkTextProperty *tprop,
                                     const vtkUnicodeString& str,
                                     int bbox[4])
{
  // We need the tprop and bbox
  if (!tprop || !bbox)
    {
    vtkErrorMacro(<< "Wrong parameters, one of them is NULL or zero");
    return false;
    }

  // No string to render, bail out now
  if (str.empty())
    {
    return false;
    }

  return this->CalculateBoundingBox(tprop, str, bbox);
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::RenderString(vtkTextProperty *tprop,
                                   const vtkStdString& str,
                                   vtkImageData *data)
{
  // Check parameters
  if (!tprop || !data)
    {
    vtkErrorMacro(<< "Wrong parameters, one of them is NULL or zero");
    return false;
    }

  if (data->GetNumberOfScalarComponents() > 4)
    {
    vtkErrorMacro("The image data must have a maximum of four components");
    return false;
    }

  if (str.empty())
    {
    return false;
    }

  // Prepare the ImageData to receive the text
  int x = 0;
  int y = 0;
  this->PrepareImageData(data, tprop, str, &x, &y);

  // Execute text
  return this->PopulateImageData(tprop, str, x, y, data);
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::RenderString(vtkTextProperty *tprop,
                                   const vtkUnicodeString& str,
                                   vtkImageData *data)
{
  // Check parameters
  if (!tprop || !data)
    {
    vtkErrorMacro(<< "Wrong parameters, one of them is NULL or zero");
    return false;
    }

  if (data->GetNumberOfScalarComponents() > 4)
    {
    vtkErrorMacro("The image data must have a maximum of four components");
    return false;
    }

  if (str.empty())
    {
    return false;
    }

  // Prepare the ImageData to receive the text
  int x = 0;
  int y = 0;
  this->PrepareImageData(data, tprop, str, &x, &y);

  // Execute text
  return this->PopulateImageData(tprop, str, x, y, data);
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::StringToPath(vtkTextProperty *tprop,
                                    const vtkStdString &str, vtkPath *path)
{
  return this->PopulatePath<vtkStdString>(tprop, str, 0, 0, path);
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::StringToPath(vtkTextProperty *tprop,
                                    const vtkUnicodeString &str, vtkPath *path)
{
  return this->PopulatePath<vtkUnicodeString>(tprop, str, 0, 0, path);
}

//----------------------------------------------------------------------------
vtkTypeUInt16 vtkFreeTypeTools::HashString(const char *str)
{
  if (str == NULL)
    return 0;

  vtkTypeUInt16 hash = 0;
  while (*str != 0)
    {
    vtkTypeUInt8 high = ((hash<<8)^hash) >> 8;
    vtkTypeUInt8 low = tolower(*str)^(hash<<2);
    hash = (high<<8) ^ low;
    ++str;
    }

  return hash;
}

//----------------------------------------------------------------------------
void vtkFreeTypeTools::MapTextPropertyToId(vtkTextProperty *tprop,
                                           unsigned long *id)
{
  if (!tprop || !id)
    {
    vtkErrorMacro(<< "Wrong parameters, one of them is NULL");
    return;
    }

  // Set the first bit to avoid id = 0
  // (the id will be mapped to a pointer, FTC_FaceID, so let's avoid NULL)
  *id = 1;
  int bits = 1;

  // The font family is hashed into 16 bits (= 17 bits so far)
  // (2 would be enough right now, but who knows, it might grow)
  // Avoid unknown as this can cause segfaluts - this should be fixed...
  *id |= vtkFreeTypeTools::HashString(tprop->GetFontFamilyAsString()) << bits;
  bits += 16;

  // Bold is in 1 bit (= 18 bits so far)
  int bold = (tprop->GetBold() ? 1 : 0) << bits;
  ++bits;

  // Italic is in 1 bit (= 19 bits so far)
  int italic = (tprop->GetItalic() ? 1 : 0) << bits;
  ++bits;

  // Orientation (in degrees)
  // We need 9 bits for 0 to 360. What do we need for more precisions:
  // - 1/10th degree: 12 bits (11.8) (31 bits)
  int angle = (vtkMath::Round(tprop->GetOrientation() * 10.0) % 3600) << bits;

  // We really should not use more than 32 bits
  // Now final id
  *id |= bold | italic | angle;

  // Insert the TextProperty into the lookup table
  if (!this->TextPropertyLookup->contains(*id))
    (*this->TextPropertyLookup)[*id] = tprop;
}

//----------------------------------------------------------------------------
void vtkFreeTypeTools::MapIdToTextProperty(unsigned long id,
                                           vtkTextProperty *tprop)
{
  if (!tprop)
    {
    vtkErrorMacro(<< "Wrong parameters, one of them is NULL");
    return;
    }

  vtkTextPropertyLookup::const_iterator tpropIt =
      this->TextPropertyLookup->find(id);

  if (tpropIt == this->TextPropertyLookup->end())
    {
    vtkErrorMacro(<<"Unknown id; call MapTextPropertyToId first!");
    return;
    }

  tprop->ShallowCopy(tpropIt->second);
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::GetSize(unsigned long tprop_cache_id,
                               int font_size,
                               FT_Size *size)
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeTools::GetSize()\n");
#endif

  if (!size || font_size <= 0)
    {
    vtkErrorMacro(<< "Wrong parameters, size is NULL or invalid font size");
    return 0;
    }

  FTC_Manager *manager = this->GetCacheManager();
  if (!manager)
    {
    vtkErrorMacro(<< "Failed querying the cache manager !");
    return 0;
    }

  // Map the id of a text property in the cache to a FTC_FaceID
  FTC_FaceID face_id = reinterpret_cast<FTC_FaceID>(tprop_cache_id);

  FTC_ScalerRec scaler_rec;
  scaler_rec.face_id = face_id;
  scaler_rec.width = font_size;
  scaler_rec.height = font_size;
  scaler_rec.pixel = 1;

  FT_Error error = FTC_Manager_LookupSize(*manager, &scaler_rec, size);
  if (error)
    {
    vtkErrorMacro(<< "Failed looking up a FreeType Size");
    }

  return error ? false : true;
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::GetSize(vtkTextProperty *tprop,
                               FT_Size *size)
{
  if (!tprop)
    {
    vtkErrorMacro(<< "Wrong parameters, text property is NULL");
    return 0;
    }

  // Map the text property to a unique id that will be used as face id
  unsigned long tprop_cache_id;
  this->MapTextPropertyToId(tprop, &tprop_cache_id);

  return this->GetSize(tprop_cache_id, tprop->GetFontSize(), size);
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::GetFace(unsigned long tprop_cache_id,
                               FT_Face *face)
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeTools::GetFace()\n");
#endif

  if (!face)
    {
    vtkErrorMacro(<< "Wrong parameters, face is NULL");
    return false;
    }

  FTC_Manager *manager = this->GetCacheManager();
  if (!manager)
    {
    vtkErrorMacro(<< "Failed querying the cache manager !");
    return false;
    }

  // Map the id of a text property in the cache to a FTC_FaceID
  FTC_FaceID face_id = reinterpret_cast<FTC_FaceID>(tprop_cache_id);

  FT_Error error = FTC_Manager_LookupFace(*manager, face_id, face);
  if (error)
    {
    vtkErrorMacro(<< "Failed looking up a FreeType Face");
    }

  return error ? false : true;
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::GetFace(vtkTextProperty *tprop,
                               FT_Face *face)
{
  if (!tprop)
    {
    vtkErrorMacro(<< "Wrong parameters, face is NULL");
    return 0;
    }

  // Map the text property to a unique id that will be used as face id
  unsigned long tprop_cache_id;
  this->MapTextPropertyToId(tprop, &tprop_cache_id);

  return this->GetFace(tprop_cache_id, face);
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::GetGlyphIndex(unsigned long tprop_cache_id,
                                     FT_UInt32 c,
                                     FT_UInt *gindex)
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeTools::GetGlyphIndex()\n");
#endif

  if (!gindex)
    {
    vtkErrorMacro(<< "Wrong parameters, gindex is NULL");
    return 0;
    }

  FTC_CMapCache *cmap_cache = this->GetCMapCache();
  if (!cmap_cache)
    {
    vtkErrorMacro(<< "Failed querying the charmap cache manager !");
    return 0;
    }

  // Map the id of a text property in the cache to a FTC_FaceID
  FTC_FaceID face_id = reinterpret_cast<FTC_FaceID>(tprop_cache_id);

  // Lookup the glyph index
  *gindex = FTC_CMapCache_Lookup(*cmap_cache, face_id, 0, c);

  return *gindex ? true : false;
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::GetGlyphIndex(vtkTextProperty *tprop,
                                     FT_UInt32 c,
                                     FT_UInt *gindex)
{
  if (!tprop)
    {
    vtkErrorMacro(<< "Wrong parameters, text property is NULL");
    return 0;
    }

  // Map the text property to a unique id that will be used as face id
  unsigned long tprop_cache_id;
  this->MapTextPropertyToId(tprop, &tprop_cache_id);

  return this->GetGlyphIndex(tprop_cache_id, c, gindex);
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::GetGlyph(unsigned long tprop_cache_id,
                               int font_size,
                               FT_UInt gindex,
                               FT_Glyph *glyph,
                               int request)
{
#if VTK_FTFC_DEBUG_CD
  printf("vtkFreeTypeTools::GetGlyph()\n");
#endif

  if (!glyph)
    {
    vtkErrorMacro(<< "Wrong parameters, one of them is NULL");
    return false;
    }

  FTC_ImageCache *image_cache = this->GetImageCache();
  if (!image_cache)
    {
    vtkErrorMacro(<< "Failed querying the image cache manager !");
    return false;
    }

  // Map the id of a text property in the cache to a FTC_FaceID
  FTC_FaceID face_id = reinterpret_cast<FTC_FaceID>(tprop_cache_id);

  // Which font are we looking for
  FTC_ImageTypeRec image_type_rec;
  image_type_rec.face_id = face_id;
  image_type_rec.width = font_size;
  image_type_rec.height = font_size;
  image_type_rec.flags = FT_LOAD_DEFAULT;
  if (request == GLYPH_REQUEST_BITMAP)
    {
    image_type_rec.flags |= FT_LOAD_RENDER;
    }
  else if (request == GLYPH_REQUEST_OUTLINE)
    {
    image_type_rec.flags |= FT_LOAD_NO_BITMAP;
    }

  // Lookup the glyph
  FT_Error error = FTC_ImageCache_Lookup(
    *image_cache, &image_type_rec, gindex, glyph, NULL);

  return error ? false : true;
}

bool vtkFreeTypeTools::LookupFace(vtkTextProperty *tprop, FT_Library lib,
                                  FT_Face *face)
{
  // Fonts, organized by [Family][Bold][Italic]
  static EmbeddedFontStruct EmbeddedFonts[3][2][2] =
    {
      {
        {
          { // VTK_ARIAL: Bold [ ] Italic [ ]
            face_arial_buffer_length, face_arial_buffer
          },
          { // VTK_ARIAL: Bold [ ] Italic [x]
            face_arial_italic_buffer_length, face_arial_italic_buffer
          }
        },
        {
          { // VTK_ARIAL: Bold [x] Italic [ ]
            face_arial_bold_buffer_length, face_arial_bold_buffer
          },
          { // VTK_ARIAL: Bold [x] Italic [x]
            face_arial_bold_italic_buffer_length, face_arial_bold_italic_buffer
          }
        }
      },
      {
        {
          { // VTK_COURIER: Bold [ ] Italic [ ]
            face_courier_buffer_length, face_courier_buffer
          },
          { // VTK_COURIER: Bold [ ] Italic [x]
            face_courier_italic_buffer_length, face_courier_italic_buffer
          }
        },
        {
          { // VTK_COURIER: Bold [x] Italic [ ]
            face_courier_bold_buffer_length, face_courier_bold_buffer
          },
          { // VTK_COURIER: Bold [x] Italic [x]
            face_courier_bold_italic_buffer_length,
            face_courier_bold_italic_buffer
          }
        }
      },
      {
        {
          { // VTK_TIMES: Bold [ ] Italic [ ]
            face_times_buffer_length, face_times_buffer
          },
          { // VTK_TIMES: Bold [ ] Italic [x]
            face_times_italic_buffer_length, face_times_italic_buffer
          }
        },
        {
          { // VTK_TIMES: Bold [x] Italic [ ]
            face_times_bold_buffer_length, face_times_bold_buffer
          },
          { // VTK_TIMES: Bold [x] Italic [x]
            face_times_bold_italic_buffer_length, face_times_bold_italic_buffer
          }
        }
      }
    };

  int family = tprop->GetFontFamily();
  // If font family is unknown, fall back to Arial.
  if (family == VTK_UNKNOWN_FONT)
    {
    vtkDebugWithObjectMacro(
          tprop,
          << "Requested font '" << tprop->GetFontFamilyAsString() << "'"
          " unavailable. Substituting Arial.");
    family = VTK_ARIAL;
    }

  FT_Long length = EmbeddedFonts
    [family][tprop->GetBold()][tprop->GetItalic()].length;
  FT_Byte *ptr = EmbeddedFonts
    [family][tprop->GetBold()][tprop->GetItalic()].ptr;

  // Create a new face from the embedded fonts if possible

  // If the font face is of type unknown, attempt to load it from disk
  FT_Error error = FT_New_Memory_Face(lib, ptr, length, 0, face);

  if (error)
    {
    vtkErrorWithObjectMacro(
          tprop,
          << "Unable to create font !" << " (family: " << family
          << ", bold: " << tprop->GetBold() << ", italic: " << tprop->GetItalic()
          << ", length: " << length << ")");
    return false;
    }
  else
    {
#if VTK_FTFC_DEBUG
    cout << "Requested: " << *face
         << " (F: " << tprop->GetFontFamily()
         << ", B: " << tprop->GetBold()
         << ", I: " << tprop->GetItalic()
         << ", O: " << tprop->GetOrientation() << ")" << endl;
#endif
    }

  return true;
}

//----------------------------------------------------------------------------
bool vtkFreeTypeTools::GetGlyph(vtkTextProperty *tprop,
                                FT_UInt32 c,
                                FT_Glyph *glyph,
                                int request)
{
  if (!tprop)
    {
    vtkErrorMacro(<< "Wrong parameters, text property is NULL");
    return 0;
    }

  // Map the text property to a unique id that will be used as face id
  unsigned long tprop_cache_id;
  this->MapTextPropertyToId(tprop, &tprop_cache_id);

  // Get the character/glyph index
  FT_UInt gindex;
  if (!this->GetGlyphIndex(tprop_cache_id, c, &gindex))
    {
    vtkErrorMacro(<< "Failed querying a glyph index");
    return false;
    }

  // Get the glyph
  return this->GetGlyph(
    tprop_cache_id, tprop->GetFontSize(), gindex, glyph, request);
}

//----------------------------------------------------------------------------
void vtkFreeTypeTools::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);

  os << indent << "MaximumNumberOfFaces: "
     << this->MaximumNumberOfFaces << endl;

  os << indent << "MaximumNumberOfSizes: "
     << this->MaximumNumberOfSizes << endl;

  os << indent << "MaximumNumberOfBytes: "
     << this->MaximumNumberOfBytes << endl;

  os << indent << "Scale to nearest power of 2 for image sizes: "
     << this->ScaleToPowerTwo << endl;
}

//----------------------------------------------------------------------------
FT_Error vtkFreeTypeTools::CreateFTCManager()
{
  return FTC_Manager_New(*this->GetLibrary(),
                         this->MaximumNumberOfFaces,
                         this->MaximumNumberOfSizes,
                         this->MaximumNumberOfBytes,
                         vtkFreeTypeToolsFaceRequester,
                         static_cast<FT_Pointer>(this),
                         this->CacheManager);
}

//----------------------------------------------------------------------------
template <typename T>
bool vtkFreeTypeTools::CalculateBoundingBox(vtkTextProperty *tprop,
                                            const T& str,
                                            int bbox[4])
{
  // Initialize bbox minima to 0 -- this is the starting point of the pen,
  // omitting it will not consider the first character's bearing.
  bbox[0] = bbox[2] = 0;
  // This will be updated as the glyphs bboxes are tested:
  bbox[1] = bbox[3] = VTK_INT_MIN;

  // Map the text property to a unique id that will be used as face id, get the
  // font face and establish whether kerning information is available.
  unsigned long tprop_cache_id;
  FT_Face face;
  bool face_has_kerning = false;
  if (!this->GetFace(tprop, tprop_cache_id, face, face_has_kerning))
    {
    return false;
    }

  int tprop_font_size = tprop->GetFontSize();
  FT_BitmapGlyph bitmap_glyph;
  FT_Bitmap *bitmap;
  FT_UInt gindex, previous_gindex = 0;
  FT_Vector kerning_delta;

  int x = 0, y = 0;

  // Render char by char
  for (typename T::const_iterator it = str.begin(); it != str.end(); ++it)
    {
    bitmap = this->GetBitmap(*it, tprop_cache_id, tprop_font_size, gindex,
                             bitmap_glyph);
    if (!bitmap)
      {
      // Glyph not found in the face. FIXME: do something more elegant here.
      // We should render an empty rectangle to adhere to the specs...
      continue;
      }

    if (bitmap->width && bitmap->rows)
      {
      // Starting position given the bearings.  Move the pen to the upper-left
      // extent of this character.

      // Subtract 1 to the bearing Y, because this is the vertical distance
      // from the glyph origin (0,0) to the topmost pixel of the glyph bitmap
      // (more precisely, to the pixel just above the bitmap). This distance is
      // expressed in integer pixels, and is positive for upwards y.
      int pen_x = x + bitmap_glyph->left;
      int pen_y = y + bitmap_glyph->top - 1;

      // Add the kerning
      if (face_has_kerning && previous_gindex && gindex)
        {
        FT_Get_Kerning(
          face, previous_gindex, gindex, ft_kerning_default, &kerning_delta);
        pen_x += kerning_delta.x >> 6;
        pen_y += kerning_delta.y >> 6;
        }

      previous_gindex = gindex;

      // Update bounding box
      if (pen_x < bbox[0])
        {
        bbox[0] = pen_x;
        }
      if (pen_y > bbox[3])
        {
        bbox[3] = pen_y;
        }
      // now move the pen to the lower-right corner of this character and
      // update the bounding box if appropriate
      pen_x += bitmap->width;
      pen_y -= bitmap->rows;

      if (pen_x > bbox[1])
        {
        bbox[1] = pen_x;
        }
      if (pen_y < bbox[2])
        {
        bbox[2] = pen_y;
        }
      }

    // Advance to next char
    x += (bitmap_glyph->root.advance.x + 0x8000) >> 16;
    y += (bitmap_glyph->root.advance.y + 0x8000) >> 16;
    }

  return true;
}

template <typename T>
void vtkFreeTypeTools::PrepareImageData(vtkImageData *data,
                                        vtkTextProperty *tprop,
                                        const T& str,
                                        int *x, int *y)
{
  int text_bbox[4];
  if (!this->GetBoundingBox(tprop, str, text_bbox))
    {
    vtkErrorMacro(<<"Could not get a valid bounding box.");
    return;
    }
  if (!this->IsBoundingBoxValid(text_bbox))
    {
    vtkErrorMacro(<<"no text in input");
    return;
    }
  // The bounding box was the area that is going to be filled with pixels
  // given a text origin of (0, 0). Now get the real size we need, i.e.
  // the full extent from the origin to the bounding box.
  int text_size[2];
  text_size[0] = (text_bbox[1] - text_bbox[0] + 1);// + abs(text_bbox[0]);
  text_size[1] = (text_bbox[3] - text_bbox[2] + 1);// + abs(text_bbox[2]);

  // If the RGBA image data is too small, resize it to the next power of 2
  // WARNING: at this point, since this image is going to be a texture
  // we should limit its size or query the hardware

  data->SetSpacing(1.0, 1.0, 1.0);

  // If the current image data is too small to render the text,
  // or more than twice as big (too hungry), then resize
  int img_dims[3], new_img_dims[3];
  data->GetDimensions(img_dims);

  if (data->GetScalarType() != VTK_UNSIGNED_CHAR ||
      data->GetNumberOfScalarComponents() != 4 ||
      img_dims[0] < text_size[0] || img_dims[1] < text_size[1] ||
      text_size[0] * 2 < img_dims[0] || text_size[1] * 2 < img_dims[0])
    {
    // Scale to the next highest power of 2 if required.
    if (this->ScaleToPowerTwo)
      {
      new_img_dims[0] =
          1 << static_cast<int>(ceil(log(static_cast<double>(text_size[0]+1)) /
                                     log(2.0)));
      new_img_dims[1] =
          1 << static_cast<int>(ceil(log(static_cast<double>(text_size[1]+1)) /
                                     log(2.0)));
      }
    else
      {
      new_img_dims[0] = text_size[0]+1;
      new_img_dims[1] = text_size[1]+1;
      }
    new_img_dims[2] = 1;
    if (data->GetScalarType() != VTK_UNSIGNED_CHAR ||
        data->GetNumberOfScalarComponents() != 4 ||
        new_img_dims[0] != img_dims[0] ||
        new_img_dims[1] != img_dims[1] ||
        new_img_dims[2] != img_dims[2])
      {
      data->SetDimensions(new_img_dims);
      data->AllocateScalars(VTK_UNSIGNED_CHAR, 4);
      data->SetOrigin(text_size[0], text_size[1], 0.0);
      data->SetSpacing((text_size[0]) / static_cast<double>(new_img_dims[0]),
                       (text_size[1]) / static_cast<double>(new_img_dims[1]),
                       0.0);
      }
    }

  // Render inside the image data
  *x = (text_bbox[0] < 0 ? -text_bbox[0] : 0);
  *y = (text_bbox[2] < 0 ? -text_bbox[2] : 0);

  memset(data->GetScalarPointer(), 0,
          (data->GetNumberOfPoints() *
            data->GetNumberOfScalarComponents()));
}

//----------------------------------------------------------------------------
template <typename T>
bool vtkFreeTypeTools::PopulateImageData(vtkTextProperty *tprop,
                                         const T& str,
                                         int x, int y,
                                         vtkImageData *data)
{
  // Map the text property to a unique id that will be used as face id, get the
  // font face and establish whether kerning information is available.
  unsigned long tprop_cache_id;
  FT_Face face;
  bool face_has_kerning = false;
  if (!this->GetFace(tprop, tprop_cache_id, face, face_has_kerning))
    {
    return false;
    }

  // Text property size and opacity
  int tprop_font_size = tprop->GetFontSize();
  float tprop_opacity = tprop->GetOpacity();

  // Image params (increments, range)
  vtkIdType data_inc_x, data_inc_y, data_inc_z;
  data->GetIncrements(data_inc_x, data_inc_y, data_inc_z);

  double data_min, data_max;
  if (data->GetScalarType() == VTK_DOUBLE || data->GetScalarType() == VTK_FLOAT)
    {
    data_min = 0.0;
    data_max = 1.0;
    }
  else
    {
    data_min = data->GetScalarTypeMin();
    data_max = data->GetScalarTypeMax();
    }
  double data_range = data_max - data_min;

  // Calculate the text color to set in the tight loop.
  double color[3];
  tprop->GetColor(color);
  unsigned char text_color[] = {
    static_cast<unsigned char>(data_min + data_range * color[0]),
    static_cast<unsigned char>(data_min + data_range * color[1]),
    static_cast<unsigned char>(data_min + data_range * color[2]) };

  FT_BitmapGlyph bitmap_glyph;
  FT_Bitmap *bitmap;
  FT_UInt gindex, previous_gindex = 0;
  FT_Vector kerning_delta;

  // Render char by char
  for (typename T::const_iterator it = str.begin(); it != str.end(); ++it)
    {
    bitmap = this->GetBitmap(*it, tprop_cache_id, tprop_font_size, gindex,
                             bitmap_glyph);
    if (!bitmap)
      {
      // Glyph not found in the face.
      continue;
      }

    if (bitmap->width && bitmap->rows)
      {
      // Starting position given the bearings.
      // Subtract 1 to the bearing Y, because this is the vertical distance
      // from the glyph origin (0,0) to the topmost pixel of the glyph bitmap
      // (more precisely, to the pixel just above the bitmap). This distance is
      // expressed in integer pixels, and is positive for upwards y.
      int pen_x = x + bitmap_glyph->left;
      int pen_y = y + bitmap_glyph->top - 1;

      // Add the kerning
      if (face_has_kerning && previous_gindex && gindex)
        {
        FT_Get_Kerning(
          face, previous_gindex, gindex, ft_kerning_default, &kerning_delta);
        pen_x += kerning_delta.x >> 6;
        pen_y += kerning_delta.y >> 6;
        }
      previous_gindex = gindex;

      // Render the current face.
      unsigned char *data_ptr =
        static_cast<unsigned char *>(data->GetScalarPointer(pen_x, pen_y, 0));
      if(!data_ptr)
        {
        return false;
        }

      int data_pitch = (-data->GetDimensions()[0] - bitmap->width) * data_inc_x;
      unsigned char *glyph_ptr_row = bitmap->buffer;
      unsigned char *glyph_ptr;
      float t_alpha, data_alpha, t_1_m_alpha;

      for (int j = 0; j < bitmap->rows; ++j)
        {
        glyph_ptr = glyph_ptr_row;

        for (int i = 0; i < bitmap->width; ++i)
          {
          t_alpha = tprop_opacity * (*glyph_ptr / 255.0);
          t_1_m_alpha = 1.0 - t_alpha;
          data_alpha = (data_ptr[3] - data_min) / data_range;
          *data_ptr = text_color[0];
          ++data_ptr;
          *data_ptr = text_color[1];
          ++data_ptr;
          *data_ptr = text_color[2];
          ++data_ptr;
          *data_ptr = static_cast<unsigned char>(
            data_min + data_range * (t_alpha + data_alpha * t_1_m_alpha));
          ++data_ptr;
          ++glyph_ptr;
          }
        glyph_ptr_row += bitmap->pitch;
        data_ptr += data_pitch;
        }
      }

    // Advance to next char
    x += (bitmap_glyph->root.advance.x + 0x8000) >> 16;
    y += (bitmap_glyph->root.advance.y + 0x8000) >> 16;
    }
  return true;
}

//----------------------------------------------------------------------------
template <typename T>
bool vtkFreeTypeTools::PopulatePath(vtkTextProperty *tprop,
                                    const T& str,
                                    int x, int y,
                                    vtkPath *path)
{
  if (!path)
    {
    return false;
    }

  path->Reset();

  // Map the text property to a unique id that will be used as face id, get the
  // font face and establish whether kerning information is available.
  unsigned long tprop_cache_id;
  FT_Face face;
  bool face_has_kerning = false;
  if (!this->GetFace(tprop, tprop_cache_id, face, face_has_kerning))
    {
    return false;
    }

  // Text property size and opacity
  int tprop_font_size = tprop->GetFontSize();

  FT_OutlineGlyph outline_glyph;
  FT_Outline *outline;
  FT_UInt gindex, previous_gindex = 0;
  FT_Vector kerning_delta;

  // The FT_CURVE defines don't really work in a switch...only the first two
  // bits are meaningful, and the rest appear to be garbage. We'll convert them
  // into values in the enum below:
  enum controlType
    {
    FIRST_POINT,
    ON_POINT,
    CUBIC_POINT,
    CONIC_POINT
    };

  // Bounding box for all control points, used for justification
  double cbox[4] = {VTK_FLOAT_MAX, VTK_FLOAT_MAX, VTK_FLOAT_MIN, VTK_FLOAT_MIN};

  // Render char by char
  for (typename T::const_iterator it = str.begin(); it != str.end(); ++it)
    {
    outline = this->GetOutline(*it, tprop_cache_id, tprop_font_size, gindex,
                               outline_glyph);
    if (!outline)
      {
      // Glyph not found in the face.
      continue;
      }

    if (outline->n_points > 0)
      {
      int pen_x = x;
      int pen_y = y;

      // Add the kerning
      if (face_has_kerning && previous_gindex && gindex)
        {
        FT_Get_Kerning(
              face, previous_gindex, gindex, ft_kerning_default, &kerning_delta);
        pen_x += kerning_delta.x >> 6;
        pen_y += kerning_delta.y >> 6;
        }
      previous_gindex = gindex;

      short point = 0;
      for (short contour = 0; contour < outline->n_contours; ++contour)
        {
        short contourEnd = outline->contours[contour];
        controlType lastTag = FIRST_POINT;
        double contourStartVec[2];
        contourStartVec[0] = contourStartVec[1] = 0.0;
        double lastVec[2];
        lastVec[0] = lastVec[1] = 0.0;
        for (; point <= contourEnd; ++point)
          {
          FT_Vector ftvec = outline->points[point];
          char fttag = outline->tags[point];
          controlType tag = FIRST_POINT;
          if (fttag & FT_CURVE_TAG_ON)
            {
            tag = ON_POINT;
            }
          else if (fttag & FT_CURVE_TAG_CUBIC)
            {
            tag = CUBIC_POINT;
            }
          else if (fttag & FT_CURVE_TAG_CONIC)
            {
            tag = CONIC_POINT;
            }

          double vec[2];
          vec[0] = ftvec.x / 64.0 + x;
          vec[1] = ftvec.y / 64.0 + y;

          // Update (control) bounding box
          if (vec[0] < cbox[0])
            {
            cbox[0] = vec[0];
            }
          if (vec[1] < cbox[1])
            {
            cbox[1] = vec[1];
            }
          if (vec[0] > cbox[2])
            {
            cbox[2] = vec[0];
            }
          if (vec[1] > cbox[3])
            {
            cbox[3] = vec[1];
            }

          // Handle the first point here, unless it is a CONIC point, in which
          // case the switches below handle it.
          if (lastTag == FIRST_POINT && tag != CONIC_POINT)
            {
            path->InsertNextPoint(vec[0], vec[1], 0.0, vtkPath::MOVE_TO);
            lastTag = tag;
            lastVec[0] = vec[0];
            lastVec[1] = vec[1];
            contourStartVec[0] = vec[0];
            contourStartVec[1] = vec[1];
            continue;
            }

          switch (tag)
            {
            case ON_POINT:
              switch(lastTag)
                {
                case ON_POINT:
                  path->InsertNextPoint(vec[0], vec[1], 0.0, vtkPath::LINE_TO);
                  break;
                case CONIC_POINT:
                  path->InsertNextPoint(vec[0], vec[1], 0.0,
                                        vtkPath::CONIC_CURVE);
                  break;
                case CUBIC_POINT:
                  path->InsertNextPoint(vec[0], vec[1], 0.0,
                                        vtkPath::CUBIC_CURVE);
                  break;
                case FIRST_POINT:
                default:
                  break;
                }
              break;
            case CONIC_POINT:
              switch(lastTag)
                {
                case ON_POINT:
                  path->InsertNextPoint(vec[0], vec[1], 0.0,
                                        vtkPath::CONIC_CURVE);
                  break;
                case CONIC_POINT: {
                  // Two conic points indicate a virtual "ON" point between
                  // them. Insert both points.
                  double virtualOn[2] = {(vec[0] + lastVec[0]) * 0.5,
                                         (vec[1] + lastVec[1]) * 0.5};
                  path->InsertNextPoint(virtualOn[0], virtualOn[1], 0.0,
                                        vtkPath::CONIC_CURVE);
                  path->InsertNextPoint(vec[0], vec[1], 0.0,
                                        vtkPath::CONIC_CURVE);
                  }
                  break;
                case FIRST_POINT: {
                  // The first point in the contour can be a conic control
                  // point. Use the last point of the contour as the starting
                  // point. If the last point is a conic point as well, start
                  // on a virtual point between the two:
                  FT_Vector lastContourFTVec = outline->points[contourEnd];
                  double lastContourVec[2] = {lastContourFTVec.x / 64.0 + x,
                                              lastContourFTVec.y / 64.0 + y};
                  char lastContourFTTag = outline->tags[contourEnd];
                  if (lastContourFTTag & FT_CURVE_TAG_CONIC)
                    {
                    double virtualOn[2] = {(vec[0] + lastContourVec[0]) * 0.5,
                                           (vec[1] + lastContourVec[1]) * 0.5};
                    path->InsertNextPoint(virtualOn[0], virtualOn[1],
                                          0.0, vtkPath::MOVE_TO);
                    path->InsertNextPoint(vec[0], vec[1], 0.0,
                                          vtkPath::CONIC_CURVE);
                    }
                  else
                    {
                    path->InsertNextPoint(lastContourVec[0], lastContourVec[1],
                                          0.0, vtkPath::MOVE_TO);
                    path->InsertNextPoint(vec[0], vec[1], 0.0,
                                          vtkPath::CONIC_CURVE);
                    }
                  }
                  break;
                case CUBIC_POINT:
                default:
                  break;
                }
              break;
            case CUBIC_POINT:
              switch(lastTag)
                {
                case ON_POINT:
                case CUBIC_POINT:
                  path->InsertNextPoint(vec[0], vec[1], 0.0,
                                        vtkPath::CUBIC_CURVE);
                  break;
                case CONIC_POINT:
                case FIRST_POINT:
                default:
                  break;
                }
              break;
            case FIRST_POINT:
            default:
              break;
            } // end switch

          lastTag = tag;
          lastVec[0] = vec[0];
          lastVec[1] = vec[1];
          } // end contour

        // The contours are always implicitly closed to the start point of the
        // contour:
        switch (lastTag)
          {
          case ON_POINT:
            path->InsertNextPoint(contourStartVec[0], contourStartVec[1], 0.0,
                                  vtkPath::LINE_TO);
            break;
          case CUBIC_POINT:
            path->InsertNextPoint(contourStartVec[0], contourStartVec[1], 0.0,
                                  vtkPath::CUBIC_CURVE);
            break;
          case CONIC_POINT:
            path->InsertNextPoint(contourStartVec[0], contourStartVec[1], 0.0,
                                  vtkPath::CONIC_CURVE);
            break;
          case FIRST_POINT:
          default:
            break;
          }

        } // end if n_points > 0
      } // end character iteration

    // Advance to next char
    x += (outline_glyph->root.advance.x + 0x8000) >> 16;
    y += (outline_glyph->root.advance.y + 0x8000) >> 16;
    }

  double delta[2] = {0.0, 0.0};

  // Apply justification:
  switch (tprop->GetJustification())
    {
    default:
    case VTK_TEXT_LEFT:
      delta[0] = -cbox[0];
      break;
    case VTK_TEXT_CENTERED:
      delta[0] = -(cbox[2] - cbox[0]) * 0.5;
      break;
    case VTK_TEXT_RIGHT:
      delta[0] = -cbox[2];
      break;
    }
  switch (tprop->GetVerticalJustification())
    {
    default:
    case VTK_TEXT_BOTTOM:
      delta[1] = -cbox[1];
      break;
    case VTK_TEXT_CENTERED:
      delta[1] = -(cbox[3] - cbox[1]) * 0.5;
      break;
    case VTK_TEXT_TOP:
      delta[1] = -cbox[3];
    }

  vtkPoints *points = path->GetPoints();
  for (vtkIdType i = 0; i < points->GetNumberOfPoints(); ++i)
    {
    double *point = points->GetPoint(i);
    point[0] += delta[0];
    point[1] += delta[1];
    points->SetPoint(i, point);
    }

  return true;
}

inline bool vtkFreeTypeTools::GetFace(vtkTextProperty *prop,
                                      unsigned long &prop_cache_id,
                                      FT_Face &face, bool &face_has_kerning)
{
  this->MapTextPropertyToId(prop, &prop_cache_id);
  if (!this->GetFace(prop_cache_id, &face))
    {
    vtkErrorMacro(<< "Failed retrieving the face");
    return false;
    }
  face_has_kerning = (FT_HAS_KERNING(face) != 0);
  return true;
}

inline FT_Bitmap* vtkFreeTypeTools::GetBitmap(FT_UInt32 c,
                                              unsigned long prop_cache_id,
                                              int prop_font_size,
                                              FT_UInt &gindex,
                                              FT_BitmapGlyph &bitmap_glyph)
{
  // Get the glyph index
  if (!this->GetGlyphIndex(prop_cache_id, c, &gindex))
    {
    return 0;
    }
  FT_Glyph glyph;
  // Get the glyph as a bitmap
  if (!this->GetGlyph(prop_cache_id,
                      prop_font_size,
                      gindex,
                      &glyph,
                      vtkFreeTypeTools::GLYPH_REQUEST_BITMAP) ||
                        glyph->format != ft_glyph_format_bitmap)
    {
    return 0;
    }

  bitmap_glyph = reinterpret_cast<FT_BitmapGlyph>(glyph);
  FT_Bitmap *bitmap = &bitmap_glyph->bitmap;

  if (bitmap->pixel_mode != ft_pixel_mode_grays)
    {
    return 0;
    }

  return bitmap;
}

inline FT_Outline *vtkFreeTypeTools::GetOutline(FT_UInt32 c,
                                                unsigned long prop_cache_id,
                                                int prop_font_size,
                                                FT_UInt &gindex,
                                                FT_OutlineGlyph &outline_glyph)
{
  // Get the glyph index
  if (!this->GetGlyphIndex(prop_cache_id, c, &gindex))
    {
    return 0;
    }
  FT_Glyph glyph;
  // Get the glyph as a outline
  if (!this->GetGlyph(prop_cache_id,
                      prop_font_size,
                      gindex,
                      &glyph,
                      vtkFreeTypeTools::GLYPH_REQUEST_OUTLINE) ||
                        glyph->format != ft_glyph_format_outline)
    {
    return 0;
    }

  outline_glyph = reinterpret_cast<FT_OutlineGlyph>(glyph);
  FT_Outline *outline= &outline_glyph->outline;

  return outline;
}
