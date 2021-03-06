//============================================================================
// Name        : Mesh.cpp
// Author      : Ben Grabham
// Description : OpenCL Mesh implementation
//============================================================================

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include <vtkCell.h>
#include <vtkCellData.h>
#include <vtkPoints.h>
#include <vtkPointData.h>
#include <vtkUnstructuredGrid.h>
#include <vtkXMLUnstructuredGridReader.h>

#include "Mesh.hpp"

Mesh::Mesh(const char *filename) : clMode(false) {
  // Check whether the provided file exists.
  std::ifstream ifile(filename);
  if(!ifile) {
    std::cerr << "File " << filename << " does not exist." << std::endl;
    exit(EXIT_FAILURE);
  }

  vtkXMLUnstructuredGridReader *reader = vtkXMLUnstructuredGridReader::New();
  reader->SetFileName(filename);
  reader->Update();

  vtkUnstructuredGrid *ug = reader->GetOutput();

  NNodes    = ug->GetNumberOfPoints();
  NElements = ug->GetNumberOfCells();

  NNList  = new std::vector<size_t>[NNodes];
  NEList  = new std::set   <size_t>[NNodes];
  normals = new double [2 * NNodes];
  ENList  = new cl_uint[3 * NElements];
  metric  = new double [3 * NNodes];
  coords  = new double [2 * NNodes];

  // Get the coordinates of each mesh vertex. There is no z coordinate in 2D,
  // but VTK treats 2D and 3D meshes uniformly, so we have to provide memory
  // for z as well (r[2] will always be zero and we ignore it).
  for(size_t i = 0; i < NNodes; ++i) {
    double r[3];
    ug->GetPoints()->GetPoint(i, r);
    coords[i * 2    ] = r[0];
    coords[i * 2 + 1] = r[1];
  }

  // Get the metric at each vertex.
  for(size_t i = 0; i < NNodes; ++i){
    double *tensor = ug->GetPointData()->GetArray("Metric")->GetTuple4(i);
    metric[i * 3] = tensor[0];
    metric[i * 3 + 1] = tensor[1];
    metric[i * 3 + 2] = tensor[3];
    assert(tensor[1] == tensor[2]);
  }

  // Get the 3 vertices comprising each element.
  for(size_t i = 0; i < NElements; ++i) {
    vtkCell *cell = ug->GetCell(i);
    for(int j = 0;j < 3; ++j){
      ENList[i * 3 + j] = cell->GetPointId(j);
    }
  }

  create_adjacency();
  find_surface();
  set_orientation();

  color_mesh(ug);

  reader->Delete();

  size_t NEListLen = 0;
  size_t NNListLen = 0;

  for(size_t i = 0; i < NNodes; ++i) {
    NEListLen += NEList[i].size();
    NNListLen += NNList[i].size();
  }

  cl_uint *NEListVals = new cl_uint[NEListLen];
  cl_uint *NNListVals = new cl_uint[NNListLen];

  cl_uint *NEListOffs = new cl_uint[NNodes + 1];
  cl_uint *NNListOffs = new cl_uint[NNodes + 1];

  NEListOffs[0] = 0;
  NNListOffs[0] = 0;

  size_t NECurrOff = 0;
  size_t NNCurrOff = 0;

  for(size_t i = 0; i < NNodes; ++i) {
    NEListOffs[i + 1] = NEListOffs[i] + NEList[i].size();
    NNListOffs[i + 1] = NNListOffs[i] + NNList[i].size();

    for(std::set<size_t>::const_iterator it = NEList[i].begin();
      it != NEList[i].end(); ++it)
      NEListVals[NECurrOff++] = *it;
    for(std::vector<size_t>::const_iterator it = NNList[i].begin();
      it != NNList[i].end(); ++it)
      NNListVals[NNCurrOff++] = *it;
  }

  wrapper = new CLWrapper();
  smooth_kernel = wrapper->compileKernel("Mesh.cl", "smooth");

  NEListOffsBuff = wrapper->uploadData(CL_MEM_READ_ONLY,   (NNodes + 1) * sizeof(cl_uint), NEListOffs);
  NNListOffsBuff = wrapper->uploadData(CL_MEM_READ_ONLY,   (NNodes + 1) * sizeof(cl_uint), NNListOffs);
  NEListBuff     = wrapper->uploadData(CL_MEM_READ_ONLY,   NEListLen    * sizeof(cl_uint), NEListVals);
  NNListBuff     = wrapper->uploadData(CL_MEM_READ_ONLY,   NNListLen    * sizeof(cl_uint), NNListVals);
  ENListBuff     = wrapper->uploadData(CL_MEM_READ_ONLY,  3 * NElements * sizeof(cl_uint), ENList);
  metricBuff     = wrapper->uploadData(CL_MEM_READ_ONLY,  3 * NNodes    * sizeof(double),  metric);
  coordsBuff     = wrapper->uploadData(CL_MEM_READ_WRITE, 2 * NNodes    * sizeof(double),  coords);
  normalsBuff    = wrapper->uploadData(CL_MEM_READ_ONLY,  2 * NNodes    * sizeof(double),  normals);
  colorBuff      = wrapper->uploadData(CL_MEM_READ_ONLY,      NNodes    * sizeof(cl_uint), colorVals);
}

Mesh::~Mesh() {
  delete[] NNList;
  delete[] NEList;
  delete[] normals;
  delete[] ENList;
  delete[] metric;
  delete[] coords;
  delete   wrapper;
}

void Mesh::create_adjacency() {
  for(size_t eid = 0; eid < NElements; ++eid) {
    // Get a pointer to the three vertices comprising element eid.
    const cl_uint *n = &ENList[3 * eid];

    // For each vertex, add the other two vertices to its node-node adjacency
    // list and element eid to its node-element adjacency list.
    for(size_t i = 0; i < 3; ++i) {
      NNList[n[i]].push_back(n[(i + 1) % 3]);
      NNList[n[i]].push_back(n[(i + 2) % 3]);

      NEList[n[i]].insert(eid);
    }
  }
}

void Mesh::find_surface(){
  memset(normals, 0, 2 * NNodes * sizeof(double));

  // If an edge is on the surface, then it belongs to only 1 element. We
  // traverse all edges (vid0,vid1) and for each edge we find the intersection
  // of NEList[vid0] and NEList[vid1]. If the intersection size is 1, then
  // this edge belongs to only one element, so it lies on the mesh surface.
  for(size_t vid = 0; vid < NNodes; ++vid){
    for(std::vector<size_t>::const_iterator it=NNList[vid].begin();
      it!=NNList[vid].end(); ++it){
      // In order to avoid processing an edge twice, one in the
      // form of (vid0,vid1) and one in the form of (vid1,vid0),
      // an edge is processed only of vid0 < vid1.
      if(vid > *it)
        continue;

      std::set<size_t> intersection;
      std::set_intersection(NEList[vid].begin(), NEList[vid].end(),
          NEList[*it].begin(), NEList[*it].end(),
          std::inserter(intersection, intersection.begin()));

      // If we have found a surface edge
      if(intersection.size() == 1) {
        double x = coords[2 * vid];
        double y = coords[2 * vid + 1];

        // Find which surface vid and *it belong to and set the corresponding
        // coordinate of the normal vector to ±1.0. The other coordinate is
        // intentionally left intact. This way, the normal vector for corner
        // vertices will be at the end (±1.0,±1.0), which enables us to detect
        // that they are corner vertices and are not allowed to be smoothed.
        if(fabs(y - 1.0) < 1E-12) {
          // vid is on the top surface
          normals[2 * vid + 1]   = 1.0;
          normals[2 * (*it) + 1] = 1.0;
        } else if(fabs(y) < 1E-12) {
          // vid is on the bottom surface
          normals[2 * vid + 1]   = -1.0;
          normals[2 * (*it) + 1] = -1.0;
        } else if(fabs(x - 1.0) < 1E-12) {
          // vid is on the right surface
          normals[2 * vid]   = 1.0;
          normals[2 * (*it)] = 1.0;
        } else if(fabs(x) < 1E-12) {
          // vid is on the left surface
          normals[2 * vid]   = -1.0;
          normals[2 * (*it)] = -1.0;
        } else {
          std::cerr << "Invalid surface vertex coordinates" << std::endl;
        }
      }
    }
  }
}

/* Computing the area of an element as the inner product of two element edges
 * depends on the order in which the three vertices comprising the element have
 * been stored in ENList. Using the right-hand rule for the cross product of
 * two 2D vectors, we can find whether the three vertices define a positive or
 * negative element. If the order in ENList suggests a clockwise traversal of
 * the element, the cross product faces the negative z-axis, so the element is
 * negative and we will have to correct the calculation of its area by
 * multiplying the result by -1.0. During smoothing, after a vertex is
 * relocated, if the area of an adjacent element is found to be negative it
 * means that the element has been inverted and the new location of the vertex
 * should be discarded. The orientation is the same for all mesh elements, so
 * it is enough to calculate it for one mesh element only.
 */
void Mesh::set_orientation() {
  // Find the orientation for the first element
  const cl_uint *n = &ENList[0];

  // Pointers to the coordinates of each vertex
  const double *c0 = &coords[2*n[0]];
  const double *c1 = &coords[2*n[1]];
  const double *c2 = &coords[2*n[2]];

  double x1 = c0[0] - c1[0];
  double y1 = c0[1] - c1[1];

  double x2 = c0[0] - c2[0];
  double y2 = c0[1] - c2[1];

  orientation = 2 * (x1 * y2 - x2 * y1 >= 0) - 1;
}

void Mesh::smooth(size_t niter) {
  const size_t local_size = 128;
  cl::NDRange local_ws    = local_size;

  size_t argnum = 0;
  smooth_kernel.setArg(argnum++, NEListBuff);
  smooth_kernel.setArg(argnum++, NNListBuff);
  smooth_kernel.setArg(argnum++, NEListOffsBuff);
  smooth_kernel.setArg(argnum++, NNListOffsBuff);
  smooth_kernel.setArg(argnum++, ENListBuff);
  smooth_kernel.setArg(argnum++, metricBuff);
  smooth_kernel.setArg(argnum++, coordsBuff);
  smooth_kernel.setArg(argnum++, normalsBuff);
  smooth_kernel.setArg(argnum++, colorBuff);
  smooth_kernel.setArg(argnum++, sizeof(cl_uint), &orientation);

  clMode = true;
  for(size_t iter = 0; iter < niter; ++iter) {
    const size_t arr[7] = {1, 0, 3, 2, 4, 5, 6};
    for(size_t i = 0; i < colorCount; ++i) {
      size_t global_size = (colorIdxs[arr[i]] + local_size - 1) & ~(local_size - 1);
      cl::NDRange global_ws   = global_size;

      smooth_kernel.setArg(argnum,     sizeof(cl_uint), &colorIdxs[arr[i]]);
      smooth_kernel.setArg(argnum + 1, sizeof(cl_uint), &colorOffs[arr[i]]);
      wrapper->run(smooth_kernel, global_ws, local_ws);
    }
  }
  wrapper->flush();
}

bool Mesh::isSurfaceNode(size_t vid) const {
  return NEList[vid].size() < NNList[vid].size();
}

bool Mesh::isCornerNode(size_t vid) const {
  return fabs(normals[2*vid])==1.0 && fabs(normals[2*vid+1])==1.0;
}

/* Element area in physical (Euclidean) space. Recall that the area of a
 * triangle ABC is calculated as area=0.5*(AB⋅AC), i.e. half the inner product
 * of two of the element's edges (e.g. AB and AC). The result is corrected by
 * the orientation factor ±1.0, so that the area is always a positive number.
 */
double Mesh::element_area(size_t eid) const{
  const cl_uint *n = &ENList[3*eid];

  // Pointers to the coordinates of each vertex
  const double *c0 = &coords[2*n[0]];
  const double *c1 = &coords[2*n[1]];
  const double *c2 = &coords[2*n[2]];

  return orientation * 0.5 *
            ( (c0[1] - c2[1]) * (c0[0] - c1[0]) -
              (c0[1] - c1[1]) * (c0[0] - c2[0]) );
}

/* This function evaluates the quality of an element, based on the 2D quality
 * functional proposed by Lipnikov et. al.. The description for the functional
 * is taken from: Yu. V. Vasileskii and K. N. Lipnikov, An Adaptive Algorithm
 * for Quasioptimal Mesh Generation, Computational Mathematics and Mathematical
 * Physics, Vol. 39, No. 9, 1999, pp. 1468 - 1486.
 */
double Mesh::element_quality(size_t eid) const {
  const cl_uint *n = &ENList[3*eid];

  // Pointers to the coordinates of each vertex
  const double *c0 = &coords[2*n[0]];
  const double *c1 = &coords[2*n[1]];
  const double *c2 = &coords[2*n[2]];

  // Pointers to the metric tensor at each vertex
  const double *m0 = &metric[3*n[0]];
  const double *m1 = &metric[3*n[1]];
  const double *m2 = &metric[3*n[2]];

  // Metric tensor averaged over the element
  double m00 = (m0[0] + m1[0] + m2[0])/3;
  double m01 = (m0[1] + m1[1] + m2[1])/3;
  double m11 = (m0[2] + m1[2] + m2[2])/3;

  // l is the length of the perimeter, measured in metric space
  double l =
    sqrt((c0[1] - c1[1])*((c0[1] - c1[1])*m11 + (c0[0] - c1[0])*m01) +
         (c0[0] - c1[0])*((c0[1] - c1[1])*m01 + (c0[0] - c1[0])*m00))+
    sqrt((c0[1] - c2[1])*((c0[1] - c2[1])*m11 + (c0[0] - c2[0])*m01) +
         (c0[0] - c2[0])*((c0[1] - c2[1])*m01 + (c0[0] - c2[0])*m00))+
    sqrt((c2[1] - c1[1])*((c2[1] - c1[1])*m11 + (c2[0] - c1[0])*m01) +
         (c2[0] - c1[0])*((c2[1] - c1[1])*m01 + (c2[0] - c1[0])*m00));

  // Area in physical space
  double a = element_area(eid);

  // Area in metric space
  double a_m = a*sqrt(m00*m11 - m01*m01);

  // Function
  double f = l >= 3.0 ? 3.0 / l : l / 3.0;
  double F = f * (2.0 - f);
  F = F * F * F;

  // This is the 2D Lipnikov functional.
  double quality = 12.0 * sqrt(3.0) * a_m * F / (l*l);

  return quality;
}

// Finds the mean quality, averaged over all mesh elements,
// and the quality of the worst element.
Quality Mesh::get_mesh_quality() {
  if(clMode) {
    wrapper->downloadData(coordsBuff, sizeof(double) * NNodes * 2, coords);
    clMode = false;
  }

  Quality q;

  double mean_q = 0.0;
  double min_q = 1.0;

  for(size_t i=0;i<NElements;i++){
    double ele_q = element_quality(i);

    mean_q += ele_q;
    min_q = std::min(min_q, ele_q);
  }

  q.mean = mean_q/NElements;
  q.min = min_q;

  return q;
}

void Mesh::color_mesh(vtkUnstructuredGrid* ug) {
  cl_uint *colors = new cl_uint[NNodes];
  cl_uint color;
  colorCount = 0;

  for(size_t vid = 0; vid<NNodes; ++vid) {
    colors[vid] = -1;
  }

  for(size_t vid = 0; vid < NNodes; ++vid) {
    color = 0;
    bool foundColor;
    do {
      foundColor = true;
      for(std::vector<size_t>::const_iterator it = NNList[vid].begin();
          it != NNList[vid].end(); ++it) {
        if(colors[*it] == color) {
          foundColor = false;
          break;
        }
      }
    } while(!foundColor && ++color);
    colorCount = colorCount >= color ? colorCount : color;
    colors[vid] = color;
  }

  ++colorCount;
  std::cout << "Amount of colours: " << colorCount << std::endl;

  for (size_t vid = 0; vid < NNodes; ++vid) {
    assert( colors[vid] != (cl_uint)-1 );
  }

  colorOffs = new cl_uint[colorCount + 1];
  colorIdxs = new cl_uint[colorCount]();
  colorVals = new cl_uint[NNodes];

  for(size_t vid = 0; vid<NNodes; ++vid)
    ++colorIdxs[colors[vid]];

  colorOffs[0] = 0;
  for(size_t i = 0; i < colorCount; ++i) {
    colorOffs[i + 1] = colorOffs[i] + colorIdxs[i];
    colorIdxs[i]     = 0;
  }

  for(size_t vid = 0; vid<NNodes; ++vid) {
    colorVals[colorOffs[colors[vid]] + colorIdxs[colors[vid]]++] = vid;
  }

  delete[] colors;
}
