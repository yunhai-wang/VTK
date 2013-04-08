vtk_module(vtkFiltersFlowPaths
  GROUPS
    StandAlone
  DEPENDS
    vtkCommonExecutionModel
    vtkFiltersGeneral
    vtkFiltersSources
    vtkIOCore
  TEST_DEPENDS
    vtkFiltersAMR
    vtkTestingCore
    vtkTestingRendering
    vtkRenderingOpenGL
  )
