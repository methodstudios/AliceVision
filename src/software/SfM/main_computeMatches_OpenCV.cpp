
// Copyright (c) 2012, 2013 Pierre MOULON.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.


#include "openMVG/image/image.hpp"
#include "openMVG/features/features.hpp"

/// Generic Image Collection image matching
#include "openMVG/matching_image_collection/Matcher_AllInMemory.hpp"
#include "openMVG/matching_image_collection/GeometricFilter.hpp"
#include "openMVG/matching_image_collection/F_ACRobust.hpp"
#include "openMVG/matching_image_collection/E_ACRobust.hpp"
#include "openMVG/matching_image_collection/H_ACRobust.hpp"
#include "software/SfM/pairwiseAdjacencyDisplay.hpp"
#include "software/SfM/SfMIOHelper.hpp"
#include "openMVG/matching/matcher_brute_force.hpp"
#include "openMVG/matching/matcher_kdtree_flann.hpp"
#include "openMVG/matching/indMatch_utils.hpp"
#include "openMVG/matching/metric_hamming.hpp"

#include "openMVG/system/timer.hpp"

#include "third_party/cmdLine/cmdLine.h"

// OpenCV Includes
#include "opencv2/core/eigen.hpp" //To Convert Eigen matrix to cv matrix
// Legacy free features
#include "opencv2/features2d/features2d.hpp"
// Patent protected features
#include "opencv2/nonfree/features2d.hpp"

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <iterator>
#include <vector>

using namespace openMVG;
using namespace openMVG::matching;
using namespace openMVG::robust;
using namespace std;

enum eGeometricModel
{
  FUNDAMENTAL_MATRIX = 0,
  ESSENTIAL_MATRIX = 1,
  HOMOGRAPHY_MATRIX = 2
};

// Equality functor to count the number of similar K matrices in the essential matrix case.
bool testIntrinsicsEquality(
  SfMIO::IntrinsicCameraInfo const &ci1,
  SfMIO::IntrinsicCameraInfo const &ci2)
{
  return ci1.m_K == ci2.m_K;
}

/// Extract OpenCV features and convert them to openMVG features/descriptor data
template <class DescriptorT, class cvFeature2DInterfaceT>
static bool ComputeCVFeatAndDesc(const Image<unsigned char>& I,
  std::vector<SIOPointFeature>& feats,
  std::vector<DescriptorT >& descs)
{
  //Convert image to OpenCV data
  cv::Mat img;
  cv::eigen2cv(I.GetMat(), img);

  cvFeature2DInterfaceT detectAndDescribeClass;

  std::vector< cv::KeyPoint > vec_keypoints;
  cv::Mat m_desc;

  detectAndDescribeClass(img, cv::Mat(), vec_keypoints, m_desc);

  if (!vec_keypoints.empty())
  {
    feats.reserve(vec_keypoints.size());
    descs.reserve(vec_keypoints.size());

    DescriptorT descriptor;
    int cpt = 0;
    for(std::vector< cv::KeyPoint >::const_iterator i_keypoint = vec_keypoints.begin();
      i_keypoint != vec_keypoints.end(); ++i_keypoint, ++cpt){

      SIOPointFeature feat((*i_keypoint).pt.x, (*i_keypoint).pt.y, (*i_keypoint).size, (*i_keypoint).angle);
      feats.push_back(feat);

      memcpy(descriptor.getData(),
             m_desc.ptr<typename DescriptorT::bin_type>(cpt),
             DescriptorT::static_size*sizeof(typename DescriptorT::bin_type));
      descs.push_back(descriptor);
    }
    return true;
  }
  return false;
}

/// Extract features and descriptor and save them to files
template<class KeypointSetT, class DescriptorT, class cvFeature2DInterfaceT>
void extractFeaturesAndDescriptors(
  const std::vector<std::string> & vec_fileNames, // input filenames
  const std::string & sOutDir,  // Output directory where features and descriptor will be saved
  std::vector<std::pair<size_t, size_t> > & vec_imagesSize) // input image size (w,h)
{
  vec_imagesSize.resize(vec_fileNames.size());
  Image<RGBColor> imageRGB;
  Image<unsigned char> imageGray;

  C_Progress_display my_progress_bar( vec_fileNames.size() );
  for(size_t i=0; i < vec_fileNames.size(); ++i)  {
    KeypointSetT kpSet;

    std::string sFeat = stlplus::create_filespec(sOutDir,
      stlplus::basename_part(vec_fileNames[i]), "feat");
    std::string sDesc = stlplus::create_filespec(sOutDir,
      stlplus::basename_part(vec_fileNames[i]), "desc");

    //Test if descriptor and feature was already computed
    if (stlplus::file_exists(sFeat) && stlplus::file_exists(sDesc)) {

      if (ReadImage(vec_fileNames[i].c_str(), &imageRGB)) {
        vec_imagesSize[i] = make_pair(imageRGB.Width(), imageRGB.Height());
      }
      else {
        ReadImage(vec_fileNames[i].c_str(), &imageGray);
        vec_imagesSize[i] = make_pair(imageGray.Width(), imageGray.Height());
      }
    }
    else  { //Not already computed, so compute and save

      if (!ReadImage(vec_fileNames[i].c_str(), &imageGray))
        continue;

      // Compute features and descriptors and export them to file
      ComputeCVFeatAndDesc<DescriptorT, cvFeature2DInterfaceT>(imageGray,  kpSet.features(), kpSet.descriptors());
      kpSet.saveToBinFile(sFeat, sDesc);
      vec_imagesSize[i] = make_pair(imageGray.Width(), imageGray.Height());
    }
    ++my_progress_bar;
  }
}

int main(int argc, char **argv)
{
  CmdLine cmd;

  std::string sImaDirectory;
  std::string sOutDir = "";
  std::string sGeometricModel = "f";
  float fDistRatio = .6f;
  int iMatchingVideoMode = -1;
  std::string sPredefinedPairList = "";

  cmd.add( make_option('i', sImaDirectory, "imadir") );
  cmd.add( make_option('o', sOutDir, "outdir") );
  cmd.add( make_option('r', fDistRatio, "distratio") );
  cmd.add( make_option('g', sGeometricModel, "geometricModel") );
  cmd.add( make_option('v', iMatchingVideoMode, "videoModeMatching") );
  cmd.add( make_option('l', sPredefinedPairList, "pairList") );

  try {
      if (argc == 1) throw std::string("Invalid command line parameter.");
      cmd.process(argc, argv);
  } catch(const std::string& s) {
      std::cerr << "Usage: " << argv[0] << '\n'
      << "[-i|--imadir path] \n"
      << "[-o|--outdir path] \n"
      << "\n[Optional]\n"
      << "[-g]--geometricModel f, e or h]"
      << "[-v|--videoModeMatching 2 -> X] \n"
      << "\t sequence matching with an overlap of X images\n"
      << "[-l]--pairList file"
      << std::endl;

      std::cerr << s << std::endl;
      return EXIT_FAILURE;
  }

  std::cout << " You called : " <<std::endl
            << argv[0] << std::endl
            << "--imadir " << sImaDirectory << std::endl
            << "--outdir " << sOutDir << std::endl
            << "--geometricModel " << sGeometricModel << std::endl
            << "--videoModeMatching " << iMatchingVideoMode << std::endl;

  if (sPredefinedPairList.length()) {
    std::cout << "--pairList " << sPredefinedPairList << std::endl;
    if (iMatchingVideoMode>0) {
      std::cerr << "\nIncompatible options: --videoModeMatching and --pairList" << std::endl;
      return EXIT_FAILURE;
    }
  }

  if (sOutDir.empty())  {
    std::cerr << "\nIt is an invalid output directory" << std::endl;
    return EXIT_FAILURE;
  }

  eGeometricModel eGeometricModelToCompute = FUNDAMENTAL_MATRIX;
  std::string sGeometricMatchesFilename = "";
  switch(sGeometricModel[0])
  {
    case 'f': case 'F':
      eGeometricModelToCompute = FUNDAMENTAL_MATRIX;
      sGeometricMatchesFilename = "matches.f.txt";
    break;
    case 'e': case 'E':
      eGeometricModelToCompute = ESSENTIAL_MATRIX;
      sGeometricMatchesFilename = "matches.e.txt";
    break;
    case 'h': case 'H':
      eGeometricModelToCompute = HOMOGRAPHY_MATRIX;
      sGeometricMatchesFilename = "matches.h.txt";
    break;
    default:
      std::cerr << "Unknown geometric model" << std::endl;
      return EXIT_FAILURE;
  }

  // -----------------------------
  // a. List images
  // b. Compute features and descriptors
  // c. Compute putative descriptor matches
  // d. Geometric filtering of putative matches
  // e. Export some statistics
  // -----------------------------

  // Create output dir
  if (!stlplus::folder_exists(sOutDir))
    stlplus::folder_create( sOutDir );

  //---------------------------------------
  // a. List images
  //---------------------------------------
  std::string sListsFile = stlplus::create_filespec(sOutDir, "lists.txt" );
  if (!stlplus::is_file(sListsFile)) {
    std::cerr << std::endl
      << "The input file \""<< sListsFile << "\" is missing" << std::endl;
    return false;
  }

  std::vector<openMVG::SfMIO::CameraInfo> vec_camImageName;
  std::vector<openMVG::SfMIO::IntrinsicCameraInfo> vec_focalGroup;
  if (!openMVG::SfMIO::loadImageList( vec_camImageName,
                                      vec_focalGroup,
                                      sListsFile) )
  {
    std::cerr << "\nEmpty or invalid image list." << std::endl;
    return false;
  }

  //-- Two alias to ease access to image filenames and image sizes
  std::vector<std::string> vec_fileNames;
  std::vector<std::pair<size_t, size_t> > vec_imagesSize;
  for ( std::vector<openMVG::SfMIO::CameraInfo>::const_iterator
    iter_camInfo = vec_camImageName.begin();
    iter_camInfo != vec_camImageName.end();
    iter_camInfo++ )
  {
    vec_imagesSize.push_back( std::make_pair( vec_focalGroup[iter_camInfo->m_intrinsicId].m_w,
                                              vec_focalGroup[iter_camInfo->m_intrinsicId].m_h ) );
    vec_fileNames.push_back( stlplus::create_filespec( sImaDirectory, iter_camInfo->m_sImageName) );
  }

  //---------------------------------------
  // b. Compute features and descriptor
  //    - extract sift features and descriptor
  //    - if keypoints already computed, re-load them
  //    - else save features and descriptors on disk
  //---------------------------------------

  //-- Make your choice about the Feature Detector your want to use
  //--  Note that the openCV + openMVG interface is working only for
  //     floating point descriptor.

  //-- Surf opencv => default 64 floating point values
  typedef cv::SURF cvFeature2DInterfaceT;
  typedef Descriptor<float, 64> DescriptorT;
  std::cout << "\nUse the opencv SURF interface" << std::endl;

  //-- Sift opencv => default 128 floating point values
  //typedef cv::SIFT cvFeature2DInterfaceT;
  //typedef Descriptor<float, 128> DescriptorT;
  //std::cout << "\nUse the opencv SIFT interface" << std::endl;

  typedef SIOPointFeature FeatureT;
  typedef std::vector<FeatureT> FeatsT;
  typedef vector<DescriptorT > DescsT;
  typedef KeypointSet<FeatsT, DescsT > KeypointSetT;


  std::cout << "\n\n - EXTRACT FEATURES - " << std::endl;
  {
    Timer timer;
    extractFeaturesAndDescriptors<KeypointSetT, DescriptorT, cvFeature2DInterfaceT>(
      vec_fileNames, // input filenames
      sOutDir,  // Output directory where features and descriptor will be saved
      vec_imagesSize);
    std::cout << "Task done in (s): " << timer.elapsed() << std::endl;
  }

  //---------------------------------------
  // c. Compute putative descriptor matches
  //    - L2 descriptor matching
  //    - Keep correspondences only if NearestNeighbor ratio is ok
  //---------------------------------------
  PairWiseMatches map_PutativesMatches;
  // Define the matcher and the used metric (Squared L2)
  // ANN matcher could be defined as follow:
  typedef flann::L2<DescriptorT::bin_type> MetricT;
  typedef ArrayMatcher_Kdtree_Flann<DescriptorT::bin_type, MetricT> MatcherT;
  // Brute force matcher can be defined as following:
  //typedef L2_Vectorized<DescriptorT::bin_type> MetricT;
  //typedef ArrayMatcherBruteForce<DescriptorT::bin_type, MetricT> MatcherT;

  std::cout << std::endl << " - PUTATIVE MATCHES - " << std::endl;
  // If the matches already exists, reload them
  if (stlplus::file_exists(sOutDir + "/matches.putative.txt"))
  {
    PairedIndMatchImport(sOutDir + "/matches.putative.txt", map_PutativesMatches);
    std::cout << "\t PREVIOUS RESULTS LOADED" << std::endl;
  }
  else // Compute the putatives matches
  {
    std::cout << "Use: "
      << ((iMatchingVideoMode > 0) ? "sequence matching" : ((sPredefinedPairList.length()) ? sPredefinedPairList : "exhaustive matching" ) ) << std::endl;

    Timer timer;
    Matcher_AllInMemory<KeypointSetT, MatcherT> collectionMatcher(fDistRatio);
    if (collectionMatcher.loadData(vec_fileNames, sOutDir))
    {
      // Get pair to match according the matching mode:
      const PairsT pairs =
        (iMatchingVideoMode > 0) ?
        contiguousWithOverlap(vec_fileNames.size(), iMatchingVideoMode) :
        (sPredefinedPairList.length()) ?
        predefinedPairs(sPredefinedPairList) : exhaustivePairs(vec_fileNames.size());

      if (pairs.empty()) {
        std::cerr << "Empty pair list" << std::endl;
        return EXIT_FAILURE;
      }
      // Photometric matching of putative pairs
      collectionMatcher.Match(vec_fileNames, pairs, map_PutativesMatches);
      //---------------------------------------
      //-- Export putative matches
      //---------------------------------------
      std::ofstream file (std::string(sOutDir + "/matches.putative.txt").c_str());
      if (file.is_open())
        PairedIndMatchToStream(map_PutativesMatches, file);
      file.close();
    }
    std::cout << "Task done in (s): " << timer.elapsed() << std::endl;
  }
  //-- export putative matches Adjacency matrix
  PairWiseMatchingToAdjacencyMatrixSVG(vec_fileNames.size(),
    map_PutativesMatches,
    stlplus::create_filespec(sOutDir, "PutativeAdjacencyMatrix", "svg"));


  //---------------------------------------
  // d. Geometric filtering of putative matches
  //    - AContrario Estimation of the desired geometric model
  //    - Use an upper bound for the a contrario estimated threshold
  //---------------------------------------
  PairWiseMatches map_GeometricMatches;

  ImageCollectionGeometricFilter<FeatureT> collectionGeomFilter;
  const double maxResidualError = 4.0;
  if (collectionGeomFilter.loadData(vec_fileNames, sOutDir))
  {
    Timer timer;
    std::cout << std::endl << " - GEOMETRIC FILTERING - " << std::endl;
    switch (eGeometricModelToCompute)
    {
      case FUNDAMENTAL_MATRIX:
      {
       collectionGeomFilter.Filter(
          GeometricFilter_FMatrix_AC(maxResidualError),
          map_PutativesMatches,
          map_GeometricMatches,
          vec_imagesSize);
      }
      break;
      case ESSENTIAL_MATRIX:
      {
        // Build the intrinsic parameter map for each image
        std::map<size_t, Mat3> map_K;
        size_t cpt = 0;
        for ( std::vector<openMVG::SfMIO::CameraInfo>::const_iterator
          iter_camInfo = vec_camImageName.begin();
          iter_camInfo != vec_camImageName.end();
          ++iter_camInfo, ++cpt )
        {
          if (vec_focalGroup[iter_camInfo->m_intrinsicId].m_bKnownIntrinsic)
            map_K[cpt] = vec_focalGroup[iter_camInfo->m_intrinsicId].m_K;
        }

        collectionGeomFilter.Filter(
          GeometricFilter_EMatrix_AC(map_K, maxResidualError),
          map_PutativesMatches,
          map_GeometricMatches,
          vec_imagesSize);

        //-- Perform an additional check to remove pairs with poor overlap
        std::vector<PairWiseMatches::key_type> vec_toRemove;
        for (PairWiseMatches::const_iterator iterMap = map_GeometricMatches.begin();
          iterMap != map_GeometricMatches.end(); ++iterMap)
        {
          size_t putativePhotometricCount = map_PutativesMatches.find(iterMap->first)->second.size();
          size_t putativeGeometricCount = iterMap->second.size();
          float ratio = putativeGeometricCount / (float)putativePhotometricCount;
          if (putativeGeometricCount < 50 || ratio < .3f)  {
            // the pair will be removed
            vec_toRemove.push_back(iterMap->first);
          }
        }
        //-- remove discarded pairs
        for (std::vector<PairWiseMatches::key_type>::const_iterator
          iter =  vec_toRemove.begin(); iter != vec_toRemove.end(); ++iter)
        {
          map_GeometricMatches.erase(*iter);
        }
      }
      break;
      case HOMOGRAPHY_MATRIX:
      {
        collectionGeomFilter.Filter(
          GeometricFilter_HMatrix_AC(maxResidualError),
          map_PutativesMatches,
          map_GeometricMatches,
          vec_imagesSize);
      }
      break;
    }

    //---------------------------------------
    //-- Export geometric filtered matches
    //---------------------------------------
    std::ofstream file (string(sOutDir + "/" + sGeometricMatchesFilename).c_str());
    if (file.is_open())
      PairedIndMatchToStream(map_GeometricMatches, file);
    file.close();

    std::cout << "Task done in (s): " << timer.elapsed() << std::endl;

    //-- export Adjacency matrix
    std::cout << "\n Export Adjacency Matrix of the pairwise's geometric matches"
      << std::endl;
    PairWiseMatchingToAdjacencyMatrixSVG(vec_fileNames.size(),
      map_GeometricMatches,
      stlplus::create_filespec(sOutDir, "GeometricAdjacencyMatrix", "svg"));
  }
  return EXIT_SUCCESS;
}
