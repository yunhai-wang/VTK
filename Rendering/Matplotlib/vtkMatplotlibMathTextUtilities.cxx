/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkMatplotlibMathTextUtilities.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkMatplotlibMathTextUtilities.h"

// Prevent redefined symbol warnings in vtkPython.h:
#ifdef _POSIX_C_SOURCE
#  undef _POSIX_C_SOURCE
#endif // _POSIX_C_SOURCE
#ifdef _XOPEN_SOURCE
#  undef _XOPEN_SOURCE
#endif // _XOPEN_SOURCE
#include "vtkPython.h"

#include "vtkImageData.h"
#include "vtkImageReslice.h"
#include "vtkMath.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkPath.h"
#include "vtkPoints.h"
#include "vtkTextProperty.h"
#include "vtkTransform.h"

#include <vector>

// Smart pointer for PyObjects. Calls Py_XDECREF when scope ends.
class SmartPyObject
{
  PyObject *Object;

public:
  SmartPyObject(PyObject *obj = NULL)
    : Object(obj)
  {
  }

  ~SmartPyObject()
  {
    Py_XDECREF(this->Object);
  }

  PyObject *operator->() const
  {
    return this->Object;
  }

  PyObject *GetPointer() const
  {
    return this->Object;
  }
};

//----------------------------------------------------------------------------
vtkObjectFactoryNewMacro(vtkMatplotlibMathTextUtilities)

//----------------------------------------------------------------------------
vtkMatplotlibMathTextUtilities::vtkMatplotlibMathTextUtilities()
  : Superclass(), PythonIsInitialized(false), MaskParser(NULL),
    PathParser(NULL), FontPropertiesClass(NULL)
{
}

//----------------------------------------------------------------------------
vtkMatplotlibMathTextUtilities::~vtkMatplotlibMathTextUtilities()
{
  Py_XDECREF(this->MaskParser);
  Py_XDECREF(this->PathParser);
  Py_XDECREF(this->FontPropertiesClass);
  if (this->PythonIsInitialized)
    {
    Py_Finalize();
    }
}

//----------------------------------------------------------------------------
bool vtkMatplotlibMathTextUtilities::InitializePython()
{
  if (this->PythonIsInitialized)
    {
    return true;
    }
  Py_Initialize();
  this->PythonIsInitialized = !this->CheckForError();
  return this->PythonIsInitialized;
}

//----------------------------------------------------------------------------
bool vtkMatplotlibMathTextUtilities::InitializeMaskParser()
{
  if (!this->InitializePython())
    {
    return false;
    }

  SmartPyObject mplMathTextLib(PyImport_ImportModule("matplotlib.mathtext"));
  if (this->CheckForError(mplMathTextLib.GetPointer()))
    {
    return false;
    }

  SmartPyObject mathTextParserClass(
        PyObject_GetAttrString(mplMathTextLib.GetPointer(), "MathTextParser"));
  if (this->CheckForError(mathTextParserClass.GetPointer()))
    {
    return false;
    }

  this->MaskParser =
      PyObject_CallFunction(mathTextParserClass.GetPointer(),
                            const_cast<char*>("s"), "bitmap");
  if (this->CheckForError(this->MaskParser))
    {
    Py_CLEAR(this->MaskParser);
    return false;
    }

  return true;
}

//----------------------------------------------------------------------------
bool vtkMatplotlibMathTextUtilities::InitializePathParser()
{
  if (!this->InitializePython())
    {
    return false;
    }

  SmartPyObject mplTextPathLib(PyImport_ImportModule("matplotlib.textpath"));
  if (this->CheckForError(mplTextPathLib.GetPointer()))
    {
    return false;
    }

  SmartPyObject textToPathClass(
        PyObject_GetAttrString(mplTextPathLib.GetPointer(), "TextToPath"));
  if (this->CheckForError(textToPathClass.GetPointer()))
    {
    return false;
    }

  this->PathParser = PyObject_CallFunction(textToPathClass.GetPointer(), NULL);
  if (this->CheckForError(this->PathParser))
    {
    Py_CLEAR(this->PathParser);
    return false;
    }

  return true;
}

//----------------------------------------------------------------------------
bool vtkMatplotlibMathTextUtilities::InitializeFontPropertiesClass()
{
  if (!this->InitializePython())
    {
    return false;
    }

  SmartPyObject mplFontManagerLib(
        PyImport_ImportModule("matplotlib.font_manager"));
  if (this->CheckForError(mplFontManagerLib.GetPointer()))
    {
    return false;
    }

  this->FontPropertiesClass = PyObject_GetAttrString(
        mplFontManagerLib.GetPointer(), "FontProperties");
  if (this->CheckForError(this->FontPropertiesClass))
    {
    Py_CLEAR(this->FontPropertiesClass);
    return false;
    }

  return true;
}

//----------------------------------------------------------------------------
bool vtkMatplotlibMathTextUtilities::CheckForError()
{
  PyObject *exception = PyErr_Occurred();
  if (exception)
    {
    vtkDebugMacro(<< "Python exception raised.");
    PyErr_PrintEx(0);
    return true;
    }
  return false;
}

//----------------------------------------------------------------------------
bool vtkMatplotlibMathTextUtilities::CheckForError(PyObject *object)
{
  // Print any exceptions
  bool result = this->CheckForError();

  if (object == NULL)
    {
    vtkErrorMacro(<< "Object is NULL!");
    return true;
    }
  return result;
}

//----------------------------------------------------------------------------
PyObject *
vtkMatplotlibMathTextUtilities::GetFontProperties(vtkTextProperty *tprop)
{
  if (!this->FontPropertiesClass)
    {
    if (!this->InitializeFontPropertiesClass())
      {
      vtkErrorMacro(<<"FontPropertiesClass is not initialized!");
      return NULL;
      }
    }

  char tpropFamily[16];
  char tpropStyle[16];
  char tpropVariant[16] = "normal";
  char tpropWeight[16];
  long tpropFontSize;

  switch (tprop->GetFontFamily())
    {
    default:
    case VTK_ARIAL:
      strcpy(tpropFamily, "sans-serif");
      break;
    case VTK_COURIER:
      strcpy(tpropFamily, "monospace");
      break;
    case VTK_TIMES:
      strcpy(tpropFamily, "serif");
      break;
    }

  if (tprop->GetItalic())
    {
    strcpy(tpropStyle, "italic");
    }
  else
    {
    strcpy(tpropStyle, "normal");
    }

  if (tprop->GetBold())
    {
    strcpy(tpropWeight, "bold");
    }
  else
    {
    strcpy(tpropWeight, "normal");
    }

  tpropFontSize = tprop->GetFontSize();

  return PyObject_CallFunction(this->FontPropertiesClass,
                               const_cast<char*>("sssssi"), tpropFamily,
                               tpropStyle, tpropVariant, tpropStyle,
                               tpropWeight, tpropFontSize);
}

//----------------------------------------------------------------------------
void vtkMatplotlibMathTextUtilities::RotateCorners(double angleDeg,
                                                   double corners[4][2],
                                                   double bbox[4])
{
  double angleRad = vtkMath::RadiansFromDegrees(angleDeg);
  double c = cos(angleRad);
  double s = sin(angleRad);
  // Rotate corners
  for (int i = 0; i < 4; ++i)
    {
    int newpt[2];
    newpt[0] = c * corners[i][0] - s * corners[i][1];
    newpt[1] = s * corners[i][0] + c * corners[i][1];
    corners[i][0] = newpt[0];
    corners[i][1] = newpt[1];
    }
  // Find new bounds
  bbox[0] = VTK_INT_MAX;
  bbox[1] = VTK_INT_MIN;
  bbox[2] = VTK_INT_MAX;
  bbox[3] = VTK_INT_MIN;
  for (int i = 0; i < 4; ++i)
    {
    if (corners[i][0] < bbox[0])
      {
      bbox[0] = corners[i][0];
      }
    if (corners[i][0] > bbox[1])
      {
      bbox[1] = corners[i][0];
      }
    if (corners[i][1] < bbox[2])
      {
      bbox[2] = corners[i][1];
      }
    if (corners[i][1] > bbox[3])
      {
      bbox[3] = corners[i][1];
      }
    }
}

//----------------------------------------------------------------------------
bool vtkMatplotlibMathTextUtilities::GetBoundingBox(
    vtkTextProperty *tprop, const char *str, unsigned int dpi, int bbox[4])
{
  if (!this->MaskParser)
    {
    if (!this->InitializeMaskParser())
      {
      vtkErrorMacro(<<"MaskParser is not initialized!");
      return false;
      }
    }

  vtkDebugMacro(<<"Calculation bbox for '" << str << "'");

  long int rows = 0;
  long int cols = 0;
  // matplotlib.mathtext seems to mishandle the dpi, this conversion makes the
  // text size match the images produced by vtkFreeTypeUtilities, as well as the
  // paths generated by StringToPath
  long int fontSize = tprop->GetFontSize() * 72.0 / static_cast<float>(dpi);

  SmartPyObject resultTuple(PyObject_CallMethod(this->MaskParser,
                                                const_cast<char*>("to_mask"),
                                                const_cast<char*>("sii"),
                                                const_cast<char*>(str),
                                                fontSize, dpi));
  if (this->CheckForError(resultTuple.GetPointer()))
    {
    return false;
    }

  // numpyArray is a borrowed reference, no smart wrapper needed:
  PyObject *numpyArray = PyTuple_GetItem(resultTuple.GetPointer(), 0);
  if (this->CheckForError(numpyArray))
    {
    return false;
    }

  SmartPyObject dimTuple(PyObject_GetAttrString(numpyArray,
                                                const_cast<char*>("shape")));
  if (this->CheckForError(dimTuple.GetPointer()))
    {
    return false;
    }

  PyArg_ParseTuple(dimTuple.GetPointer(), "ii", &rows, &cols);
  if (this->CheckForError())
    {
    return false;
    }


  // Determine the dimensions of the rotated image
  double angleDeg = tprop->GetOrientation();
  // Corners of original image
  double corners[4][2] = { {0, 0},
                           {cols, 0},
                           {0, rows},
                           {cols, rows} };

  double bboxd[4];
  this->RotateCorners(angleDeg, corners, bboxd);
  bbox[0] = vtkMath::Round(bboxd[0]);
  bbox[1] = vtkMath::Round(bboxd[1]);
  bbox[2] = vtkMath::Round(bboxd[2]);
  bbox[3] = vtkMath::Round(bboxd[3]);

  return true;
}

//----------------------------------------------------------------------------
bool vtkMatplotlibMathTextUtilities::RenderString(const char *str,
                                                  vtkImageData *image,
                                                  vtkTextProperty *tprop,
                                                  unsigned int dpi)
{
  if (!this->MaskParser)
    {
    if (!this->InitializeMaskParser())
      {
      vtkErrorMacro(<<"MaskParser is not initialized!");
      return false;
      }
    }

  vtkDebugMacro(<<"Converting '" << str << "' into MathText image...");

  long int rows = 0;
  long int cols = 0;
  long int ind = 0;
  //long int numPixels = 0;
  // matplotlib.mathtext seems to mishandle the dpi, this conversion makes the
  // text size match the images produced by vtkFreeTypeUtilities, as well as the
  // paths generated by StringToPath
  long int fontSize = tprop->GetFontSize() * 72.0 / static_cast<float>(dpi);
  double *dcolor = tprop->GetColor();
  unsigned char r = static_cast<unsigned char>(dcolor[0] * 255);
  unsigned char g = static_cast<unsigned char>(dcolor[1] * 255);
  unsigned char b = static_cast<unsigned char>(dcolor[2] * 255);
  double alpha = tprop->GetOpacity();

  SmartPyObject resultTuple(PyObject_CallMethod(this->MaskParser,
                                                const_cast<char*>("to_mask"),
                                                const_cast<char*>("sii"),
                                                const_cast<char*>(str),
                                                fontSize, dpi));
  if (this->CheckForError(resultTuple.GetPointer()))
    {
    return false;
    }

  // numpyArray is a borrowed reference, no smart wrapper needed:
  PyObject *numpyArray = PyTuple_GetItem(resultTuple.GetPointer(), 0);
  if (this->CheckForError(numpyArray))
    {
    return false;
    }

  SmartPyObject flatArray(PyObject_CallMethod(numpyArray,
                                              const_cast<char*>("flatten"),
                                              const_cast<char*>("")));
  if (this->CheckForError(flatArray.GetPointer()))
    {
    return false;
    }

  SmartPyObject list(PyObject_CallMethod(flatArray.GetPointer(),
                                         const_cast<char*>("tolist"),
                                         const_cast<char*>("")));
  if (this->CheckForError(list.GetPointer()))
    {
    return false;
    }

  SmartPyObject dimTuple(PyObject_GetAttrString(numpyArray,
                                                const_cast<char*>("shape")));
  if (this->CheckForError(dimTuple.GetPointer()))
    {
    return false;
    }

  PyArg_ParseTuple(dimTuple.GetPointer(), "ii", &rows, &cols);
  if (this->CheckForError())
    {
    return false;
    }

  //numPixels = PyObject_Length(list.GetPointer());
  if (this->CheckForError())
    {
    return false;
    }

  image->SetDimensions(cols, rows, 1);
  image->AllocateScalars(VTK_UNSIGNED_CHAR, 4);
  for (long int row = rows-1; row >= 0; --row)
    {
    for (long int col = 0; col < cols; ++col)
      {
      // item is borrowed, no need for a smart wrapper
      PyObject *item = PyList_GetItem(list.GetPointer(), ind++);
      if (this->CheckForError(item))
        {
        return false;
        }
      unsigned char val =
          static_cast<unsigned char>(alpha * PyInt_AsLong(item));
      if (this->CheckForError())
        {
        return false;
        }
      unsigned char *ptr =
          static_cast<unsigned char*>(image->GetScalarPointer(col, row, 0));
      ptr[0] = r;
      ptr[1] = g;
      ptr[2] = b;
      ptr[3] = val;
      }
    }

  // Determine the dimensions of the rotated image
  double angleDeg = tprop->GetOrientation();
  // Save some time if no rotation needed
  if (fabs(angleDeg) < 0.01)
    {
    return true;
    }

  double *bounds = image->GetBounds();
  // Corners of original image
  double corners[4][2] = { {bounds[0], bounds[2]},
                           {bounds[1], bounds[2]},
                           {bounds[0], bounds[3]},
                           {bounds[1], bounds[3]} };
  double bbox[4];

  // Rotate the corners of the image and determine the bounding box
  this->RotateCorners(angleDeg, corners, bbox);

  // Rotate the temporary image into the returned image:
  vtkNew<vtkTransform> rotation;
  rotation->RotateWXYZ(-angleDeg, 0, 0, 1);
  // Dummy image with the output dimensions
  vtkNew<vtkImageData> dummyImage;
  dummyImage->SetExtent(bbox[0], bbox[1], bbox[2], bbox[3], 0, 0);
  vtkNew<vtkImageReslice> rotator;
  rotator->SetInputData(image);
  rotator->SetInformationInput(dummyImage.GetPointer());
  rotator->SetResliceTransform(rotation.GetPointer());
  rotator->SetInterpolationModeToLinear();
  rotator->Update();
  image->ShallowCopy(rotator->GetOutput());

  return true;
}

//----------------------------------------------------------------------------
bool vtkMatplotlibMathTextUtilities::StringToPath(const char *str,
                                                  vtkPath *path,
                                                  vtkTextProperty *tprop)
{
  if (!this->PathParser)
    {
    if (!this->InitializePathParser())
      {
      vtkErrorMacro(<<"PathParser is not initialized!");
      return false;
      }
    }

  vtkDebugMacro(<<"Converting '" << str << "' into a vtkPath...");

  // Matplotlib path codes:
  const int pathStop = 0;
  const int pathMoveTo = 1;
  const int pathLineTo = 2;
  const int pathCurve3 = 3;
  const int pathCurve4 = 4;
  const int pathClosePoly = 0x4f;

  // List sizes:
  Py_ssize_t numCodes;
  Py_ssize_t numVerts;

  // Temp vars:
  float origin[2];
  float vert[2];
  float delta[2] = {0.0, 0.0};
  int code;
  bool hasOrigin;

  // Bounding box for all control points, used for justification
  float cbox[4] = {VTK_FLOAT_MAX, VTK_FLOAT_MAX, VTK_FLOAT_MIN, VTK_FLOAT_MIN};

  // The path is always generated at a font size of 100. Use this factor to
  // recover the font.
  const float fontScale = ((tprop->GetFontSize()) / 100.);
  path->Reset();

  // Create the font property
  SmartPyObject pyFontProp(this->GetFontProperties(tprop));
  if (this->CheckForError(pyFontProp.GetPointer()))
    {
    return false;
    }

  SmartPyObject pyResultTuple(
        PyObject_CallMethod(this->PathParser,
                            const_cast<char*>("get_text_path"),
                            const_cast<char*>("Osi"),
                            pyFontProp.GetPointer(),// prop
                            const_cast<char*>(str), // texstring
                            1,                      // boolean, ismath
                            0));                    // boolean, usetex
  if (this->CheckForError(pyResultTuple.GetPointer()))
    {
    return false;
    }

  // pyVerts and pyCodes are borrowed references -- no need for smart wrappers
  PyObject *pyVerts = PyTuple_GetItem(pyResultTuple.GetPointer(), 0);
  PyObject *pyCodes = PyTuple_GetItem(pyResultTuple.GetPointer(), 1);
  if (this->CheckForError(pyVerts)  ||
      this->CheckForError(pyCodes))
    {
    return false;
    }

  // Both verts and codes are lists?
  if (!PySequence_Check(pyVerts) || !PySequence_Check(pyCodes))
    {
    return false;
    }

  numVerts = PySequence_Size(pyVerts);
  numCodes = PySequence_Size(pyCodes);
  if (numVerts != numCodes)
    {
    return false;
    }

  path->Allocate(numVerts);

  for (Py_ssize_t i = 0; i < numVerts; ++i)
    {
    SmartPyObject pyVert(PySequence_GetItem(pyVerts, i));
    SmartPyObject pyCode(PySequence_GetItem(pyCodes, i));
    if (this->CheckForError(pyVert.GetPointer()) ||
        this->CheckForError(pyCode.GetPointer()))
      {
      return false;
      }

    // pyVert is sometimes a numpy array, sometimes it's a tuple.
    // Initialize the following objects in the following conditional, then
    // convert to smart pointers afterwards.
    PyObject *pyVertXObj = NULL;
    PyObject *pyVertYObj = NULL;
    if (pyVert->ob_type == &PyTuple_Type)
      {
      pyVertXObj = PyTuple_GetItem(pyVert.GetPointer(), 0);
      pyVertYObj = PyTuple_GetItem(pyVert.GetPointer(), 1);
      // Increase reference count -- the other branch returns a new reference,
      // this keeps cleanup consistent
      if (pyVertXObj)
        {
        Py_INCREF(pyVertXObj);
        }
      if (pyVertYObj)
        {
        Py_INCREF(pyVertYObj);
        }
      }
    else // Assume numpy array. Convert to list and extract elements.
      {
      SmartPyObject pyVertList(PyObject_CallMethod(pyVert.GetPointer(),
                                                   const_cast<char*>("tolist"),
                                                   NULL));
      if (this->CheckForError(pyVertList.GetPointer()) ||
          PySequence_Size(pyVertList.GetPointer()) < 2)
        {
        return false;
        }

      pyVertXObj = PySequence_GetItem(pyVertList.GetPointer(), 0);
      pyVertYObj = PySequence_GetItem(pyVertList.GetPointer(), 1);
      }

    SmartPyObject pyVertX(pyVertXObj);
    SmartPyObject pyVertY(pyVertYObj);
    if (this->CheckForError(pyVertX.GetPointer()) ||
        this->CheckForError(pyVertY.GetPointer()))
      {
      return false;
      }

    vert[0] = PyFloat_AsDouble(pyVertX.GetPointer()) * fontScale;
    vert[1] = PyFloat_AsDouble(pyVertY.GetPointer()) * fontScale;
    if (this->CheckForError())
      {
      return false;
      }

    if (vert[0] < cbox[0])
      {
      cbox[0] = vert[0];
      }
    if (vert[1] < cbox[1])
      {
      cbox[1] = vert[1];
      }
    if (vert[0] > cbox[2])
      {
      cbox[2] = vert[0];
      }
    if (vert[1] > cbox[3])
      {
      cbox[3] = vert[1];
      }

    code = PyInt_AsLong(pyCode.GetPointer());
    if (this->CheckForError())
      {
      return false;
      }

    switch (code)
      {
      case pathStop:
        hasOrigin = false;
        break;
      case pathMoveTo:
        path->InsertNextPoint(vert[0], vert[1], 0, vtkPath::MOVE_TO);
        origin[0] = vert[0];
        origin[1] = vert[1];
        hasOrigin = true;
        break;
      case pathLineTo:
        path->InsertNextPoint(vert[0], vert[1], 0, vtkPath::LINE_TO);
        break;
      case pathCurve3:
        path->InsertNextPoint(vert[0], vert[1], 0, vtkPath::CONIC_CURVE);
        break;
      case pathCurve4:
        path->InsertNextPoint(vert[0], vert[1], 0, vtkPath::CUBIC_CURVE);
        break;
      case pathClosePoly:
        if (hasOrigin)
          path->InsertNextPoint(origin[0], origin[1], 0, vtkPath::LINE_TO);
        hasOrigin = false;
        break;
      default:
        vtkWarningMacro(<<"Unrecognized code: " << code);
        break;
      }
    }

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

//----------------------------------------------------------------------------
void vtkMatplotlibMathTextUtilities::PrintSelf(ostream &os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  os << indent << "PythonIsInitialized: " << this->PythonIsInitialized << endl;
  os << indent << "MaskParser: " << this->MaskParser << endl;
  os << indent << "PathParser: " << this->PathParser << endl;
  os << indent << "FontPropertiesClass: " << this->FontPropertiesClass << endl;
}
