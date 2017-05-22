#ifndef KINEMATIC_CONSTRAINT_KDL_HPP
#define KINEMATIC_CONSTRAINT_KDL_HPP

#include "Constraint.hpp"

#include <kdl/jacobian.hpp>
#include <kdl/chainjnttojacsolver.hpp>

namespace wbc{

class KinematicConstraintKDL : public Constraint{
public:

    KinematicConstraintKDL(const ConstraintConfig& _config,
                       const uint n_robot_joints) :
        Constraint(_config)
    {
        uint no_vars;
        if(_config.type == jnt)
            no_vars = _config.joint_names.size();
        else
            no_vars = 6;
        init(no_vars);
        A.resize(no_variables, n_robot_joints);
        A.setZero();
    }

    ~KinematicConstraintKDL(){
    }

    void init(uint no_vars){
        jac_helper = KDL::Jacobian(no_vars);
        jac_helper.data.setZero();

        H.resize(no_vars,6);
        H.setZero();

        Uf.resize(6, no_vars);
        Uf.setIdentity();

        Vf.resize(no_vars, no_vars);
        Vf.setIdentity();

        Sf.resize(no_vars);
        Sf.setZero();
    }
    /** Pose of the tip of the kinematic chain associated with this constraint expressed in root coordinates */
    KDL::Frame pose_tip_in_root;
    /** Pose of the ref frame of the kinematic chain associated with this constraint expressed in root coordinates */
    KDL::Frame pose_ref_frame_in_root;
    /** Constraint matrix */
    base::MatrixXd A;

    //Helper variables
    KDL::Jacobian jac_helper;
    Eigen::MatrixXd Uf, Vf;
    Eigen::VectorXd Sf;
    Eigen::MatrixXd H;
};
}

#endif