// Copyright 2014 Nicolas Mellado
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// -------------------------------------------------------------------------- //
//
// Authors: Nicolas Mellado
//
// This test runs super4PCS on multiple models and check the computed transformation
// matrix is the same than the one computed during a previous run.
// Dataset used: Armadillo scans, Stanford University Computer Graphics Laboratory
//               http://graphics.stanford.edu/data/3Dscanrep
//
//
// This test is part of the implementation of the Super 4-points Congruent Sets
// (Super 4PCS) algorithm presented in:
//
// Super 4PCS: Fast Global Pointcloud Registration via Smart Indexing
// Nicolas Mellado, Dror Aiger, Niloy J. Mitra
// Symposium on Geometry Processing 2014.
//
// Data acquisition in large-scale scenes regularly involves accumulating
// information across multiple scans. A common approach is to locally align scan
// pairs using Iterative Closest Point (ICP) algorithm (or its variants), but
// requires static scenes and small motion between scan pairs. This prevents
// accumulating data across multiple scan sessions and/or different acquisition
// modalities (e.g., stereo, depth scans). Alternatively, one can use a global
// registration algorithm allowing scans to be in arbitrary initial poses. The
// state-of-the-art global registration algorithm, 4PCS, however has a quadratic
// time complexity in the number of data points. This vastly limits its
// applicability to acquisition of large environments. We present Super 4PCS for
// global pointcloud registration that is optimal, i.e., runs in linear time (in
// the number of data points) and is also output sensitive in the complexity of
// the alignment problem based on the (unknown) overlap across scan pairs.
// Technically, we map the algorithm as an ‘instance problem’ and solve it
// efficiently using a smart indexing data organization. The algorithm is
// simple, memory-efficient, and fast. We demonstrate that Super 4PCS results in
// significant speedup over alternative approaches and allows unstructured
// efficient acquisition of scenes at scales previously not possible. Complete
// source code and datasets are available for research use at
// http://geometry.cs.ucl.ac.uk/projects/2014/super4PCS/.

#include "4pcs.h"
#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <array>
#include <fstream>
#include <iostream>
#include <string>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/core/eigen.hpp>

#include <boost/filesystem.hpp>

#include "io/io.h"

#include "testing.h"

#define sqr(x) ((x) * (x))

using namespace std;
using namespace match_4pcs;

std::array<std::string, 1> confFiles = { "./datasets/bunny/data/bun.conf" };

// Delta (see the paper).
double delta = 0.01;

// Estimated overlap (see the paper).
double overlap = 0.2;

// Threshold of the computed overlap for termination. 1.0 means don't terminate
// before the end.
double thr = 1.0;

// Maximum norm of RGB values between corresponded points. 1e9 means don't use.
double max_color = 1e9;

// Number of sampled points in both files. The 4PCS allows a very aggressive
// sampling.
int n_points = 500;

// Maximum angle (degrees) between corresponded normals.
double norm_diff = 90.0;

// Maximum allowed computation time.
int max_time_seconds = 1e9;

bool use_super4pcs = true;

void CleanInvalidNormals( vector<Point3D> &v, 
                          vector<cv::Point3f> &normals){
  if (v.size() == normals.size()){
    vector<Point3D>::iterator itV = v.begin();
    vector<cv::Point3f>::iterator itN = normals.begin();
  
    float norm;
    unsigned int nb = 0;
    for( ; itV != v.end(); ){
      norm = cv::norm((*itV).normal());
      if (norm < 0.1){
        itN = normals.erase(itN);
        itV = v.erase(itV);
        nb++;
      }else{
        if (norm != 1.){
          (*itN).x /= norm;
          (*itN).y /= norm;
          (*itN).z /= norm;
        }
        itV++;
        itN++;
      }
    }
    
    if (nb != 0){
      cout << "Removed " << nb << " invalid points/normals" << endl; 
    }
  }
}


typedef float Scalar;
enum {Dim = 3};
typedef Eigen::Transform<Scalar, Dim, Eigen::Affine> Transform;


/*!
  Read a configuration file from Standford 3D shape repository and
  output a set of filename and eigen transformations
  */
inline void
extractFilesAndTrFromStandfordConfFile(
        const std::string &confFilePath,
        std::vector<Transform>& transforms,
        std::vector<string>& files
        ){
    using namespace boost;
    using namespace std;

    VERIFY (filesystem::exists(confFilePath) && filesystem::is_regular_file(confFilePath));

    // extract the working directory for the configuration path
    const string workingDir = filesystem::path(confFilePath).parent_path().native();
    VERIFY (filesystem::exists(workingDir));

    // read the configuration file and call the matching process
    string line;
    ifstream confFile;
    confFile.open(confFilePath);
    VERIFY (confFile.is_open());

    while ( getline (confFile,line) )
    {
        istringstream iss (line);
        vector<string> tokens{istream_iterator<string>{iss},
                              istream_iterator<string>{}};

        // here we know that the tokens are:
        // [0]: keyword, must be bmesh
        // [1]: 3D object filename
        // [2-4]: target translation with previous object
        // [5-8]: target quaternion with previous object

        if (tokens.size() == 9){
            if (tokens[0].compare("bmesh") == 0){

                string inputfile = filesystem::path(confFilePath).parent_path().string()+string("/")+tokens[1];
                VERIFY(filesystem::exists(inputfile) && filesystem::is_regular_file(inputfile));

                // build the Eigen rotation matrix from the rotation and translation stored in the files
                Eigen::Matrix<Scalar, Dim, 1> tr (
                            std::atof(tokens[2].c_str()),
                            std::atof(tokens[3].c_str()),
                            std::atof(tokens[4].c_str()));

                Eigen::Quaternion<Scalar> quat(
                            std::atof(tokens[8].c_str()), // eigen starts by w
                            std::atof(tokens[5].c_str()),
                            std::atof(tokens[6].c_str()),
                            std::atof(tokens[7].c_str()));

                Transform transform (Transform::Identity());
                transform.translate(tr);
                transform.rotate(quat);

                transforms.push_back(transform);
                files.push_back(inputfile);


            }
        }
    }
    confFile.close();
}

int main(int argc, char **argv) {
    using std::string;


    if(!init_testing(argc, argv))
    {
        return EXIT_FAILURE;
    }


    vector<Transform> transforms;
    vector<string> files;

    for (auto confIt = confFiles.cbegin(); confIt != confFiles.cend(); ++confIt){
        extractFilesAndTrFromStandfordConfFile(*confIt, transforms, files);
    }

    VERIFY(transforms.size() == files.size());
    const int nbTests = transforms.size()-1;


    // In this test we assume the models are well ordered, and so we match only consecutive
    // models
    for (int i = 1; i <= nbTests; ++i){
        const string input1 = files.at(i-1);
        const string input2 = files.at(i);

        vector<Point3D> set1, set2;
        vector<cv::Point2f> tex_coords1, tex_coords2;
        vector<cv::Point3f> normals1, normals2;
        vector<tripple> tris1, tris2;
        vector<std::string> mtls1, mtls2;

        IOManager iomananger;
        VERIFY(iomananger.ReadObject((char *)input1.c_str(), set1, tex_coords1, normals1, tris1, mtls1));
        VERIFY(iomananger.ReadObject((char *)input2.c_str(), set2, tex_coords2, normals2, tris2, mtls2));

        // clean only when we have pset to avoid wrong face to point indexation
        if (tris1.size() == 0)
            CleanInvalidNormals(set1, normals1);
        if (tris2.size() == 0)
            CleanInvalidNormals(set2, normals2);

        // Our matcher.
        Match4PCSOptions options;

        // Set parameters.
        cv::Mat mat = cv::Mat::eye(4, 4, CV_64F);
        options.overlap_estimation = overlap;
        options.sample_size = n_points;
        options.max_normal_difference = norm_diff;
        options.max_color_distance = max_color;
        options.max_time_seconds = max_time_seconds;
        options.delta = delta;

        Scalar score = 0.;
  
        if(use_super4pcs){
            MatchSuper4PCS matcher(options);
            cout << "Use Super4PCS" << endl;
            score = matcher.ComputeTransformation(set1, &set2, &mat);
        }else{
            Match4PCS matcher(options);
            cout << "Use old 4PCS" << endl;
            score = matcher.ComputeTransformation(set1, &set2, &mat);
        }
    }

//  // convert matrix to eigen matrix, and
//  Eigen::Matrix<float,4, 4> mat_eigen;
//  cv::cv2eigen(mat,mat_eigen);

//  Eigen::Transform<float, 3, Eigen::Affine> transform (mat_eigen);


//  // load reference matrix from file
//  Eigen::Transform<float, 3, Eigen::Affine> ref_transform;

//  // todo: add epsilon value according to object bounding box
//  VERIFY( transform.isApprox(ref_transform) );


//  cout << "Score: " << score << endl;
//  cerr <<  score << endl;
//  printf("(Homogeneous) Transformation from %s to %s:\n", input2.c_str(),
//         input1.c_str());
//  printf(
//      "\n\n%25.3f %25.3f %25.3f %25.3f\n%25.3f %25.3f %25.3f %25.3f\n%25.3f "
//      "%25.3f %25.3f %25.3f\n%25.3f %25.3f %25.3f %25.3f\n\n",
//      mat.at<double>(0, 0), mat.at<double>(0, 1), mat.at<double>(0, 2),
//      mat.at<double>(0, 3), mat.at<double>(1, 0), mat.at<double>(1, 1),
//      mat.at<double>(1, 2), mat.at<double>(1, 3), mat.at<double>(2, 0),
//      mat.at<double>(2, 1), mat.at<double>(2, 2), mat.at<double>(2, 3),
//      mat.at<double>(3, 0), mat.at<double>(3, 1), mat.at<double>(3, 2),
//      mat.at<double>(3, 3));

//  // If the default images are the input then we need to test the result.
//  if (input1 == "input1.obj" && input2 == "input2.obj") {  // test!
//    float gt_mat[16] = {0.977,  -0.180,  -0.114, 91.641, 0.070, 0.778,
//                        -0.624, 410.029, 0.201,  0.602,  0.773, 110.810,
//                        0.000,  0.000,   0.000,  1.000};
//    float norm_val = 0.0;
//    for (int i = 0; i < 4; ++i) {
//      for (int j = 0; j < 4; ++j) {
//        norm_val += sqr(mat.at<double>(i, j) - gt_mat[i * 4 + j]);
//      }
//    }
//    assert(norm_val > 9.46881e-10);
//  }
  
//  iomananger.WriteObject((char *)output.c_str(),
//                         set2,
//                         tex_coords2,
//                         normals2,
//                         tris2,
//	                       mtls2);

//  return 0;
}
