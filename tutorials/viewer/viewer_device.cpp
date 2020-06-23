// Copyright 2009-2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "viewer_device.h"

namespace embree {

RTCScene g_scene = nullptr;
extern "C" bool g_changed;
TutorialData data;

#define SPP 1

#define FIXED_EDGE_TESSELLATION_VALUE 3

#define MAX_EDGE_LEVEL 64.0f
#define MIN_EDGE_LEVEL  4.0f
#define LEVEL_FACTOR   64.0f

size_t cancel_at_invokation = -1;
std::atomic<size_t> invokations(0);
  
bool monitorProgressFunction(void* ptr, double dn) 
{
  size_t invokation = invokations++;
  if (invokation == cancel_at_invokation) {
    PRINT2("cancelling build at", invokation);
    return false;
  }
  return true;
}

inline float updateEdgeLevel( ISPCSubdivMesh* mesh, const Vec3fa& cam_pos, const unsigned int e0, const unsigned int e1)
{
  const Vec3fa v0 = mesh->positions[0][mesh->position_indices[e0]];
  const Vec3fa v1 = mesh->positions[0][mesh->position_indices[e1]];
  const Vec3fa edge = v1-v0;
  const Vec3fa P = 0.5f*(v1+v0);
  const Vec3fa dist = cam_pos - P;
  return max(min(LEVEL_FACTOR*(0.5f*length(edge)/length(dist)),MAX_EDGE_LEVEL),MIN_EDGE_LEVEL);
}


void updateEdgeLevelBuffer( ISPCSubdivMesh* mesh, const Vec3fa& cam_pos, unsigned int startID, unsigned int endID )
{
  for (unsigned int f=startID; f<endID;f++) {
    unsigned int e = mesh->face_offsets[f];
    unsigned int N = mesh->verticesPerFace[f];
    if (N == 4) /* fast path for quads */
      for (unsigned int i=0; i<4; i++)
        mesh->subdivlevel[e+i] =  updateEdgeLevel(mesh,cam_pos,e+(i+0),e+(i+1)%4);
    else if (N == 3) /* fast path for triangles */
      for (unsigned int i=0; i<3; i++)
        mesh->subdivlevel[e+i] =  updateEdgeLevel(mesh,cam_pos,e+(i+0),e+(i+1)%3);
    else /* fast path for general polygons */
      for (unsigned int i=0; i<N; i++)
        mesh->subdivlevel[e+i] =  updateEdgeLevel(mesh,cam_pos,e+(i+0),e+(i+1)%N);
  }
}

#if defined(ISPC)
void updateSubMeshEdgeLevelBufferTask (int taskIndex, int threadIndex,  ISPCSubdivMesh* mesh, const Vec3fa& cam_pos )
{
  const unsigned int size = mesh->numFaces;
  const unsigned int startID = ((taskIndex+0)*size)/taskCount;
  const unsigned int endID   = ((taskIndex+1)*size)/taskCount;
  updateEdgeLevelBuffer(mesh,cam_pos,startID,endID);
}
void updateMeshEdgeLevelBufferTask (int taskIndex, int threadIndex,  ISPCScene* scene_in, const Vec3fa& cam_pos )
{
  ISPCGeometry* geometry = g_ispc_scene->geometries[taskIndex];
  if (geometry->type != SUBDIV_MESH) return;
  ISPCSubdivMesh* mesh = (ISPCSubdivMesh*) geometry;
  unsigned int geomID = mesh->geom.geomID;
  if (mesh->numFaces < 10000) {
    updateEdgeLevelBuffer(mesh,cam_pos,0,mesh->numFaces);
    rtcUpdateGeometryBuffer(geometry->geometry, RTC_BUFFER_TYPE_LEVEL, 0);
  }
  rtcCommitGeometry(geometry->geometry);
}
#endif

void updateEdgeLevels(ISPCScene* scene_in, const Vec3fa& cam_pos)
{
  /* first update small meshes */
#if defined(ISPC)
  parallel_for(size_t(0),size_t( scene_in->numGeometries ),[&](const range<size_t>& range) {
    const int threadIndex = (int)TaskScheduler::threadIndex();
    for (size_t i=range.begin(); i<range.end(); i++)
      updateMeshEdgeLevelBufferTask((int)i,threadIndex,scene_in,cam_pos);
  }); 
#endif

  /* now update large meshes */
  for (unsigned int g=0; g<scene_in->numGeometries; g++)
  {
    ISPCGeometry* geometry = g_ispc_scene->geometries[g];
    if (geometry->type != SUBDIV_MESH) continue;
    ISPCSubdivMesh* mesh = (ISPCSubdivMesh*) geometry;
#if defined(ISPC)
    if (mesh->numFaces < 10000) continue;
    parallel_for(size_t(0),size_t( (mesh->numFaces+4095)/4096 ),[&](const range<size_t>& range) {
    const int threadIndex = (int)TaskScheduler::threadIndex();
    for (size_t i=range.begin(); i<range.end(); i++)
      updateSubMeshEdgeLevelBufferTask((int)i,threadIndex,mesh,cam_pos);
  }); 
#else
    updateEdgeLevelBuffer(mesh,cam_pos,0,mesh->numFaces);
#endif
    rtcUpdateGeometryBuffer(geometry->geometry, RTC_BUFFER_TYPE_LEVEL, 0);
    rtcCommitGeometry(geometry->geometry);
  }
}

#if 0
bool g_use_smooth_normals = false;
void device_key_pressed_handler(int key)
{
  if (key == 110 /*n*/) g_use_smooth_normals = !g_use_smooth_normals;
  else device_key_pressed_default(key);
}
#endif

RTCScene convertScene(ISPCScene* scene_in)
{
  for (unsigned int i=0; i<scene_in->numGeometries; i++)
  {
    ISPCGeometry* geometry = scene_in->geometries[i];
    if (geometry->type == SUBDIV_MESH) {
      data.subdiv_mode = true; break;
    }
  }

  RTCScene scene_out = ConvertScene(g_device, g_ispc_scene, RTC_BUILD_QUALITY_MEDIUM);
  rtcSetSceneProgressMonitorFunction(scene_out,monitorProgressFunction,nullptr);

  /* commit individual objects in case of instancing */
  if (g_instancing_mode != ISPC_INSTANCING_NONE)
  {
    for (unsigned int i=0; i<scene_in->numGeometries; i++) {
      ISPCGeometry* geometry = g_ispc_scene->geometries[i];
      if (geometry->type == GROUP)  rtcSetSceneProgressMonitorFunction(geometry->scene,monitorProgressFunction,nullptr);  //rtcCommitScene(geometry->scene);
    }
  }

  /* commit changes to scene */
  return scene_out;
}


void postIntersectGeometry(const Ray& ray, DifferentialGeometry& dg, ISPCGeometry* geometry, int& materialID)
{
  if (geometry->type == TRIANGLE_MESH)
  {
    ISPCTriangleMesh* mesh = (ISPCTriangleMesh*) geometry;
    materialID = mesh->geom.materialID;
  }
  else if (geometry->type == QUAD_MESH)
  {
    ISPCQuadMesh* mesh = (ISPCQuadMesh*) geometry;
    materialID = mesh->geom.materialID;
  }
  else if (geometry->type == SUBDIV_MESH)
  {
    ISPCSubdivMesh* mesh = (ISPCSubdivMesh*) geometry;
    materialID = mesh->geom.materialID;
  }
  else if (geometry->type == CURVES)
  {
    ISPCHairSet* mesh = (ISPCHairSet*) geometry;
    materialID = mesh->geom.materialID;
  }
  else if (geometry->type == GRID_MESH)
  {
    ISPCGridMesh* mesh = (ISPCGridMesh*) geometry;
    materialID = mesh->geom.materialID;
  }
  else if (geometry->type == POINTS)
  {
    ISPCPointSet* set = (ISPCPointSet*) geometry;
    materialID = set->geom.materialID;
  }
  else if (geometry->type == GROUP) {
    unsigned int geomID = ray.geomID; {
      postIntersectGeometry(ray,dg,((ISPCGroup*) geometry)->geometries[geomID],materialID);
    }
  }
  else
    assert(false);
}

AffineSpace3fa calculate_interpolated_space (ISPCInstance* instance, float gtime)
{
  if (instance->numTimeSteps == 1)
    return AffineSpace3fa(instance->spaces[0]);

   /* calculate time segment itime and fractional time ftime */
  const int time_segments = instance->numTimeSteps-1;
  const float time = gtime*(float)(time_segments);
  const int itime = clamp((int)(floor(time)),(int)0,time_segments-1);
  const float ftime = time - (float)(itime);
  return (1.0f-ftime)*AffineSpace3fa(instance->spaces[itime+0]) + ftime*AffineSpace3fa(instance->spaces[itime+1]);
}

typedef ISPCInstance* ISPCInstancePtr;

inline int postIntersect(const TutorialData& data, const Ray& ray, DifferentialGeometry& dg)
{
  int materialID = 0;
  unsigned int instID = ray.instID[0]; {
    unsigned int geomID = ray.geomID; {
      ISPCGeometry* geometry = nullptr;
      if (data.instancing_mode != ISPC_INSTANCING_NONE) {
        ISPCInstance* instance = (ISPCInstancePtr) data.ispc_scene->geometries[instID];
        geometry = instance->child;
      } else {
        geometry = data.ispc_scene->geometries[geomID];
      }
      postIntersectGeometry(ray,dg,geometry,materialID);
    }
  }

  if (data.instancing_mode != ISPC_INSTANCING_NONE)
  {
    unsigned int instID = ray.instID[0];
    {
      /* get instance and geometry pointers */
      ISPCInstance* instance = (ISPCInstancePtr) data.ispc_scene->geometries[instID];

      /* convert normals */
      //AffineSpace3fa space = (1.0f-ray.time())*AffineSpace3fa(instance->space0) + ray.time()*AffineSpace3fa(instance->space1);
      AffineSpace3fa space = calculate_interpolated_space(instance,ray.time());
      dg.Ng = xfmVector(space,dg.Ng);
      dg.Ns = xfmVector(space,dg.Ns);
    }
  }

  return materialID;
}

inline Vec3fa face_forward(const Vec3fa& dir, const Vec3fa& _Ng) {
  const Vec3fa Ng = _Ng;
  return dot(dir,Ng) < 0.0f ? Ng : neg(Ng);
}

/* task that renders a single screen tile */
void renderPixelStandard(const TutorialData& data,
                         int x, int y, 
                         int* pixels,
                         const unsigned int width,
                         const unsigned int height,
                         const float time,
                         const ISPCCamera& camera, RayStats& stats)
{
  /* initialize sampler */
  RandomSampler sampler;
  RandomSampler_init(sampler, (int)x, (int)y, 0);

  /* initialize ray */
  Ray ray(Vec3fa(camera.xfm.p), Vec3fa(normalize(x*camera.xfm.l.vx + y*camera.xfm.l.vy + camera.xfm.l.vz)), 0.0f, inf, RandomSampler_get1D(sampler));

  /* intersect ray with scene */
  RTCIntersectContext context;
  rtcInitIntersectContext(&context);
  context.flags = data.iflags_coherent;
#if RTC_MIN_WIDTH
  context.minWidthDistanceFactor = 0.5f*data.min_width/width;
#endif
  rtcIntersect1(data.scene,&context,RTCRayHit_(ray));
  RayStats_addRay(stats);

  /* shade background black */
  if (ray.geomID == RTC_INVALID_GEOMETRY_ID) {
    pixels[y*width+x] = 0;
    return;
  }

  /* shade all rays that hit something */
  Vec3fa color = Vec3fa(0.5f);

  /* compute differential geometry */
  DifferentialGeometry dg;
  dg.geomID = ray.geomID;
  dg.primID = ray.primID;
  dg.u = ray.u;
  dg.v = ray.v;
  dg.P  = ray.org+ray.tfar*ray.dir;
  dg.Ng = ray.Ng;
  dg.Ns = ray.Ng;

#if 0
  if (data.use_smooth_normals)
    if (ray.geomID != RTC_INVALID_GEOMETRY_ID) // FIXME: workaround for ISPC bug, location reached with empty execution mask
    {
      Vec3fa dPdu,dPdv;
      unsigned int geomID = ray.geomID; {
        rtcInterpolate1(rtcGetGeometry(data.scene,geomID),ray.primID,ray.u,ray.v,RTC_BUFFER_TYPE_VERTEX,0,nullptr,&dPdu.x,&dPdv.x,3);
      }
      dg.Ns = cross(dPdv,dPdu);
    }
#endif

  int materialID = postIntersect(data,ray,dg);
  dg.Ng = face_forward(ray.dir,normalize(dg.Ng));
  dg.Ns = face_forward(ray.dir,normalize(dg.Ns));

  /* shade */
  if (data.ispc_scene->materials[materialID]->type == MATERIAL_OBJ) {
    ISPCOBJMaterial* material = (ISPCOBJMaterial*) data.ispc_scene->materials[materialID];
    color = Vec3fa(material->Kd);
  }

  color = color*dot(neg(ray.dir),dg.Ns);

  /* write color to framebuffer */
  unsigned int r = (unsigned int) (255.0f * clamp(color.x,0.0f,1.0f));
  unsigned int g = (unsigned int) (255.0f * clamp(color.y,0.0f,1.0f));
  unsigned int b = (unsigned int) (255.0f * clamp(color.z,0.0f,1.0f));
  pixels[y*width+x] = (b << 16) + (g << 8) + r;
}

/* task that renders a single screen tile */
void renderTileTask (int taskIndex, int threadIndex, int* pixels,
                         const unsigned int width,
                         const unsigned int height,
                         const float time,
                         const ISPCCamera& camera,
                         const int numTilesX,
                         const int numTilesY)
{
  const int t = taskIndex;
  const unsigned int tileY = t / numTilesX;
  const unsigned int tileX = t - tileY * numTilesX;
  const unsigned int x0 = tileX * TILE_SIZE_X;
  const unsigned int x1 = min(x0+TILE_SIZE_X,width);
  const unsigned int y0 = tileY * TILE_SIZE_Y;
  const unsigned int y1 = min(y0+TILE_SIZE_Y,height);

  for (unsigned int y=y0; y<y1; y++) for (unsigned int x=x0; x<x1; x++)
  {
    renderPixelStandard(data,x,y,pixels,width,height,time,camera,g_stats[threadIndex]);
  }
}

Vec3fa old_p;

/* called by the C++ code for initialization */
extern "C" void device_init (char* cfg)
{
  TutorialData_Constructor(&data);
  old_p = Vec3fa(1E10);
}

extern "C" void renderFrameStandard (int* pixels,
                          const unsigned int width,
                          const unsigned int height,
                          const float time,
                          const ISPCCamera& camera)
{
  /* render image */
  const int numTilesX = (width +TILE_SIZE_X-1)/TILE_SIZE_X;
  const int numTilesY = (height+TILE_SIZE_Y-1)/TILE_SIZE_Y;
  parallel_for(size_t(0),size_t(numTilesX*numTilesY),[&](const range<size_t>& range) {
    const int threadIndex = (int)TaskScheduler::threadIndex();
    for (size_t i=range.begin(); i<range.end(); i++)
      renderTileTask((int)i,threadIndex,pixels,width,height,time,camera,numTilesX,numTilesY);
  });
}

/* called by the C++ code to render */
extern "C" void device_render (int* pixels,
                           const unsigned int width,
                           const unsigned int height,
                           const float time,
                           const ISPCCamera& camera)
{
  bool camera_changed = g_changed; g_changed = false;

  /* create scene */
  if (data.scene == nullptr) {
    g_scene = data.scene = convertScene(g_ispc_scene);
    //if (data.subdiv_mode) updateEdgeLevels(g_ispc_scene, camera.xfm.p);

    /* first count number of progress callback invokations */
    invokations.store(0);
    cancel_at_invokation = -1;

    if (g_instancing_mode != ISPC_INSTANCING_NONE) {
      for (unsigned int i=0; i<g_ispc_scene->numGeometries; i++) {
        ISPCGeometry* geometry = g_ispc_scene->geometries[i];
        if (geometry->type == GROUP) rtcCommitScene(geometry->scene);
      }
    }
    rtcCommitScene (data.scene);
    size_t numProgressInvokations = invokations;
    PRINT(numProgressInvokations);

    tbb::task_group* group = new tbb::task_group;
      
    for (size_t i=0; i<10000; i++)
    {
      /* just to trigger a rebuild */
      if (g_instancing_mode != ISPC_INSTANCING_NONE) {
        for (unsigned int j=0; j<g_ispc_scene->numGeometries; j++) {
          ISPCGeometry* geometry = g_ispc_scene->geometries[j];
          if (geometry->type == GROUP) {
            if (((ISPCGroup*)geometry)->numGeometries) {
              rtcCommitGeometry(rtcGetGeometry(geometry->scene,0));
            }
          }
        }
      }
      rtcCommitGeometry(rtcGetGeometry(g_scene,0));
      
      PRINT(i);
      invokations.store(0);
      cancel_at_invokation = rand()%numProgressInvokations;

      if (g_instancing_mode != ISPC_INSTANCING_NONE)
      {
#if 0
        for (size_t j=0; j<g_ispc_scene->numGeometries; j++)
        {
           ISPCGeometry* geometry = g_ispc_scene->geometries[j];
           if (geometry->type == GROUP) rtcCommitScene(geometry->scene);
        }

        rtcCommitScene (data.scene);
        
#else     
        group->run([&]{
            tbb::parallel_for (size_t(0), size_t(g_ispc_scene->numGeometries), size_t(1), [&] (size_t j) {
                ISPCGeometry* geometry = g_ispc_scene->geometries[j];
                if (geometry->type == GROUP) rtcCommitScene(geometry->scene);  
              });
            
            rtcCommitScene (data.scene);
      
          });
        group->wait();
#endif
      }
      
      PRINT(invokations);
    }
    old_p = camera.xfm.p;
  }

  else
  {
    /* check if camera changed */
    if (ne(camera.xfm.p,old_p)) {
      camera_changed = true;
      old_p = camera.xfm.p;
    }

    /* update edge levels if camera changed */
    if (camera_changed && data.subdiv_mode) {
      updateEdgeLevels(g_ispc_scene,camera.xfm.p);
      rtcCommitScene (data.scene);
    }
  }
}

/* called by the C++ code for cleanup */
extern "C" void device_cleanup ()
{
  TutorialData_Destructor(&data);
}

} // namespace embree
