/* ----------------------------------------------------------------------------
 * Copyright 2020, Massachusetts Institute of Technology, * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Yulun Tian, et al. (see README for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

#include <DPGO/DPGO_utils.h>
#include <DPGO/PGOAgent.h>
#include <DPGO/QuadraticOptimizer.h>
#include <DPGO/QuadraticProblem.h>

#include <Eigen/CholmodSupport>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>

#include "StieVariable.h"

using std::lock_guard;
using std::unique_lock;
using std::set;
using std::thread;
using std::vector;

namespace DPGO {

PGOAgent::PGOAgent(unsigned ID, const PGOAgentParameters &params)
    : mID(ID), mCluster(ID), d(params.d), r(params.r), n(1),
      mState(PGOAgentState::WAIT_FOR_DATA), mParams(params),
      mInstanceNumber(0), mIterationNumber(0), mNumPosesReceived(0),
      logger(params.logDirectory) {
  if (mParams.verbose) std::cout << mParams << std::endl;
  relativeChanges.assign(mParams.numRobots, 1e3);
  funcDecreases.assign(mParams.numRobots, 1e3);
  // Initialize X
  X = Matrix::Zero(r, d + 1);
  X.block(0, 0, d, d) = Matrix::Identity(d, d);
  if (mID == 0) setLiftingMatrix(fixedStiefelVariable(d, r));
}

PGOAgent::~PGOAgent() {
  // Make sure that optimization thread is not running, before exiting
  endOptimizationLoop();
}

void PGOAgent::setX(const Matrix &Xin) {
  lock_guard<mutex> lock(mPosesMutex);
  X = Xin;
  n = X.cols() / (d + 1);
  assert(X.cols() == n * (d + 1));
  assert(X.rows() == r);
  if (mParams.verbose)
    std::cout << "WARNING: Agent " << mID
              << " resets trajectory. New trajectory length: " << n
              << std::endl;
}

bool PGOAgent::getX(Matrix &Mout) {
  if (mState == PGOAgentState::WAIT_FOR_DATA)
    return false;
  lock_guard<mutex> lock(mPosesMutex);
  Mout = X;
  return true;
}

bool PGOAgent::getXComponent(const unsigned index, Matrix &Mout) {
  if (mState == PGOAgentState::WAIT_FOR_DATA)
    return false;
  lock_guard<mutex> lock(mPosesMutex);
  if (index >= num_poses()) return false;
  Mout = X.block(0, index * (d + 1), r, d + 1);
  return true;
}

void PGOAgent::setLiftingMatrix(const Matrix &M) {
  assert(M.rows() == r);
  assert(M.cols() == d);
  YLift.emplace(M);
}

void PGOAgent::setPoseGraph(
    const std::vector<RelativeSEMeasurement> &inputOdometry,
    const std::vector<RelativeSEMeasurement> &inputPrivateLoopClosures,
    const std::vector<RelativeSEMeasurement> &inputSharedLoopClosures) {
  assert(!isOptimizationRunning());
  assert(mState == PGOAgentState::WAIT_FOR_DATA);
  assert(n == 1);

  if (inputOdometry.empty()) return;

  for (const auto &edge : inputOdometry) {
    addOdometry(edge);
  }
  for (const auto &edge : inputPrivateLoopClosures) {
    addPrivateLoopClosure(edge);
  }
  for (const auto &edge : inputSharedLoopClosures) {
    addSharedLoopClosure(edge);
  }

  mState = PGOAgentState::WAIT_FOR_INITIALIZATION;

  if (mParams.logData) {
    std::vector<RelativeSEMeasurement> measurements = odometry;
    measurements.insert(measurements.end(), privateLoopClosures.begin(), privateLoopClosures.end());
    measurements.insert(measurements.end(), sharedLoopClosures.begin(), sharedLoopClosures.end());
    logger.logMeasurements(measurements, "measurements.csv");
  }


  if (mID == 0) {
    // The first agent can further initialize its trajectory estimate
    Matrix T = localChordalInitialization();
    X = YLift.value() * T;  // Lift to correct relaxation rank
    mState = PGOAgentState::INITIALIZED;

    // Save initial trajectory
    if (mParams.logData) {
      logger.logTrajectory(dimension(), num_poses(), T, "trajectory_initial.csv");
    }
  }
}

void PGOAgent::addOdometry(const RelativeSEMeasurement &factor) {
  // check that this is a odometry measurement
  assert(factor.r1 == mID);
  assert(factor.r2 == mID);
  assert(factor.p1 == n - 1);
  assert(factor.p2 == n);
  assert(factor.R.rows() == d && factor.R.cols() == d);
  assert(factor.t.rows() == d && factor.t.cols() == 1);

  lock_guard<mutex> tLock(mPosesMutex);
  lock_guard<mutex> nLock(mNeighborPosesMutex);

  Matrix X_ = X;
  assert(X_.cols() == (d + 1) * n);
  assert(X_.rows() == r);
  X = Matrix::Zero(r, (d + 1) * (n + 1));
  X.block(0, 0, r, (d + 1) * n) = X_;

  Matrix currR = X.block(0, (n - 1) * (d + 1), r, d);
  Matrix currt = X.block(0, (n - 1) * (d + 1) + d, r, 1);

  // initialize next pose by propagating odometry
  Matrix nextR = currR * factor.R;
  Matrix nextt = currt + currR * factor.t;
  X.block(0, n * (d + 1), r, d) = nextR;
  X.block(0, n * (d + 1) + d, r, 1) = nextt;

  n++;
  assert((d + 1) * n == X.cols());

  lock_guard<mutex> mLock(mMeasurementsMutex);
  odometry.push_back(factor);
}

void PGOAgent::addPrivateLoopClosure(const RelativeSEMeasurement &factor) {
  assert(factor.r1 == mID);
  assert(factor.r2 == mID);
  assert(factor.p1 < n);
  assert(factor.p2 < n);
  assert(factor.R.rows() == d && factor.R.cols() == d);
  assert(factor.t.rows() == d && factor.t.cols() == 1);

  lock_guard<mutex> lock(mMeasurementsMutex);
  privateLoopClosures.push_back(factor);
}

void PGOAgent::addSharedLoopClosure(const RelativeSEMeasurement &factor) {
  assert(factor.R.rows() == d && factor.R.cols() == d);
  assert(factor.t.rows() == d && factor.t.cols() == 1);

  if (factor.r1 == mID) {
    assert(factor.p1 < n);
    assert(factor.r2 != mID);
    mSharedPoses.insert(std::make_pair(mID, factor.p1));
    neighborSharedPoses.insert(std::make_pair(factor.r2, factor.p2));
    neighborAgents.insert(factor.r2);
  } else {
    assert(factor.r2 == mID);
    assert(factor.p2 < n);
    mSharedPoses.insert(std::make_pair(mID, factor.p2));
    neighborSharedPoses.insert(std::make_pair(factor.r1, factor.p1));
    neighborAgents.insert(factor.r1);
  }

  lock_guard<mutex> lock(mMeasurementsMutex);
  sharedLoopClosures.push_back(factor);
}

void PGOAgent::updateNeighborPose(unsigned neighborCluster, unsigned neighborID,
                                  unsigned neighborPose, const Matrix &var) {
  assert(neighborID != mID);
  assert(var.rows() == r);
  assert(var.cols() == d + 1);

  PoseID nID = std::make_pair(neighborID, neighborPose);

  mNumPosesReceived++;

  // Do not store this pose if not needed
  if (neighborSharedPoses.find(nID) == neighborSharedPoses.end()) return;

  // Check if this agent is ready to initialize
  if (mState == PGOAgentState::WAIT_FOR_INITIALIZATION) {
    if (neighborCluster == 0) {
      std::cout << "Agent " << mID << " informed by agent " << neighborID
           << " to initialize! " << std::endl;

      // Require the lifting matrix to initialize
      assert(YLift);

      // Halt optimization
      bool optimizationHalted = false;
      if (isOptimizationRunning()) {
        if (mParams.verbose)
          std::cout << "Agent " << mID << " halt optimization thread..." << std::endl;
        optimizationHalted = true;
        endOptimizationLoop();
      }

      // Halt insertion of new poses
      lock_guard<mutex> tLock(mPosesMutex);
      assert(X.cols() == n * (d + 1));

      // Halt insertion of new measurements
      lock_guard<mutex> mLock(mMeasurementsMutex);

      // Clear cache
      lock_guard<mutex> nLock(mNeighborPosesMutex);
      neighborPoseDict.clear();

      // Find the corresponding inter-robot loop closure
      RelativeSEMeasurement m;
      bool found = findSharedLoopClosure(neighborID, neighborPose, m);
      assert(found);

      // Notations:
      // world1: world frame before alignment
      // world2: world frame after alignment
      // frame1 : frame associated to my public pose
      // frame2 : frame associated to neighbor's public pose
      Matrix dT = Matrix::Identity(d + 1, d + 1);
      dT.block(0, 0, d, d) = m.R;
      dT.block(0, d, d, 1) = m.t;
      Matrix T_world2_frame2 = Matrix::Identity(d + 1, d + 1);
      T_world2_frame2.block(0, 0, d, d + 1) =
          YLift.value().transpose() *
              var;  // Round the received neighbor pose value back to SE(d)
      Matrix T = localChordalInitialization();
      Matrix T_frame1_frame2 = Matrix::Identity(d + 1, d + 1);
      Matrix T_world1_frame1 = Matrix::Identity(d + 1, d + 1);
      if (m.r1 == neighborID) {
        // Incoming edge
        T_frame1_frame2 = dT.inverse();
        T_world1_frame1.block(0, 0, d, d + 1) =
            T.block(0, m.p2 * (d + 1), d, d + 1);
      } else {
        // Outgoing edge
        T_frame1_frame2 = dT;
        T_world1_frame1.block(0, 0, d, d + 1) =
            T.block(0, m.p1 * (d + 1), d, d + 1);
      }
      Matrix T_world2_frame1 = T_world2_frame2 * T_frame1_frame2.inverse();
      Matrix T_world2_world1 = T_world2_frame1 * T_world1_frame1.inverse();

      Matrix T_world1_frame = Matrix::Identity(d + 1, d + 1);
      Matrix T_world2_frame = Matrix::Identity(d + 1, d + 1);
      for (size_t i = 0; i < n; ++i) {
        T_world1_frame.block(0, 0, d, d + 1) =
            T.block(0, i * (d + 1), d, d + 1);
        T_world2_frame = T_world2_world1 * T_world1_frame;
        T.block(0, i * (d + 1), d, d + 1) =
            T_world2_frame.block(0, 0, d, d + 1);
      }

      // Lift back to correct relaxation rank
      X = YLift.value() * T;

      // Mark this agent as initialized
      mCluster = neighborCluster;
      mState = PGOAgentState::INITIALIZED;

      // Log initial trajectory
      if (mParams.logData) {
          logger.logTrajectory(dimension(), num_poses(), T, "trajectory_initial.csv");
      }

      if (optimizationHalted) startOptimizationLoop(rate);
    }
  }

  // Only save poses from neighbors if this agent is initialized
  // and if the sending agent is also initialized
  if (mState == PGOAgentState::INITIALIZED && neighborCluster == 0) {
    lock_guard<mutex> lock(mNeighborPosesMutex);
    neighborPoseDict[nID] = var;
  }
}

bool PGOAgent::getTrajectoryInLocalFrame(Matrix &Trajectory) {
  if (mState != PGOAgentState::INITIALIZED) {
    return false;
  }
  lock_guard<mutex> lock(mPosesMutex);

  Matrix T = X.block(0, 0, r, d).transpose() * X;
  Matrix t0 = T.block(0, d, d, 1);

  for (unsigned i = 0; i < n; ++i) {
    T.block(0, i * (d + 1), d, d) =
        projectToRotationGroup(T.block(0, i * (d + 1), d, d));
    T.block(0, i * (d + 1) + d, d, 1) = T.block(0, i * (d + 1) + d, d, 1) - t0;
  }

  Trajectory = T;
  return true;
}

bool PGOAgent::getTrajectoryInGlobalFrame(Matrix &Trajectory) {
  if (!globalAnchor) return false;
  assert(globalAnchor.value().rows() == relaxation_rank());
  assert(globalAnchor.value().cols() == dimension() + 1);
  if (mState != PGOAgentState::INITIALIZED) return false;
  lock_guard<mutex> lock(mPosesMutex);

  Matrix T = globalAnchor.value().block(0, 0, r, d).transpose() * X;
  Matrix t0 = globalAnchor.value().block(0, 0, r, d).transpose() *
      globalAnchor.value().block(0, d, r, 1);

  for (unsigned i = 0; i < n; ++i) {
    T.block(0, i * (d + 1), d, d) =
        projectToRotationGroup(T.block(0, i * (d + 1), d, d));
    T.block(0, i * (d + 1) + d, d, 1) = T.block(0, i * (d + 1) + d, d, 1) - t0;
  }

  Trajectory = T;
  return true;
}

PoseDict PGOAgent::getSharedPoses() {
  PoseDict map;
  lock_guard<mutex> lock(mPosesMutex);
  for (const auto &mSharedPose : mSharedPoses) {
    unsigned idx = std::get<1>(mSharedPose);
    map[mSharedPose] = X.block(0, idx * (d + 1), r, d + 1);
  }
  return map;
}

std::vector<unsigned> PGOAgent::getNeighborPublicPoses(
    const unsigned &neighborID) const {
  // Check that neighborID is indeed a neighbor of this agent
  assert(neighborAgents.find(neighborID) != neighborAgents.end());
  std::vector<unsigned> poseIndices;
  for (PoseID pair : neighborSharedPoses) {
    if (pair.first == neighborID) {
      poseIndices.push_back(pair.second);
    }
  }
  return poseIndices;
}

std::vector<unsigned> PGOAgent::getNeighbors() const {
  std::vector<unsigned> v(neighborAgents.size());
  std::copy(neighborAgents.begin(), neighborAgents.end(), v.begin());
  return v;
}

void PGOAgent::reset() {
  // Terminate optimization thread if running
  endOptimizationLoop();

  if (mParams.logData) {
    Matrix T;
    if (getTrajectoryInGlobalFrame(T)) {
      logger.logTrajectory(dimension(), num_poses(), T, "trajectory_optimized.csv");
    }
  }

  // Assume that the old lifting matrix can still be used
  mState = PGOAgentState::WAIT_FOR_DATA;

  mInstanceNumber++;
  mIterationNumber = 0;
  mNumPosesReceived = 0;

  relativeChanges.assign(mParams.numRobots, 1e3);
  funcDecreases.assign(mParams.numRobots, 1e3);

  odometry.clear();
  privateLoopClosures.clear();
  sharedLoopClosures.clear();

  neighborPoseDict.clear();
  mSharedPoses.clear();
  neighborSharedPoses.clear();
  neighborAgents.clear();

  n = 1;
  X = Matrix::Zero(r, d + 1);
  X.block(0, 0, d, d) = Matrix::Identity(d, d);

  mCluster = mID;
}

void PGOAgent::preprocess() {
  mIterationNumber++;

  // Save early stopped solution
  if (mIterationNumber == 50 && mParams.logData) {
    Matrix T;
    if (getTrajectoryInGlobalFrame(T)) {
      logger.logTrajectory(dimension(), num_poses(), T, "trajectory_early_stop.csv");
    }
  }
}

void PGOAgent::postprocess() {

}

ROPTResult PGOAgent::optimize() {
  if (mParams.verbose) std::cout << "Agent " << mID << " optimize..." << std::endl;

  if (mState != PGOAgentState::INITIALIZED) {
    if (mParams.verbose)
      std::cout << "Not initialized. Skip optimization!" << std::endl;
    return ROPTResult(false);
  }

  if (odometry.empty()) {
    if (mParams.verbose)
      std::cout << "No odometry measurement. Skip optimization!" << std::endl;
    return ROPTResult(false);
  }

  // lock pose update
  unique_lock<mutex> tLock(mPosesMutex);

  // lock measurements
  unique_lock<mutex> mLock(mMeasurementsMutex);

  // lock neighbor pose update
  unique_lock<mutex> lock(mNeighborPosesMutex);

  // construct data matrices
  auto startTime = std::chrono::high_resolution_clock::now();
  SparseMatrix Q((d + 1) * n, (d + 1) * n);
  SparseMatrix G(r, (d + 1) * n);
  bool success =
      constructCostMatrices(Q, G);
  if (!success) {
    if (mParams.verbose)
      std::cout << "Could not create cost matrices. Skip optimization!"
                << std::endl;
    return ROPTResult(false);
  }

  // Read current estimates of the first k poses
  Matrix Xcurr = X.block(0, 0, r, (d + 1) * n);
  assert(Xcurr.cols() == Q.cols());

  // Construct optimization problem
  QuadraticProblem problem(n, d, r, Q, G);
  double fInit = problem.f(Xcurr);
  double gradNormInit = problem.gradNorm(Xcurr);

  // Initialize optimizer object
  QuadraticOptimizer optimizer(&problem);
  optimizer.setVerbose(mParams.verbose);
  optimizer.setAlgorithm(mParams.algorithm);

  // Optimize
  Matrix Xnext = optimizer.optimize(Xcurr);
  auto counter = std::chrono::high_resolution_clock::now() - startTime;
  double elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(counter).count();
  double fOpt = problem.f(Xnext);
  double gradNormOpt = problem.gradNorm(Xnext);
  assert(fOpt <= fInit);
  // Measure relative change as root mean squared change
  double relchange = sqrt((Xnext - Xcurr).squaredNorm() / n);

  // Update solution
  X = Xnext;
  assert(X.rows() == r);
  assert(X.cols() == (d+1)*n);

  return ROPTResult(true, fInit, gradNormInit, fOpt, gradNormOpt, relchange,
                    elapsedMs);
}

bool PGOAgent::constructCostMatrices(SparseMatrix &Q, SparseMatrix &G) {

  vector<RelativeSEMeasurement> privateMeasurements = odometry;
  privateMeasurements.insert(privateMeasurements.end(), privateLoopClosures.begin(), privateLoopClosures.end());
  vector<RelativeSEMeasurement> sharedMeasurements = sharedLoopClosures;

  // All private measurements appear in the quadratic term
  Q = constructConnectionLaplacianSE(privateMeasurements);

  for (auto m : sharedMeasurements) {
    // Construct relative SE matrix in homogeneous form
    Matrix T = Matrix::Zero(d + 1, d + 1);
    T.block(0, 0, d, d) = m.R;
    T.block(0, d, d, 1) = m.t;
    T(d, d) = 1;

    // Construct aggregate weight matrix
    Matrix Omega = Matrix::Zero(d + 1, d + 1);
    for (unsigned row = 0; row < d; ++row) {
      Omega(row, row) = m.kappa;
    }
    Omega(d, d) = m.tau;

    if (m.r1 == mID) {
      // First pose belongs to this robot
      // Hence, this is an outgoing edge in the pose graph
      assert(m.r2 != mID);

      // Read neighbor's pose
      const PoseID nID = std::make_pair(m.r2, m.p2);
      auto KVpair = neighborPoseDict.find(nID);
      if (KVpair == neighborPoseDict.end()) {
        return false;
      }
      Matrix Xj = KVpair->second;

      // Modify quadratic cost
      size_t idx = m.p1;

      Matrix W = T * Omega * T.transpose();
      for (size_t col = 0; col < d + 1; ++col) {
        for (size_t row = 0; row < d + 1; ++row) {
          Q.coeffRef(idx * (d + 1) + row, idx * (d + 1) + col) += W(row, col);
        }
      }

      // Modify linear cost
      Matrix L = -Xj * Omega * T.transpose();
      for (size_t col = 0; col < d + 1; ++col) {
        for (size_t row = 0; row < r; ++row) {
          G.coeffRef(row, idx * (d + 1) + col) += L(row, col);
        }
      }

    } else {
      // Second pose belongs to this robot
      // Hence, this is an incoming edge in the pose graph
      assert(m.r2 == mID);

      // Read neighbor's pose
      const PoseID nID = std::make_pair(m.r1, m.p1);
      auto KVpair = neighborPoseDict.find(nID);
      if (KVpair == neighborPoseDict.end()) {
        return false;
      }
      Matrix Xi = KVpair->second;

      // Modify quadratic cost
      size_t idx = m.p2;

      for (size_t col = 0; col < d + 1; ++col) {
        for (size_t row = 0; row < d + 1; ++row) {
          Q.coeffRef(idx * (d + 1) + row, idx * (d + 1) + col) +=
              Omega(row, col);
        }
      }

      // Modify linear cost
      Matrix L = -Xi * T * Omega;
      for (size_t col = 0; col < d + 1; ++col) {
        for (size_t row = 0; row < r; ++row) {
          G.coeffRef(row, idx * (d + 1) + col) += L(row, col);
        }
      }
    }
  }

  return true;
}

void PGOAgent::startOptimizationLoop(double freq) {
  if (isOptimizationRunning()) {
    if (mParams.verbose)
      std::cout << "WARNING: optimization thread already running! Skip..." << std::endl;
    return;
  }

  rate = freq;

  mOptimizationThread = new thread(&PGOAgent::runOptimizationLoop, this);
}

void PGOAgent::runOptimizationLoop() {
  if (mParams.verbose)
    std::cout << "Agent " << mID << " optimization thread running at " << rate
         << " Hz." << std::endl;

  // Create exponential distribution with the desired rate
  std::random_device
      rd;  // Will be used to obtain a seed for the random number engine
  std::mt19937 rng(rd());  // Standard mersenne_twister_engine seeded with rd()
  std::exponential_distribution<double> ExponentialDistribution(rate);

  while (true) {
    double sleepUs =
        1e6 * ExponentialDistribution(rng);  // sleeping time in microsecond

    usleep(sleepUs);

    optimize();

    // Check if finish requested
    if (mFinishRequested) {
      break;
    }
  }
}

void PGOAgent::endOptimizationLoop() {
  if (!isOptimizationRunning()) return;

  mFinishRequested = true;

  // wait for thread to finish
  mOptimizationThread->join();

  delete mOptimizationThread;

  mOptimizationThread = nullptr;

  mFinishRequested = false;  // reset request flag

  if (mParams.verbose)
    std::cout << "Agent " << mID << " optimization thread exited. " << std::endl;
}

bool PGOAgent::isOptimizationRunning() {
  return mOptimizationThread != nullptr;
}

bool PGOAgent::findSharedLoopClosure(unsigned neighborID, unsigned neighborPose,
                                     RelativeSEMeasurement &mOut) {
  for (const auto &m : sharedLoopClosures) {
    if ((m.r1 == neighborID && m.p1 == neighborPose) ||
        (m.r2 == neighborID && m.p2 == neighborPose)) {
      mOut = m;
      return true;
    }
  }

  return false;
}

Matrix PGOAgent::localChordalInitialization() {
  std::vector<RelativeSEMeasurement> measurements = odometry;
  measurements.insert(measurements.end(), privateLoopClosures.begin(),
                      privateLoopClosures.end());

  return chordalInitialization(dimension(), num_poses(), measurements);
}

Matrix PGOAgent::localPoseGraphOptimization() {
  std::vector<RelativeSEMeasurement> measurements = odometry;
  measurements.insert(measurements.end(), privateLoopClosures.begin(),
                      privateLoopClosures.end());

  // Compute initialization
  Matrix Tinit = localChordalInitialization();
  assert(Tinit.rows() == d);
  assert(Tinit.cols() == (d + 1) * n);

  // Construct data matrices
  SparseMatrix Q = constructConnectionLaplacianSE(measurements);
  SparseMatrix G(d, (d + 1) * n);  // linear terms should be zero

  // Form optimization problem
  QuadraticProblem problem(n, d, d, Q, G);

  // Initialize optimizer object
  QuadraticOptimizer optimizer(&problem);
  optimizer.setVerbose(mParams.verbose);
  optimizer.setTrustRegionIterations(20);

  // Optimize
  Matrix Topt = optimizer.optimize(Tinit);
  return Topt;
}

bool PGOAgent::getLiftingMatrix(Matrix &M) const {
  assert(mID == 0);
  if (YLift.has_value()) {
    M = YLift.value();
    return true;
  }
  return false;
}

void PGOAgent::setGlobalAnchor(const Matrix &M) {
  assert(M.rows() == relaxation_rank());
  assert(M.cols() == dimension() + 1);
  globalAnchor.emplace(M);
}

bool PGOAgent::shouldTerminate() {
  // terminate if reached maximum iterations
  if (iteration_number() > mParams.maxNumIters) {
    std::cout << "Reached maximum iterations." << std::endl;
    return true;
  }

  // terminate if all agents satisfy relative change condition
  bool relative_change_reached = true;
  for (size_t i = 0; i < relativeChanges.size(); ++i) {
    if (relativeChanges[i] > mParams.relChangeTol) {
      relative_change_reached = false;
      break;
    }
  }
  if (relative_change_reached) {
    std::cout << "Reached relative change stopping condition" << std::endl;
    return true;
  }

  // terminate if all agents satisfy function decrease condition
  bool func_decrease_reached = true;
  for (size_t i = 0; i < funcDecreases.size(); ++i) {
    if (funcDecreases[i] > mParams.funcDecreaseTol) {
      func_decrease_reached = false;
      break;
    }
  }
  if (func_decrease_reached) {
    std::cout << "Reached function decrease stopping condition." << std::endl;
    return true;
  }

  return false;
}

void PGOAgent::resetAcceleration() {
  assert(mParams.acceleration);
  assert(mState == PGOAgentState::INITIALIZED);
  gamma = 0;
  alpha = 0;
  V = X;
  Y = X;
}

}  // namespace DPGO