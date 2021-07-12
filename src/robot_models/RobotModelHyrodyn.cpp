#include "RobotModelHyrodyn.hpp"
#include <base-logging/Logging.hpp>
#include <urdf_parser/urdf_parser.h>
#include <tools/URDFTools.hpp>

namespace wbc{

RobotModelHyrodyn::RobotModelHyrodyn(){
}

RobotModelHyrodyn::~RobotModelHyrodyn(){
}

void RobotModelHyrodyn::clear(){
    joint_state.clear();
    active_contacts.clear();
    contact_points.clear();
    base_frame="";
    gravity = base::Vector3d(0,0,-9.81);
    joint_limits.clear();
    robot_urdf.reset();
    joint_names_floating_base.clear();
    joint_names.clear();
}

bool RobotModelHyrodyn::configure(const RobotModelConfig& cfg){

    clear();

    // 1. Load Robot Model

    if(!cfg.joint_names.empty())
        LOG_WARN("Configured joint names will be ignored! The Hyrodyn based model will get the joint names from submechanism file");
    if(!cfg.actuated_joint_names.empty())
        LOG_WARN("Configured actuated joint names will be ignored! The Hyrodyn based model will get the actuated joint names from submechanism file");


    robot_urdf = urdf::parseURDFFile(cfg.file);
    if(!robot_urdf){
        LOG_ERROR("Unable to parse urdf model from file %s", cfg.file.c_str());
        return false;
    }

    // Blacklist not required joints
    URDFTools::applyJointBlacklist(robot_urdf, cfg.joint_blacklist);

    // Add floating base
    if(cfg.floating_base)
        joint_names_floating_base = URDFTools::addFloatingBaseToURDF(robot_urdf, cfg.world_frame_id);

    URDFTools::jointLimitsFromURDF(robot_urdf, joint_limits);

    TiXmlDocument *doc = urdf::exportURDF(robot_urdf);
    std::string robot_urdf_file = "/tmp/floating_base_model.urdf";
    doc->SaveFile(robot_urdf_file);
    try{
        load_robotmodel(robot_urdf_file, cfg.submechanism_file);
    }
    catch(std::exception e){
        LOG_ERROR_S << "Failed to load hyrodyn model from URDF " << robot_urdf_file <<
                       " and submechanism file " << cfg.submechanism_file << std::endl;
        throw e;
    }

    joint_state.names = jointnames_spanningtree;
    joint_state.elements.resize(jointnames_spanningtree.size());

    joint_names = joint_names_floating_base + jointnames_active;

    if(floating_base_robot){
        base::samples::RigidBodyStateSE3 rbs;
        rbs.pose = cfg.floating_base_state.pose;
        rbs.twist = cfg.floating_base_state.twist;
        rbs.acceleration = cfg.floating_base_state.acceleration;
        rbs.time = base::Time::now();
        rbs.frame_id = cfg.world_frame_id;
        updateFloatingBase(rbs, joint_names_floating_base, joint_state);

    }

    // 2. Verify consistency of URDF and config

    // TODO

    // 3. Create data structures

    jacobian.resize(6,noOfJoints());
    jacobian.setConstant(std::numeric_limits<double>::quiet_NaN());
    base_frame =  robot_urdf->getRoot()->name;
    contact_points = cfg.contact_points;
    joint_space_inertia_mat.resize(noOfJoints(), noOfJoints());
    bias_forces.resize(noOfJoints());
    selection_matrix.resize(noOfActuatedJoints(),noOfJoints());
    selection_matrix.setZero();
    for(int i = 0; i < jointnames_active.size(); i++)
        selection_matrix(i, jointIndex(jointnames_active[i])) = 1.0;

    return true;
}

void RobotModelHyrodyn::update(const base::samples::Joints& joint_state_in,
                               const base::samples::RigidBodyStateSE3& _floating_base_state){

    if(joint_state_in.elements.size() != joint_state_in.names.size()){
        LOG_ERROR_S << "Size of names and size of elements in joint state do not match"<<std::endl;
        throw std::runtime_error("Invalid joint state");
    }

    if(joint_state_in.time.isNull()){
        LOG_ERROR_S << "Joint State does not have a valid timestamp. Or do we have 1970?"<<std::endl;
        throw std::runtime_error("Invalid joint state");
    }

    uint start_idx = 0;
    // Update floating base if available
    if(floating_base_robot){
        updateFloatingBase(_floating_base_state, joint_names_floating_base, joint_state);
        start_idx = 6;
        for(int i = 0; i < 6; i++){
            y(i)   = joint_state[i].position;
            yd(i)   = joint_state[i].speed;
            ydd(i)   = joint_state[i].acceleration;
        }
    }

    // Update independent joints. This assumes that joints 0..5 are the floating base joints
    for( unsigned int i = start_idx; i < jointnames_independent.size(); ++i){
        const std::string& name =  jointnames_independent[i];
        try{
            y[i] = joint_state_in[name].position;
            yd[i] = joint_state_in[name].speed;
            ydd[i] = joint_state_in[name].acceleration;
            Tau_independentjointspace[i] = joint_state_in[name].effort;
        }
        catch(base::samples::Joints::InvalidName e){
            LOG_ERROR_S << "Joint " << name << " is in independent joints of Hyrodyn model, but it is not given in joint state vector" << std::endl;
            throw e;
        }
    }

    // Compute full spanning tree
    calculate_system_state();

    for(size_t i = 0; i < jointnames_spanningtree.size(); i++){
        const std::string &name = jointnames_spanningtree[i];
        joint_state[name].position = Q[i];
        joint_state[name].speed = QDot[i];
        joint_state[name].acceleration = QDDot[i];
    }
    joint_state.time = joint_state_in.time;

    calculate_com_properties();
    com_rbs.frame_id = base_frame;
    com_rbs.pose.position = com;
    com_rbs.pose.orientation.setIdentity();
    com_rbs.twist.linear = com_vel;
    com_rbs.twist.angular.setZero();
    com_rbs.acceleration.linear = com_acc;
    com_rbs.acceleration.angular.setZero();
    com_rbs.time = joint_state.time;
}

const base::samples::Joints& RobotModelHyrodyn::jointState(const std::vector<std::string> &joint_names){

    if(joint_state.time.isNull()){
        LOG_ERROR("RobotModelKDL: You have to call update() with appropriately timestamped joint data at least once before requesting kinematic information!");
        throw std::runtime_error(" Invalid call to jointState()");
    }

    joint_state_out.resize(joint_names.size());
    joint_state_out.names = joint_names;
    joint_state_out.time = joint_state.time;

    for(size_t i = 0; i < joint_names.size(); i++){
        try{
            joint_state_out[i] = joint_state.getElementByName(joint_names[i]);
        }
        catch(std::exception e){
            LOG_ERROR("RobotModelKDL: Requested state of joint %s but this joint does not exist in robot model", joint_names[i].c_str());
            throw std::invalid_argument("Invalid call to jointState()");
        }
    }
    return joint_state_out;
}

const base::samples::RigidBodyStateSE3 &RobotModelHyrodyn::rigidBodyState(const std::string &root_frame, const std::string &tip_frame){

    if(joint_state.time.isNull()){
        LOG_ERROR("RobotModelKDL: You have to call update() with appropriately timestamped joint data at least once before requesting kinematic information!");
        throw std::runtime_error(" Invalid call to rigidBodyState()");
    }

    if(root_frame != base_frame){
        LOG_ERROR_S<<"Requested Forward kinematics computation for kinematic chain "<<root_frame<<"->"<<tip_frame<<" but hyrodyn robot model always requires the root frame to be the root of the full model"<<std::endl;
        throw std::runtime_error("Invalid root frame");
    }

    calculate_forward_kinematics(tip_frame);
    rbs.pose.position        = pose.segment(0,3);
    rbs.pose.orientation     = base::Quaterniond(pose[6],pose[3],pose[4],pose[5]);
    rbs.twist.linear         = twist.segment(3,3);
    rbs.twist.angular        = twist.segment(0,3);
    rbs.acceleration.linear  = spatial_acceleration.segment(3,3);
    rbs.acceleration.angular = spatial_acceleration.segment(0,3);//
    rbs.time                 = joint_state.time;
    rbs.frame_id             = tip_frame;
    return rbs;
}

const base::MatrixXd &RobotModelHyrodyn::spaceJacobian(const std::string &root_frame, const std::string &tip_frame){

    if(joint_state.time.isNull()){
        LOG_ERROR("RobotModelKDL: You have to call update() with appropriately timestamped joint data at least once before requesting kinematic information!");
        throw std::runtime_error(" Invalid call to rigidBodyState()");
    }

    if(!hasLink(root_frame)){
        LOG_ERROR_S << "Request jacobian for " << root_frame << " -> " << tip_frame << " but link " << root_frame << " does not exist in robot model" << std::endl;
        throw std::runtime_error("Invalid call to spaceJacobian()");
    }

    if(!hasLink(tip_frame)){
        LOG_ERROR_S << "Request jacobian for " << root_frame << " -> " << tip_frame << " but link " << tip_frame << " does not exist in robot model" << std::endl;
        throw std::runtime_error("Invalid call to spaceJacobian()");
    }

    if(root_frame != base_frame){
        LOG_ERROR_S<<"Requested Jacobian computation for kinematic chain "<<root_frame<<"->"<<tip_frame<<" but hyrodyn robot model always requires the root frame to be the root of the full model"<<std::endl;
        throw std::runtime_error("Invalid root frame");
    }

    if(floating_base_robot){
        calculate_space_jacobian_actuation_space_including_floatingbase(tip_frame);
        uint n_cols = Jsufb.cols();
        jacobian.block(0,0,3,n_cols) = Jsufb.block(3,0,3,n_cols);
        jacobian.block(3,0,3,n_cols) = Jsufb.block(0,0,3,n_cols);
    }else{
        calculate_space_jacobian_actuation_space(tip_frame);
        uint n_cols = Jsu.cols();
        jacobian.block(0,0,3,n_cols) = Jsu.block(3,0,3,n_cols);
        jacobian.block(3,0,3,n_cols) = Jsu.block(0,0,3,n_cols);
    }

    return jacobian;
}

const base::MatrixXd &RobotModelHyrodyn::bodyJacobian(const std::string &root_frame, const std::string &tip_frame){

    if(joint_state.time.isNull()){
        LOG_ERROR("RobotModelKDL: You have to call update() with appropriately timestamped joint data at least once before requesting kinematic information!");
        throw std::runtime_error(" Invalid call to rigidBodyState()");
    }

    if(!hasLink(root_frame)){
        LOG_ERROR_S << "Request jacobian for " << root_frame << " -> " << tip_frame << " but link " << root_frame << " does not exist in robot model" << std::endl;
        throw std::runtime_error("Invalid call to bodyJacobian()");
    }

    if(!hasLink(tip_frame)){
        LOG_ERROR_S << "Request jacobian for " << root_frame << " -> " << tip_frame << " but link " << tip_frame << " does not exist in robot model" << std::endl;
        throw std::runtime_error("Invalid call to bodyJacobian()");
    }


    if(root_frame != base_frame){
        LOG_ERROR_S<<"Requested Jacobian computation for kinematic chain "<<root_frame<<"->"<<tip_frame<<" but hyrodyn robot model always requires the root frame to be the root of the full model"<<std::endl;
        throw std::runtime_error("Invalid root frame");
    }

    if(floating_base_robot){
        calculate_body_jacobian_actuation_space_including_floatingbase(tip_frame);
        uint n_cols = Jbufb.cols();
        jacobian.block(0,0,3,n_cols) = Jbufb.block(3,0,3,n_cols);
        jacobian.block(3,0,3,n_cols) = Jbufb.block(0,0,3,n_cols);
    }
    else{
        calculate_body_jacobian_actuation_space(tip_frame);
        uint n_cols = Jbu.cols();
        jacobian.block(0,0,3,n_cols) = Jbu.block(3,0,3,n_cols);
        jacobian.block(3,0,3,n_cols) = Jbu.block(0,0,3,n_cols);
    }

    return jacobian;
}

const base::MatrixXd &RobotModelHyrodyn::jacobianDot(const std::string &root_frame, const std::string &tip_frame){

    throw std::runtime_error("Not implemented: jacobianDot has not been implemented for RobotModelHyrodyn");
}

const base::Acceleration &RobotModelHyrodyn::spatialAccelerationBias(const std::string &root_frame, const std::string &tip_frame){
    calculate_spatial_acceleration_bias(tip_frame);
    spatial_acc_bias = base::Acceleration(spatial_acceleration_bias.segment(3,3), spatial_acceleration_bias.segment(0,3));
    return spatial_acc_bias;
}

const base::MatrixXd &RobotModelHyrodyn::jointSpaceInertiaMatrix(){
    if(joint_state.time.isNull()){
        LOG_ERROR("RobotModelKDL: You have to call update() with appropriately timestamped joint data at least once before requesting kinematic information!");
        throw std::runtime_error(" Invalid call to rigidBodyState()");
    }

    calculate_mass_interia_matrix();
    joint_space_inertia_mat = H;
    return joint_space_inertia_mat;
}

const base::VectorXd &RobotModelHyrodyn::biasForces(){
    if(joint_state.time.isNull()){
        LOG_ERROR("RobotModelKDL: You have to call update() with appropriately timestamped joint data at least once before requesting kinematic information!");
        throw std::runtime_error(" Invalid call to rigidBodyState()");
    }

    ydd.setZero(); // TODO: Should be restored after the ID computation
    calculate_inverse_dynamics_independentjointspace();
    bias_forces = Tau_independentjointspace;
    return bias_forces;
}


uint RobotModelHyrodyn::jointIndex(const std::string &joint_name){
    uint idx = std::find(joint_names.begin(), joint_names.end(), joint_name) - joint_names.begin();
    if(idx >= joint_names.size())
        throw std::invalid_argument("Index of joint  " + joint_name + " was requested but this joint is not in robot model");
    return idx;
}

bool RobotModelHyrodyn::hasLink(const std::string &link_name){
    for(auto l  : robot_urdf->links_)
        if(l.second->name == link_name)
            return true;
    return false;
}

bool RobotModelHyrodyn::hasJoint(const std::string &joint_name){
    return std::find(joint_state.names.begin(), joint_state.names.end(), joint_name) != joint_state.names.end();
}

bool RobotModelHyrodyn::hasActuatedJoint(const std::string &joint_name){
    return std::find(jointnames_active.begin(), jointnames_active.end(), joint_name) != jointnames_active.end();
}

}
