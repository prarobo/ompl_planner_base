/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2010, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: Christian Connette, Eitan Marder-Eppstein
*********************************************************************/

#include <ompl_planner_base/ompl_planner_base.h>
#include <costmap_2d/layered_costmap.h>

// pluginlib macros (defines, ...)
#include <pluginlib/class_list_macros.h>


// register this planner as a BaseGlobalPlanner plugin
// (see http://www.ros.org/wiki/pluginlib/Tutorials/Writing%20and%20Using%20a%20Simple%20Plugin)
PLUGINLIB_DECLARE_CLASS(ompl_planner_base, OMPLPlannerBase, ompl_planner_base::OMPLPlannerBase, nav_core::BaseGlobalPlanner)


namespace ompl_planner_base {

  OMPLPlannerBase::OMPLPlannerBase()
    : costmap_ros_(NULL), initialized_(false){}

  OMPLPlannerBase::OMPLPlannerBase(std::string name, costmap_2d::Costmap2DROS* costmap_ros)
    : costmap_ros_(NULL), initialized_(false)
  {
    initialize(name, costmap_ros);
  }

  OMPLPlannerBase::~OMPLPlannerBase()
  {
    delete world_model_;
  }

  
  void OMPLPlannerBase::readParameters()
  {
    private_nh_.param("max_dist_between_pathframes", max_dist_between_pathframes_, 0.10);
    private_nh_.param("max_footprint_cost", max_footprint_cost_, 256);
    private_nh_.param("relative_validity_check_resolution", relative_validity_check_resolution_, 0.004);
    private_nh_.param("interpolate_path", interpolate_path_, true);
    private_nh_.param("solver_maxtime", solver_maxtime_, 1.0);

    // check whether parameters have been set to valid values
    if(max_dist_between_pathframes_ <= 0.0)
    {
      ROS_WARN("Assigned Distance for interpolation of path-frames invalid. Distance must be greater to 0. Distance set to default value: 0.10");
      max_dist_between_pathframes_ = 0.10;
    }
  }

  void OMPLPlannerBase::initialize(std::string name, costmap_2d::Costmap2DROS* costmap_ros)
  {
    if(!initialized_)
    {
      //create Nodehandle
      ros::NodeHandle private_nh("~/" + name);
      private_nh_ = private_nh;

      // advertise topics
      plan_pub_ = private_nh_.advertise<nav_msgs::Path>("plan", 1);

      private_nh_.param("publish_diagnostics", publish_diagnostics_, true);

      if(publish_diagnostics_)
      {
        diagnostic_ompl_pub_ = private_nh_.advertise<ompl_planner_base::OMPLPlannerDiagnostics>("diagnostics_ompl", 1);
        stats_ompl_pub_ = private_nh_.advertise<ompl_planner_base::OMPLPlannerBaseStats>("statistics_ompl", 1);
      }

      // get costmap
      costmap_ros_ = costmap_ros;
      costmap_     = costmap_ros_->getCostmap();
      world_model_ = new base_local_planner::CostmapModel(*costmap_);

      // we'll get the parameters for the robot radius from the costmap we're associated with
      inscribed_radius_     = costmap_ros_->getLayeredCostmap()->getInscribedRadius();
      circumscribed_radius_ = costmap_ros_->getLayeredCostmap()->getCircumscribedRadius();
      footprint_spec_       = costmap_ros_->getRobotFootprint();
      initialized_ = true;
    }
    else{
      ROS_WARN("This planner has already been initialized... doing nothing");
    }
  }



  bool OMPLPlannerBase::makePlan(const geometry_msgs::PoseStamped& start,
                                 const geometry_msgs::PoseStamped& goal,
                                 std::vector<geometry_msgs::PoseStamped>& plan)
  {

    if(!initialized_)
    {
      ROS_ERROR("The planner has not been initialized, please call initialize() to use the planner");
      return false;
    }

    // get parameters from prms-server for planner (robot-geometry + environment are obtained from coastmap)
    readParameters();

    ROS_DEBUG("Got a start: %.2f, %.2f, and a goal: %.2f, %.2f", start.pose.position.x, start.pose.position.y, goal.pose.position.x, goal.pose.position.y);

    // clear path and get up to date copy of costmap
    plan.clear();
    costmap_ = costmap_ros_->getCostmap();

    // make sure goal is set in the same frame, in which the map is set
    if(goal.header.frame_id != costmap_ros_->getGlobalFrameID()){
      ROS_ERROR("This planner as configured will only accept goals in the %s frame, but a goal was sent in the %s frame.",
                costmap_ros_->getGlobalFrameID().c_str(), goal.header.frame_id.c_str());
      return false;
    }

    // instantiate variables for statistics and diagnostics plotting
    ros::Time start_time, end_time;

    ompl_planner_base::OMPLPlannerBaseStats msg_stats_ompl;
    ompl_planner_base::OMPLPlannerDiagnostics msg_diag_ompl;
    start_time = ros::Time::now();

    // create instance of the manifold to plan in -> for mobile base SE2
    // 	ompl::base::StateManifoldPtr manifold(new ompl::base::SE2StateManifold());
    ompl::base::StateSpacePtr manifold(new ompl::base::SE2StateSpace());

    // get bounds from worldmap and set it to bounds for the planner
    // as goal and map are set in same frame (checked above) we can directly get the extensions of the manifold from the map-prms
    ompl::base::RealVectorBounds bounds(2);
    double map_upperbound, map_lowerbound;

    // get bounds for x coordinate
    map_upperbound = costmap_->getSizeInMetersX() - costmap_->getOriginX();
    map_lowerbound = map_upperbound - costmap_->getSizeInMetersX();
    bounds.setHigh(0, map_upperbound);
    bounds.setLow(0, map_lowerbound);
    ROS_INFO("Setting upper and lower bounds of map x-coordinate to (%f, %f).", map_upperbound, map_lowerbound);

    // get bounds for y coordinate
    map_upperbound = costmap_->getSizeInMetersY() - costmap_->getOriginY();
    map_lowerbound = map_upperbound - costmap_->getSizeInMetersY();
    bounds.setHigh(1, map_upperbound);
    bounds.setLow(1, map_lowerbound);
    ROS_INFO("Setting upper and lower bounds of map y-coordinate to (%f, %f).", map_upperbound, map_lowerbound);

    // now set it to the planner
    // 	manifold->as<ompl::base::SE2StateManifold>()->setBounds(bounds);
    manifold->as<ompl::base::SE2StateSpace>()->setBounds(bounds);

    // now create instance to ompl setup
    ompl::geometric::SimpleSetup simple_setup(manifold);

    // set state validity checker
    simple_setup.setStateValidityChecker(boost::bind(&OMPLPlannerBase::isStateValid2DGrid, this, _1));

    // get SpaceInformationPointer from simple_setup (initialized in makePlan routine)
    ompl::base::SpaceInformationPtr si_ptr = simple_setup.getSpaceInformation();

    // set validity checking resolution
    si_ptr->setStateValidityCheckingResolution(relative_validity_check_resolution_);

    // convert start and goal pose from ROS PoseStamped to ompl ScopedState for SE2
    // convert PoseStamped into Pose2D
    geometry_msgs::Pose2D start2D, goal2D;
    convert(start.pose, start2D);
    convert(goal.pose, goal2D);

    // before starting planner -> check whether target configuration is collision-free
    int sample_costs = footprintCost(goal2D);
    if( (sample_costs < 0.0) || (sample_costs > max_footprint_cost_) )
    {
      ROS_ERROR("Collision on target: Planning aborted! Change target position.");
      return false;
    }
    // before starting planner -> check whether start configuration is collision-free
    sample_costs = footprintCost(start2D);
    if( (sample_costs < 0.0) || (sample_costs > max_footprint_cost_) )
    {
      ROS_ERROR("Collision on start: Planning aborted! Free start position.");
      return false;
    }

    if(publish_diagnostics_)
    {
      // set start and end pose, as well as distance between poses
      msg_stats_ompl.start = start.pose;
      msg_stats_ompl.goal = goal.pose;
      double diff_X = goal2D.x - start2D.x;
      double diff_Y = goal2D.y - start2D.y;
      msg_stats_ompl.start_goal_dist = sqrt( diff_X*diff_X + diff_Y*diff_Y);
    }

    // convert Pose2D to ScopedState
    ROS_DEBUG("Converting Start (%f, %f, %f) and Goal State (%f, %f, %f) to ompl ScopedState format",
              start2D.x, start2D.y, start2D.theta,
              goal2D.x,  goal2D.y,  goal2D.theta);

    // create a Scoped State according to above specified Manifold (SE2)
    ompl::base::ScopedState<> ompl_scoped_state_start(manifold);

    // and set this state to the start pose
    convert(ompl_scoped_state_start, start2D);

    // check whether this satisfies the bounds of the manifold
    // 	bool in_bound = manifold->satisfiesBounds(ompl_scoped_state_start->as<ompl::base::SE2StateManifold::StateType>());
    bool in_bound = manifold->satisfiesBounds(ompl_scoped_state_start->as<ompl::base::SE2StateSpace::StateType>());
    if(!in_bound)
    {
      ROS_ERROR("Start Pose lies outside the bounds of the map - Aborting Planer");
      return false;
    }

    // create a Scoped State according to above specified Manifold (SE2)
    ompl::base::ScopedState<> ompl_scoped_state_goal(manifold);

    // and set this state to goal pose
    convert(ompl_scoped_state_goal, goal2D);

    // check whether this satisfies the bounds of the manifold
    // 	inBound = manifold->satisfiesBounds(ompl_scoped_state_goal->as<ompl::base::SE2StateManifold::StateType>());
    in_bound = manifold->satisfiesBounds(ompl_scoped_state_goal->as<ompl::base::SE2StateSpace::StateType>());
    if(!in_bound)
    {
      ROS_ERROR("Target Pose lies outside the bounds of the map - Aborting Planer");
      return false;
    }

    // set start and goal state to planner
    simple_setup.setStartAndGoalStates(ompl_scoped_state_start, ompl_scoped_state_goal);

    // read desired planner-type from parameter server and set according planner to SimpleSetup
    setPlannerType(simple_setup);

    // finally --> plan a path (give ompl 1 second to find a valid path)
    ROS_DEBUG("Requesting Plan");
    bool solved = simple_setup.solve( solver_maxtime_ );

    if(publish_diagnostics_)
    {
      // prepare diagnostic msg -> we do that before simplifying the plan to make sure we get the right computation time
      msg_diag_ompl.summary = solved ? "Planning success" : "Planning Failed";
      msg_diag_ompl.group = "base";
      msg_diag_ompl.planner = planner_type_;
      msg_diag_ompl.result = solved ? "success" : "failed";
      msg_diag_ompl.planning_time = simple_setup.getLastPlanComputationTime();
    }

    if(!solved)
    {
      ROS_WARN("No path found");

      if(publish_diagnostics_)
      {
        // but still publish diagnostics of ompl -> compose msg
        msg_diag_ompl.trajectory_size = 0;
        msg_diag_ompl.trajectory_duration = 0.0; // does not apply

        diagnostic_ompl_pub_.publish(msg_diag_ompl);
      }
      return false;
    }

    // give ompl a chance to simplify the found solution
    simple_setup.simplifySolution();

    // if path found -> get resulting path
    ompl::geometric::PathGeometric ompl_path(simple_setup.getSolutionPath());

    if(publish_diagnostics_)
    {
      // finish composition of msg
      msg_diag_ompl.trajectory_size = ompl_path.getStateCount();
      msg_diag_ompl.trajectory_duration = 0.0; // does not apply
      // publish msg
      diagnostic_ompl_pub_.publish(msg_diag_ompl);
    }

    // convert into vector of pose2D
    ROS_DEBUG("Converting Path from ompl PathGeometric format to vector of PoseStamped");
    std::vector<geometry_msgs::Pose2D> temp_plan_Pose2D;
    const int num_frames_inpath = (int) ompl_path.getStateCount();
    temp_plan_Pose2D.reserve(num_frames_inpath);

    for(int i = 0; i < num_frames_inpath; i++)
    {
      geometry_msgs::Pose2D temp_pose;
      // get frame and tranform it to Pose2D
      convert(ompl_path.getState(i), temp_pose);

      // output states for Debug
      ROS_DEBUG("Coordinates of %dth frame: (x, y, theta) = (%f, %f, %f).", i, temp_pose.x, temp_pose.y, temp_pose.theta);

      // and append them to plan
      temp_plan_Pose2D.push_back(temp_pose);
    }

    if(interpolate_path_)
    {
      ROS_DEBUG("Interpolating path to increase density of frames for local planning");
      // interpolate between frames to meet density requirement of local_planner
      const bool ipo_success = interpolatePathPose2D(temp_plan_Pose2D);
      if(!ipo_success)
      {
        ROS_ERROR("Something went wrong during interpolation. Probably plan empty. Aborting!");
        return false;
      }
      ROS_DEBUG("Interpolated Path has %d frames", num_frames_inpath);
    }

    // convert into vector of PoseStamped
    std::vector<geometry_msgs::PoseStamped> temp_plan;
    geometry_msgs::PoseStamped temp_pose_stamped;

    for(int i = 0; i < num_frames_inpath; i++)
    {
      // set Frame to PoseStamped
      ros::Time plan_time = ros::Time::now();

      // set header
      temp_pose_stamped.header.stamp = plan_time;
      temp_pose_stamped.header.frame_id = costmap_ros_->getGlobalFrameID();

      // convert Pose2D to pose and set to Pose of PoseStamped
      convert(temp_plan_Pose2D[i], temp_pose_stamped.pose);

      // append to plan
      temp_plan.push_back(temp_pose_stamped);
    }

    // done -> pass temp_plan to referenced variable plan ...
    ROS_INFO("Global planning finished: Path Found.");
    plan = temp_plan;

    // publish the plan for visualization purposes ...
    publishPlan(plan);

    if(publish_diagnostics_)
    {
      // compose msg with stats
      msg_stats_ompl.path_length = ompl_path.length();
      // set end time for logging of planner statistics
      end_time = ros::Time::now();
      ros::Duration planning_duration = end_time - start_time;
      msg_stats_ompl.total_planning_time = planning_duration.toSec();
      // publish statistics
      stats_ompl_pub_.publish(msg_stats_ompl);
    }

    // and return with true
    return true;
  }

  double OMPLPlannerBase::footprintCost(const geometry_msgs::Pose2D& pose)
  {
    if(footprint_spec_.size() < 3){
      ROS_ERROR("We have no footprint... do nothing");
      return -1.0;
    }

    // check the pose using the footprint_cost check
    return world_model_->footprintCost(pose.x, pose.y, pose.theta,
                                        footprint_spec_,
                                        inscribed_radius_, circumscribed_radius_ );
  }


  bool OMPLPlannerBase::isStateValid2DGrid(const ompl::base::State *state)
  {
    geometry_msgs::Pose2D checked_state;
    convert(state, checked_state);

    double costs = footprintCost( checked_state );

    return ( (costs >= 0) && (costs < max_footprint_cost_) );
  }


  bool OMPLPlannerBase::interpolatePathPose2D(std::vector<geometry_msgs::Pose2D>& path)
  {
    std::vector<geometry_msgs::Pose2D> ipoPath;
    geometry_msgs::Pose2D last_frame, curr_frame, diff_frame, temp_frame;
    double frame_distance, num_insertions;
    int path_size = path.size();

    // check whether path is correct - at least 2 Elements
    if(path_size < 2)
    {
      ROS_ERROR("Path is not valid. It has only %d Elements. Interpolation not possible. Aborting.", path_size);
      return false;
    }

    // init plan with start frame
    ipoPath.push_back(path[0]);

    // make sure plan is dense enough to be processed by local planner
    for(int i = 1; i < path_size; i++)
    {
      // check wether current frame is close enough to last frame --> otherwise insert interpolated frames
      last_frame = ipoPath[(ipoPath.size()-1)];
      curr_frame = path[i];

      // calc distance between frames
      diff_frame.x = curr_frame.x - last_frame.x;
      diff_frame.y = curr_frame.y - last_frame.y;
      diff_frame.theta = curr_frame.theta - last_frame.theta;
      // normalize angle
      diff_frame.theta = angles::normalize_angle(diff_frame.theta);
      // calulate distance --> following is kind of a heuristic measure, ...
      // ... as it only takes into account the euclidean distance in the cartesian coordinates
      frame_distance = sqrt( diff_frame.x*diff_frame.x + diff_frame.y*diff_frame.y );

      // insert frames until path is dense enough
      if(frame_distance > max_dist_between_pathframes_)
      {
        // just in case --> insert one frame more than neccesarry
        num_insertions = ceil(frame_distance/max_dist_between_pathframes_);
        //ROS_DEBUG("Distance between frames too large (%fm): Inserting %f frames.", frame_distance, num_insertions);
        // n insertions create n+1 intervalls --> add one to division
        diff_frame.x = diff_frame.x/(num_insertions + 1.0);
        diff_frame.y = diff_frame.y/(num_insertions + 1.0);
        diff_frame.theta = diff_frame.theta/(num_insertions + 1.0);
        for(int j = 1; j <= (int)num_insertions; j++)
        {
          temp_frame.x = last_frame.x + j*diff_frame.x;
          temp_frame.y = last_frame.y + j*diff_frame.y;
          temp_frame.theta = last_frame.theta + j*diff_frame.theta;
          // normalize angle
          temp_frame.theta = angles::normalize_angle(temp_frame.theta);

          // append frame to interpolated path
          ipoPath.push_back(temp_frame);
        }
      }

      // finally insert frame from path
      ipoPath.push_back(curr_frame);
    }

    // done --> copy ipoPath to refernce-variable and return with true
    path = ipoPath;

    return true;
  }


  // Configuration

  void OMPLPlannerBase::setPlannerType(ompl::geometric::SimpleSetup& simple_setup)
  {
    // set default planner
    const std::string default_planner("LBKPIECE");

    // read desired planner from parameter server
    private_nh_.param("global_planner_type", planner_type_, default_planner);

    // get SpaceInformationPointer from simple_setup (initialized in makePlan routine)
    const ompl::base::SpaceInformationPtr& si_ptr = simple_setup.getSpaceInformation();

    // init according planner --> this is a little bit arkward, but as there is no switch/case for strings ...

    ompl::base::PlannerPtr target_planner_ptr;

    if(planner_type_.compare("EST") == 0)
    {
      target_planner_ptr = ompl::base::PlannerPtr(new ompl::geometric::EST(si_ptr));
    }
    else if(planner_type_.compare("KPIECE") == 0)
    {
      target_planner_ptr = ompl::base::PlannerPtr(new ompl::geometric::KPIECE1(si_ptr));
    }
    else if(planner_type_.compare("LBKPIECE") == 0)
    {
      target_planner_ptr = ompl::base::PlannerPtr(new ompl::geometric::LBKPIECE1(si_ptr));
    }
    else if(planner_type_.compare("LazyRRT") == 0)
    {
      target_planner_ptr = ompl::base::PlannerPtr(new ompl::geometric::LazyRRT(si_ptr));
    }
    else if(planner_type_.compare("pRRT") == 0)
    {
      target_planner_ptr = ompl::base::PlannerPtr(new ompl::geometric::pRRT(si_ptr));
    }
    else if(planner_type_.compare("RRT") == 0)
    {
      target_planner_ptr = ompl::base::PlannerPtr(new ompl::geometric::RRT(si_ptr));
    }
    else if(planner_type_.compare("RRTConnect") == 0)
    {
      target_planner_ptr = ompl::base::PlannerPtr(new ompl::geometric::RRTConnect(si_ptr));
    }
    else if(planner_type_.compare("pSBL") == 0)
    {
      target_planner_ptr = ompl::base::PlannerPtr(new ompl::geometric::pSBL(si_ptr));
    }
    else if(planner_type_.compare("SBL") == 0)
    {
      target_planner_ptr = ompl::base::PlannerPtr(new ompl::geometric::SBL(si_ptr));
    }
    else{
      ROS_FATAL("The planner named [%s] passed in global_planner_type is not supported", planner_type_.c_str() );
    }

    simple_setup.setPlanner(target_planner_ptr);
  }


  // Visualization
  void OMPLPlannerBase::publishPlan(const std::vector<geometry_msgs::PoseStamped>& path)
  {
    // check whether there really is a path --> given an empty path we won't do anything
    if(path.empty())
    {
      ROS_INFO("Plan is empty - Nothing to display");
      return;
    }

    // create a message for the plan
    nav_msgs::Path gui_path;
    gui_path.poses.resize(path.size());
    gui_path.header.frame_id = path[0].header.frame_id;
    gui_path.header.stamp = path[0].header.stamp;

    // Extract the plan in world co-ordinates, we assume the path is all in the same frame
    for(unsigned int i=0; i < path.size(); i++)
    {
      gui_path.poses[i] = path[i];
    }

    plan_pub_.publish(gui_path);
  }


  // Type Conversions

  void convert(const ompl::base::State* ompl_state, geometry_msgs::Pose2D& pose2D)
  {
    pose2D.x = ompl_state->as<ompl::base::SE2StateSpace::StateType>()->getX();
    pose2D.y = ompl_state->as<ompl::base::SE2StateSpace::StateType>()->getY();
    pose2D.theta = ompl_state->as<ompl::base::SE2StateSpace::StateType>()->getYaw();

    // normalize angle - just in case
    pose2D.theta = angles::normalize_angle(pose2D.theta);
  }


  void convert(const ompl::base::ScopedState<>& scoped_state,
               geometry_msgs::Pose2D& pose2D)
  {
    pose2D.x = scoped_state->as<ompl::base::SE2StateSpace::StateType>()->getX();
    pose2D.y = scoped_state->as<ompl::base::SE2StateSpace::StateType>()->getY();
    pose2D.theta = scoped_state->as<ompl::base::SE2StateSpace::StateType>()->getYaw();

    // normalize angle - just in case
    pose2D.theta = angles::normalize_angle(pose2D.theta);
  }


  void convert(const geometry_msgs::Pose2D& pose2D,
               ompl::base::ScopedState<>& scoped_state)
  {
    scoped_state->as<ompl::base::SE2StateSpace::StateType>()->setX(pose2D.x);
    scoped_state->as<ompl::base::SE2StateSpace::StateType>()->setY(pose2D.y);
    scoped_state->as<ompl::base::SE2StateSpace::StateType>()->setYaw(pose2D.theta);
  }


  void convert(const geometry_msgs::Pose &pose, geometry_msgs::Pose2D& pose2D)
  {
    tf::Pose pose_tf;
    tf::poseMsgToTF(pose, pose_tf);

    double useless_pitch, useless_roll, yaw;
    pose_tf.getBasis().getEulerYPR(yaw, useless_pitch, useless_roll);

    pose2D.x = pose.position.x;
    pose2D.y = pose.position.y;
    pose2D.theta = angles::normalize_angle(yaw);
  }


  void convert(const geometry_msgs::Pose2D& pose2D, geometry_msgs::Pose& pose)
  {
    tf::Quaternion frame_quat = tf::createQuaternionFromYaw(pose2D.theta);

    pose.position.x = pose2D.x;
    pose.position.y = pose2D.y;
    pose.position.z = 0.0;

    pose.orientation.x = frame_quat.x();
    pose.orientation.y = frame_quat.y();
    pose.orientation.z = frame_quat.z();
    pose.orientation.w = frame_quat.w();
  }

}
