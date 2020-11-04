/* ----------------------------------------------------------------------------
 * Copyright 2020, Massachusetts Institute of Technology, * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Yulun Tian, et al. (see README for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */
#include <cassert>
#include <DPGO/PGOLogger.h>
#include <Eigen/Geometry>

namespace DPGO {

PGOLogger::PGOLogger(const std::string &logDir) : logDirectory(logDir) {}

PGOLogger::~PGOLogger() = default;

void PGOLogger::logMeasurements(std::vector<RelativeSEMeasurement> &measurements, const std::string &filename) {
  std::ofstream file;
  file.open(logDirectory + filename);
  if (!file.is_open()) return;

  // Insert header row
  file << "robot_src,pose_src,robot_dst,pose_dst,qx,qy,qz,qw,tx,ty,tz,kappa,tau\n";

  for (RelativeSEMeasurement m: measurements) {
    // Convert rotation matrix to quaternion
    Eigen::Matrix3d R = m.R;
    Eigen::Quaternion<double> quat(R);
    file << m.r1 << ",";
    file << m.p1 << ",";
    file << m.r2 << ",";
    file << m.p2 << ",";
    file << quat.x() << ",";
    file << quat.y() << ",";
    file << quat.z() << ",";
    file << quat.w() << ",";
    file << m.t(0) << ",";
    file << m.t(1) << ",";
    file << m.t(2) << ",";
    file << m.kappa << ",";
    file << m.tau << "\n";
  }

  file.close();
}

void PGOLogger::logTrajectory(unsigned int d, unsigned int n, const Matrix &T, const std::string &filename) {
  assert(T.rows() == d);
  assert(T.cols() == (d + 1) * n);
  std::ofstream file;
  file.open(logDirectory + filename);
  if (!file.is_open()) return;

  // Insert header row
  file << "pose_index,qx,qy,qz,qw,tx,ty,tz\n";

  for (size_t i = 0; i < n; ++i) {
    Eigen::Matrix3d R = T.block(0, i * (d + 1), d, d);
    Eigen::Quaternion<double> quat(R);
    Matrix t = T.block(0, i * (d + 1) + d, d, 1);
    file << i << ",";
    file << quat.x() << ",";
    file << quat.y() << ",";
    file << quat.z() << ",";
    file << quat.w() << ",";
    file << t(0) << ",";
    file << t(1) << ",";
    file << t(2) << "\n";
  }

  file.close();
}

}