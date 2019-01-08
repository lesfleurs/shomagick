#include "camera.h"
#include <ostream>
#include <algorithm>
#include <string>
#include <boost/filesystem.hpp>

using std::ostream;
using std::max;
using std::string;

cv::Mat Pose::getRotationMatrix() const
{
    cv::Mat rods;
    cv::Rodrigues(this->rotation, rods);
    return rods;
}

cv::Mat Pose::getOrigin() const
{
    cv::Mat tRot;
    auto origin = this->getRotationMatrix();
    std::cout << "Rotation matrix is " << origin << std::endl;
    cv::transpose(-origin, tRot);
    std::cout << "Rotation matrix transpose is " << tRot << std::endl;
    origin = tRot * this->translation;
    return origin;
}

ostream & operator << (ostream &out, const Pose &p)
{
    out << "Rotation vector: " << p.rotation << std::endl;
    out << "Translation vector: " << p.translation << std::endl;
    return out;
}

void Camera::_cvPointsToBearingVec(cv::Mat pRect, opengv::bearingVectors_t &bearings)
{
    double l;
    cv::Vec3f p;
    opengv::bearingVector_t bearing;
    for (auto i = 0; i < pRect.rows; ++i)
    {
        p = cv::Vec3f(pRect.row(i));
        l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        for (int j = 0; j < 3; ++j)
            bearing[j] = p[j] / l;
        bearings.push_back(bearing);
    }
}
Camera::Camera() : cameraMatrix(), distortionCoefficients(), height(), width()
{
    int dimension = 3;
    this->cameraMatrix = cv::Mat::eye(dimension, dimension, CV_32F);
}

Camera::Camera(cv::Mat cameraMatrix, cv::Mat distortion, int height, int width) : cameraMatrix(cameraMatrix), distortionCoefficients(distortion), height(height),
width(width) {}

cv::Mat Camera::getKMatrix() { return this->cameraMatrix; }

cv::Mat Camera::getNormalizedKMatrix() const {
    auto lensSize = this->getPhysicalFocalLength();
    cv::Mat normK = (cv::Mat_<double>(3, 3) <<
        lensSize,   0.,          0.,
        0.,         lensSize,    0,
        0.,          0.,         1);
    return normK;
}

cv::Mat Camera::getDistortionMatrix() const
{
    return this->distortionCoefficients;
}

void Camera::cvPointsToBearingVec(
    const std::vector<cv::Point2f> &points, opengv::bearingVectors_t &bearings)
{
    const int N1 = static_cast<int>(points.size());
    cv::Mat points1_mat = cv::Mat(points).reshape(1);

    // first rectify points and construct homogeneous points
    // construct homogeneous points
    cv::Mat ones_col1 = cv::Mat::ones(N1, 1, CV_32F);
    cv::hconcat(points1_mat, ones_col1, points1_mat);

    // undistort points
    cv::Mat points1_rect = points1_mat * this->cameraMatrix.inv();

    // compute bearings
    this->_cvPointsToBearingVec(points1_rect, bearings);
}

opengv::bearingVector_t Camera::normalizedPointToBearingVec(const cv::Point2f &point) const
{
    std::cout << "Converting point " << point << std::endl;
    std::vector<cv::Point2f> points{ point };
    std::vector<cv::Point3f> hPoints;
    //std::vector<cv::Point2f> uPoints;
    cv::undistortPoints(points, points, this->getNormalizedKMatrix(), this->getDistortionMatrix());
    std::cout << "Undistorted points were " << points[0] << std::endl;
    cv::convertPointsHomogeneous(points, hPoints);
    opengv::bearingVector_t bearing;
    auto convPoint = hPoints[0];
    auto hPoint = cv::Vec3f(convPoint);
    const double l = std::sqrt(hPoint[0] * hPoint[0] + hPoint[1] * hPoint[1] + hPoint[2] * hPoint[2]);
    for (int j = 0; j < 3; ++j)
        bearing[j] = hPoint[j] / l;

    return bearing;
}

double Camera::getFocal() const{
    return this->cameraMatrix.at<double>(0, 0);
}

double Camera::getPhysicalFocalLength() const {
    return (double)this->getFocal() / (double)max(this->height, this->width);
}

double Camera::getK1() const {
    return this->getDistortionMatrix().at<double>(0,0);
}

double Camera::getk2() const{
    return this->getDistortionMatrix().at<double>(1, 0);
}

cv::Point2f Camera::normalizeImageCoordinates(const cv::Point2f pixelCoords) const
{
    const auto size = max(width, height);

    const auto pixelX = pixelCoords.x;
    const auto pixelY = pixelCoords.y;
    const auto normX = ((1.0f * width) / size) * (pixelX - width / 2.0f) / width;
    const auto normY = ((1.0f * height) / size) * (pixelY - height / 2.0f) / height;
    return {
        normX,
        normY,
    };
}

cv::Point2f Camera::denormalizeImageCoordinates(const cv::Point2f normalizedCoords) const
{
    const auto size = max(width, height);
    auto normX = normalizedCoords.x;
    auto normY = normalizedCoords.y;

    const auto pixelX = (normX * width  * size / (1.0f * width)) + width / 2.0f;
    const auto pixelY = (normY * height * size / (1.0f * height)) + height / 2.0f;

    return { pixelX, pixelY };
}

cv::Point2f Camera::projectBearing(opengv::bearingVector_t b) {
    auto x = b[0] / b[2];
    auto y = b[1] / b[2];

    auto r = x * x + y * y;
    auto radialDistortion = 1.0 + r * (getK1() + getk2() * r);

    return cv::Point2f{
        static_cast<float>(getPhysicalFocalLength() * radialDistortion * x),
        static_cast<float>(getPhysicalFocalLength() * radialDistortion * y)
    };
}

Camera Camera::getCameraFromCalibrationFile(string calibrationFile) {
    int height, width;
    cv::Mat cameraMatrix, distortionParameters;
    assert(boost::filesystem::exists(calibrationFile));
    cv::FileStorage fs(calibrationFile, cv::FileStorage::READ);
    fs["image_height"] >> height;
    fs["image_width"] >> width;
    fs["camera_matrix"] >> cameraMatrix;
    fs["distortion_coefficients"] >> distortionParameters;
    return Camera{ cameraMatrix, distortionParameters, height, width };
}


