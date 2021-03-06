/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkParse.h

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

/*
  This is the header file for vtkParse.tab.c, which is generated
  from vtkParse.y with the "yacc" compiler-compiler.
*/

#ifndef VTK_PARSE_H
#define VTK_PARSE_H

#include "vtkParseType.h"
#include "vtkParseData.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Add a hint about a class that will be parsed.  The only
 * acknowledged hint is "concrete", which if set to "true"
 * is a promise that the class is instantiable.
 */
void vtkParse_SetClassProperty(
  const char *classname, const char *property);

/**
 * Define a preprocessor macro. Function macros are not supported.
 */
void vtkParse_DefineMacro(const char *name, const char *definition);

/**
 * Undefine a preprocessor macro.
 */
void vtkParse_UndefineMacro(const char *name);

/**
 * Add an include directory, for use with the "-I" option.
 */
void vtkParse_IncludeDirectory(const char *dirname);

/**
 * Return the full path to a header file.
 */
const char *vtkParse_FindIncludeFile(const char *filename);

/**
 * Ignore BTX/ETX markers
 */
void vtkParse_SetIgnoreBTX(int option);

/**
 * Parse a header file and return a FileInfo struct
 */
FileInfo *vtkParse_ParseFile(
  const char *filename, FILE *ifile, FILE *errfile);

/**
 * Read a hints file and update the FileInfo
 */
int vtkParse_ReadHints(FileInfo *data, FILE *hfile, FILE *errfile);

/**
 * Free the FileInfo struct returned by vtkParse_ParseFile()
 */
void vtkParse_Free(FileInfo *data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
