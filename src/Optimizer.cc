﻿/**
* This file is part of ORB-SLAM2.
*
* Copyright (C) 2014-2016 Raúl Mur-Artal <raulmur at unizar dot es> (University of Zaragoza)
* For more information see <https://github.com/raulmur/ORB_SLAM2>
*
* ORB-SLAM2 is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM2 is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with ORB-SLAM2. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Optimizer.h"

#include "Thirdparty/g2o/g2o/core/block_solver.h"
#include "Thirdparty/g2o/g2o/core/optimization_algorithm_levenberg.h"
#include "Thirdparty/g2o/g2o/core/optimization_algorithm_dogleg.h"
#include "Thirdparty/g2o/g2o/solvers/linear_solver_eigen.h"
#include "Thirdparty/g2o/g2o/types/types_six_dof_expmap.h"
#include "Thirdparty/g2o/g2o/core/robust_kernel_impl.h"
#include "Thirdparty/g2o/g2o/solvers/linear_solver_dense.h"
#include "Thirdparty/g2o/g2o/types/types_seven_dof_expmap.h"

#include<Eigen/StdVector>

#include "Converter.h"

#include<mutex>

namespace ORB_SLAM2
{

void Optimizer::GlobalBundleAdjustemnt(Map* pMap, int nIterations, bool* pbStopFlag, const unsigned long nLoopKF, const bool bRobust)
{
    vector<KeyFrame*> vpKFs = pMap->GetAllKeyFrames();
    vector<MapPoint*> vpMP = pMap->GetAllMapPoints();
    BundleAdjustment(vpKFs,vpMP,nIterations,pbStopFlag, nLoopKF, bRobust);
}


void Optimizer::BundleAdjustment(const vector<KeyFrame *> &vpKFs, const vector<MapPoint *> &vpMP,
                                 int nIterations, bool* pbStopFlag, const unsigned long nLoopKF, const bool bRobust)
{
    vector<bool> vbNotIncludedMP;
    vbNotIncludedMP.resize(vpMP.size());

    g2o::SparseOptimizer optimizer;
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverEigen<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    optimizer.setAlgorithm(solver);

    if(pbStopFlag)
        optimizer.setForceStopFlag(pbStopFlag);

    long unsigned int maxKFid = 0;

    // Set KeyFrame vertices
    for(size_t i=0; i<vpKFs.size(); i++)
    {
        KeyFrame* pKF = vpKFs[i];
        if(pKF->isBad())
            continue;
        g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
        vSE3->setEstimate(Converter::toSE3Quat(pKF->GetPose()));
        vSE3->setId(pKF->mnId);
        vSE3->setFixed(pKF->mnId==0);
        optimizer.addVertex(vSE3);
        if(pKF->mnId>maxKFid)
            maxKFid=pKF->mnId;
    }

    const float thHuber2D = sqrt(5.99);
    const float thHuber3D = sqrt(7.815);

    // Set MapPoint vertices
    for(size_t i=0; i<vpMP.size(); i++)
    {
        MapPoint* pMP = vpMP[i];
        if(pMP->isBad())
            continue;
        g2o::VertexSBAPointXYZ* vPoint = new g2o::VertexSBAPointXYZ();
        vPoint->setEstimate(Converter::toVector3d(pMP->GetWorldPos()));
        const int id = pMP->mnId+maxKFid+1;
        vPoint->setId(id);
        vPoint->setMarginalized(true);
        optimizer.addVertex(vPoint);

       const map<KeyFrame*,size_t> observations = pMP->GetObservations();

        int nEdges = 0;
        //SET EDGES
        for(map<KeyFrame*,size_t>::const_iterator mit=observations.begin(); mit!=observations.end(); mit++)
        {

            KeyFrame* pKF = mit->first;
            if(pKF->isBad() || pKF->mnId>maxKFid)
                continue;

            nEdges++;

            const cv::KeyPoint &kpUn = pKF->mvKeysUn[mit->second];

            if(pKF->mvuRight[mit->second]<0)
            {
                Eigen::Matrix<double,2,1> obs;
                obs << kpUn.pt.x, kpUn.pt.y;

                g2o::EdgeSE3ProjectXYZ* e = new g2o::EdgeSE3ProjectXYZ();

                e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
                e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(pKF->mnId)));
                e->setMeasurement(obs);
                const float &invSigma2 = pKF->mvInvLevelSigma2[kpUn.octave];
                e->setInformation(Eigen::Matrix2d::Identity()*invSigma2);

                if(bRobust)
                {
                    g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
                    e->setRobustKernel(rk);
                    rk->setDelta(thHuber2D);
                }

                e->fx = pKF->fx;
                e->fy = pKF->fy;
                e->cx = pKF->cx;
                e->cy = pKF->cy;

                optimizer.addEdge(e);
            }
            else
            {
                Eigen::Matrix<double,3,1> obs;
                const float kp_ur = pKF->mvuRight[mit->second];
                obs << kpUn.pt.x, kpUn.pt.y, kp_ur;

                g2o::EdgeStereoSE3ProjectXYZ* e = new g2o::EdgeStereoSE3ProjectXYZ();

                e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
                e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(pKF->mnId)));
                e->setMeasurement(obs);
                const float &invSigma2 = pKF->mvInvLevelSigma2[kpUn.octave];
                Eigen::Matrix3d Info = Eigen::Matrix3d::Identity()*invSigma2;
                e->setInformation(Info);

                if(bRobust)
                {
                    g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
                    e->setRobustKernel(rk);
                    rk->setDelta(thHuber3D);
                }

                e->fx = pKF->fx;
                e->fy = pKF->fy;
                e->cx = pKF->cx;
                e->cy = pKF->cy;
                e->bf = pKF->mbf;

                optimizer.addEdge(e);
            }
        }

        if(nEdges==0)
        {
            optimizer.removeVertex(vPoint);
            vbNotIncludedMP[i]=true;
        }
        else
        {
            vbNotIncludedMP[i]=false;
        }
    }

    // Optimize!
    optimizer.initializeOptimization();
    optimizer.optimize(nIterations);

    // Recover optimized data

    //Keyframes
    for(size_t i=0; i<vpKFs.size(); i++)
    {
        KeyFrame* pKF = vpKFs[i];
        if(pKF->isBad())
            continue;
        g2o::VertexSE3Expmap* vSE3 = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(pKF->mnId));
        g2o::SE3Quat SE3quat = vSE3->estimate();
        if(nLoopKF==0)
        {
            pKF->SetPose(Converter::toCvMat(SE3quat));
        }
        else
        {
            pKF->mTcwGBA.create(4,4,CV_32F);
            Converter::toCvMat(SE3quat).copyTo(pKF->mTcwGBA);
            pKF->mnBAGlobalForKF = nLoopKF;
        }
    }

    //Points
    for(size_t i=0; i<vpMP.size(); i++)
    {
        if(vbNotIncludedMP[i])
            continue;

        MapPoint* pMP = vpMP[i];

        if(pMP->isBad())
            continue;
        g2o::VertexSBAPointXYZ* vPoint = static_cast<g2o::VertexSBAPointXYZ*>(optimizer.vertex(pMP->mnId+maxKFid+1));

        if(nLoopKF==0)
        {
            pMP->SetWorldPos(Converter::toCvMat(vPoint->estimate()));
            pMP->UpdateNormalAndDepth();
        }
        else
        {
            pMP->mPosGBA.create(3,1,CV_32F);
            Converter::toCvMat(vPoint->estimate()).copyTo(pMP->mPosGBA);
            pMP->mnBAGlobalForKF = nLoopKF;
        }
    }

}

int Optimizer::PoseOptimizationNew(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &TemperalMatch)
{
    // cv::RNG rng((unsigned)time(NULL));

    float rp_thres = 0.25;

    g2o::SparseOptimizer optimizer;
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    vSE3->setEstimate(Converter::toSE3Quat(pCurFrame->mTcw));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set MapPoint vertices
    // const int N = pCurFrame->N_s;
    const int N = TemperalMatch.size();

    vector<g2o::EdgeSE3ProjectXYZOnlyPose*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    const float deltaMono = sqrt(rp_thres);

    bool mono = 1; // monocular
    float repro_e = 0;
    std::vector<bool> vIsOutlier(N,false);

    for(int i=0; i<N; i++)
    {
        // if (TemperalMatch[i]==-1)
        //     continue;
        // if (pCurFrame->vSemLabel[i]!=0)
        //     continue;

        if(mono)
        {
            nInitialCorrespondences++;
            vIsOutlier[i] = false;

            Eigen::Matrix<double,2,1> obs;
            const cv::KeyPoint &kpUn = pCurFrame->mvSiftKeys[TemperalMatch[i]]; // i
            obs << kpUn.pt.x, kpUn.pt.y;

            g2o::EdgeSE3ProjectXYZOnlyPose* e = new g2o::EdgeSE3ProjectXYZOnlyPose();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setMeasurement(obs);
            // const float invSigma2 = pCurFrame->mvInvLevelSigma2[kpUn.octave];
            e->setInformation(Eigen::Matrix2d::Identity());

            g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            e->setRobustKernel(rk);
            rk->setDelta(deltaMono);

            e->fx = pCurFrame->fx;
            e->fy = pCurFrame->fy;
            e->cx = pCurFrame->cx;
            e->cy = pCurFrame->cy;

            cv::Mat Xw = pLastFrame->UnprojectStereoSift(TemperalMatch[i],1);
            e->Xw[0] = Xw.at<float>(0);
            e->Xw[1] = Xw.at<float>(1);
            e->Xw[2] = Xw.at<float>(2);

            optimizer.addEdge(e);

            vpEdgesMono.push_back(e);
            vnIndexEdgeMono.push_back(i);

        }

    }


    if(nInitialCorrespondences<3)
        return 0;

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={rp_thres,5.991,5.991,5.991};
    const int its[4]={100,10,10,10};

    int nBad=0;
    for(size_t it=0; it<1; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(pCurFrame->mTcw));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;
        // monocular
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectXYZOnlyPose* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            if(chi2>chi2Mono[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==0)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }

        if(optimizer.edges().size()<5)
            break;
    }

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);
    pCurFrame->SetPose(pose);

    int inliers = nInitialCorrespondences-nBad;
    cout << "(Camera) inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    // cout << "re-projection error from the optimization: " << repro_e << endl;

    return nInitialCorrespondences-nBad;
}

int Optimizer::PoseOptimizationFlow2Cam(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &TemperalMatch, const vector<Eigen::Vector2d> &flo_gt, const vector<double> &e_bef)
{
    float rp_thres = 0.04; // 0.25

    g2o::SparseOptimizer optimizer;
    // optimizer.setVerbose(true);
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);

    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set MapPoint vertices
    const int N = TemperalMatch.size();

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    cv::Mat Init = pCurFrame->mTcw; // initial with camera pose
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set Edge info
    vector<g2o::EdgeSE3ProjectFlow2*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    // parameter for robust function
    const float deltaMono = sqrt(rp_thres);  // 5.991

    bool mono = 1; // monocular
    float repro_e = 0;
    std::vector<bool> vIsOutlier(N);

    for(int i=0; i<N; i++)
    {

        if(mono)
        {
            nInitialCorrespondences++;
            vIsOutlier[i] = false;

            // Set Flow vertices
            g2o::VertexSBAFlow* vFlo = new g2o::VertexSBAFlow();
            Eigen::Matrix<double,3,1> FloD = Converter::toVector3d(pLastFrame->ObtainFlowDepthCamera(TemperalMatch[i],1));
            vFlo->setEstimate(FloD.head(2));
            const int id = i+1;
            vFlo->setId(id);
            vFlo->setMarginalized(true);
            optimizer.addVertex(vFlo);

            Eigen::Matrix<double,2,1> obs_2d;
            const cv::KeyPoint &kpUn = pLastFrame->mvSiftKeys[TemperalMatch[i]];
            obs_2d << kpUn.pt.x, kpUn.pt.y;

            // Set Binary Edges
            g2o::EdgeSE3ProjectFlow2* e = new g2o::EdgeSE3ProjectFlow2();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setMeasurement(obs_2d);
            Eigen::Matrix2d info_flow;
            info_flow << 0.1, 0.0, 0.0, 0.1;
            e->setInformation(Eigen::Matrix2d::Identity()*info_flow);

            g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            e->setRobustKernel(rk);
            rk->setDelta(deltaMono);

            e->fx = pCurFrame->fx;
            e->fy = pCurFrame->fy;
            e->cx = pCurFrame->cx;
            e->cy = pCurFrame->cy;

            e->depth = FloD(2);

            const cv::Mat Rlw = pLastFrame->mTcw.rowRange(0,3).colRange(0,3);
            const cv::Mat Rwl = Rlw.t();
            const cv::Mat tlw = pLastFrame->mTcw.rowRange(0,3).col(3);
            const cv::Mat twl = -Rlw.t()*tlw;
            e->Twl.setIdentity(4,4);
            e->Twl.block(0,0,3,3) = Converter::toMatrix3d(Rwl);
            e->Twl.col(3).head(3) = Converter::toVector3d(twl);

            optimizer.addEdge(e);

            vpEdgesMono.push_back(e);
            vnIndexEdgeMono.push_back(i);

            Eigen::Matrix<double,2,1> obs_flo;
            obs_flo << FloD(0), FloD(1);

            // Set Unary Edges (constraints)
            g2o::EdgeFlowPrior* e_con = new g2o::EdgeFlowPrior();
            e_con->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e_con->setMeasurement(obs_flo);
            Eigen::Matrix2d invSigma2_flo;
            invSigma2_flo << 0.3, 0.0, 0.0, 0.3;
            e_con->setInformation(Eigen::Matrix2d::Identity()*invSigma2_flo);
            optimizer.addEdge(e_con);

        }

    }


    if(nInitialCorrespondences<3)
        return 0;

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={rp_thres,5.991,5.991,5.991}; // {5.991,5.991,5.991,5.991} {4,4,4,4}
    const int its[4]={100,100,100,100};

    int nBad=0;
    cout << endl;
    for(size_t it=0; it<1; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(Init));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;

        // monocular
        // cout << endl << "chi2: " << endl;
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectFlow2* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            // if(chi2>chi2Mono[it])
            //     cout << chi2 << " ";
            // if (i==(iend-1))
            //     cout << endl << endl;

            if(chi2>chi2Mono[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==0)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }


        if(optimizer.edges().size()<5)
            break;
    }

    // *** Recover optimized pose and return number of inliers ***
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);
    pCurFrame->SetPose(pose);

    // cout << "pose after update: " << endl << pose << endl;

    // *** Recover optimized optical flow ***
    // cout << "flow error before and after optimized: " << endl;
    double e_aft_sum = 0.0, e_bef_sum = 0.0;
    for (int i = 0; i < N; ++i)
    {
        g2o::VertexSBAFlow* vFlow = static_cast<g2o::VertexSBAFlow*>(optimizer.vertex(i+1));
        Eigen::Vector2d flo_error = vFlow->estimate() - flo_gt[i];
        e_aft_sum = e_aft_sum + flo_error.norm();
        e_bef_sum = e_bef_sum + e_bef[i];
        // cout << e_bef[i]-flo_error.norm() << endl;
    }
    // cout << "average flow error before and after: " << e_bef_sum/N << " " << e_aft_sum/N << endl;
    int inliers = nInitialCorrespondences-nBad;
    cout << "(Camera) inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    // cout << "re-projection error from the optimization: " << repro_e << endl;

    return nInitialCorrespondences-nBad;
}

cv::Mat Optimizer::PoseOptimizationObj(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &TemperalMatch, const vector<int> &ObjId, float &repro_e)
{

    g2o::SparseOptimizer optimizer;
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    cv::Mat Init = cv::Mat::eye(4,4,CV_32F);
    // cv::Mat Init = (cv::Mat_<float>(4, 4) << 0.99938428, 0.00097664632, -0.035076648, 0.4861393, -0.00095128408, 0.99999928, 0.00073939934, -0.011283636, 0.035077468, -0.00070558861, 0.9993844, -0.47322178, 0, 0, 0, 1);
    // cv::Mat Twc = Converter::toInvMatrix(pCurFrame->mTcw_gt);
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set MapPoint vertices
    const int N = ObjId.size();

    vector<g2o::EdgeSE3ProjectXYZOnlyPose*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    vector<g2o::EdgeStereoSE3ProjectXYZOnlyPose*> vpEdgesStereo;
    vector<size_t> vnIndexEdgeStereo;
    vpEdgesStereo.reserve(N);
    vnIndexEdgeStereo.reserve(N);

    const float deltaMono = sqrt(5.991);

    bool mono = 1; // monocular
    std::vector<bool> vIsOutlier(N);

    for(int i=0; i<N; i++)
    {
        if(TemperalMatch[ObjId[i]]==-1)
            continue;

        if(mono)
        {
            nInitialCorrespondences++;
            vIsOutlier[i] = false;

            Eigen::Matrix<double,2,1> obs;
            const cv::KeyPoint &kpUn = pCurFrame->mvSiftKeys[ObjId[i]];
            obs << kpUn.pt.x, kpUn.pt.y;

            g2o::EdgeSE3ProjectXYZOnlyPose* e = new g2o::EdgeSE3ProjectXYZOnlyPose();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setMeasurement(obs);
            // const float invSigma2 = pFrame->mvInvLevelSigma2[kpUn.octave];
            e->setInformation(Eigen::Matrix2d::Identity());

            g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            e->setRobustKernel(rk);
            rk->setDelta(deltaMono);

            e->fx = pCurFrame->fx;
            e->fy = pCurFrame->fy;
            e->cx = pCurFrame->cx;
            e->cy = pCurFrame->cy;

            cv::Mat Xw = pLastFrame->UnprojectStereoSift(TemperalMatch[ObjId[i]],1);
            e->Xw[0] = Xw.at<float>(0);
            e->Xw[1] = Xw.at<float>(1);
            e->Xw[2] = Xw.at<float>(2);

            optimizer.addEdge(e);

            vpEdgesMono.push_back(e);
            vnIndexEdgeMono.push_back(i);

        }

    }


    // if(nInitialCorrespondences<3)
    //     return cv::Mat::eye(4,4,CV_32F);

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={5.991,5.991,5.991,5.991};
    const int its[4]={10,10,10,10};

    int nBad=0;
    for(size_t it=0; it<4; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(cv::Mat::eye(4,4,CV_32F)));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;
        // monocular
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectXYZOnlyPose* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            if(chi2>chi2Mono[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==3)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }

        if(optimizer.edges().size()<10)
            break;
    }

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);


    // cv::Mat Twc = cv::Mat::eye(4,4,CV_32F);

    // cv::Mat Tcw = pCurFrame->mTcw;
    // cv::Mat R_cw = Tcw.rowRange(0,3).colRange(0,3);
    // cv::Mat t_cw = Tcw.rowRange(0,3).col(3);
    // cv::Mat R_wc = R_cw.t();
    // cv::Mat t_wc = -R_wc*t_cw;

    // cv::Mat tmp_R = Twc.rowRange(0,3).colRange(0,3);
    // R_wc.copyTo(tmp_R);
    // cv::Mat tmp_t = Twc.rowRange(0,3).col(3);
    // t_wc.copyTo(tmp_t);

    // pose = Twc*pose;

    int inliers = nInitialCorrespondences-nBad;
    cout << endl;
    cout << "(OBJ)inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    // cout << "Object Motion: " << endl << pose << endl;
    repro_e = repro_e/inliers;
    cout << "re-projection error from the optimization: " << repro_e << endl;

    return pose;
}

cv::Mat Optimizer::PoseOptimizationObjTest(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &ObjId)
{

    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(true);
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set MapPoint vertices
    const int N = ObjId.size();

    // ************************** preconditioning *****************************

    // (1) compute center location (in {o}) of the two point clouds of cur and pre frames.
    cv::Mat NewCentre = (cv::Mat_<float>(3,1) << 0, 0, 0);
    for(int i=0; i<N; i++)
    {
        cv::Mat Xp = pLastFrame->UnprojectStereoObject(ObjId[i],1);
        cv::Mat Xc = pCurFrame->UnprojectStereoObject(ObjId[i],1);
        NewCentre.at<float>(0) = NewCentre.at<float>(0) + Xp.at<float>(0) + Xc.at<float>(0);
        NewCentre.at<float>(1) = NewCentre.at<float>(1) + Xp.at<float>(1) + Xc.at<float>(1);
        NewCentre.at<float>(2) = NewCentre.at<float>(2) + Xp.at<float>(2) + Xc.at<float>(2);
    }

    // (2) construct preconditioning coordinate ^{o}T_{p}
    cv::Mat Twp = cv::Mat::eye(4,4,CV_32F);
    Twp.at<float>(0,3)=NewCentre.at<float>(0)/(2*N);
    Twp.at<float>(1,3)=NewCentre.at<float>(1)/(2*N);
    Twp.at<float>(2,3)=NewCentre.at<float>(2)/(2*N);

    cout << "the preconditioning coordinate: " << endl << Twp << endl;

    const cv::Mat Twp_inv = Converter::toInvMatrix(Twp);
    const cv::Mat R_wp_inv = Twp_inv.rowRange(0,3).colRange(0,3);
    const cv::Mat t_wp_inv = Twp_inv.rowRange(0,3).col(3);

    // ************************************************************************

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    cv::Mat Init = cv::Mat::eye(4,4,CV_32F); // initial with identity matrix
    // cv::Mat Init = (pCurFrame->mTcw_gt)*Twp;
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set Edge info
    vector<g2o::EdgeSE3ProjectXYZOnlyPose*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    // ******* Add hard constraint for object motion ******
    // cv::Mat KK = cv::Mat::eye(4,4,CV_32F);

    // KK.at<float>(0,0) = pCurFrame->fx;
    // KK.at<float>(1,1) = pCurFrame->fy;
    // KK.at<float>(0,2) = pCurFrame->cx;
    // KK.at<float>(1,2) = pCurFrame->cy;

    // KK = KK*pCurFrame->mTcw_gt*Twp;
    // ****************************************************

    // parameter for robust function
    const float deltaMono = sqrt(4);  // 5.991, 3.24

    bool mono = 1; // monocular
    float repro_e = 0;
    std::vector<bool> vIsOutlier(N);

    for(int i=0; i<N; i++)
    {

        if(mono)
        {
            nInitialCorrespondences++;
            vIsOutlier[i] = false;

            Eigen::Matrix<double,2,1> obs;
            const cv::KeyPoint &kpUn = pCurFrame->mvObjKeys[ObjId[i]];
            obs << kpUn.pt.x, kpUn.pt.y;

            g2o::EdgeSE3ProjectXYZOnlyPose* e = new g2o::EdgeSE3ProjectXYZOnlyPose();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setMeasurement(obs);
            // const float invSigma2 = pFrame->mvInvLevelSigma2[kpUn.octave];
            // const float invSigma2 = 1.0;
            e->setInformation(Eigen::Matrix2d::Identity());

            g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            e->setRobustKernel(rk);
            rk->setDelta(deltaMono);

            e->fx = pCurFrame->fx;
            e->fy = pCurFrame->fy;
            e->cx = pCurFrame->cx;
            e->cy = pCurFrame->cy;
            // e->fx = KK.at<float>(0,0);
            // e->fy = KK.at<float>(1,1);
            // e->cx = KK.at<float>(0,2);
            // e->cy = KK.at<float>(1,2);

            cv::Mat Xw = pLastFrame->UnprojectStereoObject(ObjId[i],1);
            // *** transfer to preconditioning coordinate ***
            // Xw = R_wp_inv*Xw+t_wp_inv;
            // cout << Xw.at<float>(0) << " " << Xw.at<float>(1) << " " << Xw.at<float>(2) << endl;
            // **********************************************
            e->Xw[0] = Xw.at<float>(0);
            e->Xw[1] = Xw.at<float>(1);
            e->Xw[2] = Xw.at<float>(2);

            optimizer.addEdge(e);

            vpEdgesMono.push_back(e);
            vnIndexEdgeMono.push_back(i);

        }

    }


    if(nInitialCorrespondences<3)
        return cv::Mat::eye(4,4,CV_32F);

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={4,4,4,4}; // {5.991,5.991,5.991,5.991}
    const int its[4]={10,10,10,10};

    int nBad=0;
    cout << endl;
    for(size_t it=0; it<4; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(cv::Mat::eye(4,4,CV_32F)));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;

        // monocular
        cout << endl << "chi2: " << endl;
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectXYZOnlyPose* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            if(chi2>chi2Mono[it])
                cout << chi2 << " ";
            if (i==(iend-1))
                cout << endl << endl;

            if(chi2>chi2Mono[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==3)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }


        if(optimizer.edges().size()<5)
            break;
    }

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);

    int inliers = nInitialCorrespondences-nBad;
    cout << endl;
    cout << "(OBJ)inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    cout << "re-projection error from the optimization: " << repro_e << endl;

    return pose;
}

cv::Mat Optimizer::PoseOptimizationObjMot(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &ObjId, const cv::Point2f flo_co)
{

    g2o::SparseOptimizer optimizer;
    // optimizer.setVerbose(true);
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set MapPoint vertices
    const int N = ObjId.size();

    // ************************** preconditioning *****************************
    // (1) compute center location (in {o}) of the two point clouds of cur and pre frames.
    cv::Mat NewCentre = (cv::Mat_<float>(3,1) << 0, 0, 0);
    for(int i=0; i<N; i++)
    {
        cv::Mat Xp = pLastFrame->UnprojectStereoObject(ObjId[i],1);
        cv::Mat Xc = pCurFrame->UnprojectStereoObject(ObjId[i],1);
        NewCentre.at<float>(0) = NewCentre.at<float>(0) + Xp.at<float>(0) + Xc.at<float>(0);
        NewCentre.at<float>(1) = NewCentre.at<float>(1) + Xp.at<float>(1) + Xc.at<float>(1);
        NewCentre.at<float>(2) = NewCentre.at<float>(2) + Xp.at<float>(2) + Xc.at<float>(2);

    }

    // (2) construct preconditioning coordinate ^{o}T_{p}
    cv::Mat Twp = cv::Mat::eye(4,4,CV_32F);
    Twp.at<float>(0,3)=NewCentre.at<float>(0)/(2*N);
    Twp.at<float>(1,3)=NewCentre.at<float>(1)/(2*N);
    Twp.at<float>(2,3)=NewCentre.at<float>(2)/(2*N);

    // cout << "the preconditioning coordinate: " << endl << Twp << endl;

    const cv::Mat Twp_inv = Converter::toInvMatrix(Twp);
    const cv::Mat R_wp_inv = Twp_inv.rowRange(0,3).colRange(0,3);
    const cv::Mat t_wp_inv = Twp_inv.rowRange(0,3).col(3);
    // ************************************************************************

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    // cv::Mat Init = cv::Mat::eye(4,4,CV_32F); // initial with identity matrix
    cv::Mat Init = Converter::toInvMatrix(pCurFrame->mTcw)*pCurFrame->mInitModel; // initial with identity matrix
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set Edge info
    vector<g2o::EdgeSE3ProjectXYZOnlyObjMotion*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    // Set Projection Matrix
    Eigen::Matrix<double, 3, 4> KK, PP;
    KK << pCurFrame->fx, 0, pCurFrame->cx, 0, 0, pCurFrame->fy, pCurFrame->cy, 0, 0, 0, 1, 0;
    PP = KK*Converter::toMatrix4d(pCurFrame->mTcw); // *Converter::toMatrix4d(Twp)
    // cout << "PP: " << endl << PP << endl;

    // parameter for robust function
    // const float deltaMono = sqrt(4);  // 5.991

    bool mono = 1; // monocular
    float repro_e = 0;
    std::vector<bool> vIsOutlier(N);

    for(int i=0; i<N; i++)
    {

        if(mono)
        {
            nInitialCorrespondences++;
            vIsOutlier[i] = false;

            Eigen::Matrix<double,2,1> obs;
            const cv::KeyPoint &kpUn = pCurFrame->mvObjKeys[ObjId[i]];
            obs << kpUn.pt.x, kpUn.pt.y;

            g2o::EdgeSE3ProjectXYZOnlyObjMotion* e = new g2o::EdgeSE3ProjectXYZOnlyObjMotion();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setMeasurement(obs);
            // const float invSigma2 = 1.0;
            Eigen::Matrix2d invSigma2;
            // invSigma2 << 1.0/flo_co.x, 0, 0, 1.0/flo_co.y;
            invSigma2 << 1.0, 0, 0, 1.0;
            e->setInformation(Eigen::Matrix2d::Identity()*invSigma2);

            // g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            // e->setRobustKernel(rk);
            // rk->setDelta(deltaMono);

            // add projection matrix
            e->P = PP;

            cv::Mat Xw = pLastFrame->UnprojectStereoObject(ObjId[i],0);
            // transfer to preconditioning coordinate
            // Xw = R_wp_inv*Xw+t_wp_inv;

            e->Xw[0] = Xw.at<float>(0);
            e->Xw[1] = Xw.at<float>(1);
            e->Xw[2] = Xw.at<float>(2);

            optimizer.addEdge(e);

            vpEdgesMono.push_back(e);
            vnIndexEdgeMono.push_back(i);

        }

    }


    if(nInitialCorrespondences<3)
        return cv::Mat::eye(4,4,CV_32F);

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={0.09,5.991,5.991,5.991}; // {5.991,5.991,5.991,5.991} {4,4,4,4}
    const int its[4]={100,100,100,100};

    int nBad=0;
    cout << endl;
    for(size_t it=0; it<1; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(Init));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;

        // monocular
        // cout << endl << "chi2: " << endl;
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectXYZOnlyObjMotion* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            // if(chi2>chi2Mono[it])
            //     cout << chi2 << " ";
            // if (i==(iend-1))
            //     cout << endl << endl;

            if(chi2>chi2Mono[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==0)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }


        if(optimizer.edges().size()<5)
            break;
    }

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);

    int inliers = nInitialCorrespondences-nBad;
    // cout << endl;
    cout << "(OBJ)inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    // cout << "re-projection error from the optimization: " << repro_e << endl;

    return pose; // Twp*pose*Twp_inv
}

cv::Mat Optimizer::PoseOptimizationObjMotTLS(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &ObjId)
{

    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(true);
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    // g2o::OptimizationAlgorithmDogleg* solver = new g2o::OptimizationAlgorithmDogleg(solver_ptr);
    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set MapPoint vertices
    const int N = ObjId.size();

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    cv::Mat Init = pCurFrame->mTcw_gt; // initial with camera pose
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set Edge info
    vector<g2o::EdgeSE3ProjectXYZ*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    // parameter for robust function
    // const float deltaMono = sqrt(4);  // 5.991

    bool mono = 1; // monocular
    float repro_e = 0;
    std::vector<bool> vIsOutlier(N);

    for(int i=0; i<N; i++)
    {

        if(mono)
        {
            nInitialCorrespondences++;
            vIsOutlier[i] = false;

            // Set MapPoint vertices
            g2o::VertexSBAPointXYZ* vPoint = new g2o::VertexSBAPointXYZ();
            vPoint->setEstimate(Converter::toVector3d(pLastFrame->UnprojectStereoObject(ObjId[i],1)));
            const int id = i+1;
            vPoint->setId(id);
            vPoint->setMarginalized(true);
            optimizer.addVertex(vPoint);

            Eigen::Matrix<double,2,1> obs_2d;
            const cv::KeyPoint &kpUn = pCurFrame->mvObjKeys[ObjId[i]];
            obs_2d << kpUn.pt.x, kpUn.pt.y;

            // Set Binary Edges
            g2o::EdgeSE3ProjectXYZ* e = new g2o::EdgeSE3ProjectXYZ();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setMeasurement(obs_2d);
            Eigen::Matrix2d info_2d;
            info_2d << 17.6, 0, 0, 80.5;
            e->setInformation(Eigen::Matrix2d::Identity()*info_2d);
            // const float invSigma2_2d = 120;
            // e->setInformation(Eigen::Matrix2d::Identity()*invSigma2_2d);

            // g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            // e->setRobustKernel(rk);
            // rk->setDelta(deltaMono);

            e->fx = pCurFrame->fx;
            e->fy = pCurFrame->fy;
            e->cx = pCurFrame->cx;
            e->cy = pCurFrame->cy;

            optimizer.addEdge(e);

            vpEdgesMono.push_back(e);
            vnIndexEdgeMono.push_back(i);

            Eigen::Matrix<double,3,1> obs_3d;
            cv::Mat Xw = pLastFrame->UnprojectStereoObject(ObjId[i],1);
            obs_3d << Xw.at<float>(0), Xw.at<float>(1), Xw.at<float>(2);

            // Set Unary Edges (constraints)
            g2o::EdgeXYZPrior* e_con = new g2o::EdgeXYZPrior();

            e_con->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e_con->setMeasurement(obs_3d);
            // Eigen::Matrix3d info_3d;
            // info_3d << 299.1, 0, 0, 0, 54.9, 0, 0, 0, 531.4;
            // info_3d << 1, 0, 0, 0, 1, 0, 0, 0, 1;
            // e_con->setInformation(Eigen::Matrix3d::Identity()*info_3d);
            const float invSigma2_3d = 531.3777;
            e_con->setInformation(Eigen::Matrix3d::Identity()*invSigma2_3d);

            optimizer.addEdge(e_con);

        }

    }


    if(nInitialCorrespondences<3)
        return cv::Mat::eye(4,4,CV_32F);

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={5.991,5.991,5.991,5.991}; // {5.991,5.991,5.991,5.991} {4,4,4,4}
    const int its[4]={500,100,100,100};

    int nBad=0;
    cout << endl;
    for(size_t it=0; it<1; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(Init));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;

        // monocular
        cout << endl << "chi2: " << endl;
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectXYZ* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            if(chi2>chi2Mono[it])
                cout << chi2 << " ";
            if (i==(iend-1))
                cout << endl << endl;

            if(chi2>chi2Mono[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==0)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }


        if(optimizer.edges().size()<5)
            break;
    }

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);

    int inliers = nInitialCorrespondences-nBad;
    cout << endl;
    cout << "(OBJ)inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    cout << "re-projection error from the optimization: " << repro_e << endl;

    return pose;
}

cv::Mat Optimizer::PoseOptimizationFlowDepth(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &ObjId)
{

    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(true);
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);

    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set MapPoint vertices
    const int N = ObjId.size();

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    cv::Mat Init = pCurFrame->mTcw_gt; // initial with camera pose
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set Edge info
    vector<g2o::EdgeSE3ProjectFlowDepth*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    // parameter for robust function
    const float deltaMono = sqrt(4);  // 5.991

    bool mono = 1; // monocular
    float repro_e = 0;
    std::vector<bool> vIsOutlier(N);

    for(int i=0; i<N; i++)
    {

        if(mono)
        {
            nInitialCorrespondences++;
            vIsOutlier[i] = false;

            // Set Flow and Depth vertices
            g2o::VertexSBAFlowDepth* vFloD = new g2o::VertexSBAFlowDepth();
            vFloD->setEstimate(Converter::toVector3d(pLastFrame->ObtainFlowDepthObject(ObjId[i],1)));
            const int id = i+1;
            vFloD->setId(id);
            vFloD->setMarginalized(true);
            optimizer.addVertex(vFloD);

            Eigen::Matrix<double,2,1> obs_2d;
            const cv::KeyPoint &kpUn = pCurFrame->mvObjKeys[ObjId[i]];
            obs_2d << kpUn.pt.x, kpUn.pt.y;

            // Set Binary Edges
            g2o::EdgeSE3ProjectFlowDepth* e = new g2o::EdgeSE3ProjectFlowDepth();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setMeasurement(obs_2d);
            const float invSigma2_2d = 1.0;
            e->setInformation(Eigen::Matrix2d::Identity()*invSigma2_2d);

            g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            e->setRobustKernel(rk);
            rk->setDelta(deltaMono);

            e->fx = pCurFrame->fx;
            e->fy = pCurFrame->fy;
            e->cx = pCurFrame->cx;
            e->cy = pCurFrame->cy;

            e->meas = obs_2d;

            const cv::Mat Rlw = pLastFrame->mTcw_gt.rowRange(0,3).colRange(0,3);
            const cv::Mat Rwl = Rlw.t();
            const cv::Mat tlw = pLastFrame->mTcw_gt.rowRange(0,3).col(3);
            const cv::Mat twl = -Rlw.t()*tlw;
            e->Twl.setIdentity(4,4);
            e->Twl.block(0,0,3,3) = Converter::toMatrix3d(Rwl);
            e->Twl.col(3).head(3) = Converter::toVector3d(twl);

            optimizer.addEdge(e);

            vpEdgesMono.push_back(e);
            vnIndexEdgeMono.push_back(i);

            Eigen::Matrix<double,3,1> obs_3d;
            cv::Mat Xw = pLastFrame->ObtainFlowDepthObject(ObjId[i],1);
            obs_3d << Xw.at<float>(0), Xw.at<float>(1), Xw.at<float>(2);

            // Set Unary Edges (constraints)
            g2o::EdgeFlowDepthPrior* e_con = new g2o::EdgeFlowDepthPrior();

            e_con->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e_con->setMeasurement(obs_3d);
            const float invSigma2_3d = 1.0;
            e_con->setInformation(Eigen::Matrix3d::Identity()*invSigma2_3d);

            optimizer.addEdge(e_con);

        }

    }


    if(nInitialCorrespondences<3)
        return cv::Mat::eye(4,4,CV_32F);

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={4,4,4,4}; // {5.991,5.991,5.991,5.991}
    const int its[4]={100,100,100,100};

    int nBad=0;
    cout << endl;
    for(size_t it=0; it<4; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(Init));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;

        // monocular
        cout << endl << "chi2: " << endl;
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectFlowDepth* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            if(chi2>chi2Mono[it])
                cout << chi2 << " ";
            if (i==(iend-1))
                cout << endl << endl;

            if(chi2>chi2Mono[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==3)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }


        if(optimizer.edges().size()<5)
            break;
    }

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);

    int inliers = nInitialCorrespondences-nBad;
    cout << endl;
    cout << "(OBJ)inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    cout << "re-projection error from the optimization: " << repro_e << endl;

    return pose;
}

cv::Mat Optimizer::PoseOptimizationFlowDepth2(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &ObjId)
{

    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(true);
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);

    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set MapPoint vertices
    const int N = ObjId.size();

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    cv::Mat Init = pCurFrame->mTcw_gt; // initial with camera pose
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set Edge info
    vector<g2o::EdgeSE3ProjectFlowDepth2*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    // parameter for robust function
    // const float deltaMono = sqrt(4);  // 5.991

    bool mono = 1; // monocular
    float repro_e = 0;
    std::vector<bool> vIsOutlier(N);

    for(int i=0; i<N; i++)
    {

        if(mono)
        {
            nInitialCorrespondences++;
            vIsOutlier[i] = false;

            // Set Flow and Depth vertices
            g2o::VertexSBAFlowDepth* vFloD = new g2o::VertexSBAFlowDepth();
            Eigen::Matrix<double,3,1> floD;
            cv::Mat Xw = pLastFrame->ObtainFlowDepthObject(ObjId[i],1);
            floD << Xw.at<float>(0), Xw.at<float>(1), Xw.at<float>(2);
            vFloD->setEstimate(floD);
            const int id = i+1;
            vFloD->setId(id);
            vFloD->setMarginalized(true);
            optimizer.addVertex(vFloD);

            Eigen::Matrix<double,2,1> obs_2d;
            const cv::KeyPoint &kpUn = pLastFrame->mvObjKeys[ObjId[i]];
            obs_2d << kpUn.pt.x, kpUn.pt.y;

            // Set Binary Edges
            g2o::EdgeSE3ProjectFlowDepth2* e = new g2o::EdgeSE3ProjectFlowDepth2();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setMeasurement(obs_2d);
            Eigen::Matrix2d invSigma2_2d;
            invSigma2_2d << 12, 0, 0, 196;
            e->setInformation(Eigen::Matrix2d::Identity()*invSigma2_2d);
            // const float invSigma2_2d = 1.0;
            // e->setInformation(Eigen::Matrix2d::Identity()*invSigma2_2d);

            // g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            // e->setRobustKernel(rk);
            // rk->setDelta(deltaMono);

            e->fx = pCurFrame->fx;
            e->fy = pCurFrame->fy;
            e->cx = pCurFrame->cx;
            e->cy = pCurFrame->cy;

            const cv::Mat Rlw = pLastFrame->mTcw_gt.rowRange(0,3).colRange(0,3);
            const cv::Mat Rwl = Rlw.t();
            const cv::Mat tlw = pLastFrame->mTcw_gt.rowRange(0,3).col(3);
            const cv::Mat twl = -Rlw.t()*tlw;
            e->Twl.setIdentity(4,4);
            e->Twl.block(0,0,3,3) = Converter::toMatrix3d(Rwl);
            e->Twl.col(3).head(3) = Converter::toVector3d(twl);

            optimizer.addEdge(e);

            vpEdgesMono.push_back(e);
            vnIndexEdgeMono.push_back(i);

            // Set Unary Edges (constraints)
            g2o::EdgeFlowDepthPrior* e_con = new g2o::EdgeFlowDepthPrior();
            e_con->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e_con->setMeasurement(floD);
            Eigen::Matrix<double,3,3> info = Eigen::Matrix3d::Identity();
            info(0,0) = 1.0;
            info(1,1) = 1.0;
            info(2,2) = floD(2)*0.01;
            e_con->setInformation(info);
            optimizer.addEdge(e_con);

            // // Set Unary Depth Edges (constraints)
            // g2o::EdgeDepthPrior* e_depth = new g2o::EdgeDepthPrior();
            // e_depth->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            // e_depth->setMeasurement(depth);
            // Eigen::Matrix<double, 1, 1> info_depth(10);
            // e_depth->setInformation(info_depth);
            // optimizer.addEdge(e_depth);

        }

    }


    if(nInitialCorrespondences<3)
        return cv::Mat::eye(4,4,CV_32F);

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={4,4,4,4}; // {5.991,5.991,5.991,5.991}
    const int its[4]={200,100,100,100};

    int nBad=0;
    cout << endl;
    for(size_t it=0; it<1; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(Init));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;

        // monocular
        cout << endl << "chi2: " << endl;
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectFlowDepth2* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            if(chi2>chi2Mono[it])
                cout << chi2 << " ";
            if (i==(iend-1))
                cout << endl << endl;

            if(chi2>chi2Mono[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==0)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }


        if(optimizer.edges().size()<5)
            break;
    }

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);

    int inliers = nInitialCorrespondences-nBad;
    cout << endl;
    cout << "(OBJ)inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    cout << "re-projection error from the optimization: " << repro_e << endl;

    return pose;
}

cv::Mat Optimizer::PoseOptimizationFlowDepth3(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &ObjId)
{

    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(true);
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    // g2o::OptimizationAlgorithmDogleg* solver = new g2o::OptimizationAlgorithmDogleg(solver_ptr);

    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set MapPoint vertices
    const int N = ObjId.size();

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    cv::Mat Init = pCurFrame->mTcw_gt; // initial with camera pose
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set Edge info
    vector<g2o::EdgeSE3ProjectFlowDepth3*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    // parameter for robust function
    // const float deltaMono = sqrt(5.991);  // 5.991  4

    bool mono = 1; // monocular
    float repro_e = 0;
    std::vector<bool> vIsOutlier(N);

    for(int i=0; i<N; i++)
    {

        if(mono)
        {
            nInitialCorrespondences++;
            vIsOutlier[i] = false;

            Eigen::Matrix<double,3,1> FloD = Converter::toVector3d(pLastFrame->ObtainFlowDepthObject(ObjId[i],1));

            // Set Flow Vertices
            g2o::VertexSBAFlow* vFlo = new g2o::VertexSBAFlow();
            vFlo->setEstimate(FloD.head(2));
            const int id = i*2+1;
            vFlo->setId(id);
            vFlo->setMarginalized(true);
            optimizer.addVertex(vFlo);

            // Set Depth Vertices
            g2o::VertexSBADepth* vDepth = new g2o::VertexSBADepth();
            Eigen::Matrix<double, 1, 1> depth(FloD(2));
            vDepth->setEstimate(depth);
            const int id2 = id+1;
            vDepth->setId(id2);
            vDepth->setMarginalized(true);
            optimizer.addVertex(vDepth);

            Eigen::Matrix<double,2,1> obs_2d;
            const cv::KeyPoint &kpUn = pLastFrame->mvObjKeys[ObjId[i]];
            obs_2d << kpUn.pt.x, kpUn.pt.y;

            // Set Multiple Edges
            g2o::EdgeSE3ProjectFlowDepth3* e = new g2o::EdgeSE3ProjectFlowDepth3();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setVertex(2, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id2)));
            e->setMeasurement(obs_2d);
            Eigen::Matrix2d info_flow;
            info_flow << 12, 0, 0, 196;
            e->setInformation(Eigen::Matrix2d::Identity()*info_flow);
            // const float invSigma2_2d = 1.0;
            // e->setInformation(Eigen::Matrix2d::Identity()*invSigma2_2d);

            // g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            // e->setRobustKernel(rk);
            // rk->setDelta(deltaMono);

            e->fx = pCurFrame->fx;
            e->fy = pCurFrame->fy;
            e->cx = pCurFrame->cx;
            e->cy = pCurFrame->cy;

            const cv::Mat Rlw = pLastFrame->mTcw_gt.rowRange(0,3).colRange(0,3);
            const cv::Mat Rwl = Rlw.t();
            const cv::Mat tlw = pLastFrame->mTcw_gt.rowRange(0,3).col(3);
            const cv::Mat twl = -Rlw.t()*tlw;
            e->Twl.setIdentity(4,4);
            e->Twl.block(0,0,3,3) = Converter::toMatrix3d(Rwl);
            e->Twl.col(3).head(3) = Converter::toVector3d(twl);

            optimizer.addEdge(e);

            vpEdgesMono.push_back(e);
            vnIndexEdgeMono.push_back(i);

            // // Set Unary Flow Edges (constraints)
            // g2o::EdgeFlowPrior* e_flow = new g2o::EdgeFlowPrior();
            // e_flow->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            // e_flow->setMeasurement(FloD.head(2));
            // Eigen::Matrix<double,2,2> info_flow = Eigen::Matrix2d::Identity();
            // e_flow->setInformation(info_flow);
            // optimizer.addEdge(e_flow);

            // Set Unary Depth Edges (constraints)
            g2o::EdgeDepthPrior* e_depth = new g2o::EdgeDepthPrior();
            e_depth->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id2)));
            e_depth->setMeasurement(depth);
            Eigen::Matrix<double, 1, 1> info_depth(10);
            e_depth->setInformation(info_depth);
            optimizer.addEdge(e_depth);

        }

    }


    if(nInitialCorrespondences<3)
        return cv::Mat::eye(4,4,CV_32F);

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={5.991,5.991,5.991,5.991}; // {5.991,5.991,5.991,5.991}   {4,4,4,4}
    const int its[4]={200,100,100,100};

    int nBad=0;
    cout << endl;
    for(size_t it=0; it<1; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(Init));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;

        // monocular
        cout << endl << "chi2: " << endl;
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectFlowDepth3* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            if(chi2>chi2Mono[it])
                cout << chi2 << " ";
            if (i==(iend-1))
                cout << endl << endl;

            if(chi2>chi2Mono[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==0)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }


        if(optimizer.edges().size()<5)
            break;
    }

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);

    int inliers = nInitialCorrespondences-nBad;
    cout << endl;
    cout << "(OBJ)inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    cout << "re-projection error from the optimization: " << repro_e << endl;

    return pose;
}

cv::Mat Optimizer::PoseOptimizationFlow(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &ObjId)
{

    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(true);
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);

    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set MapPoint vertices
    const int N = ObjId.size();

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    cv::Mat Init = pCurFrame->mTcw_gt; // initial with camera pose
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set Edge info
    vector<g2o::EdgeSE3ProjectFlow*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    // parameter for robust function
    const float deltaMono = sqrt(4);  // 5.991

    bool mono = 1; // monocular
    float repro_e = 0;
    std::vector<bool> vIsOutlier(N);

    for(int i=0; i<N; i++)
    {

        if(mono)
        {
            nInitialCorrespondences++;
            vIsOutlier[i] = false;

            // Set Flow and Depth vertices
            g2o::VertexSBAFlow* vFlo = new g2o::VertexSBAFlow();
            Eigen::Matrix<double,3,1> FloD = Converter::toVector3d(pLastFrame->ObtainFlowDepthObject(ObjId[i],0));
            vFlo->setEstimate(FloD.head(2));
            const int id = i+1;
            vFlo->setId(id);
            vFlo->setMarginalized(true);
            optimizer.addVertex(vFlo);

            Eigen::Matrix<double,2,1> obs_2d;
            const cv::KeyPoint &kpUn = pCurFrame->mvObjKeys[ObjId[i]];
            obs_2d << kpUn.pt.x, kpUn.pt.y;

            // Set Binary Edges
            g2o::EdgeSE3ProjectFlow* e = new g2o::EdgeSE3ProjectFlow();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setMeasurement(obs_2d);
            const float invSigma2_2d = 0.02;
            e->setInformation(Eigen::Matrix2d::Identity()*invSigma2_2d);

            g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            e->setRobustKernel(rk);
            rk->setDelta(deltaMono);

            e->fx = pCurFrame->fx;
            e->fy = pCurFrame->fy;
            e->cx = pCurFrame->cx;
            e->cy = pCurFrame->cy;

            e->meas = obs_2d;
            e->depth = FloD(2);

            const cv::Mat Rlw = pLastFrame->mTcw_gt.rowRange(0,3).colRange(0,3);
            const cv::Mat Rwl = Rlw.t();
            const cv::Mat tlw = pLastFrame->mTcw_gt.rowRange(0,3).col(3);
            const cv::Mat twl = -Rlw.t()*tlw;
            e->Twl.setIdentity(4,4);
            e->Twl.block(0,0,3,3) = Converter::toMatrix3d(Rwl);
            e->Twl.col(3).head(3) = Converter::toVector3d(twl);

            optimizer.addEdge(e);

            vpEdgesMono.push_back(e);
            vnIndexEdgeMono.push_back(i);

            Eigen::Matrix<double,2,1> obs_flo;
            obs_flo << FloD(0), FloD(1);

            // Set Unary Edges (constraints)
            g2o::EdgeFlowPrior* e_con = new g2o::EdgeFlowPrior();

            e_con->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e_con->setMeasurement(obs_flo);
            const float invSigma2_flo = 1.0;
            e_con->setInformation(Eigen::Matrix2d::Identity()*invSigma2_flo);

            optimizer.addEdge(e_con);

        }

    }


    if(nInitialCorrespondences<3)
        return cv::Mat::eye(4,4,CV_32F);

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={4,4,4,4}; // {5.991,5.991,5.991,5.991}
    const int its[4]={1000,100,100,100};

    int nBad=0;
    cout << endl;
    for(size_t it=0; it<1; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(Init));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;

        // monocular
        cout << endl << "chi2: " << endl;
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectFlow* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            if(chi2>chi2Mono[it])
                cout << chi2 << " ";
            if (i==(iend-1))
                cout << endl << endl;

            if(chi2>chi2Mono[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==0)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }


        if(optimizer.edges().size()<5)
            break;
    }

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);

    int inliers = nInitialCorrespondences-nBad;
    cout << endl;
    cout << "(OBJ)inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    cout << "re-projection error from the optimization: " << repro_e << endl;

    return pose;
}

cv::Mat Optimizer::PoseOptimizationFlow2(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &ObjId, const vector<Eigen::Vector2d> &flo_gt, const vector<double> &e_bef)
{
    float rp_thres = 0.01;  // 0.04

    g2o::SparseOptimizer optimizer;
    // optimizer.setVerbose(true);
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);

    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set MapPoint vertices
    const int N = ObjId.size();

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    // cv::Mat Init = pCurFrame->mTcw_gt; // initial with camera pose
    cv::Mat Init = pCurFrame->mInitModel; // initial with camera pose
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set Edge info
    vector<g2o::EdgeSE3ProjectFlow2*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    // parameter for robust function
    const float deltaMono = sqrt(rp_thres);  // 5.991

    bool mono = 1; // monocular
    float repro_e = 0;
    std::vector<bool> vIsOutlier(N);

    for(int i=0; i<N; i++)
    {

        if(mono)
        {
            nInitialCorrespondences++;
            vIsOutlier[i] = false;

            // Set Flow vertices
            g2o::VertexSBAFlow* vFlo = new g2o::VertexSBAFlow();
            Eigen::Matrix<double,3,1> FloD = Converter::toVector3d(pLastFrame->ObtainFlowDepthObject(ObjId[i],0));
            vFlo->setEstimate(FloD.head(2));
            const int id = i+1;
            vFlo->setId(id);
            vFlo->setMarginalized(true);
            optimizer.addVertex(vFlo);

            Eigen::Matrix<double,2,1> obs_2d;
            const cv::KeyPoint &kpUn = pLastFrame->mvObjKeys[ObjId[i]];
            obs_2d << kpUn.pt.x, kpUn.pt.y;

            // Set Binary Edges
            g2o::EdgeSE3ProjectFlow2* e = new g2o::EdgeSE3ProjectFlow2();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setMeasurement(obs_2d);
            Eigen::Matrix2d info_flow;
            info_flow << 0.1, 0.0, 0.0, 0.1;
            e->setInformation(Eigen::Matrix2d::Identity()*info_flow);

            g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            e->setRobustKernel(rk);
            rk->setDelta(deltaMono);

            e->fx = pCurFrame->fx;
            e->fy = pCurFrame->fy;
            e->cx = pCurFrame->cx;
            e->cy = pCurFrame->cy;

            e->depth = FloD(2);

            const cv::Mat Rlw = pLastFrame->mTcw.rowRange(0,3).colRange(0,3);
            const cv::Mat Rwl = Rlw.t();
            const cv::Mat tlw = pLastFrame->mTcw.rowRange(0,3).col(3);
            const cv::Mat twl = -Rlw.t()*tlw;
            e->Twl.setIdentity(4,4);
            e->Twl.block(0,0,3,3) = Converter::toMatrix3d(Rwl);
            e->Twl.col(3).head(3) = Converter::toVector3d(twl);

            optimizer.addEdge(e);

            vpEdgesMono.push_back(e);
            vnIndexEdgeMono.push_back(i);

            Eigen::Matrix<double,2,1> obs_flo;
            obs_flo << FloD(0), FloD(1);

            // Set Unary Edges (constraints)
            g2o::EdgeFlowPrior* e_con = new g2o::EdgeFlowPrior();
            e_con->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e_con->setMeasurement(obs_flo);
            // const float invSigma2_flo = 1.0;
            Eigen::Matrix2d invSigma2_flo;
            invSigma2_flo << 0.5, 0.0, 0.0, 0.5;
            e_con->setInformation(Eigen::Matrix2d::Identity()*invSigma2_flo);
            optimizer.addEdge(e_con);

        }

    }


    if(nInitialCorrespondences<3)
        return cv::Mat::eye(4,4,CV_32F);

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={rp_thres,5.991,5.991,5.991}; // {5.991,5.991,5.991,5.991} {4,4,4,4}
    const int its[4]={200,100,100,100};

    int nBad=0;
    cout << endl;
    for(size_t it=0; it<1; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(Init));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;

        // monocular
        // cout << endl << "chi2: " << endl;
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectFlow2* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            // if(chi2>chi2Mono[it])
            //     cout << chi2 << " ";
            // if (i==(iend-1))
            //     cout << endl << endl;

            if(chi2>chi2Mono[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==0)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }


        if(optimizer.edges().size()<5)
            break;
    }

    // *** Recover optimized pose and return number of inliers ***
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);

    // *** Recover optimized optical flow ***
    // cout << "flow error before and after optimized: " << endl;
    double e_aft_sum = 0.0, e_bef_sum = 0.0;
    for (int i = 0; i < N; ++i)
    {
        g2o::VertexSBAFlow* vFlow = static_cast<g2o::VertexSBAFlow*>(optimizer.vertex(i+1));
        // Eigen::Vector2d flo_pre;
        // flo_pre << pLastFrame->mvObjFlowNext[ObjId[i]].x, pLastFrame->mvObjFlowNext[ObjId[i]].y;
        // Eigen::Vector2d flo_error = flo_pre - flo_gt[i];
        Eigen::Vector2d flo_error = vFlow->estimate() - flo_gt[i];
        e_aft_sum = e_aft_sum + flo_error.norm();
        e_bef_sum = e_bef_sum + e_bef[i];
        // cout << e_bef[i]-flo_error.norm() << endl;
    }
    // cout << "average flow error before and after: " << e_bef_sum/N << " " << e_aft_sum/N << endl;
    int inliers = nInitialCorrespondences-nBad;
    cout << "(OBJ)inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    // cout << "re-projection error from the optimization: " << repro_e << endl;

    return pose;
}

cv::Mat Optimizer::PoseOptimizationFlow2RanSac(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &ObjId, const vector<Eigen::Vector2d> &flo_gt, const vector<double> &e_bef)
{

    // Preset Parameters
    cv::RNG rng((unsigned)time(NULL));
    int Iter_num = 100, sample_num = 100;
    float rpe_thres = 0.4;
    cv::Mat best_model, tmp_model;
    std::vector<int> ObjId_best;

    // Main Loop for RanSac
    for (int i = 0; i < Iter_num; ++i)
    {
        // (1) sample a subset of candidates for optimization
        std::vector<int> ObjId_sub(sample_num);
        for (int j = 0; j < sample_num; ++j){
            const int sample = rng.uniform(0, ObjId.size()-1);
            ObjId_sub[j] = ObjId[sample];
        }

        // (2) create a new solver
        g2o::SparseOptimizer optimizer;
        g2o::BlockSolver_6_3::LinearSolverType * linearSolver;
        linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();
        g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);
        g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
        optimizer.setAlgorithm(solver);

        // (3) set frame vertex
        g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
        cv::Mat Init = pCurFrame->mTcw_gt; // initial with camera pose
        vSE3->setEstimate(Converter::toSE3Quat(Init));
        vSE3->setId(0);
        vSE3->setFixed(false);
        optimizer.addVertex(vSE3);

        // (4) parameter for robust function
        const float deltaB = sqrt(0.16);

        // (5) main loop for graph setting
        for (int j = 0; j < ObjId_sub.size(); ++j)
        {
            // set flow vertices
            g2o::VertexSBAFlow* vFlo = new g2o::VertexSBAFlow();
            Eigen::Matrix<double,3,1> FloD = Converter::toVector3d(pLastFrame->ObtainFlowDepthObject(ObjId_sub[j],1));
            vFlo->setEstimate(FloD.head(2));
            const int id = j+1;
            vFlo->setId(id);
            vFlo->setMarginalized(true);
            optimizer.addVertex(vFlo);

            Eigen::Matrix<double,2,1> obs_2d;
            const cv::KeyPoint &kpUn = pLastFrame->mvObjKeys[ObjId_sub[j]];
            obs_2d << kpUn.pt.x, kpUn.pt.y;

            // set binary edges
            g2o::EdgeSE3ProjectFlow2* e = new g2o::EdgeSE3ProjectFlow2();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setMeasurement(obs_2d);
            Eigen::Matrix2d info_flow;
            info_flow << 25.0, 0, 0, 50.0;
            e->setInformation(Eigen::Matrix2d::Identity()*info_flow);

            g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            e->setRobustKernel(rk);
            rk->setDelta(deltaB);

            e->fx = pCurFrame->fx;
            e->fy = pCurFrame->fy;
            e->cx = pCurFrame->cx;
            e->cy = pCurFrame->cy;

            e->depth = FloD(2);

            const cv::Mat Rlw = pLastFrame->mTcw_gt.rowRange(0,3).colRange(0,3);
            const cv::Mat Rwl = Rlw.t();
            const cv::Mat tlw = pLastFrame->mTcw_gt.rowRange(0,3).col(3);
            const cv::Mat twl = -Rlw.t()*tlw;
            e->Twl.setIdentity(4,4);
            e->Twl.block(0,0,3,3) = Converter::toMatrix3d(Rwl);
            e->Twl.col(3).head(3) = Converter::toVector3d(twl);

            optimizer.addEdge(e);

            Eigen::Matrix<double,2,1> obs_flo;
            obs_flo << FloD(0), FloD(1);

            // set unary edges (constraints)
            g2o::EdgeFlowPrior* e_con = new g2o::EdgeFlowPrior();
            e_con->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e_con->setMeasurement(obs_flo);
            Eigen::Matrix2d invSigma2_flo;
            invSigma2_flo << 1.0, 0, 0, 1.0;
            e_con->setInformation(Eigen::Matrix2d::Identity()*invSigma2_flo);
            optimizer.addEdge(e_con);
        }

        // (6) optimize
        optimizer.initializeOptimization(0);
        optimizer.optimize(100);

        // (7) get optimized pose result
        g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
        g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
        tmp_model = Converter::toCvMat(SE3quat_recov);

        cout << "temperal model: " << endl << tmp_model << endl;

        // (8) check inlier via re-projection error
        int inlier_sum = 0;
        std::vector<int> ObjId_inlier;
        for (int j = 0; j < ObjId.size(); ++j)
        {
            cv::Mat x3D_p = pLastFrame->UnprojectStereoObject(ObjId[j],1);

            // cv::Mat TH = Converter::toInvMatrix(pCurFrame->mTcw_gt)*tmp_model;
            cv::Mat x3D_pc = tmp_model.rowRange(0,3).colRange(0,3)*x3D_p+tmp_model.rowRange(0,3).col(3);

            float xc = x3D_pc.at<float>(0);
            float yc = x3D_pc.at<float>(1);
            float invzc = 1.0/x3D_pc.at<float>(2);

            float u = pCurFrame->fx*xc*invzc+pCurFrame->cx;
            float v = pCurFrame->fy*yc*invzc+pCurFrame->cy;
            float u_ = pCurFrame->mvObjKeys[ObjId[j]].pt.x - u;
            float v_ = pCurFrame->mvObjKeys[ObjId[j]].pt.x - v;
            float Rpe = std::sqrt(u_*u_ + v_*v_);
            if (Rpe<rpe_thres)
            {
                inlier_sum = inlier_sum + 1;
                ObjId_inlier.push_back(ObjId[j]);
            }
        }

        cout << "inlier subset of object id: " << ObjId_inlier.size() << endl;

        // (9) update model and inlier list
        if (i==0){
            ObjId_best = ObjId_inlier;
            best_model = tmp_model;
        }
        else{
            if (ObjId_inlier.size()>ObjId_best.size())
            {
                ObjId_best = ObjId_inlier;
                best_model = tmp_model;
            }
        }
    }

    // Final optimization with most inlier set
    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(true);
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);

    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set MapPoint Vertices
    const int N = ObjId_best.size();

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    cv::Mat Init = best_model;
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set Edge info
    vector<g2o::EdgeSE3ProjectFlow2*> vpEdges;
    vector<size_t> vnIndexEdge;
    vpEdges.reserve(N);
    vnIndexEdge.reserve(N);

    // parameter for robust function
    const float delta = sqrt(0.04);

    float repro_e = 0;

    for(int i=0; i<N; i++)
    {

        nInitialCorrespondences++;

        // Set Flow Vertices
        g2o::VertexSBAFlow* vFlo = new g2o::VertexSBAFlow();
        Eigen::Matrix<double,3,1> FloD = Converter::toVector3d(pLastFrame->ObtainFlowDepthObject(ObjId_best[i],1));
        vFlo->setEstimate(FloD.head(2));
        const int id = i+1;
        vFlo->setId(id);
        vFlo->setMarginalized(true);
        optimizer.addVertex(vFlo);

        Eigen::Matrix<double,2,1> obs_2d;
        const cv::KeyPoint &kpUn = pLastFrame->mvObjKeys[ObjId_best[i]];
        obs_2d << kpUn.pt.x, kpUn.pt.y;

        // Set Binary Edges
        g2o::EdgeSE3ProjectFlow2* e = new g2o::EdgeSE3ProjectFlow2();

        e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
        e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
        e->setMeasurement(obs_2d);
        Eigen::Matrix2d info_flow;
        info_flow << 10.0, 0, 0, 30.0;
        e->setInformation(Eigen::Matrix2d::Identity()*info_flow);

        g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
        e->setRobustKernel(rk);
        rk->setDelta(delta);

        e->fx = pCurFrame->fx;
        e->fy = pCurFrame->fy;
        e->cx = pCurFrame->cx;
        e->cy = pCurFrame->cy;

        e->depth = FloD(2);

        const cv::Mat Rlw = pLastFrame->mTcw_gt.rowRange(0,3).colRange(0,3);
        const cv::Mat Rwl = Rlw.t();
        const cv::Mat tlw = pLastFrame->mTcw_gt.rowRange(0,3).col(3);
        const cv::Mat twl = -Rlw.t()*tlw;
        e->Twl.setIdentity(4,4);
        e->Twl.block(0,0,3,3) = Converter::toMatrix3d(Rwl);
        e->Twl.col(3).head(3) = Converter::toVector3d(twl);

        optimizer.addEdge(e);

        vpEdges.push_back(e);
        vnIndexEdge.push_back(i);

        Eigen::Matrix<double,2,1> obs_flo;
        obs_flo << FloD(0), FloD(1);

        // Set Unary Edges (constraints)
        g2o::EdgeFlowPrior* e_con = new g2o::EdgeFlowPrior();
        e_con->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
        e_con->setMeasurement(obs_flo);
        Eigen::Matrix2d invSigma2_flo;
        invSigma2_flo << 1.0, 0, 0, 1.0;
        e_con->setInformation(Eigen::Matrix2d::Identity()*invSigma2_flo);
        optimizer.addEdge(e_con);

    }


    if(nInitialCorrespondences<3)
        return cv::Mat::eye(4,4,CV_32F);


    int nBad=0;
    float chi2_thres = 0.04;

    vSE3->setEstimate(Converter::toSE3Quat(Init));
    optimizer.initializeOptimization(0);
    optimizer.optimize(200);

    cout << endl << "chi2: " << endl;
    for(size_t i=0, iend=vpEdges.size(); i<iend; i++)
    {
        g2o::EdgeSE3ProjectFlow2* e = vpEdges[i];

        e->computeError();
        const float chi2 = e->chi2();

        if(chi2>chi2_thres)
        {
            cout << chi2 << " ";
            e->setLevel(1);
            nBad++;
        }
        else
        {
            // ++++ new added for calculating re-projection error +++
            repro_e = repro_e + std::sqrt(chi2);
            e->setLevel(0);
        }
    }
    cout << endl;

    // *** Recover optimized pose and return number of inliers ***
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);

    // *** Recover optimized optical flow ***
    // cout << "flow error before and after optimized: " << endl;
    double e_aft_sum = 0.0, e_bef_sum = 0.0;
    for (int i = 0; i < N; ++i)
    {
        g2o::VertexSBAFlow* vFlow = static_cast<g2o::VertexSBAFlow*>(optimizer.vertex(i+1));
        // Eigen::Vector2d flo_pre;
        // flo_pre << pLastFrame->mvObjFlowNext[ObjId[i]].x, pLastFrame->mvObjFlowNext[ObjId[i]].y;
        // Eigen::Vector2d flo_error = flo_pre - flo_gt[i];
        Eigen::Vector2d flo_error = vFlow->estimate() - flo_gt[i];
        e_aft_sum = e_aft_sum + flo_error.norm();
        e_bef_sum = e_bef_sum + e_bef[i];
        // cout << e_bef[i]-flo_error.norm() << endl;
    }
    cout << "average flow error before and after: " << e_bef_sum/N << " " << e_aft_sum/N << endl;
    int inliers = nInitialCorrespondences-nBad;
    cout << "(OBJ)inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    cout << "re-projection error from the optimization: " << repro_e << endl;

    return pose;
}

cv::Mat Optimizer::PoseOptimizationDepth(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &ObjId)
{

    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(true);
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);

    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set MapPoint vertices
    const int N = ObjId.size();

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    cv::Mat Init = pCurFrame->mTcw_gt; // initial with camera pose
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set Edge info
    vector<g2o::EdgeSE3ProjectDepth*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    // parameter for robust function
    const float deltaMono = sqrt(4);  // 5.991

    bool mono = 1; // monocular
    float repro_e = 0;
    std::vector<bool> vIsOutlier(N);

    for(int i=0; i<N; i++)
    {

        if(mono)
        {
            nInitialCorrespondences++;
            vIsOutlier[i] = false;

            // Set Depth vertices
            g2o::VertexSBADepth* vDepth = new g2o::VertexSBADepth();
            Eigen::Matrix<double,3,1> FloD = Converter::toVector3d(pLastFrame->ObtainFlowDepthObject(ObjId[i],1));
            Eigen::Matrix<double, 1, 1> depth(FloD(2));
            vDepth->setEstimate(depth);
            const int id = i+1;
            vDepth->setId(id);
            vDepth->setMarginalized(true);
            optimizer.addVertex(vDepth);

            Eigen::Matrix<double,2,1> obs_2d;
            const cv::KeyPoint &kpUn = pLastFrame->mvObjKeys[ObjId[i]];
            obs_2d << kpUn.pt.x, kpUn.pt.y;

            // Set Binary Edges
            g2o::EdgeSE3ProjectDepth* e = new g2o::EdgeSE3ProjectDepth();

            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
            e->setMeasurement(obs_2d);
            const float invSigma2_2d = 1.0;
            e->setInformation(Eigen::Matrix2d::Identity()*invSigma2_2d);

            g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
            e->setRobustKernel(rk);
            rk->setDelta(deltaMono);

            e->fx = pCurFrame->fx;
            e->fy = pCurFrame->fy;
            e->cx = pCurFrame->cx;
            e->cy = pCurFrame->cy;

            Eigen::Vector2d obs_flo = FloD.head(2);
            e->flow = obs_flo;

            const cv::Mat Rlw = pLastFrame->mTcw_gt.rowRange(0,3).colRange(0,3);
            const cv::Mat Rwl = Rlw.t();
            const cv::Mat tlw = pLastFrame->mTcw_gt.rowRange(0,3).col(3);
            const cv::Mat twl = -Rlw.t()*tlw;
            e->Twl.setIdentity(4,4);
            e->Twl.block(0,0,3,3) = Converter::toMatrix3d(Rwl);
            e->Twl.col(3).head(3) = Converter::toVector3d(twl);

            optimizer.addEdge(e);

            vpEdgesMono.push_back(e);
            vnIndexEdgeMono.push_back(i);

            // Set Unary Edges (constraints)
            g2o::EdgeDepthPrior* e_con = new g2o::EdgeDepthPrior();

            e_con->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
            e_con->setMeasurement(depth);

            Eigen::Matrix<double, 1, 1> info(depth*depth/(725*0.5)*0.15);
            e_con->setInformation(info);

            optimizer.addEdge(e_con);

        }

    }


    if(nInitialCorrespondences<3)
        return cv::Mat::eye(4,4,CV_32F);

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={4,4,4,4}; // {5.991,5.991,5.991,5.991}
    const int its[4]={500,100,100,100};

    int nBad=0;
    cout << endl;
    for(size_t it=0; it<1; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(Init));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;

        // monocular
        cout << endl << "chi2: " << endl;
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectDepth* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            if(chi2>chi2Mono[it])
                cout << chi2 << " ";
            if (i==(iend-1))
                cout << endl << endl;

            if(chi2>chi2Mono[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==0)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }


        if(optimizer.edges().size()<5)
            break;
    }

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);

    int inliers = nInitialCorrespondences-nBad;
    cout << endl;
    cout << "(OBJ)inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    cout << "re-projection error from the optimization: " << repro_e << endl;

    return pose;
}

cv::Mat Optimizer::PoseOptimizationForBack(Frame *pCurFrame, Frame *pLastFrame, const vector<int> &ObjId)
{

    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(true);
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set MapPoint vertices
    const int N = ObjId.size();

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    cv::Mat Init = pCurFrame->mTcw_gt; // initial with camera pose
    vSE3->setEstimate(Converter::toSE3Quat(Init));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set Forward Edge info
    vector<g2o::EdgeSE3ProjectXYZOnlyPose*> vpEdgesForward;
    vector<size_t> vnIndexEdgeForward;
    vpEdgesForward.reserve(N);
    vnIndexEdgeForward.reserve(N);

    // Set Backward Edge info
    vector<g2o::EdgeSE3ProjectXYZOnlyPoseBack*> vpEdgesBackward;
    vector<size_t> vnIndexEdgeBackward;
    vpEdgesBackward.reserve(N);
    vnIndexEdgeBackward.reserve(N);

    // parameter for robust function
    const float delta = sqrt(4);  // 5.991

    float repro_e = 0;
    std::vector<bool> vIsOutlier(N*2);

    for(int i=0; i<N; i++)
    {

        nInitialCorrespondences++;
        vIsOutlier[i] = false;

        // Set Forward Projection Edges
        Eigen::Matrix<double,2,1> obs_cur;
        const cv::KeyPoint &kpUn_cur = pCurFrame->mvObjKeys[ObjId[i]];
        obs_cur << kpUn_cur.pt.x, kpUn_cur.pt.y;

        g2o::EdgeSE3ProjectXYZOnlyPose* e = new g2o::EdgeSE3ProjectXYZOnlyPose();

        e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
        e->setMeasurement(obs_cur);
        const float invSigma2 = 1.0;
        e->setInformation(Eigen::Matrix2d::Identity()*invSigma2);

        g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
        e->setRobustKernel(rk);
        rk->setDelta(delta);

        e->fx = pCurFrame->fx;
        e->fy = pCurFrame->fy;
        e->cx = pCurFrame->cx;
        e->cy = pCurFrame->cy;

        cv::Mat Xc_p = pLastFrame->UnprojectStereoObjectCamera(ObjId[i],1);

        e->Xw[0] = Xc_p.at<float>(0);
        e->Xw[1] = Xc_p.at<float>(1);
        e->Xw[2] = Xc_p.at<float>(2);

        optimizer.addEdge(e);

        vpEdgesForward.push_back(e);
        vnIndexEdgeForward.push_back(i);

        // Set Backward Projection Edges
        Eigen::Matrix<double,2,1> obs_pre;
        const cv::KeyPoint &kpUn_pre = pLastFrame->mvObjKeys[ObjId[i]];
        obs_pre << kpUn_pre.pt.x, kpUn_pre.pt.y;

        g2o::EdgeSE3ProjectXYZOnlyPoseBack* e_b = new g2o::EdgeSE3ProjectXYZOnlyPoseBack();

        e_b->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
        e_b->setMeasurement(obs_pre);
        e_b->setInformation(Eigen::Matrix2d::Identity()*invSigma2);

        g2o::RobustKernelHuber* rk_2 = new g2o::RobustKernelHuber;
        e_b->setRobustKernel(rk_2);
        rk_2->setDelta(delta);

        e_b->fx = pCurFrame->fx;
        e_b->fy = pCurFrame->fy;
        e_b->cx = pCurFrame->cx;
        e_b->cy = pCurFrame->cy;

        cv::Mat Xc_c = pCurFrame->UnprojectStereoObjectCamera(ObjId[i],1);

        e_b->Xw[0] = Xc_c.at<float>(0);
        e_b->Xw[1] = Xc_c.at<float>(1);
        e_b->Xw[2] = Xc_c.at<float>(2);

        optimizer.addEdge(e_b);

        vpEdgesBackward.push_back(e_b);
        vnIndexEdgeBackward.push_back(i);

    }


    if(nInitialCorrespondences<3)
        return cv::Mat::eye(4,4,CV_32F);

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2ForBack[4]={4,4,4,4}; // {5.991,5.991,5.991,5.991}
    const int its[4]={100,100,100,100};

    int nBad=0;
    cout << endl;
    for(size_t it=0; it<4; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(Init));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;

        // Forward
        // cout << endl << "chi2: " << endl;
        for(size_t i=0, iend=vpEdgesForward.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectXYZOnlyPose* e = vpEdgesForward[i];

            const size_t idx = vnIndexEdgeForward[i];

            if(vIsOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            // if(chi2>chi2ForBack[it])
            //     cout << chi2 << " ";
            // if (i==(iend-1))
            //     cout << endl << endl;

            if(chi2>chi2ForBack[it])
            {
                vIsOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                // ++++ new added for calculating re-projection error +++
                if (it==3)
                {
                    repro_e = repro_e + std::sqrt(chi2);
                }
                vIsOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }

        // Backward
        // cout << endl << "chi2: " << endl;
        for(size_t i=0, iend=vpEdgesBackward.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectXYZOnlyPoseBack* e = vpEdgesBackward[i];

            const size_t idx = vnIndexEdgeBackward[i];

            if(vIsOutlier[idx+N])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            // if(chi2>chi2ForBack[it])
            //     cout << chi2 << " ";
            // if (i==(iend-1))
            //     cout << endl << endl;

            if(chi2>chi2ForBack[it])
            {
                vIsOutlier[idx+N]=true;
                e->setLevel(1);
            }
            else
            {
                vIsOutlier[idx+N]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }


        if(optimizer.edges().size()<10){
            break;
        }
    }

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);

    int inliers = nInitialCorrespondences-nBad;
    cout << endl;
    cout << "(OBJ)inliers number/total numbers: " << inliers << "/" << nInitialCorrespondences << endl;
    repro_e = repro_e/inliers;
    cout << "re-projection error from the optimization: " << repro_e << endl;

    return pose;
}

int Optimizer::PoseOptimization(Frame *pFrame)
{
    g2o::SparseOptimizer optimizer;
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    optimizer.setAlgorithm(solver);

    int nInitialCorrespondences=0;

    // Set Frame vertex
    g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
    vSE3->setEstimate(Converter::toSE3Quat(pFrame->mTcw));
    vSE3->setId(0);
    vSE3->setFixed(false);
    optimizer.addVertex(vSE3);

    // Set MapPoint vertices
    const int N = pFrame->N;

    vector<g2o::EdgeSE3ProjectXYZOnlyPose*> vpEdgesMono;
    vector<size_t> vnIndexEdgeMono;
    vpEdgesMono.reserve(N);
    vnIndexEdgeMono.reserve(N);

    vector<g2o::EdgeStereoSE3ProjectXYZOnlyPose*> vpEdgesStereo;
    vector<size_t> vnIndexEdgeStereo;
    vpEdgesStereo.reserve(N);
    vnIndexEdgeStereo.reserve(N);

    const float deltaMono = sqrt(5.991);
    const float deltaStereo = sqrt(7.815);  // 7.815


    {
    unique_lock<mutex> lock(MapPoint::mGlobalMutex);

    for(int i=0; i<N; i++)
    {
        MapPoint* pMP = pFrame->mvpMapPoints[i];
        if(pMP)
        {

            // just use the background points on semantically segmented background
            // if(pFrame->vSemLabel[i]!=0){
            //     continue;
            // }

            // Monocular observation
            if(pFrame->mvuRight[i]<0)
            {
                nInitialCorrespondences++;
                pFrame->mvbOutlier[i] = false;

                Eigen::Matrix<double,2,1> obs;
                const cv::KeyPoint &kpUn = pFrame->mvKeysUn[i];
                obs << kpUn.pt.x, kpUn.pt.y;

                g2o::EdgeSE3ProjectXYZOnlyPose* e = new g2o::EdgeSE3ProjectXYZOnlyPose();

                e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
                e->setMeasurement(obs);
                const float invSigma2 = pFrame->mvInvLevelSigma2[kpUn.octave];
                e->setInformation(Eigen::Matrix2d::Identity()*invSigma2);

                g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
                e->setRobustKernel(rk);
                rk->setDelta(deltaMono);

                e->fx = pFrame->fx;
                e->fy = pFrame->fy;
                e->cx = pFrame->cx;
                e->cy = pFrame->cy;
                cv::Mat Xw = pMP->GetWorldPos();
                e->Xw[0] = Xw.at<float>(0);
                e->Xw[1] = Xw.at<float>(1);
                e->Xw[2] = Xw.at<float>(2);

                optimizer.addEdge(e);

                vpEdgesMono.push_back(e);
                vnIndexEdgeMono.push_back(i);
            }
            else  // Stereo observation
            {
                nInitialCorrespondences++;
                pFrame->mvbOutlier[i] = false;

                //SET EDGE
                Eigen::Matrix<double,3,1> obs;
                const cv::KeyPoint &kpUn = pFrame->mvKeysUn[i];
                const float &kp_ur = pFrame->mvuRight[i];
                obs << kpUn.pt.x, kpUn.pt.y, kp_ur;

                g2o::EdgeStereoSE3ProjectXYZOnlyPose* e = new g2o::EdgeStereoSE3ProjectXYZOnlyPose();

                e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
                e->setMeasurement(obs);
                const float invSigma2 = pFrame->mvInvLevelSigma2[kpUn.octave];
                Eigen::Matrix3d Info = Eigen::Matrix3d::Identity()*invSigma2;
                e->setInformation(Info);

                g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
                e->setRobustKernel(rk);
                rk->setDelta(deltaStereo);

                e->fx = pFrame->fx;
                e->fy = pFrame->fy;
                e->cx = pFrame->cx;
                e->cy = pFrame->cy;
                e->bf = pFrame->mbf;
                cv::Mat Xw = pMP->GetWorldPos();
                e->Xw[0] = Xw.at<float>(0);
                e->Xw[1] = Xw.at<float>(1);
                e->Xw[2] = Xw.at<float>(2);

                optimizer.addEdge(e);

                vpEdgesStereo.push_back(e);
                vnIndexEdgeStereo.push_back(i);
            }
        }

    }
    }


    if(nInitialCorrespondences<3)
        return 0;

    // We perform 4 optimizations, after each optimization we classify observation as inlier/outlier
    // At the next optimization, outliers are not included, but at the end they can be classified as inliers again.
    const float chi2Mono[4]={5.991,5.991,5.991,5.991};
    const float chi2Stereo[4]={7.815,7.815,7.815,7.815}; //{7.815,7.815,7.815, 7.815}
    const int its[4]={10,10,10,10};

    int nBad=0;
    for(size_t it=0; it<4; it++)
    {

        vSE3->setEstimate(Converter::toSE3Quat(pFrame->mTcw));
        optimizer.initializeOptimization(0);
        optimizer.optimize(its[it]);

        nBad=0;
        for(size_t i=0, iend=vpEdgesMono.size(); i<iend; i++)
        {
            g2o::EdgeSE3ProjectXYZOnlyPose* e = vpEdgesMono[i];

            const size_t idx = vnIndexEdgeMono[i];

            if(pFrame->mvbOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            if(chi2>chi2Mono[it])
            {
                pFrame->mvbOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                pFrame->mvbOutlier[idx]=false;
                e->setLevel(0);
            }

            if(it==2)
                e->setRobustKernel(0);
        }

        for(size_t i=0, iend=vpEdgesStereo.size(); i<iend; i++)
        {
            g2o::EdgeStereoSE3ProjectXYZOnlyPose* e = vpEdgesStereo[i];

            const size_t idx = vnIndexEdgeStereo[i];

            if(pFrame->mvbOutlier[idx])
            {
                e->computeError();
            }

            const float chi2 = e->chi2();

            if(chi2>chi2Stereo[it])
            {
                pFrame->mvbOutlier[idx]=true;
                e->setLevel(1);
                nBad++;
            }
            else
            {
                e->setLevel(0);
                pFrame->mvbOutlier[idx]=false;
            }

            if(it==2)
                e->setRobustKernel(0);
        }

        if(optimizer.edges().size()<10)
            break;
    }

    // Recover optimized pose and return number of inliers
    g2o::VertexSE3Expmap* vSE3_recov = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(0));
    g2o::SE3Quat SE3quat_recov = vSE3_recov->estimate();
    cv::Mat pose = Converter::toCvMat(SE3quat_recov);
    pFrame->SetPose(pose);

    return nInitialCorrespondences-nBad;
}

void Optimizer::LocalBundleAdjustment(KeyFrame *pKF, bool* pbStopFlag, Map* pMap)
{
    // Local KeyFrames: First Breath Search from Current Keyframe
    list<KeyFrame*> lLocalKeyFrames;

    lLocalKeyFrames.push_back(pKF);
    pKF->mnBALocalForKF = pKF->mnId;

    const vector<KeyFrame*> vNeighKFs = pKF->GetVectorCovisibleKeyFrames();
    for(int i=0, iend=vNeighKFs.size(); i<iend; i++)
    {
        KeyFrame* pKFi = vNeighKFs[i];
        pKFi->mnBALocalForKF = pKF->mnId;
        if(!pKFi->isBad())
            lLocalKeyFrames.push_back(pKFi);
    }

    // Local MapPoints seen in Local KeyFrames
    list<MapPoint*> lLocalMapPoints;
    for(list<KeyFrame*>::iterator lit=lLocalKeyFrames.begin() , lend=lLocalKeyFrames.end(); lit!=lend; lit++)
    {
        vector<MapPoint*> vpMPs = (*lit)->GetMapPointMatches();
        for(vector<MapPoint*>::iterator vit=vpMPs.begin(), vend=vpMPs.end(); vit!=vend; vit++)
        {
            MapPoint* pMP = *vit;
            if(pMP)
                if(!pMP->isBad())
                    if(pMP->mnBALocalForKF!=pKF->mnId)
                    {
                        lLocalMapPoints.push_back(pMP);
                        pMP->mnBALocalForKF=pKF->mnId;
                    }
        }
    }

    // Fixed Keyframes. Keyframes that see Local MapPoints but that are not Local Keyframes
    list<KeyFrame*> lFixedCameras;
    for(list<MapPoint*>::iterator lit=lLocalMapPoints.begin(), lend=lLocalMapPoints.end(); lit!=lend; lit++)
    {
        map<KeyFrame*,size_t> observations = (*lit)->GetObservations();
        for(map<KeyFrame*,size_t>::iterator mit=observations.begin(), mend=observations.end(); mit!=mend; mit++)
        {
            KeyFrame* pKFi = mit->first;

            if(pKFi->mnBALocalForKF!=pKF->mnId && pKFi->mnBAFixedForKF!=pKF->mnId)
            {                
                pKFi->mnBAFixedForKF=pKF->mnId;
                if(!pKFi->isBad())
                    lFixedCameras.push_back(pKFi);
            }
        }
    }

    // Setup optimizer
    g2o::SparseOptimizer optimizer;
    g2o::BlockSolver_6_3::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverEigen<g2o::BlockSolver_6_3::PoseMatrixType>();

    g2o::BlockSolver_6_3 * solver_ptr = new g2o::BlockSolver_6_3(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    optimizer.setAlgorithm(solver);

    if(pbStopFlag)
        optimizer.setForceStopFlag(pbStopFlag);

    unsigned long maxKFid = 0;

    // Set Local KeyFrame vertices
    for(list<KeyFrame*>::iterator lit=lLocalKeyFrames.begin(), lend=lLocalKeyFrames.end(); lit!=lend; lit++)
    {
        KeyFrame* pKFi = *lit;
        g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
        vSE3->setEstimate(Converter::toSE3Quat(pKFi->GetPose()));
        vSE3->setId(pKFi->mnId);
        vSE3->setFixed(pKFi->mnId==0);
        optimizer.addVertex(vSE3);
        if(pKFi->mnId>maxKFid)
            maxKFid=pKFi->mnId;
    }

    // Set Fixed KeyFrame vertices
    for(list<KeyFrame*>::iterator lit=lFixedCameras.begin(), lend=lFixedCameras.end(); lit!=lend; lit++)
    {
        KeyFrame* pKFi = *lit;
        g2o::VertexSE3Expmap * vSE3 = new g2o::VertexSE3Expmap();
        vSE3->setEstimate(Converter::toSE3Quat(pKFi->GetPose()));
        vSE3->setId(pKFi->mnId);
        vSE3->setFixed(true);
        optimizer.addVertex(vSE3);
        if(pKFi->mnId>maxKFid)
            maxKFid=pKFi->mnId;
    }

    // Set MapPoint vertices
    const int nExpectedSize = (lLocalKeyFrames.size()+lFixedCameras.size())*lLocalMapPoints.size();

    vector<g2o::EdgeSE3ProjectXYZ*> vpEdgesMono;
    vpEdgesMono.reserve(nExpectedSize);

    vector<KeyFrame*> vpEdgeKFMono;
    vpEdgeKFMono.reserve(nExpectedSize);

    vector<MapPoint*> vpMapPointEdgeMono;
    vpMapPointEdgeMono.reserve(nExpectedSize);

    vector<g2o::EdgeStereoSE3ProjectXYZ*> vpEdgesStereo;
    vpEdgesStereo.reserve(nExpectedSize);

    vector<KeyFrame*> vpEdgeKFStereo;
    vpEdgeKFStereo.reserve(nExpectedSize);

    vector<MapPoint*> vpMapPointEdgeStereo;
    vpMapPointEdgeStereo.reserve(nExpectedSize);

    const float thHuberMono = sqrt(5.991);
    const float thHuberStereo = sqrt(7.815);

    for(list<MapPoint*>::iterator lit=lLocalMapPoints.begin(), lend=lLocalMapPoints.end(); lit!=lend; lit++)
    {
        MapPoint* pMP = *lit;
        g2o::VertexSBAPointXYZ* vPoint = new g2o::VertexSBAPointXYZ();
        vPoint->setEstimate(Converter::toVector3d(pMP->GetWorldPos()));
        int id = pMP->mnId+maxKFid+1;
        vPoint->setId(id);
        vPoint->setMarginalized(true);
        optimizer.addVertex(vPoint);

        const map<KeyFrame*,size_t> observations = pMP->GetObservations();

        //Set edges
        for(map<KeyFrame*,size_t>::const_iterator mit=observations.begin(), mend=observations.end(); mit!=mend; mit++)
        {
            KeyFrame* pKFi = mit->first;

            if(!pKFi->isBad())
            {                
                const cv::KeyPoint &kpUn = pKFi->mvKeysUn[mit->second];

                // Monocular observation
                if(pKFi->mvuRight[mit->second]<0)
                {
                    Eigen::Matrix<double,2,1> obs;
                    obs << kpUn.pt.x, kpUn.pt.y;

                    g2o::EdgeSE3ProjectXYZ* e = new g2o::EdgeSE3ProjectXYZ();

                    e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
                    e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(pKFi->mnId)));
                    e->setMeasurement(obs);
                    const float &invSigma2 = pKFi->mvInvLevelSigma2[kpUn.octave];
                    e->setInformation(Eigen::Matrix2d::Identity()*invSigma2);

                    g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
                    e->setRobustKernel(rk);
                    rk->setDelta(thHuberMono);

                    e->fx = pKFi->fx;
                    e->fy = pKFi->fy;
                    e->cx = pKFi->cx;
                    e->cy = pKFi->cy;

                    optimizer.addEdge(e);
                    vpEdgesMono.push_back(e);
                    vpEdgeKFMono.push_back(pKFi);
                    vpMapPointEdgeMono.push_back(pMP);
                }
                else // Stereo observation
                {
                    Eigen::Matrix<double,3,1> obs;
                    const float kp_ur = pKFi->mvuRight[mit->second];
                    obs << kpUn.pt.x, kpUn.pt.y, kp_ur;

                    g2o::EdgeStereoSE3ProjectXYZ* e = new g2o::EdgeStereoSE3ProjectXYZ();

                    e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id)));
                    e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(pKFi->mnId)));
                    e->setMeasurement(obs);
                    const float &invSigma2 = pKFi->mvInvLevelSigma2[kpUn.octave];
                    Eigen::Matrix3d Info = Eigen::Matrix3d::Identity()*invSigma2;
                    e->setInformation(Info);

                    g2o::RobustKernelHuber* rk = new g2o::RobustKernelHuber;
                    e->setRobustKernel(rk);
                    rk->setDelta(thHuberStereo);

                    e->fx = pKFi->fx;
                    e->fy = pKFi->fy;
                    e->cx = pKFi->cx;
                    e->cy = pKFi->cy;
                    e->bf = pKFi->mbf;

                    optimizer.addEdge(e);
                    vpEdgesStereo.push_back(e);
                    vpEdgeKFStereo.push_back(pKFi);
                    vpMapPointEdgeStereo.push_back(pMP);
                }
            }
        }
    }

    if(pbStopFlag)
        if(*pbStopFlag)
            return;

    optimizer.initializeOptimization();
    optimizer.optimize(5);

    bool bDoMore= true;

    if(pbStopFlag)
        if(*pbStopFlag)
            bDoMore = false;

    if(bDoMore)
    {

    // Check inlier observations
    for(size_t i=0, iend=vpEdgesMono.size(); i<iend;i++)
    {
        g2o::EdgeSE3ProjectXYZ* e = vpEdgesMono[i];
        MapPoint* pMP = vpMapPointEdgeMono[i];

        if(pMP->isBad())
            continue;

        if(e->chi2()>5.991 || !e->isDepthPositive())
        {
            e->setLevel(1);
        }

        e->setRobustKernel(0);
    }

    for(size_t i=0, iend=vpEdgesStereo.size(); i<iend;i++)
    {
        g2o::EdgeStereoSE3ProjectXYZ* e = vpEdgesStereo[i];
        MapPoint* pMP = vpMapPointEdgeStereo[i];

        if(pMP->isBad())
            continue;

        if(e->chi2()>7.815 || !e->isDepthPositive())
        {
            e->setLevel(1);
        }

        e->setRobustKernel(0);
    }

    // Optimize again without the outliers

    optimizer.initializeOptimization(0);
    optimizer.optimize(10);

    }

    vector<pair<KeyFrame*,MapPoint*> > vToErase;
    vToErase.reserve(vpEdgesMono.size()+vpEdgesStereo.size());

    // Check inlier observations       
    for(size_t i=0, iend=vpEdgesMono.size(); i<iend;i++)
    {
        g2o::EdgeSE3ProjectXYZ* e = vpEdgesMono[i];
        MapPoint* pMP = vpMapPointEdgeMono[i];

        if(pMP->isBad())
            continue;

        if(e->chi2()>5.991 || !e->isDepthPositive())
        {
            KeyFrame* pKFi = vpEdgeKFMono[i];
            vToErase.push_back(make_pair(pKFi,pMP));
        }
    }

    for(size_t i=0, iend=vpEdgesStereo.size(); i<iend;i++)
    {
        g2o::EdgeStereoSE3ProjectXYZ* e = vpEdgesStereo[i];
        MapPoint* pMP = vpMapPointEdgeStereo[i];

        if(pMP->isBad())
            continue;

        if(e->chi2()>7.815 || !e->isDepthPositive())
        {
            KeyFrame* pKFi = vpEdgeKFStereo[i];
            vToErase.push_back(make_pair(pKFi,pMP));
        }
    }

    // Get Map Mutex
    unique_lock<mutex> lock(pMap->mMutexMapUpdate);

    if(!vToErase.empty())
    {
        for(size_t i=0;i<vToErase.size();i++)
        {
            KeyFrame* pKFi = vToErase[i].first;
            MapPoint* pMPi = vToErase[i].second;
            pKFi->EraseMapPointMatch(pMPi);
            pMPi->EraseObservation(pKFi);
        }
    }

    // Recover optimized data

    //Keyframes
    for(list<KeyFrame*>::iterator lit=lLocalKeyFrames.begin(), lend=lLocalKeyFrames.end(); lit!=lend; lit++)
    {
        KeyFrame* pKF = *lit;
        g2o::VertexSE3Expmap* vSE3 = static_cast<g2o::VertexSE3Expmap*>(optimizer.vertex(pKF->mnId));
        g2o::SE3Quat SE3quat = vSE3->estimate();
        pKF->SetPose(Converter::toCvMat(SE3quat));
    }

    //Points
    for(list<MapPoint*>::iterator lit=lLocalMapPoints.begin(), lend=lLocalMapPoints.end(); lit!=lend; lit++)
    {
        MapPoint* pMP = *lit;
        g2o::VertexSBAPointXYZ* vPoint = static_cast<g2o::VertexSBAPointXYZ*>(optimizer.vertex(pMP->mnId+maxKFid+1));
        pMP->SetWorldPos(Converter::toCvMat(vPoint->estimate()));
        pMP->UpdateNormalAndDepth();
    }
}


void Optimizer::OptimizeEssentialGraph(Map* pMap, KeyFrame* pLoopKF, KeyFrame* pCurKF,
                                       const LoopClosing::KeyFrameAndPose &NonCorrectedSim3,
                                       const LoopClosing::KeyFrameAndPose &CorrectedSim3,
                                       const map<KeyFrame *, set<KeyFrame *> > &LoopConnections, const bool &bFixScale)
{
    // Setup optimizer
    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(false);
    g2o::BlockSolver_7_3::LinearSolverType * linearSolver =
           new g2o::LinearSolverEigen<g2o::BlockSolver_7_3::PoseMatrixType>();
    g2o::BlockSolver_7_3 * solver_ptr= new g2o::BlockSolver_7_3(linearSolver);
    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);

    solver->setUserLambdaInit(1e-16);
    optimizer.setAlgorithm(solver);

    const vector<KeyFrame*> vpKFs = pMap->GetAllKeyFrames();
    const vector<MapPoint*> vpMPs = pMap->GetAllMapPoints();

    const unsigned int nMaxKFid = pMap->GetMaxKFid();

    vector<g2o::Sim3,Eigen::aligned_allocator<g2o::Sim3> > vScw(nMaxKFid+1);
    vector<g2o::Sim3,Eigen::aligned_allocator<g2o::Sim3> > vCorrectedSwc(nMaxKFid+1);
    vector<g2o::VertexSim3Expmap*> vpVertices(nMaxKFid+1);

    const int minFeat = 100;

    // Set KeyFrame vertices
    for(size_t i=0, iend=vpKFs.size(); i<iend;i++)
    {
        KeyFrame* pKF = vpKFs[i];
        if(pKF->isBad())
            continue;
        g2o::VertexSim3Expmap* VSim3 = new g2o::VertexSim3Expmap();

        const int nIDi = pKF->mnId;

        LoopClosing::KeyFrameAndPose::const_iterator it = CorrectedSim3.find(pKF);

        if(it!=CorrectedSim3.end())
        {
            vScw[nIDi] = it->second;
            VSim3->setEstimate(it->second);
        }
        else
        {
            Eigen::Matrix<double,3,3> Rcw = Converter::toMatrix3d(pKF->GetRotation());
            Eigen::Matrix<double,3,1> tcw = Converter::toVector3d(pKF->GetTranslation());
            g2o::Sim3 Siw(Rcw,tcw,1.0);
            vScw[nIDi] = Siw;
            VSim3->setEstimate(Siw);
        }

        if(pKF==pLoopKF)
            VSim3->setFixed(true);

        VSim3->setId(nIDi);
        VSim3->setMarginalized(false);
        VSim3->_fix_scale = bFixScale;

        optimizer.addVertex(VSim3);

        vpVertices[nIDi]=VSim3;
    }


    set<pair<long unsigned int,long unsigned int> > sInsertedEdges;

    const Eigen::Matrix<double,7,7> matLambda = Eigen::Matrix<double,7,7>::Identity();

    // Set Loop edges
    for(map<KeyFrame *, set<KeyFrame *> >::const_iterator mit = LoopConnections.begin(), mend=LoopConnections.end(); mit!=mend; mit++)
    {
        KeyFrame* pKF = mit->first;
        const long unsigned int nIDi = pKF->mnId;
        const set<KeyFrame*> &spConnections = mit->second;
        const g2o::Sim3 Siw = vScw[nIDi];
        const g2o::Sim3 Swi = Siw.inverse();

        for(set<KeyFrame*>::const_iterator sit=spConnections.begin(), send=spConnections.end(); sit!=send; sit++)
        {
            const long unsigned int nIDj = (*sit)->mnId;
            if((nIDi!=pCurKF->mnId || nIDj!=pLoopKF->mnId) && pKF->GetWeight(*sit)<minFeat)
                continue;

            const g2o::Sim3 Sjw = vScw[nIDj];
            const g2o::Sim3 Sji = Sjw * Swi;

            g2o::EdgeSim3* e = new g2o::EdgeSim3();
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(nIDj)));
            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(nIDi)));
            e->setMeasurement(Sji);

            e->information() = matLambda;

            optimizer.addEdge(e);

            sInsertedEdges.insert(make_pair(min(nIDi,nIDj),max(nIDi,nIDj)));
        }
    }

    // Set normal edges
    for(size_t i=0, iend=vpKFs.size(); i<iend; i++)
    {
        KeyFrame* pKF = vpKFs[i];

        const int nIDi = pKF->mnId;

        g2o::Sim3 Swi;

        LoopClosing::KeyFrameAndPose::const_iterator iti = NonCorrectedSim3.find(pKF);

        if(iti!=NonCorrectedSim3.end())
            Swi = (iti->second).inverse();
        else
            Swi = vScw[nIDi].inverse();

        KeyFrame* pParentKF = pKF->GetParent();

        // Spanning tree edge
        if(pParentKF)
        {
            int nIDj = pParentKF->mnId;

            g2o::Sim3 Sjw;

            LoopClosing::KeyFrameAndPose::const_iterator itj = NonCorrectedSim3.find(pParentKF);

            if(itj!=NonCorrectedSim3.end())
                Sjw = itj->second;
            else
                Sjw = vScw[nIDj];

            g2o::Sim3 Sji = Sjw * Swi;

            g2o::EdgeSim3* e = new g2o::EdgeSim3();
            e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(nIDj)));
            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(nIDi)));
            e->setMeasurement(Sji);

            e->information() = matLambda;
            optimizer.addEdge(e);
        }

        // Loop edges
        const set<KeyFrame*> sLoopEdges = pKF->GetLoopEdges();
        for(set<KeyFrame*>::const_iterator sit=sLoopEdges.begin(), send=sLoopEdges.end(); sit!=send; sit++)
        {
            KeyFrame* pLKF = *sit;
            if(pLKF->mnId<pKF->mnId)
            {
                g2o::Sim3 Slw;

                LoopClosing::KeyFrameAndPose::const_iterator itl = NonCorrectedSim3.find(pLKF);

                if(itl!=NonCorrectedSim3.end())
                    Slw = itl->second;
                else
                    Slw = vScw[pLKF->mnId];

                g2o::Sim3 Sli = Slw * Swi;
                g2o::EdgeSim3* el = new g2o::EdgeSim3();
                el->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(pLKF->mnId)));
                el->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(nIDi)));
                el->setMeasurement(Sli);
                el->information() = matLambda;
                optimizer.addEdge(el);
            }
        }

        // Covisibility graph edges
        const vector<KeyFrame*> vpConnectedKFs = pKF->GetCovisiblesByWeight(minFeat);
        for(vector<KeyFrame*>::const_iterator vit=vpConnectedKFs.begin(); vit!=vpConnectedKFs.end(); vit++)
        {
            KeyFrame* pKFn = *vit;
            if(pKFn && pKFn!=pParentKF && !pKF->hasChild(pKFn) && !sLoopEdges.count(pKFn))
            {
                if(!pKFn->isBad() && pKFn->mnId<pKF->mnId)
                {
                    if(sInsertedEdges.count(make_pair(min(pKF->mnId,pKFn->mnId),max(pKF->mnId,pKFn->mnId))))
                        continue;

                    g2o::Sim3 Snw;

                    LoopClosing::KeyFrameAndPose::const_iterator itn = NonCorrectedSim3.find(pKFn);

                    if(itn!=NonCorrectedSim3.end())
                        Snw = itn->second;
                    else
                        Snw = vScw[pKFn->mnId];

                    g2o::Sim3 Sni = Snw * Swi;

                    g2o::EdgeSim3* en = new g2o::EdgeSim3();
                    en->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(pKFn->mnId)));
                    en->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(nIDi)));
                    en->setMeasurement(Sni);
                    en->information() = matLambda;
                    optimizer.addEdge(en);
                }
            }
        }
    }

    // Optimize!
    optimizer.initializeOptimization();
    optimizer.optimize(20);

    unique_lock<mutex> lock(pMap->mMutexMapUpdate);

    // SE3 Pose Recovering. Sim3:[sR t;0 1] -> SE3:[R t/s;0 1]
    for(size_t i=0;i<vpKFs.size();i++)
    {
        KeyFrame* pKFi = vpKFs[i];

        const int nIDi = pKFi->mnId;

        g2o::VertexSim3Expmap* VSim3 = static_cast<g2o::VertexSim3Expmap*>(optimizer.vertex(nIDi));
        g2o::Sim3 CorrectedSiw =  VSim3->estimate();
        vCorrectedSwc[nIDi]=CorrectedSiw.inverse();
        Eigen::Matrix3d eigR = CorrectedSiw.rotation().toRotationMatrix();
        Eigen::Vector3d eigt = CorrectedSiw.translation();
        double s = CorrectedSiw.scale();

        eigt *=(1./s); //[R t/s;0 1]

        cv::Mat Tiw = Converter::toCvSE3(eigR,eigt);

        pKFi->SetPose(Tiw);
    }

    // Correct points. Transform to "non-optimized" reference keyframe pose and transform back with optimized pose
    for(size_t i=0, iend=vpMPs.size(); i<iend; i++)
    {
        MapPoint* pMP = vpMPs[i];

        if(pMP->isBad())
            continue;

        int nIDr;
        if(pMP->mnCorrectedByKF==pCurKF->mnId)
        {
            nIDr = pMP->mnCorrectedReference;
        }
        else
        {
            KeyFrame* pRefKF = pMP->GetReferenceKeyFrame();
            nIDr = pRefKF->mnId;
        }


        g2o::Sim3 Srw = vScw[nIDr];
        g2o::Sim3 correctedSwr = vCorrectedSwc[nIDr];

        cv::Mat P3Dw = pMP->GetWorldPos();
        Eigen::Matrix<double,3,1> eigP3Dw = Converter::toVector3d(P3Dw);
        Eigen::Matrix<double,3,1> eigCorrectedP3Dw = correctedSwr.map(Srw.map(eigP3Dw));

        cv::Mat cvCorrectedP3Dw = Converter::toCvMat(eigCorrectedP3Dw);
        pMP->SetWorldPos(cvCorrectedP3Dw);

        pMP->UpdateNormalAndDepth();
    }
}

int Optimizer::OptimizeSim3(KeyFrame *pKF1, KeyFrame *pKF2, vector<MapPoint *> &vpMatches1, g2o::Sim3 &g2oS12, const float th2, const bool bFixScale)
{
    g2o::SparseOptimizer optimizer;
    g2o::BlockSolverX::LinearSolverType * linearSolver;

    linearSolver = new g2o::LinearSolverDense<g2o::BlockSolverX::PoseMatrixType>();

    g2o::BlockSolverX * solver_ptr = new g2o::BlockSolverX(linearSolver);

    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(solver_ptr);
    optimizer.setAlgorithm(solver);

    // Calibration
    const cv::Mat &K1 = pKF1->mK;
    const cv::Mat &K2 = pKF2->mK;

    // Camera poses
    const cv::Mat R1w = pKF1->GetRotation();
    const cv::Mat t1w = pKF1->GetTranslation();
    const cv::Mat R2w = pKF2->GetRotation();
    const cv::Mat t2w = pKF2->GetTranslation();

    // Set Sim3 vertex
    g2o::VertexSim3Expmap * vSim3 = new g2o::VertexSim3Expmap();    
    vSim3->_fix_scale=bFixScale;
    vSim3->setEstimate(g2oS12);
    vSim3->setId(0);
    vSim3->setFixed(false);
    vSim3->_principle_point1[0] = K1.at<float>(0,2);
    vSim3->_principle_point1[1] = K1.at<float>(1,2);
    vSim3->_focal_length1[0] = K1.at<float>(0,0);
    vSim3->_focal_length1[1] = K1.at<float>(1,1);
    vSim3->_principle_point2[0] = K2.at<float>(0,2);
    vSim3->_principle_point2[1] = K2.at<float>(1,2);
    vSim3->_focal_length2[0] = K2.at<float>(0,0);
    vSim3->_focal_length2[1] = K2.at<float>(1,1);
    optimizer.addVertex(vSim3);

    // Set MapPoint vertices
    const int N = vpMatches1.size();
    const vector<MapPoint*> vpMapPoints1 = pKF1->GetMapPointMatches();
    vector<g2o::EdgeSim3ProjectXYZ*> vpEdges12;
    vector<g2o::EdgeInverseSim3ProjectXYZ*> vpEdges21;
    vector<size_t> vnIndexEdge;

    vnIndexEdge.reserve(2*N);
    vpEdges12.reserve(2*N);
    vpEdges21.reserve(2*N);

    const float deltaHuber = sqrt(th2);

    int nCorrespondences = 0;

    for(int i=0; i<N; i++)
    {
        if(!vpMatches1[i])
            continue;

        MapPoint* pMP1 = vpMapPoints1[i];
        MapPoint* pMP2 = vpMatches1[i];

        const int id1 = 2*i+1;
        const int id2 = 2*(i+1);

        const int i2 = pMP2->GetIndexInKeyFrame(pKF2);

        if(pMP1 && pMP2)
        {
            if(!pMP1->isBad() && !pMP2->isBad() && i2>=0)
            {
                g2o::VertexSBAPointXYZ* vPoint1 = new g2o::VertexSBAPointXYZ();
                cv::Mat P3D1w = pMP1->GetWorldPos();
                cv::Mat P3D1c = R1w*P3D1w + t1w;
                vPoint1->setEstimate(Converter::toVector3d(P3D1c));
                vPoint1->setId(id1);
                vPoint1->setFixed(true);
                optimizer.addVertex(vPoint1);

                g2o::VertexSBAPointXYZ* vPoint2 = new g2o::VertexSBAPointXYZ();
                cv::Mat P3D2w = pMP2->GetWorldPos();
                cv::Mat P3D2c = R2w*P3D2w + t2w;
                vPoint2->setEstimate(Converter::toVector3d(P3D2c));
                vPoint2->setId(id2);
                vPoint2->setFixed(true);
                optimizer.addVertex(vPoint2);
            }
            else
                continue;
        }
        else
            continue;

        nCorrespondences++;

        // Set edge x1 = S12*X2
        Eigen::Matrix<double,2,1> obs1;
        const cv::KeyPoint &kpUn1 = pKF1->mvKeysUn[i];
        obs1 << kpUn1.pt.x, kpUn1.pt.y;

        g2o::EdgeSim3ProjectXYZ* e12 = new g2o::EdgeSim3ProjectXYZ();
        e12->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id2)));
        e12->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
        e12->setMeasurement(obs1);
        const float &invSigmaSquare1 = pKF1->mvInvLevelSigma2[kpUn1.octave];
        e12->setInformation(Eigen::Matrix2d::Identity()*invSigmaSquare1);

        g2o::RobustKernelHuber* rk1 = new g2o::RobustKernelHuber;
        e12->setRobustKernel(rk1);
        rk1->setDelta(deltaHuber);
        optimizer.addEdge(e12);

        // Set edge x2 = S21*X1
        Eigen::Matrix<double,2,1> obs2;
        const cv::KeyPoint &kpUn2 = pKF2->mvKeysUn[i2];
        obs2 << kpUn2.pt.x, kpUn2.pt.y;

        g2o::EdgeInverseSim3ProjectXYZ* e21 = new g2o::EdgeInverseSim3ProjectXYZ();

        e21->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(id1)));
        e21->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(optimizer.vertex(0)));
        e21->setMeasurement(obs2);
        float invSigmaSquare2 = pKF2->mvInvLevelSigma2[kpUn2.octave];
        e21->setInformation(Eigen::Matrix2d::Identity()*invSigmaSquare2);

        g2o::RobustKernelHuber* rk2 = new g2o::RobustKernelHuber;
        e21->setRobustKernel(rk2);
        rk2->setDelta(deltaHuber);
        optimizer.addEdge(e21);

        vpEdges12.push_back(e12);
        vpEdges21.push_back(e21);
        vnIndexEdge.push_back(i);
    }

    // Optimize!
    optimizer.initializeOptimization();
    optimizer.optimize(5);

    // Check inliers
    int nBad=0;
    for(size_t i=0; i<vpEdges12.size();i++)
    {
        g2o::EdgeSim3ProjectXYZ* e12 = vpEdges12[i];
        g2o::EdgeInverseSim3ProjectXYZ* e21 = vpEdges21[i];
        if(!e12 || !e21)
            continue;

        if(e12->chi2()>th2 || e21->chi2()>th2)
        {
            size_t idx = vnIndexEdge[i];
            vpMatches1[idx]=static_cast<MapPoint*>(NULL);
            optimizer.removeEdge(e12);
            optimizer.removeEdge(e21);
            vpEdges12[i]=static_cast<g2o::EdgeSim3ProjectXYZ*>(NULL);
            vpEdges21[i]=static_cast<g2o::EdgeInverseSim3ProjectXYZ*>(NULL);
            nBad++;
        }
    }

    int nMoreIterations;
    if(nBad>0)
        nMoreIterations=10;
    else
        nMoreIterations=5;

    if(nCorrespondences-nBad<10)
        return 0;

    // Optimize again only with inliers

    optimizer.initializeOptimization();
    optimizer.optimize(nMoreIterations);

    int nIn = 0;
    for(size_t i=0; i<vpEdges12.size();i++)
    {
        g2o::EdgeSim3ProjectXYZ* e12 = vpEdges12[i];
        g2o::EdgeInverseSim3ProjectXYZ* e21 = vpEdges21[i];
        if(!e12 || !e21)
            continue;

        if(e12->chi2()>th2 || e21->chi2()>th2)
        {
            size_t idx = vnIndexEdge[i];
            vpMatches1[idx]=static_cast<MapPoint*>(NULL);
        }
        else
            nIn++;
    }

    // Recover optimized Sim3
    g2o::VertexSim3Expmap* vSim3_recov = static_cast<g2o::VertexSim3Expmap*>(optimizer.vertex(0));
    g2oS12= vSim3_recov->estimate();

    return nIn;
}


} //namespace ORB_SLAM
