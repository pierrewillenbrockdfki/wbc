#include "KinematicRobotModelKDL.hpp"
#include "KinematicChainKDL.hpp"
#include <kdl_conversions/KDLConversions.hpp>

namespace wbc{

KinematicRobotModelKDL::KinematicRobotModelKDL(const std::vector<std::string> &joint_names, const std::string& base_frame)
    : RobotModel(joint_names, base_frame),
      robot_jacobian(base::MatrixXd(6,joint_names.size())){
}

KinematicRobotModelKDL::~KinematicRobotModelKDL(){
    current_joint_state.clear();
    current_poses.clear();
    full_tree = KDL::Tree();

    KinematicChainKDLMap::const_iterator it;
    for(it = kdl_chain_map.begin(); it != kdl_chain_map.end(); it++)
        delete it->second;
    kdl_chain_map.clear();
}

void KinematicRobotModelKDL::createChain(const std::string &root_frame, const std::string &tip_frame){
    KDL::Chain chain;
    if(!full_tree.getChain(root_frame, tip_frame, chain))
        throw std::invalid_argument("KinematicRobotModelKDL: Unable to extract chain from " + root_frame + " to " + tip_frame + " from KDL Tree");

    KinematicChainKDL* kin_chain = new KinematicChainKDL(chain);
    kin_chain->update(current_joint_state, current_poses);
    kdl_chain_map[root_frame + "_" + tip_frame] = kin_chain;
}

void KinematicRobotModelKDL::addTree(const KDL::Tree& tree, const std::string& hook, const base::samples::RigidBodyState &pose){

    if(full_tree.getNrOfSegments() == 0)
        full_tree = tree;
    else{
        if(hook.empty())
            throw std::invalid_argument("KinematicRobotModelKDL::addTree: Unexpected empty hook name. To which segment do you want to attach the tree?");
        if(!hasFrame(hook))
            throw std::invalid_argument("KinematicRobotModelKDL::addTree: Hook name is " + hook + ", but this segment does not exist in tree");

        std::string root = tree.getRootSegment()->first;
        KDL::Frame pose_kdl;
        kdl_conversions::RigidBodyState2KDL(pose, pose_kdl);
        if(!full_tree.addSegment(KDL::Segment(root, KDL::Joint(KDL::Joint::None), pose_kdl), hook))
            throw std::invalid_argument("Unable to attach segment " + root + " to existing tree segment " + hook);
        if(!full_tree.addTree(tree, root))
            throw std::invalid_argument("Unable to attach tree with root segment " + root);
    }
}

void KinematicRobotModelKDL::update(const base::samples::Joints& joint_state,
                                    const std::vector<base::samples::RigidBodyState>& poses){

    current_joint_state = joint_state;
    current_poses = poses;

    KinematicChainKDLMap::const_iterator it;
    for(it = kdl_chain_map.begin(); it != kdl_chain_map.end(); it++)
        it->second->update(joint_state, poses);
}

const base::samples::RigidBodyState &KinematicRobotModelKDL::rigidBodyState(const std::string &root_frame, const std::string &tip_frame){

    // Create chain if it does not exist
    if(kdl_chain_map.count(root_frame + "_" + tip_frame) == 0)
        createChain(root_frame, tip_frame);

    KinematicChainKDL* kdl_chain = kdl_chain_map[root_frame + "_" + tip_frame];
    if(kdl_chain->last_update.isNull())
        throw std::runtime_error("KinematicRobotModelKDL: You have to call update() at least once before requesting kinematic information!");

    return kdl_chain->rigid_body_state;
}

const base::samples::Joints& KinematicRobotModelKDL::jointState(const std::vector<std::string> &joint_names){

    if(current_joint_state.time.isNull())
        throw std::runtime_error("KinematicRobotModelKDL: You have to call update() at least once before requesting kinematic information!");

    joint_state.resize(joint_names.size());
    joint_state.names = joint_names;
    joint_state.time = current_joint_state.time;

    for(uint i = 0; i < joint_names.size(); i++){
        try{
            joint_state[i] = current_joint_state.getElementByName(joint_names[i]);
        }
        catch(std::exception e){
            throw std::invalid_argument("KinematicRobotModelKDL: Requested state of joint "
                                        + joint_names[i] + " but this joint does not exist in robot model");
        }
    }
    return joint_state;
}

const base::MatrixXd& KinematicRobotModelKDL::jacobian(const std::string &root_frame, const std::string &tip_frame){

    // Create chain if it does not exist
    if(kdl_chain_map.count(root_frame + "_" + tip_frame) == 0)
        createChain(root_frame, tip_frame);

    KinematicChainKDL* kdl_chain = kdl_chain_map[root_frame + "_" + tip_frame];
    if(kdl_chain->last_update.isNull())
        throw std::runtime_error("KinematicRobotModelKDL: You have to call update() at least once before requesting kinematic information!");

    robot_jacobian.setZero(6,noOfJoints());
    for(uint j = 0; j < kdl_chain->joint_names.size(); j++){
        int idx = jointIndex(kdl_chain->joint_names[j]);
        robot_jacobian.col(idx) = kdl_chain->jacobian.data.col(j);
    }
    return robot_jacobian;
}

bool KinematicRobotModelKDL::hasFrame(const std::string &name){
    return full_tree.getSegments().count(name) > 0;
}

std::vector<std::string> KinematicRobotModelKDL::jointNamesFromTree(const KDL::Tree &tree){

    std::vector<std::string> joint_names;
    for (auto& kv : tree.getSegments()) {
        KDL::Joint joint = kv.second.segment.getJoint();
        if(joint.getType() != KDL::Joint::None)
            joint_names.push_back(joint.getName());
    }
    return joint_names;
}

}