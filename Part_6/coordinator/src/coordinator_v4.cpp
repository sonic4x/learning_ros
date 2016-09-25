// coordinator: 
// wsn, Sept, 2016
// illustrates use of object_finder, object_grabber (need  navigator) action servers
// presents an action-server interface to clients
// see example action client: coordinator_action_client.cpp

//OLD:  trigger this process with:
// to find toy block:
//rostopic pub action_codes std_msgs/Int32 100
/// to grab toy block:
// rostopic pub action_codes std_msgs/Int32 101

#include<ros/ros.h>
#include <actionlib/client/simple_action_client.h>
#include <actionlib/client/terminal_state.h>
#include <object_finder/objectFinderAction.h>
#include <object_grabber/object_grabberAction.h>
#include <navigator/navigatorAction.h>
#include <Eigen/Eigen>
#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <std_msgs/UInt32.h>
#include <std_msgs/Int32.h>
#include <coordinator/CoordinatorSrv.h> 
#include <actionlib/server/simple_action_server.h>
//the following #include refers to the "action" message defined for this package
// The action message can be found in: .../example_action_server/action/demo.action
// Automated header generation creates multiple headers for message I/O
// These are referred to by the root name (demo) and appended name (Action)
#include<coordinator/ManipTaskAction.h>
#include <object_manipulation_properties/object_manipulation_properties.h>


class TaskActionServer {
private:
    ros::NodeHandle nh_; // we'll need a node handle; get one upon instantiation
    actionlib::SimpleActionServer<coordinator::ManipTaskAction> as_;

    // here are some message types to communicate with our client(s)
    coordinator::ManipTaskGoal goal_; // goal message, received from client
    coordinator::ManipTaskResult result_; // put results here, to be sent back to the client when done w/ goal
    coordinator::ManipTaskFeedback feedback_; // for feedback 
    actionlib::SimpleActionClient<object_finder::objectFinderAction> object_finder_ac_;
    actionlib::SimpleActionClient<object_grabber::object_grabberAction> object_grabber_ac_;

    object_finder::objectFinderGoal object_finder_goal_;
    object_grabber::object_grabberGoal object_grabber_goal_;
    void objectGrabberDoneCb_(const actionlib::SimpleClientGoalState& state,
            const object_grabber::object_grabberResultConstPtr& result);
    void objectFinderDoneCb_(const actionlib::SimpleClientGoalState& state,
            const object_finder::objectFinderResultConstPtr& result);
    int object_grabber_return_code_; //feedback status from object grabber
    int found_object_code_; //feedback status from object finder

    //the following items are elements of the goal message
    geometry_msgs::PoseStamped pickup_pose_;
    geometry_msgs::PoseStamped dropoff_pose_;
    int goal_action_code_, object_code_, perception_source_;
    int vision_object_code_; //SHOULD be reconciled with ManipTask object codes

    //the following are used for logic in executeCB
    bool working_on_task_; //true as long as goal is still in progress
    int status_code_; //values to be published as feedback during action service
    int action_code_, pickup_action_code_, dropoff_action_code_;
    ros::Publisher pose_publisher_;

public:
    TaskActionServer(); //define the body of the constructor outside of class definition

    ~TaskActionServer(void) {
    }
    // Action Interface
    void executeCB(const actionlib::SimpleActionServer<coordinator::ManipTaskAction>::GoalConstPtr& goal);
};

TaskActionServer::TaskActionServer() :
as_(nh_, "manip_task_action_service", boost::bind(&TaskActionServer::executeCB, this, _1), false),
object_finder_ac_("object_finder_action_service", true),
object_grabber_ac_("object_grabber_action_service", true) {
    ROS_INFO("in constructor of TaskActionServer...");
    // do any other desired initializations here...specific to your implementation
    as_.start(); //start the server running

    action_code_ = coordinator::ManipTaskGoal::NO_CURRENT_TASK;
    status_code_ = coordinator::ManipTaskFeedback::NO_CURRENT_TASK;
    working_on_task_ = false;

    //connect to the object_grabber server
    bool server_exists = false;
    while ((!server_exists)&&(ros::ok())) {
        server_exists = object_grabber_ac_.waitForServer(ros::Duration(0.5)); // 
        ros::spinOnce();
        ros::Duration(0.5).sleep();
        ROS_INFO("retrying...");
    }
    ROS_INFO("connected to object_grabber action server"); // if here, then we connected to the server; 

    ROS_INFO("attempting to connect to object-finder action server");
    server_exists = false;
    while ((!server_exists)&&(ros::ok())) {
        server_exists = object_finder_ac_.waitForServer(ros::Duration(0.5)); // 
        ros::spinOnce();
        ros::Duration(0.5).sleep();
        ROS_INFO("retrying...");
    }
    ROS_INFO("connected to object_finder action server");


    pose_publisher_ = nh_.advertise<geometry_msgs::PoseStamped>("triad_display_pose", 1, true);
}

void TaskActionServer::objectFinderDoneCb_(const actionlib::SimpleClientGoalState& state,
        const object_finder::objectFinderResultConstPtr& result) {
    ROS_INFO(" objectFinderDoneCb: server responded with state [%s]", state.toString().c_str());
    found_object_code_ = result->found_object_code;
    ROS_INFO("got object code response = %d; ", found_object_code_);
    if (found_object_code_ == object_finder::objectFinderResult::OBJECT_CODE_NOT_RECOGNIZED) {
        ROS_WARN("object code not recognized");
    } else if (found_object_code_ == object_finder::objectFinderResult::OBJECT_FOUND) {
        ROS_INFO("found object!");
        pickup_pose_ = result->object_pose;
        ROS_INFO("got pose x,y,z = %f, %f, %f", pickup_pose_.pose.position.x,
                pickup_pose_.pose.position.y,
                pickup_pose_.pose.position.z);
        pose_publisher_.publish(pickup_pose_);
    } else {
        ROS_WARN("object not found!");
    }
}

void TaskActionServer::objectGrabberDoneCb_(const actionlib::SimpleClientGoalState& state,
        const object_grabber::object_grabberResultConstPtr& result) {
    ROS_INFO(" objectGrabberDoneCb: server responded with state [%s]", state.toString().c_str());
    ROS_INFO("got result output = %d; ", result->return_code);
    object_grabber_return_code_ = result->return_code;
}

void TaskActionServer::executeCB(const actionlib::SimpleActionServer<coordinator::ManipTaskAction>::GoalConstPtr& goal) {
    ROS_INFO("in executeCB: received manipulation task");

    goal_action_code_ = goal->action_code; //verbatim from received goal
    action_code_ = goal->action_code; //init: this value changes as state machine advances through steps
    ROS_INFO("requested action code is: %d", goal_action_code_);
    //if action code is "MANIP_OBJECT", need to go through a sequence of action codes
    //otherwise, action code is a simple action code, and can use it as-is
    if (goal_action_code_ == coordinator::ManipTaskGoal::MANIP_OBJECT) {
        //if command is for manip, then we can expect an object code, perception source and dropoff pose
        object_code_ = goal->object_code; //what type of object is this?

        perception_source_ = goal->perception_source; //name sensor or provide coords
        dropoff_pose_ = goal->dropoff_frame;
        ROS_INFO("object code is: %d", object_code_);
        ROS_INFO("perception_source is: %d", goal->perception_source);
        //if (object_code_ == coordinator::ManipTaskGoal::TOY_BLOCK) {
        if (object_code_ == TOY_BLOCK_ID) {            
            vision_object_code_ = object_finder::objectFinderGoal::TOY_BLOCK;
            ROS_INFO("using object-finder object code %d",vision_object_code_);
            pickup_action_code_ = object_grabber::object_grabberGoal::GRAB_TOY_BLOCK;
            dropoff_action_code_ = object_grabber::object_grabberGoal::PLACE_TOY_BLOCK;
            //start the state machine with perceptual processing task:
            action_code_ = coordinator::ManipTaskGoal::GET_PICKUP_POSE; 
        } else {
            ROS_WARN("unknown object type in manipulation action");
            as_.setAborted(result_);
        }
    } else {
        //allow other modes
        //ROS_INFO("sorry--only manipulation mode is implemented");
        //as_.setAborted(result_);
    }

    status_code_ = coordinator::ManipTaskFeedback::RECEIVED_NEW_TASK; //coordinator::ManipTaskFeedback::RECEIVED_NEW_TASK;
    working_on_task_ = true;
    //do work here
    while (working_on_task_) { //coordinator::ManipTaskResult::MANIP_SUCCESS) {
        feedback_.feedback_status = status_code_;
        as_.publishFeedback(feedback_);
        //ROS_INFO("executeCB: status_code = %d", status_code_);
        // each iteration, check if cancellation has been ordered

        if (as_.isPreemptRequested()) {
            ROS_WARN("goal cancelled!");
            result_.manip_return_code = coordinator::ManipTaskResult::ABORTED;
            action_code_ = coordinator::ManipTaskGoal::NO_CURRENT_TASK;
            status_code_ = coordinator::ManipTaskFeedback::ABORTED;
            working_on_task_ = false;
            as_.setAborted(result_); // tell the client we have given up on this goal; send the result message as well
            return; // done with callback
        }
        //here is where we step through states:
        switch (action_code_) {

            case coordinator::ManipTaskGoal::GET_PICKUP_POSE:
                ROS_INFO("establishing pick-up pose");
                if (perception_source_ == coordinator::ManipTaskGoal::BLIND_MANIP) {
                    ROS_INFO("blind manipulation; using provided pick-up pose");
                    pickup_pose_ = goal->pickup_frame;
                    //done with perception, but do fake waiting anyway
                    //declare victor on finding object
                    found_object_code_ = object_finder::objectFinderResult::OBJECT_FOUND;
                    action_code_ = coordinator::ManipTaskGoal::WAIT_FOR_FINDER;
                    status_code_ = coordinator::ManipTaskFeedback::PERCEPTION_BUSY;
                } else if(perception_source_ == coordinator::ManipTaskGoal::PCL_VISION){
                    ROS_INFO("invoking object finder");
                    found_object_code_ = object_finder::objectFinderResult::OBJECT_FINDER_BUSY;
                    ROS_INFO("instructing finder to locate object %d",vision_object_code_);
                    object_finder_goal_.object_id = vision_object_code_;

                    object_finder_goal_.known_surface_ht = false; //require find table height
                    object_finder_goal_.surface_ht = 0.05;

                    ROS_INFO("sending object-finder goal: ");

                    object_finder_ac_.sendGoal(object_finder_goal_,
                            boost::bind(&TaskActionServer::objectFinderDoneCb_, this, _1, _2));

                    action_code_ = coordinator::ManipTaskGoal::WAIT_FOR_FINDER; 
                }
                else {
                    ROS_WARN("unrecognized perception mode; quitting");
                    action_code_ = coordinator::ManipTaskGoal::ABORT;
                    result_.manip_return_code = coordinator::ManipTaskResult::FAILED_PERCEPTION;
                }
                
                ROS_INFO("executeCB: action_code, status_code = %d, %d", action_code_, status_code_);
                break;
                
            case coordinator::ManipTaskGoal::WAIT_FOR_FINDER:
                if (found_object_code_ == object_finder::objectFinderResult::OBJECT_FOUND) {
                    ROS_INFO("object-finder success");
                    //next step: use the pose to grab object:
                    action_code_ = coordinator::ManipTaskGoal::GRAB_OBJECT;
                    status_code_ = coordinator::ManipTaskFeedback::DROPOFF_PLANNING_BUSY;
                    //will later test for result code of object grabber, so initialize it to PENDING
                    object_grabber_return_code_ = object_grabber::object_grabberResult::PENDING;
                } else if (found_object_code_ == object_finder::objectFinderResult::OBJECT_FINDER_BUSY) {
                    ROS_INFO("waiting on perception");
                } else {
                    ROS_WARN("object-finder failure; aborting");
                    action_code_ = coordinator::ManipTaskGoal::ABORT;
                    result_.manip_return_code = coordinator::ManipTaskResult::FAILED_PERCEPTION;
                }
                break;

            case coordinator::ManipTaskGoal::GRAB_OBJECT:
                status_code_ = coordinator::ManipTaskFeedback::PICKUP_MOTION_BUSY;
                ROS_INFO("executeCB: action_code, status_code = %d, %d", action_code_, status_code_);
                ros::Duration(2.0).sleep();
                //if here, then presumably have a valid pose for object of interest; grab it!       
                object_grabber_goal_.action_code = pickup_action_code_; //specify the object to be grabbed 
                object_grabber_goal_.desired_frame = pickup_pose_; //and the object's current pose
                ROS_INFO("sending goal to grab object: ");
                object_grabber_ac_.sendGoal(object_grabber_goal_,
                        boost::bind(&TaskActionServer::objectGrabberDoneCb_, this, _1, _2));

                action_code_ = coordinator::ManipTaskGoal::WAIT_FOR_GRAB_OBJECT;
                status_code_ = coordinator::ManipTaskFeedback::PICKUP_MOTION_BUSY;
                //will inspect this status to see if object grasp is eventually successful
                object_grabber_return_code_ = object_grabber::object_grabberResult::OBJECT_GRABBER_BUSY;

                break;

            case coordinator::ManipTaskGoal::WAIT_FOR_GRAB_OBJECT:
                if (object_grabber_return_code_ == object_grabber::object_grabberResult::OBJECT_ACQUIRED) { //success!
                    ROS_INFO("switch/case happiness!  acquired object");
                    action_code_ = coordinator::ManipTaskGoal::DROPOFF_OBJECT;
                    status_code_ = coordinator::ManipTaskFeedback::PICKUP_SUCCESSFUL;
                } else if (object_grabber_return_code_ == object_grabber::object_grabberResult::OBJECT_GRABBER_BUSY) {
                    // do nothing--just wait patiently
                    //ROS_INFO("waiting for object grab");
                } else {
                    ROS_WARN("trouble with acquiring object");
                    action_code_ = coordinator::ManipTaskGoal::ABORT;
                    result_.manip_return_code = coordinator::ManipTaskResult::FAILED_PICKUP;
                }
                break;

            case coordinator::ManipTaskGoal::DROPOFF_OBJECT:
                status_code_ = coordinator::ManipTaskFeedback::DROPOFF_MOTION_BUSY; //coordinator::ManipTaskResult::MANIP_SUCCESS; //code 0
                ROS_INFO("executeCB: action_code, status_code = %d, %d", action_code_, status_code_);
                object_grabber_goal_.action_code = dropoff_action_code_; //specify the object to be grabbed 
                object_grabber_goal_.desired_frame = dropoff_pose_; //and the object's current pose
                ROS_INFO("sending goal to drop off object: ");
                object_grabber_ac_.sendGoal(object_grabber_goal_,
                        boost::bind(&TaskActionServer::objectGrabberDoneCb_, this, _1, _2));

                action_code_ = coordinator::ManipTaskGoal::WAIT_FOR_DROPOFF_OBJECT;
                //will inspect this status to see if object grasp is eventually successful
                object_grabber_return_code_ = object_grabber::object_grabberResult::OBJECT_GRABBER_BUSY;

                break;


            case coordinator::ManipTaskGoal::WAIT_FOR_DROPOFF_OBJECT:
                //ROS_INFO("object_grabber_return_code_ = %d",object_grabber_return_code_);
                if (object_grabber_return_code_ == object_grabber::object_grabberResult::SUCCESS) { //success!
                    ROS_INFO("switch/case happiness!  dropped off object; manip complete");
                    working_on_task_ = false; // test--set to goal achieved
                    action_code_ = coordinator::ManipTaskGoal::NO_CURRENT_TASK;
                    status_code_ = coordinator::ManipTaskFeedback::COMPLETED_DROPOFF;

                    result_.manip_return_code = coordinator::ManipTaskResult::MANIP_SUCCESS;
                    as_.setSucceeded(result_); // return the "result" message to client, along with "success" status
                    return; //done w/ callback
                } else if (object_grabber_return_code_ == object_grabber::object_grabberResult::OBJECT_GRABBER_BUSY) {
                    // do nothing--just wait patiently
                    //ROS_INFO("waiting for object dropoff");
                } else {
                    ROS_WARN("trouble with acquiring object");
                    action_code_ = coordinator::ManipTaskGoal::ABORT;
                    result_.manip_return_code = coordinator::ManipTaskResult::FAILED_PICKUP;
                }
                break;
                
            case coordinator::ManipTaskGoal::MOVE_TO_PRE_POSE:
                status_code_ = coordinator::ManipTaskFeedback::PREPOSE_MOVE_BUSY;
                object_grabber_goal_.action_code = object_grabber::object_grabberGoal::MOVE_TO_PRE_POSE; //specify the object to be grabbed 
                ROS_INFO("sending goal to move to pre-pose: ");
                object_grabber_ac_.sendGoal(object_grabber_goal_,
                        boost::bind(&TaskActionServer::objectGrabberDoneCb_, this, _1, _2));

                action_code_ = coordinator::ManipTaskGoal::WAIT_FOR_MOVE_TO_PREPOSE;
                //will inspect this status to see if object grasp is eventually successful
                object_grabber_return_code_ = object_grabber::object_grabberResult::OBJECT_GRABBER_BUSY;
                break;
            case coordinator::ManipTaskGoal::WAIT_FOR_MOVE_TO_PREPOSE:    
                if (object_grabber_return_code_ == object_grabber::object_grabberResult::SUCCESS) { //success!
                    ROS_INFO("completed move to pre-pose");
                    working_on_task_ = false; // test--set to goal achieved
                    action_code_ = coordinator::ManipTaskGoal::NO_CURRENT_TASK;
                    status_code_ = coordinator::ManipTaskFeedback::COMPLETED_DROPOFF;
                    result_.manip_return_code = coordinator::ManipTaskResult::MANIP_SUCCESS;
                    as_.setSucceeded(result_); // return the "result" message to client, along with "success" status
                    return; //done w/ callback
                } 
                
                break;

            case coordinator::ManipTaskGoal::ABORT:
                ROS_WARN("aborting goal...");

                result_.manip_return_code = coordinator::ManipTaskResult::ABORTED;
                action_code_ = coordinator::ManipTaskGoal::NO_CURRENT_TASK;
                status_code_ = coordinator::ManipTaskFeedback::ABORTED;
                working_on_task_ = false;
                as_.setAborted(result_); // tell the client we have given up on this goal; send the result message as well
                return; // done with callback

            default:
                ROS_WARN("executeCB: error--case not recognized");
                working_on_task_ = false;
                break;
        }

    }
    ROS_INFO("executeCB: I should not be here...");
    //if we survive to here, then the goal was successfully accomplished; inform the client
    result_.manip_return_code = coordinator::ManipTaskResult::ABORTED;
    as_.setAborted(result_); // return the "result" message to client, along with "success" status
    return;
}

int main(int argc, char** argv) {
    ros::init(argc, argv, "coordinator"); // name this node 
    ros::NodeHandle nh; //standard ros node handle    

    TaskActionServer taskActionServer; //create a task action server

    ROS_INFO("main going into loop");
    while (ros::ok()) {
        ros::spinOnce(); //NEED spins, or action server does not respond
        ros::Duration(0.1).sleep();
    }
    return 0;
}

