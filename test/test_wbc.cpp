#include <boost/test/unit_test.hpp>
#include <kdl/chainfksolverpos_recursive.hpp>
#include <kdl/chainjnttojacsolver.hpp>
#include <kdl_conversions/KDLConversions.hpp>
#include <iostream>
#include "KinematicRobotModelKDL.hpp"
#include "RobotModelConfig.hpp"
#include "Jacobian.hpp"
#include "ConstraintConfig.hpp"
#include "WbcVelocityScene.hpp"

using namespace std;
using namespace wbc;

void pose_diff(const Eigen::Affine3d& a, const Eigen::Affine3d& b, const double dt, base::Vector6d& twist){

    Eigen::Matrix3d rot_mat = a.rotation().inverse() * b.rotation();
    Eigen::AngleAxisd angle_axis;
    angle_axis.fromRotationMatrix(rot_mat);

    twist.segment(0,3) = (b.translation() - a.translation())/dt;
    twist.segment(3,3) = a.rotation() * (angle_axis.axis() * angle_axis.angle())/dt;
}

void pose_diff(const base::samples::RigidBodyState& a, const base::samples::RigidBodyState& b, const double dt, base::Vector6d& twist){
    pose_diff(a.getTransform(), b.getTransform(), dt, twist);
}

BOOST_AUTO_TEST_CASE(test_jacobian)
{
    cout<<"Testing Jacobian ...."<<endl<<endl;

    srand(time(NULL));

    cout<<"Testing change Ref point ..."<<endl<<endl;

    Jacobian jac(7);
    jac.setIdentity();
    jac.changeRefPoint(base::Vector3d(0.1,0.2,0.3));

    KDL::Jacobian jac_kdl(7);
    jac_kdl.data.setIdentity();
    jac_kdl.changeRefPoint(KDL::Vector(0.1,0.2,0.3));

    for(int i = 0; i < 6; i++)
        for(int j = 0; j < 7; j++)
            BOOST_CHECK_EQUAL(jac(i,j), jac_kdl.data(i,j));

    cout<<"Jacobian from WBC is " <<endl;
    cout<<jac<<endl;

    cout<<"Jacobian from KDL is " <<endl;
    cout<<jac_kdl.data<<endl<<endl;

    cout<<"Testing change Ref Frame ..."<<endl<<endl;

    base::Affine3d a;
    a.setIdentity();
    a.translate(base::Vector3d(1,2,3));
    jac.changeRefFrame(a);

    KDL::Frame a_kdl;
    a_kdl.p = KDL::Vector(1,2,3);
    jac_kdl.changeRefFrame(a_kdl);

    for(int i = 0; i < 6; i++)
        for(int j = 0; j < 7; j++)
            BOOST_CHECK_EQUAL(jac(i,j), jac_kdl.data(i,j));

    cout<<"Jacobian from WBC is " <<endl;
    cout<<jac<<endl;

    cout<<"Jacobian from KDL is " <<endl;
    cout<<jac_kdl.data<<endl<<endl;

    cout<<"...done"<<endl<<endl;
}

/*BOOST_AUTO_TEST_CASE(wbc_velocity_scene){

    vector<string> joint_names;
    for(int i = 0; i < 7; i++)
        joint_names.push_back("kuka_lbr_l_joint_" + to_string(i+1));

    // Create WBC config
    vector<ConstraintConfig> wbc_config;

    // Constraint for Cartesian Position Control
    ConstraintConfig cart_constraint;
    cart_constraint.name       = "cart_pos_ctrl_left";
    cart_constraint.type       = cart;
    cart_constraint.priority   = 0;
    cart_constraint.root       = "kuka_lbr_base";
    cart_constraint.tip        = "kuka_lbr_l_tcp";
    cart_constraint.ref_frame  = "kuka_lbr_base";
    cart_constraint.activation = 1;
    cart_constraint.weights    = vector<double>(6,1);
    wbc_config.push_back(cart_constraint);

    // Configure Robot model
    shared_ptr<KinematicRobotModelKDL> robot_model = make_shared<KinematicRobotModelKDL>();
    vector<RobotModelConfig> config(1);
    config[0].file = std::string(getenv("AUTOPROJ_CURRENT_ROOT")) + "/control/wbc/test/data/kuka_lbr.urdf";
    BOOST_CHECK_EQUAL(robot_model->configure(config, joint_names, "kuka_lbr_base"), true);

    // Configure Solver
    shared_ptr<HierarchicalLSSolver> solver = make_shared<HierarchicalLSSolver>();
    BOOST_CHECK_EQUAL(solver->configure(WbcScene::getNConstraintVariablesPerPrio(wbc_config), robot_model->noOfJoints()), true);

    // Configure WBC Scene
    WbcVelocityScene wbc_scene(robot_model);
    BOOST_CHECK_EQUAL(wbc_scene.configure(wbc_config), true);

    // Set reference
    base::samples::RigidBodyState target, ref, act;
    target.time = base::Time::now();
    target.position = base::Vector3d(0.235498, 0.822147, 1.41881);
    target.orientation.setIdentity();
    act.position.setZero();
    act.orientation.setIdentity();

    // Run control loop
    base::samples::Joints joint_state;
    joint_state.resize(7);
    joint_state.names = robot_model->jointNames();
    for(int i = 0; i < 7; i++)
        joint_state[i].position = 0.1;
    joint_state.time = base::Time::now();

    base::VectorXd solver_output;

    double loop_time = 0.1; // seconds
    while((target.position - act.position).norm() > 1e-4){

        // Update robot model
        BOOST_CHECK_NO_THROW(robot_model->update(joint_state));

        act = robot_model->rigidBodyState(cart_constraint.root, cart_constraint.tip);
        base::Vector6d diff;
        pose_diff(act, target, 1, diff);
        ref.velocity = diff.segment(0,3);
        ref.angular_velocity = diff.segment(3,3);
        ref.time = base::Time::now();
        shared_ptr<CartesianVelocityConstraint> constraint = static_pointer_cast<CartesianVelocityConstraint>(wbc_scene.getConstraint("cart_pos_ctrl_left"));
        constraint->setReference(ref);

        // Compute ctrl solution
        wbc_scene.update();
        solver->solve(wbc_scene.getConstraintsByPrio(), solver_output);

        for(size_t i = 0; i < joint_state.size(); i++)
            joint_state[i].position += solver_output(i) * loop_time;

        cout<<"Target: x: "<<target.position(0)<<" y: "<<target.position(1)<<" z: "<<target.position(2)<<endl;
        cout<<"Target: qx: "<<target.orientation.x()<<" qy: "<<target.orientation.y()<<" qz: "<<target.orientation.z()<<" qw: "<<target.orientation.w()<<endl<<endl;

        cout<<"Actual x: "<<act.position(0)<<" y: "<<act.position(1)<<" z: "<<act.position(2)<<endl;
        cout<<"Actual qx: "<<act.orientation.x()<<" qy: "<<act.orientation.y()<<" qz: "<<act.orientation.z()<<" qw: "<<act.orientation.w()<<endl;
        cout<<"---------------------------------------------------------------------------------------------"<<endl<<endl;

        usleep(loop_time * 1e6);
    }
}
*/
