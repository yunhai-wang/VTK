/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkNewickTreeReader.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkNewickTreeReader.h"

#include "vtkByteSwap.h"
#include "vtkCellArray.h"
#include "vtkDataSetAttributes.h"
#include "vtkDoubleArray.h"
#include "vtkFieldData.h"
#include "vtkNew.h"
#include "vtkTree.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkMutableDirectedGraph.h"
#include "vtkObjectFactory.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkStringArray.h"

#include <iostream>
#include <fstream>

vtkStandardNewMacro(vtkNewickTreeReader);

#ifdef read
#undef read
#endif

//----------------------------------------------------------------------------
vtkNewickTreeReader::vtkNewickTreeReader()
{
  vtkTree *output = vtkTree::New();
  this->SetOutput(output);
  // Releasing data for pipeline parallism.
  // Filters will know it is empty.
  output->ReleaseData();
  output->Delete();
}

//----------------------------------------------------------------------------
vtkNewickTreeReader::~vtkNewickTreeReader()
{
}

//----------------------------------------------------------------------------
vtkTree* vtkNewickTreeReader::GetOutput()
{
  return this->GetOutput(0);
}

//----------------------------------------------------------------------------
vtkTree* vtkNewickTreeReader::GetOutput(int idx)
{
  return vtkTree::SafeDownCast(this->GetOutputDataObject(idx));
}

//----------------------------------------------------------------------------
void vtkNewickTreeReader::SetOutput(vtkTree *output)
{
  this->GetExecutive()->SetOutputData(0, output);
}

//----------------------------------------------------------------------------
// I do not think this should be here, but I do not want to remove it now.
int vtkNewickTreeReader::RequestUpdateExtent(
  vtkInformation *,
  vtkInformationVector **,
  vtkInformationVector *outputVector)
{
  vtkInformation *outInfo = outputVector->GetInformationObject(0);
  int piece, numPieces;

  piece = outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_PIECE_NUMBER());
  numPieces = outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_NUMBER_OF_PIECES());

  // make sure piece is valid
  if (piece < 0 || piece >= numPieces)
    {
    return 1;
    }

  return 1;
}

//----------------------------------------------------------------------------
int vtkNewickTreeReader::RequestData(
  vtkInformation *,
  vtkInformationVector **,
  vtkInformationVector *outputVector)
{
  vtkInformation *outInfo = outputVector->GetInformationObject(0);

  // Return all data in the first piece ...
  if(outInfo->Get(vtkStreamingDemandDrivenPipeline::UPDATE_PIECE_NUMBER()) > 0)
    {
    return 1;
    }

  vtkDebugMacro(<<"Reading Newick tree ...");

  if(strcmp(this->GetFileName(), "") == 0)
    {
    vtkErrorMacro(<<"Input filename not set");
    return 1;
    }

  std::ifstream ifs( this->GetFileName(), std::ifstream::in );
  if(!ifs.good())
    {
    vtkErrorMacro(<<"Unable to open " << this->GetFileName() << " for reading");
    return 1;
    }

  vtkTree* const output = vtkTree::SafeDownCast(
    outInfo->Get(vtkDataObject::DATA_OBJECT()));

  vtkNew<vtkMutableDirectedGraph> builder;

  // Read the input file into a char *
  int length;
  ifs.seekg(0, std::ios::end);
  length = ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  char *buffer = new char[length];
  ifs.read(buffer, length);

  // Read through the input file to count the number of nodes in the tree.
  // We start at one to account for the root node
  vtkIdType numNodes = 1;
  this->CountNodes(buffer, &numNodes);

  // Rewind input buffer
  ifs.seekg(0, std::ios::beg);
  ifs.read(buffer, length);
  ifs.close();

  // Create the edge weight array
  vtkNew<vtkDoubleArray> weights;
  weights->SetNumberOfComponents(1);
  weights->SetName("weight");
  weights->SetNumberOfValues(numNodes);

  // Create the names array
  vtkNew<vtkStringArray> names;
  names->SetNumberOfComponents(1);
  names->SetName("node name");
  names->SetNumberOfValues(numNodes);

  //parse the input file to create the graph
  this->BuildTree(buffer, builder.GetPointer(), weights.GetPointer(),
    names.GetPointer(), -1);

  delete [] buffer;

  builder->GetEdgeData()->AddArray(weights.GetPointer());
  builder->GetVertexData()->AddArray(names.GetPointer());

  if (!output->CheckedShallowCopy(builder.GetPointer()))
    {
    vtkErrorMacro(<<"Edges do not create a valid tree.");
    return 1;
    }

  vtkNew<vtkDoubleArray> nodeWeights;
  nodeWeights->SetNumberOfTuples(output->GetNumberOfVertices());

  //set node weights
  double maxWeight = 0.0;
  for (vtkIdType vertex = 0; vertex < output->GetNumberOfVertices(); ++vertex)
    {
      double weight = 0.0;
      vtkIdType node = vertex;
      vtkIdType parent = output->GetParent(node);
      while (parent != -1)
        {
        weight += weights->GetValue(output->GetEdgeId(parent, node));
        node = parent;
        parent = output->GetParent(node);
        }

      if (weight > maxWeight)
        {
        maxWeight = weight;
        }
      nodeWeights->SetValue(vertex, weight);
    }
  for (vtkIdType vertex = 0; vertex < output->GetNumberOfVertices(); ++vertex)
    {
    if (output->IsLeaf(vertex))
      {
      nodeWeights->SetValue(vertex, maxWeight);
      }
    }
  nodeWeights->SetName("node weight");
  output->GetVertexData()->AddArray(nodeWeights.GetPointer());

  vtkDebugMacro(<< "Read " << output->GetNumberOfVertices() <<" vertices and "
                << output->GetNumberOfEdges() <<" edges.\n");

  return 1;
}

void vtkNewickTreeReader::CountNodes(char *buffer, vtkIdType *numNodes)
{
  char *current;
  char *start;
  char temp;
  int childCount;

  start = buffer;

  if (*start != '(')
  {
    // Leaf node. Separate name from weight.
    // If weight doesn't exist then take care of name only
    current = buffer;
    while (*current != '\0')
    {
      current++;
    }
    ++(*numNodes);
  }
  else
  {
    ++(*numNodes);
    // Search for all child nodes
    // Find all ',' until corresponding ')' is encountered
    childCount = 0;
    start++;
    current = start;
    while (childCount >= 0)
    {
      switch (*current)
      {
        case '(':
          // Find corresponding ')' by counting
          start = current;
          current++;
          childCount++;
          while (childCount > 0)
          {
            if (*current == '(')
            {
              childCount++;
            }
            else if (*current == ')')
            {
              childCount--;
            }
            current++;
          }
          while (*current != ',' && *current != ')')
          {
            current++;
          }

          temp = *current;
          *current = '\0';
          // Count child nodes using recursion
          this->CountNodes(start, numNodes);
          *current = temp;

          if (*current != ')')
          {
            current++;
          }
        break;

        case ')':
          // End of this tree. Go to next part to retrieve distance
          childCount--;
        break;

        case ',':
          // Impossible separation since according to the algorithm, this symbol will never encountered.
          // Currently don't handle this and don't create any node
        break;

        default:
          // leaf node encountered
          start = current;
          while (*current != ',' && *current != ')')
          {
            current++;
          }

          temp = *current;
          *current = '\0';
          // Count child nodes using recursion
          this->CountNodes(start, numNodes);
          *current = temp;

          if (*current != ')')
          {
            current++;
          }
        break;
      }
    }

    // If start at ':', then the internal node has no name.
    current++;
    if (*current == ':')
    {
      start = current + 1;
      while (*current != '\0' && *current != ';')
      {
        current++;
      }
    }
    else if (*current != ';' && *current != '\0')
    {
      while (*current != ':')
      {
        current++;
      }
      current++;
      while (*current != '\0' && *current != ';')
      {
        current++;
      }
    }
  }
}

vtkIdType vtkNewickTreeReader::BuildTree(char *buffer,
  vtkMutableDirectedGraph *g, vtkDoubleArray *weights, vtkStringArray *names,
  vtkIdType parent)
{
  char *current;
  char *start;
  char *colon = NULL;
  char temp;
  int childCount;
  vtkIdType node;

  start = buffer;

  if(parent == -1)
    {
    parent = g->AddVertex();
    names->SetValue(parent, "");
    }

  if (*start != '(')
  {
    // Leaf node. Separate name from weight (if it exists).
    current = buffer;
    while (*current != '\0')
    {
      if (*current == ':')
      {
        colon = current;
      }
      current++;
    }
    node = g->AddChild(parent);
    if (colon == NULL)
    {
      // Name only
      std::string name(start, strlen(start));
      names->SetValue(node, name);
    }
    else
    {
      // Name
      *colon = '\0';
      std::string name(start, strlen(start));
      names->SetValue(node, name);
      *colon = ':';
      // Weight
      colon++;
      weights->SetValue(g->GetEdgeId(parent, node), atof(colon));
    }
  }
  else
  {
    // Create node
    node = g->AddChild(parent);

    // Search for all child nodes
    // Find all ',' until corresponding ')' is encountered
    childCount = 0;
    start++;
    current = start;
    while (childCount >= 0)
    {
      switch (*current)
      {
        case '(':
          // Find corresponding ')' by counting
          start = current;
          current++;
          childCount++;
          while (childCount > 0)
          {
            if (*current == '(')
            {
              childCount++;
            }
            else if (*current == ')')
            {
              childCount--;
            }
            current++;
          }
          while (*current != ',' && *current != ')')
          {
            current++;
          }
          temp = *current;
          *current = '\0';
          // Create a child node using recursion
          this->BuildTree(start, g, weights, names, node);
          *current = temp;
          if (*current != ')')
          {
            current++;
          }
        break;

        case ')':
          // End of this tree. Go to next part to retrieve distance
          childCount--;
        break;

        case ',':
          // Impossible separation since according to the algorithm, this symbol will never encountered.
          // Currently don't handle this and don't create any node
        break;

        default:
          // leaf node encountered
          start = current;
          while (*current != ',' && *current != ')')
          {
            current++;
          }
          temp = *current;
          *current = '\0';
          // Create a child node using recursion
          this->BuildTree(start, g, weights, names, node);
          *current = temp;
          if (*current != ')')
          {
            current++;
          }
        break;
      }
    }

    // If start at ':', then the internal node has no name.
    current++;
    if (*current == ':')
    {
      start = current + 1;
      while (*current != '\0' && *current != ';')
      {
        current++;
      }
      temp = *current;
      *current = '\0';
      weights->SetValue(g->GetEdgeId(parent, node), atof(start));
      names->SetValue(node, "");
      *current = temp;
    }
    else if (*current != ';' && *current != '\0')
    {
      // Find ':' to retrieve distance, if any.
      // At this time *current should equal to ')'
      start = current;
      while (*current != ':')
      {
        current++;
      }
      temp = *current;
      *current = '\0';
      std::string name(start, strlen(start));
      names->SetValue(node, name);
      *current = temp;
      current++;
      start = current;
      while (*current != '\0' && *current != ';')
      {
        current++;
      }
      temp = *current;
      *current = '\0';
      weights->SetValue(g->GetEdgeId(parent, node), atof(start));
      *current = temp;
    }
  }

  return node;
}

//----------------------------------------------------------------------------
int vtkNewickTreeReader::FillOutputPortInformation(int, vtkInformation* info)
{
  info->Set(vtkDataObject::DATA_TYPE_NAME(), "vtkTree");
  return 1;
}

//----------------------------------------------------------------------------
void vtkNewickTreeReader::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);
}
