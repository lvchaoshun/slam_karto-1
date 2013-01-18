/*
 * slam_karto
 * Copyright (c) 2008, Willow Garage, Inc.
 *
 * THE WORK (AS DEFINED BELOW) IS PROVIDED UNDER THE TERMS OF THIS CREATIVE
 * COMMONS PUBLIC LICENSE ("CCPL" OR "LICENSE"). THE WORK IS PROTECTED BY
 * COPYRIGHT AND/OR OTHER APPLICABLE LAW. ANY USE OF THE WORK OTHER THAN AS
 * AUTHORIZED UNDER THIS LICENSE OR COPYRIGHT LAW IS PROHIBITED.
 *
 * BY EXERCISING ANY RIGHTS TO THE WORK PROVIDED HERE, YOU ACCEPT AND AGREE TO
 * BE BOUND BY THE TERMS OF THIS LICENSE. THE LICENSOR GRANTS YOU THE RIGHTS
 * CONTAINED HERE IN CONSIDERATION OF YOUR ACCEPTANCE OF SUCH TERMS AND
 * CONDITIONS.
 *
 */

/* Author: Brian Gerkey, Robert Huitl */

/**

@mainpage karto_gmapping

@htmlinclude manifest.html

*/

#include "ros/ros.h"
#include "ros/console.h"
#include "message_filters/subscriber.h"
#include "tf/transform_broadcaster.h"
#include "tf/transform_listener.h"
#include "tf/message_filter.h"
#include "visualization_msgs/MarkerArray.h"
#include "geometry_msgs/PoseArray.h"

#include "nav_msgs/MapMetaData.h"
#include "sensor_msgs/LaserScan.h"
#include "nav_msgs/GetMap.h"

#include <dynamic_reconfigure/server.h>
#include <karto/KartoConfig.h>
#include <karto/SaveMapperState.h>
#include <karto/LoadMapperState.h>

#include <Karto/Karto.h>

#include <boost/thread.hpp>

#include <string>
#include <map>
#include <vector>

// compute linear index for given map coords
#define MAP_IDX(sx, i, j) ((sx) * (j) + (i))

class SlamKarto
{
  public:
    SlamKarto();
    ~SlamKarto();

    void laserCallback(const sensor_msgs::LaserScan::ConstPtr& scan);
    bool mapCallback(nav_msgs::GetMap::Request  &req,
                     nav_msgs::GetMap::Response &res);

  private:
    bool getOdomPose(karto::Pose2& karto_pose, const ros::Time& t);
    bool addLaserToKarto(const sensor_msgs::LaserScan::ConstPtr& scan);
    bool addScan(const sensor_msgs::LaserScan::ConstPtr& scan,
                 karto::Pose2& karto_pose);
    bool updateMap();
    void publishTransform();
    void publishPreciseTransform(const tf::StampedTransform& t);
//    void publishLoop();
//    void publishGraphVisualization();
    void publishParticlesVisualization();
    void reconfigurationCallback(karto::KartoConfig &config, uint32_t level);
    void goalCallback(const geometry_msgs::PoseStamped& msg);
    void forceUpdateCallback(const std_msgs::Header& msg);


    bool saveMapperState(karto::SaveMapperState::Request& request,
                         karto::SaveMapperState::Response& response);
    bool loadMapperState(karto::LoadMapperState::Request& request,
                         karto::LoadMapperState::Response& response);
    // need a type to use advertiseService() with boost::bind()
    //   see https://code.ros.org/trac/ros/ticket/1245
    typedef boost::function<bool(karto::SaveMapperState::Request&, karto::SaveMapperState::Response&)> SaveMapperStateCallback;
    typedef boost::function<bool(karto::LoadMapperState::Request&, karto::LoadMapperState::Response&)> LoadMapperStateCallback;

    void setLocalizerConfig();


    // ROS handles
    ros::NodeHandle node_;
    tf::TransformListener tf_;
    tf::TransformBroadcaster tfB_;
    message_filters::Subscriber<sensor_msgs::LaserScan>* scan_filter_sub_;
    tf::MessageFilter<sensor_msgs::LaserScan>* scan_filter_;
    ros::Publisher sst_;
    ros::Publisher marker_publisher_;
    ros::Publisher particles_publisher_;
    ros::Publisher sstm_;
    ros::ServiceServer ss_;
    boost::recursive_mutex serverMutex_;
    dynamic_reconfigure::Server<karto::KartoConfig> server_;
    karto::KartoConfig config_;
    ros::ServiceServer saveMapperStateService_;
    ros::ServiceServer loadMapperStateService_;
    ros::Subscriber goal_subs_;
    ros::Subscriber forceUpdate_subs_;

    // The map that will be published / sent to service callers
    nav_msgs::GetMap::Response map_;

    // Storage for ROS parameters
    std::string odom_frame_;
    std::string map_frame_, map_precise_frame_;
    std::string base_frame_;
    int throttle_scans_;
    ros::Duration map_update_interval_;
    double resolution_;
    boost::mutex map_mutex_;
    boost::mutex map_to_odom_mutex_;
    double transform_publish_period_;

    // Karto bookkeeping
    karto::mapper::MapperPtr mapper_;
    karto::localizer::LocalizerPtr localizer_;
    std::map<std::string, bool> lasers_added_to_karto_;
    karto::Pose2 last_corrected_pose_;

    // Internal state
    bool got_map_;
    int laser_count_;
    boost::thread* transform_thread_;
	tf::StampedTransform map_to_odom_;
    unsigned marker_count_;
    bool force_update_;
    ros::Time force_update_time_;

    // debug
    //karto::DatasetWriterPtr w1, w2;
};

SlamKarto::SlamKarto() :
        server_(serverMutex_),
        got_map_(false),
        laser_count_(0),
        transform_thread_(NULL),
        marker_count_(0),
        force_update_(false)
{
  // Retrieve parameters
  ros::NodeHandle private_nh_("~");
  if(!private_nh_.getParam("odom_frame", odom_frame_))
    odom_frame_ = "odom";
  if(!private_nh_.getParam("map_frame", map_frame_))
    map_frame_ = "map";
  if(!private_nh_.getParam("map_precise_frame", map_precise_frame_))
    map_precise_frame_ = "map_precise";
  if(!private_nh_.getParam("base_frame", base_frame_))
    base_frame_ = "base_link";
  if(!private_nh_.getParam("throttle_scans", throttle_scans_))
    throttle_scans_ = 1;
  double tmp;
  if(!private_nh_.getParam("map_update_interval", tmp))
    tmp = 1.0;
  map_update_interval_.fromSec(tmp);
  if(!private_nh_.getParam("resolution", resolution_))
  {
    // Compatibility with slam_gmapping, which uses "delta" to mean
    // resolution
    if(!private_nh_.getParam("delta", resolution_))
      resolution_ = 0.05;
  }
  private_nh_.param("transform_publish_period", transform_publish_period_, .05);

  // Initialize the stamped transform between odometry and map frame
  map_to_odom_ = tf::StampedTransform ( tf::Transform::getIdentity(),
    ros::Time::now(), map_frame_, odom_frame_);

  // Set up advertisements and subscriptions
  sst_ = node_.advertise<nav_msgs::OccupancyGrid>("map", 1, true);
  sstm_ = node_.advertise<nav_msgs::MapMetaData>("map_metadata", 1, true);
  ss_ = node_.advertiseService("dynamic_map", &SlamKarto::mapCallback, this);

  scan_filter_sub_ = new message_filters::Subscriber<sensor_msgs::LaserScan>(node_, "scan", 5);
  scan_filter_ = new tf::MessageFilter<sensor_msgs::LaserScan>(*scan_filter_sub_, tf_, odom_frame_, 5);
  scan_filter_->registerCallback(boost::bind(&SlamKarto::laserCallback, this, _1));

  marker_publisher_ = node_.advertise<visualization_msgs::MarkerArray>("visualization_marker_array",1);
  particles_publisher_ = node_.advertise<geometry_msgs::PoseArray>("particles_array",1);

  // Subscribe to force_update topic, when receiving a message, a scan is forced to be added
  forceUpdate_subs_ = node_.subscribe("force_update", 100, &SlamKarto::forceUpdateCallback, this);

  // Create a thread to periodically publish the latest map->odom
  // transform; it needs to go out regularly, uninterrupted by potentially
  // long periods of computation in our main loop.
//  transform_thread_ = new boost::thread(boost::bind(&SlamKarto::publishLoop, this));

  // Initialize Karto structures
  mapper_ = karto::mapper::CreateDefaultMapper();
  localizer_ = karto::localizer::CreateDefaultLocalizer();

  // Initialize configuration object. The real defaults will always be set by
  // the first reconfiguration callback (which is called inside setCallback()
  // and sets the parameters from the launch file).
  config_.add_scans = false;
  config_.update_map = false;

  // Callback for dynamic reconfiguration
  server_.setCallback(boost::bind(&SlamKarto::reconfigurationCallback, this, _1, _2));

  // Register services to save and restore the mapper state
  saveMapperStateService_ = node_.advertiseService("slam_karto/SaveMapperState",
    SaveMapperStateCallback(boost::bind(&SlamKarto::saveMapperState, this, _1, _2)));
  loadMapperStateService_ = node_.advertiseService("slam_karto/LoadMapperState",
    LoadMapperStateCallback(boost::bind(&SlamKarto::loadMapperState, this, _1, _2)));

  // Subscriber to /goal in order to set localizer position manually.
  goal_subs_ = node_.subscribe("/goal", 10, &SlamKarto::goalCallback, this);

  // Send out map (even if it's empty) so RViz can reflect the current state
  //updateMap();
  // TODO send out an empty map. Not so easy after all.

  ROS_INFO("slam_karto configuration: add_scans = %s, update_map = %s",
           config_.add_scans ? "true" : "false",
           config_.update_map ? "true" : "false");

  //w1 = karto::DatasetWriter::CreateWriter("/tmp/dataset_loc.kxd");
  //w2 = karto::DatasetWriter::CreateWriter("/tmp/dataset_map.kxd");
}

SlamKarto::~SlamKarto()
{
  if(transform_thread_)
  {
    transform_thread_->join();
    delete transform_thread_;
  }
  if (scan_filter_)
    delete scan_filter_;
  if (scan_filter_sub_)
    delete scan_filter_sub_;
}

/*void
SlamKarto::publishLoop()
{
  if(transform_publish_period_ == 0)
    return;

  ros::Rate r(1.0 / transform_publish_period_);
  while(ros::ok())
  {
    if(laser_count_ != 0)    // play nice with "rosbag play --clock" and don't
      publishTransform();    //   send out transforms before the first scan
    r.sleep();
  }
}*/

void
SlamKarto::publishTransform()
{
  boost::mutex::scoped_lock(map_to_odom_mutex_);
  tfB_.sendTransform(map_to_odom_);
}

void
SlamKarto::publishPreciseTransform(const tf::StampedTransform& t)
{
  tfB_.sendTransform(t);
}

bool
SlamKarto::addLaserToKarto(const sensor_msgs::LaserScan::ConstPtr& scan)
{
  // Strip trailing / from the name. Internally, Karto seems to do that, too.
  std::string sensor_id = scan->header.frame_id;
  if(sensor_id.at(0) == '/')
	  sensor_id = sensor_id.substr(1);

  // Check whether we know about this laser yet
  if(lasers_added_to_karto_.find(sensor_id) == lasers_added_to_karto_.end())
  {
    // New laser; need to create a Karto device for it.

    // Get the laser's pose, relative to base.
    tf::Stamped<tf::Pose> ident;
    tf::Stamped<tf::Pose> laser_pose;
    ident.setIdentity();
    ident.frame_id_ = scan->header.frame_id;
    ident.stamp_ = scan->header.stamp;
    try
    {
      tf_.transformPose(base_frame_, ident, laser_pose);
    }
    catch(tf::TransformException e)
    {
      ROS_WARN("Failed to compute laser pose, aborting initialization (%s)",
               e.what());
      return false;
    }

    double yaw = tf::getYaw(laser_pose.getRotation());

    ROS_INFO("Laser %s's pose with respect to base: %.3f %.3f %.3f",
      sensor_id.c_str(),
      laser_pose.getOrigin().x(),
      laser_pose.getOrigin().y(),
      yaw);

    // To account for lasers that are mounted upside-down, we determine the
    // min, max, and increment angles of the laser in the base frame.
    tf::Quaternion q;
    q.setRPY(0.0, 0.0, scan->angle_min);
    tf::Stamped<tf::Quaternion> min_q(q, scan->header.stamp,
                                      scan->header.frame_id);
    q.setRPY(0.0, 0.0, scan->angle_max);
    tf::Stamped<tf::Quaternion> max_q(q, scan->header.stamp,
                                      scan->header.frame_id);
    try
    {
      tf_.transformQuaternion(base_frame_, min_q, min_q);
      tf_.transformQuaternion(base_frame_, max_q, max_q);
    }
    catch(tf::TransformException& e)
    {
      ROS_WARN("Unable to transform min/max laser angles into base frame: %s",
               e.what());
      return false;
    }

    double angle_min = tf::getYaw(min_q);
    double angle_max = tf::getYaw(max_q);
    bool inverse = angle_max < angle_min;
    if (!inverse)
      assert(0); // had to drop that functionality //ROS_INFO("laser is mounted upside-down");


    // Create a laser range finder device and copy in data from the first scan
    karto::LaserRangeFinder* laser = karto::LaserRangeFinder::CreateLaserRangeFinder(
        karto::LaserRangeFinder_Custom, karto::Identifier(sensor_id.c_str()));
    laser->SetOffsetPose(karto::Pose2(laser_pose.getOrigin().x(),
                                      laser_pose.getOrigin().y(), yaw));
    laser->SetMinimumRange(scan->range_min);
    laser->SetMaximumRange(scan->range_max);
    laser->SetMinimumAngle(scan->angle_min);
    laser->SetMaximumAngle(scan->angle_max);
    laser->SetAngularResolution(scan->angle_increment);
    // TODO: expose this, and many other parameters
    laser->SetRangeThreshold(scan->range_max);

    ROS_INFO("Adding a laser to the mapper: (%.2f,%.2f) yaw %.1f, range: [%.1f %.1f], angle: [%.1f %.1f]",
             laser_pose.getOrigin().x(), laser_pose.getOrigin().y(), yaw,
             scan->range_min, scan->range_max, scan->angle_min, scan->angle_max);

    // Add to Karto
    if(!mapper_->Process(laser))
      return false;

    // Remember we added this device
    lasers_added_to_karto_[sensor_id] = true;
  }
  return true;
}

bool
SlamKarto::getOdomPose(karto::Pose2& karto_pose, const ros::Time& t)
{
  // Get the robot's pose
  tf::Stamped<tf::Pose> ident (btTransform(btQuaternion(0,0,0),
                                           btVector3(0,0,0)), t, base_frame_);
  tf::Stamped<tf::Pose> odom_pose;
  try
  {
    tf_.transformPose(odom_frame_, ident, odom_pose);
  }
  catch(tf::TransformException e)
  {
    ROS_WARN("Failed to compute odom pose, skipping scan (%s)", e.what());
    return false;
  }

  double yaw = tf::getYaw(odom_pose.getRotation());

  karto_pose = karto::Pose2(odom_pose.getOrigin().x(),
                            odom_pose.getOrigin().y(), yaw);
  return true;
}

#if 0
void
SlamKarto::publishGraphVisualization()
{
  //if(!solver_)
  return;

  std::vector<float> graph;
  //solver_->getGraph(graph);
  //karto::MapperGraph* graph = mapper_->GetGraph();

  visualization_msgs::MarkerArray marray;

  visualization_msgs::Marker m;
  m.header.frame_id = "map";
  m.header.stamp = ros::Time::now();
  m.id = 0;
  m.ns = "karto";
  m.type = visualization_msgs::Marker::SPHERE;
  m.pose.position.x = 0.0;
  m.pose.position.y = 0.0;
  m.pose.position.z = 0.0;
  m.scale.x = 0.1;
  m.scale.y = 0.1;
  m.scale.z = 0.1;
  m.color.r = 1.0;
  m.color.g = 0;
  m.color.b = 0.0;
  m.color.a = 1.0;
  m.lifetime = ros::Duration(0);

  visualization_msgs::Marker edge;
  edge.header.frame_id = "map";
  edge.header.stamp = ros::Time::now();
  edge.action = visualization_msgs::Marker::ADD;
  edge.ns = "karto";
  edge.id = 0;
  edge.type = visualization_msgs::Marker::LINE_STRIP;
  edge.scale.x = 0.1;
  edge.scale.y = 0.1;
  edge.scale.z = 0.1;
  edge.color.a = 1.0;
  edge.color.r = 0.0;
  edge.color.g = 0.0;
  edge.color.b = 1.0;

  m.action = visualization_msgs::Marker::ADD;
  uint id = 0;
  for (uint i=0; i<graph.size()/2; i++)
  {
    m.id = id;
    m.pose.position.x = graph[2*i];
    m.pose.position.y = graph[2*i+1];
    marray.markers.push_back(visualization_msgs::Marker(m));
    id++;

    if(i>0)
    {
      edge.points.clear();

      geometry_msgs::Point p;
      p.x = graph[2*(i-1)];
      p.y = graph[2*(i-1)+1];
      edge.points.push_back(p);
      p.x = graph[2*i];
      p.y = graph[2*i+1];
      edge.points.push_back(p);
      edge.id = id;

      marray.markers.push_back(visualization_msgs::Marker(edge));
      id++;
    }
  }

  m.action = visualization_msgs::Marker::DELETE;
  for (; id < marker_count_; id++)
  {
    m.id = id;
    marray.markers.push_back(visualization_msgs::Marker(m));
  }

  marker_count_ = marray.markers.size();

  marker_publisher_.publish(marray);
}
#endif

void
SlamKarto::publishParticlesVisualization()
{
  geometry_msgs::PoseArray arr;
  std::vector<geometry_msgs::Pose> poses;
  if(!config_.update_map) {      // The mapper does not expose its particles
    boost::mutex::scoped_lock(map_mutex_);
    karto_const_forEach(karto::localizer::ParticleList, &localizer_->GetParticles()) {
      const karto::localizer::Particle& p = (*iter);
      geometry_msgs::Pose pose;
      pose.position.x = p.x;
      pose.position.y = p.y;
      pose.position.z = 0;

      tf::Quaternion q = tf::createQuaternionFromYaw(p.heading);
      pose.orientation.x = q.x();
      pose.orientation.y = q.y();
      pose.orientation.z = q.z();
      pose.orientation.w = q.w();
      arr.poses.push_back(pose);
    }
  }

  arr.header.stamp = ros::Time::now();
  arr.header.frame_id = map_frame_;

  particles_publisher_.publish(arr);
}

void
SlamKarto::laserCallback(const sensor_msgs::LaserScan::ConstPtr& scan)
{
  laser_count_++;
  if ((laser_count_ % throttle_scans_) != 0)
    return;

  // Drop scans when user requested that no scans should be added. Can be useful
  // if we have to wait for some external event (e.g., odometry stabilizing).
  if(!config_.add_scans)
    return;

  static ros::Time last_map_update(0,0);

  // Check whether Karto knows about this laser yet
  if(!addLaserToKarto(scan))
  {
    ROS_WARN("Failed to create laser device for %s; discarding scan",
	     scan->header.frame_id.c_str());
    return;
  }

  karto::Pose2 odom_pose;
  if(addScan(scan, odom_pose))
  {
    ROS_INFO("added scan at pose: %.3f %.3f %.3f",
              odom_pose.GetX(),
              odom_pose.GetY(),
              odom_pose.GetHeading());

    //publishGraphVisualization();

    if(!got_map_ || (scan->header.stamp - last_map_update) > map_update_interval_)
    {
      if(updateMap())
      {
        last_map_update = scan->header.stamp;
        got_map_ = true;
        ROS_DEBUG("Updated the map");
      }
    }
  }

  publishParticlesVisualization();
}

bool
SlamKarto::updateMap()
{
  boost::mutex::scoped_lock(map_mutex_);

  karto::OccupancyGridPtr occ_grid = karto::OccupancyGrid::CreateFromScans(
      mapper_->GetAllProcessedScans(), resolution_);

  if(!occ_grid) {
    ROS_ERROR("No occ grid!");
    return false;
  }

  if(!got_map_) {
    map_.map.info.resolution = resolution_;
    map_.map.info.origin.position.x = 0.0;
    map_.map.info.origin.position.y = 0.0;
    map_.map.info.origin.position.z = 0.0;
    map_.map.info.origin.orientation.x = 0.0;
    map_.map.info.origin.orientation.y = 0.0;
    map_.map.info.origin.orientation.z = 0.0;
    map_.map.info.origin.orientation.w = 1.0;
  }

  // Translate to ROS format
  kt_int32s width = occ_grid->GetWidth();
  kt_int32s height = occ_grid->GetHeight();
  karto::Vector2<kt_double> offset = occ_grid->GetCoordinateConverter()->GetOffset();

  if(map_.map.info.width != (unsigned int) width ||
     map_.map.info.height != (unsigned int) height ||
     map_.map.info.origin.position.x != offset.GetX() ||
     map_.map.info.origin.position.y != offset.GetY())
  {
    map_.map.info.origin.position.x = offset.GetX();
    map_.map.info.origin.position.y = offset.GetY();
    map_.map.info.width = width;
    map_.map.info.height = height;
    map_.map.data.resize(map_.map.info.width * map_.map.info.height);
  }

  for (kt_int32s y=0; y<height; y++)
  {
    for (kt_int32s x=0; x<width; x++)
    {
      // Getting the value at position x,y
      kt_int8u value = occ_grid->GetValue(karto::Vector2<kt_int32s>(x, y));

      switch (value)
      {
        case karto::GridStates_Unknown:
          map_.map.data[MAP_IDX(map_.map.info.width, x, y)] = -1;
          break;
        case karto::GridStates_Occupied:
          map_.map.data[MAP_IDX(map_.map.info.width, x, y)] = 100;
          break;
        case karto::GridStates_Free:
          map_.map.data[MAP_IDX(map_.map.info.width, x, y)] = 0;
          break;
        default:
          ROS_WARN("Encountered unknown cell value at %d, %d", x, y);
          break;
      }
    }
  }

  // Set the header information on the map
  map_.map.header.stamp = ros::Time::now();
  map_.map.header.frame_id = map_frame_;

  sst_.publish(map_.map);
  sstm_.publish(map_.map.info);

  return true;
}

bool
SlamKarto::addScan(const sensor_msgs::LaserScan::ConstPtr& scan,
                   karto::Pose2& karto_pose)
{
  if(!getOdomPose(karto_pose, scan->header.stamp))
    return false;

  // Create a vector of doubles for karto
  karto::RangeReadingsList readings;

  /*if (lasers_inverted_[scan->header.frame_id]) {*/
    for(std::vector<float>::const_reverse_iterator it = scan->ranges.rbegin();
      it != scan->ranges.rend();
      ++it)
    {
      readings.Add(*it);
    }
  /*} else {
    for(std::vector<float>::const_iterator it = scan->ranges.begin();
      it != scan->ranges.end();
      ++it)
    {
      readings.Add(*it);
    }
  //}*/

  // Strip trailing / from the name. Internally, Karto seems to do that, too.
  std::string sensor_id = scan->header.frame_id;
  if(sensor_id.at(0) == '/')
    sensor_id = sensor_id.substr(1);

  // Create the Karto identifier of the laser scanner
  karto::Identifier laserId(sensor_id.c_str());

  // create localized range scan
  karto::LocalizedRangeScan* range_scan = new karto::LocalizedRangeScan(laserId, readings);
  range_scan->SetOdometricPose(karto_pose);
  range_scan->SetCorrectedPose(karto_pose);

  // Add the localized range scan to the mapper (or localizer)
  karto::Module* module = config_.update_map
                        ? static_cast<karto::Module*>(mapper_)
                        : static_cast<karto::Module*>(localizer_);

  // Set MinimumTravelDistance to 0 to force processing of this scan
  karto::Parameter<kt_double>* minimumTravelDistance = dynamic_cast< karto::Parameter<kt_double>* >(
    module->GetParameter("MinimumTravelDistance") );
  double oldMinimumTravelDistance = minimumTravelDistance->GetValue();

  bool forcing_update = false;
  if(force_update_ && force_update_time_ <= scan->header.stamp) {
    ROS_DEBUG_STREAM("Forcibly adding laser scan to map " << scan->header.stamp << " (trigger was at " << force_update_time_ << ")");
    module->SetParameters("MinimumTravelDistance", 0.);
    forcing_update = true;
  }

  // Process the laser scan
  boost::mutex::scoped_lock(map_mutex_);
  bool processed;
  if((processed = module->Process(range_scan)))
  {
    ROS_INFO_STREAM((config_.update_map ? "[MAPPER] " : "[LOCALIZER] ")
              << "Pose: " << range_scan->GetOdometricPose()
              << " Corrected Pose: " << range_scan->GetCorrectedPose()
              << (range_scan->IsScanMatched() ? " MATCHED" : " not matched"));
    /*if(!config_.update_map)
        *w1 << range_scan;
    else
           *w2 << range_scan;*/

    if(range_scan->IsScanMatched()) {
        last_corrected_pose_ = range_scan->GetCorrectedPose();

        // Compute the map->odom transform
        tf::Stamped<tf::Pose> odom_to_map;
        try
        {
          tf_.transformPose(
              odom_frame_,
              tf::Stamped<tf::Pose>(
                btTransform(
                  btQuaternion(last_corrected_pose_.GetHeading(), 0, 0),
                  btVector3(last_corrected_pose_.GetX(), last_corrected_pose_.GetY(), 0.0)
                ).inverse(),
                scan->header.stamp,
                base_frame_
              ),
              odom_to_map);
        }
        catch(tf::TransformException e)
        {
          ROS_ERROR("Transform from base_link to odom failed\n");
          odom_to_map.setIdentity();
        }

        // Update the continuous transform
        map_to_odom_mutex_.lock();
        map_to_odom_ = tf::StampedTransform (
                            tf::Transform(tf::Quaternion( odom_to_map.getRotation() ),
                                          tf::Point( odom_to_map.getOrigin().getX(),
                                                     odom_to_map.getOrigin().getY(),
                                                     0. /* never adjust Z */ ) ).inverse(),
                            scan->header.stamp, map_frame_, odom_frame_);
        map_to_odom_mutex_.unlock();

        if(forcing_update) {
          force_update_ = false;

          // Publish precise transform only when we have a forced update
          // Note: we publish the inverse transform because ROS TF hierarchies
          //       must be a tree, i.e., we cannot publish more than one transform
          //       with odom_frame_ as parent.
          publishPreciseTransform(tf::StampedTransform (
                            tf::Transform(tf::Quaternion( odom_to_map.getRotation() ),
                                          tf::Point( odom_to_map.getOrigin().getX(),
                                                     odom_to_map.getOrigin().getY(),
                                                     0. /* never adjust Z */ ) )/*.inverse()*/,
//                            force_update_time_ /*scan->header.stamp*/, map_precise_frame_, odom_frame_));
                            force_update_time_ /*scan->header.stamp*/, odom_frame_, map_precise_frame_));
        }

    } else {
        ROS_INFO("Not adding unmatched scan");
    }
  } else {
    // Karto ignored the scan, e.g., because the distance was too short
  }

  if(forcing_update) {
    // Restore the MinimumTravelDistance
    module->SetParameters("MinimumTravelDistance", oldMinimumTravelDistance);
  }

  // Periodically publish the last-known transform, but fill in the current
  // scan's timestamp regardless of whether it has been processed by the
  // SLAM module.
  map_to_odom_.stamp_ = scan->header.stamp;
  publishTransform();

  return processed;
}

bool
SlamKarto::mapCallback(nav_msgs::GetMap::Request  &req,
                       nav_msgs::GetMap::Response &res)
{
  boost::mutex::scoped_lock(map_mutex_);
  if(got_map_ && map_.map.info.width && map_.map.info.height)
  {
    res = map_;
    return true;
  }
  else
    return false;
}

void
SlamKarto::reconfigurationCallback(karto::KartoConfig &config, uint32_t level)
{
  boost::mutex::scoped_lock(map_mutex_);

  if(config_.add_scans != config.add_scans)
    ROS_INFO("Incoming laser scans are %s.", config.add_scans
             ? "used" : "NOT used (GMapping is paused)");

  bool set_localizer_config = false;
  bool set_localizer_last_corrected_pose  = false;

  // Note: config is the new configuration, config_ the old one.

  if(config_.update_map != config.update_map) {
    ROS_INFO("Map updates are %s.", config.update_map ? "enabled" : "disabled");

    // Set localizer position when switching from mapping to localizing.
    // Further, call setLocalizerConfig() to copy occupancy grid map from
    // mapper to localizer and initialize the probability distribution.
    if(!config.update_map) {
      // Switching from mapper to localizer
      set_localizer_last_corrected_pose = true;
      set_localizer_config = true;

    } else {
      // Switching from localizer to mapper

      // Find the scanners and set their pose
      if(lasers_added_to_karto_.size() != 0) {
        try {
          if(lasers_added_to_karto_.size() > 1)
            ROS_WARN("There's more than one laser scanner, using only first one. FIXME.");

          std::string name = (*lasers_added_to_karto_.begin()).first;
          karto::Sensor* s = karto::SensorRegistry::GetInstance()->GetSensorByName(name.c_str());
          if(!s)
            ROS_ERROR("Cannot get sensor");
          else {
            ROS_INFO_STREAM("Setting mapper position to " << last_corrected_pose_);
            mapper_->LocalizeSensorAt(s, last_corrected_pose_);
          }
        } catch (karto::Exception error) {
          ROS_ERROR_STREAM("Exception in karto (reconf cb): " << error.GetErrorMessage());
        }
      }
    }

  }

  // Changing localizer mode
  if(config_.dead_reckoning != config.dead_reckoning) {
    set_localizer_config = true;
    // If dead-reckoning is being disabled, set the localizer position to the
    // last known position so it can take over from there. If it's being
    // enabled, no start position is required as the particles will be
    // distributed uniformly.
    if(!config.dead_reckoning)
      set_localizer_last_corrected_pose = true;
  }

  config_ = config;

  if(set_localizer_config || set_localizer_last_corrected_pose)
    setLocalizerConfig();

  if(set_localizer_last_corrected_pose) {
    ROS_INFO_STREAM("Using last corrected pose to bootstrap localizer: "
                    << last_corrected_pose_.ToString().ToCString());
    localizer_->SetStartPosition(last_corrected_pose_);
  }
}

void
SlamKarto::goalCallback(const geometry_msgs::PoseStamped& msg)
{
    boost::mutex::scoped_lock(map_mutex_);
    ROS_INFO_STREAM("Using navigation goal to set localizer position: "
                    << msg.pose.position);
    karto::Pose2 pose(msg.pose.position.x, msg.pose.position.y, 0 /*heading*/);
    localizer_->SetStartPosition(pose);
}

void
SlamKarto::forceUpdateCallback(const std_msgs::Header& msg)
{
  force_update_time_ = msg.stamp;
  force_update_ = true;
}

bool
SlamKarto::saveMapperState(karto::SaveMapperState::Request& request,
                           karto::SaveMapperState::Response& response)
{
  boost::mutex::scoped_lock(map_mutex_);

  ROS_INFO_STREAM("Saving mapper state to " << request.filename);
  try {
    if(!karto::mapper::WriteState(mapper_, request.filename.c_str()))
      return false;
  }
  catch (karto::Exception error) {
    ROS_ERROR_STREAM("Exception in karto: " << error.GetErrorMessage());
    return false;
  }
  return true;
}


bool
SlamKarto::loadMapperState(karto::LoadMapperState::Request& request,
                           karto::LoadMapperState::Response& response)
{
  ROS_INFO_STREAM("Loading mapper state from " << request.filename);

  /* scope the map_mutex_ variable */
  {
    boost::mutex::scoped_lock(map_mutex_);
    try {
      mapper_ = karto::mapper::CreateDefaultMapper();
      lasers_added_to_karto_.clear();

      karto::mapper::ReadState(mapper_, request.filename.c_str());

      // When loading the state, laser scanners are added automatically to
      // Karto. In order to find out which ones where added, iterate the list
      // and mark them as added, so they won't get added a second time in the
      // laserCallback().
      karto::List<karto::Identifier> sensorIds =
        mapper_->GetMapperSensorManager()->GetSensorNames();

      karto_forEach(karto::List<karto::Identifier>, &sensorIds) {
        ROS_INFO_STREAM("Loaded a laser scanner description: " << (*iter).GetName());
        lasers_added_to_karto_[(*iter).GetName().ToCString()] = true;
      }

      config_.update_map = false;      // switch to localizer module
      config_.dead_reckoning = true;   // assume position is unknown
      server_.updateConfig(config_);   // publish modified configuration

      setLocalizerConfig();

    } catch(karto::Exception error) {
      ROS_ERROR_STREAM("Exception in karto: " << error.GetErrorMessage());
      return false;
    }
  } // release mutex here as updateMap() will lock it, too.

  // send out map
  updateMap();

  ROS_INFO_STREAM("Mapper state loading complete.");
  return true;
}

void
SlamKarto::setLocalizerConfig()
{
  try {
    karto::OccupancyGridPtr occ_grid =
      karto::OccupancyGrid::CreateFromScans(mapper_->GetAllProcessedScans(), resolution_);
    if(!occ_grid.IsValid()) {
      // This is normal during startup when no scan has been processed yet.
      return;
    }

    if(!localizer_->SetOccupancyGrid(occ_grid.Get())) {
      ROS_ERROR_STREAM("SetOccupancyGrid failed in setLocalizerConfig()");
      return;
    }

    if(config_.dead_reckoning) {
      // use large number of particles for the particle filter algorithm
      ROS_INFO_STREAM("Setting localizer module to dead-reckoning mode");
      localizer_->SetParameters("NumberOfParticles", config_.part_count_dead);
      localizer_->SetParameters("SamplingInterval", 1);  // use all scan points
      localizer_->SetParameters("ParticleDistribution", "Uniform");
      localizer_->SetParameters("LocalizedPoseCriterion", "Best Particle");
    } else {
      ROS_INFO_STREAM("Setting localizer module to normal mode");
      localizer_->SetParameters("NumberOfParticles", config_.part_count);
      localizer_->SetParameters("SamplingInterval", 1);  // use all scan points
      localizer_->SetParameters("ParticleDistribution", "Gauss");
      localizer_->SetParameters("LocalizedPoseCriterion", "Weighted Mean");
      //localizer_->SetParameters("LocalizedPoseCriterion", "Best Particle");
    }

    ROS_INFO_STREAM("Resetting probability distribution");
    localizer_->ResetDistribution();

  } catch (karto::Exception error) {
    ROS_ERROR_STREAM("Exception in karto in setLocalizerConfig(): " << error.GetErrorMessage());
  }
}

int
main(int argc, char** argv)
{
  ros::init(argc, argv, "slam_karto");
  try {
    karto::Environment::Initialize(argc, argv);

    SlamKarto kn;
    ros::spin();

  } catch (karto::Exception error) {
    ROS_ERROR_STREAM("Exception in karto: " << error.GetErrorMessage());
  }

  // After kn has gone out of scope!
  karto::Environment::Terminate();
  ros::shutdown();

  return 0;
}
