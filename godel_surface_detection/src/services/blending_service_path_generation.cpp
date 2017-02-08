#include <services/surface_blending_service.h>
#include <segmentation/surface_segmentation.h>
#include <detection/surface_detection.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/PointIndices.h>
#include <pcl/point_types.h>
#include <pcl_ros/point_cloud.h>

// Temporary constants for storing blending path `planning parameters
// Will be replaced by loadable, savable parameters
const static std::string BLEND_TRAJECTORY_BAGFILE = "blend_trajectory.bag";
const static std::string BLEND_TRAJECTORY_GROUP_NAME = "manipulator_tcp";
const static std::string BLEND_TRAJECTORY_TOOL_FRAME = "tcp_frame";
const static std::string BLEND_TRAJECTORY_WORLD_FRAME = "world_frame";
const static double BLEND_TRAJECTORY_ANGLE_DISC = M_PI / 10.0;

// Temporary constants for storing scan path planning parameters
// Will be replaced by loadable, savable parameters
const static std::string SCAN_TRAJECTORY_BAGFILE = "scan_trajectory.bag";
const static std::string SCAN_TRAJECTORY_GROUP_NAME = "manipulator_keyence";
const static std::string SCAN_TRAJECTORY_TOOL_FRAME = "keyence_tcp_frame";
const static std::string SCAN_TRAJECTORY_WORLD_FRAME = "world_frame";
const static double SCAN_TRAJECTORY_ANGLE_DISC = 0.2;

const static std::string BLEND_TYPE = "blend";
const static std::string EDGE_TYPE = "edge";
const static std::string SCAN_TYPE = "scan";

// Together these constants define a 5cm approach and departure path for the
// laser scans
const static int SCAN_APPROACH_STEP_COUNT = 5;
const static double SCAN_APPROACH_STEP_DISTANCE = 0.01; // 1cm

// Edge Processing constants
const static double SEGMENTATION_SEARCH_RADIUS = 0.03; // 3cm
const static int BOUNDARY_THRESHOLD = 10;
const static double MIN_BOUNDARY_LENGTH = 0.1; // 10 cm

// Variables to select path type
const static int PATH_TYPE_BLENDING = 0;
const static int PATH_TYPE_SCAN = 1;
const static int PATH_TYPE_EDGE = 2;

const static std::string SURFACE_DESIGNATION = "surface_marker_server_";

// Temporary hack: remove when you figure out how to populate process parameters in a better fashion
const static double TOOL_FORCE = 0.0;
const static double SPINDLE_SPEED = 0.0;
const static double APPROACH_SPD = 0.005;
const static double BLENDING_SPD = 0.3;
const static double RETRACT_SPD = 0.02;
const static double TRAVERSE_SPD = 0.05;
const static double APPROACH_DISTANCE = 0.15;
const static int QUALITY_METRIC = 0;
const static double WINDOW_WIDTH = 0.02;
const static double MIN_QA_VALUE = 0.05;
const static double MAX_QA_VALUE = 0.05;

/**
 * Prototype ProcessPlan refactoring - make it compatible with trajectory library and GUI
 */
static godel_process_path::PolygonBoundaryCollection
filterPolygonBoundaries(const godel_process_path::PolygonBoundaryCollection& boundaries,
                        const double& min_boudnary_length)
{
  godel_process_path::PolygonBoundaryCollection filtered_boundaries;

  for (std::size_t i = 0; i < boundaries.size(); ++i)
  {
    const godel_process_path::PolygonBoundary& bnd = boundaries[i];
    double circ = godel_process_path::polygon_utils::circumference(bnd);

    if (circ < min_boudnary_length)
    {
      ROS_WARN_STREAM("Ignoring boundary with length " << circ);
    }
    else if (!godel_process_path::polygon_utils::checkBoundary(bnd))
    {
      ROS_WARN_STREAM("Ignoring ill-formed boundary");
    }
    else
    {
      filtered_boundaries.push_back(bnd);
      godel_process_path::polygon_utils::filter(filtered_boundaries.back(), 0.1);
      std::reverse(filtered_boundaries.back().begin(), filtered_boundaries.back().end());
    }
  }
  return filtered_boundaries;
}


bool SurfaceBlendingService::requestEdgePath(std::vector<pcl::IndicesPtr> &boundaries,
                                             int index,
                                             SurfaceSegmentation& SS,
                                             geometry_msgs::PoseArray& path)
{
  geometry_msgs::Pose geo_pose;
  std::vector<Eigen::Matrix4d, Eigen::aligned_allocator<Eigen::Matrix4d>> poses;

  // Get boundary trajectory and trim last two poses (last poses are susceptible to large velocity changes)
  SS.getBoundaryTrajectory(boundaries, index, poses);
  poses.resize(poses.size() - 2);

  // Convert eigen poses to geometry poses for messaging and visualization
  for(const auto& p : poses)
  {
    Eigen::Affine3d pose(p.matrix());
    tf::poseEigenToMsg(pose, geo_pose);
    path.poses.push_back(geo_pose);
  }

  return true;
}

bool SurfaceBlendingService::requestBlendPath(
    const godel_process_path::PolygonBoundaryCollection& boundaries,
    const geometry_msgs::Pose& boundary_pose,
    const godel_msgs::PathPlanningParameters& params,
    geometry_msgs::PoseArray& path)
{
  godel_msgs::PathPlanning srv;
  srv.request.params = params;
  godel_process_path::utils::translations::godelToGeometryMsgs(srv.request.surface.boundaries, boundaries);
  tf::poseTFToMsg(tf::Transform::getIdentity(), srv.request.surface.pose);

  if (!process_path_client_.call(srv))
  {
    return false;
  }

  // blend process path calculations suceeded. Save data into results.
  geometry_msgs::PoseArray path_local = srv.response.poses;
  geometry_msgs::Pose p;
  p.orientation.x = 0.0;
  p.orientation.y = 0.0;
  p.orientation.z = 0.0;
  p.orientation.w = 1.0;

  // Transform points to world frame and generate pose
  Eigen::Affine3d boundary_pose_eigen;
  Eigen::Affine3d eigen_p;
  Eigen::Affine3d result;

  tf::poseMsgToEigen(boundary_pose, boundary_pose_eigen);

  for (const auto& pose : path_local.poses)
  {
    p.position.x = pose.position.x;
    p.position.y = pose.position.y;
    p.position.z = pose.position.z;

    tf::poseMsgToEigen(p, eigen_p);
    result = boundary_pose_eigen*eigen_p;
    tf::poseEigenToMsg(result, p);
    p.orientation = boundary_pose.orientation;
    path.poses.push_back(p);
  }

  return true;
}

bool SurfaceBlendingService::requestScanPath(
    const godel_process_path::PolygonBoundaryCollection& boundaries,
    const geometry_msgs::Pose& boundary_pose,
    const godel_msgs::PathPlanningParameters& params,
    geometry_msgs::PoseArray& path)
{
  using namespace godel_process_path;

  // 0 - Skip if boundaries are empty
  if (boundaries.empty())
    return false;

  // 1 - Generate scan polygon boundary
  PolygonBoundary scan = godel_surface_detection::generateProfilimeterScanPath(boundaries.front(), params);


  // 2 - Get boundary pose eigen
  Eigen::Affine3d boundary_pose_eigen;
  tf::poseMsgToEigen(boundary_pose, boundary_pose_eigen);

  // 3 - Transform points to world frame and generate pose
  // Because the output of our profilometer generation is a path of points in the boundary pose, we
  // generate our output path by taking the boundary pose & just offset it by each point
  std::vector<geometry_msgs::Point> points;
  for(const auto& pt : scan)
  {
    geometry_msgs::Point p;
	p.x = pt.x;
	p.y = pt.y;
    p.position.z = 0.0;
	points.push_back(p);
  }
  
  std::transform(points.begin(), points.end(), std::back_inserter(path.poses),
                 [boundary_pose_eigen] (const geometry_msgs::Point& point) {
    geometry_msgs::Pose pose;
    Eigen::Affine3d r = boundary_pose_eigen * Eigen::Translation3d(point.x, point.y, point.z);
    tf::poseEigenToMsg(r, pose);
    return pose;
  });

  // 4 - Add in approach and departure
  geometry_msgs::PoseArray approach;
  geometry_msgs::Pose start_pose;
  geometry_msgs::Pose end_pose;
  start_pose = path.poses.front();
  end_pose = path.poses.back();
  double start_z = start_pose.position.z;
  double end_z = end_pose.position.z;

  for (std::size_t i = 0; i < SCAN_APPROACH_STEP_COUNT; ++i)
  {
    double z_offset = i * SCAN_APPROACH_STEP_DISTANCE;
    geometry_msgs::Pose approach_pose;
    geometry_msgs::Pose departure_pose;

    approach_pose.orientation = start_pose.orientation;
    approach_pose.position = start_pose.position;
    approach_pose.position.z = start_z + z_offset;
    path.poses.insert(path.poses.begin(), approach_pose);

    departure_pose.orientation = end_pose.orientation;
    departure_pose.position = end_pose.position;
    departure_pose.position.z = end_z + z_offset;
    path.poses.push_back(departure_pose);
  }

  return true;
}

void computeBoundaries(const godel_surface_detection::detection::CloudRGB::Ptr surface_cloud,
                       SurfaceSegmentation& SS,
                       std::vector< pcl::IndicesPtr>& sorted_boundaries)
{
  pcl::PointCloud<pcl::Boundary>::Ptr boundary_ptr (new pcl::PointCloud<pcl::Boundary>());
  pcl::PointCloud<pcl::PointXYZRGB>::Ptr boundary_cloud_ptr(new pcl::PointCloud<pcl::PointXYZRGB>());
  SS.getBoundaryCloud(boundary_ptr);
  int k=0;

  pcl::IndicesPtr boundary_idx(new std::vector<int>());
  for(const auto& pt : boundary_ptr->points)
  {
    if(pt.boundary_point)
    {
      boundary_cloud_ptr->points.push_back(surface_cloud->points[k]);
      boundary_idx->push_back(k);
    }
    k++;
  }

  boundary_cloud_ptr->width = 1;
  boundary_cloud_ptr->height = boundary_cloud_ptr->points.size();


  // sort the boundaries
  SS.sortBoundary(boundary_idx, sorted_boundaries);

  int max=0;
  int max_idx=0;

  for(int i=0;i<sorted_boundaries.size();i++)
  {
    if(sorted_boundaries[i]->size() > max)
    {
      max = sorted_boundaries[i]->size();
      max_idx = i;
    }
  }
}

inline static bool isBlendingPath(const std::string& name)
{
  const static std::string suffix("_blend");
  if (name.size() < suffix.size())
    return false;
  return name.find(suffix, name.size() - suffix.length()) != std::string::npos;
}


inline static bool isEdgePath(const std::string& name)
{
  const static std::string suffix("_edge");
  if (name.size() < suffix.size())
    return false;
  return name.find(suffix) != std::string::npos;
}

inline static bool isScanPath(const std::string& name)
{
  const static std::string suffix("_scan");
  if (name.size() < suffix.size())
    return false;
  return name.find(suffix) != std::string::npos;
}


ProcessPathResult
SurfaceBlendingService::generateProcessPath(const int& id,
                                            const godel_msgs::PathPlanningParameters& params)
{
  using godel_surface_detection::detection::CloudRGB;

  std::string name;
  pcl::PolygonMesh mesh;
  CloudRGB::Ptr surface_ptr (new CloudRGB);

  data_coordinator_.getSurfaceName(id, name);
  data_coordinator_.getSurfaceMesh(id, mesh);
  data_coordinator_.getCloud(godel_surface_detection::data::CloudTypes::surface_cloud, id, *surface_ptr);

  return generateProcessPath(id, name, mesh, surface_ptr, params);
}


ProcessPathResult
SurfaceBlendingService::generateProcessPath(const int& id,
                                            const std::string& name,
                                            const pcl::PolygonMesh& mesh,
                                            godel_surface_detection::detection::CloudRGB::Ptr surface,
                                            const godel_msgs::PathPlanningParameters& params)
{
  using godel_process_path::PolygonBoundaryCollection;
  using godel_process_path::PolygonBoundary;

  ProcessPathResult result;

  // Calculate boundaries for a surface
  if (!mesh_importer_.calculateSimpleBoundary(mesh))
  {
    ROS_WARN_STREAM("Could not calculate boundary for mesh associated with name: " << name);
    return result;
  }

  // Read & filter boundaries that are ill-formed or too small
  PolygonBoundaryCollection filtered_boundaries =
      filterPolygonBoundaries(mesh_importer_.getBoundaries(), MIN_BOUNDARY_LENGTH);

  // Read pose
  geometry_msgs::Pose boundary_pose;
  mesh_importer_.getPose(boundary_pose);

  // Send request to blend path generation service
  geometry_msgs::PoseArray blend_poses;
  if (requestBlendPath(filtered_boundaries, boundary_pose, params, blend_poses))
  {
    ProcessPathResult::value_type blend_path_result; // pair<string, geometry_msgs::PoseArray>
    blend_path_result.first = name + "_blend";
    blend_path_result.second = blend_poses;
    result.paths.push_back(blend_path_result);
    data_coordinator_.setPoses(godel_surface_detection::data::PoseTypes::blend_pose, id, blend_poses);
  }
  else
  {
    // Blend path request failed
    ROS_WARN_STREAM("Could not calculate blend path for surface: " << name);
  }

  ROS_INFO_STREAM("Blend Path Generation Complete");

  // Send request to edge path generation service
  std::vector<pcl::IndicesPtr> sorted_boundaries;

  ROS_INFO_STREAM("Surface has " + std::to_string(surface->points.size()) + "points");
  // Compute the boundary
  SurfaceSegmentation SS(surface);

  SS.setSearchRadius(SEGMENTATION_SEARCH_RADIUS);
  std::vector<double> filt_coef;
  filt_coef.push_back(1);
  filt_coef.push_back(2);
  filt_coef.push_back(3);
  filt_coef.push_back(4);
  filt_coef.push_back(5);
  filt_coef.push_back(4);
  filt_coef.push_back(3);
  filt_coef.push_back(2);
  filt_coef.push_back(1);

  SS.setSmoothCoef(filt_coef);
  computeBoundaries(surface, SS, sorted_boundaries);

  ROS_INFO_STREAM("Boundaries Computed");
  geometry_msgs::PoseArray all_edge_poses;
  for(int i = 0; i < sorted_boundaries.size(); i++)
  {
    if(sorted_boundaries.at(i)->size() < BOUNDARY_THRESHOLD)
      continue;

    geometry_msgs::PoseArray edge_poses;

    if(requestEdgePath(sorted_boundaries, i, SS, edge_poses))
    {
      ProcessPathResult::value_type edge_path_result; // pair<string, geometry_msgs::PoseArray>
      edge_path_result.first = name + "_edge_" + std::to_string(i);

      /*
       * Set the orientation for all edge points to be the orientation of the surface normal
       * This is a hack that should be removed when planar surfaces assumption is dropped
       * The main purpose here is to "smooth" the trajectory of the edges w.r.t. the z axis
      */
      for(auto& p : edge_poses.poses)
        p.orientation = boundary_pose.orientation;

      edge_path_result.second = edge_poses;
      result.paths.push_back(edge_path_result);

      // Add poses to visualization
      all_edge_poses.poses.insert(std::end(all_edge_poses.poses), std::begin(edge_poses.poses),
                                  std::end(edge_poses.poses));

      // Add edge to data coordinator
      data_coordinator_.addEdge(id, edge_path_result.first, edge_poses);
    }
    else
    {
      // Blend path request failed
      ROS_WARN_STREAM("Could not calculate blend path for surface: " << name);
    }
  }

  // Request laser scan paths
  geometry_msgs::PoseArray scan_poses;
  if (requestScanPath(filtered_boundaries, boundary_pose, params, scan_poses))
  {
    ProcessPathResult::value_type scan_path_result;
    scan_path_result.first = name + "_scan";
    scan_path_result.second = scan_poses;
    result.paths.push_back(scan_path_result);

    data_coordinator_.setPoses(godel_surface_detection::data::PoseTypes::scan_pose, id, scan_poses);
  }
  else
  {
    ROS_WARN_STREAM("Could not calculate scan path for surface: " << name);
  }

  return result;
}

godel_surface_detection::TrajectoryLibrary SurfaceBlendingService::generateMotionLibrary(
    const godel_msgs::PathPlanningParameters& params)
{
  std::vector<int> selected_ids;
  surface_server_.getSelectedIds(selected_ids);

  godel_surface_detection::TrajectoryLibrary lib;
  // Clear previous results
  process_path_results_.blend_poses_.clear();
  process_path_results_.edge_poses_.clear();
  process_path_results_.scan_poses_.clear();

  for (const auto& id : selected_ids)
  {
    // Generate motion plan
    ProcessPathResult paths = generateProcessPath(id, params);

    // Add new path to result
    for(const auto& vt: paths.paths)
    {
      if(isBlendingPath(vt.first))
        process_path_results_.blend_poses_.push_back(vt.second);

      else if(isEdgePath(vt.first))
        process_path_results_.edge_poses_.push_back(vt.second);

      else if(isScanPath(vt.first))
        process_path_results_.scan_poses_.push_back(vt.second);

      else
        ROS_ERROR_STREAM("Tried to process an unrecognized path type: " << vt.first);
    }

    godel_msgs::BlendingPlanParameters blend_params;
    blend_params.margin = params.margin;
    blend_params.overlap = params.overlap;
    blend_params.tool_radius = params.tool_radius;
    blend_params.discretization = params.discretization;
    blend_params.safe_traverse_height = params.traverse_height;
    blend_params.spindle_speed = SPINDLE_SPEED;
    blend_params.approach_spd = APPROACH_SPD;
    blend_params.blending_spd = BLENDING_SPD;
    blend_params.retract_spd = RETRACT_SPD;
    blend_params.traverse_spd = TRAVERSE_SPD;

    godel_msgs::ScanPlanParameters scan_params;
    scan_params.scan_width = params.scan_width;
    scan_params.margin = params.margin;
    scan_params.overlap = params.overlap;
    scan_params.scan_width = params.scan_width;
    scan_params.approach_distance = APPROACH_DISTANCE;
    scan_params.traverse_spd = TRAVERSE_SPD;
    scan_params.quality_metric = QUALITY_METRIC;
    scan_params.window_width = WINDOW_WIDTH;
    scan_params.min_qa_value = MIN_QA_VALUE;
    scan_params.max_qa_value = MAX_QA_VALUE;


    // Generate trajectory plans from motion plan
    for (std::size_t j = 0; j < paths.paths.size(); ++j)
    {
      ProcessPlanResult plan = generateProcessPlan(paths.paths[j].first, paths.paths[j].second, blend_params,
                                                   scan_params);

      for (std::size_t k = 0; k < plan.plans.size(); ++k)
        lib.get()[plan.plans[k].first] = plan.plans[k].second;
    }
  }

  return lib;
}


ProcessPlanResult
SurfaceBlendingService::generateProcessPlan(const std::string& name,
                                            const geometry_msgs::PoseArray& poses,
                                            const godel_msgs::BlendingPlanParameters& params,
                                            const godel_msgs::ScanPlanParameters& scan_params)
{
  ProcessPlanResult result;

  bool success = false;
  godel_msgs::ProcessPlan process_plan;


  if (isBlendingPath(name))
  {
    godel_msgs::BlendProcessPlanning srv;
    srv.request.path.poses = poses;
    srv.request.params = params;

    success = blend_planning_client_.call(srv);
    process_plan = srv.response.plan;
  }
  else if (isEdgePath(name))
  {
    godel_msgs::BlendProcessPlanning srv;
    srv.request.path.poses = poses;
    srv.request.params = params;

    success = blend_planning_client_.call(srv);
    process_plan = srv.response.plan;
  }
  else
  {
    godel_msgs::KeyenceProcessPlanning srv;
    srv.request.path.poses = poses;
    srv.request.params = scan_params;

    success = keyence_planning_client_.call(srv);
    process_plan = srv.response.plan;
  }

  if (success)
  {
    ProcessPlanResult::value_type plan;
    plan.first = name;
    plan.second = process_plan;
    result.plans.push_back(plan);
  }
  else
  {
    ROS_ERROR_STREAM("Failed to plan for: " << name);
  }

  return result;
}
