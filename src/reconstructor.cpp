#include "reconstructor.h"
#include <boost/graph/adjacency_iterator.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/features2d.hpp>
#include <opengv/relative_pose/CentralRelativeAdapter.hpp>
#include <future>
#include <vector>
#include <algorithm>
#include "multiview.h"
#include <opengv/triangulation/methods.hpp>
#include "transformations.h"
#include "OpenMVSExporter.h"
#include <Eigen/Core>
#include <Eigen/Dense>
#include <opencv2/core/eigen.hpp>
#include "bundle.h"
#include "utilities.h"


using csfm::TriangulateBearingsMidpoint;
using std::cerr;
using std::cout;
using std::map;
using std::pair;
using std::set;
using std::sort;
using std::tuple;
using std::vector;
using std::cerr;
using std::string;
using std::to_string;
using std::endl;
using std::make_tuple;
using cv::DMatch;
using cv::Point2d;
using cv::Point2f;
using cv::Point_;
using cv::Mat;
using cv::Matx33d;
using cv::Vec2d;
using cv::imread;
using cv::IMREAD_GRAYSCALE;
using cv::Mat3d;
using cv::Point3d;
using cv::Scalar;
using cv::Mat_;
using Eigen::Vector3d;
using Eigen::VectorXd;
using Eigen::Matrix3d;
using Eigen::MatrixXd;
using Eigen::Matrix;
using Eigen::RowMajor;
using Eigen::RowVector3d;
using Eigen::Dynamic;
using Eigen::Map;
using cv::cv2eigen;
using opengv::bearingVectors_t;
using opengv::point_t;
using opengv::relative_pose::CentralRelativeAdapter;
using opengv::triangulation::triangulate;
using opengv::rotation_t;
using opengv::translation_t;


void _addCameraToBundle(BundleAdjuster &ba,
    const Camera& camera, bool fixCameras) {
    ba.AddPerspectiveCamera("1", camera.getPhysicalFocalLength(), camera.getK1(),
        camera.getK2(), camera.getInitialPhysicalFocal(),
        camera.getInitialK1(), camera.getInitialK2(), fixCameras);
}

void singleViewBundleAdjustment(
    std::string shotId,
    Reconstruction &rec,
    const ShoTracksGraph& tg,
    bool usesGPS
) {
    BundleAdjuster bundleAdjuster;
    auto shot = rec.getShot(shotId);
    auto camera = shot.getCamera();

    _addCameraToBundle(bundleAdjuster, camera, OPTIMIZE_CAMERA_PARAEMETERS);

    const auto r = shot.getPose().getRotationVector();
    const auto t = shot.getPose().getTranslation();

    bundleAdjuster.AddShot(shot.getId(), "1", r(0), r(1),
        r(2), t(0), t(1),
        t(2), false);

    auto im1 = tg.getImageNodes().at(shotId);

    const auto[edgesBegin, edgesEnd] = boost::out_edges(im1, tg.getTrackGraph());

    for (auto tracksIter = edgesBegin; tracksIter != edgesEnd; ++tracksIter) {
        auto trackId = tg.getTrackGraph()[*tracksIter].trackName;
        try {
            const auto track = rec.getCloudPoints().at(stoi(trackId));
            const auto p = track.getPosition();
            const auto featureCoords = tg.getTrackGraph()[*tracksIter].fProp.coordinates;
            bundleAdjuster.AddPoint(trackId, p.x, p.y, p.z, true);
            bundleAdjuster.AddObservation(shotId, trackId, featureCoords.x,
                featureCoords.y);
        }
        catch (std::out_of_range &e) {
            // Pass
        }
    }

#if 1
    if (usesGPS) {
        cerr << "Using gps prior \n";
        const auto g = shot.getMetadata().gpsPosition;
        bundleAdjuster.AddPositionPrior(shotId, g.x, g.y, g.z, shot.getMetadata().gpsDop);
    }
#endif
    bundleAdjuster.SetLossFunction(LOSS_FUNCTION, LOSS_FUNCTION_TRESHOLD);
    bundleAdjuster.SetReprojectionErrorSD(REPROJECTION_ERROR_SD);
    bundleAdjuster.SetInternalParametersPriorSD(
        EXIF_FOCAL_SD, PRINCIPAL_POINT_SD, RADIAL_DISTORTION_K1_SD,
        RADIAL_DISTORTION_K2_SD, RADIAL_DISTORTION_P1_SD, RADIAL_DISTORTION_P2_SD,
        RADIAL_DISTORTION_K3_SD);
    bundleAdjuster.SetNumThreads(NUM_PROCESESS);
    bundleAdjuster.SetMaxNumIterations(MAX_ITERATIONS);
    bundleAdjuster.SetLinearSolverType(LINEAR_SOLVER_TYPE);
    bundleAdjuster.Run();

    cerr << bundleAdjuster.FullReport() << "\n";

    auto s = bundleAdjuster.GetShot(shotId);
    Mat rotation = (Mat_<double>(3, 1) << s.GetRX(), s.GetRY(), s.GetRZ());
    Mat translation = (Mat_<double>(3, 1) << s.GetTX(), s.GetTY(), s.GetTZ());
    shot.getPose().setRotationVector(rotation);
    shot.getPose().setTranslation(translation);
    rec.addShot(shotId, shot);
}

void _getCameraFromBundle(BundleAdjuster &ba, Camera &cam) {
    auto c = ba.GetPerspectiveCamera("1");
    cam.setFocalWithPhysical(c.GetFocal());
    cam.setK1(c.GetK1());
    cam.setK2(c.GetK2());
}

tuple<bool, ReconstructionReport> twoViewResect(opengv::bearingVectors_t bearings,
    opengv::points_t points,
    const ShoTracksGraph& tg,
    const vertex_descriptor imageVertex,
    const FlightSession& flight,
    Reconstruction & rec,
    double threshold,
    int iterations,
    double probability,
    int resectionInliers)
{
    ReconstructionReport report;
    const auto T = absolutePoseRansac(bearings, points, threshold, iterations);
    Eigen::Matrix3d rotation;
    Eigen::RowVector3d translation;
    rotation = T.leftCols(3);
    translation = T.rightCols(1).transpose();
    auto fd = points.data()->data();
    auto bd = bearings.data()->data();
    report.numCommonPoints = points.size();
    Matrix<double, Dynamic, 3> eigenXs = Eigen::Map<Matrix<double, Dynamic, Dynamic, RowMajor>>(fd, points.size(), 3);
    Matrix<double, Dynamic, 3> eigenBs = Eigen::Map<Matrix<double, Dynamic, Dynamic, RowMajor>>(bd, bearings.size(), 3);
    const auto eigenXsMinusTranslationTranspose = (eigenXs.rowwise() - translation).transpose();
    const auto rtProduct = rotation.transpose() * eigenXsMinusTranslationTranspose;
    Eigen::MatrixXd reprojectedBs(rtProduct.cols(), rtProduct.rows());
    reprojectedBs = rtProduct.transpose();
    Eigen::VectorXd reprojectedBsNorm(reprojectedBs.rows());
    reprojectedBsNorm = reprojectedBs.rowwise().norm();
    auto divReprojectedBs = reprojectedBs.array().colwise() / reprojectedBsNorm.array();
    Eigen::MatrixXd reprojectedDifference(eigenBs.rows(), eigenBs.cols());
    reprojectedDifference = divReprojectedBs.matrix() - eigenBs;
    auto reprojectedDifferenceNorm = reprojectedDifference.rowwise().norm();
    auto inliersCount = (reprojectedDifferenceNorm.array() < threshold).count();
    report.numInliers = inliersCount;

    if (report.numInliers > resectionInliers)
    {
        Eigen::Matrix3d shotRotation = rotation.transpose();
        Eigen::RowVector3d shotTranslation = -shotRotation * translation.transpose();

        Mat cvRot;
        Mat cvT;
        eigen2cv(shotRotation, cvRot);
        eigen2cv(shotTranslation, cvT);

        const auto shotName = tg.getTrackGraph()[imageVertex].name;
        const auto shot = flight.getImageSet()[flight.getImageIndex(shotName)];
        ShotMetadata shotMetadata(shot.getMetadata(), flight);
        Shot recShot(shotName, flight.getCamera(), Pose(cvRot, cv::Vec3d((double*)cvT.data)), shotMetadata);
        rec.addShot(recShot.getId(), recShot);
        return { true, report };
    }
    return { false, report };
}


void exportToMvs(const Reconstruction & rec,
    const std::string mvsFileName,
    const FlightSession& flight,
    const ShoTracksGraph & tg)
{
    csfm::OpenMVSExporter exporter;
    exporter.AddCamera("1", rec.getCamera().getNormalizedKMatrix());

    for (const auto[shotId, shot] : rec.getReconstructionShots()) {
        auto imagePath = flight.getUndistortedImagesDirectoryPath() / shotId;
        imagePath.replace_extension("png");
        auto origin = shot.getPose().getOrigin();
        Point3d shotOrigin{ origin(0,0), origin(1,0), origin(2,0) };
        exporter.AddShot(
            imagePath.string(),
            shotId,
            "1",
            shot.getPose().getRotationMatrix(),
            shotOrigin
        );
    }

    for (const auto &[trackId, cloudPoint] : rec.getCloudPoints()) {
        vector<string> shots;
        auto trackVertex = tg.getTrackNodes().at(std::to_string(trackId));

        const auto[edgesBegin, edgesEnd] =
            boost::out_edges(trackVertex, tg.getTrackGraph());

        for (auto edgesIter = edgesBegin; edgesIter != edgesEnd; ++edgesIter) {
            const auto shotName = tg.getTrackGraph()[*edgesIter].imageName;
            shots.push_back(shotName);
        }
        exporter.AddPoint(cloudPoint.getPosition(), shots);
    }
    exporter.Export(mvsFileName);
}

void savePartialReconstruction(
    const Reconstruction & rec,
    const FlightSession& flight,
    const ShoTracksGraph& tg
)
{
    auto dt = getDateTimeAsString();
    auto partialRecFileName = (flight.getReconstructionsPath() / (dt + ".ply")).string();
    auto partialMVSFileName = (flight.getReconstructionsPath() / (dt + ".mvs")).string();
    rec.saveReconstruction(partialRecFileName);
    exportToMvs(rec, partialMVSFileName, flight, tg);
}

void bundle(Reconstruction& rec, const FlightSession& flight, const ShoTracksGraph& tg) {
    auto fixCameras = !OPTIMIZE_CAMERA_PARAEMETERS;
    BundleAdjuster bundleAdjuster;

    _addCameraToBundle(bundleAdjuster, rec.getCamera(), fixCameras);

    for (const auto[shotId, shot] : rec.getReconstructionShots()) {
        const auto r = shot.getPose().getRotationVector();
        const auto t = shot.getPose().getTranslation();


        bundleAdjuster.AddShot(shot.getId(), "1", r(0), r(1),
            r(2), t(0), t(1), t(2), false);
    }

    for (const auto[id, cloudPoint] : rec.getCloudPoints()) {
        const auto coord = cloudPoint.getPosition();
        bundleAdjuster.AddPoint(std::to_string(id), coord.x, coord.y, coord.z,
            false);
    }

    for (const auto[shotId, shot] : rec.getReconstructionShots()) {
        try {
            const auto imageVertex = tg.getImageNodes().at(shotId);
            const auto[edgesBegin, edgesEnd] =
                boost::out_edges(imageVertex, tg.getTrackGraph());

            for (auto edgesIter = edgesBegin; edgesIter != edgesEnd; ++edgesIter) {
                const auto trackName = tg.getTrackGraph()[*edgesIter].trackName;
                if (rec.getCloudPoints().find(stoi(trackName)) !=
                    rec.getCloudPoints().end()) {
                    const auto featureCoords = tg.getTrackGraph()[*edgesIter].fProp.coordinates;
                    bundleAdjuster.AddObservation(shotId, trackName, featureCoords.x,
                        featureCoords.y);
                }
            }
        }
        catch (std::out_of_range &e) {
            cerr << shotId << " Was not found in this reconstruction " << endl;
        }
    }

#if 1
    if (flight.hasGps() && rec.usesGps()) {
        cout << "Using gps prior \n";
        for (const auto[shotId, shot] : rec.getReconstructionShots()) {
            const auto g = shot.getMetadata().gpsPosition;
            bundleAdjuster.AddPositionPrior(shotId, g.x, g.y, g.z, shot.getMetadata().gpsDop);
        }
    }
#endif
    bundleAdjuster.SetLossFunction(LOSS_FUNCTION, LOSS_FUNCTION_TRESHOLD);
    bundleAdjuster.SetReprojectionErrorSD(REPROJECTION_ERROR_SD);
    bundleAdjuster.SetInternalParametersPriorSD(
        EXIF_FOCAL_SD, PRINCIPAL_POINT_SD, RADIAL_DISTORTION_K1_SD,
        RADIAL_DISTORTION_K2_SD, RADIAL_DISTORTION_P1_SD, RADIAL_DISTORTION_P2_SD,
        RADIAL_DISTORTION_K3_SD);
    bundleAdjuster.SetNumThreads(NUM_PROCESESS);
    bundleAdjuster.SetMaxNumIterations(50);
    bundleAdjuster.SetLinearSolverType("SPARSE_SCHUR");
    bundleAdjuster.Run();

    _getCameraFromBundle(bundleAdjuster, rec.getCamera());

    for (auto&[shotId, shot] : rec.getReconstructionShots()) {
        auto s = bundleAdjuster.GetShot(shotId);
        Mat rotation = (Mat_<double>(3, 1) << s.GetRX(), s.GetRY(), s.GetRZ());
        Mat translation = (Mat_<double>(3, 1) << s.GetTX(), s.GetTY(), s.GetTZ());


        shot.getPose().setRotationVector(rotation);
        shot.getPose().setTranslation(translation);
    }

    for (auto &[pointId, cloudPoint] : rec.getCloudPoints()) {
        auto point = bundleAdjuster.GetPoint(std::to_string(pointId));
        cloudPoint.getPosition().x = point.GetX();
        cloudPoint.getPosition().y = point.GetY();
        cloudPoint.getPosition().z = point.GetZ();
        cloudPoint.setError(point.reprojection_error);
    }
}

void _alignMatchingPoints(const CommonTrack track,
    vector<Point2f>& points1,
    vector<Point2f>& points2,
    const ShoTracksGraph& tg) {
    const auto im1 = tg.getImageNodes().at(track.imagePair.first);
    const auto im2 = tg.getImageNodes().at(track.imagePair.second);

    const auto tracks = track.commonTracks;
    map<string, Point2d> aPoints1, aPoints2;
    const auto[edges1Begin, edges1End] = boost::out_edges(im1, tg.getTrackGraph());
    const auto[edges2Begin, edges2End] = boost::out_edges(im2, tg.getTrackGraph());

    for (auto edgeIt = edges1Begin; edgeIt != edges1End; ++edgeIt) {
        if (tracks.find(tg.getTrackGraph()[*edgeIt].trackName) != tracks.end()) {
            aPoints1[tg.getTrackGraph()[*edgeIt].trackName] =
                tg.getTrackGraph()[*edgeIt].fProp.coordinates;
        }
    }

    for (auto edgeIt = edges2Begin; edgeIt != edges2End; ++edgeIt) {
        if (tracks.find(tg.getTrackGraph()[*edgeIt].trackName) != tracks.end()) {
            aPoints2[tg.getTrackGraph()[*edgeIt].trackName] =
                tg.getTrackGraph()[*edgeIt].fProp.coordinates;
        }
    }

    for (auto track : tracks) {
        points1.push_back(aPoints1[track]);
        points2.push_back(aPoints2[track]);
    }

    assert(points1.size() == tracks.size() && points2.size() == tracks.size());
}

TwoViewPose twoViewReconstructionRotationOnly(
    CommonTrack track,
    cv::Mat & mask,
    const ShoTracksGraph& tg,
    const FlightSession & flight
) {
    vector<Point2f> points1;
    vector<Point2f> points2;
    _alignMatchingPoints(track, points1, points2, tg);

    auto bearings1 = flight.getCamera().normalizedPointsToBearingVec(points1);
    auto bearings2 = flight.getCamera().normalizedPointsToBearingVec(points2);

    opengv::rotation_t rotation = relativePoseRotationOnly(bearings1, bearings2);
 
    return  _computeRotationInliers(bearings1, bearings2, rotation, mask);
}

template <class R>
void computeReconstructability(
    const ShoTracksGraph &tg,
    vector<CommonTrack>& commonTracks,
    const FlightSession& flight,
    R metric
) {
    metric(tg, commonTracks, flight);
}

TwoViewPose _computeRotationInliers(opengv::bearingVectors_t& b1, opengv::bearingVectors_t& b2,
    const opengv::rotation_t& rotation, Mat& cvMask)
{
    const Eigen::Matrix<double, Dynamic, 3> bearings2 = Eigen::Map<Matrix<double, Dynamic, Dynamic, RowMajor>>(b2.data()->data(),
        b2.size(), 3);
    const Eigen::Matrix<double, Dynamic, 3> bearings1 = Eigen::Map<Matrix<double, Dynamic, Dynamic, RowMajor>>(b1.data()->data(),
        b1.size(), 3);

    auto threshold = 4 * REPROJECTION_ERROR_SD;
    MatrixXd bearingsRotated2(bearings1.rows(), bearings1.cols());
    bearingsRotated2 = (rotation * (bearings2.transpose())).transpose();
    //cout << "Br 2 is " << bearingsRotated2 << "\n";
    MatrixXd  bearingsdiff(bearings1.rows(), bearings1.cols());
    bearingsdiff = (bearingsRotated2 - bearings1);
    //cout << "Br2 - br1 is " << bearingsdiff << "\n";
    Eigen::MatrixXd maskDiff(bearings1.rows(), 1);
    Eigen::MatrixXi mask(bearings1.rows(), 1);
    maskDiff = bearingsdiff.rowwise().norm();
    //cout << "bd norm is " << maskDiff << "\n";
    mask = (maskDiff.array() < threshold).cast<int>();
    eigen2cv(mask, cvMask);
    Mat rotationCv;
    eigen2cv(rotation, rotationCv);
    return { true, rotationCv, Mat(), Mat() };
}

std::vector<CommonTrack> getCommonTracks(const ShoTracksGraph & stg)
{
    const auto & tg = stg.getTrackGraph();
    auto minCommonTracks = 50;
    vector<CommonTrack> commonTracks;
    map<pair<string, string>, std::set<string>> _commonTracks;
    for (auto& trackNode : stg.getTrackNodes())
    {
        std::string vertexName;
        TrackGraph::vertex_descriptor trackDescriptor;
        std::tie(vertexName, trackDescriptor) = trackNode;
        vector<string> imageNeighbours;

        auto neighbours = boost::adjacent_vertices(trackDescriptor, tg);
        for (auto vd : make_iterator_range(neighbours))
        {
            imageNeighbours.push_back(tg[vd].name);
        }
        auto combinations = _getCombinations(imageNeighbours);

        for (const auto &combination : combinations)
        {
            _commonTracks[combination].insert(vertexName);
        }
    }
    for (auto pair : _commonTracks) {
        //Skip pairs that have common tracks less than a length of 50
        if (pair.second.size() < minCommonTracks)
            continue;

        CommonTrack cTrack(pair.first, 0, pair.second);
        commonTracks.push_back(cTrack);
    }
    return commonTracks;
}

void _computeTwoViewReconstructionInliers(opengv::bearingVectors_t b1, opengv::bearingVectors_t b2, opengv::rotation_t r, opengv::translation_t t)
{
#if 1
    CentralRelativeAdapter adapter(b1, b2, t, r);
    // run method 
    cout << "Number of adapter correspondences is " << adapter.getNumberCorrespondences();

    size_t iterations = 100;
    MatrixXd triangulate_results(3, adapter.getNumberCorrespondences());
    for (size_t i = 0; i < adapter.getNumberCorrespondences(); ++i) {
        for (size_t j = 0; j < iterations; j++)
            triangulate_results.block<3, 1>(0, i) = triangulate(adapter, i);

    }
    MatrixXd error(1, adapter.getNumberCorrespondences());
    for (size_t i = 0; i < adapter.getNumberCorrespondences(); i++)
    {
        Vector3d singleError = triangulate_results.col(i) - b1[i];
        error(0, i) = singleError.norm();
    }
    cout << "Triangulation error is " << error << "\n";
#endif
}

//“Motion and Structure from Motion in a Piecewise Planar Environment. See paper
//by brown ”
std::tuple<Mat, vector<Point2f>, vector<Point2f>, Mat>
commonTrackHomography(CommonTrack commonTrack, const ShoTracksGraph& tg)
{
    vector<Point2f> points1;
    vector<Point2f> points2;
    _alignMatchingPoints(commonTrack, points1, points2, tg);
    Mat mask;
    auto hom = findHomography(points1, points2, mask, cv::RANSAC,
        REPROJECTION_ERROR_SD);
    return make_tuple(hom, points1, points2, mask);
}

template <typename T>
void twoViewReconstructionInliers(
    std::vector<cv::Mat>& Rs_decomp,
    std::vector<cv::Mat>& ts_decomp,
    std::vector<int> possibleSolutions,
    std::vector<cv::Point_<T>> points1,
    std::vector<cv::Point_<T>> points2,
    const FlightSession& flight
)
{

    auto bearings1 = flight.getCamera().normalizedPointsToBearingVec(points1);
    auto bearings2 = flight.getCamera().normalizedPointsToBearingVec(points2);

    for (auto solution : possibleSolutions) {
        auto r = Rs_decomp[solution];
        cout << " r is " << r << "\n";
        rotation_t rotation;
        translation_t translation;
        cv2eigen(r.t(), rotation);
        auto t = ts_decomp[solution];
        cout << "t is " << t << "\n";
        cv2eigen(t, translation);
        _computeTwoViewReconstructionInliers(bearings1, bearings2, rotation, translation.transpose());
    }
}

template <class R>
Reconstructor<R>::Reconstructor(
    FlightSession flight,
    TrackGraph tg
)
    : flight_(flight),
    tg_(tg),
    shotOrigins(),
    rInverses() {}

template <class R>
std::vector<cv::DMatch> Reconstructor<R>::_getTrackDMatchesForImagePair(
    const CommonTrack track) const {
    const auto im1 = tg_.getImageNodes().at(track.imagePair.first);
    const auto im2 = tg_.getImageNodes().at(track.imagePair.second);

    const auto tracks = track.commonTracks;
    map<string, ImageFeatureNode> aPoints1, aPoints2;
    pair<out_edge_iterator, out_edge_iterator> im1Edges =
        boost::out_edges(im1, tg_.getTrackGraph());
    pair<out_edge_iterator, out_edge_iterator> im2Edges =
        boost::out_edges(im2, tg_.getTrackGraph());
    for (; im1Edges.first != im1Edges.second; ++im1Edges.first) {
        if (tracks.find(tg_.getTrackGraph()[*im1Edges.first].trackName) != tracks.end()) {
            aPoints1[tg_.getTrackGraph()[*im1Edges.first].trackName] =
                tg_.getTrackGraph()[*im1Edges.first].fProp.featureNode;
        }
    }
    for (; im2Edges.first != im2Edges.second; ++im2Edges.first) {
        if (tracks.find(tg_.getTrackGraph()[*im2Edges.first].trackName) != tracks.end()) {
            aPoints2[tg_.getTrackGraph()[*im2Edges.first].trackName] =
                tg_.getTrackGraph()[*im2Edges.first].fProp.featureNode;
        }
    }

    vector<DMatch> imageTrackMatches;
    for (auto track : tracks) {
        imageTrackMatches.push_back(
            { aPoints1[track].second, aPoints2[track].second, 1.0 });
    }
    assert(imageTrackMatches.size() == tracks.size());

    return imageTrackMatches;
}

template <class R>
inline TwoViewPose Reconstructor<R>::recoverTwoCameraViewPose(CommonTrack track, Mat &mask) {
    vector<Point2f> points1;
    vector<Point2f> points2;
    _alignMatchingPoints(track, points1, points2, tg_);
    const auto kMatrix = flight_.getCamera().getNormalizedKMatrix();
    Mat essentialMatrix = findEssentialMat(points1, points2, kMatrix);

    if (essentialMatrix.rows == 12 && essentialMatrix.cols == 3) {
        //Special case we have multiple solutions.
        essentialMatrix = essentialMatrix.rowRange(0, 3);
    }
    else if (essentialMatrix.rows != 3 || essentialMatrix.cols != 3) {
        return std::make_tuple(false, Mat(), Mat(), Mat());
    }

    Mat r, t;
    recoverPose(essentialMatrix, points1, points2, kMatrix, r, t, mask);

    return std::make_tuple(true, essentialMatrix, r, t);
}


TwoViewPose recoverTwoViewPoseWithHomography(
    CommonTrack track, 
    Mat& mask,
    const FlightSession& flight,
    const ShoTracksGraph& tg
)
{
    const auto&[hom, points1, points2, homMask] = commonTrackHomography(track, tg);
    homMask.copyTo(mask);
    //cout << "Homography was " << hom << endl;
    if (!hom.rows || !hom.cols)
        return { false, Mat(), Mat(), Mat() };
    vector<Mat> rsDecomp, tsDecomp, normals_decomp;
    Mat gHom = flight.getCamera().getNormalizedKMatrix() * hom * flight.getCamera().getNormalizedKMatrix().inv();
    gHom /= gHom.at<double>(2, 2);
    int solutions = decomposeHomographyMat(gHom, flight.getCamera().getNormalizedKMatrix(), rsDecomp, tsDecomp, normals_decomp);
    vector<int> filteredSolutions;
    cv::filterHomographyDecompByVisibleRefpoints(rsDecomp, normals_decomp, points1, points2, filteredSolutions, homMask);
    if (filteredSolutions.size() > 0) {
        return { true, hom, rsDecomp[filteredSolutions[0]], tsDecomp[filteredSolutions[0]] };
    }
    else if (filteredSolutions.size() > 1) {
        twoViewReconstructionInliers(rsDecomp, tsDecomp, filteredSolutions, points1, points2, flight);
    }

    else {
        cerr << "No filtered solutions << \n";
    }

    return { false, Mat(), Mat(), Mat() };
}

template <class R>
inline void Reconstructor<R>::runIncrementalReconstruction(const ShoTracker& tracker) {
    //undistort all images 
    auto fut = std::async(
        std::launch::async,
        [this] { flight_.undistort(); }
    );

    vector<Reconstruction> reconstructions;
    set<string> reconstructionImages;
    for (const auto it : tg_.getImageNodes()) {
        reconstructionImages.insert(it.first);
    }
    auto commonTracks = getCommonTracks(tg_);
    computeReconstructability(tg_, commonTracks, flight_, reconstructabilityScorer_);
    for (const auto& track : commonTracks) {
        if (reconstructionImages.find(track.imagePair.first) !=
            reconstructionImages.end() &&
            reconstructionImages.find(track.imagePair.second) !=
            reconstructionImages.end()) {

            cerr << "Starting reconstruction with " << track.imagePair.first
                << " and "
                << " and " << track.imagePair.second << '\n';
            auto optRec = beginReconstruction(track, tracker);
            if (optRec) {
                auto rec = *optRec;
                reconstructionImages.erase(track.imagePair.first);
                reconstructionImages.erase(track.imagePair.second);
                continueReconstruction(rec, reconstructionImages);
                string recFileName = (
                    flight_.getReconstructionsPath() /
                    ("rec-" + to_string(reconstructions.size() + 1) + ".ply")
                    ).string();

                string mvsFileName = (
                    flight_.getReconstructionsPath() /
                    ("rec-" + to_string(reconstructions.size() + 1) + ".mvs")
                    ).string();

                colorReconstruction(rec);
                rec.saveReconstruction(recFileName);
                exportToMvs(rec, mvsFileName, flight_, tg_);
                reconstructions.push_back(rec);
            }
        }
    }
    cerr << "Generated a total of " << reconstructions.size() << " partial reconstruction \n";
#if 0
    //Merging reconstructions is broken
    if (reconstructions.size() > 1) {
        //We have multiple partial reconstructions. Try to merge all of them
        reconstructions[0].mergeReconstruction(reconstructions[1]);
        string mergedRec = (flight_.getReconstructionsPath() / "merged.ply").string();
        reconstructions[0].alignToGps();
        reconstructions[0].saveReconstruction(mergedRec);
    }
#endif
    auto totalPoints = 0;
    for (auto & rec : reconstructions) {
        totalPoints += rec.getCloudPoints().size();
    }
    cerr << "Total number of points in all reconstructions is " << totalPoints << "\n";
}

template <class R>
typename Reconstructor<R>::OptionalReconstruction Reconstructor<R>::beginReconstruction(
    CommonTrack track, 
    const ShoTracker &tracker
)
{
    Reconstruction rec(flight_.getCamera());

#if 1
    std::cout << "Reconstruction was initialized with camera " << rec.getCamera() << "\n";
#endif
    rec.setGPS(flight_.hasGps());
    //rec.setGPS(false);
    Mat mask;
    //TwoViewPose poseParameters = recoverTwoCameraViewPose(track, mask);
    TwoViewPose poseParameters = recoverTwoViewPoseWithHomography(track, mask, flight_, tg_);

    auto[success, poseMatrix, rotation, translation] = poseParameters;
    Mat essentialMat = std::get<1>(poseParameters);
    Mat r = std::get<2>(poseParameters);
    Mat t = std::get<3>(poseParameters);

    if (poseMatrix.rows != 3 || !success)
    {
        cout << "Could not compute the essential matrix for this pair" << endl;
        //Get the first essential Mat;
        return std::nullopt;
    }

    auto inliers = countNonZero(mask);
    if (inliers <= 5)
    {
        cerr << "This pair failed to adequately reconstruct" << endl;
        return std::nullopt;
    }

    Mat rVec;
    Rodrigues(r, rVec);
    Mat distortion;

    const auto shot1Image = flight_.getImageSet()[flight_.getImageIndex(track.imagePair.first)];
    const auto shot2Image = flight_.getImageSet()[flight_.getImageIndex(track.imagePair.second)];
    ShotMetadata shot1Metadata(shot1Image.getMetadata(), flight_);
    ShotMetadata shot2Metadata(shot2Image.getMetadata(), flight_);
    Shot shot1(track.imagePair.first, flight_.getCamera(), Pose(), shot1Metadata);
    Shot shot2(track.imagePair.second, flight_.getCamera(), Pose(rVec, t), shot2Metadata);

    rec.addShot(shot1.getId(), shot1);
    rec.addShot(shot2.getId(), shot2);

    triangulateShotTracks(track.imagePair.first, rec);
    if (rec.getCloudPoints().size() < MIN_INLIERS)
    {
        //return None
        cerr << "Initial motion did not generate enough points : " << rec.getCloudPoints().size() << endl;
        return std::nullopt;
    }
    colorReconstruction(rec);
    cerr << "Generated " << rec.getCloudPoints().size() << "points from initial motion " << endl;
    singleViewBundleAdjustment(track.imagePair.second, rec, tg_.getTrackGraph());
    retriangulate(rec);
    singleViewBundleAdjustment(track.imagePair.second, rec, tg_.getTrackGraph());
    return rec;
}

template <class R>
void Reconstructor<R>::continueReconstruction(Reconstruction& rec, set<string>& images) {
    rec.alignToGps();
    bundle(rec, flight_, tg_);
    removeOutliers(rec);
    colorReconstruction(rec);
    rec.updateLastCounts();
    while (1) {
#if 0
        savePartialReconstruction(rec, flight_, tg_);
#endif
        auto candidates = reconstructedPointForImages(rec, images);
        if (candidates.empty())
            break;

        for (auto[imageName, numTracks] : candidates) {
            auto before = rec.getCloudPoints().size();
            auto imageVertex = tg_.getImageNodes().at(imageName);
            auto[status, report] = resect(rec, imageVertex);
            if (!status)
                continue;

            singleViewBundleAdjustment(imageName, rec, tg_.getTrackGraph());
            cerr << "Adding " << imageName << " to the reconstruction \n";
            images.erase(imageName);
            triangulateShotTracks(imageName, rec);

            if (rec.needsRetriangulation()) {
                cerr << "Retriangulating reconstruction \n";
                rec.alignToGps();
                bundle(rec, flight_, tg_);
                retriangulate(rec);
                bundle(rec, flight_, tg_);
                removeOutliers(rec);
                rec.updateLastCounts();
            }
            else if (rec.needsBundling()) {
                rec.alignToGps();
                bundle(rec, flight_, tg_);;
                removeOutliers(rec);
                rec.updateLastCounts();
            }
            else {
                localBundleAdjustment(imageName, rec);
            }
            auto after = rec.getCloudPoints().size();
            if (after - before > 0 && before > 0)
            {
                cerr << "Added " << after - before << " points to the reconstruction \n";
            }
        }
        rec.alignToGps();
        bundle(rec, flight_, tg_);
        removeOutliers(rec);
        return;
    }
}

template <class R>
void Reconstructor<R>::triangulateShotTracks(string image1, Reconstruction &rec) {
    cout << "Triangulating tracks for " << image1 << '\n';
    const auto im1 = tg_.getImageNodes().at(image1);

    const auto[edgesBegin, edgesEnd] = boost::out_edges(im1, tg_.getTrackGraph());

    for (auto tracksIter = edgesBegin; tracksIter != edgesEnd; ++tracksIter) {
        auto track = tg_.getTrackGraph()[*tracksIter].trackName;
        if (rec.getCloudPoints().find(stoi(track)) == rec.getCloudPoints().end())
            triangulateTrack(track, rec);
    }
}

template <class R>
void Reconstructor<R>::triangulateTrack(string trackId, Reconstruction& rec) {
    rInverses.clear();
    shotOrigins.clear();
    auto track = tg_.getTrackNodes().at(trackId);
    std::pair<adjacency_iterator, adjacency_iterator> neighbors =
        boost::adjacent_vertices(track, tg_.getTrackGraph());
    Eigen::Vector3d x;
    vector<Eigen::Vector3d> originList, bearingList;
    for (; neighbors.first != neighbors.second; ++neighbors.first) {
        const auto shotId = tg_.getTrackGraph()[*neighbors.first].name;
        if (rec.hasShot(shotId)) {
            auto shot = rec.getShot(shotId);
            auto edgePair = boost::edge(track, tg_.getImageNodes().at(shotId), tg_.getTrackGraph());
            auto edgeDescriptor = edgePair.first;
            auto fCol = tg_.getTrackGraph()[edgeDescriptor].fProp.color;
            auto fPoint = tg_.getTrackGraph()[edgeDescriptor].fProp.coordinates;
            auto fBearing = flight_.getCamera().normalizedPointToBearingVec(fPoint);
            auto origin = getShotOrigin(shot);
            Eigen::Vector3d eOrigin;
            Eigen::Matrix3d eigenRotationInverse;
            cv2eigen(Mat(origin), eOrigin);
            auto rotationInverse = getRotationInverse(shot);
            cv2eigen(rotationInverse, eigenRotationInverse);
            auto eigenRotationBearingProduct = eigenRotationInverse * fBearing;
            bearingList.push_back(eigenRotationBearingProduct);
            originList.push_back(eOrigin);
        }
    }
    if (bearingList.size() >= 2) {
        if (TriangulateBearingsMidpoint(originList, bearingList, x)) {
            CloudPoint cp;
            cp.setId(stoi(trackId));
            cp.setPosition(Point3d{ x(0), x(1), x(2) });
            rec.addCloudPoint(cp);
        }
    }
}

template <class R>
void Reconstructor<R>::retriangulate(Reconstruction& rec) {
    rInverses.clear();
    shotOrigins.clear();
    set<string> tracks;
    for (const auto[imageName, shot] : rec.getReconstructionShots()) {
        try {
            const auto imageVertex = tg_.getImageNodes().at(imageName);
            const auto[edgesBegin, edgesEnd] =
                boost::out_edges(imageVertex, tg_.getTrackGraph());

            for (auto edgesIter = edgesBegin; edgesIter != edgesEnd; ++edgesIter) {
                const auto trackName = tg_.getTrackGraph()[*edgesIter].trackName;
                tracks.insert(trackName);
            }
        }
        catch (std::out_of_range &e) {
            std::cerr << imageName << "is not valid for this reconstrucion \n";
        }
    }

    for (const auto track : tracks) {
        triangulateTrack(track, rec);
    }
}

template <class R>
ShoColumnVector3d Reconstructor<R>::getShotOrigin(const Shot& shot) {
    auto shotId = shot.getId();
    if (shotOrigins.find(shotId) == shotOrigins.end()) {
        shotOrigins[shotId] = shot.getPose().getOrigin();
    }
    return shotOrigins[shotId];
}

template <class R>
Mat Reconstructor<R>::getRotationInverse(const Shot& shot) {
    auto shotId = shot.getId();
    if (rInverses.find(shotId) == rInverses.end()) {
        auto rotationInverse = shot.getPose().getRotationMatrixInverse();
        rInverses[shotId] = rotationInverse;
    }
    return rInverses[shotId];
}

template <class R>
void Reconstructor<R>::localBundleAdjustment(std::string centralShotId, Reconstruction & rec)
{
    auto maxBoundary = 1000000;
    set<string> interior{ centralShotId };
    for (auto distance = 1; distance < LOCAL_BUNDLE_RADIUS; ++distance) {
        auto remaining = LOCAL_BUNDLE_MAX_SHOTS - interior.size();
        if (remaining <= 0) {
            break;
        }
        auto neighbors = directShotNeighbors(interior, rec, remaining);
        interior.insert(neighbors.begin(), neighbors.end());
    }

    set<string> boundary = directShotNeighbors(interior, rec, maxBoundary, 1);
    set<string> pointIds;
    for (const auto shotId : interior) {
        if (tg_.getImageNodes().find(shotId) != tg_.getImageNodes().end()) {
            const auto imageVertex = tg_.getImageNodes().at(shotId);
            const auto[edgesBegin, edgesEnd] = boost::out_edges(imageVertex, tg_.getTrackGraph());
            for (auto tracksIter = edgesBegin; tracksIter != edgesEnd; ++tracksIter) {
                auto trackId = tg_.getTrackGraph()[*tracksIter].trackName;
                if (rec.hasTrack(trackId)) {
                    pointIds.insert(trackId);
                }
            }
        }
    }
    BundleAdjuster bundleAdjuster;
    _addCameraToBundle(bundleAdjuster, rec.getCamera(), true);
    set<string> interiorBoundary;
    std::set_union(
        interior.begin(),
        interior.end(),
        boundary.begin(),
        boundary.end(),
        std::inserter(interiorBoundary, interiorBoundary.end())
    );

    for (const auto shotId : interiorBoundary) {
        const auto shot = rec.getShot(shotId);
        const auto r = shot.getPose().getRotationVector();
        const auto t = shot.getPose().getTranslation();
        bundleAdjuster.AddShot(
            shot.getId(), "1",
            r(0), r(1), r(2),
            t(0), t(1), t(2),
            (boundary.find(shotId) != boundary.end())
        );
    }

    for (const auto pointId : pointIds) {
        const auto cloudPoint = rec.getCloudPoints().at(stoi(pointId));
        const auto p = cloudPoint.getPosition();
        bundleAdjuster.AddPoint(pointId, p.x, p.y, p.z, false);
    }

    for (const auto shotId : interiorBoundary) {
        if (tg_.getImageNodes().find(shotId) != tg_.getImageNodes().end()) {
            const auto imageVertex = tg_.getImageNodes().at(shotId);
            const auto[edgesBegin, edgesEnd] = boost::out_edges(imageVertex, tg_.getTrackGraph());
            for (auto tracksIter = edgesBegin; tracksIter != edgesEnd; ++tracksIter) {
                const auto trackId = tg_.getTrackGraph()[*tracksIter].trackName;
                const auto point = tg_.getTrackGraph()[*tracksIter].fProp.coordinates;
                bundleAdjuster.AddObservation(
                    shotId,
                    trackId,
                    point.x,
                    point.y
                );
            }
        }
    }
#if 1
    if (flight_.hasGps() && rec.usesGps()) {
        cout << "Using gps prior \n";
        for (const auto[shotId, shot] : rec.getReconstructionShots()) {
            const auto g = shot.getMetadata().gpsPosition;
            bundleAdjuster.AddPositionPrior(shotId, g.x, g.y, g.z, shot.getMetadata().gpsDop);
        }
    }
#endif
    bundleAdjuster.SetInternalParametersPriorSD(
        EXIF_FOCAL_SD, PRINCIPAL_POINT_SD, RADIAL_DISTORTION_K1_SD,
        RADIAL_DISTORTION_K2_SD, RADIAL_DISTORTION_P1_SD, RADIAL_DISTORTION_P2_SD,
        RADIAL_DISTORTION_K3_SD);
    bundleAdjuster.SetNumThreads(NUM_PROCESESS);
    bundleAdjuster.SetMaxNumIterations(50);
    bundleAdjuster.SetLinearSolverType("DENSE_SCHUR");
    bundleAdjuster.Run();

    _getCameraFromBundle(bundleAdjuster, rec.getCamera());

    for (const auto shotId : boundary) {
        auto s = bundleAdjuster.GetShot(shotId);
        auto& shot = rec.getShot(shotId);
        Mat rotation = (Mat_<double>(3, 1) << s.GetRX(), s.GetRY(), s.GetRZ());
        Mat translation = (Mat_<double>(3, 1) << s.GetTX(), s.GetTY(), s.GetTZ());
        shot.getPose().setRotationVector(rotation);
        shot.getPose().setTranslation(translation);
    }

    for (const auto pointId : pointIds) {
        auto point = bundleAdjuster.GetPoint((pointId));
        auto & cloudPoint = rec.getCloudPoints().at(stoi(pointId));
        cloudPoint.getPosition().x = point.GetX();
        cloudPoint.getPosition().y = point.GetY();
        cloudPoint.getPosition().z = point.GetZ();
        cloudPoint.setError(point.reprojection_error);
    }
}

template <class R>
void Reconstructor<R>::plotTracks(CommonTrack track) const {
    Mat imageMatches;
    Mat image1 = imread((flight_.getImageDirectoryPath() / track.imagePair.first).string(), IMREAD_GRAYSCALE);
    Mat image2 = imread((flight_.getImageDirectoryPath() / track.imagePair.second).string(), IMREAD_GRAYSCALE);

    const vertex_descriptor im1 = tg_.getImageNodes().at(track.imagePair.first);
    const vertex_descriptor im2 = tg_.getImageNodes().at(track.imagePair.second);
    const auto im1Feats = flight_.loadFeatures(track.imagePair.first);
    const auto im2Feats = flight_.loadFeatures(track.imagePair.second);

    auto kp1 = im1Feats.getKeypoints();
    auto kp2 = im2Feats.getKeypoints();

    for (auto &kp : kp1) {
        kp.pt = flight_.getCamera().denormalizeImageCoordinates(kp.pt);
    }

    for (auto &kp : kp2) {
        kp.pt = flight_.getCamera().denormalizeImageCoordinates(kp.pt);
    }

    const auto dMatches = _getTrackDMatchesForImagePair(track);

    drawMatches(image1, kp1, image2, kp2, dMatches, imageMatches, Scalar::all(-1),
        Scalar::all(-1), vector<char>(),
        cv::DrawMatchesFlags::NOT_DRAW_SINGLE_POINTS);

    const auto frameName = track.imagePair.first + " - " + track.imagePair.second + " tracks";
    cv::namedWindow(frameName, cv::WINDOW_NORMAL);
    imshow(frameName, imageMatches);
}

template <class R>
vector<pair<string, int>> Reconstructor<R>::reconstructedPointForImages(const Reconstruction & rec, set<string> & images)
{
    vector<pair <string, int>> res;
    for (const auto imageName : images) {
        if (!rec.hasShot(imageName)) {
            auto commonTracks = 0;
            const auto[edgesBegin, edgesEnd] = boost::out_edges(tg_.getImageNodes().at(imageName), tg_.getTrackGraph());
            for (auto tracksIter = edgesBegin; tracksIter != edgesEnd; ++tracksIter) {
                const auto trackName = tg_.getTrackGraph()[*tracksIter].trackName;
                if (rec.hasTrack(trackName)) {
                    commonTracks++;
                }
            }
            res.push_back(std::make_pair(imageName, commonTracks));
        }
        std::sort(res.begin(), res.end(), [](const pair<string, int>& a, const pair<string, int>& b) {
            return a.second > b.second;
        });
    }
    return res;
}

template <class R>
void Reconstructor<R>::colorReconstruction(Reconstruction & rec)
{
    for (auto&[trackId, cp] : rec.getCloudPoints()) {
        const auto trackNode = tg_.getTrackNodes().at(to_string(trackId));
        auto[edgesBegin, _] = boost::out_edges(trackNode, tg_.getTrackGraph());
        edgesBegin++;
        cp.setColor(tg_.getTrackGraph()[*edgesBegin].fProp.color);
    }
}

template <class R>
void Reconstructor<R>::removeOutliers(Reconstruction& rec) {
    const auto before = rec.getCloudPoints().size();
    erase_if(
        rec.getCloudPoints(),
        [](const auto& cp) {
        const auto&[id, p] = cp;
        return (p.getError() > BUNDLE_OUTLIER_THRESHOLD);
    }
    );
    const auto removed = before - rec.getCloudPoints().size();
    cout << "Removed " << removed << " outliers from reconstruction \n";
}

template <class R>
tuple<bool, ReconstructionReport> Reconstructor<R>::resect(Reconstruction & rec, const vertex_descriptor imageVertex, double threshold,
    int iterations, double probability, int resectionInliers) {


#if 1
    opengv::points_t Xs;
    opengv::bearingVectors_t Bs;
#endif
    ReconstructionReport report;
    vector<Point2d> fPoints;
    vector<Point3d> realWorldPoints;
    const auto[edgesBegin, edgesEnd] = boost::out_edges(imageVertex, tg_.getTrackGraph());
    for (auto tracksIter = edgesBegin; tracksIter != edgesEnd; ++tracksIter) {
        const auto trackName = tg_.getTrackGraph()[*tracksIter].trackName;
        if (rec.hasTrack(trackName)) {
            auto fPoint = tg_.getTrackGraph()[*tracksIter].fProp.coordinates;
            fPoints.push_back(flight_.getCamera().denormalizeImageCoordinates(fPoint));
            auto position = rec.getCloudPoints().at(stoi(trackName)).getPosition();
            realWorldPoints.push_back(position);
            Xs.push_back({ position.x, position.y, position.z });
            auto fBearing = flight_.getCamera().normalizedPointToBearingVec(fPoint);
            Bs.push_back(fBearing);
        }
    }

    if (realWorldPoints.size() < 5) {
        report.numCommonPoints = realWorldPoints.size();
        return make_tuple(false, report);
    }

#if 1
    return twoViewResect(Bs,
        Xs,
        tg_,
        imageVertex,
        flight_,
        rec,
        threshold,
        iterations,
        probability,
        resectionInliers
    );
#endif

#if 0
    Mat pnpRot, pnpTrans, inliers;
    if (cv::solvePnPRansac(realWorldPoints, fPoints, flight_.getCamera().getKMatrix(),
        flight_.getCamera().getDistortionMatrix(), pnpRot, pnpTrans, false, iterations, 8.0, probability, inliers)) {
        std::cout << "Rotation was " << pnpRot << "\n";
        std::cout << "Translation was " << pnpTrans << "\n";
        const auto shotName = tg_.getTrackGraph()[imageVertex].name;
        const auto shot = flight_.getImageSet()[flight_.getImageIndex(shotName)];
        ShotMetadata shotMetadata(shot.getMetadata(), flight_);
        report.numCommonPoints = realWorldPoints.size();
        report.numInliers = cv::countNonZero(inliers);
        cout << "Rotation is " << pnpRot << "\n";
        cout << "Translation is " << pnpTrans << "\n";
        Shot recShot(shotName, flight_.getCamera(), Pose(pnpRot, pnpTrans), shotMetadata);
        rec.addShot(recShot.getId(), recShot);
        return { true, report };
    }
    return make_tuple(false, report);
#endif
}

template <class R>
set<string> Reconstructor<R>::directShotNeighbors(
    std::set<std::string> shotIds,
    const Reconstruction & rec,
    int maxNeighbors,
    int minCommonPoints)
{
    set<string> neighbors;
    set<string> reconstructionShots;
    std::transform(
        rec.getReconstructionShots().begin(),
        rec.getReconstructionShots().end(),
        std::inserter(reconstructionShots, reconstructionShots.end()),
        [](pair<string, Shot> aShot) -> string { return aShot.first; }
    );
    set<string> points;
    for (const auto shot : shotIds) {
        auto shotVertex = tg_.getImageNodes().at(shot);
        const auto[edgesBegin, edgesEnd] = boost::out_edges(shotVertex, tg_.getTrackGraph());
        for (auto tracksIter = edgesBegin; tracksIter != edgesEnd; ++tracksIter) {
            const auto trackName = tg_.getTrackGraph()[*tracksIter].trackName;
            if (rec.hasTrack(trackName)) {
                points.insert(trackName);
            }
        }
    }

    set<string> candidateShots;
    std::set_difference(
        reconstructionShots.begin(),
        reconstructionShots.end(),
        shotIds.begin(),
        shotIds.end(),
        std::inserter(candidateShots, candidateShots.end())
    );
    map<string, int> commonPoints;

    for (const auto trackId : points) {
        const auto trackVertex = tg_.getTrackNodes().at(trackId);
        const auto[edgesBegin, edgesEnd] =
            boost::out_edges(trackVertex, tg_.getTrackGraph());

        for (auto edgesIter = edgesBegin; edgesIter != edgesEnd; ++edgesIter) {
            const auto neighbor = tg_.getTrackGraph()[*edgesIter].imageName;
            if (candidateShots.find(neighbor) != candidateShots.end()) {
                commonPoints[neighbor]++;
            }
        }
    }
    auto cmp = [](pair<string, int> a, pair<string, int> b) { return  a.second > b.second; };
    std::set<pair<string, int>, decltype(cmp)> neighborPairs(commonPoints.begin(), commonPoints.end(), cmp);

    for (const auto neighbor : neighborPairs) {
        if (neighbor.second >= minCommonPoints) {
            neighbors.insert(neighbor.first);
            if (neighbors.size() == maxNeighbors) {
                break;
            }
        }
        else {
            break;
        }
    }
    return neighbors;
}

#include "reconstructabilityscores.h"

template class Reconstructor<SnavelyReconstructionabilityScore>;
template class Reconstructor<RotationOnlyReconstructabilityScore>;
template class Reconstructor<MatchesCountReconstructabilityScore>;